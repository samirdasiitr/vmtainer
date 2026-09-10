<!--
Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.

PROPRIETARY AND CONFIDENTIAL.
Unauthorized copying, reproduction, distribution, or modification of this
file, via any medium, is strictly prohibited.
All rights reserved.
-->

# 09 — Disk Images and `bximage`

## 1. Why Disk Images?

Even when a VMM supports direct kernel boot, a real operating system needs a root filesystem. A disk image is a file that the VMM presents as a hard disk to the guest. The VMM intercepts port I/O or MMIO accesses to a virtual disk controller and translates them into reads and writes against the image file.

This chapter covers:

1. Using **Bochs `bximage`** to create disk images.
2. Attaching the image to a simple VMM.
3. Loading a `bzImage` from an image (optional) or booting from the disk.
4. A minimal block-device simulation in userspace.

## 2. Installing `bximage`

`bximage` is part of the **Bochs** emulator. Install it from your distribution package manager:

```bash
# Debian/Ubuntu
sudo apt-get install bochs

# Fedora/RHEL
sudo dnf install bochs

# Arch
sudo pacman -S bochs
```

After installation, `bximage` is usually in `/usr/bin/bximage`. It can create flat raw images, sparse images, and other formats.

## 3. Creating a Flat Raw Disk Image

A **flat raw image** is just a file of `N` bytes. It is the easiest format to implement because `offset = (lba * 512)` and you read or write with `pread`/`pwrite`.

### Interactive `bximage`

```bash
bximage
```

Select:

1. `Create new floppy or hard disk image`
2. `hd`
3. `flat`
4. Enter size, e.g., `128` for 128 MB.
5. Enter filename, e.g., `disk.img`.

### Non-interactive example

`bximage` is typically interactive, but you can script it:

```bash
bximage <<EOF
create
hd
flat
128
disk.img
EOF
```

Alternatively, use `qemu-img` or `dd` to create a raw image:

```bash
dd if=/dev/zero of=disk.img bs=1M count=128
```

## 4. Partitioning and Formatting the Image

A raw image must be partitioned and formatted before the guest can use it. You can do this with `parted`/`fdisk` and a loop device:

```bash
# Create MBR partition table + one primary Linux partition
sudo losetup -fP disk.img
LOOP=$(losetup -j disk.img | head -1 | cut -d: -f1)
echo ',,L,*' | sudo sfdisk "$LOOP"

# Format the partition
PART="${LOOP}p1"
sudo mkfs.ext4 "$PART"

# Mount and copy files
sudo mkdir -p /mnt/vmdisk
sudo mount "$PART" /mnt/vmdisk
# ... copy rootfs, kernel, initrd, etc ...
sudo umount /mnt/vmdisk
sudo losetup -d "$LOOP"
```

For testing, the image can also contain a boot sector that the VMM loads. A simple flat image with an ext4 filesystem on the whole device (no partition table) is easier to implement if your guest can boot from it.

## 5. VMM Disk-Image Access

### Simple IDE/ATA model

A minimal IDE controller uses the I/O ports:

```
Primary channel:
  0x1f0  — data
  0x1f1  — error / features
  0x1f2  — sector count
  0x1f3  — LBA low
  0x1f4  — LBA mid
  0x1f5  — LBA high
  0x1f6  — drive/head
  0x1f7  — command / status
  0x3f6  — alternate status / device control
```

For a read command (0x20), the guest places the starting LBA in `0x1f3..0x1f6`, the sector count in `0x1f2`, and writes `0x20` to `0x1f7`. The VMM reads 512 bytes per sector from the image and copies them to the data port.

### State

```c
struct ide_state {
    int fd;
    uint32_t lba;
    uint16_t count;
    uint16_t io_offset;
    uint8_t  status;
    uint8_t  command;
    uint8_t  data[512];
    bool     pending;
};
```

### Port I/O handler

```c
void handle_ide(uint16_t port, uint8_t *data, int size, int dir, struct ide_state *ide)
{
    switch (port) {
    case 0x1f0:
        if (dir == KVM_EXIT_IO_IN) {
            for (int i = 0; i < size; i++)
                data[i] = ide->data[ide->io_offset++];
        } else {
            for (int i = 0; i < size; i++)
                ide->data[ide->io_offset++] = data[i];
        }
        break;
    case 0x1f2:
        if (dir == KVM_EXIT_IO_OUT)
            ide->count = data[0];
        else
            data[0] = ide->count;
        break;
    case 0x1f3:
        if (dir == KVM_EXIT_IO_OUT)
            ide->lba = (ide->lba & ~0xff) | data[0];
        else
            data[0] = ide->lba & 0xff;
        break;
    case 0x1f4:
        if (dir == KVM_EXIT_IO_OUT)
            ide->lba = (ide->lba & ~0xff00) | (data[0] << 8);
        else
            data[0] = (ide->lba >> 8) & 0xff;
        break;
    case 0x1f5:
        if (dir == KVM_EXIT_IO_OUT)
            ide->lba = (ide->lba & ~0xff0000) | (data[0] << 16);
        else
            data[0] = (ide->lba >> 16) & 0xff;
        break;
    case 0x1f6:
        if (dir == KVM_EXIT_IO_OUT) {
            /* bit 6 = LBA mode */
            ide->lba = (ide->lba & 0x0fffff) | ((data[0] & 0x0f) << 24);
        }
        else
            data[0] = 0xa0 | ((ide->lba >> 24) & 0x0f);
        break;
    case 0x1f7:
        if (dir == KVM_EXIT_IO_OUT) {
            ide->command = data[0];
            if (ide->command == 0x20)
                ide_do_read(ide);
            else if (ide->command == 0x30)
                ide_do_write(ide);
            else if (ide->command == 0xec)
                ide_do_identify(ide);
        } else {
            data[0] = ide->status;
        }
        break;
    case 0x3f6:
        if (dir == KVM_EXIT_IO_IN)
            data[0] = ide->status;
        break;
    }
}
```

### Read implementation

```c
void ide_do_read(struct ide_state *ide)
{
    off_t off = (off_t)ide->lba * 512;
    ssize_t n = pread(ide->fd, ide->data, 512, off);
    if (n != 512) {
        ide->status = 0x01;  /* error */
        return;
    }
    ide->io_offset = 0;
    ide->status = 0x40;  /* ready */

    /* Raise an interrupt. */
    struct kvm_interrupt intr = { .irq = 14 };  /* IDE primary IRQ */
    ioctl(vcpu_fd, KVM_INTERRUPT, &intr);
}
```

### Write implementation

```c
void ide_do_write(struct ide_state *ide)
{
    off_t off = (off_t)ide->lba * 512;
    ssize_t n = pwrite(ide->fd, ide->data, 512, off);
    if (n != 512)
        ide->status = 0x01;
    else
        ide->status = 0x40;

    struct kvm_interrupt intr = { .irq = 14 };
    ioctl(vcpu_fd, KVM_INTERRUPT, &intr);
}
```

For a multi-sector read or write, the guest will read `0x1f0` 256 times (for 16-bit data) and then the VMM automatically advances to the next LBA.

## 6. Loading a bzImage from a Disk Image

Some VMMs store the kernel and initrd inside the disk image and boot from there. For a minimal direct boot, it is simpler to load the kernel from the host filesystem and the root filesystem from the disk image.

If you really want to boot from disk:

1. Place a boot sector in the first 512 bytes of the image.
2. The boot sector loads a second-stage loader or the kernel.
3. The VMM sets `CS:IP = 0x0000:0x7c00` and `rip = 0x7c00`.
4. The boot sector takes over.

Writing a boot loader is outside the scope of this guide; use a pre-built one (e.g., `syslinux` or `extlinux`) or use the direct kernel path.

## 7. `bximage` Output Formats

`bximage` can produce:

- `flat` — raw, pre-allocated; easy to implement.
- `sparse` — sparse file; saves disk space.
- `growing` — grows on demand.
- `volatile` — in-memory.

For a VMM, `flat` or `sparse` is recommended. `flat` is the most compatible with `pread`/`pwrite`.

## 8. Image Size and Geometry

Many BIOSs and bootloaders expect a **CHS geometry** (cylinders, heads, sectors). Modern LBA kernels do not care, but if you use `bximage` it will print the geometry. You can also make one up for the IDE identify data.

A 128 MiB flat image:

```
Cylinders: 261
Heads:     16
Sectors:   63
LBA48:     262144 sectors (128 MiB)
```

For the `IDENTIFY DEVICE` command, the VMM returns a 512-byte buffer containing:

- General config at word 0.
- Number of logical cylinders at word 1.
- Number of logical heads at word 3.
- Number of logical sectors per track at word 6.
- Total number of logical sectors at words 60-61 and 100-103.

This is enough for the guest to see a real disk.

## 9. Example: Attach a Disk to the VMM

```c
struct ide_state ide;

void vm_init_disks(struct vm *vm)
{
    ide.fd = open("disk.img", O_RDWR);
    if (ide.fd < 0)
        err(1, "disk.img");

    ide.status = 0x40;  /* ready, no error */
    ide.lba = 0;
    ide.count = 0;
    ide.io_offset = 0;
    ide.pending = false;

    /* Register PIO ports 0x1f0..0x1f7 and 0x3f6. */
    for (uint16_t p = 0x1f0; p <= 0x1f7; p++)
        vm_register_port(vm, p, handle_ide, &ide);
    vm_register_port(vm, 0x3f6, handle_ide, &ide);
}
```

In the `KVM_EXIT_IO` handler, the VMM dispatches by port:

```c
void handle_io(struct kvm_run *run, struct vm *vm)
{
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    uint16_t port = run->io.port;

    struct iodev *dev = vm_find_port(vm, port);
    if (dev) {
        dev->handler(port, data, run->io.size,
                     run->io.direction == KVM_EXIT_IO_OUT ? 1 : 0,
                     dev->ctx);
    } else {
        /* default: return 0xff on IN, ignore OUT. */
        if (run->io.direction == KVM_EXIT_IO_IN)
            memset(data, 0xff, run->io.size);
    }
}
```

## 10. Using `qemu-img` as an Alternative

`qemu-img` is more common than `bximage` today. Create a raw image:

```bash
qemu-img create -f raw disk.img 1G
```

Create a qcow2 image (more advanced, copy-on-write):

```bash
qemu-img create -f qcow2 disk.qcow2 1G
```

A VMM can still read `qcow2` with a library, but `raw` is the simplest to implement.

## 11. Summary

- Use `bximage` (or `qemu-img`/`dd`) to create a flat raw image.
- Partition and format it with `fdisk`/`parted` and `mkfs.ext4`.
- Implement a minimal IDE/ATA controller at I/O ports `0x1f0..0x1f7`.
- Translate LBA reads/writes to `pread`/`pwrite` on the image file.
- Raise IRQ 14 after each command completes.
- Return `IDENTIFY` data so the guest can determine the disk size.
- Attach the image by registering port handlers in the `KVM_EXIT_IO` dispatch loop.
