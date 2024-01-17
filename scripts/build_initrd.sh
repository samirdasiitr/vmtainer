#!/bin/bash
set -euo pipefail

ROOT=/home/sadas/vmtainer
OUT=$ROOT/images
INITRD=$ROOT/initrd
INIT_SRC=$ROOT/initrd_src/init

mkdir -p "$INITRD" "$OUT" "$ROOT/initrd_src"

rm -rf "$INITRD"/*
mkdir -p "$INITRD"/{bin,dev,proc,sys,tmp,mnt,etc,share}

cp /usr/bin/busybox "$INITRD/bin/busybox"
chmod +x "$INITRD/bin/busybox"

for applet in sh echo cat sleep mkdir mount mountpoint umount pivot_root chroot head mknod halt poweroff reboot dd hostname ls; do
    ln -sf /bin/busybox "$INITRD/bin/$applet"
done

# Build signal_vmm — MMIO-based, no ioperm/iopl needed
cat > /tmp/signal_vmm.c << "SIGEOF"
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#define VMM_MMIO_GPA  0xd0000000ULL
#define VMM_MMIO_MAGIC 0x48594C54U   /* "HYLT" */

int main() {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        /* fallback: use /proc/self/mem to poke the physical address
         * via a direct mmap of /dev/mem. If /dev/mem is unavailable,
         * we can map the GPA via pagemap tricks, but the simplest
         * approach is to just mmap /dev/mem. */
        return 1;
    }
    volatile uint32_t *mmio = (volatile uint32_t *)mmap(
        NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, (off_t)VMM_MMIO_GPA);
    if (mmio == MAP_FAILED) {
        close(fd);
        return 1;
    }
    *mmio = VMM_MMIO_MAGIC;
    munmap((void *)mmio, 4096);
    close(fd);
    return 0;
}
SIGEOF
gcc -static -o "$INITRD/bin/signal_vmm" /tmp/signal_vmm.c

# Build serial_write — still IO-port based (kernel serial driver uses IO ports)
cat > /tmp/serial_write.c << "SWEOF"
#include <sys/io.h>
void serial_putchar(char c) {
    while (!(inb(0x3fd) & 0x20)) {}
    outb(c, 0x3f8);
}
void serial_puts(const char *s) {
    while (*s) serial_putchar(*s++);
    serial_putchar(10);
}
int main(int argc, char **argv) {
    if (iopl(3) < 0) return 1;
    for (int i = 1; i < argc; i++) serial_puts(argv[i]);
    return 0;
}
SWEOF
gcc -static -o "$INITRD/bin/serial_write" /tmp/serial_write.c

mknod "$INITRD/dev/console" c 5 1 2>/dev/null || true
mknod "$INITRD/dev/null" c 1 3 2>/dev/null || true
mknod "$INITRD/dev/ttyS0" c 4 64 2>/dev/null || true
mknod "$INITRD/dev/tty" c 5 0 2>/dev/null || true
mknod "$INITRD/dev/kmsg" c 1 11 2>/dev/null || true
mknod "$INITRD/dev/mem" c 1 1 2>/dev/null || true

# Copy init from source
if [ -f "$INIT_SRC" ]; then
    cp "$INIT_SRC" "$INITRD/init"
else
    cp "$ROOT/initrd/init" "$INITRD/init" 2>/dev/null || true
fi
chmod +x "$INITRD/init"

cd "$INITRD"
find . -print0 | cpio --null --create --format=newc 2>/dev/null | gzip -9 > "$OUT/initrd.cpio.gz"
echo "[build_initrd] done: $OUT/initrd.cpio.gz (size: $(stat -c%s $OUT/initrd.cpio.gz) bytes)"
