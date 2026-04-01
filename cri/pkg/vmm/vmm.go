// Package vmm wraps the vmtainer binary, managing VM lifecycle, config
// serialisation, and stdout/stderr log capture.
//
// The vmtainer CLI contract expected here:
//
//	vmtainer restore <snapshot-path> --config <config.json>
//
// Config JSON format (see top-level README for canonical schema):
//
//	{
//	  "hostname": "mycontainer",
//	  "rootfs":   "/var/lib/vmtainer/containers/<id>/rootfs",
//	  "net": {
//	    "tap":     "vmtap0",
//	    "ip":      "10.200.0.2/16",
//	    "gateway": "10.200.0.1",
//	    "mac":     "52:54:00:ab:cd:ef"
//	  },
//	  "entrypoint": "/bin/sh -c 'exec /entrypoint.sh'",
//	  "env": { "PATH": "…" }
//	}
package vmm

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/sirupsen/logrus"
)

// ---------------------------------------------------------------------------
// Config JSON types
// ---------------------------------------------------------------------------

// NetConfig mirrors the "net" block in vmtainer's config.json.
type NetConfig struct {
	TAP     string `json:"tap"`
	IP      string `json:"ip"`
	Gateway string `json:"gateway"`
	MAC     string `json:"mac"`
}

// VMConfig is the full config.json payload passed to `vmtainer restore`.
type VMConfig struct {
	Hostname   string            `json:"hostname"`
	RootFS     string            `json:"rootfs"`
	Net        NetConfig         `json:"net"`
	Entrypoint string            `json:"entrypoint"`
	Env        map[string]string `json:"env,omitempty"`
}

// WriteConfig serialises cfg and writes it atomically to path.
func WriteConfig(path string, cfg *VMConfig) error {
	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return fmt.Errorf("vmm: marshal config: %w", err)
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return fmt.Errorf("vmm: write config: %w", err)
	}
	return os.Rename(tmp, path)
}

// ---------------------------------------------------------------------------
// Instance — a running vmtainer process
// ---------------------------------------------------------------------------

// Instance represents a single vmtainer VM process.
type Instance struct {
	mu         sync.Mutex
	sandboxID  string
	cmd        *exec.Cmd
	logFile    *os.File
	pid        int
	exitCode   int
	exited     bool
	exitCh     chan struct{}
	cancelFunc context.CancelFunc
}

// PID returns the OS PID of the vmtainer process, or 0 if it has not started.
func (i *Instance) PID() int {
	i.mu.Lock()
	defer i.mu.Unlock()
	return i.pid
}

// IsRunning reports whether the vmtainer process is still alive.
func (i *Instance) IsRunning() bool {
	i.mu.Lock()
	defer i.mu.Unlock()
	return !i.exited && i.pid != 0
}

// ExitCode returns the exit code (valid only after the process has exited).
func (i *Instance) ExitCode() int {
	i.mu.Lock()
	defer i.mu.Unlock()
	return i.exitCode
}

// WaitCh returns a channel that is closed when the process exits.
func (i *Instance) WaitCh() <-chan struct{} {
	return i.exitCh
}

// Stop sends SIGTERM; if the process does not exit within the grace period it
// sends SIGKILL.
func (i *Instance) Stop(gracePeriod time.Duration) error {
	i.mu.Lock()
	if i.exited || i.pid == 0 {
		i.mu.Unlock()
		return nil
	}
	pid := i.pid
	i.mu.Unlock()

	if err := syscall.Kill(pid, syscall.SIGTERM); err != nil && err != syscall.ESRCH {
		return fmt.Errorf("vmm: SIGTERM pid %d: %w", pid, err)
	}
	select {
	case <-i.exitCh:
		return nil
	case <-time.After(gracePeriod):
	}
	// Force-kill.
	if err := syscall.Kill(pid, syscall.SIGKILL); err != nil && err != syscall.ESRCH {
		return fmt.Errorf("vmm: SIGKILL pid %d: %w", pid, err)
	}
	select {
	case <-i.exitCh:
	case <-time.After(5 * time.Second):
		return fmt.Errorf("vmm: process %d did not exit after SIGKILL", pid)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

// Manager creates and tracks vmtainer Instance objects.
type Manager struct {
	mu           sync.Mutex
	binaryPath   string
	snapshotPath string
	instances    map[string]*Instance // sandboxID → instance
}

// Config carries construction parameters for Manager.
type Config struct {
	// BinaryPath is the full path to the vmtainer executable.
	// Defaults to searching PATH for "vmtainer".
	BinaryPath string
	// SnapshotPath is the golden KVM snapshot used by `vmtainer restore`.
	// Defaults to /var/lib/vmtainer/golden.snap.
	SnapshotPath string
}

// NewManager constructs a Manager and resolves the vmtainer binary path.
func NewManager(cfg Config) (*Manager, error) {
	bin := cfg.BinaryPath
	if bin == "" {
		var err error
		bin, err = exec.LookPath("vmtainer")
		if err != nil {
			// Allow running without the binary present so the server can still
			// start and serve status / list calls.
			logrus.Warn("vmm: 'vmtainer' binary not found in PATH — VM launches will fail")
			bin = "vmtainer"
		}
	}
	snap := cfg.SnapshotPath
	if snap == "" {
		snap = "/var/lib/vmtainer/golden.snap"
	}
	return &Manager{
		binaryPath:   bin,
		snapshotPath: snap,
		instances:    make(map[string]*Instance),
	}, nil
}

// Restore launches `vmtainer restore <snapshot> --config <configPath>` and
// returns an Instance.  The process's stdout/stderr are tee'd to logPath.
func (m *Manager) Restore(ctx context.Context, sandboxID, configPath, logPath string) (*Instance, error) {
	ctx, cancel := context.WithCancel(ctx)

	args := []string{"restore", m.snapshotPath, "--config", configPath}
	cmd := exec.CommandContext(ctx, m.binaryPath, args...)

	// Open log file.
	if err := os.MkdirAll(filepath.Dir(logPath), 0o755); err != nil {
		cancel()
		return nil, fmt.Errorf("vmm: mkdir log dir: %w", err)
	}
	logF, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		cancel()
		return nil, fmt.Errorf("vmm: open log file: %w", err)
	}

	// Write a startup banner so the log is never empty.
	fmt.Fprintf(logF, "=== vmtainer started at %s ===\n", time.Now().Format(time.RFC3339))
	fmt.Fprintf(logF, "=== cmd: %s %s ===\n", m.binaryPath, strings.Join(args, " "))

	mw := io.MultiWriter(logF, logrus.WithField("sandbox", sandboxID).Writer())
	cmd.Stdout = mw
	cmd.Stderr = mw

	// Run in a new process group so we can cleanly kill the whole tree.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}

	if err := cmd.Start(); err != nil {
		logF.Close()
		cancel()
		return nil, fmt.Errorf("vmm: start vmtainer: %w", err)
	}

	inst := &Instance{
		sandboxID:  sandboxID,
		cmd:        cmd,
		logFile:    logF,
		pid:        cmd.Process.Pid,
		exitCh:     make(chan struct{}),
		cancelFunc: cancel,
	}

	m.mu.Lock()
	m.instances[sandboxID] = inst
	m.mu.Unlock()

	// Background goroutine to wait for exit.
	go func() {
		defer close(inst.exitCh)
		err := cmd.Wait()
		inst.mu.Lock()
		inst.exited = true
		if err != nil {
			if exitErr, ok := err.(*exec.ExitError); ok {
				inst.exitCode = exitErr.ExitCode()
			} else {
				inst.exitCode = -1
			}
		}
		inst.mu.Unlock()
		logF.Close()
		cancel()

		logrus.WithFields(logrus.Fields{
			"sandbox":   sandboxID,
			"pid":       cmd.Process.Pid,
			"exit_code": inst.exitCode,
		}).Info("vmm: vmtainer process exited")

		m.mu.Lock()
		delete(m.instances, sandboxID)
		m.mu.Unlock()
	}()

	logrus.WithFields(logrus.Fields{
		"sandbox": sandboxID,
		"pid":     cmd.Process.Pid,
		"config":  configPath,
	}).Info("vmm: vmtainer started")

	return inst, nil
}

// GetInstance returns the running Instance for sandboxID, or nil.
func (m *Manager) GetInstance(sandboxID string) *Instance {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.instances[sandboxID]
}

// StopInstance signals the vmtainer process for sandboxID to stop.
func (m *Manager) StopInstance(sandboxID string, gracePeriod time.Duration) error {
	inst := m.GetInstance(sandboxID)
	if inst == nil {
		return nil // already gone
	}
	return inst.Stop(gracePeriod)
}

// ---------------------------------------------------------------------------
// Exec helpers
// ---------------------------------------------------------------------------

// ExecResult holds the output of a command executed inside the VM.
type ExecResult struct {
	Stdout   []byte
	Stderr   []byte
	ExitCode int32
}

// ExecSync executes cmd inside the sandbox VM via the virtiofs command channel.
//
// vmtainer's guest init reads commands written as JSON to
// /share/.vmtainer-exec/<request-id>/cmd and writes the result back to
// /share/.vmtainer-exec/<request-id>/result.  The rootfs is shared at /share
// (virtiofs mount), so the host can communicate by writing into the container
// rootfs directory.
//
// This is a best-effort fire-and-poll implementation.  Production use would
// benefit from a proper virtio-vsock channel.
func ExecSync(ctx context.Context, rootfsDir string, cmd []string, timeout time.Duration) (*ExecResult, error) {
	reqID := strconv.FormatInt(time.Now().UnixNano(), 36)
	execDir := filepath.Join(rootfsDir, ".vmtainer-exec", reqID)
	if err := os.MkdirAll(execDir, 0o755); err != nil {
		return nil, fmt.Errorf("vmm: exec: mkdir: %w", err)
	}
	defer os.RemoveAll(execDir)

	type execReq struct {
		Cmd     []string `json:"cmd"`
		Timeout int64    `json:"timeout_ms"`
	}
	reqData, _ := json.Marshal(execReq{Cmd: cmd, Timeout: timeout.Milliseconds()})
	if err := os.WriteFile(filepath.Join(execDir, "cmd"), reqData, 0o644); err != nil {
		return nil, fmt.Errorf("vmm: exec: write cmd: %w", err)
	}

	// Poll for result file.
	deadline := time.Now().Add(timeout)
	resultPath := filepath.Join(execDir, "result")
	for time.Now().Before(deadline) {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		default:
		}
		if _, err := os.Stat(resultPath); err == nil {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}

	data, err := os.ReadFile(resultPath)
	if err != nil {
		return nil, fmt.Errorf("vmm: exec: read result (timeout?): %w", err)
	}

	type execResult struct {
		Stdout   string `json:"stdout"`
		Stderr   string `json:"stderr"`
		ExitCode int32  `json:"exit_code"`
	}
	var res execResult
	if err := json.Unmarshal(data, &res); err != nil {
		return nil, fmt.Errorf("vmm: exec: parse result: %w", err)
	}
	return &ExecResult{
		Stdout:   []byte(res.Stdout),
		Stderr:   []byte(res.Stderr),
		ExitCode: res.ExitCode,
	}, nil
}
