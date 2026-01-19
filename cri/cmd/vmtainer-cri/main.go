// vmtainer-cri is a Kubernetes CRI v1 runtime shim that drives the vmtainer
// KVM micro-VM runtime via gRPC.
//
// Usage:
//
//	vmtainer-cri [flags]
//
// Flags:
//
//	--socket          Unix socket path for the CRI gRPC server (default /run/vmtainer/vmtainer.sock)
//	--data-dir        Root data directory (default /var/lib/vmtainer)
//	--snapshot        Path to the golden KVM snapshot (default /var/lib/vmtainer/golden.snap)
//	--vmtainer        Path to the vmtainer binary (default: search PATH)
//	--subnet          IP subnet for guest VMs (default 10.200.0.0/16)
//	--gateway         Host-side default gateway (default 10.200.0.1)
//	--cni-conf-dir    Directory containing CNI *.conflist files (optional)
//	--cni-bin-dir     Directory containing CNI plugin binaries (optional)
//	--streaming-addr  host:port for the streaming (exec/attach) server (default 0.0.0.0:10250)
//	--log-level       Logrus log level: debug/info/warn/error (default info)
//	--log-json        Emit logs as JSON
package main

import (
	"context"
	"flag"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"github.com/sirupsen/logrus"
	"google.golang.org/grpc"
	"google.golang.org/grpc/keepalive"
	runtimev1 "k8s.io/cri-api/pkg/apis/runtime/v1"

	"github.com/vmtainer/vmtainer-cri/pkg/network"
	"github.com/vmtainer/vmtainer-cri/pkg/runtime"
	"github.com/vmtainer/vmtainer-cri/pkg/store"
	"github.com/vmtainer/vmtainer-cri/pkg/vmm"
)

func main() {
	var (
		socketPath     = flag.String("socket", "/run/vmtainer/vmtainer.sock", "CRI gRPC unix socket path")
		dataDir        = flag.String("data-dir", "/var/lib/vmtainer", "Root data directory")
		snapshotPath   = flag.String("snapshot", "/var/lib/vmtainer/golden.snap", "Golden KVM snapshot path")
		vmtainerBin    = flag.String("vmtainer", "", "Path to vmtainer binary (default: search PATH)")
		subnet         = flag.String("subnet", "10.200.0.0/16", "Guest VM IP subnet")
		gateway        = flag.String("gateway", "10.200.0.1", "Host-side default gateway for guests")
		cniConfDir     = flag.String("cni-conf-dir", "", "CNI configuration directory (optional)")
		cniBinDir      = flag.String("cni-bin-dir", "/opt/cni/bin", "CNI plugin binary directory")
		streamingAddr  = flag.String("streaming-addr", "0.0.0.0:10250", "Streaming server address (exec/attach/portforward)")
		logLevel       = flag.String("log-level", "info", "Log level (debug/info/warn/error)")
		logJSON        = flag.Bool("log-json", false, "Emit JSON logs")
	)
	flag.Parse()

	// ── Logging ──────────────────────────────────────────────────────────────
	if *logJSON {
		logrus.SetFormatter(&logrus.JSONFormatter{
			TimestampFormat: time.RFC3339Nano,
		})
	} else {
		logrus.SetFormatter(&logrus.TextFormatter{
			FullTimestamp:   true,
			TimestampFormat: time.RFC3339,
		})
	}
	lvl, err := logrus.ParseLevel(*logLevel)
	if err != nil {
		logrus.Fatalf("invalid log level %q: %v", *logLevel, err)
	}
	logrus.SetLevel(lvl)

	logrus.WithFields(logrus.Fields{
		"socket":   *socketPath,
		"data_dir": *dataDir,
		"snapshot": *snapshotPath,
	}).Info("vmtainer-cri: starting")

	// ── Data directories ──────────────────────────────────────────────────────
	for _, dir := range []string{
		*dataDir,
		filepath.Join(*dataDir, "sandboxes"),
		filepath.Join(*dataDir, "containers"),
		filepath.Join(*dataDir, "images"),
		filepath.Dir(*socketPath),
	} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			logrus.Fatalf("mkdir %s: %v", dir, err)
		}
	}

	// ── Metadata store ────────────────────────────────────────────────────────
	st, err := store.New(*dataDir)
	if err != nil {
		logrus.Fatalf("store.New: %v", err)
	}

	// ── Network manager ───────────────────────────────────────────────────────
	netMgr, err := network.NewManager(network.Config{
		Subnet:     *subnet,
		Gateway:    *gateway,
		CNIConfDir: *cniConfDir,
		CNIBinDir:  *cniBinDir,
	})
	if err != nil {
		logrus.Fatalf("network.NewManager: %v", err)
	}

	// If no CNI conf dir was given, generate a default conflist so CNI plugins
	// can be used if installed.
	if *cniConfDir == "" {
		defaultCNIDir := filepath.Join(*dataDir, "cni", "net.d")
		if err := os.MkdirAll(defaultCNIDir, 0o755); err == nil {
			confPath := filepath.Join(defaultCNIDir, "vmtainer.conflist")
			if _, err := os.Stat(confPath); os.IsNotExist(err) {
				if data, err := network.GenerateCNIConfList(*subnet, *gateway, "vmtainer0"); err == nil {
					_ = os.WriteFile(confPath, data, 0o644)
					logrus.Infof("vmtainer-cri: wrote default CNI conflist to %s", confPath)
				}
			}
		}
	}

	// ── VMM manager ───────────────────────────────────────────────────────────
	vmmMgr, err := vmm.NewManager(vmm.Config{
		BinaryPath:   *vmtainerBin,
		SnapshotPath: *snapshotPath,
	})
	if err != nil {
		logrus.Fatalf("vmm.NewManager: %v", err)
	}

	// ── Runtime service ───────────────────────────────────────────────────────
	runtimeSvc, err := runtime.NewRuntimeService(runtime.Config{
		DataDir:          *dataDir,
		StreamingAddress: *streamingAddr,
	}, st, netMgr, vmmMgr)
	if err != nil {
		logrus.Fatalf("runtime.NewRuntimeService: %v", err)
	}
	if err := runtimeSvc.StartStreamingServer(); err != nil {
		logrus.Fatalf("streaming server: %v", err)
	}

	// ── Image service ─────────────────────────────────────────────────────────
	imageSvc := runtime.NewImageService(st, *dataDir)

	// ── gRPC server ───────────────────────────────────────────────────────────
	// Remove stale socket if present.
	if err := os.Remove(*socketPath); err != nil && !os.IsNotExist(err) {
		logrus.Warnf("vmtainer-cri: remove stale socket %s: %v", *socketPath, err)
	}

	lis, err := net.Listen("unix", *socketPath)
	if err != nil {
		logrus.Fatalf("listen %s: %v", *socketPath, err)
	}
	// Set socket permissions so kubelet (running as root) can connect.
	if err := os.Chmod(*socketPath, 0o660); err != nil {
		logrus.Warnf("chmod socket: %v", err)
	}

	grpcServer := grpc.NewServer(
		grpc.KeepaliveParams(keepalive.ServerParameters{
			MaxConnectionIdle:     5 * time.Minute,
			MaxConnectionAge:      30 * time.Minute,
			MaxConnectionAgeGrace: 5 * time.Second,
			Time:                  1 * time.Minute,
			Timeout:               20 * time.Second,
		}),
		grpc.KeepaliveEnforcementPolicy(keepalive.EnforcementPolicy{
			MinTime:             30 * time.Second,
			PermitWithoutStream: true,
		}),
		grpc.ChainUnaryInterceptor(loggingInterceptor),
	)

	runtimev1.RegisterRuntimeServiceServer(grpcServer, runtimeSvc)
	runtimev1.RegisterImageServiceServer(grpcServer, imageSvc)

	logrus.Infof("vmtainer-cri: gRPC server listening on unix://%s", *socketPath)

	// ── Signal handling ───────────────────────────────────────────────────────
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGINT)

	serverErrCh := make(chan error, 1)
	go func() {
		serverErrCh <- grpcServer.Serve(lis)
	}()

	select {
	case sig := <-sigCh:
		logrus.Infof("vmtainer-cri: received signal %v, shutting down", sig)
		grpcServer.GracefulStop()
	case err := <-serverErrCh:
		if err != nil {
			logrus.Fatalf("vmtainer-cri: gRPC server error: %v", err)
		}
	}

	logrus.Info("vmtainer-cri: shutdown complete")
}

// loggingInterceptor logs every RPC call with its method name and duration.
func loggingInterceptor(ctx context.Context, req interface{}, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (interface{}, error) {
	start := time.Now()
	resp, err := handler(ctx, req)
	dur := time.Since(start)
	log := logrus.WithFields(logrus.Fields{
		"method": info.FullMethod,
		"dur_ms": dur.Milliseconds(),
	})
	if err != nil {
		log.WithError(err).Warn("grpc: request failed")
	} else {
		log.Debug("grpc: request ok")
	}
	return resp, err
}
