# ────────────────────────────────────────────────────────────────────────────
# Stage 1 — Build
# ────────────────────────────────────────────────────────────────────────────
FROM golang:1.21-bookworm AS builder

WORKDIR /src

# Cache module downloads separately from the source build.
COPY go.mod go.sum ./
RUN go mod download

# Copy source and build a fully-static binary.
COPY . .
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
    go build \
      -trimpath \
      -ldflags="-s -w -extldflags=-static" \
      -o /out/vmtainer-cri \
      ./cmd/vmtainer-cri

# ────────────────────────────────────────────────────────────────────────────
# Stage 2 — Runtime image
# ────────────────────────────────────────────────────────────────────────────
FROM debian:bookworm-slim

# Runtime dependencies:
#   iproute2      — ip tuntap, ip link (TAP device management)
#   iptables      — DNAT port-forward rules
#   docker.io     — docker pull / create / export for PullImage
#   tar           — rootfs extraction
#   netcat-openbsd — nc (used by the streaming portforward handler)
RUN apt-get update && apt-get install -y --no-install-recommends \
        iproute2 \
        iptables \
        docker.io \
        tar \
        ca-certificates \
        netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

# Copy the CRI shim binary.
COPY --from=builder /out/vmtainer-cri /usr/local/bin/vmtainer-cri

# The vmtainer binary itself must be supplied at runtime (host bind-mount or
# a separate layer).  We create the expected paths so the shim can start even
# when vmtainer is mounted in later.
RUN mkdir -p \
        /var/lib/vmtainer/images \
        /var/lib/vmtainer/containers \
        /var/lib/vmtainer/sandboxes \
        /var/lib/vmtainer/cni/net.d \
        /run/vmtainer \
        /opt/cni/bin

# Default data and socket paths — override with flags or env-based wrapper.
VOLUME ["/var/lib/vmtainer", "/run/vmtainer", "/opt/cni/bin"]

EXPOSE 10250

ENTRYPOINT ["/usr/local/bin/vmtainer-cri"]
CMD [
  "--socket",        "/run/vmtainer/vmtainer.sock",
  "--data-dir",      "/var/lib/vmtainer",
  "--snapshot",      "/var/lib/vmtainer/golden.snap",
  "--streaming-addr","0.0.0.0:10250",
  "--log-level",     "info"
]
