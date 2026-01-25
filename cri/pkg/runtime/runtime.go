// This file contains the RuntimeService implementation for vmtainer-cri.
// Each pod sandbox maps to exactly one vmtainer VM process.  Containers inside
// a sandbox are therefore 1:1 with the sandbox; CreateContainer records the
// container metadata and prepares the rootfs, while StartContainer is a no-op
// because the VM was already started in RunPodSandbox.
package runtime

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/sirupsen/logrus"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	runtimev1 "k8s.io/cri-api/pkg/apis/runtime/v1"

	"github.com/vmtainer/vmtainer-cri/pkg/network"
	"github.com/vmtainer/vmtainer-cri/pkg/store"
	"github.com/vmtainer/vmtainer-cri/pkg/vmm"
)

const (
	// runtimeName is reported in Version responses.
	runtimeName    = "vmtainer"
	runtimeVersion = "0.1.0"
	criVersion     = "0.1.0"

	// defaultStopGrace is the timeout before SIGKILL when stopping a VM.
	defaultStopGrace = 10 * time.Second

	// execDefaultTimeout is used when the caller does not supply a timeout.
	execDefaultTimeout = 30 * time.Second

	// streamingServerPort is the port on which the streaming server listens.
	streamingServerPort = "10250"
)

// Config carries construction parameters for RuntimeService.
type Config struct {
	// DataDir is the root data directory, e.g. /var/lib/vmtainer.
	DataDir string
	// StreamingAddress is host:port for the streaming (exec/attach) server.
	// Defaults to 0.0.0.0:10250.
	StreamingAddress string
}

// RuntimeService implements runtimev1.RuntimeServiceServer.
type RuntimeService struct {
	runtimev1.UnimplementedRuntimeServiceServer
	cfg      Config
	store    *store.Store
	netMgr   *network.Manager
	vmmMgr   *vmm.Manager
	streaming *streamingServer
}

// NewRuntimeService constructs a RuntimeService.
func NewRuntimeService(cfg Config, st *store.Store, netMgr *network.Manager, vmmMgr *vmm.Manager) (*RuntimeService, error) {
	if cfg.StreamingAddress == "" {
		cfg.StreamingAddress = "0.0.0.0:" + streamingServerPort
	}
	rs := &RuntimeService{
		cfg:    cfg,
		store:  st,
		netMgr: netMgr,
		vmmMgr: vmmMgr,
	}
	rs.streaming = newStreamingServer(cfg.StreamingAddress, rs)
	return rs, nil
}

// StartStreamingServer starts the HTTP streaming server (for Exec/Attach).
func (rs *RuntimeService) StartStreamingServer() error {
	return rs.streaming.start()
}

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

func (rs *RuntimeService) Version(_ context.Context, req *runtimev1.VersionRequest) (*runtimev1.VersionResponse, error) {
	return &runtimev1.VersionResponse{
		Version:           criVersion,
		RuntimeName:       runtimeName,
		RuntimeVersion:    runtimeVersion,
		RuntimeApiVersion: "v1",
	}, nil
}

// ---------------------------------------------------------------------------
// RunPodSandbox
// ---------------------------------------------------------------------------

// RunPodSandbox creates a TAP interface, writes config.json, and launches a
// vmtainer VM process.
func (rs *RuntimeService) RunPodSandbox(ctx context.Context, req *runtimev1.RunPodSandboxRequest) (*runtimev1.RunPodSandboxResponse, error) {
	cfg := req.GetConfig()
	if cfg == nil {
		return nil, status.Error(codes.InvalidArgument, "RunPodSandbox: nil config")
	}
	meta := cfg.GetMetadata()
	if meta == nil {
		return nil, status.Error(codes.InvalidArgument, "RunPodSandbox: nil metadata")
	}

	sandboxID := generateID()
	log := logrus.WithFields(logrus.Fields{
		"sandbox": sandboxID,
		"name":    meta.Name,
		"ns":      meta.Namespace,
	})
	log.Info("runtime: RunPodSandbox")

	// Allocate TAP + IP.
	net, err := rs.netMgr.SetupSandbox(sandboxID)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "RunPodSandbox: network setup: %v", err)
	}

	sandboxDir := rs.store.SandboxDir(sandboxID)
	if err := os.MkdirAll(sandboxDir, 0o755); err != nil {
		_ = rs.netMgr.TeardownSandbox(sandboxID, net.TapName, net.IP)
		return nil, status.Errorf(codes.Internal, "RunPodSandbox: mkdir: %v", err)
	}

	now := time.Now()
	sbMeta := &store.SandboxMeta{
		ID:          sandboxID,
		Name:        meta.Name,
		Namespace:   meta.Namespace,
		UID:         meta.Uid,
		Labels:      cfg.Labels,
		Annotations: cfg.Annotations,
		State:       store.SandboxStateNotReady,
		Network: store.NetworkConfig{
			TapName: net.TapName,
			IP:      net.IP,
			Gateway: net.Gateway,
			MAC:     net.MAC,
		},
		CreatedAt:  now,
		ConfigPath: filepath.Join(sandboxDir, "config.json"),
		LogPath:    filepath.Join(sandboxDir, "vm.log"),
	}
	if err := rs.store.CreateSandbox(sbMeta); err != nil {
		_ = rs.netMgr.TeardownSandbox(sandboxID, net.TapName, net.IP)
		return nil, status.Errorf(codes.Internal, "RunPodSandbox: persist meta: %v", err)
	}

	// Apply port-forward rules requested in the sandbox config.
	for _, pm := range cfg.PortMappings {
		if pm.HostPort == 0 || pm.ContainerPort == 0 {
			continue
		}
		proto := portProto(pm.Protocol)
		guestIP := strings.Split(net.IP, "/")[0]
		if err := network.PortForward(guestIP, pm.HostPort, pm.ContainerPort, proto); err != nil {
			log.WithError(err).Warnf("runtime: port-forward %d→%d failed", pm.HostPort, pm.ContainerPort)
		}
	}

	// Write a placeholder vmtainer config (container rootfs not known yet;
	// it will be overwritten when StartContainer is called).  This allows the
	// sandbox to reach Ready state independently, which is how kubelet expects
	// things to work.
	placeholderCfg := &vmm.VMConfig{
		Hostname: cfg.Hostname,
		RootFS:   filepath.Join(rs.cfg.DataDir, "containers", sandboxID, "rootfs"),
		Net: vmm.NetConfig{
			TAP:     net.TapName,
			IP:      net.IP,
			Gateway: net.Gateway,
			MAC:     net.MAC,
		},
		Entrypoint: "/bin/sh",
	}
	if err := vmm.WriteConfig(sbMeta.ConfigPath, placeholderCfg); err != nil {
		log.WithError(err).Warn("runtime: write placeholder config failed (non-fatal)")
	}

	// Mark sandbox as Ready immediately — the VM will be started on
	// StartContainer once the container's rootfs is known.
	sbMeta.State = store.SandboxStateReady
	sbMeta.StartedAt = now
	if err := rs.store.UpdateSandbox(sbMeta); err != nil {
		log.WithError(err).Warn("runtime: failed to persist sandbox Ready state")
	}

	return &runtimev1.RunPodSandboxResponse{PodSandboxId: sandboxID}, nil
}

// ---------------------------------------------------------------------------
// StopPodSandbox
// ---------------------------------------------------------------------------

func (rs *RuntimeService) StopPodSandbox(ctx context.Context, req *runtimev1.StopPodSandboxRequest) (*runtimev1.StopPodSandboxResponse, error) {
	id := req.GetPodSandboxId()
	log := logrus.WithField("sandbox", id)
	log.Info("runtime: StopPodSandbox")

	sb, err := rs.store.GetSandbox(id)
	if err != nil {
		// Idempotent per spec.
		return &runtimev1.StopPodSandboxResponse{}, nil
	}

	// Stop VM process.
	if err := rs.vmmMgr.StopInstance(id, defaultStopGrace); err != nil {
		log.WithError(err).Warn("runtime: stop VM failed (continuing cleanup)")
	}

	// Tear down TAP.
	if err := rs.netMgr.TeardownSandbox(id, sb.Network.TapName, sb.Network.IP); err != nil {
		log.WithError(err).Warn("runtime: teardown network failed")
	}

	// Update state.
	sb.State = store.SandboxStateNotReady
	sb.FinishedAt = time.Now()
	_ = rs.store.UpdateSandbox(sb)

	// Stop all containers in this sandbox.
	containers, _ := rs.store.ListContainers(id)
	for _, c := range containers {
		if c.State == store.ContainerStateRunning {
			c.State = store.ContainerStateExited
			c.FinishedAt = time.Now()
			_ = rs.store.UpdateContainer(c)
		}
	}

	return &runtimev1.StopPodSandboxResponse{}, nil
}

// ---------------------------------------------------------------------------
// RemovePodSandbox
// ---------------------------------------------------------------------------

func (rs *RuntimeService) RemovePodSandbox(ctx context.Context, req *runtimev1.RemovePodSandboxRequest) (*runtimev1.RemovePodSandboxResponse, error) {
	id := req.GetPodSandboxId()
	log := logrus.WithField("sandbox", id)
	log.Info("runtime: RemovePodSandbox")

	sb, err := rs.store.GetSandbox(id)
	if err != nil {
		return &runtimev1.RemovePodSandboxResponse{}, nil
	}

	// Force-stop if still running.
	if sb.State == store.SandboxStateReady {
		_, _ = rs.StopPodSandbox(ctx, &runtimev1.StopPodSandboxRequest{PodSandboxId: id})
	}

	// Remove all containers.
	containers, _ := rs.store.ListContainers(id)
	for _, c := range containers {
		_ = rs.store.DeleteContainer(c.ID)
	}

	// Remove sandbox metadata (includes its directory).
	_ = rs.store.DeleteSandbox(id)
	return &runtimev1.RemovePodSandboxResponse{}, nil
}

// ---------------------------------------------------------------------------
// PodSandboxStatus
// ---------------------------------------------------------------------------

func (rs *RuntimeService) PodSandboxStatus(ctx context.Context, req *runtimev1.PodSandboxStatusRequest) (*runtimev1.PodSandboxStatusResponse, error) {
	id := req.GetPodSandboxId()
	sb, err := rs.store.GetSandbox(id)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "PodSandboxStatus: %v", err)
	}

	// Reconcile: if the VM process has exited, mark NotReady.
	if sb.State == store.SandboxStateReady {
		inst := rs.vmmMgr.GetInstance(id)
		if inst != nil && !inst.IsRunning() {
			sb.State = store.SandboxStateNotReady
			sb.FinishedAt = time.Now()
			_ = rs.store.UpdateSandbox(sb)
		}
	}

	criState := runtimev1.PodSandboxState_SANDBOX_NOTREADY
	if sb.State == store.SandboxStateReady {
		criState = runtimev1.PodSandboxState_SANDBOX_READY
	}

	guestIP := strings.Split(sb.Network.IP, "/")[0]
	sbStatus := &runtimev1.PodSandboxStatus{
		Id: id,
		Metadata: &runtimev1.PodSandboxMetadata{
			Name:      sb.Name,
			Uid:       sb.UID,
			Namespace: sb.Namespace,
		},
		State:       criState,
		CreatedAt:   sb.CreatedAt.UnixNano(),
		Network:     &runtimev1.PodSandboxNetworkStatus{Ip: guestIP},
		Labels:      sb.Labels,
		Annotations: sb.Annotations,
	}

	resp := &runtimev1.PodSandboxStatusResponse{Status: sbStatus}
	if req.GetVerbose() {
		resp.Info = map[string]string{
			"tap":     sb.Network.TapName,
			"ip":      sb.Network.IP,
			"gateway": sb.Network.Gateway,
			"mac":     sb.Network.MAC,
			"pid":     fmt.Sprintf("%d", sb.PID),
			"log":     sb.LogPath,
		}
	}
	return resp, nil
}

// ---------------------------------------------------------------------------
// ListPodSandbox
// ---------------------------------------------------------------------------

func (rs *RuntimeService) ListPodSandbox(ctx context.Context, req *runtimev1.ListPodSandboxRequest) (*runtimev1.ListPodSandboxResponse, error) {
	sandboxes, err := rs.store.ListSandboxes()
	if err != nil {
		return nil, status.Errorf(codes.Internal, "ListPodSandbox: %v", err)
	}

	filter := req.GetFilter()
	var out []*runtimev1.PodSandbox
	for _, sb := range sandboxes {
		if filter != nil {
			if filter.Id != "" && filter.Id != sb.ID {
				continue
			}
			if filter.State != nil {
				wantReady := filter.State.State == runtimev1.PodSandboxState_SANDBOX_READY
				isReady := sb.State == store.SandboxStateReady
				if wantReady != isReady {
					continue
				}
			}
			if !labelsMatch(sb.Labels, filter.LabelSelector) {
				continue
			}
		}

		criState := runtimev1.PodSandboxState_SANDBOX_NOTREADY
		if sb.State == store.SandboxStateReady {
			criState = runtimev1.PodSandboxState_SANDBOX_READY
		}
		out = append(out, &runtimev1.PodSandbox{
			Id: sb.ID,
			Metadata: &runtimev1.PodSandboxMetadata{
				Name:      sb.Name,
				Uid:       sb.UID,
				Namespace: sb.Namespace,
			},
			State:       criState,
			CreatedAt:   sb.CreatedAt.UnixNano(),
			Labels:      sb.Labels,
			Annotations: sb.Annotations,
		})
	}
	return &runtimev1.ListPodSandboxResponse{Items: out}, nil
}

// ---------------------------------------------------------------------------
// CreateContainer
// ---------------------------------------------------------------------------

// CreateContainer prepares the container rootfs (bind-mount or copy from the
// image cache) and records metadata.  The VM is NOT started here; that happens
// in StartContainer.
func (rs *RuntimeService) CreateContainer(ctx context.Context, req *runtimev1.CreateContainerRequest) (*runtimev1.CreateContainerResponse, error) {
	sandboxID := req.GetPodSandboxId()
	cfg := req.GetConfig()
	if cfg == nil {
		return nil, status.Error(codes.InvalidArgument, "CreateContainer: nil config")
	}
	imgSpec := cfg.GetImage()
	if imgSpec == nil {
		return nil, status.Error(codes.InvalidArgument, "CreateContainer: nil image spec")
	}

	sb, err := rs.store.GetSandbox(sandboxID)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "CreateContainer: sandbox not found: %v", err)
	}

	imgRef := imgSpec.Image
	imgMeta, err := rs.store.GetImageByRef(imgRef)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "CreateContainer: image %q not found (pull first): %v", imgRef, err)
	}

	containerID := generateID()
	log := logrus.WithFields(logrus.Fields{
		"container": containerID,
		"sandbox":   sandboxID,
		"image":     imgRef,
	})
	log.Info("runtime: CreateContainer")

	// Create container rootfs directory (a copy of the image rootfs so each
	// container has its own writable tree).
	containerRootFS := rs.store.ContainerRootFSDir(containerID)
	if err := os.MkdirAll(containerRootFS, 0o755); err != nil {
		return nil, status.Errorf(codes.Internal, "CreateContainer: mkdir rootfs: %v", err)
	}
	log.Infof("runtime: copying image rootfs %s → %s", imgMeta.RootFS, containerRootFS)
	if err := copyDir(imgMeta.RootFS, containerRootFS); err != nil {
		_ = os.RemoveAll(containerRootFS)
		return nil, status.Errorf(codes.Internal, "CreateContainer: copy rootfs: %v", err)
	}

	// Build env map.
	env := make(map[string]string)
	for _, kv := range cfg.Envs {
		if kv != nil {
			env[kv.Key] = kv.Value
		}
	}

	containerMeta := &store.ContainerMeta{
		ID:          containerID,
		SandboxID:   sandboxID,
		Name:        cfg.GetMetadata().GetName(),
		Image:       imgRef,
		ImageRef:    imgMeta.ID,
		RootFS:      containerRootFS,
		Labels:      cfg.Labels,
		Annotations: cfg.Annotations,
		Env:         env,
		Command:     cfg.Command,
		Args:        cfg.Args,
		WorkingDir:  cfg.WorkingDir,
		State:       store.ContainerStateCreated,
		CreatedAt:   time.Now(),
		LogPath:     filepath.Join(rs.store.SandboxDir(sandboxID), containerID+".log"),
	}
	if err := rs.store.CreateContainer(containerMeta); err != nil {
		return nil, status.Errorf(codes.Internal, "CreateContainer: persist meta: %v", err)
	}

	// Pre-write the vmtainer config.json for this container into the sandbox
	// directory.  StartContainer will use this to (re)launch the VM.
	entrypoint := buildEntrypoint(cfg.Command, cfg.Args, cfg.WorkingDir)
	vmCfg := &vmm.VMConfig{
		Hostname: sb.Name,
		RootFS:   containerRootFS,
		Net: vmm.NetConfig{
			TAP:     sb.Network.TapName,
			IP:      sb.Network.IP,
			Gateway: sb.Network.Gateway,
			MAC:     sb.Network.MAC,
		},
		Entrypoint: entrypoint,
		Env:        env,
	}
	if err := vmm.WriteConfig(sb.ConfigPath, vmCfg); err != nil {
		log.WithError(err).Warn("runtime: write vm config failed (non-fatal)")
	}

	return &runtimev1.CreateContainerResponse{ContainerId: containerID}, nil
}

// ---------------------------------------------------------------------------
// StartContainer
// ---------------------------------------------------------------------------

// StartContainer launches the vmtainer VM process using the config.json that
// was written during CreateContainer.
func (rs *RuntimeService) StartContainer(ctx context.Context, req *runtimev1.StartContainerRequest) (*runtimev1.StartContainerResponse, error) {
	containerID := req.GetContainerId()
	log := logrus.WithField("container", containerID)
	log.Info("runtime: StartContainer")

	ctr, err := rs.store.GetContainer(containerID)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "StartContainer: %v", err)
	}
	if ctr.State == store.ContainerStateRunning {
		return &runtimev1.StartContainerResponse{}, nil // idempotent
	}

	sb, err := rs.store.GetSandbox(ctr.SandboxID)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "StartContainer: sandbox not found: %v", err)
	}

	// Start the VM.
	inst, err := rs.vmmMgr.Restore(ctx, sb.ID, sb.ConfigPath, sb.LogPath)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "StartContainer: start VM: %v", err)
	}

	// Persist PID.
	now := time.Now()
	sb.PID = inst.PID()
	sb.StartedAt = now
	_ = rs.store.UpdateSandbox(sb)

	ctr.State = store.ContainerStateRunning
	ctr.StartedAt = now
	_ = rs.store.UpdateContainer(ctr)

	// Watch for VM exit in background and update states.
	go func() {
		<-inst.WaitCh()
		exitCode := int32(inst.ExitCode())

		if s, err := rs.store.GetSandbox(sb.ID); err == nil {
			s.State = store.SandboxStateNotReady
			s.FinishedAt = time.Now()
			_ = rs.store.UpdateSandbox(s)
		}
		if c, err := rs.store.GetContainer(containerID); err == nil {
			c.State = store.ContainerStateExited
			c.ExitCode = exitCode
			c.FinishedAt = time.Now()
			_ = rs.store.UpdateContainer(c)
		}
	}()

	return &runtimev1.StartContainerResponse{}, nil
}

// ---------------------------------------------------------------------------
// StopContainer
// ---------------------------------------------------------------------------

func (rs *RuntimeService) StopContainer(ctx context.Context, req *runtimev1.StopContainerRequest) (*runtimev1.StopContainerResponse, error) {
	containerID := req.GetContainerId()
	log := logrus.WithField("container", containerID)
	log.Info("runtime: StopContainer")

	ctr, err := rs.store.GetContainer(containerID)
	if err != nil {
		return &runtimev1.StopContainerResponse{}, nil // idempotent
	}
	if ctr.State != store.ContainerStateRunning {
		return &runtimev1.StopContainerResponse{}, nil
	}

	grace := time.Duration(req.GetTimeout()) * time.Second
	if grace <= 0 {
		grace = defaultStopGrace
	}
	if err := rs.vmmMgr.StopInstance(ctr.SandboxID, grace); err != nil {
		log.WithError(err).Warn("runtime: StopContainer: stop VM failed")
	}

	ctr.State = store.ContainerStateExited
	ctr.FinishedAt = time.Now()
	_ = rs.store.UpdateContainer(ctr)
	return &runtimev1.StopContainerResponse{}, nil
}

// ---------------------------------------------------------------------------
// RemoveContainer
// ---------------------------------------------------------------------------

func (rs *RuntimeService) RemoveContainer(ctx context.Context, req *runtimev1.RemoveContainerRequest) (*runtimev1.RemoveContainerResponse, error) {
	containerID := req.GetContainerId()
	log := logrus.WithField("container", containerID)
	log.Info("runtime: RemoveContainer")

	ctr, err := rs.store.GetContainer(containerID)
	if err != nil {
		return &runtimev1.RemoveContainerResponse{}, nil
	}
	if ctr.State == store.ContainerStateRunning {
		_, _ = rs.StopContainer(ctx, &runtimev1.StopContainerRequest{ContainerId: containerID})
	}

	// Remove rootfs.
	if ctr.RootFS != "" {
		if err := os.RemoveAll(ctr.RootFS); err != nil {
			log.WithError(err).Warn("runtime: RemoveContainer: remove rootfs failed")
		}
	}
	_ = rs.store.DeleteContainer(containerID)
	return &runtimev1.RemoveContainerResponse{}, nil
}

// ---------------------------------------------------------------------------
// ListContainers
// ---------------------------------------------------------------------------

func (rs *RuntimeService) ListContainers(ctx context.Context, req *runtimev1.ListContainersRequest) (*runtimev1.ListContainersResponse, error) {
	filter := req.GetFilter()
	sandboxID := ""
	if filter != nil {
		sandboxID = filter.PodSandboxId
	}

	containers, err := rs.store.ListContainers(sandboxID)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "ListContainers: %v", err)
	}

	var out []*runtimev1.Container
	for _, c := range containers {
		if filter != nil {
			if filter.Id != "" && filter.Id != c.ID {
				continue
			}
			if filter.State != nil && containerStateToCRI(c.State) != filter.State.State {
				continue
			}
			if !labelsMatch(c.Labels, filter.LabelSelector) {
				continue
			}
		}
		out = append(out, &runtimev1.Container{
			Id:           c.ID,
			PodSandboxId: c.SandboxID,
			Metadata:     &runtimev1.ContainerMetadata{Name: c.Name},
			Image:        &runtimev1.ImageSpec{Image: c.Image},
			ImageRef:     c.ImageRef,
			ImageId:      c.ImageRef,
			State:        containerStateToCRI(c.State),
			CreatedAt:    c.CreatedAt.UnixNano(),
			Labels:       c.Labels,
			Annotations:  c.Annotations,
		})
	}
	return &runtimev1.ListContainersResponse{Containers: out}, nil
}

// ---------------------------------------------------------------------------
// ContainerStatus
// ---------------------------------------------------------------------------

func (rs *RuntimeService) ContainerStatus(ctx context.Context, req *runtimev1.ContainerStatusRequest) (*runtimev1.ContainerStatusResponse, error) {
	id := req.GetContainerId()
	c, err := rs.store.GetContainer(id)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "ContainerStatus: %v", err)
	}

	var startedAt, finishedAt int64
	if !c.StartedAt.IsZero() {
		startedAt = c.StartedAt.UnixNano()
	}
	if !c.FinishedAt.IsZero() {
		finishedAt = c.FinishedAt.UnixNano()
	}

	cs := &runtimev1.ContainerStatus{
		Id:          c.ID,
		Metadata:    &runtimev1.ContainerMetadata{Name: c.Name},
		State:       containerStateToCRI(c.State),
		CreatedAt:   c.CreatedAt.UnixNano(),
		StartedAt:   startedAt,
		FinishedAt:  finishedAt,
		ExitCode:    c.ExitCode,
		Image:       &runtimev1.ImageSpec{Image: c.Image},
		ImageRef:    c.ImageRef,
		ImageId:     c.ImageRef,
		Labels:      c.Labels,
		Annotations: c.Annotations,
		LogPath:     c.LogPath,
	}
	resp := &runtimev1.ContainerStatusResponse{Status: cs}
	if req.GetVerbose() {
		resp.Info = map[string]string{
			"rootfs":  c.RootFS,
			"sandbox": c.SandboxID,
		}
	}
	return resp, nil
}

// ---------------------------------------------------------------------------
// UpdateContainerResources — not supported by vmtainer VMs
// ---------------------------------------------------------------------------

func (rs *RuntimeService) UpdateContainerResources(ctx context.Context, req *runtimev1.UpdateContainerResourcesRequest) (*runtimev1.UpdateContainerResourcesResponse, error) {
	return &runtimev1.UpdateContainerResourcesResponse{}, nil
}

// ---------------------------------------------------------------------------
// ReopenContainerLog
// ---------------------------------------------------------------------------

func (rs *RuntimeService) ReopenContainerLog(ctx context.Context, req *runtimev1.ReopenContainerLogRequest) (*runtimev1.ReopenContainerLogResponse, error) {
	// vmtainer writes directly to a file; no handle to reopen.
	return &runtimev1.ReopenContainerLogResponse{}, nil
}

// ---------------------------------------------------------------------------
// ExecSync
// ---------------------------------------------------------------------------

// ExecSync runs a command inside the VM by writing a request file to the
// shared virtiofs volume and polling for the result.
func (rs *RuntimeService) ExecSync(ctx context.Context, req *runtimev1.ExecSyncRequest) (*runtimev1.ExecSyncResponse, error) {
	id := req.GetContainerId()
	c, err := rs.store.GetContainer(id)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "ExecSync: container %q: %v", id, err)
	}
	if c.State != store.ContainerStateRunning {
		return nil, status.Errorf(codes.FailedPrecondition, "ExecSync: container not running")
	}

	timeout := execDefaultTimeout
	if t := req.GetTimeout(); t > 0 {
		timeout = time.Duration(t) * time.Second
	}

	result, err := vmm.ExecSync(ctx, c.RootFS, req.GetCmd(), timeout)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "ExecSync: %v", err)
	}
	return &runtimev1.ExecSyncResponse{
		Stdout:   result.Stdout,
		Stderr:   result.Stderr,
		ExitCode: result.ExitCode,
	}, nil
}

// ---------------------------------------------------------------------------
// Exec (streaming)
// ---------------------------------------------------------------------------

func (rs *RuntimeService) Exec(ctx context.Context, req *runtimev1.ExecRequest) (*runtimev1.ExecResponse, error) {
	if _, err := rs.store.GetContainer(req.GetContainerId()); err != nil {
		return nil, status.Errorf(codes.NotFound, "Exec: container not found")
	}
	url := rs.streaming.buildURL("exec", req.GetContainerId())
	return &runtimev1.ExecResponse{Url: url}, nil
}

// ---------------------------------------------------------------------------
// Attach (streaming)
// ---------------------------------------------------------------------------

func (rs *RuntimeService) Attach(ctx context.Context, req *runtimev1.AttachRequest) (*runtimev1.AttachResponse, error) {
	if _, err := rs.store.GetContainer(req.GetContainerId()); err != nil {
		return nil, status.Errorf(codes.NotFound, "Attach: container not found")
	}
	url := rs.streaming.buildURL("attach", req.GetContainerId())
	return &runtimev1.AttachResponse{Url: url}, nil
}

// ---------------------------------------------------------------------------
// PortForward (streaming)
// ---------------------------------------------------------------------------

func (rs *RuntimeService) PortForward(ctx context.Context, req *runtimev1.PortForwardRequest) (*runtimev1.PortForwardResponse, error) {
	if _, err := rs.store.GetSandbox(req.GetPodSandboxId()); err != nil {
		return nil, status.Errorf(codes.NotFound, "PortForward: sandbox not found")
	}
	url := rs.streaming.buildURL("portforward", req.GetPodSandboxId())
	return &runtimev1.PortForwardResponse{Url: url}, nil
}

// ---------------------------------------------------------------------------
// ContainerStats / ListContainerStats
// ---------------------------------------------------------------------------

func (rs *RuntimeService) ContainerStats(ctx context.Context, req *runtimev1.ContainerStatsRequest) (*runtimev1.ContainerStatsResponse, error) {
	id := req.GetContainerId()
	c, err := rs.store.GetContainer(id)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "ContainerStats: %v", err)
	}
	return &runtimev1.ContainerStatsResponse{Stats: containerStats(c)}, nil
}

func (rs *RuntimeService) ListContainerStats(ctx context.Context, req *runtimev1.ListContainerStatsRequest) (*runtimev1.ListContainerStatsResponse, error) {
	filter := req.GetFilter()
	sandboxID := ""
	if filter != nil {
		sandboxID = filter.PodSandboxId
	}
	containers, err := rs.store.ListContainers(sandboxID)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "ListContainerStats: %v", err)
	}
	var stats []*runtimev1.ContainerStats
	for _, c := range containers {
		if filter != nil && filter.Id != "" && filter.Id != c.ID {
			continue
		}
		stats = append(stats, containerStats(c))
	}
	return &runtimev1.ListContainerStatsResponse{Stats: stats}, nil
}

func containerStats(c *store.ContainerMeta) *runtimev1.ContainerStats {
	now := time.Now().UnixNano()
	usedBytes := uint64(dirSize(c.RootFS))
	return &runtimev1.ContainerStats{
		Attributes: &runtimev1.ContainerAttributes{
			Id:          c.ID,
			Metadata:    &runtimev1.ContainerMetadata{Name: c.Name},
			Labels:      c.Labels,
			Annotations: c.Annotations,
		},
		WritableLayer: &runtimev1.FilesystemUsage{
			Timestamp:  now,
			FsId:       &runtimev1.FilesystemIdentifier{Mountpoint: c.RootFS},
			UsedBytes:  &runtimev1.UInt64Value{Value: usedBytes},
			InodesUsed: &runtimev1.UInt64Value{Value: 0},
		},
	}
}

// ---------------------------------------------------------------------------
// PodSandboxStats / ListPodSandboxStats
// ---------------------------------------------------------------------------

func (rs *RuntimeService) PodSandboxStats(ctx context.Context, req *runtimev1.PodSandboxStatsRequest) (*runtimev1.PodSandboxStatsResponse, error) {
	return &runtimev1.PodSandboxStatsResponse{}, nil
}

func (rs *RuntimeService) ListPodSandboxStats(ctx context.Context, req *runtimev1.ListPodSandboxStatsRequest) (*runtimev1.ListPodSandboxStatsResponse, error) {
	return &runtimev1.ListPodSandboxStatsResponse{}, nil
}

// ---------------------------------------------------------------------------
// UpdateRuntimeConfig
// ---------------------------------------------------------------------------

func (rs *RuntimeService) UpdateRuntimeConfig(ctx context.Context, req *runtimev1.UpdateRuntimeConfigRequest) (*runtimev1.UpdateRuntimeConfigResponse, error) {
	return &runtimev1.UpdateRuntimeConfigResponse{}, nil
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

func (rs *RuntimeService) Status(ctx context.Context, req *runtimev1.StatusRequest) (*runtimev1.StatusResponse, error) {
	ready := &runtimev1.RuntimeCondition{
		Type:    "RuntimeReady",
		Status:  true,
		Reason:  "RuntimeReady",
		Message: "vmtainer CRI runtime is ready",
	}
	networkReady := &runtimev1.RuntimeCondition{
		Type:    "NetworkReady",
		Status:  true,
		Reason:  "NetworkReady",
		Message: "vmtainer network is ready",
	}
	return &runtimev1.StatusResponse{
		Status: &runtimev1.RuntimeStatus{
			Conditions: []*runtimev1.RuntimeCondition{ready, networkReady},
		},
	}, nil
}

// ---------------------------------------------------------------------------
// CheckpointContainer — unsupported
// ---------------------------------------------------------------------------

func (rs *RuntimeService) CheckpointContainer(ctx context.Context, req *runtimev1.CheckpointContainerRequest) (*runtimev1.CheckpointContainerResponse, error) {
	return nil, status.Error(codes.Unimplemented, "CheckpointContainer not supported by vmtainer")
}

// ---------------------------------------------------------------------------
// GetContainerEvents — send a single synthetic event and close
// ---------------------------------------------------------------------------

func (rs *RuntimeService) GetContainerEvents(req *runtimev1.GetEventsRequest, srv runtimev1.RuntimeService_GetContainerEventsServer) error {
	// vmtainer does not produce a stream of container events; return immediately.
	return nil
}

// ---------------------------------------------------------------------------
// ListMetricDescriptors / ListPodSandboxMetrics
// ---------------------------------------------------------------------------

func (rs *RuntimeService) ListMetricDescriptors(ctx context.Context, req *runtimev1.ListMetricDescriptorsRequest) (*runtimev1.ListMetricDescriptorsResponse, error) {
	return &runtimev1.ListMetricDescriptorsResponse{}, nil
}

func (rs *RuntimeService) ListPodSandboxMetrics(ctx context.Context, req *runtimev1.ListPodSandboxMetricsRequest) (*runtimev1.ListPodSandboxMetricsResponse, error) {
	return &runtimev1.ListPodSandboxMetricsResponse{}, nil
}

// ---------------------------------------------------------------------------
// RuntimeConfig
// ---------------------------------------------------------------------------

func (rs *RuntimeService) RuntimeConfig(ctx context.Context, req *runtimev1.RuntimeConfigRequest) (*runtimev1.RuntimeConfigResponse, error) {
	return &runtimev1.RuntimeConfigResponse{}, nil
}

// ---------------------------------------------------------------------------
// internal helpers
// ---------------------------------------------------------------------------

func generateID() string {
	b := make([]byte, 16)
	// crypto/rand is preferred but not available in all minimal environments;
	// fall back to os.ReadFile /dev/urandom.
	f, err := os.Open("/dev/urandom")
	if err == nil {
		_, _ = io.ReadFull(f, b)
		f.Close()
	} else {
		// Use time-based fallback.
		now := time.Now().UnixNano()
		for i := range b {
			b[i] = byte(now >> (uint(i) * 3))
		}
	}
	return fmt.Sprintf("%x", b)
}

func buildEntrypoint(command, args []string, workingDir string) string {
	parts := append(command, args...)
	if len(parts) == 0 {
		return "/bin/sh"
	}
	// If there is a working directory, prefix with "cd <dir> && ".
	ep := strings.Join(parts, " ")
	if workingDir != "" {
		ep = fmt.Sprintf("cd %s && %s", workingDir, ep)
	}
	return ep
}

func containerStateToCRI(s store.ContainerState) runtimev1.ContainerState {
	switch s {
	case store.ContainerStateCreated:
		return runtimev1.ContainerState_CONTAINER_CREATED
	case store.ContainerStateRunning:
		return runtimev1.ContainerState_CONTAINER_RUNNING
	case store.ContainerStateExited:
		return runtimev1.ContainerState_CONTAINER_EXITED
	default:
		return runtimev1.ContainerState_CONTAINER_UNKNOWN
	}
}

func labelsMatch(labels, selector map[string]string) bool {
	for k, v := range selector {
		if labels[k] != v {
			return false
		}
	}
	return true
}

func portProto(p runtimev1.Protocol) string {
	switch p {
	case runtimev1.Protocol_UDP:
		return "udp"
	default:
		return "tcp"
	}
}

// copyDir recursively copies src directory tree to dst.
func copyDir(src, dst string) error {
	return filepath.Walk(src, func(path string, fi os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)

		if fi.IsDir() {
			return os.MkdirAll(target, fi.Mode())
		}
		// Regular file or symlink.
		if fi.Mode()&os.ModeSymlink != 0 {
			link, err := os.Readlink(path)
			if err != nil {
				return err
			}
			_ = os.Remove(target)
			return os.Symlink(link, target)
		}
		return copyFile(path, target, fi.Mode())
	})
}

func copyFile(src, dst string, mode os.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

// ---------------------------------------------------------------------------
// Minimal streaming server
// ---------------------------------------------------------------------------
// The streaming server handles Exec/Attach/PortForward by redirecting the
// kubelet (and its proxying) to a URL served here.  The actual I/O is wired
// via a serial console virtiofs channel — the same mechanism as ExecSync but
// without a timeout.

type streamingServer struct {
	addr string
	rs   *RuntimeService
	mux  *http.ServeMux
}

func newStreamingServer(addr string, rs *RuntimeService) *streamingServer {
	ss := &streamingServer{addr: addr, rs: rs, mux: http.NewServeMux()}
	ss.mux.HandleFunc("/exec/", ss.handleExec)
	ss.mux.HandleFunc("/attach/", ss.handleAttach)
	ss.mux.HandleFunc("/portforward/", ss.handlePortForward)
	return ss
}

func (ss *streamingServer) start() error {
	srv := &http.Server{
		Addr:    ss.addr,
		Handler: ss.mux,
	}
	go func() {
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			logrus.WithError(err).Error("streaming server: ListenAndServe")
		}
	}()
	logrus.Infof("streaming server: listening on %s", ss.addr)
	return nil
}

func (ss *streamingServer) buildURL(kind, id string) string {
	host := ss.addr
	if strings.HasPrefix(host, "0.0.0.0:") {
		host = "127.0.0.1:" + strings.TrimPrefix(host, "0.0.0.0:")
	}
	return fmt.Sprintf("http://%s/%s/%s", host, kind, id)
}

func (ss *streamingServer) handleExec(w http.ResponseWriter, r *http.Request) {
	containerID := filepath.Base(r.URL.Path)
	c, err := ss.rs.store.GetContainer(containerID)
	if err != nil {
		http.Error(w, "container not found", http.StatusNotFound)
		return
	}

	cmdStr := r.URL.Query().Get("cmd")
	var cmd []string
	if cmdStr != "" {
		cmd = strings.Fields(cmdStr)
	} else {
		cmd = []string{"/bin/sh"}
	}

	result, err := vmm.ExecSync(r.Context(), c.RootFS, cmd, 30*time.Second)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	_, _ = w.Write(result.Stdout)
	if len(result.Stderr) > 0 {
		_, _ = w.Write(result.Stderr)
	}
}

func (ss *streamingServer) handleAttach(w http.ResponseWriter, r *http.Request) {
	containerID := filepath.Base(r.URL.Path)
	if _, err := ss.rs.store.GetContainer(containerID); err != nil {
		http.Error(w, "container not found", http.StatusNotFound)
		return
	}
	// Attach via the exec mechanism with an interactive shell.
	// In production this would upgrade to WebSocket / SPDY and wire a serial
	// console.  Stub responds 501 to signal the limitation clearly.
	http.Error(w, "attach: interactive sessions require virtio-vsock; not yet implemented", http.StatusNotImplemented)
}

func (ss *streamingServer) handlePortForward(w http.ResponseWriter, r *http.Request) {
	sandboxID := filepath.Base(r.URL.Path)
	sb, err := ss.rs.store.GetSandbox(sandboxID)
	if err != nil {
		http.Error(w, "sandbox not found", http.StatusNotFound)
		return
	}
	// Resolve target from query parameters.
	portStr := r.URL.Query().Get("port")
	if portStr == "" {
		http.Error(w, "missing port parameter", http.StatusBadRequest)
		return
	}
	guestIP := strings.Split(sb.Network.IP, "/")[0]
	target := fmt.Sprintf("%s:%s", guestIP, portStr)

	// Forward the TCP connection.
	conn, err := dialTCP(r.Context(), target)
	if err != nil {
		http.Error(w, fmt.Sprintf("portforward: dial %s: %v", target, err), http.StatusBadGateway)
		return
	}
	defer conn.Close()

	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "hijacking not supported", http.StatusInternalServerError)
		return
	}
	clientConn, _, err := hj.Hijack()
	if err != nil {
		return
	}
	defer clientConn.Close()

	done := make(chan struct{}, 2)
	go func() { _, _ = io.Copy(conn, clientConn); done <- struct{}{} }()
	go func() { _, _ = io.Copy(clientConn, conn); done <- struct{}{} }()
	<-done
}

func dialTCP(ctx context.Context, addr string) (io.ReadWriteCloser, error) {
	cmd := exec.CommandContext(ctx, "nc", "-q1", strings.Split(addr, ":")[0], strings.Split(addr, ":")[1])
	pr, pw, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	cmd.Stdout = pw
	cmd.Stdin = pr
	if err := cmd.Start(); err != nil {
		pr.Close()
		pw.Close()
		return nil, fmt.Errorf("portforward: nc: %w", err)
	}
	return &ncConn{cmd: cmd, r: pr, w: pw}, nil
}

type ncConn struct {
	cmd *exec.Cmd
	r   *os.File
	w   *os.File
}

func (c *ncConn) Read(b []byte) (int, error)  { return c.r.Read(b) }
func (c *ncConn) Write(b []byte) (int, error) { return c.w.Write(b) }
func (c *ncConn) Close() error {
	c.r.Close()
	c.w.Close()
	return c.cmd.Process.Kill()
}
