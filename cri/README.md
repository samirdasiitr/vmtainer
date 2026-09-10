<!--
Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.

PROPRIETARY AND CONFIDENTIAL.
Unauthorized copying, reproduction, distribution, or modification of this
file, via any medium, is strictly prohibited.
All rights reserved.
-->

# vmtainer-cri

A Kubernetes **CRI v1** gRPC runtime shim for **vmtainer**, a KVM-based
micro-VM container runtime.

Each pod sandbox is backed by a single vmtainer KVM virtual machine that boots
a pre-snapshotted Linux 6.10 kernel in milliseconds, shares an OCI-image
rootfs over virtiofs, and runs the container entrypoint inside the VM.

---

## Architecture

```
kubelet
  │  (CRI gRPC / unix socket)
  ▼
vmtainer-cri                 /run/vmtainer/vmtainer.sock
  ├── RuntimeService
  │     RunPodSandbox  ──► ip tuntap add vmtapN
  │                   ──► allocate 10.200.x.x/16
  │                   ──► write config.json
  │     StartContainer ──► vmtainer restore <snap> --config config.json
  │     ExecSync       ──► virtiofs .vmtainer-exec/ channel
  │     PortForward    ──► iptables DNAT + TCP proxy
  │
  └── ImageService
        PullImage  ──► docker pull + docker export | tar -x → rootfs/
        RemoveImage──► rm -rf rootfs/
```

### Key design decisions

| Concern | Approach |
|---|---|
| **Pod ↔ VM mapping** | 1 pod sandbox = 1 vmtainer VM process |
| **Container rootfs** | OCI image extracted via `docker export`, copied per-container |
| **Networking** | TAP device `vmtapN` per VM; IP from 10.200.0.0/16 pool; CNI if available |
| **Metadata store** | JSON files under `/var/lib/vmtainer/{sandboxes,containers,images}/` |
| **Exec/Attach** | virtiofs side-channel (`/share/.vmtainer-exec/<id>/cmd`); streaming via HTTP upgrade |
| **Port forwarding** | `iptables DNAT PREROUTING` + TCP proxy in streaming server |
| **Snapshot** | Single golden snapshot (`golden.snap`) restored for every VM |

---

## Project layout

```
vmtainer-cri/
├── cmd/vmtainer-cri/
│   └── main.go               gRPC server entry point + flag parsing
├── pkg/
│   ├── runtime/
│   │   ├── runtime.go        RuntimeService (RunPodSandbox … PortForward)
│   │   └── image.go          ImageService (PullImage … ImageFsInfo)
│   ├── vmm/
│   │   └── vmm.go            vmtainer binary wrapper + ExecSync channel
│   ├── store/
│   │   └── store.go          JSON-file metadata store
│   └── network/
│       └── cni.go            TAP management, IP pool, CNI integration
├── Dockerfile
├── go.mod
└── README.md
```

---

## Building

### From source

```bash
# Requires Go 1.21+
go build -o vmtainer-cri ./cmd/vmtainer-cri
```

### Container image

```bash
docker build -t vmtainer-cri:latest .
```

### Cross-compile for a Linux node

```bash
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
  go build -trimpath -ldflags="-s -w" \
  -o vmtainer-cri-linux-amd64 ./cmd/vmtainer-cri
```

---

## Prerequisites on the node

| Dependency | Purpose |
|---|---|
| `vmtainer` binary | KVM micro-VM launcher (must be in `PATH` or set with `--vmtainer`) |
| `golden.snap` | Pre-boot KVM snapshot created by `vmtainer snapshot` |
| `ip` (iproute2) | TAP device lifecycle |
| `iptables` | Port-forwarding DNAT rules |
| `docker` | Image pull + rootfs extraction |
| KVM device | `/dev/kvm` must be accessible |
| `virtiofsd` | Guest rootfs sharing (started by vmtainer) |

---

## Running

```bash
# Run directly (requires root for TAP/iptables)
sudo vmtainer-cri \
  --socket        /run/vmtainer/vmtainer.sock \
  --data-dir      /var/lib/vmtainer \
  --snapshot      /var/lib/vmtainer/golden.snap \
  --vmtainer      /usr/local/bin/vmtainer \
  --subnet        10.200.0.0/16 \
  --gateway       10.200.0.1 \
  --streaming-addr 0.0.0.0:10250 \
  --log-level     info
```

### All flags

| Flag | Default | Description |
|---|---|---|
| `--socket` | `/run/vmtainer/vmtainer.sock` | CRI gRPC unix socket |
| `--data-dir` | `/var/lib/vmtainer` | Root data directory |
| `--snapshot` | `/var/lib/vmtainer/golden.snap` | Golden KVM snapshot |
| `--vmtainer` | *(search PATH)* | vmtainer binary path |
| `--subnet` | `10.200.0.0/16` | Guest VM IP pool |
| `--gateway` | `10.200.0.1` | Host default gateway for guests |
| `--cni-conf-dir` | *(empty)* | CNI conflist directory |
| `--cni-bin-dir` | `/opt/cni/bin` | CNI plugin binary dir |
| `--streaming-addr` | `0.0.0.0:10250` | Exec/Attach/PortForward server |
| `--log-level` | `info` | `debug/info/warn/error` |
| `--log-json` | `false` | Emit JSON logs |

---

## Configuring kubelet to use vmtainer-cri

### 1. Install the binary

```bash
install -m 755 vmtainer-cri /usr/local/bin/vmtainer-cri
```

### 2. Create a systemd unit

```ini
# /etc/systemd/system/vmtainer-cri.service
[Unit]
Description=vmtainer CRI runtime shim
After=network.target
Before=kubelet.service

[Service]
ExecStart=/usr/local/bin/vmtainer-cri \
    --socket        /run/vmtainer/vmtainer.sock \
    --data-dir      /var/lib/vmtainer \
    --snapshot      /var/lib/vmtainer/golden.snap \
    --streaming-addr 0.0.0.0:10250
Restart=always
RestartSec=5s
LimitNOFILE=1048576

[Install]
WantedBy=multi-user.target
```

```bash
systemctl daemon-reload
systemctl enable --now vmtainer-cri
```

### 3. Create a RuntimeClass

```yaml
# vmtainer-runtimeclass.yaml
apiVersion: node.k8s.io/v1
kind: RuntimeClass
metadata:
  name: vmtainer
handler: vmtainer
```

```bash
kubectl apply -f vmtainer-runtimeclass.yaml
```

### 4. Configure kubelet

Add the runtime handler to `/etc/containerd/config.toml` (if using
containerd as the primary CRI) **or** point kubelet directly at the socket.

**Option A — kubelet flags (standalone / kubeadm)**

Edit `/etc/default/kubelet` or the kubelet `KubeletConfiguration`:

```yaml
# /etc/kubernetes/kubelet-config.yaml
apiVersion: kubelet.config.k8s.io/v1beta1
kind: KubeletConfiguration
containerRuntimeEndpoint: "unix:///run/vmtainer/vmtainer.sock"
imageServiceEndpoint:      "unix:///run/vmtainer/vmtainer.sock"
```

Or pass flags directly:

```bash
kubelet \
  --container-runtime-endpoint unix:///run/vmtainer/vmtainer.sock \
  --image-service-endpoint      unix:///run/vmtainer/vmtainer.sock \
  ...
```

**Option B — containerd runtime handler shim**

If the node already runs containerd as its primary CRI, add a runtime handler
section that delegates to vmtainer-cri:

```toml
# /etc/containerd/config.toml  (relevant excerpt)
[plugins."io.containerd.grpc.v1.cri".containerd.runtimes.vmtainer]
  runtime_type = "io.containerd.runc.v2"
  # Override the socket — containerd will proxy CRI calls to vmtainer-cri.
  # NOTE: this requires a containerd plugin or kata-style shim integration.
  # The simplest approach for early testing is Option A.
```

> **Recommendation:** For development and testing, use Option A (direct socket)
> with a dedicated node. Production multi-runtime clusters typically integrate
> through kata-style containerd shims.

### 5. Use vmtainer for a pod

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: hello-vmtainer
spec:
  runtimeClassName: vmtainer          # ← selects the handler
  containers:
  - name: hello
    image: busybox:latest
    command: ["/bin/echo", "hello from KVM!"]
```

---

## Data directory layout

```
/var/lib/vmtainer/
├── golden.snap                  # KVM snapshot (pre-created)
├── images/
│   └── sha256:<digest>/
│       ├── meta.json
│       └── rootfs/              # extracted OCI image filesystem
├── containers/
│   └── <container-id>/
│       ├── meta.json
│       └── rootfs/              # per-container copy (writable)
├── sandboxes/
│   └── <sandbox-id>/
│       ├── meta.json
│       ├── config.json          # vmtainer VMConfig
│       └── vm.log               # vmtainer stdout/stderr
└── cni/
    └── net.d/
        └── vmtainer.conflist    # auto-generated CNI config
```

---

## Exec channel protocol

vmtainer-cri communicates with the guest init over the shared virtiofs
volume.  The guest init polls for request files:

```
/share/.vmtainer-exec/<request-id>/cmd     ← JSON {cmd:[…], timeout_ms:N}
/share/.vmtainer-exec/<request-id>/result  ← JSON {stdout:"…", stderr:"…", exit_code:N}
```

The host writes `cmd`, polls for `result`.  The guest init processes the
request, writes `result`, and cleans up.  This is a simple, dependency-free
side-channel that requires no extra kernel modules.

For interactive sessions (Attach) a virtio-vsock connection is recommended as
a future enhancement.

---

## CNI networking

If `--cni-conf-dir` points to a directory with CNI conflist files, vmtainer-cri
will delegate network setup to the CNI plugins.  Otherwise it manages TAP
devices and IP allocation itself.

A minimal bridge CNI conflist is auto-generated at
`/var/lib/vmtainer/cni/net.d/vmtainer.conflist` on first start.  Install CNI
plugins from https://github.com/containernetworking/plugins to use it.

---

## Limitations

- **Attach / interactive exec**: stub-level only; requires virtio-vsock or a
  serial console SPDY upgrade for full implementation.
- **CPU/memory limits**: vmtainer currently uses a fixed 64 MB RAM per VM.
  Resource limit enforcement is a no-op in `UpdateContainerResources`.
- **Checkpointing**: not supported (`CheckpointContainer` returns Unimplemented).
- **Metrics**: no CPU/memory metrics are currently collected from inside the VM.
- **Multi-container pods**: only one container per sandbox is meaningful (maps
  to the VM entrypoint); additional containers share the same VM process.
- **Image authentication**: credentials are passed to `docker login` before
  pull; token refresh is not handled automatically.
