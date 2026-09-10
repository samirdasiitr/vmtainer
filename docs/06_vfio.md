# 06 — VFIO: PCI Device Passthrough

## 1. What Is VFIO?

**VFIO (Virtual Function I/O)** is a Linux userspace driver framework that allows a VMM to directly assign a physical device — usually a PCI or PCIe device — to a guest. With an IOMMU, VFIO groups the device into an **IOMMU group**, isolates its DMA, and exposes a safe ioctl interface to the VMM. The VMM then maps the device's PCI configuration space, MMIO, and I/O port regions into the VM.

The benefits of passthrough are:

- Near-native I/O performance.
- Full use of device offloads (e.g., SR-IOV, RDMA, GPU, NVMe).
- Minimal or no host kernel involvement in the data path.

The requirements are:

- IOMMU hardware: Intel VT-d, AMD-Vi, or ARM SMMU.
- Host IOMMU enabled in firmware and Linux (e.g., `intel_iommu=on` or `amd_iommu=on`).
- The device is bound to the `vfio-pci` driver, not to a host driver.
- The device is not part of a group that contains other devices the VMM does not want to assign.

## 2. VFIO Architecture

```
+-------------------------------------+
|  Guest                              |
|  (sees physical PCI device)         |
+-------------------------------------+
|  KVM                                |
|  - PCI MSIs, INTx, DMA via IOMMU    |
+-------------------------------------+
|  Userspace VMM                      |
|  - VFIO container                   |
|  - VFIO group                       |
|  - VFIO device descriptor           |
+-------------------------------------+
|  /dev/vfio/vfio                     |
|  /dev/vfio/N                        |
|  /sys/bus/pci/devices/...           |
+-------------------------------------+
```

A **container** is the IOMMU abstraction. A **group** is a set of devices that must be assigned together because they are not isolated from each other at the IOMMU level. A **device** is the actual PCI function.

## 3. Preparing the Host

### Enable IOMMU

Add to the kernel command line:

- Intel: `intel_iommu=on iommu=pt` or `intel_iommu=on`.
- AMD: `amd_iommu=on iommu=pt`.

Verify:

```bash
dmesg | grep -i iommu
ls /sys/kernel/iommu_groups/   # should show group directories
```

### Bind the device to vfio-pci

Suppose the device is at `0000:01:00.0`:

```bash
echo "0000:01:00.0" > /sys/bus/pci/devices/0000:01:00.0/driver/unbind

echo "vfio-pci" > /sys/bus/pci/drivers_probe
echo "0000:01:00.0" > /sys/bus/pci/drivers/vfio-pci/bind
```

Or use the vendor/device IDs:

```bash
echo "vfio-pci" > /sys/bus/pci/devices/0000:01:00.0/driver_override
echo "0000:01:00.0" > /sys/bus/pci/drivers/vfio-pci/bind
```

Check the new group:

```bash
ls -l /sys/bus/pci/devices/0000:01:00.0/iommu_group
# -> .../iommu_groups/NN
```

## 4. Opening the VFIO Container

The first step in userspace is to open `/dev/vfio/vfio` and ensure the API version and IOMMU type are supported.

```c
#include <linux/vfio.h>
#include <sys/ioctl.h>

int container = open("/dev/vfio/vfio", O_RDWR);
if (container < 0)
    err(1, "/dev/vfio/vfio");

int version = ioctl(container, VFIO_GET_API_VERSION);
if (version != VFIO_API_VERSION)
    errx(1, "VFIO API version mismatch");

int iommu_type = ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU);
if (iommu_type != 1)
    errx(1, "VFIO type1 IOMMU not supported");

if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0)
    err(1, "VFIO_SET_IOMMU");
```

`VFIO_TYPE1_IOMMU` is the most common x86 IOMMU type; `VFIO_TYPE1v2_IOMMU` adds new features. Use `VFIO_TYPE1_IOMMU` to start.

## 5. Opening the Group

The IOMMU group number for the device is found from `/sys/bus/pci/devices/.../iommu_group`. Open the corresponding `/dev/vfio/N`.

```c
char path[64];
snprintf(path, sizeof(path), "/dev/vfio/%d", group_id);

int group = open(path, O_RDWR);
if (group < 0)
    err(1, "%s", path);

struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
ioctl(group, VFIO_GROUP_GET_STATUS, &group_status);

if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE))
    errx(1, "VFIO group %d is not viable", group_id);

if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0)
    err(1, "VFIO_GROUP_SET_CONTAINER");
```

A group is viable only if all its devices are bound to a VFIO driver. If the group contains another device still bound to a host driver, you must either unbind it or assign the whole group to a different VM.

## 6. Getting the Device

```c
struct vfio_device_info dev_info = { .argsz = sizeof(dev_info) };

int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, "0000:01:00.0");
if (device < 0)
    err(1, "VFIO_GROUP_GET_DEVICE_FD");

ioctl(device, VFIO_DEVICE_GET_INFO, &dev_info);

printf("Device: %d regions, %d irqs, flags 0x%x\n",
       dev_info.num_regions, dev_info.num_irqs, dev_info.flags);
```

`VFIO_GROUP_GET_DEVICE_FD` takes the PCI BDF as a string.

## 7. Querying and Mapping Regions

A VFIO device has several regions:

- `VFIO_PCI_BAR0_REGION_INDEX` .. `VFIO_PCI_BAR5_REGION_INDEX` — PCI BARs.
- `VFIO_PCI_ROM_REGION_INDEX` — the option ROM.
- `VFIO_PCI_CONFIG_REGION_INDEX` — the PCI configuration space.
- `VFIO_PCI_VGA_REGION_INDEX` — VGA I/O and MMIO (if applicable).

### Get region info

```c
for (int i = VFIO_PCI_CONFIG_REGION_INDEX;
     i <= VFIO_PCI_BAR5_REGION_INDEX; i++) {
    struct vfio_region_info reg = { .argsz = sizeof(reg) };
    reg.index = i;

    int ret = ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &reg);
    if (ret < 0) continue;

    printf("Region %d: offset 0x%llx, size 0x%llx, flags 0x%x\n",
           i, (unsigned long long)reg.offset,
           (unsigned long long)reg.size, reg.flags);

    /* Map the region using the file offset. */
    void *map = mmap(NULL, reg.size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED, device, reg.offset);
    if (map != MAP_FAILED) {
        bar_mapping[i] = map;
        bar_size[i] = reg.size;
    }
}
```

The `mmap` offset for a region is `reg.offset`. The size and offset are returned by `VFIO_DEVICE_GET_REGION_INFO`.

### Read and write the PCI config space

```c
uint8_t config[4096];

struct vfio_region_info cfg = { .argsz = sizeof(cfg) };
cfg.index = VFIO_PCI_CONFIG_REGION_INDEX;
ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &cfg);

pread(device, config, cfg.size, cfg.offset);
```

Or `mmap` the config space and read directly:

```c
uint8_t *cfg_ptr = mmap(NULL, cfg.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        device, cfg.offset);
```

## 8. Setting Up DMA Mappings

With IOMMU type1, the VMM must map each guest-physical memory range into the IOMMU so the device can DMA to and from guest RAM. This is done with `VFIO_IOMMU_MAP_DMA`.

```c
struct vfio_iommu_type1_dma_map dma = {
    .argsz = sizeof(dma),
    .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
    .vaddr = (uint64_t)(uintptr_t)guest_mem,  /* host virtual */
    .iova  = 0,                                /* guest physical */
    .size  = GUEST_RAM_SIZE,
};

ioctl(container, VFIO_IOMMU_MAP_DMA, &dma);
```

- `vaddr`: the host-virtual address of the guest RAM.
- `iova`: the guest-physical address, i.e., the DMA address the device sees.
- `size`: the length of the mapping.
- `flags`: `VFIO_DMA_MAP_FLAG_READ`, `VFIO_DMA_MAP_FLAG_WRITE`, both, or `VFIO_DMA_MAP_FLAG_MMIO` for non-RAM.

To support guest physical address holes or multiple memory slots, call `VFIO_IOMMU_MAP_DMA` once per slot. To remove a mapping, use `VFIO_IOMMU_UNMAP_DMA`.

### Dirty page tracking

If you want to migrate a VM with assigned devices, use `VFIO_DMA_MAP_FLAG_DIRTY` and `VFIO_IOMMU_DIRTY_PAGES` to track modified pages. This is not needed for a static passthrough VMM.

## 9. Setting Up IRQs

A VFIO device can generate interrupts. You must tell the kernel which IRQs to use, then route them into the VM. The typical flow is:

1. Read the device's `VFIO_IRQ_INFO`.
2. For MSI-X or MSI, use `VFIO_DEVICE_SET_IRQS` with `VFIO_IRQ_SET_DATA_EVENTFD` and `VFIO_IRQ_SET_ACTION_TRIGGER`.
3. Route each eventfd to a guest IRQ (or MSI vector) using `KVM_IRQFD`.

### Example: register an eventfd for an INTx pin

```c
struct vfio_irq_set *irq_set;
struct vfio_irq_info irq_info = { .argsz = sizeof(irq_info) };
irq_info.index = VFIO_PCI_INTX_IRQ_INDEX;

ioctl(device, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);

if (!(irq_info.flags & VFIO_IRQ_INFO_EVENTFD))
    errx(1, "INTx does not support eventfd");

int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

size_t size = sizeof(struct vfio_irq_set) + sizeof(int);
irq_set = alloca(size);
irq_set->argsz = size;
irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
irq_set->start = 0;
irq_set->count = 1;
*(int *)irq_set->data = efd;

ioctl(device, VFIO_DEVICE_SET_IRQS, irq_set);

/* Now any device INTx is reported on efd. */
```

### Injecting the interrupt into the VM

For INTx, use `KVM_IRQFD` to route `efd` to a guest GSI. The VMM creates a `KVM_IRQFD` struct:

```c
int kvm_irqfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

struct kvm_irqfd kifd = {
    .fd = kvm_irqfd,
    .flags = 0,
    .gsi  = guest_irq_number,
    .resamplefd = -1,
};

ioctl(vm_fd, KVM_IRQFD, &kifd);
```

Then a thread that waits on `efd` and writes `1` to `kvm_irqfd` when an interrupt occurs:

```c
uint64_t x;
read(efd, &x, sizeof(x));
write(kvm_irqfd, &x, sizeof(x));
```

For MSI/MSI-X the vector is delivered directly by the IOMMU; the routing is more involved and usually uses `KVM_SIGNAL_MSI` or the VFIO `KVM_DEV_VFIO` interface. This is beyond the minimal VFIO example.

## 10. Reset and Teardown

Before closing, reset the device and unmap DMA:

```c
ioctl(device, VFIO_DEVICE_RESET, 0);

/* unmap DMA */
struct vfio_iommu_type1_dma_unmap unmap = {
    .argsz = sizeof(unmap),
    .flags = 0,
    .iova  = 0,
    .size  = GUEST_RAM_SIZE,
};
ioctl(container, VFIO_IOMMU_UNMAP_DMA, &unmap);

close(device);
close(group);
close(container);
```

## 11. Full VFIO Setup Skeleton

```c
void vfio_setup_passthrough(const char *bdf, int vm_fd, void *guest_mem,
                            size_t guest_mem_size)
{
    int container = open("/dev/vfio/vfio", O_RDWR);
    ioctl(container, VFIO_GET_API_VERSION, 0);
    ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU);
    ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

    int group_id = get_group_id(bdf);  /* read /sys/bus/pci/devices/.../iommu_group */
    char path[64];
    snprintf(path, sizeof(path), "/dev/vfio/%d", group_id);
    int group = open(path, O_RDWR);

    struct vfio_group_status gs = { .argsz = sizeof(gs) };
    ioctl(group, VFIO_GROUP_GET_STATUS, &gs);
    if (!(gs.flags & VFIO_GROUP_FLAGS_VIABLE))
        errx(1, "group not viable");

    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);

    int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, bdf);

    struct vfio_device_info dinfo = { .argsz = sizeof(dinfo) };
    ioctl(device, VFIO_DEVICE_GET_INFO, &dinfo);

    /* Map guest RAM into the IOMMU. */
    struct vfio_iommu_type1_dma_map dma = {
        .argsz = sizeof(dma),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = (uint64_t)(uintptr_t)guest_mem,
        .iova  = 0,
        .size  = guest_mem_size,
    };
    ioctl(container, VFIO_IOMMU_MAP_DMA, &dma);

    /* Map BARs and config space into the VMM. */
    for (int i = 0; i < dinfo.num_regions; i++) {
        struct vfio_region_info reg = { .argsz = sizeof(reg) };
        reg.index = i;
        if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &reg) < 0)
            continue;
        void *map = mmap(NULL, reg.size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, device, reg.offset);
        (void)map;  /* store for later */
    }

    /* Configure and route an INTx interrupt. */
    int efd = eventfd(0, EFD_NONBLOCK);
    struct vfio_irq_set *irq_set = alloca(sizeof(*irq_set) + sizeof(int));
    irq_set->argsz = sizeof(*irq_set) + sizeof(int);
    irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
    irq_set->start = 0;
    irq_set->count = 1;
    *(int *)irq_set->data = efd;
    ioctl(device, VFIO_DEVICE_SET_IRQS, irq_set);

    int kvm_ee = eventfd(0, EFD_NONBLOCK);
    struct kvm_irqfd kifd = { .fd = kvm_ee, .flags = 0, .gsi = 10,
                              .resamplefd = -1 };
    ioctl(vm_fd, KVM_IRQFD, &kifd);

    /* Spawn a thread that waits on efd and writes to kvm_ee. */
    /* ... */
}
```

## 12. Summary

| Step | Ioctl / action |
|------|----------------|
| 1 | Bind device to `vfio-pci` on the host. |
| 2 | Open `/dev/vfio/vfio`, check `VFIO_TYPE1_IOMMU`. |
| 3 | Open the IOMMU group (`/dev/vfio/N`). |
| 4 | `VFIO_GROUP_SET_CONTAINER`. |
| 5 | `VFIO_SET_IOMMU`. |
| 6 | `VFIO_GROUP_GET_DEVICE_FD` for the PCI BDF. |
| 7 | `VFIO_DEVICE_GET_INFO`, `VFIO_DEVICE_GET_REGION_INFO`. |
| 8 | `mmap` config space and BARs. |
| 9 | `VFIO_IOMMU_MAP_DMA` for guest RAM. |
| 10 | `VFIO_DEVICE_SET_IRQS` with eventfds. |
| 11 | `KVM_IRQFD` to route device interrupts into the VM. |
| 12 | `VFIO_DEVICE_RESET` on shutdown. |

VFIO passthrough is the most advanced I/O path in a VMM. When set up correctly, the guest directly drives the hardware with no host userspace in the hot path for data, while the IOMMU keeps memory safe.
