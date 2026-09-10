/*
 * Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL.
 * Unauthorized copying, reproduction, distribution, or modification of this
 * file, via any medium, is strictly prohibited.
 * All rights reserved.
 */

/*
 * vmm.c
 *
 * Main VMM file.  Opens KVM, creates the VM, maps guest memory, creates and
 * starts multiple vCPUs in threads, and runs the main KVM execution loop.
 * It also demonstrates per-step logging and optional debug-register support.
 */

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <asm/kvm.h>      /* x86 specific constants (CR0_PE, etc.) */
#include <linux/kvm.h>    /* main KVM API */

#include "vmm.h"
#include "kernel.h"
#include "acpi.h"
#include "bios.h"

/* Global debug flag used by the DBG() macro in vmm.h. */
int g_debug = 0;

/* ---- utility helpers ------------------------------------------------------ */

/*
 * Print usage and exit.  Keep the list in sync with main()'s getopt string.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -k <bzImage> [-i <initrd>] [-D <disk>] [-m <MB>] "
        "[-s <vcpus>] [-c <cmdline>] [-d]\n"
        "  -k <path>   bzImage kernel (required)\n"
        "  -i <path>   initrd (optional)\n"
        "  -D <path>   flat disk image (optional)\n"
        "  -m <MB>     guest RAM in megabytes (default %d)\n"
        "  -s <N>      number of vCPUs (default %d)\n"
        "  -c <cmd>    kernel command line\n"
        "  -d          extra debug logging\n"
        "  -h          show this help\n",
        prog, GUEST_MEM_DEFAULT_MB, GUEST_NR_VCPUS_DEFAULT);
    exit(1);
}

/* ---- VMM lifecycle -------------------------------------------------------- */

struct vmm *vmm_create(size_t mem_size, int nr_vcpus)
{
    struct vmm *vmm = calloc(1, sizeof(*vmm));
    if (!vmm) {
        LOG("failed to allocate vmm struct");
        return NULL;
    }

    vmm->mem_size = mem_size;
    vmm->nr_vcpus = nr_vcpus;
    vmm->uart_lcr = 0x03;  /* 8N1, DLAB=0, like vmtainer                 */
    vmm->uart_lsr = 0x60;  /* THRE + TEMT: the UART is always empty.   */
    vmm->uart_msr = 0xf0;  /* DCD/RI/DSR/CTS all asserted               */
    vmm->uart_dll = 0x01;  /* divisor latch low byte, 115200 baud       */

    /* Open /dev/kvm.  This is the first step in any KVM-based VMM. */
    LOG("opening /dev/kvm");
    vmm->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (vmm->kvm_fd < 0) {
        LOG("cannot open /dev/kvm: %s", strerror(errno));
        free(vmm);
        return NULL;
    }

    /* KVM uses an API version; confirm we are compatible. */
    int api = ioctl(vmm->kvm_fd, KVM_GET_API_VERSION, 0);
    if (api < 0) {
        LOG("KVM_GET_API_VERSION failed: %s", strerror(errno));
        close(vmm->kvm_fd);
        free(vmm);
        return NULL;
    }
    LOG("KVM API version: %d", api);
    if (api != 12) {
        LOG("unsupported KVM API version %d (expected 12)", api);
        close(vmm->kvm_fd);
        free(vmm);
        return NULL;
    }

    /*
     * User-backed guest memory is a hard requirement for this example.
     * It lets us fill the allocation from userspace and then hand it to KVM.
     */
    int have = ioctl(vmm->kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
    LOG("KVM_CAP_USER_MEMORY: %d", have);
    if (!have) {
        LOG("KVM does not support user memory");
        close(vmm->kvm_fd);
        free(vmm);
        return NULL;
    }

    /* Create the VM file descriptor. */
    vmm->vm_fd = ioctl(vmm->kvm_fd, KVM_CREATE_VM, 0);
    if (vmm->vm_fd < 0) {
        LOG("KVM_CREATE_VM failed: %s", strerror(errno));
        close(vmm->kvm_fd);
        free(vmm);
        return NULL;
    }

    /*
     * Create the in-kernel 8259/I/O-APIC/LAPIC and a dummy PIT.  These
     * are expected by the bzImage real-mode setup and by the kernel later.
     */
    if (ioctl(vmm->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
        LOG("KVM_CREATE_IRQCHIP failed: %s", strerror(errno));
        close(vmm->vm_fd);
        close(vmm->kvm_fd);
        free(vmm);
        return NULL;
    }
    LOG("created in-kernel IRQ chip");

    struct kvm_pit_config pit = { .flags = KVM_PIT_SPEAKER_DUMMY };
    if (ioctl(vmm->vm_fd, KVM_CREATE_PIT2, &pit) < 0) {
        LOG("KVM_CREATE_PIT2 failed: %s", strerror(errno));
        close(vmm->vm_fd);
        close(vmm->kvm_fd);
        free(vmm);
        return NULL;
    }
    LOG("created in-kernel PIT");

    vmm->vcpus = calloc(nr_vcpus, sizeof(*vmm->vcpus));
    if (!vmm->vcpus) {
        LOG("failed to allocate vCPU array");
        close(vmm->vm_fd);
        close(vmm->kvm_fd);
        free(vmm);
        return NULL;
    }

    pthread_mutex_init(&vmm->vio_mutex, NULL);

    return vmm;
}

void vmm_destroy(struct vmm *vmm)
{
    if (!vmm)
        return;

    /* Tear down all vCPUs first. */
    for (int i = 0; i < vmm->nr_vcpus; i++) {
        if (vmm->vcpus[i])
            vcpu_destroy(vmm->vcpus[i]);
    }
    free(vmm->vcpus);

    /* Unmap the guest RAM before closing the VM. */
    if (vmm->mem) {
        LOG("unmapping guest memory");
        munmap(vmm->mem, vmm->mem_size);
    }

    pthread_mutex_destroy(&vmm->vio_mutex);

    if (vmm->disk_fd >= 0) {
        LOG("closing disk image");
        close(vmm->disk_fd);
    }

    if (vmm->vm_fd >= 0)
        close(vmm->vm_fd);
    if (vmm->kvm_fd >= 0)
        close(vmm->kvm_fd);
    free(vmm);
}

/*
 * Allocate a single contiguous host buffer and tell KVM to use it as the
 * guest physical address space [0, mem_size).
 */
int vmm_map_memory(struct vmm *vmm)
{
    LOG("mapping %zu bytes of guest memory at GPA 0", vmm->mem_size);

    vmm->mem = mmap(NULL, vmm->mem_size,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vmm->mem == MAP_FAILED) {
        LOG("guest memory mmap failed: %s", strerror(errno));
        return -1;
    }

    /*
     * struct kvm_userspace_memory_region is the key data structure for
     * memory slots.  slot 0 starts at guest physical 0.
     */
    struct kvm_userspace_memory_region region = {
        .slot           = 0,
        .flags          = 0,
        .guest_phys_addr = 0,
        .memory_size    = vmm->mem_size,
        .userspace_addr = (uint64_t)(unsigned long)vmm->mem,
    };

    if (ioctl(vmm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        LOG("KVM_SET_USER_MEMORY_REGION failed: %s", strerror(errno));
        munmap(vmm->mem, vmm->mem_size);
        vmm->mem = NULL;
        return -1;
    }

    LOG("guest memory mapped at host %p", (void *)vmm->mem);
    return 0;
}

/*
 * Load a flat disk image for the simple IDE controller.  The image is used
 * in 512-byte LBA sectors.  It is fine if the file is not writable; write
 * requests will simply fail silently.
 */
int vmm_load_disk(struct vmm *vmm, const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            LOG("cannot open disk image %s: %s", path, strerror(errno));
            return -1;
        }
        LOG("disk image opened read-only");
    }

    off64_t size = lseek64(fd, 0, SEEK_END);
    if (size < 0) {
        LOG("cannot seek disk image: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (size < 512) {
        LOG("disk image is too small (%lld bytes)", (long long)size);
        close(fd);
        return -1;
    }

    vmm->disk_fd = fd;
    vmm->disk_size = (uint64_t)size;
    vmm->disk_num_sectors = (uint32_t)(size / 512);
    vmm->ide_status = 0x50;    /* RDY + DSC */

    memset(vmm->vio_config, 0, sizeof(vmm->vio_config));
    uint64_t cap = vmm->disk_num_sectors;
    uint32_t seg_max = 32;
    uint32_t blk_size = 512;
    memcpy(vmm->vio_config + 0x00, &cap, sizeof(cap));      /* capacity   */
    memcpy(vmm->vio_config + 0x0c, &seg_max, sizeof(seg_max)); /* seg_max */
    memcpy(vmm->vio_config + 0x14, &blk_size, sizeof(blk_size)); /* blk_size */

    LOG("loaded disk image %s (0x%llx bytes, %u sectors)",
        path, (unsigned long long)size, vmm->disk_num_sectors);
    return 0;
}

/* ---- vCPU lifecycle ------------------------------------------------------- */

struct vcpu *vcpu_create(struct vmm *vmm, int vcpu_id)
{
    struct vcpu *vcpu = calloc(1, sizeof(*vcpu));
    if (!vcpu) {
        LOG("failed to allocate vCPU %d", vcpu_id);
        return NULL;
    }

    vcpu->vmm = vmm;
    vcpu->id = vcpu_id;
    vcpu->is_bsp = (vcpu_id == 0);

    /* Create the vCPU.  The second argument is the vCPU id. */
    vcpu->fd = ioctl(vmm->vm_fd, KVM_CREATE_VCPU, (unsigned long)vcpu_id);
    if (vcpu->fd < 0) {
        LOG("KVM_CREATE_VCPU %d failed: %s", vcpu_id, strerror(errno));
        free(vcpu);
        return NULL;
    }

    /*
     * The kvm_run structure is mmap'd from the vCPU fd.  It is where the
     * kernel reports why a vCPU exited.
     */
    vcpu->mmap_size = ioctl(vmm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if ((int)vcpu->mmap_size < 0) {
        LOG("KVM_GET_VCPU_MMAP_SIZE failed: %s", strerror(errno));
        close(vcpu->fd);
        free(vcpu);
        return NULL;
    }

    vcpu->run = mmap(NULL, vcpu->mmap_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, vcpu->fd, 0);
    if (vcpu->run == MAP_FAILED) {
        LOG("vCPU %d mmap failed: %s", vcpu_id, strerror(errno));
        close(vcpu->fd);
        free(vcpu);
        return NULL;
    }

    LOG("created vCPU %d (BSP=%d)", vcpu->id, vcpu->is_bsp);
    return vcpu;
}

void vcpu_destroy(struct vcpu *vcpu)
{
    if (!vcpu)
        return;
    if (vcpu->run && vcpu->run != MAP_FAILED)
        munmap(vcpu->run, vcpu->mmap_size);
    if (vcpu->fd >= 0)
        close(vcpu->fd);
    free(vcpu);
}

/*
 * Set the supported CPUID leaves for this vCPU.  Linux kernels inspect
 * CPUID early to choose 32/64-bit and feature paths.
 */
static int vcpu_set_cpuid(struct vcpu *vcpu)
{
    int nent = 256;
    struct kvm_cpuid2 *cpuid = calloc(1, sizeof(*cpuid) +
                                       nent * sizeof(cpuid->entries[0]));
    if (!cpuid) {
        LOG("vCPU %d cannot allocate CPUID buffer", vcpu->id);
        return -1;
    }

    cpuid->nent = nent;
    if (ioctl(vcpu->vmm->kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0) {
        LOG("vCPU %d KVM_GET_SUPPORTED_CPUID failed: %s",
            vcpu->id, strerror(errno));
        free(cpuid);
        return -1;
    }

    /*
     * Fix the initial APIC ID in CPUID leaf 0x1 so it matches the MADT
     * / MP table.  Disable x2APIC too, because we only provide 8-bit
     * local APIC IDs in the MADT.
     */
    for (uint32_t i = 0; i < cpuid->nent; i++) {
        struct kvm_cpuid_entry2 *e = &cpuid->entries[i];
        if (e->function == 1) {
            e->ebx &= 0x00ffffff;
            e->ebx |= (uint32_t)(vcpu->id & 0xff) << 24;
            e->ecx &= ~(1u << 21);            /* x2APIC */
            e->edx |= (1u << 9);              /* APIC */
        }
    }

    if (ioctl(vcpu->fd, KVM_SET_CPUID2, cpuid) < 0) {
        LOG("vCPU %d KVM_SET_CPUID2 failed: %s", vcpu->id, strerror(errno));
        free(cpuid);
        return -1;
    }

    free(cpuid);
    return 0;
}

/*
 * Set the initial 16-bit real-mode register state.
 * vCPU0 (BSP) starts at the bzImage setup entry 0x1000:0x0200, with
 * ds:si -> boot_params at 0x10000.  APs are parked on the halt stub
 * at 0xF000:0x0000.  This matches how a real BIOS jumps to the loader.
 */
int vcpu_init_regs(struct vcpu *vcpu)
{
    struct kvm_sregs sregs;
    struct kvm_regs  regs;

    /*
     * Start from the vCPU's own default register state, then override
     * the fields we care about.  This is more robust than zero-filling.
     */
    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0) {
        LOG("vCPU %d KVM_GET_SREGS failed: %s", vcpu->id, strerror(errno));
        return -1;
    }
    if (ioctl(vcpu->fd, KVM_GET_REGS, &regs) < 0) {
        LOG("vCPU %d KVM_GET_REGS failed: %s", vcpu->id, strerror(errno));
        return -1;
    }

    /*
     * Segment descriptor layout for real mode: base = selector * 16,
     * limit = 0xFFFF, G = 0, D/B = 0, L = 0, P = 1, S = 1, DPL = 0.
     * Type: 0x0B for a code segment, 0x03 for a data segment.
     */
    #define SET_SEG(sp, sel, base_addr, typ) do {                \
        (*sp).selector = (sel);                              \
        (*sp).base     = (base_addr);                        \
        (*sp).limit    = 0xFFFF;                             \
        (*sp).type     = (typ);                              \
        (*sp).s        = 1;                                  \
        (*sp).dpl      = 0;                                  \
        (*sp).present  = 1;                                  \
        (*sp).avl      = 0;                                  \
        (*sp).l        = 0;                                  \
        (*sp).db       = 0;                                  \
        (*sp).g        = 0;                                  \
        (*sp).unusable = 0;                                  \
    } while (0)

    /*
     * The BSP starts in the bzImage setup segment.  The real-mode setup
     * code is copied to 0x10000 and its first instruction is at 0x200.
     * APs are parked in the BIOS hlt stub at 0xF000:0xFFF0 until a SIPI.
     */
    if (vcpu->is_bsp)
        SET_SEG(&sregs.cs, 0x1000, 0x10000, 0x0B);
    else
        SET_SEG(&sregs.cs, 0xF000, 0xF0000, 0x0B);

    /*
     * ds:si must point to the boot_params / setup_header at 0x10000.
     * ss:sp is a small stack in the same 0x1000 segment (just above setup).
     * es is also the setup segment because the kernel uses it at boot.
     */
    SET_SEG(&sregs.ds, 0x1000, 0x10000, 0x03);
    sregs.es = sregs.ds;
    sregs.fs = sregs.gs = sregs.ds;
    if (vcpu->is_bsp) {
        SET_SEG(&sregs.ss, 0x1000, 0x10000, 0x03);
    } else {
        /* APs keep a flat real-mode environment for the SIPI startup. */
        SET_SEG(&sregs.ss, 0x0000, 0x00000, 0x03);
        SET_SEG(&sregs.ds, 0x0000, 0x00000, 0x03);
        sregs.es = sregs.ds;
        sregs.fs = sregs.gs = sregs.ds;
    }

    /*
     * vmm.cpp runs the data segments with D/B=1, so do the same here.
     * The task and ldt descriptors are left exactly as KVM_GET_SREGS
     * returned them; marking them unusable causes VMX to reject the vCPU.
     */
    sregs.ds.db = sregs.es.db = sregs.fs.db = 1;
    sregs.gs.db = sregs.ss.db = 1;

    /*
     * Real mode still requires a valid descriptor table shape.  tr/ldt are
     * already correct from the KVM_GET_SREGS() call above.
     */
    sregs.idt.limit = 0xffff;
    sregs.idt.base  = 0;
    sregs.gdt.limit = 0xffff;
    sregs.gdt.base  = 0;

    /*
     * Start in real mode with all protection bits off, but set CR0.ET
     * (bit 4) so the host does not consider the state invalid.
     */
    sregs.cr0   = 0x00000010;
    sregs.cr3   = 0;
    sregs.cr4   = 0;
    sregs.efer  = 0;

    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0) {
        LOG("vCPU %d KVM_SET_SREGS failed: %s", vcpu->id, strerror(errno));
        return -1;
    }

    /*
     * rflags bit 1 is the reserved "1" bit.  %eip is the bzImage setup
     * entry (0x0200) for the BSP or the AP reset vector (0xFFF0).  %esi
     * is the offset within the setup segment, and %esp is the top of
     * the conventional stack (1:1 into the 0x10000 segment).
     */
    regs.rflags = 0x2;
    regs.rip    = vcpu->is_bsp ? 0x0200 : 0xFFF0;
    regs.rsi    = 0x0000;
    regs.rsp    = vcpu->is_bsp ? 0x8000 : 0xF000;

    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0) {
        LOG("vCPU %d KVM_SET_REGS failed: %s", vcpu->id, strerror(errno));
        return -1;
    }

    /*
     * The BSP runs immediately; APs are parked waiting for a SIPI so the
     * kernel can bring them up via the local APIC.
     */
    struct kvm_mp_state mp = {
        .mp_state = vcpu->is_bsp ? KVM_MP_STATE_RUNNABLE
                                  : KVM_MP_STATE_INIT_RECEIVED
    };
    if (ioctl(vcpu->fd, KVM_SET_MP_STATE, &mp) < 0) {
        LOG("vCPU %d KVM_SET_MP_STATE failed: %s", vcpu->id, strerror(errno));
        return -1;
    }

    /*
     * Set the supported CPUID leaves for this vCPU, matching the host's
     * advertised capabilities.  The kernel inspects these early.
     */
    if (vcpu_set_cpuid(vcpu) < 0)
        return -1;

    DBG("vCPU %d init rip=0x%04x rsp=0x%04x", vcpu->id,
        (uint16_t)regs.rip, (uint16_t)regs.rsp);
    return 0;
}

/*
 * Demonstrate KVM_SET_DEBUGREGS / KVM_GET_DEBUGREGS.  When the -d flag is
 * used we read the current guest debug registers, print them, and then set
 * them to safe zeroed values.  To actually break on an address one would also
 * need KVM_SET_GUEST_DEBUG; this function shows the register API only.
 */
int vcpu_set_debug_regs(struct vcpu *vcpu)
{
    struct kvm_debugregs dr;
    int r;

    r = ioctl(vcpu->fd, KVM_GET_DEBUGREGS, &dr);
    if (r < 0) {
        LOG("vCPU %d KVM_GET_DEBUGREGS failed: %s", vcpu->id, strerror(errno));
        return -1;
    }

    DBG("vCPU %d current DR0=0x%016llx DR6=0x%016llx DR7=0x%016llx",
        vcpu->id,
        (unsigned long long)dr.db[0], (unsigned long long)dr.dr6, (unsigned long long)dr.dr7);

    memset(&dr, 0, sizeof(dr));

    /*
     * With debug mode on, place a hardware breakpoint at the kernel setup
     * entry (0x10000).  We do not enable it in DR7, so it is harmless.
     * This shows how the VMM would prepare guest debug registers.
     */
    if (g_debug) {
        dr.db[0] = 0x10000;
        dr.dr6 = 0xFFFF0FF0ULL;  /* x86 architectural reset value       */
        dr.dr7 = 0x00000000;      /* disabled; set bit 0 to enable db[0] */
    }

    r = ioctl(vcpu->fd, KVM_SET_DEBUGREGS, &dr);
    if (r < 0) {
        LOG("vCPU %d KVM_SET_DEBUGREGS failed: %s", vcpu->id, strerror(errno));
        return -1;
    }

    return 0;
}

/* ---- IDE helper functions ------------------------------------------------- */

#include <fcntl.h>

static uint32_t ide_lba(struct vmm *vmm)
{
    return ((uint32_t)(vmm->ide_drive_head & 0x0f) << 24) |
           ((uint32_t)vmm->ide_lba_high << 16) |
           ((uint32_t)vmm->ide_lba_mid  <<  8) |
           (uint32_t)vmm->ide_lba_low;
}

static void ide_load_sector(struct vmm *vmm, uint32_t lba)
{
    off64_t off = (off64_t)lba * 512;
    if (off + 512 > (off64_t)vmm->disk_size || vmm->disk_fd < 0) {
        memset(vmm->ide_data, 0, sizeof(vmm->ide_data));
        return;
    }
    ssize_t n = pread64(vmm->disk_fd, vmm->ide_data, 512, off);
    if (n < 0)
        n = 0;
    if (n < 512)
        memset((uint8_t *)vmm->ide_data + n, 0, 512 - (size_t)n);
    vmm->ide_data_pos = 0;
    vmm->ide_data_len = 256;
}

static void ide_store_sector(struct vmm *vmm, uint32_t lba)
{
    if (vmm->disk_fd < 0)
        return;
    off64_t off = (off64_t)lba * 512;
    if (off + 512 > (off64_t)vmm->disk_size)
        return;
    (void)pwrite64(vmm->disk_fd, vmm->ide_data, 512, off);
}

static void ide_identify(struct vmm *vmm)
{
    memset(vmm->ide_data, 0, sizeof(vmm->ide_data));

    /* Word 0: non-removable, ATA, 0x0040 */
    vmm->ide_data[0]  = 0x0040;
    /* Word 49: LBA supported */
    vmm->ide_data[49] = 0x0200;
    /* Words 60-61: total number of 28-bit LBA sectors */
    vmm->ide_data[60] = (uint16_t)(vmm->disk_num_sectors & 0xffff);
    vmm->ide_data[61] = (uint16_t)(vmm->disk_num_sectors >> 16);

    vmm->ide_data_pos = 0;
    vmm->ide_data_len = 256;
    vmm->ide_status   = 0x58;  /* RDY + DSC + DRQ */
    vmm->ide_error    = 0x00;
}

static void ide_command(struct vmm *vmm, uint8_t cmd)
{
    switch (cmd) {
    case 0x20: {  /* READ SECTORS */
        uint32_t lba = ide_lba(vmm);
        if (vmm->ide_nsect == 0)
            vmm->ide_nsect = 1; /* 0 means 256 sectors in ATA, but start with 1 */
        vmm->ide_lba = lba;
        ide_load_sector(vmm, vmm->ide_lba);
        vmm->ide_status = 0x58;  /* RDY + DSC + DRQ */
        vmm->ide_error  = 0x00;
        break;
    }
    case 0x30: {  /* WRITE SECTORS */
        uint32_t lba = ide_lba(vmm);
        if (vmm->ide_nsect == 0)
            vmm->ide_nsect = 1;
        vmm->ide_lba = lba;
        vmm->ide_data_pos = 0;
        vmm->ide_data_len = 256;
        vmm->ide_status   = 0x58;  /* wait for data from host */
        vmm->ide_error    = 0x00;
        break;
    }
    case 0xec:    /* IDENTIFY DRIVE */
        ide_identify(vmm);
        break;
    case 0x08:    /* DEVICE RESET */
    case 0xef:    /* SET FEATURES */
    case 0x00:    /* NOP */
    case 0x70:    /* SEEK */
        vmm->ide_status = 0x50;
        vmm->ide_error  = 0x00;
        break;
    default:
        vmm->ide_status = 0x51;  /* RDY + ERR */
        vmm->ide_error  = 0x04;  /* ABORT */
        break;
    }
}

static uint16_t ide_data_in(struct vmm *vmm)
{
    if (vmm->ide_data_pos >= vmm->ide_data_len) {
        if (vmm->ide_nsect > 0) {
            vmm->ide_nsect--;
            if (vmm->ide_nsect > 0) {
                vmm->ide_lba++;
                ide_load_sector(vmm, vmm->ide_lba);
            } else {
                vmm->ide_status = 0x50;
                vmm->ide_data_len = 0;
                return 0;
            }
        } else {
            return 0;
        }
    }
    return vmm->ide_data[vmm->ide_data_pos++];
}

static void ide_data_out(struct vmm *vmm, uint16_t data)
{
    if (vmm->ide_data_pos < 256)
        vmm->ide_data[vmm->ide_data_pos++] = data;

    if (vmm->ide_data_pos == 256) {
        if (vmm->ide_nsect > 0) {
            ide_store_sector(vmm, vmm->ide_lba);
            vmm->ide_nsect--;
            if (vmm->ide_nsect > 0) {
                vmm->ide_lba++;
                vmm->ide_data_pos = 0;
                vmm->ide_data_len = 256;
                vmm->ide_status   = 0x58;
            } else {
                vmm->ide_status = 0x50;
                vmm->ide_data_len = 0;
            }
        } else {
            vmm->ide_status = 0x50;
            vmm->ide_data_len = 0;
        }
    }
}

static void ide_in(struct vcpu *vcpu, uint16_t port, uint8_t *data, unsigned size)
{
    struct vmm *vmm = vcpu->vmm;
    uint16_t val;

    if (port == 0x1f0 && size == 2) {
        val = ide_data_in(vmm);
        data[0] = (uint8_t)(val & 0xff);
        data[1] = (uint8_t)(val >> 8);
        return;
    }

    switch (port) {
    case 0x1f1: *data = vmm->ide_error;  break;
    case 0x1f2: *data = vmm->ide_nsect;  break;
    case 0x1f3: *data = vmm->ide_lba_low;  break;
    case 0x1f4: *data = vmm->ide_lba_mid;  break;
    case 0x1f5: *data = vmm->ide_lba_high; break;
    case 0x1f6: *data = vmm->ide_drive_head; break;
    case 0x1f7: *data = vmm->ide_status; break;
    case 0x3f6: *data = vmm->ide_status; break;  /* alternate status */
    default:    *data = 0xff; break;
    }
}

static void ide_out(struct vcpu *vcpu, uint16_t port, uint8_t *data, unsigned size)
{
    struct vmm *vmm = vcpu->vmm;
    uint16_t val;

    if (port == 0x1f0 && size == 2) {
        val = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        ide_data_out(vmm, val);
        return;
    }

    switch (port) {
    case 0x1f1: /* feature register, ignored */ break;
    case 0x1f2: vmm->ide_nsect = data[0];       break;
    case 0x1f3: vmm->ide_lba_low = data[0];     break;
    case 0x1f4: vmm->ide_lba_mid = data[0];     break;
    case 0x1f5: vmm->ide_lba_high = data[0];    break;
    case 0x1f6: vmm->ide_drive_head = data[0];  break;
    case 0x1f7: ide_command(vmm, data[0]);      break;
    case 0x3f6: vmm->ide_control = data[0];     break;
    default: break;
    }
}

/* ---- VM exit handlers ----------------------------------------------------- */

/*
 * Minimal 16550A-style COM1 (0x3F8-0x3FF) emulation.  This is enough for
 * the Linux early serial console and for a ttyS0 boot console.
 */
static void serial_in(struct vcpu *vcpu, uint16_t port, uint8_t *data)
{
    struct vmm *vmm = vcpu->vmm;
    bool dlab = vmm->uart_lcr & 0x80;

    switch (port) {
    case 0x3f8: *data = dlab ? vmm->uart_dll : 0;            break;
    case 0x3f9: *data = dlab ? vmm->uart_dlh : vmm->uart_ier; break;
    case 0x3fa: *data = (vmm->uart_ier & 0x02) ? 0x02 : 0x01; break;
    case 0x3fb: *data = vmm->uart_lcr;                        break;
    case 0x3fc: *data = vmm->uart_mcr;                        break;
    case 0x3fd: *data = vmm->uart_lsr;                        break;
    case 0x3fe: *data = vmm->uart_msr;                        break;
    case 0x3ff: *data = vmm->uart_scr;                        break;
    default:    *data = 0;                                    break;
    }
}

static void serial_out(struct vcpu *vcpu, uint16_t port, uint8_t data)
{
    struct vmm *vmm = vcpu->vmm;
    bool dlab = vmm->uart_lcr & 0x80;

    switch (port) {
    case 0x3f8:
        if (dlab) {
            vmm->uart_dll = data;
        } else {
            putchar(data);
            fflush(stdout);
            /* Generate THRE interrupt if the driver asked for it. */
            if (vmm->uart_ier & 0x02) {
                struct kvm_irq_level irq = { .irq = 4, .level = 1 };
                ioctl(vmm->vm_fd, KVM_IRQ_LINE, &irq);
            }
        }
        break;
    case 0x3f9:
        if (dlab) vmm->uart_dlh = data;
        else      vmm->uart_ier = data;
        break;
    case 0x3fa: vmm->uart_fcr = data; break;
    case 0x3fb: vmm->uart_lcr = data; break;
    case 0x3fc: vmm->uart_mcr = data; break;
    case 0x3ff: vmm->uart_scr = data; break;
    default: break;
    }
}

void vmm_handle_io(struct vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    uint8_t *data = (uint8_t *)run + run->io.data_offset;

    if (run->io.direction == KVM_EXIT_IO_OUT) {
        for (unsigned i = 0; i < run->io.count; i++) {
            uint8_t *p = data + i * run->io.size;
            if (run->io.port >= 0x3f8 && run->io.port <= 0x3ff) {
                for (unsigned j = 0; j < run->io.size; j++)
                    serial_out(vcpu, run->io.port, p[j]);
            } else if (run->io.port == 0x1f0 ||
                       (run->io.port >= 0x1f1 && run->io.port <= 0x1f7) ||
                       run->io.port == 0x3f6) {
                ide_out(vcpu, run->io.port, p, run->io.size);
            } else if (run->io.port == 0xE9) {
                /* Bochs e9 port */
                for (unsigned j = 0; j < run->io.size; j++)
                    fputc(p[j], stderr);
                fflush(stderr);
            } else if (run->io.port == 0x80) {
                DBG("vCPU %d POST 0x%02x", vcpu->id, p[0]);
            } else {
                DBG("vCPU %d IO OUT port=0x%04x size=%u data=0x%02x",
                    vcpu->id, run->io.port, run->io.size, p[0]);
            }
        }
    } else if (run->io.direction == KVM_EXIT_IO_IN) {
        for (unsigned i = 0; i < run->io.count; i++) {
            uint8_t *p = data + i * run->io.size;
            if (run->io.port >= 0x3f8 && run->io.port <= 0x3ff) {
                for (unsigned j = 0; j < run->io.size; j++)
                    serial_in(vcpu, run->io.port, &p[j]);
            } else if (run->io.port == 0x1f0 ||
                       (run->io.port >= 0x1f1 && run->io.port <= 0x1f7) ||
                       run->io.port == 0x3f6) {
                ide_in(vcpu, run->io.port, p, run->io.size);
            } else {
                for (unsigned j = 0; j < run->io.size; j++)
                    p[j] = 0x00;
            }
        }
        DBG("vCPU %d IO IN  port=0x%04x size=%u count=%u",
            vcpu->id, run->io.port, run->io.size, run->io.count);
    }
}

/* ---- Virtio-MMIO block device --------------------------------------------- */

#define VIO_MMIO_BASE  0xa0000000ULL
#define VIO_MMIO_SIZE  0x200
#define VIO_MMIO_IRQ   5

/* Virtio 1.0-ish (legacy transport) register offsets. */
#define VIO_REG_MAGIC               0x000
#define VIO_REG_VERSION             0x004
#define VIO_REG_DEVICE_ID           0x008
#define VIO_REG_VENDOR_ID           0x00c
#define VIO_REG_HOST_FEATURES       0x010
#define VIO_REG_HOST_FEATURES_SEL   0x014
#define VIO_REG_GUEST_FEATURES      0x020
#define VIO_REG_GUEST_FEATURES_SEL  0x024
#define VIO_REG_GUEST_PAGE_SIZE     0x028
#define VIO_REG_QUEUE_SEL           0x030
#define VIO_REG_QUEUE_NUM_MAX       0x034
#define VIO_REG_QUEUE_NUM           0x038
#define VIO_REG_QUEUE_ALIGN         0x03c
#define VIO_REG_QUEUE_PFN           0x040
#define VIO_REG_QUEUE_NOTIFY        0x050
#define VIO_REG_INTERRUPT_STATUS    0x060
#define VIO_REG_INTERRUPT_ACK       0x064
#define VIO_REG_STATUS              0x070
#define VIO_REG_CONFIG              0x100

#define VIO_BLK_T_IN    0
#define VIO_BLK_T_OUT   1
#define VIO_BLK_T_FLUSH 4

#define VIO_STATUS_ACK          1
#define VIO_STATUS_DRIVER       2
#define VIO_STATUS_DRIVER_OK    4
#define VIO_STATUS_FEATURES_OK  8

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

struct virtio_blk_req {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
} __attribute__((packed));

static uint64_t vio_guest_to_host(struct vmm *vmm, uint64_t gpa)
{
    if (gpa >= vmm->mem_size)
        return 0;
    return (uint64_t)(uintptr_t)(vmm->mem + gpa);
}

static void vio_inject_irq(struct vmm *vmm)
{
    struct kvm_irq_level irq = { .irq = VIO_MMIO_IRQ, .level = 1 };
    ioctl(vmm->vm_fd, KVM_IRQ_LINE, &irq);
}

static uint32_t vio_read32(struct vmm *vmm, uint64_t gpa)
{
    uint32_t *p = (uint32_t *)(uintptr_t)vio_guest_to_host(vmm, gpa);
    return p ? *p : 0;
}

static uint16_t vio_read16(struct vmm *vmm, uint64_t gpa)
{
    uint16_t *p = (uint16_t *)(uintptr_t)vio_guest_to_host(vmm, gpa);
    return p ? *p : 0;
}

static void vio_write8(struct vmm *vmm, uint64_t gpa, uint8_t v)
{
    uint8_t *p = (uint8_t *)(uintptr_t)vio_guest_to_host(vmm, gpa);
    if (p) *p = v;
}

static void vio_write16(struct vmm *vmm, uint64_t gpa, uint16_t v)
{
    uint16_t *p = (uint16_t *)(uintptr_t)vio_guest_to_host(vmm, gpa);
    if (p) *p = v;
}

static void vio_write32(struct vmm *vmm, uint64_t gpa, uint32_t v)
{
    uint32_t *p = (uint32_t *)(uintptr_t)vio_guest_to_host(vmm, gpa);
    if (p) *p = v;
}

static void vio_copy_to_guest(struct vmm *vmm, uint64_t gpa,
                              const uint8_t *src, uint32_t len)
{
    uint8_t *dst = (uint8_t *)(uintptr_t)vio_guest_to_host(vmm, gpa);
    if (dst && gpa + len <= vmm->mem_size)
        memcpy(dst, src, len);
}

static void vio_copy_from_guest(struct vmm *vmm, uint8_t *dst,
                                uint64_t gpa, uint32_t len)
{
    uint8_t *src = (uint8_t *)(uintptr_t)vio_guest_to_host(vmm, gpa);
    if (src && gpa + len <= vmm->mem_size)
        memcpy(dst, src, len);
}

static void vio_process_queue(struct vmm *vmm)
{
    if (vmm->vio_queue_num == 0 || vmm->vio_queue_pfn == 0)
        return;

    uint64_t base = (uint64_t)vmm->vio_queue_pfn * vmm->vio_guest_page_size;
    uint32_t n = vmm->vio_queue_num;
    uint64_t desc_gpa = base;
    uint64_t avail_gpa = base + n * sizeof(struct vring_desc);
    /* vring legacy layout: desc, avail(6 + 2*n), then used aligned. */
    uint32_t used_off = (n * sizeof(struct vring_desc) +
                         2 * (3 + n) +
                         vmm->vio_queue_align - 1) &
                        ~(uint32_t)(vmm->vio_queue_align - 1);
    uint64_t used_gpa = base + used_off;

    uint16_t avail_idx = vio_read16(vmm, avail_gpa + 2);

    while (vmm->vio_used_idx != avail_idx) {
        uint16_t desc_idx = vio_read16(vmm,
            avail_gpa + 4 + (vmm->vio_used_idx % n) * 2);

        uint64_t hdr_gpa = desc_idx * sizeof(struct vring_desc) + desc_gpa;
        struct vring_desc hdr_desc;
        vio_copy_from_guest(vmm, (uint8_t *)&hdr_desc, hdr_gpa, sizeof(hdr_desc));

        struct virtio_blk_req req;
        vio_copy_from_guest(vmm, (uint8_t *)&req, hdr_desc.addr, sizeof(req));

        uint8_t  status = 0;
        uint16_t next   = hdr_desc.next;
        uint8_t  data[131072];
        uint32_t total  = 0;
        struct vring_desc st_desc = hdr_desc;

        /* Gather or scatter data segments; stop when a desc has no NEXT. */
        if (le32toh(req.type) == VIO_BLK_T_IN ||
            le32toh(req.type) == VIO_BLK_T_OUT) {
            uint64_t off = le64toh(req.sector) * 512ULL;
            while (next < n) {
                uint64_t d_gpa = desc_gpa + next * sizeof(struct vring_desc);
                struct vring_desc d;
                vio_copy_from_guest(vmm, (uint8_t *)&d, d_gpa, sizeof(d));

                if (!(d.flags & 0x01)) { /* no NEXT: this is the status desc */
                    st_desc = d;
                    break;
                }

                uint32_t chunk = d.len;
                if (total + chunk > sizeof(data))
                    chunk = sizeof(data) - total;

                if (le32toh(req.type) == VIO_BLK_T_IN) {
                    (void)pread64(vmm->disk_fd, data + total, chunk,
                                  (off64_t)(off + total));
                    vio_copy_to_guest(vmm, d.addr, data + total, chunk);
                } else if (le32toh(req.type) == VIO_BLK_T_OUT) {
                    vio_copy_from_guest(vmm, data + total, d.addr, chunk);
                    (void)pwrite64(vmm->disk_fd, data + total, chunk,
                                   (off64_t)(off + total));
                }

                total += chunk;
                next = d.next;
            }
        }

        /* st_desc is the last descriptor in the chain, which is the status. */
        vio_write8(vmm, st_desc.addr, status);

        uint16_t uidx = vio_read16(vmm, used_gpa + 2);
        uint64_t uelem = used_gpa + 4 + (uidx % n) * sizeof(struct vring_used_elem);
        vio_write32(vmm, uelem, desc_idx);
        vio_write32(vmm, uelem + 4, total);     /* bytes in data segments */
        vio_write16(vmm, used_gpa + 2, uidx + 1);

        vmm->vio_used_idx++;
    }

    vmm->vio_isr |= 1;
    vio_inject_irq(vmm);
}

static uint64_t virtio_mmio_read(struct vmm *vmm, uint64_t off, unsigned len)
{
    uint64_t val = 0;
    unsigned i;

    pthread_mutex_lock(&vmm->vio_mutex);

    if (off >= VIO_REG_CONFIG &&
        off - VIO_REG_CONFIG + len <= sizeof(vmm->vio_config)) {
        unsigned base = off - VIO_REG_CONFIG;
        for (i = 0; i < len && i < 8; i++)
            val |= (uint64_t)vmm->vio_config[base + i] << (i * 8);
        pthread_mutex_unlock(&vmm->vio_mutex);
        return val;
    }

    switch (off) {
    case VIO_REG_MAGIC:          val = 0x74726976; break;
    case VIO_REG_VERSION:        val = 1;          break;
    case VIO_REG_DEVICE_ID:      val = 2;          break;
    case VIO_REG_VENDOR_ID:      val = 0x554d4551; break; /* "QEMU" */
    case VIO_REG_HOST_FEATURES:  val = 0;          break;
    case VIO_REG_QUEUE_NUM_MAX:  val = 64;         break;
    case VIO_REG_QUEUE_NUM:      val = vmm->vio_queue_num;      break;
    case VIO_REG_QUEUE_PFN:      val = vmm->vio_queue_pfn;      break;
    case VIO_REG_INTERRUPT_STATUS: val = vmm->vio_isr;          break;
    case VIO_REG_STATUS:         val = vmm->vio_status;         break;
    default:
        break;
    }

    if (len < 8)
        val &= (1ULL << (len * 8)) - 1;
    pthread_mutex_unlock(&vmm->vio_mutex);
    return val;
}

static void virtio_mmio_write(struct vmm *vmm, uint64_t off, uint64_t val)
{
    pthread_mutex_lock(&vmm->vio_mutex);

    switch (off) {
    case VIO_REG_GUEST_PAGE_SIZE:
        vmm->vio_guest_page_size = (uint32_t)val;
        break;
    case VIO_REG_QUEUE_SEL:
        vmm->vio_queue_sel = (uint32_t)val;
        break;
    case VIO_REG_QUEUE_NUM:
        vmm->vio_queue_num = (uint32_t)val;
        break;
    case VIO_REG_QUEUE_ALIGN:
        vmm->vio_queue_align = (uint32_t)val;
        if (vmm->vio_queue_align == 0)
            vmm->vio_queue_align = 4096;
        break;
    case VIO_REG_QUEUE_PFN:
        vmm->vio_queue_pfn = (uint32_t)val;
        break;
    case VIO_REG_QUEUE_NOTIFY:
        if (val == 0)
            vio_process_queue(vmm);
        break;
    case VIO_REG_INTERRUPT_ACK:
        vmm->vio_isr &= ~(uint32_t)val;
        if (vmm->vio_isr == 0) {
            struct kvm_irq_level irq = { .irq = VIO_MMIO_IRQ, .level = 0 };
            ioctl(vmm->vm_fd, KVM_IRQ_LINE, &irq);
        }
        break;
    case VIO_REG_STATUS:
        if (val == 0) {
            vmm->vio_status = 0;
            vmm->vio_queue_num = 0;
            vmm->vio_queue_pfn = 0;
            vmm->vio_used_idx = 0;
        } else {
            vmm->vio_status = (uint32_t)val;
        }
        break;
    default:
        break;
    }

    pthread_mutex_unlock(&vmm->vio_mutex);
}

/*
 * MMIO exits are expected if the kernel tries to touch the local APIC or
 * another un-emulated device.  We log the access, zero any read data, and
 * resume.  Real VMMs would implement the device here.
 */
void vmm_handle_mmio(struct vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    uint64_t addr = run->mmio.phys_addr;
    unsigned len = run->mmio.len;

    if (addr >= VIO_MMIO_BASE && addr < VIO_MMIO_BASE + VIO_MMIO_SIZE) {
        uint64_t off = addr - VIO_MMIO_BASE;
        if (run->mmio.is_write) {
            uint64_t val = 0;
            for (unsigned i = 0; i < len; i++)
                val |= (uint64_t)run->mmio.data[i] << (i * 8);
            virtio_mmio_write(vcpu->vmm, off, val);
        } else {
            uint64_t val = virtio_mmio_read(vcpu->vmm, off, len);
            for (unsigned i = 0; i < len && i < 8; i++)
                run->mmio.data[i] = (uint8_t)(val >> (i * 8));
        }
        return;
    }

    if (run->mmio.is_write) {
        DBG("vCPU %d MMIO WRITE 0x%016llx len=%u",
            vcpu->id, (unsigned long long)run->mmio.phys_addr,
            run->mmio.len);
    } else {
        DBG("vCPU %d MMIO READ  0x%016llx len=%u",
            vcpu->id, (unsigned long long)run->mmio.phys_addr,
            run->mmio.len);
        /* Return all zeros for reads from missing devices. */
        if (run->mmio.len <= 8)
            memset(run->mmio.data, 0, run->mmio.len);
    }
}

/* ---- vCPU run loop -------------------------------------------------------- */

void *vcpu_thread(void *arg)
{
    struct vcpu *vcpu = arg;

    /* Initialize the vCPU real-mode state. */
    if (vcpu_init_regs(vcpu) < 0)
        return NULL;

    LOG("vCPU %d starting (BSP=%d)", vcpu->id, vcpu->is_bsp);
    vcpu->running = true;

    while (vcpu->running) {
        int r = ioctl(vcpu->fd, KVM_RUN, 0);
        if (r < 0) {
            LOG("vCPU %d KVM_RUN failed: %s", vcpu->id, strerror(errno));
            break;
        }

        switch (vcpu->run->exit_reason) {
        case KVM_EXIT_HLT:
            LOG("vCPU %d exited: HLT", vcpu->id);
            vcpu->running = false;
            break;

        case KVM_EXIT_SHUTDOWN:
            LOG("vCPU %d exited: SHUTDOWN", vcpu->id);
            vcpu->running = false;
            break;

        case KVM_EXIT_IO:
            vmm_handle_io(vcpu);
            break;

        case KVM_EXIT_MMIO:
            vmm_handle_mmio(vcpu);
            break;

        case KVM_EXIT_INTR:
        case KVM_EXIT_IRQ_WINDOW_OPEN:
            /* Let the in-kernel IRQ chip deliver the interrupt and resume. */
            break;

        case KVM_EXIT_DEBUG:
            /* Only expected if KVM_SET_GUEST_DEBUG is enabled. */
            LOG("vCPU %d exited: DEBUG", vcpu->id);
            vcpu->running = false;
            break;

        case KVM_EXIT_FAIL_ENTRY:
            LOG("vCPU %d KVM_EXIT_FAIL_ENTRY: hardware_entry_failure="
                "0x%llu", vcpu->id,
                (unsigned long long)vcpu->run->fail_entry.hardware_entry_failure_reason);
            vcpu->running = false;
            break;

        case KVM_EXIT_INTERNAL_ERROR:
            LOG("vCPU %d KVM_EXIT_INTERNAL_ERROR: suberror=0x%x",
                vcpu->id, vcpu->run->internal.suberror);
            vcpu->running = false;
            break;

        case KVM_EXIT_UNKNOWN:
            LOG("vCPU %d KVM_EXIT_UNKNOWN: hardware_exit_reason=0x%llu",
                vcpu->id,
                (unsigned long long)vcpu->run->hw.hardware_exit_reason);
            vcpu->running = false;
            break;

        default:
            LOG("vCPU %d unhandled exit reason %d",
                vcpu->id, vcpu->run->exit_reason);
            vcpu->running = false;
            break;
        }
    }

    LOG("vCPU %d stopped", vcpu->id);
    return NULL;
}

/* ---- multi-vCPU startup --------------------------------------------------- */

/*
 * Create all requested vCPUs and start a pthread for each one.
 * The BSP is vCPU0; the rest are APs.  Memory has already been prepared.
 */
void vmm_run_all(struct vmm *vmm)
{
    for (int i = 0; i < vmm->nr_vcpus; i++) {
        vmm->vcpus[i] = vcpu_create(vmm, i);
        if (!vmm->vcpus[i]) {
            LOG("failed to create vCPU %d", i);
            exit(1);
        }
    }

    for (int i = 0; i < vmm->nr_vcpus; i++) {
        if (pthread_create(&vmm->vcpus[i]->thread, NULL,
                           vcpu_thread, vmm->vcpus[i]) != 0) {
            LOG("pthread_create for vCPU %d failed: %s", i, strerror(errno));
            exit(1);
        }
    }
}

void vmm_wait_all(struct vmm *vmm)
{
    for (int i = 0; i < vmm->nr_vcpus; i++) {
        if (vmm->vcpus[i])
            pthread_join(vmm->vcpus[i]->thread, NULL);
    }
    LOG("all vCPUs stopped, cleaning up");
}

/* ---- main ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *kernel_path = NULL;
    const char *initrd_path = NULL;
    const char *disk_path = NULL;
    size_t mem_mb = GUEST_MEM_DEFAULT_MB;
    int nr_vcpus = GUEST_NR_VCPUS_DEFAULT;
    char cmdline_buf[1024];
    int opt;

    snprintf(cmdline_buf, sizeof(cmdline_buf), "%s", GUEST_CMDLINE_DEFAULT);

    while ((opt = getopt(argc, argv, "k:i:m:s:c:D:dh")) != -1) {
        switch (opt) {
        case 'k':
            kernel_path = optarg;
            break;
        case 'i':
            initrd_path = optarg;
            break;
        case 'm':
            mem_mb = strtoul(optarg, NULL, 10);
            break;
        case 's':
            nr_vcpus = (int)strtol(optarg, NULL, 10);
            if (nr_vcpus < 1)
                nr_vcpus = 1;
            break;
        case 'c':
            snprintf(cmdline_buf, sizeof(cmdline_buf), "%s", optarg);
            break;
        case 'D':
            disk_path = optarg;
            break;
        case 'd':
            g_debug = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
        }
    }

    if (!kernel_path)
        usage(argv[0]);

    /* KVM minimum: at least enough for low 1M + some high memory. */
    if (mem_mb < 8)
        mem_mb = 8;
    size_t mem_size = (size_t)mem_mb * 1024 * 1024;

    LOG("creating %d vCPU(s), %zu MiB guest RAM", nr_vcpus, mem_mb);

    struct vmm *vmm = vmm_create(mem_size, nr_vcpus);
    if (!vmm)
        return 1;

    if (vmm_map_memory(vmm) < 0) {
        vmm_destroy(vmm);
        return 1;
    }

    if (disk_path && vmm_load_disk(vmm, disk_path) < 0) {
        vmm_destroy(vmm);
        return 1;
    }

    /*
     * Install the 16-bit reset vector and the AP hlt stub.  This must happen
     * before the kernel, because the reset vector jumps to the kernel start.
     */
    bios_install(vmm);

    /* Build the minimal ACPI tables and place them in guest low memory. */
    if (acpi_install(vmm) < 0) {
        LOG("ACPI install failed");
        vmm_destroy(vmm);
        return 1;
    }

    /* Load the bzImage and the initrd, fill boot params / E820. */
    if (kernel_load(vmm, kernel_path, initrd_path, cmdline_buf) < 0) {
        vmm_destroy(vmm);
        return 1;
    }

    /* Start vCPUs in threads and wait until they all stop. */
    vmm_run_all(vmm);
    vmm_wait_all(vmm);

    vmm_destroy(vmm);
    return 0;
}
