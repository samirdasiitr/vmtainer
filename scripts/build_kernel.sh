#!/bin/bash
set -euo pipefail

ROOT=/home/sadas/vmtainer
BUILD=$ROOT/build/kernel
SRC=$BUILD/linux
OUT=$ROOT/images
JOBS=$(nproc)

mkdir -p "$BUILD" "$OUT"

KERNEL_TARBALL="linux-6.10.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/$KERNEL_TARBALL"

if [[ ! -d "$SRC" ]]; then
    if [[ ! -f "$BUILD/$KERNEL_TARBALL" ]]; then
        echo "[build_kernel] downloading kernel source..."
        wget -q --show-progress -O "$BUILD/$KERNEL_TARBALL" "$KERNEL_URL"
    fi
    echo "[build_kernel] extracting..."
    tar -xf "$BUILD/$KERNEL_TARBALL" -C "$BUILD"
    mv "$BUILD/linux-6.10" "$SRC"
fi

cd "$SRC"

echo "[build_kernel] generating tiny config..."
make O="$BUILD/out" ARCH=x86_64 tinyconfig -j$JOBS

# Enable the minimal features we need
cd "$BUILD/out"
cat > "$BUILD/minimal.config" <<CONF
# Basic architecture
CONFIG_64BIT=y
CONFIG_X86_64=y
CONFIG_OUTPUT_FORMAT="elf64-x86-64"
CONFIG_ARCH_DEFCONFIG="arch/x86/configs/x86_64_defconfig"
CONFIG_GENERIC_CALIBRATE_DELAY=y
CONFIG_HZ=250
CONFIG_SMP=n
CONFIG_PREEMPT_NONE=y

# Console / serial
CONFIG_TTY=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_SERIAL_8250_NR_UARTS=4
CONFIG_SERIAL_8250_RUNTIME_UARTS=4

# printk and earlycon
CONFIG_PRINTK=y
CONFIG_CONSOLE_TRANSLATIONS=y
CONFIG_VT=y
CONFIG_VT_CONSOLE=y

# initrd
CONFIG_BLK_DEV=y
CONFIG_BLK_DEV_INITRD=y

# devtmpfs
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y

# filesystems for initrd
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_TMPFS=y
CONFIG_RAMFS=y

# ELF and binfmt
CONFIG_BINFMT_ELF=y

# No PCI
# CONFIG_PCI is not set
# CONFIG_PCI_DOMAINS is not set

# virtio-mmio (for later extension)
CONFIG_VIRTIO=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y

# Hugepage support (guest and host can use hugetlbfs)
CONFIG_HUGETLBFS=y
CONFIG_HUGETLB_PAGE=y

# Minimal networking for virtio-net
CONFIG_NET=y
CONFIG_NET_CORE=y
CONFIG_INET=y
CONFIG_NETDEVICES=y
CONFIG_VIRTIO_NET=y

# FUSE / virtiofs
CONFIG_FUSE_FS=y
CONFIG_VIRTIO_FS=y
CONFIG_9P_FS=y
CONFIG_9P_FS_POSIX_ACL=y

# Keep it small
CONFIG_MODULES=n
# Unix domain sockets (needed by nginx, many apps)
CONFIG_UNIX=y

# Power / reset
CONFIG_ACPI=n
CONFIG_REBOOT=y
CONFIG_POWER_RESET=y
CONF

# Apply the fragment to the tiny config
# First create a base .config from the fragment, then run oldconfig
cp "$BUILD/minimal.config" .config
make olddefconfig

echo "[build_kernel] building bzImage with $JOBS jobs..."
make -j$JOBS

cp arch/x86/boot/bzImage "$OUT/bzImage"
cp vmlinux "$OUT/vmlinux" 2>/dev/null || true

echo "[build_kernel] done: $OUT/bzImage"
