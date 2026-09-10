/*
 * Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL.
 * Unauthorized copying, reproduction, distribution, or modification of this
 * file, via any medium, is strictly prohibited.
 * All rights reserved.
 */

// vmm.cpp -- vmtainer: Minimal KVM VMM with snapshots, virtiofs, vhost-net
//
// Usage:
//   vmtainer boot   <bzImage> <initrd> --share <dir> [--snapshot <path>]
//   vmtainer restore <snapshot>
//   vmtainer clone   <snapshot> <count> --share <dir>

#include "vmm.hpp"

bool g_debug = false;

// ---------------------------------------------------------------------------
// Implementation -- Setup
// ---------------------------------------------------------------------------

bool Vmm::query_msr_list() {
    size_t sz = sizeof(struct kvm_msr_list) + MAX_MSRS * sizeof(uint32_t);
    auto *list = (struct kvm_msr_list *)calloc(1, sz);
    list->nmsrs = MAX_MSRS;

    if (ioctl(kvm_fd_, KVM_GET_MSR_INDEX_LIST, list) < 0) {
        perror("KVM_GET_MSR_INDEX_LIST");
        free(list);
        return false;
    }

    num_msrs_ = (list->nmsrs > MAX_MSRS) ? MAX_MSRS : list->nmsrs;
    memcpy(msr_list_, list->indices, num_msrs_ * sizeof(uint32_t));
    free(list);
    return true;
}

bool Vmm::init(bool zero_ram) {
    DBG("init: ram_mb=%zu zero_ram=%d", ram_mb_, zero_ram);
    kvm_fd_ = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd_ < 0) { perror("open /dev/kvm"); return false; }
    if (ioctl(kvm_fd_, KVM_GET_API_VERSION, 0) != 12) {
        fprintf(stderr, "KVM API version mismatch\n");
        return false;
    }

    if (!query_msr_list()) return false;

    vm_fd_ = ioctl(kvm_fd_, KVM_CREATE_VM, 0);
    if (vm_fd_ < 0) { perror("KVM_CREATE_VM"); return false; }

    if (ioctl(vm_fd_, KVM_CREATE_IRQCHIP, 0) < 0) {
        perror("KVM_CREATE_IRQCHIP"); return false;
    }

    struct kvm_pit_config pit = {};
    pit.flags = KVM_PIT_SPEAKER_DUMMY;
    if (ioctl(vm_fd_, KVM_CREATE_PIT2, &pit) < 0) {
        perror("KVM_CREATE_PIT2"); return false;
    }

    // Allocate guest RAM via memfd so we can share it with virtiofsd.
    // Try hugetlb if requested (2MB pages = fewer faults), fall back to regular.
    if (use_hugetlb_) {
        ram_memfd_ = memfd_create("guest_ram", MFD_CLOEXEC | MFD_HUGETLB);
        if (ram_memfd_ < 0) {
            DBG("init: hugetlb memfd failed, falling back to regular");
            use_hugetlb_ = false;
        }
    }
    if (ram_memfd_ < 0)
        ram_memfd_ = memfd_create("guest_ram", MFD_CLOEXEC);
    if (ram_memfd_ < 0) { perror("memfd_create"); return false; }
    if (ftruncate(ram_memfd_, ram_bytes_) < 0) {
        perror("ftruncate memfd"); return false;
    }
    ram_ = mmap(nullptr, ram_bytes_, PROT_READ | PROT_WRITE,
                MAP_SHARED, ram_memfd_, 0);
    if (ram_ == MAP_FAILED) { perror("mmap ram"); return false; }
    if (zero_ram) memset(ram_, 0, ram_bytes_);
    DBG("init: ram=%p memfd=%d size=%zuMB hugetlb=%d", ram_, ram_memfd_, ram_bytes_ >> 20, use_hugetlb_);

    if (use_uffd_ && !init_uffd()) {
        DBG("init: userfaultfd init failed, falling back to memcpy restore");
        use_uffd_ = false;
    }

    if (!set_memslot(0, 0, ram_, ram_bytes_)) return false;

    vcpu_fd_ = ioctl(vm_fd_, KVM_CREATE_VCPU, 0);
    if (vcpu_fd_ < 0) { perror("KVM_CREATE_VCPU"); return false; }

    run_mmap_sz_ = ioctl(kvm_fd_, KVM_GET_VCPU_MMAP_SIZE, 0);
    kvm_run_ = (struct kvm_run *)mmap(nullptr, run_mmap_sz_,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, vcpu_fd_, 0);
    if (kvm_run_ == MAP_FAILED) { perror("mmap kvm_run"); return false; }

    int nent = 256;
    cpuid_ = (struct kvm_cpuid2 *)calloc(1, sizeof(kvm_cpuid2)
                + nent * sizeof(kvm_cpuid_entry2));
    cpuid_->nent = nent;
    if (ioctl(kvm_fd_, KVM_GET_SUPPORTED_CPUID, cpuid_) < 0) {
        perror("KVM_GET_SUPPORTED_CPUID"); return false;
    }

    return true;
}

bool Vmm::set_memslot(uint32_t slot, uint64_t gpa, void *hva, size_t len) {
    struct kvm_userspace_memory_region r = {};
    r.slot = slot;
    r.guest_phys_addr = gpa;
    r.memory_size = len;
    r.userspace_addr = (uint64_t)hva;
    if (ioctl(vm_fd_, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION"); return false;
    }
    return true;
}

int Vmm::read_file(const char *path, void **buf, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    *buf = malloc(st.st_size);
    *len = ::read(fd, *buf, st.st_size);
    close(fd);
    return (*len == (size_t)st.st_size) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// BIOS / boot
// ---------------------------------------------------------------------------

bool Vmm::setup_bios() {
    memset(gpa_ptr(BDA_START), 0, BDA_END - BDA_START + 1);
    memset(gpa_ptr(EBDA_START), 0, EBDA_END - EBDA_START + 1);
    memset(gpa_ptr(MB_BIOS_BEGIN), 0, MB_BIOS_SIZE);
    memset(gpa_ptr(VGA_ROM_BEGIN), 0, VGA_ROM_END - VGA_ROM_BEGIN + 1);
    memcpy(gpa_ptr(MB_BIOS_BEGIN), bios_rom, bios_rom_size);
    setup_e820();
    setup_vga_rom();

    struct real_intr_desc fake = {
        .offset  = BIOS_OFFSET__bios_intfake,
        .segment = (uint16_t)(MB_BIOS_BEGIN >> 4),
    };
    for (int i = 0; i < REAL_INTR_VECTORS; i++) ivt_[i] = fake;

    install_irq(0x10, MB_BIOS_BEGIN + BIOS_OFFSET__bios_int10,
                bios_rom + BIOS_OFFSET__bios_int10,
                BIOS_OFFSET__bios_int10_end - BIOS_OFFSET__bios_int10);
    install_irq(0x15, MB_BIOS_BEGIN + BIOS_OFFSET__bios_int15,
                bios_rom + BIOS_OFFSET__bios_int15,
                BIOS_OFFSET__bios_int15_end - BIOS_OFFSET__bios_int15);

    memcpy(gpa_ptr(REAL_MODE_IVT_BEGIN), ivt_, sizeof(ivt_));
    return true;
}

void Vmm::install_irq(uint16_t vec, unsigned long addr,
                      const void *code, size_t size) {
    memcpy(gpa_ptr(addr), code, size);
    ivt_[vec] = { (uint16_t)(addr - MB_BIOS_BEGIN),
                  (uint16_t)(MB_BIOS_BEGIN >> 4) };
}

void Vmm::setup_e820() {
    auto *e820 = (struct e820map *)gpa_ptr(E820_MAP_START);
    auto *m = e820->map;
    unsigned i = 0;
    m[i++] = { REAL_MODE_IVT_BEGIN, EBDA_START - REAL_MODE_IVT_BEGIN, E820_RAM };
    m[i++] = { EBDA_START, VGA_RAM_BEGIN - EBDA_START, E820_RESERVED };
    m[i++] = { MB_BIOS_BEGIN, MB_BIOS_SIZE, E820_RESERVED };
    m[i++] = { BZ_KERNEL_START, ram_bytes_ - BZ_KERNEL_START, E820_RAM };
    e820->nr_map = i;
}

void Vmm::setup_vga_rom() {
    char *p = (char *)gpa_ptr(VGA_ROM_OEM_STRING);
    memset(p, 0, VGA_ROM_OEM_STRING_SIZE);
    strncpy(p, "KVM VESA", VGA_ROM_OEM_STRING_SIZE);
    auto *mode = (uint16_t *)gpa_ptr(VGA_ROM_MODES);
    mode[0] = 0x0112;
    mode[1] = 0xffff;
}

bool Vmm::load_bzimage(const char *path) {
    void *buf = nullptr;
    size_t len = 0;
    if (read_file(path, &buf, &len) < 0) { perror("read kernel"); return false; }

    struct boot_params bp;
    memcpy(&bp, buf, sizeof(bp));
    if (bp.hdr.header != 0x53726448) {
        fprintf(stderr, "not a bzImage\n"); free(buf); return false;
    }

    uint16_t ss = bp.hdr.setup_sects ? bp.hdr.setup_sects : BZ_DEFAULT_SETUP_SECTS;
    size_t setup_sz = (ss + 1) * 512;
    memcpy(gpa_ptr(BOOT_LOADER_SELECTOR * 16ULL), buf, setup_sz);
    memcpy(gpa_ptr(BZ_KERNEL_START), (uint8_t *)buf + setup_sz, len - setup_sz);
    free(buf);

    // Patch boot params in guest memory
    size_t cm = bp.hdr.cmdline_size ? bp.hdr.cmdline_size : 1024;
    if (cm > 4096) cm = 4096;
    memset(gpa_ptr(BOOT_CMDLINE_OFFSET), 0, cm);
    strncpy((char *)gpa_ptr(BOOT_CMDLINE_OFFSET), cmdline_, cm - 1);

    auto *kb = (struct boot_params *)gpa_ptr(BOOT_LOADER_SELECTOR * 16ULL);
    kb->hdr.cmd_line_ptr   = BOOT_CMDLINE_OFFSET;
    kb->hdr.type_of_loader = 0xff;
    kb->hdr.heap_end_ptr   = 0xfe00;
    kb->hdr.loadflags     &= ~KASLR_FLAG;
    kb->hdr.loadflags     |= CAN_USE_HEAP;
    kb->hdr.vid_mode       = 0;
    return true;
}

bool Vmm::load_initrd(const char *path) {
    void *buf = nullptr;
    size_t len = 0;
    if (read_file(path, &buf, &len) < 0) { perror("read initrd"); return false; }

    uint64_t addr = (ram_bytes_ - len) & ~0xfffffULL;
    memcpy(gpa_ptr(addr), buf, len);
    free(buf);

    auto *kb = (struct boot_params *)gpa_ptr(BOOT_LOADER_SELECTOR * 16ULL);
    kb->hdr.ramdisk_image = addr;
    kb->hdr.ramdisk_size  = len;
    return true;
}

bool Vmm::setup_cpu() {
    struct kvm_sregs sregs;
    ioctl(vcpu_fd_, KVM_GET_SREGS, &sregs);

    sregs.cs.base = BOOT_LOADER_SELECTOR * 16ULL;
    sregs.cs.selector = BOOT_LOADER_SELECTOR;
    sregs.cs.limit = 0xffff;
    sregs.cs.type = 0xb;
    sregs.cs.present = 1;
    sregs.cs.dpl = sregs.cs.db = sregs.cs.l = sregs.cs.g = 0;
    sregs.cs.s = 1;

    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = sregs.cs;
    sregs.ds.type = sregs.es.type = sregs.fs.type = 0x3;
    sregs.gs.type = sregs.ss.type = 0x3;
    sregs.ds.db = sregs.es.db = sregs.fs.db = 1;
    sregs.gs.db = sregs.ss.db = 1;
    sregs.ss.base = sregs.ss.selector * 16ULL;
    sregs.idt.limit = sregs.gdt.limit = 0xffff;
    sregs.idt.base = sregs.gdt.base = 0;
    sregs.cr0 = 0x10;
    sregs.cr3 = sregs.cr4 = sregs.efer = 0;

    if (ioctl(vcpu_fd_, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS"); return false;
    }

    struct kvm_regs regs = {};
    regs.rip = 0x0200;
    regs.rsp = BOOT_LOADER_SP;
    regs.rflags = 0x2;
    if (ioctl(vcpu_fd_, KVM_SET_REGS, &regs) < 0) {
        perror("KVM_SET_REGS"); return false;
    }

    if (ioctl(vcpu_fd_, KVM_SET_CPUID2, cpuid_) < 0) {
        perror("KVM_SET_CPUID2"); return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Serial (16550 UART)
// ---------------------------------------------------------------------------

void Vmm::serial_in(uint16_t port, uint8_t *data) {
    bool dlab = uart_lcr_ & 0x80;
    switch (port) {
    case 0x3f8: *data = dlab ? uart_dll_ : 0;              break;
    case 0x3f9: *data = dlab ? uart_dlh_ : uart_ier_;      break;
    case 0x3fa: *data = (uart_ier_ & 2) ? 0x02 : 0x01;     break;
    case 0x3fb: *data = uart_lcr_;  break;
    case 0x3fc: *data = uart_mcr_;  break;
    case 0x3fd: *data = uart_lsr_;  break;
    case 0x3fe: *data = uart_msr_;  break;
    case 0x3ff: *data = uart_scr_;  break;
    default:    *data = 0;          break;
    }
}

void Vmm::serial_out(uint16_t port, uint8_t data) {
    bool dlab = uart_lcr_ & 0x80;
    switch (port) {
    case 0x3f8:
        if (dlab) { uart_dll_ = data; break; }
        if (data == '\n') {
            if (timing_entrypoint_ && serial_line_buf_.rfind("VMTAINER:", 0) == 0) {
                printf("[+%6.2fms] %s\n", ms_since_start(), serial_line_buf_.c_str());
            } else {
                printf("%s\n", serial_line_buf_.c_str());
            }
            fflush(stdout);
            if (timing_entrypoint_ && !entrypoint_measured_) {
                if (serial_line_buf_.find("VMTAINER: running entrypoint") != std::string::npos) {
                    entrypoint_measured_ = true;
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    long us_total = (now.tv_sec - t_cmd_start_.tv_sec) * 1000000L +
                                    (now.tv_nsec - t_cmd_start_.tv_nsec) / 1000L;
                    long us_guest = (now.tv_sec - t_vcpu_start_.tv_sec) * 1000000L +
                                    (now.tv_nsec - t_vcpu_start_.tv_nsec) / 1000L;
                    long us_vmm = us_total > us_guest ? us_total - us_guest : 0;
                    time_to_entrypoint_ms_ = us_total / 1000.0;

                    printf("\n[VMM] ====================================================\n");
                    printf("[VMM] TIME TO START OF ENTRYPOINT EXECUTION: %.2fms\n", us_total / 1000.0);
                    printf("[VMM]   - VMM setup & snapshot restore:        %.2fms\n", us_vmm / 1000.0);
                    printf("[VMM]   - Guest mount & init to entrypoint:    %.2fms\n", us_guest / 1000.0);
                    printf("[VMM] ====================================================\n\n");
                    fflush(stdout);
                }
            }
            serial_line_buf_.clear();
        } else if (serial_line_buf_.size() < 1024) {
            serial_line_buf_ += (char)data;
        }
        {
            struct kvm_irq_level irq = {};
            irq.irq = 4;
            irq.level = 1; ioctl(vm_fd_, KVM_IRQ_LINE, &irq);
            irq.level = 0; ioctl(vm_fd_, KVM_IRQ_LINE, &irq);
        }
        break;
    case 0x3f9: if (dlab) uart_dlh_ = data; else uart_ier_ = data; break;
    case 0x3fa: uart_fcr_ = data; break;
    case 0x3fb: uart_lcr_ = data; break;
    case 0x3fc: uart_mcr_ = data; break;
    case 0x3ff: uart_scr_ = data; break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

int Vmm::run() {
    int consecutive_hlt_eintr = 0;
    for (;;) {
        check_virtio_irqs();

        if (ioctl(vcpu_fd_, KVM_RUN, 0) < 0) {
            if (errno == EINTR) {
                struct kvm_regs regs;
                ioctl(vcpu_fd_, KVM_GET_REGS, &regs);
                if (!(regs.rflags & 0x200)) {
                    struct kvm_mp_state mp = {};
                    if (ioctl(vcpu_fd_, KVM_GET_MP_STATE, &mp) == 0 &&
                        mp.mp_state == KVM_MP_STATE_HALTED) {
                        if (++consecutive_hlt_eintr >= 2)
                            return 0;
                    } else {
                        consecutive_hlt_eintr = 0;
                    }
                } else {
                    consecutive_hlt_eintr = 0;
                }
                continue;
            }
            perror("KVM_RUN");
            return -1;
        }
        consecutive_hlt_eintr = 0;

        switch (kvm_run_->exit_reason) {

        case KVM_EXIT_IO: {
            auto *p = (uint8_t *)kvm_run_ + kvm_run_->io.data_offset;
            uint16_t port = kvm_run_->io.port;

            if (kvm_run_->io.direction == KVM_EXIT_IO_OUT) {
                if (port >= 0x3f8 && port <= 0x3ff && kvm_run_->io.size == 1) {
                    for (uint32_t i = 0; i < kvm_run_->io.count; i++)
                        serial_out(port, p[i]);
                }
            } else {
                if (port >= 0x3f8 && port <= 0x3ff)
                    serial_in(port, p);
                else
                    memset(p, 0, kvm_run_->io.size * kvm_run_->io.count);
            }
            break;
        }

        case KVM_EXIT_MMIO: {
            uint64_t addr = kvm_run_->mmio.phys_addr;
            uint32_t len  = kvm_run_->mmio.len;

            // Snapshot signal device
            if (kvm_run_->mmio.is_write && addr == MMIO_SIGNAL_GPA) {
                uint32_t val = 0;
                memcpy(&val, kvm_run_->mmio.data, len < 4 ? len : 4);
                if (val == MMIO_SIGNAL_MAGIC) {
                    if (ignore_next_signal_) {
                        ignore_next_signal_ = false;
                    } else {
                        return 1;
                    }
                }
                break;
            }

            // Virtio-fs MMIO region
            if (addr >= VIRTIO_MMIO_GPA &&
                addr < VIRTIO_MMIO_GPA + VIRTIO_MMIO_SIZE) {
                uint64_t off = addr - VIRTIO_MMIO_GPA;
                if (kvm_run_->mmio.is_write)
                    virtio_mmio_write(off, kvm_run_->mmio.data, len);
                else
                    virtio_mmio_read(off, kvm_run_->mmio.data, len);
                break;
            }

            // Virtio-net MMIO region
            if (addr >= VIRTIO_NET_MMIO_GPA &&
                addr < VIRTIO_NET_MMIO_GPA + VIRTIO_NET_MMIO_SIZE) {
                uint64_t off = addr - VIRTIO_NET_MMIO_GPA;
                if (kvm_run_->mmio.is_write)
                    net_mmio_write(off, kvm_run_->mmio.data, len);
                else
                    net_mmio_read(off, kvm_run_->mmio.data, len);
                break;
            }

            // Unknown MMIO -- return 0xff for reads
            if (!kvm_run_->mmio.is_write)
                memset(kvm_run_->mmio.data, 0xff, len);
            break;
        }

        case KVM_EXIT_HLT: {
            struct kvm_regs hlt_regs;
            ioctl(vcpu_fd_, KVM_GET_REGS, &hlt_regs);
            if (!(hlt_regs.rflags & 0x200))
                return 0;
            break;
        }
        case KVM_EXIT_SHUTDOWN:
            return 0;

        case KVM_EXIT_FAIL_ENTRY:
            fprintf(stderr, "FAIL_ENTRY: 0x%llx\n",
                    (unsigned long long)kvm_run_->fail_entry.hardware_entry_failure_reason);
            return -1;

        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr, "INTERNAL_ERROR: suberror=%u ndata=%u\n",
                    kvm_run_->internal.suberror, kvm_run_->internal.ndata);
            for (uint32_t di = 0; di < kvm_run_->internal.ndata; di++) {
                fprintf(stderr, "  data[%u] = 0x%llx\n", di,
                        (unsigned long long)kvm_run_->internal.data[di]);
            }
            return -1;

        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Snapshot -- save (in-memory)
// ---------------------------------------------------------------------------

bool Vmm::save_snapshot(Snapshot &snap) {
    auto &h = snap.hdr;
    memset(&h, 0, sizeof(h));
    h.magic   = SNAP_MAGIC;
    h.version = SNAP_VERSION;
    h.ram_mb  = ram_mb_;

    // vCPU registers
    if (ioctl(vcpu_fd_, KVM_GET_REGS,       &h.regs)      < 0) { perror("get regs");      return false; }
    if (ioctl(vcpu_fd_, KVM_GET_SREGS,      &h.sregs)     < 0) { perror("get sregs");     return false; }
    if (ioctl(vcpu_fd_, KVM_GET_LAPIC,      &h.lapic)     < 0) { perror("get lapic");     return false; }
    if (ioctl(vcpu_fd_, KVM_GET_VCPU_EVENTS,&h.events)    < 0) { perror("get events");    return false; }
    if (ioctl(vcpu_fd_, KVM_GET_XCRS,       &h.xcrs)      < 0) { perror("get xcrs");      return false; }
    if (ioctl(vcpu_fd_, KVM_GET_MP_STATE,   &h.mp_state)  < 0) { perror("get mp_state");  return false; }
    if (ioctl(vcpu_fd_, KVM_GET_DEBUGREGS,  &h.debugregs) < 0) { perror("get debugregs"); return false; }

    // VM-global state
    if (ioctl(vm_fd_, KVM_GET_CLOCK, &h.clock) < 0) { perror("get clock"); return false; }
    if (ioctl(vm_fd_, KVM_GET_PIT2,  &h.pit)   < 0) { perror("get pit");   return false; }

    h.pic_master.chip_id = KVM_IRQCHIP_PIC_MASTER;
    h.pic_slave.chip_id  = KVM_IRQCHIP_PIC_SLAVE;
    h.ioapic.chip_id     = KVM_IRQCHIP_IOAPIC;
    if (ioctl(vm_fd_, KVM_GET_IRQCHIP, &h.pic_master) < 0) { perror("get pic0");   return false; }
    if (ioctl(vm_fd_, KVM_GET_IRQCHIP, &h.pic_slave)  < 0) { perror("get pic1");   return false; }
    if (ioctl(vm_fd_, KVM_GET_IRQCHIP, &h.ioapic)     < 0) { perror("get ioapic"); return false; }

    int tsc = ioctl(vcpu_fd_, KVM_GET_TSC_KHZ, 0);
    h.tsc_khz    = (tsc > 0) ? (uint32_t)tsc : 0;
    h.cpuid_nent = cpuid_->nent;
    h.num_msrs   = num_msrs_;
    h.xsave_size = XSAVE_SIZE;

    // Virtio device state
    h.vdev_status = vdev_status_;
    h.vdrv_features_lo = (uint32_t)(vdrv_features_ & 0xFFFFFFFF);
    h.vdrv_features_hi = (uint32_t)(vdrv_features_ >> 32);
    h.num_vqs = NUM_QUEUES;
    for (int i = 0; i < NUM_QUEUES; i++) {
        h.vq_state[i].num    = vqs_[i].num;
        h.vq_state[i].ready  = vqs_[i].ready;
        h.vq_state[i].desc   = vqs_[i].desc;
        h.vq_state[i].driver = vqs_[i].driver;
        h.vq_state[i].device = vqs_[i].device;
    }

    // Virtio-net device state
    h.net_status = net_status_;
    h.net_drv_features_lo = (uint32_t)(net_drv_features_ & 0xFFFFFFFF);
    h.net_drv_features_hi = (uint32_t)(net_drv_features_ >> 32);
    h.net_num_vqs = NET_NUM_QUEUES;
    for (int i = 0; i < NET_NUM_QUEUES; i++) {
        h.net_vq_state[i].num    = net_vqs_[i].num;
        h.net_vq_state[i].ready  = net_vqs_[i].ready;
        h.net_vq_state[i].desc   = net_vqs_[i].desc;
        h.net_vq_state[i].driver = net_vqs_[i].driver;
        h.net_vq_state[i].device = net_vqs_[i].device;
    }
    memcpy(h.net_mac, net_config_.mac, 6);

    // XSAVE
    memset(snap.xsave, 0, XSAVE_SIZE);
    if (ioctl(vcpu_fd_, KVM_GET_XSAVE2, snap.xsave) < 0) {
        if (ioctl(vcpu_fd_, KVM_GET_XSAVE, snap.xsave) < 0) {
            perror("get xsave"); return false;
        }
    }

    // CPUID
    snap.cpuid.resize(cpuid_->nent);
    memcpy(snap.cpuid.data(), cpuid_->entries,
           cpuid_->nent * sizeof(kvm_cpuid_entry2));

    // MSRs
    size_t msz = sizeof(kvm_msrs) + num_msrs_ * sizeof(kvm_msr_entry);
    auto *msrs = (kvm_msrs *)calloc(1, msz);
    msrs->nmsrs = num_msrs_;
    for (uint32_t i = 0; i < num_msrs_; i++)
        msrs->entries[i].index = msr_list_[i];
    ioctl(vcpu_fd_, KVM_GET_MSRS, msrs);
    snap.msrs.assign(msrs->entries, msrs->entries + num_msrs_);
    free(msrs);

    // Guest RAM -- mmap a private copy
    snap.ram_size = ram_bytes_;
    snap.ram = mmap(nullptr, ram_bytes_, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (snap.ram == MAP_FAILED) { perror("mmap snap ram"); return false; }
    memcpy(snap.ram, ram_, ram_bytes_);

    return true;
}

// ---------------------------------------------------------------------------
// userfaultfd Demand-Paged Restore
// ---------------------------------------------------------------------------

bool Vmm::init_uffd() {
    uffd_ = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd_ < 0) {
        DBG("init_uffd: userfaultfd syscall failed: %s", strerror(errno));
        return false;
    }

    struct uffdio_api api = {};
    api.api = UFFD_API;
    api.features = 0;
    if (ioctl(uffd_, UFFDIO_API, &api) < 0) {
        DBG("init_uffd: UFFDIO_API failed: %s", strerror(errno));
        close(uffd_);
        uffd_ = -1;
        return false;
    }

    struct uffdio_register reg = {};
    reg.range.start = (uint64_t)ram_;
    reg.range.len = ram_bytes_;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(uffd_, UFFDIO_REGISTER, &reg) < 0) {
        DBG("init_uffd: UFFDIO_REGISTER failed: %s", strerror(errno));
        close(uffd_);
        uffd_ = -1;
        return false;
    }

    uffd_wakeup_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    DBG("init_uffd: registered userfaultfd on [%p, %p) sz=%zuMB",
        ram_, (char*)ram_ + ram_bytes_, ram_bytes_ >> 20);
    return true;
}

void Vmm::start_uffd_thread() {
    if (uffd_ < 0 || uffd_running_.load()) return;
    uffd_running_.store(true);
    pthread_create(&uffd_thread_, nullptr, uffd_worker_func, this);
}

void *Vmm::uffd_worker_func(void *arg) {
    auto *vmm = (Vmm *)arg;
    vmm->uffd_worker();
    return nullptr;
}

void Vmm::uffd_worker() {
    struct pollfd pfds[2];
    pfds[0].fd = uffd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = uffd_wakeup_fd_;
    pfds[1].events = POLLIN;

    constexpr size_t PAGE = 4096;
    size_t total_faults = 0;

    while (uffd_running_.load(std::memory_order_relaxed)) {
        int pr = poll(pfds, 2, -1);
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR) continue;
            break;
        }

        if (pfds[1].revents & POLLIN) {
            break;
        }

        if (!(pfds[0].revents & POLLIN)) continue;

        struct uffd_msg msg;
        ssize_t n = read(uffd_, &msg, sizeof(msg));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }

        if (msg.event == UFFD_EVENT_PAGEFAULT) {
            uint64_t fault_addr = msg.arg.pagefault.address & ~(PAGE - 1);
            if (fault_addr < (uint64_t)ram_ || fault_addr >= (uint64_t)ram_ + ram_bytes_) {
                fprintf(stderr, "[UFFD] fault out of bounds: 0x%lx\n", (unsigned long)fault_addr);
                continue;
            }

            uint64_t offset = fault_addr - (uint64_t)ram_;
            size_t page_idx = offset / PAGE;

            bool is_dirty = false;
            if (!snap_bitmap_buf_.empty() && page_idx < snap_total_pages_) {
                if (snap_bitmap_buf_[page_idx / 8] & (1 << (page_idx & 7))) {
                    is_dirty = true;
                }
            }

            if (is_dirty && snap_ram_) {
                struct uffdio_copy copy = {};
                copy.src = (uint64_t)(snap_ram_ + offset);
                copy.dst = fault_addr;
                copy.len = PAGE;
                copy.mode = 0;
                copy.copy = 0;
                while (ioctl(uffd_, UFFDIO_COPY, &copy) < 0) {
                    if (errno == EAGAIN) continue;
                    if (errno != EEXIST) perror("UFFDIO_COPY");
                    break;
                }
                total_faults++;
            } else {
                struct uffdio_zeropage zero = {};
                zero.range.start = fault_addr;
                zero.range.len = PAGE;
                zero.mode = 0;
                while (ioctl(uffd_, UFFDIO_ZEROPAGE, &zero) < 0) {
                    if (errno == EAGAIN) continue;
                    if (errno != EEXIST) perror("UFFDIO_ZEROPAGE");
                    break;
                }
                total_faults++;
            }
        }
    }

    uffd_fault_count_.store(total_faults, std::memory_order_relaxed);
    DBG("uffd_worker: finished, handled %zu page faults (%zu KB)",
        total_faults, total_faults * 4);
}

// ---------------------------------------------------------------------------
// Snapshot -- restore (from in-memory Snapshot)
// ---------------------------------------------------------------------------

bool Vmm::restore_snapshot(const Snapshot &snap) {
    const auto &h = snap.hdr;
    DBG("restore_snapshot: magic=0x%lx ver=%u ram_mb=%u bitmap=%u bytes",
        h.magic, h.version, h.ram_mb, h.bitmap_bytes);
    if (h.magic != SNAP_MAGIC || h.version != SNAP_VERSION || h.ram_mb != ram_mb_) {
        fprintf(stderr, "bad snapshot\n");
        return false;
    }

    struct timespec ts0, ts1, ts2;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    // Restore guest RAM
    if (use_uffd_ && uffd_ >= 0) {
        if (!snap_ram_) {
            snap_ram_ = (const uint8_t *)snap.ram;
            snap_bitmap_buf_ = snap.dirty_bitmap;
            snap_total_pages_ = ram_bytes_ / 4096;
        }

        constexpr size_t PAGE = 4096;
        size_t total_pages = ram_bytes_ / PAGE;
        const uint8_t *src = snap_ram_;

        auto prefault_gpa = [&](uint64_t gpa) {
            if (gpa == 0 || gpa >= ram_bytes_) return;
            uint64_t fault_addr = ((uint64_t)ram_ + gpa) & ~(PAGE - 1);
            uint64_t offset = fault_addr - (uint64_t)ram_;
            size_t page_idx = offset / PAGE;
            bool is_dirty = false;
            if (!snap_bitmap_buf_.empty() && page_idx < total_pages) {
                if (snap_bitmap_buf_[page_idx / 8] & (1 << (page_idx & 7)))
                    is_dirty = true;
            }
            if (is_dirty && src) {
                struct uffdio_copy copy = {};
                copy.src = (uint64_t)(src + offset);
                copy.dst = fault_addr;
                copy.len = PAGE;
                ioctl(uffd_, UFFDIO_COPY, &copy);
            } else {
                struct uffdio_zeropage zero = {};
                zero.range.start = fault_addr;
                zero.range.len = PAGE;
                ioctl(uffd_, UFFDIO_ZEROPAGE, &zero);
            }
        };

        // Pre-fault virtqueue rings for virtio-fs
        for (uint32_t i = 0; i < NUM_QUEUES && i < h.num_vqs; i++) {
            if (h.vq_state[i].num > 0) {
                prefault_gpa(h.vq_state[i].desc);
                prefault_gpa(h.vq_state[i].driver);
                prefault_gpa(h.vq_state[i].device);
            }
        }
        // Pre-fault virtqueue rings for virtio-net
        for (uint32_t i = 0; i < NET_NUM_QUEUES && i < h.net_num_vqs; i++) {
            if (h.net_vq_state[i].num > 0) {
                prefault_gpa(h.net_vq_state[i].desc);
                prefault_gpa(h.net_vq_state[i].driver);
                prefault_gpa(h.net_vq_state[i].device);
            }
        }

        start_uffd_thread();
        DBG("restore: uffd demand-paged restore active (rings pre-faulted)");
    } else {
        constexpr size_t PAGE = 4096;
        const uint8_t *src = (const uint8_t *)snap.ram;
        uint8_t *dst = (uint8_t *)ram_;
        size_t total_pages = ram_bytes_ / PAGE;

        if (!snap.dirty_bitmap.empty()) {
            constexpr size_t CHUNK_PAGES = 256; // 1MB per chunk
            size_t num_chunks = (total_pages + CHUNK_PAGES - 1) / CHUNK_PAGES;
            std::atomic<size_t> chunk_idx{0};

            struct SharedCopyContext {
                const uint8_t *src;
                uint8_t *dst;
                const uint8_t *bitmap;
                size_t total_pages;
                size_t num_chunks;
                std::atomic<size_t> *chunk_idx;
                std::atomic<size_t> total_dirty{0};
            } ctx;

            ctx.src = src;
            ctx.dst = dst;
            ctx.bitmap = snap.dirty_bitmap.data();
            ctx.total_pages = total_pages;
            ctx.num_chunks = num_chunks;
            ctx.chunk_idx = &chunk_idx;

            auto dynamic_copy_worker = [](void *arg) -> void * {
                auto *c = (SharedCopyContext *)arg;
                size_t local_dirty = 0;
                while (true) {
                    size_t ch = c->chunk_idx->fetch_add(1, std::memory_order_relaxed);
                    if (ch >= c->num_chunks) break;

                    size_t pg_start = ch * CHUNK_PAGES;
                    size_t pg_end = std::min(pg_start + CHUNK_PAGES, c->total_pages);
                    size_t pg = pg_start;

                    while (pg < pg_end) {
                        if (!(c->bitmap[pg / 8] & (1 << (pg & 7)))) { pg++; continue; }
                        size_t run_start = pg;
                        while (pg < pg_end && (c->bitmap[pg / 8] & (1 << (pg & 7)))) {
                            pg++;
                        }
                        size_t run_len = pg - run_start;
                        local_dirty += run_len;
                        memcpy(c->dst + run_start * 4096,
                               c->src + run_start * 4096,
                               run_len * 4096);
                    }
                }
                c->total_dirty.fetch_add(local_dirty, std::memory_order_relaxed);
                return nullptr;
            };

            int NUM_COPY_THREADS = copy_threads_ > 0 ? copy_threads_ : 4;
            if (NUM_COPY_THREADS > 8) NUM_COPY_THREADS = 8;
            std::vector<pthread_t> threads(NUM_COPY_THREADS - 1);

            for (int t = 0; t < NUM_COPY_THREADS - 1; t++) {
                pthread_create(&threads[t], nullptr, dynamic_copy_worker, &ctx);
            }
            // Main thread participates in copying
            dynamic_copy_worker(&ctx);

            for (int t = 0; t < NUM_COPY_THREADS - 1; t++) {
                pthread_join(threads[t], nullptr);
            }
            size_t dirty_count = ctx.total_dirty.load();

            DBG("restore: bitmap copy %zu/%zu pages (%zuKB) [%d threads, dynamic coalesced]",
                dirty_count, total_pages, dirty_count * 4, NUM_COPY_THREADS);
        } else {
            for (size_t off = 0; off < ram_bytes_; off += PAGE) {
                const uint64_t *p = (const uint64_t *)(src + off);
                uint64_t acc = 0;
                for (size_t w = 0; w < PAGE / sizeof(uint64_t); w += 8)
                    acc |= p[w] | p[w+1] | p[w+2] | p[w+3] |
                           p[w+4] | p[w+5] | p[w+6] | p[w+7];
                if (acc) memcpy(dst + off, src + off, PAGE);
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    // Restore CPUID
    size_t csz = sizeof(kvm_cpuid2) + h.cpuid_nent * sizeof(kvm_cpuid_entry2);
    free(cpuid_);
    cpuid_ = (kvm_cpuid2 *)calloc(1, csz);
    cpuid_->nent = h.cpuid_nent;
    memcpy(cpuid_->entries, snap.cpuid.data(),
           h.cpuid_nent * sizeof(kvm_cpuid_entry2));

    if (h.tsc_khz > 0)
        ioctl(vcpu_fd_, KVM_SET_TSC_KHZ, (unsigned long)h.tsc_khz);

    if (ioctl(vcpu_fd_, KVM_SET_CPUID2,      cpuid_)       < 0) { perror("set cpuid");    return false; }
    if (ioctl(vcpu_fd_, KVM_SET_MP_STATE,     &h.mp_state)  < 0) { perror("set mp_state"); return false; }
    if (ioctl(vcpu_fd_, KVM_SET_SREGS,        &h.sregs)     < 0) { perror("set sregs");    return false; }
    if (ioctl(vcpu_fd_, KVM_SET_REGS,         &h.regs)      < 0) { perror("set regs");     return false; }
    if (ioctl(vcpu_fd_, KVM_SET_XCRS,         &h.xcrs)      < 0) { perror("set xcrs");     return false; }
    if (ioctl(vcpu_fd_, KVM_SET_XSAVE,        (void*)snap.xsave) < 0) { perror("set xsave");    return false; }
    if (ioctl(vcpu_fd_, KVM_SET_DEBUGREGS,    &h.debugregs) < 0) { perror("set debugregs");return false; }
    if (ioctl(vcpu_fd_, KVM_SET_LAPIC,        &h.lapic)     < 0) { perror("set lapic");    return false; }
    if (ioctl(vcpu_fd_, KVM_SET_VCPU_EVENTS,  &h.events)    < 0) { perror("set events");   return false; }

    // MSRs
    size_t msz = sizeof(kvm_msrs) + h.num_msrs * sizeof(kvm_msr_entry);
    auto *msrs = (kvm_msrs *)calloc(1, msz);
    msrs->nmsrs = h.num_msrs;
    memcpy(msrs->entries, snap.msrs.data(), h.num_msrs * sizeof(kvm_msr_entry));
    ioctl(vcpu_fd_, KVM_SET_MSRS, msrs);
    free(msrs);

    // IRQ chips
    if (ioctl(vm_fd_, KVM_SET_IRQCHIP, &h.pic_master) < 0) { perror("set pic0");   return false; }
    if (ioctl(vm_fd_, KVM_SET_IRQCHIP, &h.pic_slave)  < 0) { perror("set pic1");   return false; }
    if (ioctl(vm_fd_, KVM_SET_IRQCHIP, &h.ioapic)     < 0) { perror("set ioapic"); return false; }
    if (ioctl(vm_fd_, KVM_SET_CLOCK,   &h.clock)       < 0) { perror("set clock");  return false; }
    if (ioctl(vm_fd_, KVM_SET_PIT2,    &h.pit)         < 0) { perror("set pit");    return false; }

    // Restore virtio device state
    vdev_status_ = h.vdev_status;
    vdrv_features_ = (uint64_t)h.vdrv_features_lo |
                     ((uint64_t)h.vdrv_features_hi << 32);
    for (uint32_t i = 0; i < NUM_QUEUES && i < h.num_vqs; i++) {
        vqs_[i].num    = h.vq_state[i].num;
        vqs_[i].ready  = h.vq_state[i].ready;
        vqs_[i].desc   = h.vq_state[i].desc;
        vqs_[i].driver = h.vq_state[i].driver;
        vqs_[i].device = h.vq_state[i].device;
    }

    // Restore virtio-net device state
    net_status_ = h.net_status;
    net_drv_features_ = (uint64_t)h.net_drv_features_lo |
                        ((uint64_t)h.net_drv_features_hi << 32);
    for (uint32_t i = 0; i < NET_NUM_QUEUES && i < h.net_num_vqs; i++) {
        net_vqs_[i].num    = h.net_vq_state[i].num;
        net_vqs_[i].ready  = h.net_vq_state[i].ready;
        net_vqs_[i].desc   = h.net_vq_state[i].desc;
        net_vqs_[i].driver = h.net_vq_state[i].driver;
        net_vqs_[i].device = h.net_vq_state[i].device;
    }

    if (vu_sock_ >= 0 && vdev_status_ & 0x4) {
        virtio_active_ = false;
        vu_setup();
    }

    if (tap_fd_ >= 0 && net_status_ & 0x4) {
        net_active_ = true;
        vhost_net_setup();
    }

    // Ensure serial UART (16550) is ready to handle guest userspace console writes
    uart_ier_ = 0x0f;
    uart_lsr_ = 0x60;
    uart_lcr_ = 0x03;
    uart_mcr_ = 0x0b;

    clock_gettime(CLOCK_MONOTONIC, &ts2);

    auto us_diff = [](struct timespec &a, struct timespec &b) -> long {
        return (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000L;
    };
    DBG("restore_snapshot breakdown: ram_copy=%.2fms kvm_state=%.2fms total=%.2fms",
        us_diff(ts0, ts1) / 1000.0,
        us_diff(ts1, ts2) / 1000.0,
        us_diff(ts0, ts2) / 1000.0);

    ignore_next_signal_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Snapshot -- file I/O (for boot --snapshot)
// ---------------------------------------------------------------------------

bool Vmm::save_snapshot_file(const char *path) {
    Snapshot snap;
    if (!save_snapshot(snap)) return false;

    constexpr size_t PAGE = 4096;
    size_t total_pages = ram_bytes_ / PAGE;
    size_t bm_bytes = (total_pages + 7) / 8;
    std::vector<uint8_t> bitmap(bm_bytes, 0);
    const uint8_t *ram = (const uint8_t *)snap.ram;
    size_t dirty_count = 0;
    for (size_t pg = 0; pg < total_pages; pg++) {
        const uint64_t *p = (const uint64_t *)(ram + pg * PAGE);
        uint64_t acc = 0;
        for (size_t w = 0; w < PAGE / sizeof(uint64_t); w += 8)
            acc |= p[w] | p[w+1] | p[w+2] | p[w+3] |
                   p[w+4] | p[w+5] | p[w+6] | p[w+7];
        if (acc) { bitmap[pg / 8] |= (1 << (pg & 7)); dirty_count++; }
    }
    snap.hdr.bitmap_bytes = bm_bytes;

    printf("[VMM] snapshot: %zu/%zu pages dirty (%.0f%%)\n",
           dirty_count, total_pages, dirty_count * 100.0 / total_pages);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open snap"); return false; }

    ssize_t nw;
    nw = write(fd, &snap.hdr, sizeof(snap.hdr)); (void)nw;
    nw = write(fd, snap.xsave, snap.hdr.xsave_size); (void)nw;
    nw = write(fd, snap.cpuid.data(), snap.cpuid.size() * sizeof(kvm_cpuid_entry2)); (void)nw;
    nw = write(fd, snap.msrs.data(), snap.msrs.size() * sizeof(kvm_msr_entry)); (void)nw;
    nw = write(fd, bitmap.data(), bm_bytes); (void)nw;

    size_t off = 0;
    while (off < ram_bytes_) {
        ssize_t n = write(fd, (uint8_t *)snap.ram + off, ram_bytes_ - off);
        if (n < 0) { perror("write"); close(fd); munmap(snap.ram, ram_bytes_); return false; }
        off += n;
    }
    close(fd);
    munmap(snap.ram, ram_bytes_);
    snap.ram = nullptr;
    return true;
}

bool Vmm::restore_snapshot_file(const char *path) {
    struct timespec tf0, tf1, tf2, tf3;
    clock_gettime(CLOCK_MONOTONIC, &tf0);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open snap"); return false; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat snap"); close(fd); return false; }
    void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap snap"); close(fd); return false; }

    madvise(map, st.st_size, MADV_WILLNEED);

    clock_gettime(CLOCK_MONOTONIC, &tf1);

    const uint8_t *p = (const uint8_t *)map;
    size_t remain = st.st_size;

    Snapshot snap;
    if (remain < sizeof(snap.hdr)) { munmap(map, st.st_size); close(fd); return false; }
    memcpy(&snap.hdr, p, sizeof(snap.hdr));
    p += sizeof(snap.hdr); remain -= sizeof(snap.hdr);

    if (remain < snap.hdr.xsave_size) { munmap(map, st.st_size); close(fd); return false; }
    memcpy(snap.xsave, p, snap.hdr.xsave_size);
    p += snap.hdr.xsave_size; remain -= snap.hdr.xsave_size;

    size_t csz = snap.hdr.cpuid_nent * sizeof(kvm_cpuid_entry2);
    if (remain < csz) { munmap(map, st.st_size); close(fd); return false; }
    snap.cpuid.resize(snap.hdr.cpuid_nent);
    memcpy(snap.cpuid.data(), p, csz);
    p += csz; remain -= csz;

    size_t msz = snap.hdr.num_msrs * sizeof(kvm_msr_entry);
    if (remain < msz) { munmap(map, st.st_size); close(fd); return false; }
    snap.msrs.resize(snap.hdr.num_msrs);
    memcpy(snap.msrs.data(), p, msz);
    p += msz; remain -= msz;

    if (snap.hdr.bitmap_bytes > 0) {
        if (remain < snap.hdr.bitmap_bytes) { munmap(map, st.st_size); close(fd); return false; }
        snap.dirty_bitmap.resize(snap.hdr.bitmap_bytes);
        memcpy(snap.dirty_bitmap.data(), p, snap.hdr.bitmap_bytes);
        p += snap.hdr.bitmap_bytes; remain -= snap.hdr.bitmap_bytes;
    }

    snap.ram_size = snap.hdr.ram_mb * 1024ULL * 1024;
    if (remain < snap.ram_size) { munmap(map, st.st_size); close(fd); return false; }
    snap.ram = (void *)p;

    snap_mmap_base_ = map;
    snap_mmap_sz_ = st.st_size;
    snap_fd_ = fd;
    snap_ram_ = (const uint8_t *)p;
    snap_bitmap_buf_ = snap.dirty_bitmap;
    snap_total_pages_ = snap.ram_size / 4096;

    clock_gettime(CLOCK_MONOTONIC, &tf2);

    bool ok = restore_snapshot(snap);

    clock_gettime(CLOCK_MONOTONIC, &tf3);

    auto us_diff = [](struct timespec &a, struct timespec &b) -> long {
        return (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000L;
    };
    DBG("restore_file breakdown: mmap=%.2fms parse=%.2fms restore=%.2fms total=%.2fms",
        us_diff(tf0, tf1) / 1000.0,
        us_diff(tf1, tf2) / 1000.0,
        us_diff(tf2, tf3) / 1000.0,
        us_diff(tf0, tf3) / 1000.0);

    snap.ram = nullptr;
    snap.ram_size = 0;
    if (!use_uffd_ || uffd_ < 0) {
        munmap(map, st.st_size);
        close(fd);
        snap_mmap_base_ = nullptr;
        snap_mmap_sz_ = 0;
        snap_fd_ = -1;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void Vmm::cleanup() {
    net_thread_stop_ = true;
    if (net_wakeup_fd_ >= 0) {
        uint64_t one = 1;
        ssize_t nw = ::write(net_wakeup_fd_, &one, sizeof(one));
        (void)nw;
        if (net_thread_running_) {
            pthread_join(net_thread_, nullptr);
            net_thread_running_ = false;
        }
        close(net_wakeup_fd_);
        net_wakeup_fd_ = -1;
    }
    if (vhost_fd_ >= 0) close(vhost_fd_);
    if (tap_fd_ >= 0) close(tap_fd_);
    for (auto &q : net_vqs_) {
        if (q.kick_fd >= 0) close(q.kick_fd);
        if (q.call_fd >= 0) close(q.call_fd);
    }

    if (irq_thread_running_.load()) {
        irq_thread_running_.store(false);
        if (irq_wakeup_fd_ >= 0) {
            uint64_t one = 1;
            ssize_t nw = ::write(irq_wakeup_fd_, &one, sizeof(one));
            (void)nw;
        }
        pthread_join(irq_thread_, nullptr);
    }
    if (irq_wakeup_fd_ >= 0) close(irq_wakeup_fd_);

    if (uffd_running_.load()) {
        uffd_running_.store(false);
        if (uffd_wakeup_fd_ >= 0) {
            uint64_t one = 1;
            ssize_t nw = ::write(uffd_wakeup_fd_, &one, sizeof(one));
            (void)nw;
        }
        pthread_join(uffd_thread_, nullptr);
        uffd_thread_ = 0;
    }
    if (uffd_wakeup_fd_ >= 0) { close(uffd_wakeup_fd_); uffd_wakeup_fd_ = -1; }
    if (uffd_ >= 0) { close(uffd_); uffd_ = -1; }

    if (snap_mmap_base_ && snap_mmap_sz_ > 0) {
        munmap(snap_mmap_base_, snap_mmap_sz_);
        snap_mmap_base_ = nullptr;
        snap_mmap_sz_ = 0;
    }
    if (snap_fd_ >= 0) {
        close(snap_fd_);
        snap_fd_ = -1;
    }

    if (kvm_run_) munmap(kvm_run_, run_mmap_sz_);
    if (vcpu_fd_ >= 0) close(vcpu_fd_);
    if (vm_fd_ >= 0) close(vm_fd_);
    if (kvm_fd_ >= 0) close(kvm_fd_);
    if (ram_ && ram_ != MAP_FAILED) munmap(ram_, ram_bytes_);
    if (ram_memfd_ >= 0) close(ram_memfd_);
    if (vu_sock_ >= 0) close(vu_sock_);
    if (vu_backend_sock_ >= 0) close(vu_backend_sock_);
    for (auto &q : vqs_) {
        if (q.kick_fd >= 0) close(q.kick_fd);
        if (q.call_fd >= 0) close(q.call_fd);
    }
    free(cpuid_);

    if (virtiofsd_pid_ > 0) {
        kill(virtiofsd_pid_, SIGTERM);
        waitpid(virtiofsd_pid_, nullptr, 0);
    }

    kvm_run_ = nullptr;
    vcpu_fd_ = vm_fd_ = kvm_fd_ = -1;
    ram_ = nullptr;
    ram_memfd_ = -1;
    tap_fd_ = -1;
    vhost_fd_ = -1;
    vu_sock_ = vu_backend_sock_ = -1;
    cpuid_ = nullptr;
    virtiofsd_pid_ = -1;
}

// ---------------------------------------------------------------------------
// main helpers
// ---------------------------------------------------------------------------

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s boot   <bzImage> <initrd> --share <dir> [--snapshot <path>] [--ram <mb>]\n"
        "  %s restore <snapshot> [--share <dir>] --config <json-file>\n"
        "  %s clone   <snapshot> <count> [--share <dir>] --config <json-file>\n"
        "\n"
        "Config JSON format:\n"
        "  {\n"
        "    \"rootfs\": \"/path/to/shared/dir\",\n"
        "    \"entrypoint\": \"/bin/sh\",\n"
        "    \"hostname\": \"myvm\",\n"
        "    \"env\": { \"KEY\": \"VAL\" },\n"
        "    \"net\": {\n"
        "      \"tap\": \"tap0\",\n"
        "      \"ip\": \"10.0.0.2/24\",\n"
        "      \"gateway\": \"10.0.0.1\",\n"
        "      \"mac\": \"52:54:00:12:34:56\"\n"
        "    }\n"
        "  }\n",
        prog, prog, prog);
}

static const char *find_arg(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

static bool has_flag(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], flag) == 0) return true;
    return false;
}

static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *parse_string(const char *p, char *out, size_t maxlen) {
    p = skip_ws(p);
    if (*p != '"') return nullptr;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < maxlen - 1) {
        if (*p == '\\' && *(p+1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static bool parse_mac(const char *s, uint8_t mac[6]) {
    unsigned int m[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
    return true;
}

static bool parse_config(const char *path, VmConfig &cfg) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("open config"); return false; }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    auto find_val = [&](const char *key, char *out, size_t maxlen) -> bool {
        char needle[128];
        snprintf(needle, sizeof(needle), "\"%s\"", key);
        const char *p = strstr(buf, needle);
        if (!p) return false;
        p += strlen(needle);
        p = skip_ws(p);
        if (*p != ':') return false;
        p = skip_ws(p + 1);
        parse_string(p, out, maxlen);
        return out[0] != '\0';
    };

    find_val("rootfs", cfg.rootfs, sizeof(cfg.rootfs));
    find_val("entrypoint", cfg.entrypoint, sizeof(cfg.entrypoint));
    find_val("hostname", cfg.hostname, sizeof(cfg.hostname));

    const char *net = strstr(buf, "\"net\"");
    if (net) {
        const char *p = net + 5;
        p = skip_ws(p);
        if (*p == ':') p = skip_ws(p + 1);
        if (*p == '{') {
            cfg.has_net = true;
            char mac_str[32] = {};
            const char *end = strchr(p, '}');
            if (!end) end = buf + n;
            size_t block_len = end - p + 1;
            char net_block[2048];
            if (block_len < sizeof(net_block)) {
                memcpy(net_block, p, block_len);
                net_block[block_len] = '\0';
                auto find_net_val = [&](const char *key, char *out, size_t maxlen) {
                    char needle2[128];
                    snprintf(needle2, sizeof(needle2), "\"%s\"", key);
                    const char *pp = strstr(net_block, needle2);
                    if (!pp) return;
                    pp += strlen(needle2);
                    pp = skip_ws(pp);
                    if (*pp == ':') pp = skip_ws(pp + 1);
                    parse_string(pp, out, maxlen);
                };
                find_net_val("tap", cfg.tap, sizeof(cfg.tap));
                find_net_val("ip", cfg.ip, sizeof(cfg.ip));
                find_net_val("gateway", cfg.gateway, sizeof(cfg.gateway));
                find_net_val("mac", mac_str, sizeof(mac_str));
                if (mac_str[0]) parse_mac(mac_str, cfg.mac);
            }
        }
    }

    const char *env = strstr(buf, "\"env\"");
    if (env) {
        const char *p = env + 5;
        p = skip_ws(p);
        if (*p == ':') p = skip_ws(p + 1);
        if (*p == '{') {
            p++;
            while (*p && *p != '}' && cfg.num_env < 32) {
                p = skip_ws(p);
                if (*p == '"') {
                    char key[64] = {}, val[256] = {};
                    p = parse_string(p, key, sizeof(key));
                    if (!p) break;
                    p = skip_ws(p);
                    if (*p == ':') p = skip_ws(p + 1);
                    p = parse_string(p, val, sizeof(val));
                    if (!p) break;
                    snprintf(cfg.env[cfg.num_env].key, sizeof(cfg.env[0].key), "%s", key);
                    snprintf(cfg.env[cfg.num_env].val, sizeof(cfg.env[0].val), "%s", val);
                    cfg.num_env++;
                    p = skip_ws(p);
                    if (*p == ',') p++;
                } else {
                    p++;
                }
            }
        }
    }

    return true;
}

static void write_config_to_share(const char *share_dir, const VmConfig &cfg) {
    if (!share_dir) return;

    if (cfg.entrypoint[0]) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/.entrypoint", share_dir);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t nw;
            nw = ::write(fd, cfg.entrypoint, strlen(cfg.entrypoint)); (void)nw;
            nw = ::write(fd, "\n", 1); (void)nw;
            close(fd);
        }
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.vmconfig", share_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (cfg.hostname[0])
        fprintf(f, "HOSTNAME=%s\n", cfg.hostname);
    if (cfg.ip[0])
        fprintf(f, "NET_IP=%s\n", cfg.ip);
    if (cfg.gateway[0])
        fprintf(f, "NET_GW=%s\n", cfg.gateway);
    if (cfg.has_net)
        fprintf(f, "NET_MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                cfg.mac[0], cfg.mac[1], cfg.mac[2],
                cfg.mac[3], cfg.mac[4], cfg.mac[5]);
    for (int i = 0; i < cfg.num_env; i++) {
        fprintf(f, "export %s=\"%s\"\n", cfg.env[i].key, cfg.env[i].val);
        fprintf(f, "ENV_%s=\"%s\"\n", cfg.env[i].key, cfg.env[i].val);
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) { g_debug = true; break; }
    }

    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];

    // ── boot ──
    if (strcmp(cmd, "boot") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        const char *kernel    = argv[2];
        const char *initrd    = argv[3];
        const char *share_dir = find_arg(argc, argv, "--share");
        const char *snap_path = find_arg(argc, argv, "--snapshot");
        const char *ram_str   = find_arg(argc, argv, "--ram");

        size_t ram_mb = ram_str ? (size_t)atoi(ram_str) : 64;
        Vmm vmm(ram_mb);
        // Optimization: skip manual zeroing -- memfd memory is kernel-zeroed on fault
        if (!vmm.init(false)) return 1;

        if (share_dir) {
            if (!vmm.start_virtiofsd(share_dir)) return 1;
        }

        vmm.setup_virtio_fs(true);

        uint8_t boot_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        vmm.setup_virtio_net(boot_mac, true);

        if (!vmm.setup_bios() || !vmm.load_bzimage(kernel) ||
            !vmm.load_initrd(initrd) || !vmm.setup_cpu())
            return 1;

        printf("[VMM] booting...\n");
        int rc = vmm.run();

        if (rc == 1 && snap_path) {
            printf("[VMM] guest signalled snapshot\n");
            if (!vmm.save_snapshot_file(snap_path)) return 1;
            printf("[VMM] snapshot saved to %s\n", snap_path);
            printf("[VMM] continuing...\n");
            rc = vmm.run();
        } else if (rc == 1) {
            printf("[VMM] guest signalled snapshot (no --snapshot path, ignoring)\n");
            rc = vmm.run();
        }
        return (rc >= 0) ? 0 : 1;
    }

    // ── restore ──
    if (strcmp(cmd, "restore") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        const char *snap_path   = argv[2];
        const char *share_dir   = find_arg(argc, argv, "--share");
        const char *config_path = find_arg(argc, argv, "--config");
        const char *ct_str      = find_arg(argc, argv, "--copy-threads");
        const char *entrypoint  = find_arg(argc, argv, "--entrypoint");

        VmConfig cfg = {};
        if (config_path) {
            if (!parse_config(config_path, cfg)) return 1;
        } else if (entrypoint) {
            strncpy(cfg.entrypoint, entrypoint, sizeof(cfg.entrypoint) - 1);
        }

        if (!share_dir && cfg.rootfs[0])
            share_dir = cfg.rootfs;

        write_config_to_share(share_dir, cfg);

        auto us_since = [](struct timespec &base) -> long {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            return (now.tv_sec - base.tv_sec) * 1000000L +
                   (now.tv_nsec - base.tv_nsec) / 1000L;
        };
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        // Read snapshot header to support arbitrary memory SKUs
        size_t snap_ram_mb = 64;
        {
            int sfd = open(snap_path, O_RDONLY);
            if (sfd >= 0) {
                SnapshotHeader hdr;
                if (::read(sfd, &hdr, sizeof(hdr)) == sizeof(hdr) && hdr.magic == SNAP_MAGIC) {
                    snap_ram_mb = hdr.ram_mb;
                }
                close(sfd);
            }
        }

        Vmm vmm(snap_ram_mb);
        vmm.set_cmd_start(t0);
        if (ct_str) vmm.set_copy_threads(atoi(ct_str));
        if (has_flag(argc, argv, "--hugetlb")) vmm.set_hugetlb(true);
        if (has_flag(argc, argv, "--no-uffd")) vmm.set_uffd(false);
        else vmm.set_uffd(true);
        if (!vmm.init(false)) return 1;
        long us_init = us_since(t0);

        long us_virtiofsd = 0;
        if (share_dir) {
            if (!vmm.start_virtiofsd(share_dir)) return 1;
            vmm.setup_virtio_fs();
            us_virtiofsd = us_since(t0) - us_init;
        }

        uint8_t restore_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        if (cfg.has_net && cfg.mac[0])
            memcpy(restore_mac, cfg.mac, 6);
        vmm.setup_virtio_net(restore_mac, false);

        if (cfg.has_net && cfg.tap[0]) {
            if (!vmm.connect_tap(cfg.tap)) return 1;
        }
        long us_setup = us_since(t0);

        if (!vmm.restore_snapshot_file(snap_path)) return 1;
        long us_snap = us_since(t0) - us_setup;

        if (vmm.timing_entrypoint())
            printf("[+%6.2fms] ", vmm.ms_since_start());
        printf("[VMM] restored in %.1fms  kvm_init=%.1fms virtiofsd=%.1fms tap=%.1fms snap=%.1fms (uffd=%d)\n",
               us_since(t0) / 1000.0,
               us_init / 1000.0,
               us_virtiofsd / 1000.0,
               (us_setup - us_init - us_virtiofsd) / 1000.0,
               us_snap / 1000.0,
               vmm.use_uffd() ? 1 : 0);

        struct timespec t_run;
        clock_gettime(CLOCK_MONOTONIC, &t_run);
        vmm.set_vcpu_start(t_run);
        return (vmm.run() >= 0) ? 0 : 1;
    }

    // ── clone ──
    if (strcmp(cmd, "clone") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        const char *snap_path   = argv[2];
        int count = atoi(argv[3]);
        const char *share_dir   = find_arg(argc, argv, "--share");
        const char *config_path = find_arg(argc, argv, "--config");
        const char *entrypoint  = find_arg(argc, argv, "--entrypoint");

        VmConfig cfg = {};
        if (config_path) {
            if (!parse_config(config_path, cfg)) return 1;
        } else if (entrypoint) {
            strncpy(cfg.entrypoint, entrypoint, sizeof(cfg.entrypoint) - 1);
        }

        if (!share_dir && cfg.rootfs[0])
            share_dir = cfg.rootfs;

        write_config_to_share(share_dir, cfg);

        Snapshot golden;
        {
            int fd = open(snap_path, O_RDONLY);
            if (fd < 0) { perror("open snap"); return 1; }
            if (::read(fd, &golden.hdr, sizeof(golden.hdr)) != sizeof(golden.hdr)) {
                perror("read hdr"); close(fd); return 1;
            }
            ssize_t nr;
            nr = ::read(fd, golden.xsave, golden.hdr.xsave_size); (void)nr;
            golden.cpuid.resize(golden.hdr.cpuid_nent);
            nr = ::read(fd, golden.cpuid.data(),
                        golden.hdr.cpuid_nent * sizeof(kvm_cpuid_entry2)); (void)nr;
            golden.msrs.resize(golden.hdr.num_msrs);
            nr = ::read(fd, golden.msrs.data(),
                        golden.hdr.num_msrs * sizeof(kvm_msr_entry)); (void)nr;
            if (golden.hdr.bitmap_bytes > 0) {
                golden.dirty_bitmap.resize(golden.hdr.bitmap_bytes);
                nr = ::read(fd, golden.dirty_bitmap.data(), golden.hdr.bitmap_bytes); (void)nr;
            }
            golden.ram_size = golden.hdr.ram_mb * 1024ULL * 1024;
            golden.ram = mmap(nullptr, golden.ram_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            size_t rd = 0;
            while (rd < golden.ram_size) {
                ssize_t n = ::read(fd, (uint8_t *)golden.ram + rd,
                                   golden.ram_size - rd);
                if (n <= 0) break;
                rd += n;
            }
            close(fd);
        }

        printf("[VMM] cloning %d instances from %s\n", count, snap_path);

        for (int i = 0; i < count; i++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); return 1; }
            if (pid == 0) {
                Vmm vmm(golden.hdr.ram_mb);
                if (!vmm.init(false)) _exit(1);

                if (share_dir) {
                    if (!vmm.start_virtiofsd(share_dir)) _exit(1);
                    vmm.setup_virtio_fs();
                }

                uint8_t clone_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
                if (cfg.has_net) {
                    memcpy(clone_mac, cfg.mac, 6);
                    clone_mac[5] = (uint8_t)(cfg.mac[5] + i);

                    char clone_tap[IFNAMSIZ];
                    snprintf(clone_tap, sizeof(clone_tap), "tap%d", i);

                    if (share_dir) {
                        VmConfig clone_cfg = cfg;
                        snprintf(clone_cfg.tap, sizeof(clone_cfg.tap), "%s", clone_tap);
                        memcpy(clone_cfg.mac, clone_mac, 6);
                        unsigned a,b,c,d,prefix;
                        if (sscanf(cfg.ip, "%u.%u.%u.%u/%u", &a,&b,&c,&d,&prefix) == 5) {
                            snprintf(clone_cfg.ip, sizeof(clone_cfg.ip),
                                     "%u.%u.%u.%u/%u", a, b, c, d + i, prefix);
                        }
                        write_config_to_share(share_dir, clone_cfg);
                    }

                    if (!vmm.connect_tap(clone_tap)) {
                        fprintf(stderr, "[VMM clone %d] TAP setup failed\n", i);
                    }
                }

                vmm.setup_virtio_net(clone_mac, false);

                if (!vmm.restore_snapshot(golden)) _exit(1);

                printf("[VMM clone %d] running\n", i);
                int rc = vmm.run();
                printf("[VMM clone %d] exited (%d)\n", i, rc);
                _exit((rc >= 0) ? 0 : 1);
            }
        }

        for (int i = 0; i < count; i++) {
            int status;
            wait(&status);
        }
        munmap(golden.ram, golden.ram_size);
        printf("[VMM] all clones finished\n");
        return 0;
    }

    usage(argv[0]);
    return 1;
}
