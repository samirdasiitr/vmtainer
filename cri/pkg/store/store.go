// Package store provides a JSON-file-backed metadata store for vmtainer-cri
// sandboxes and containers. Each object gets its own subdirectory under the
// base data dir so concurrent reads never block on a global lock beyond a
// simple per-record mutex.
package store

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// SandboxState represents the lifecycle state of a pod sandbox.
type SandboxState int32

const (
	SandboxStateUnknown SandboxState = iota
	SandboxStateReady
	SandboxStateNotReady
)

func (s SandboxState) String() string {
	switch s {
	case SandboxStateReady:
		return "Ready"
	case SandboxStateNotReady:
		return "NotReady"
	default:
		return "Unknown"
	}
}

// ContainerState represents the lifecycle state of a container.
type ContainerState int32

const (
	ContainerStateUnknown ContainerState = iota
	ContainerStateCreated
	ContainerStateRunning
	ContainerStateExited
)

func (s ContainerState) String() string {
	switch s {
	case ContainerStateCreated:
		return "Created"
	case ContainerStateRunning:
		return "Running"
	case ContainerStateExited:
		return "Exited"
	default:
		return "Unknown"
	}
}

// NetworkConfig holds the TAP/IP configuration assigned to a sandbox.
type NetworkConfig struct {
	TapName    string `json:"tap_name"`
	IP         string `json:"ip"`   // CIDR notation, e.g. "10.200.1.1/16"
	Gateway    string `json:"gateway"`
	MAC        string `json:"mac"`
	HostBridge string `json:"host_bridge,omitempty"`
}

// SandboxMeta is the persisted metadata for a pod sandbox (== one VM).
type SandboxMeta struct {
	ID          string            `json:"id"`
	Name        string            `json:"name"`
	Namespace   string            `json:"namespace"`
	UID         string            `json:"uid"`
	Labels      map[string]string `json:"labels,omitempty"`
	Annotations map[string]string `json:"annotations,omitempty"`
	State       SandboxState      `json:"state"`
	Network     NetworkConfig     `json:"network"`
	// PID of the vmtainer process (0 if not running).
	PID        int       `json:"pid,omitempty"`
	CreatedAt  time.Time `json:"created_at"`
	StartedAt  time.Time `json:"started_at,omitempty"`
	FinishedAt time.Time `json:"finished_at,omitempty"`
	// ConfigPath is the resolved path of the config.json passed to vmtainer.
	ConfigPath string `json:"config_path,omitempty"`
	// LogPath is where vmtainer stdout/stderr is redirected.
	LogPath string `json:"log_path,omitempty"`
}

// ContainerMeta is the persisted metadata for a container inside a sandbox.
// Because each sandbox IS a VM, there is exactly one container per sandbox.
type ContainerMeta struct {
	ID          string            `json:"id"`
	SandboxID   string            `json:"sandbox_id"`
	Name        string            `json:"name"`
	Image       string            `json:"image"`        // image reference
	ImageRef    string            `json:"image_ref"`    // resolved digest / ID
	RootFS      string            `json:"rootfs"`       // path to extracted rootfs
	Labels      map[string]string `json:"labels,omitempty"`
	Annotations map[string]string `json:"annotations,omitempty"`
	Env         map[string]string `json:"env,omitempty"`
	Command     []string          `json:"command,omitempty"`
	Args        []string          `json:"args,omitempty"`
	WorkingDir  string            `json:"working_dir,omitempty"`
	State       ContainerState    `json:"state"`
	ExitCode    int32             `json:"exit_code,omitempty"`
	CreatedAt   time.Time         `json:"created_at"`
	StartedAt   time.Time         `json:"started_at,omitempty"`
	FinishedAt  time.Time         `json:"finished_at,omitempty"`
	LogPath     string            `json:"log_path,omitempty"`
}

// ImageMeta stores metadata about a pulled image.
type ImageMeta struct {
	ID       string   `json:"id"`        // content-addressable ID (sha256:…)
	RepoTags []string `json:"repo_tags"` // e.g. ["nginx:latest"]
	RootFS   string   `json:"rootfs"`    // path to extracted rootfs directory
	Size     int64    `json:"size"`      // approximate size in bytes
	PulledAt time.Time `json:"pulled_at"`
}

// ----------------------------------------------------------------------------
// Store
// ----------------------------------------------------------------------------

// Store is a thread-safe, JSON-file-backed metadata store.
type Store struct {
	baseDir string
	mu      sync.RWMutex
}

// New creates (or opens) a Store rooted at baseDir.
// The required subdirectories are created on first use.
func New(baseDir string) (*Store, error) {
	for _, sub := range []string{"sandboxes", "containers", "images"} {
		if err := os.MkdirAll(filepath.Join(baseDir, sub), 0o755); err != nil {
			return nil, fmt.Errorf("store: mkdir %s: %w", sub, err)
		}
	}
	return &Store{baseDir: baseDir}, nil
}

// ---------- helpers ----------------------------------------------------------

func (s *Store) sandboxDir(id string) string {
	return filepath.Join(s.baseDir, "sandboxes", id)
}
func (s *Store) sandboxMetaPath(id string) string {
	return filepath.Join(s.sandboxDir(id), "meta.json")
}
func (s *Store) containerDir(id string) string {
	return filepath.Join(s.baseDir, "containers", id)
}
func (s *Store) containerMetaPath(id string) string {
	return filepath.Join(s.containerDir(id), "meta.json")
}
func (s *Store) imageDir(id string) string {
	return filepath.Join(s.baseDir, "images", id)
}
func (s *Store) imageMetaPath(id string) string {
	return filepath.Join(s.imageDir(id), "meta.json")
}

func writeJSON(path string, v interface{}) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	// Write atomically via a temp file.
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func readJSON(path string, v interface{}) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	return json.Unmarshal(data, v)
}

// ---------- Sandbox ----------------------------------------------------------

// CreateSandbox persists a new SandboxMeta record.
func (s *Store) CreateSandbox(meta *SandboxMeta) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	dir := s.sandboxDir(meta.ID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("store: create sandbox dir: %w", err)
	}
	return writeJSON(s.sandboxMetaPath(meta.ID), meta)
}

// GetSandbox retrieves a sandbox by ID.
func (s *Store) GetSandbox(id string) (*SandboxMeta, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	meta := &SandboxMeta{}
	if err := readJSON(s.sandboxMetaPath(id), meta); err != nil {
		if os.IsNotExist(err) {
			return nil, fmt.Errorf("store: sandbox %q not found", id)
		}
		return nil, fmt.Errorf("store: read sandbox %q: %w", id, err)
	}
	return meta, nil
}

// UpdateSandbox overwrites an existing SandboxMeta record.
func (s *Store) UpdateSandbox(meta *SandboxMeta) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return writeJSON(s.sandboxMetaPath(meta.ID), meta)
}

// DeleteSandbox removes the sandbox record and its directory.
func (s *Store) DeleteSandbox(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return os.RemoveAll(s.sandboxDir(id))
}

// ListSandboxes returns all stored sandbox records.
func (s *Store) ListSandboxes() ([]*SandboxMeta, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	entries, err := os.ReadDir(filepath.Join(s.baseDir, "sandboxes"))
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []*SandboxMeta
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		meta := &SandboxMeta{}
		path := filepath.Join(s.baseDir, "sandboxes", e.Name(), "meta.json")
		if err := readJSON(path, meta); err != nil {
			continue // skip corrupt entries
		}
		out = append(out, meta)
	}
	return out, nil
}

// ---------- Container --------------------------------------------------------

// CreateContainer persists a new ContainerMeta record.
func (s *Store) CreateContainer(meta *ContainerMeta) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	dir := s.containerDir(meta.ID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("store: create container dir: %w", err)
	}
	return writeJSON(s.containerMetaPath(meta.ID), meta)
}

// GetContainer retrieves a container by ID.
func (s *Store) GetContainer(id string) (*ContainerMeta, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	meta := &ContainerMeta{}
	if err := readJSON(s.containerMetaPath(id), meta); err != nil {
		if os.IsNotExist(err) {
			return nil, fmt.Errorf("store: container %q not found", id)
		}
		return nil, fmt.Errorf("store: read container %q: %w", id, err)
	}
	return meta, nil
}

// UpdateContainer overwrites an existing ContainerMeta record.
func (s *Store) UpdateContainer(meta *ContainerMeta) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return writeJSON(s.containerMetaPath(meta.ID), meta)
}

// DeleteContainer removes the container record directory.
func (s *Store) DeleteContainer(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return os.RemoveAll(s.containerDir(id))
}

// ListContainers returns all stored container records, optionally filtered by
// sandboxID (pass "" to list all).
func (s *Store) ListContainers(sandboxID string) ([]*ContainerMeta, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	entries, err := os.ReadDir(filepath.Join(s.baseDir, "containers"))
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []*ContainerMeta
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		meta := &ContainerMeta{}
		path := filepath.Join(s.baseDir, "containers", e.Name(), "meta.json")
		if err := readJSON(path, meta); err != nil {
			continue
		}
		if sandboxID != "" && meta.SandboxID != sandboxID {
			continue
		}
		out = append(out, meta)
	}
	return out, nil
}

// ---------- Image ------------------------------------------------------------

// CreateImage persists a new ImageMeta record.
func (s *Store) CreateImage(meta *ImageMeta) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	dir := s.imageDir(meta.ID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("store: create image dir: %w", err)
	}
	return writeJSON(s.imageMetaPath(meta.ID), meta)
}

// GetImage retrieves an image by its content-addressable ID.
func (s *Store) GetImage(id string) (*ImageMeta, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	meta := &ImageMeta{}
	if err := readJSON(s.imageMetaPath(id), meta); err != nil {
		if os.IsNotExist(err) {
			return nil, fmt.Errorf("store: image %q not found", id)
		}
		return nil, fmt.Errorf("store: read image %q: %w", id, err)
	}
	return meta, nil
}

// GetImageByRef looks up an image by any of its repo tags (e.g. "nginx:latest").
func (s *Store) GetImageByRef(ref string) (*ImageMeta, error) {
	images, err := s.ListImages()
	if err != nil {
		return nil, err
	}
	for _, img := range images {
		for _, tag := range img.RepoTags {
			if tag == ref {
				return img, nil
			}
		}
		// Also match by ID prefix.
		if len(img.ID) >= len(ref) && img.ID[:len(ref)] == ref {
			return img, nil
		}
	}
	return nil, fmt.Errorf("store: image %q not found", ref)
}

// DeleteImage removes an image record and its cached rootfs.
func (s *Store) DeleteImage(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return os.RemoveAll(s.imageDir(id))
}

// ListImages returns all stored image records.
func (s *Store) ListImages() ([]*ImageMeta, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	entries, err := os.ReadDir(filepath.Join(s.baseDir, "images"))
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []*ImageMeta
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		meta := &ImageMeta{}
		path := filepath.Join(s.baseDir, "images", e.Name(), "meta.json")
		if err := readJSON(path, meta); err != nil {
			continue
		}
		out = append(out, meta)
	}
	return out, nil
}

// ImageRootFSDir returns the path where an image's rootfs should be stored.
func (s *Store) ImageRootFSDir(id string) string {
	return filepath.Join(s.imageDir(id), "rootfs")
}

// ContainerRootFSDir returns the per-container rootfs path (bind-mount overlay
// or simple directory copy, managed by the caller).
func (s *Store) ContainerRootFSDir(id string) string {
	return filepath.Join(s.containerDir(id), "rootfs")
}

// SandboxDir exposes the sandbox directory so callers can store additional
// files (config.json, logs, etc.) alongside meta.json.
func (s *Store) SandboxDir(id string) string {
	return s.sandboxDir(id)
}
