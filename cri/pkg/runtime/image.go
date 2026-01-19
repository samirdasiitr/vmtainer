// Package runtime implements the CRI v1 RuntimeService and ImageService for
// vmtainer.  This file contains the ImageService implementation.
package runtime

import (
	"context"
	"crypto/sha256"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/sirupsen/logrus"
	runtimev1 "k8s.io/cri-api/pkg/apis/runtime/v1"

	"github.com/vmtainer/vmtainer-cri/pkg/store"
)

// ImageService implements runtimev1.ImageServiceServer.
type ImageService struct {
	runtimev1.UnimplementedImageServiceServer
	store   *store.Store
	dataDir string // /var/lib/vmtainer
}

// NewImageService creates an ImageService backed by the given store.
// dataDir is the root data directory (e.g. /var/lib/vmtainer).
func NewImageService(st *store.Store, dataDir string) *ImageService {
	return &ImageService{store: st, dataDir: dataDir}
}

// ---------------------------------------------------------------------------
// PullImage
// ---------------------------------------------------------------------------

// PullImage pulls an OCI image using the docker CLI and extracts its rootfs.
//
// Workflow:
//  1. docker pull <image>
//  2. docker inspect --format '{{.Id}}' <image>  → get canonical digest
//  3. docker create <image>  → get container ID
//  4. docker export <containerID>  → pipe tar stream into rootfs directory
//  5. docker rm <containerID>
//  6. Persist ImageMeta
func (s *ImageService) PullImage(ctx context.Context, req *runtimev1.PullImageRequest) (*runtimev1.PullImageResponse, error) {
	ref := req.GetImage().GetImage()
	if ref == "" {
		return nil, fmt.Errorf("image: PullImage: empty image reference")
	}

	log := logrus.WithField("image", ref)
	log.Info("image: pulling")

	// Build docker pull arguments; inject credentials if provided.
	pullArgs := []string{"pull"}
	if auth := req.GetAuth(); auth != nil {
		if auth.Username != "" && auth.Password != "" {
			// Log in first (best-effort; ignore errors for public registries).
			loginCmd := exec.CommandContext(ctx, "docker", "login",
				"-u", auth.Username, "--password-stdin",
				serverFromRef(ref))
			loginCmd.Stdin = strings.NewReader(auth.Password)
			if out, err := loginCmd.CombinedOutput(); err != nil {
				log.WithError(err).Warnf("image: docker login failed: %s", string(out))
			}
		}
	}
	pullArgs = append(pullArgs, ref)
	if out, err := exec.CommandContext(ctx, "docker", pullArgs...).CombinedOutput(); err != nil {
		return nil, fmt.Errorf("image: docker pull %q: %w: %s", ref, err, string(out))
	}

	// Get canonical ID.
	idOut, err := exec.CommandContext(ctx, "docker", "inspect", "--format", "{{.Id}}", ref).Output()
	if err != nil {
		return nil, fmt.Errorf("image: docker inspect %q: %w", ref, err)
	}
	imageID := strings.TrimSpace(string(idOut))
	if imageID == "" {
		// Fallback: hash the ref.
		h := sha256.Sum256([]byte(ref))
		imageID = fmt.Sprintf("sha256:%x", h[:])
	}
	// Normalise to "sha256:…" format.
	if !strings.Contains(imageID, ":") {
		imageID = "sha256:" + imageID
	}

	// If already cached, just update the tag list.
	if existing, err := s.store.GetImage(imageID); err == nil {
		existing.RepoTags = mergeStrings(existing.RepoTags, ref)
		_ = s.store.CreateImage(existing)
		log.Info("image: already cached, refreshed tags")
		return &runtimev1.PullImageResponse{ImageRef: imageID}, nil
	}

	// Get the list of RepoTags from docker.
	tagsOut, _ := exec.CommandContext(ctx, "docker", "inspect", "--format",
		"{{range .RepoTags}}{{.}}\n{{end}}", ref).Output()
	repoTags := []string{ref}
	for _, t := range strings.Split(strings.TrimSpace(string(tagsOut)), "\n") {
		if t != "" {
			repoTags = mergeStrings(repoTags, t)
		}
	}

	// Create a throwaway container and export rootfs.
	createOut, err := exec.CommandContext(ctx, "docker", "create", ref).Output()
	if err != nil {
		return nil, fmt.Errorf("image: docker create %q: %w", ref, err)
	}
	containerID := strings.TrimSpace(string(createOut))
	// Always remove the container when done.
	defer func() {
		_ = exec.Command("docker", "rm", "-f", containerID).Run()
	}()

	// Prepare rootfs directory.
	rootfsDir := s.store.ImageRootFSDir(imageID)
	if err := os.MkdirAll(rootfsDir, 0o755); err != nil {
		return nil, fmt.Errorf("image: mkdir rootfs: %w", err)
	}

	log.Infof("image: extracting rootfs to %s", rootfsDir)
	exportCmd := exec.CommandContext(ctx, "docker", "export", containerID)
	exportCmd.Stdout = nil // will be piped

	tarCmd := exec.CommandContext(ctx, "tar", "-x", "--overwrite", "-C", rootfsDir)

	// Pipe docker export → tar
	tarCmd.Stdin, err = exportCmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("image: pipe setup: %w", err)
	}
	if err := tarCmd.Start(); err != nil {
		return nil, fmt.Errorf("image: tar start: %w", err)
	}
	if err := exportCmd.Run(); err != nil {
		_ = tarCmd.Process.Kill()
		return nil, fmt.Errorf("image: docker export: %w", err)
	}
	if err := tarCmd.Wait(); err != nil {
		return nil, fmt.Errorf("image: tar extract: %w", err)
	}

	// Measure size.
	size := dirSize(rootfsDir)

	meta := &store.ImageMeta{
		ID:       imageID,
		RepoTags: repoTags,
		RootFS:   rootfsDir,
		Size:     size,
		PulledAt: time.Now(),
	}
	if err := s.store.CreateImage(meta); err != nil {
		return nil, fmt.Errorf("image: persist meta: %w", err)
	}

	log.Infof("image: pulled successfully, id=%s", imageID)
	return &runtimev1.PullImageResponse{ImageRef: imageID}, nil
}

// ---------------------------------------------------------------------------
// ListImages
// ---------------------------------------------------------------------------

func (s *ImageService) ListImages(ctx context.Context, req *runtimev1.ListImagesRequest) (*runtimev1.ListImagesResponse, error) {
	images, err := s.store.ListImages()
	if err != nil {
		return nil, fmt.Errorf("image: ListImages: %w", err)
	}

	filter := req.GetFilter()
	var out []*runtimev1.Image
	for _, img := range images {
		if filter != nil && filter.Image != nil {
			want := filter.Image.Image
			if want != "" && !imageMatchesRef(img, want) {
				continue
			}
		}
		out = append(out, imageToCRI(img))
	}
	return &runtimev1.ListImagesResponse{Images: out}, nil
}

// ---------------------------------------------------------------------------
// ImageStatus
// ---------------------------------------------------------------------------

func (s *ImageService) ImageStatus(ctx context.Context, req *runtimev1.ImageStatusRequest) (*runtimev1.ImageStatusResponse, error) {
	ref := req.GetImage().GetImage()
	img, err := s.store.GetImageByRef(ref)
	if err != nil {
		// Not found → return empty (not an error per the CRI spec).
		return &runtimev1.ImageStatusResponse{}, nil
	}
	return &runtimev1.ImageStatusResponse{Image: imageToCRI(img)}, nil
}

// ---------------------------------------------------------------------------
// RemoveImage
// ---------------------------------------------------------------------------

func (s *ImageService) RemoveImage(ctx context.Context, req *runtimev1.RemoveImageRequest) (*runtimev1.RemoveImageResponse, error) {
	ref := req.GetImage().GetImage()
	img, err := s.store.GetImageByRef(ref)
	if err != nil {
		// Idempotent — already gone.
		return &runtimev1.RemoveImageResponse{}, nil
	}
	if err := s.store.DeleteImage(img.ID); err != nil {
		return nil, fmt.Errorf("image: RemoveImage %q: %w", ref, err)
	}
	logrus.WithField("image", ref).Info("image: removed")
	return &runtimev1.RemoveImageResponse{}, nil
}

// ---------------------------------------------------------------------------
// ImageFsInfo
// ---------------------------------------------------------------------------

func (s *ImageService) ImageFsInfo(ctx context.Context, req *runtimev1.ImageFsInfoRequest) (*runtimev1.ImageFsInfoResponse, error) {
	imagesDir := filepath.Join(s.dataDir, "images")
	var usedBytes, inodes uint64
	if st, err := os.Stat(imagesDir); err == nil && st.IsDir() {
		usedBytes = uint64(dirSize(imagesDir))
		inodes = dirInodes(imagesDir)
	}
	now := time.Now().UnixNano()
	return &runtimev1.ImageFsInfoResponse{
		ImageFilesystems: []*runtimev1.FilesystemUsage{
			{
				Timestamp: now,
				FsId:      &runtimev1.FilesystemIdentifier{Mountpoint: imagesDir},
				UsedBytes: &runtimev1.UInt64Value{Value: usedBytes},
				InodesUsed: &runtimev1.UInt64Value{Value: inodes},
			},
		},
	}, nil
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

func imageToCRI(img *store.ImageMeta) *runtimev1.Image {
	return &runtimev1.Image{
		Id:       img.ID,
		RepoTags: img.RepoTags,
		Size_:    uint64(img.Size),
		Spec:     &runtimev1.ImageSpec{Image: img.ID},
	}
}

func imageMatchesRef(img *store.ImageMeta, ref string) bool {
	if img.ID == ref || strings.HasPrefix(img.ID, ref) {
		return true
	}
	for _, t := range img.RepoTags {
		if t == ref {
			return true
		}
	}
	return false
}

func mergeStrings(slice []string, s string) []string {
	for _, v := range slice {
		if v == s {
			return slice
		}
	}
	return append(slice, s)
}

// serverFromRef extracts the registry hostname from an image reference.
func serverFromRef(ref string) string {
	parts := strings.SplitN(ref, "/", 2)
	if len(parts) == 2 && (strings.Contains(parts[0], ".") || strings.Contains(parts[0], ":")) {
		return parts[0]
	}
	return "https://index.docker.io/v1/"
}

// dirSize returns the approximate byte size of a directory tree.
func dirSize(path string) int64 {
	var total int64
	_ = filepath.Walk(path, func(_ string, fi os.FileInfo, err error) error {
		if err == nil && !fi.IsDir() {
			total += fi.Size()
		}
		return nil
	})
	return total
}

// dirInodes returns the number of inodes (files) in a directory tree.
func dirInodes(path string) uint64 {
	var count uint64
	_ = filepath.Walk(path, func(_ string, fi os.FileInfo, err error) error {
		if err != nil {
			return nil
		}
		if sys, ok := fi.Sys().(*syscall.Stat_t); ok {
			_ = sys // satisfies the import
		}
		count++
		return nil
	})
	return count
}
