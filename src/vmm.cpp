// vmm.cpp -- Minimal KVM VMM with in-memory snapshots, virtiofs, and cloning
//
// Usage:
//   vmtainer boot   <bzImage> <initrd> --share <dir> [--snapshot <path>]
//   vmtainer restore <snapshot>
//   vmtainer clone   <snapshot> <count> --share <dir>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <linux/limits.h>
#include <linux/kvm.h>
#include <asm/bootparam.h>

#include "boot.hpp"
#include "bios_offsets.h"
#include "bios_rom.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// MMIO device for guest -> VMM snapshot signalling.
// The guest writes magic to this GPA; since no memslot covers it KVM traps.
static constexpr uint64_t MMIO_SIGNAL_GPA   = 0xd0000000ULL;
static constexpr uint32_t MMIO_SIGNAL_MAGIC = 0x48594C54U; // "HYLT"

// Virtio-MMIO region for the virtiofs device.
// The kernel discovers it via the command line: virtio_mmio.device=...
static constexpr uint64_t VIRTIO_MMIO_GPA  = 0xd0001000ULL;
static constexpr uint64_t VIRTIO_MMIO_SIZE = 0x1000;
static constexpr uint32_t VIRTIO_MMIO_IRQ  = 5;

// Virtio device / vendor IDs
static constexpr uint32_t VIRTIO_DEV_FS     = 26;  // virtio type for virtiofs
static constexpr uint32_t VIRTIO_VENDOR_ID  = 0x554d4551; // "QEMU"

// Virtio MMIO register offsets (virtio 1.0+ MMIO transport, spec 4.2.2)
enum VirtioMmioReg : uint32_t {
    VIRTIO_MMIO_MAGIC_VALUE       = 0x000,
    VIRTIO_MMIO_VERSION           = 0x004,
    VIRTIO_MMIO_DEVICE_ID         = 0x008,
    VIRTIO_MMIO_VENDOR_ID         = 0x00c,
    VIRTIO_MMIO_DEVICE_FEATURES   = 0x010,
    VIRTIO_MMIO_DEVICE_FEATURES_SEL = 0x014,
    VIRTIO_MMIO_DRIVER_FEATURES   = 0x020,
    VIRTIO_MMIO_DRIVER_FEATURES_SEL = 0x024,
    VIRTIO_MMIO_QUEUE_SEL         = 0x030,
    VIRTIO_MMIO_QUEUE_NUM_MAX     = 0x034,
    VIRTIO_MMIO_QUEUE_NUM         = 0x038,
    VIRTIO_MMIO_QUEUE_READY       = 0x044,
    VIRTIO_MMIO_QUEUE_NOTIFY      = 0x050,
    VIRTIO_MMIO_INTERRUPT_STATUS  = 0x060,
    VIRTIO_MMIO_INTERRUPT_ACK     = 0x064,
    VIRTIO_MMIO_STATUS            = 0x070,
    VIRTIO_MMIO_QUEUE_DESC_LOW    = 0x080,
    VIRTIO_MMIO_QUEUE_DESC_HIGH   = 0x084,
    VIRTIO_MMIO_QUEUE_DRIVER_LOW  = 0x090,
    VIRTIO_MMIO_QUEUE_DRIVER_HIGH = 0x094,
    VIRTIO_MMIO_QUEUE_DEVICE_LOW  = 0x0a0,
    VIRTIO_MMIO_QUEUE_DEVICE_HIGH = 0x0a4,
    VIRTIO_MMIO_SHM_SEL          = 0x0ac,
    VIRTIO_MMIO_SHM_LEN_LOW      = 0x0b0,
    VIRTIO_MMIO_SHM_LEN_HIGH     = 0x0b4,
    VIRTIO_MMIO_SHM_BASE_LOW     = 0x0b8,
    VIRTIO_MMIO_SHM_BASE_HIGH    = 0x0bc,
    VIRTIO_MMIO_CONFIG_GEN        = 0x0fc,
    VIRTIO_MMIO_CONFIG            = 0x100, // device-specific config starts here
};

// Virtio feature bits
static constexpr uint64_t VIRT_F_VERSION_1    = (1ULL << 32);
static constexpr uint64_t VIRT_F_RING_PACKED  = (1ULL << 34);

// Virtiofs config: just a tag (up to 36 bytes)
struct VirtioFsConfig {
    char tag[36];
    uint32_t num_request_queues;
};

// Virtqueue state
struct Virtqueue {
    uint32_t num      = 0;
    uint32_t ready    = 0;
    uint64_t desc     = 0;
    uint64_t driver   = 0;  // avail ring
    uint64_t device   = 0;  // used ring
    int      kick_fd  = -1; // ioeventfd for guest -> host notification
    int      call_fd  = -1; // irqfd for host -> guest notification
};

// ---------------------------------------------------------------------------
// Vhost-user protocol
// ---------------------------------------------------------------------------

// Message types (master -> slave)
enum VhostUserRequest : uint32_t {
    VHOST_USER_GET_FEATURES       = 1,
    VHOST_USER_SET_FEATURES       = 2,
    VHOST_USER_SET_OWNER          = 3,
    VHOST_USER_SET_MEM_TABLE      = 5,
    VHOST_USER_SET_VRING_NUM      = 8,
    VHOST_USER_SET_VRING_ADDR     = 9,
    VHOST_USER_SET_VRING_BASE     = 10,
    VHOST_USER_GET_VRING_BASE     = 12,
    VHOST_USER_SET_VRING_KICK     = 12,
    VHOST_USER_SET_VRING_CALL     = 13,
    VHOST_USER_SET_VRING_ENABLE   = 18,
    VHOST_USER_GET_PROTOCOL_FEATURES = 15,
    VHOST_USER_SET_PROTOCOL_FEATURES = 16,
    VHOST_USER_GET_QUEUE_NUM      = 17,
    VHOST_USER_SET_SLAVE_REQ_FD   = 21,
    VHOST_USER_SET_VRING_KICK_ACTUAL = 12,
};

// Corrected request numbers (the enum above has conflicts, use raw values)
static constexpr uint32_t VU_GET_FEATURES          = 1;
static constexpr uint32_t VU_SET_FEATURES          = 2;
static constexpr uint32_t VU_SET_OWNER             = 3;
static constexpr uint32_t VU_SET_MEM_TABLE         = 5;
static constexpr uint32_t VU_SET_VRING_NUM         = 8;
static constexpr uint32_t VU_SET_VRING_ADDR        = 9;
static constexpr uint32_t VU_SET_VRING_BASE        = 10;
static constexpr uint32_t VU_GET_VRING_BASE        = 11;
static constexpr uint32_t VU_SET_VRING_KICK        = 12;
static constexpr uint32_t VU_SET_VRING_CALL        = 13;
static constexpr uint32_t VU_GET_PROTOCOL_FEATURES = 15;
static constexpr uint32_t VU_SET_PROTOCOL_FEATURES = 16;
static constexpr uint32_t VU_GET_QUEUE_NUM         = 17;
static constexpr uint32_t VU_SET_VRING_ENABLE      = 18;
static constexpr uint32_t VU_SET_BACKEND_REQ_FD    = 21;

// Vhost-user protocol feature bits
static constexpr uint64_t VU_PROTO_F_MQ              = (1ULL << 0);
static constexpr uint64_t VU_PROTO_F_REPLY_ACK       = (1ULL << 3);
static constexpr uint64_t VU_PROTO_F_BACKEND_REQ     = (1ULL << 5);
static constexpr uint64_t VU_PROTO_F_CONFIG          = (1ULL << 9);
static constexpr uint64_t VU_PROTO_F_BACKEND_SEND_FD = (1ULL << 10);

// Vhost-user message header (must be packed -- no padding before payload)
struct __attribute__((packed)) VhostUserMsgHdr {
    uint32_t request;
    uint32_t flags;
    uint32_t size;
};

#define VHOST_USER_HDR_SIZE sizeof(VhostUserMsgHdr)
#define VHOST_USER_FLAG_REPLY     (1u << 2)
#define VHOST_USER_FLAG_NEED_REPLY (1u << 3)
#define VHOST_USER_VERSION        1

// Max 8 memory regions
struct VhostUserMemRegion {
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;
    uint64_t mmap_offset;
};

struct VhostUserMemMsg {
    uint32_t nregions;
    uint32_t padding;
    VhostUserMemRegion regions[8];
};

struct VhostUserVringAddr {
    uint32_t index;
    uint32_t flags;
    uint64_t desc_user_addr;
    uint64_t used_user_addr;
    uint64_t avail_user_addr;
    uint64_t log_guest_addr;
};

struct VhostUserVringState {
    uint32_t index;
    uint32_t num;
};

// Full message (header + payload) -- packed to avoid padding between hdr and payload
struct __attribute__((packed)) VhostUserMsg {
    VhostUserMsgHdr hdr;
    union {
        uint64_t            u64;
        VhostUserVringState vring_state;
        VhostUserVringAddr  vring_addr;
        VhostUserMemMsg     mem;
        uint8_t             raw[4096];
    } payload;
};

// ---------------------------------------------------------------------------
// Snapshot header -- all KVM state for one vCPU + VM
// ---------------------------------------------------------------------------

#define SNAP_MAGIC   0x48594C54534E4150ULL
#define SNAP_VERSION 5
#define MAX_MSRS     256
#define XSAVE_SIZE   8192

struct SnapshotHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t ram_mb;

    struct kvm_regs         regs;
    struct kvm_sregs        sregs;
    struct kvm_lapic_state  lapic;
    struct kvm_vcpu_events  events;
    struct kvm_xcrs         xcrs;
    struct kvm_mp_state     mp_state;
    struct kvm_debugregs    debugregs;

    struct kvm_clock_data   clock;
    struct kvm_pit_state2   pit;
    struct kvm_irqchip      pic_master;
    struct kvm_irqchip      pic_slave;
    struct kvm_irqchip      ioapic;

    uint32_t tsc_khz;
    uint32_t cpuid_nent;
    uint32_t num_msrs;
    uint32_t xsave_size;

    // Virtio device state for virtiofs
    uint32_t vdev_status;
    uint32_t vdrv_features_lo;
    uint32_t vdrv_features_hi;
    uint32_t num_vqs;
    struct {
        uint32_t num;
        uint32_t ready;
        uint64_t desc;
        uint64_t driver;
        uint64_t device;
    } vq_state[2]; // hiprio + request queue

    // Followed in memory / on disk by:
    //   xsave_buf[xsave_size]
    //   kvm_cpuid_entry2[cpuid_nent]
    //   kvm_msr_entry[num_msrs]
    //   guest_ram[ram_mb * 1M]
};

// ---------------------------------------------------------------------------
// In-memory snapshot blob
// ---------------------------------------------------------------------------

struct Snapshot {
    SnapshotHeader          hdr;
    uint8_t                 xsave[XSAVE_SIZE] __attribute__((aligned(64)));
    std::vector<kvm_cpuid_entry2> cpuid;
    std::vector<kvm_msr_entry>    msrs;
    void                   *ram      = nullptr;  // mmap'd RAM copy
    size_t                  ram_size = 0;
};

// ---------------------------------------------------------------------------
// VMM class
// ---------------------------------------------------------------------------

class Vmm {
public:
    explicit Vmm(size_t ram_mb) : ram_bytes_(ram_mb << 20), ram_mb_(ram_mb) {}
    ~Vmm() { cleanup(); }

    // Setup
    bool init();
    bool setup_bios();
    bool load_bzimage(const char *path);
    bool load_initrd(const char *path);
    bool setup_cpu();
    void set_shared_dir(const char *dir) { shared_dir_ = dir; }

    // Virtio-fs
    bool start_virtiofsd(const char *shared_dir);
    bool setup_virtio_fs();

    // Run
    int run();  // 0 = halt, 1 = snapshot signal, -1 = error

    // Snapshots (in-memory)
    bool save_snapshot(Snapshot &snap);
    bool restore_snapshot(const Snapshot &snap);
    bool save_snapshot_file(const char *path);
    bool restore_snapshot_file(const char *path);

private:
    void cleanup();
    bool set_memslot(uint32_t slot, uint64_t gpa, void *hva, size_t len);
    bool query_msr_list();
    int  read_file(const char *path, void **buf, size_t *len);
    uint8_t *gpa_ptr(uint64_t gpa) { return (uint8_t *)ram_ + gpa; }

    // BIOS helpers
    void setup_e820();
    void setup_vga_rom();
    void install_irq(uint16_t vec, unsigned long addr,
                     const void *code, size_t size);

    // Serial
    void serial_in(uint16_t port, uint8_t *data);
    void serial_out(uint16_t port, uint8_t data);

    // Virtio MMIO
    void virtio_mmio_read(uint64_t off, uint8_t *data, uint32_t len);
    void virtio_mmio_write(uint64_t off, const uint8_t *data, uint32_t len);
    void virtio_kick(uint32_t qidx);
    void check_virtio_irqs();
    void start_irq_thread();
    static void *irq_thread_func(void *arg);

    // Vhost-user
    bool vu_connect(const char *sock_path);
    bool vu_early_init();
    bool vu_setup();
    bool vu_send(uint32_t req, const void *payload, uint32_t sz,
                 const int *fds = nullptr, int nfds = 0);
    bool vu_recv(VhostUserMsg &msg);
    bool vu_transact(uint32_t req, const void *payload, uint32_t sz,
                     VhostUserMsg &reply,
                     const int *fds = nullptr, int nfds = 0);

    // KVM / VM state
    int kvm_fd_ = -1, vm_fd_ = -1, vcpu_fd_ = -1;
    void *ram_ = nullptr;
    size_t ram_bytes_, ram_mb_;
    struct kvm_run *kvm_run_ = nullptr;
    size_t run_mmap_sz_ = 0;
    struct kvm_cpuid2 *cpuid_ = nullptr;
    uint32_t msr_list_[MAX_MSRS];
    uint32_t num_msrs_ = 0;

    // Command line -- extended when virtio-mmio device is added
    char cmdline_[2048] = "console=ttyS0,115200n8 earlyprintk=serial,0x3f8,115200"
                          " nokaslr";

    // BIOS / IVT
    struct real_intr_desc ivt_[REAL_INTR_VECTORS];

    // Serial (16550) state
    uint8_t uart_ier_ = 0, uart_lcr_ = 0x03, uart_mcr_ = 0;
    uint8_t uart_lsr_ = 0x60, uart_msr_ = 0xb0, uart_scr_ = 0;
    uint8_t uart_dll_ = 0x01, uart_dlh_ = 0, uart_fcr_ = 0;

    // Virtio-fs device state
    static constexpr int NUM_QUEUES = 2; // hiprio + request
    Virtqueue  vqs_[NUM_QUEUES];
    uint32_t   vdev_features_sel_ = 0;
    uint32_t   vdrv_features_sel_ = 0;
    uint64_t   vdrv_features_     = 0;
    uint32_t   vdev_status_       = 0;
    uint32_t   vqueue_sel_        = 0;
    uint32_t   virq_status_       = 0;
    uint32_t   shm_sel_           = 0;
    VirtioFsConfig  fs_config_    = {};
    bool       virtio_active_     = false;

    // IRQ thread: monitors call_fds and interrupts KVM_RUN
    pthread_t  irq_thread_       = 0;
    pthread_t  vcpu_thread_      = 0;
    std::atomic<bool> irq_thread_running_{false};
    int        irq_wakeup_fd_    = -1; // eventfd to wake up the irq thread

    // Vhost-user connection
    int vu_sock_   = -1;
    int vu_backend_sock_ = -1;
    uint64_t vu_features_ = 0;
    uint64_t vu_proto_features_ = 0;

    // virtiofsd process
    pid_t virtiofsd_pid_ = -1;
    const char *shared_dir_ = nullptr;

    // Snapshot signal state
    bool ignore_next_signal_ = false;

    // Guest RAM backing fd (memfd) for sharing with virtiofsd
    int ram_memfd_ = -1;
};

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

bool Vmm::init() {
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

    // Allocate guest RAM via memfd so we can share it with virtiofsd
    ram_memfd_ = memfd_create("guest_ram", MFD_CLOEXEC);
    if (ram_memfd_ < 0) { perror("memfd_create"); return false; }
    if (ftruncate(ram_memfd_, ram_bytes_) < 0) {
        perror("ftruncate memfd"); return false;
    }
    ram_ = mmap(nullptr, ram_bytes_, PROT_READ | PROT_WRITE,
                MAP_SHARED, ram_memfd_, 0);
    if (ram_ == MAP_FAILED) { perror("mmap ram"); return false; }
    memset(ram_, 0, ram_bytes_);

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
        putchar(data);
        fflush(stdout);
        if (uart_ier_ & 0x02) {
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
// Virtio MMIO transport
// ---------------------------------------------------------------------------

void Vmm::virtio_mmio_read(uint64_t off, uint8_t *data, uint32_t len) {
    uint32_t val = 0;

    switch (off) {
    case VIRTIO_MMIO_MAGIC_VALUE:  val = 0x74726976; break; // "virt"
    case VIRTIO_MMIO_VERSION:      val = 2;          break; // modern
    case VIRTIO_MMIO_DEVICE_ID:    val = VIRTIO_DEV_FS; break;
    case VIRTIO_MMIO_VENDOR_ID:    val = VIRTIO_VENDOR_ID; break;

    case VIRTIO_MMIO_DEVICE_FEATURES:
        if (vdev_features_sel_ == 0)
            val = (uint32_t)(vu_features_ & 0xFFFFFFFF);
        else if (vdev_features_sel_ == 1)
            val = (uint32_t)(vu_features_ >> 32);
        break;

    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        val = 1024;
        break;

    case VIRTIO_MMIO_QUEUE_READY:
        if (vqueue_sel_ < NUM_QUEUES)
            val = vqs_[vqueue_sel_].ready;
        break;

    case VIRTIO_MMIO_INTERRUPT_STATUS:
        val = virq_status_;
        break;

    case VIRTIO_MMIO_STATUS:
        val = vdev_status_;
        break;

    case VIRTIO_MMIO_CONFIG_GEN:
        val = 0;
        break;

    case VIRTIO_MMIO_SHM_LEN_LOW:
    case VIRTIO_MMIO_SHM_LEN_HIGH:
    case VIRTIO_MMIO_SHM_BASE_LOW:
    case VIRTIO_MMIO_SHM_BASE_HIGH:
        // DAX window not supported -- return -1 per spec
        val = 0xFFFFFFFF;
        break;

    default:
        // Config space reads
        if (off >= VIRTIO_MMIO_CONFIG && off < VIRTIO_MMIO_CONFIG + sizeof(fs_config_)) {
            uint32_t cfg_off = off - VIRTIO_MMIO_CONFIG;
            memcpy(&val, (uint8_t *)&fs_config_ + cfg_off,
                   (len < 4) ? len : 4);
        }
        break;
    }

    memcpy(data, &val, (len < 4) ? len : 4);
}

void Vmm::virtio_mmio_write(uint64_t off, const uint8_t *data, uint32_t len) {
    uint32_t val = 0;
    memcpy(&val, data, (len < 4) ? len : 4);

    switch (off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        vdev_features_sel_ = val;
        break;

    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        vdrv_features_sel_ = val;
        break;

    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (vdrv_features_sel_ == 0)
            vdrv_features_ = (vdrv_features_ & 0xFFFFFFFF00000000ULL) | val;
        else if (vdrv_features_sel_ == 1)
            vdrv_features_ = (vdrv_features_ & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
        break;

    case VIRTIO_MMIO_QUEUE_SEL:
        vqueue_sel_ = val;
        break;

    case VIRTIO_MMIO_QUEUE_NUM:
        if (vqueue_sel_ < NUM_QUEUES)
            vqs_[vqueue_sel_].num = val;
        break;

    case VIRTIO_MMIO_QUEUE_READY:
        if (vqueue_sel_ < NUM_QUEUES)
            vqs_[vqueue_sel_].ready = val;
        break;

    case VIRTIO_MMIO_QUEUE_NOTIFY:
        virtio_kick(val);
        break;

    case VIRTIO_MMIO_INTERRUPT_ACK:
        virq_status_ &= ~val;
        if (virq_status_ == 0) {
            // Lower the IRQ line when all interrupts are acknowledged
            struct kvm_irq_level irq = {};
            irq.irq = VIRTIO_MMIO_IRQ;
            irq.level = 0;
            ioctl(vm_fd_, KVM_IRQ_LINE, &irq);
        }
        break;

    case VIRTIO_MMIO_STATUS: {
        uint32_t old = vdev_status_;
        vdev_status_ = val;
        if (val == 0) {
            // Device reset
            for (auto &q : vqs_) { q = {}; }
            vdrv_features_ = 0;
            virq_status_ = 0;
        }
        // DRIVER_OK (bit 2) -- queues are fully configured, wire up vhost-user
        if ((val & 0x4) && !(old & 0x4)) {
            vu_setup();
        }
        break;
    }

    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        if (vqueue_sel_ < NUM_QUEUES)
            vqs_[vqueue_sel_].desc = (vqs_[vqueue_sel_].desc & ~0xFFFFFFFFULL) | val;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        if (vqueue_sel_ < NUM_QUEUES)
            vqs_[vqueue_sel_].desc = (vqs_[vqueue_sel_].desc & 0xFFFFFFFFULL)
                                   | ((uint64_t)val << 32);
        break;

    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        if (vqueue_sel_ < NUM_QUEUES)
            vqs_[vqueue_sel_].driver = (vqs_[vqueue_sel_].driver & ~0xFFFFFFFFULL) | val;
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        if (vqueue_sel_ < NUM_QUEUES)
            vqs_[vqueue_sel_].driver = (vqs_[vqueue_sel_].driver & 0xFFFFFFFFULL)
                                     | ((uint64_t)val << 32);
        break;

    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        if (vqueue_sel_ < NUM_QUEUES)
            vqs_[vqueue_sel_].device = (vqs_[vqueue_sel_].device & ~0xFFFFFFFFULL) | val;
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        if (vqueue_sel_ < NUM_QUEUES)
            vqs_[vqueue_sel_].device = (vqs_[vqueue_sel_].device & 0xFFFFFFFFULL)
                                     | ((uint64_t)val << 32);
        break;

    case VIRTIO_MMIO_SHM_SEL:
        shm_sel_ = val;
        break;

    default:
        break;
    }
}

void Vmm::virtio_kick(uint32_t qidx) {
    if (qidx >= NUM_QUEUES) return;
    auto &q = vqs_[qidx];

    // Signal virtiofsd via the kick eventfd
    if (q.kick_fd >= 0) {
        uint64_t one = 1;
        ::write(q.kick_fd, &one, sizeof(one));
    }
}

// Check if virtiofsd has signaled any call_fd, and if so,
// set virq_status and inject an interrupt.
void Vmm::check_virtio_irqs() {
    if (!virtio_active_) return;
    bool need_irq = false;
    for (int i = 0; i < NUM_QUEUES; i++) {
        if (vqs_[i].call_fd >= 0) {
            uint64_t v;
            if (::read(vqs_[i].call_fd, &v, 8) > 0)
                need_irq = true;
        }
    }
    if (need_irq) {
        virq_status_ |= 1; // used buffer notification
        // Inject IRQ via the legacy PIC
        struct kvm_irq_level irq = {};
        irq.irq = VIRTIO_MMIO_IRQ;
        irq.level = 1;
        ioctl(vm_fd_, KVM_IRQ_LINE, &irq);
    }
}

static void sigusr1_handler(int) {
    // No-op: just interrupts KVM_RUN so the run loop can check for IRQs
}

// Background thread that polls call_fds and sends a signal to interrupt
// KVM_RUN when virtiofsd has completed a request.
void *Vmm::irq_thread_func(void *arg) {
    auto *vmm = static_cast<Vmm *>(arg);

    while (vmm->irq_thread_running_.load(std::memory_order_relaxed)) {
        struct pollfd fds[NUM_QUEUES + 1];
        int nfds = 0;

        for (int i = 0; i < NUM_QUEUES; i++) {
            if (vmm->vqs_[i].call_fd >= 0) {
                fds[nfds].fd = vmm->vqs_[i].call_fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                nfds++;
            }
        }
        // Also poll the wakeup fd to allow clean shutdown
        fds[nfds].fd = vmm->irq_wakeup_fd_;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        int ret = poll(fds, nfds, 500); // 500ms timeout

        // Check wakeup fd (shutdown signal)
        if (ret > 0 && fds[nfds - 1].revents & POLLIN) {
            uint64_t v;
            ::read(vmm->irq_wakeup_fd_, &v, 8);
            break;
        }

        // Wake vCPU: either a call_fd has data (virtiofsd completed a request)
        // or the timeout expired (allows the run loop to check for HLT).
        pthread_kill(vmm->vcpu_thread_, SIGUSR1);
    }
    return nullptr;
}

void Vmm::start_irq_thread() {
    if (irq_thread_running_.load()) return;

    // Set up SIGUSR1 handler before starting the thread
    struct sigaction sa = {};
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = 0; // No SA_RESTART -- we want KVM_RUN to return EINTR
    sigaction(SIGUSR1, &sa, nullptr);

    irq_wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    irq_thread_running_.store(true);
    vcpu_thread_ = pthread_self(); // run() is called from this thread
    pthread_create(&irq_thread_, nullptr, irq_thread_func, this);
}

// ---------------------------------------------------------------------------
// Vhost-user protocol
// ---------------------------------------------------------------------------

bool Vmm::vu_connect(const char *sock_path) {
    vu_sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (vu_sock_ < 0) { perror("socket"); return false; }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    // Retry connection (virtiofsd may still be starting)
    for (int i = 0; i < 50; i++) {
        if (connect(vu_sock_, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return true;
        usleep(100000); // 100ms
    }

    perror("connect to virtiofsd");
    return false;
}

bool Vmm::vu_send(uint32_t req, const void *payload, uint32_t sz,
                   const int *fds, int nfds) {
    VhostUserMsg msg = {};
    msg.hdr.request = req;
    msg.hdr.flags   = VHOST_USER_VERSION;
    msg.hdr.size    = sz;
    if (sz > 0) memcpy(&msg.payload, payload, sz);

    struct iovec iov = { &msg, VHOST_USER_HDR_SIZE + sz };
    struct msghdr mh = {};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int) * 8)] = {};
    if (nfds > 0) {
        mh.msg_control = cmsg_buf;
        mh.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
        auto *cmsg = CMSG_FIRSTHDR(&mh);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(nfds * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fds, nfds * sizeof(int));
    }

    ssize_t ret = sendmsg(vu_sock_, &mh, 0);
    if (ret <= 0) {
        fprintf(stderr, "[VMM] vu_send(req=%u) failed: %s\n",
                req, strerror(errno));
        return false;
    }
    return true;
}

bool Vmm::vu_recv(VhostUserMsg &msg) {
    memset(&msg, 0, sizeof(msg));
    struct iovec iov = { &msg, sizeof(msg) };
    struct msghdr mh = {};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    char cmsg_buf[CMSG_SPACE(sizeof(int) * 8)] = {};
    mh.msg_control = cmsg_buf;
    mh.msg_controllen = sizeof(cmsg_buf);

    ssize_t n = recvmsg(vu_sock_, &mh, 0);
    if (n < (ssize_t)VHOST_USER_HDR_SIZE) {
        fprintf(stderr, "[VMM] vu_recv failed (%zd bytes)\n", n);
        return false;
    }
    return true;
}

bool Vmm::vu_transact(uint32_t req, const void *payload, uint32_t sz,
                      VhostUserMsg &reply, const int *fds, int nfds) {
    if (!vu_send(req, payload, sz, fds, nfds)) return false;
    return vu_recv(reply);
}

// Early init: get features, set owner, share memory with virtiofsd.
// Called right after connecting, before the guest boots.
bool Vmm::vu_early_init() {
    VhostUserMsg reply = {};

    // 1. Get backend features
    if (!vu_transact(VU_GET_FEATURES, nullptr, 0, reply)) {
        fprintf(stderr, "[VMM] vhost-user GET_FEATURES failed\n");
        return false;
    }
    vu_features_ = reply.payload.u64;

    // SET_FEATURES must come first -- ack all offered features.
    uint64_t ack_features = vu_features_;
    if (!vu_send(VU_SET_FEATURES, &ack_features, 8)) return false;

    // Expose to guest: VERSION_1 must be present.
    vu_features_ |= VIRT_F_VERSION_1;

    // 2. Protocol features -- always try, virtiofsd supports it even if
    //    VHOST_USER_F_PROTOCOL_FEATURES isn't explicitly in features.
    uint64_t proto = 0;
    if (vu_transact(VU_GET_PROTOCOL_FEATURES, nullptr, 0, reply)) {
        vu_proto_features_ = reply.payload.u64;

        proto = vu_proto_features_ & (VU_PROTO_F_MQ | VU_PROTO_F_REPLY_ACK
                                      | VU_PROTO_F_BACKEND_REQ
                                      | VU_PROTO_F_BACKEND_SEND_FD
                                      | VU_PROTO_F_CONFIG);
        if (!vu_send(VU_SET_PROTOCOL_FEATURES, &proto, 8)) return false;
    }

    // 3. Set owner
    if (!vu_send(VU_SET_OWNER, nullptr, 0)) return false;

    // 4. Share guest RAM with virtiofsd
    VhostUserMemMsg mem = {};
    mem.nregions = 1;
    mem.regions[0].guest_phys_addr = 0;
    mem.regions[0].memory_size     = ram_bytes_;
    mem.regions[0].userspace_addr  = (uint64_t)ram_;
    mem.regions[0].mmap_offset     = 0;
    int ram_fd = ram_memfd_;
    if (!vu_send(VU_SET_MEM_TABLE, &mem,
                 sizeof(uint64_t) + sizeof(VhostUserMemRegion),
                 &ram_fd, 1)) {
        fprintf(stderr, "[VMM] SET_MEM_TABLE failed\n");
        return false;
    }

    // 5. Backend request channel (if supported)
    if (proto & VU_PROTO_F_BACKEND_REQ) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            vu_backend_sock_ = sv[0];
            uint64_t dummy = 0;
            vu_send(VU_SET_BACKEND_REQ_FD, &dummy, 0, &sv[1], 1);
            close(sv[1]);
        }
    }

    return true;
}

// Late init: called when guest writes DRIVER_OK to the virtio status register.
// Sets up the virtqueues with virtiofsd.
bool Vmm::vu_setup() {
    if (virtio_active_ || vu_sock_ < 0) return true;

    // Set up each virtqueue
    for (int i = 0; i < NUM_QUEUES; i++) {
        auto &q = vqs_[i];
        if (q.num == 0 || !q.ready) continue;

        // Close old eventfds if this is a re-setup (e.g., after snapshot restore)
        if (q.call_fd >= 0) close(q.call_fd);
        if (q.kick_fd >= 0) close(q.kick_fd);

        // Create eventfds for kick (guest->host) and call (host->guest)
        q.kick_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        q.call_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        // No ioeventfd or irqfd -- we handle everything in the VMM run loop
        // with a background thread to wake KVM_RUN when call_fds fire.

        // Tell virtiofsd about this queue
        VhostUserVringState vs = { (uint32_t)i, q.num };
        vu_send(VU_SET_VRING_NUM, &vs, sizeof(vs));

        VhostUserVringState base = { (uint32_t)i, 0 };
        vu_send(VU_SET_VRING_BASE, &base, sizeof(base));

        VhostUserVringAddr va = {};
        va.index          = i;
        // Convert GPAs to "userspace addresses" matching SET_MEM_TABLE's userspace_addr
        va.desc_user_addr  = (uint64_t)ram_ + q.desc;
        va.used_user_addr  = (uint64_t)ram_ + q.device;
        va.avail_user_addr = (uint64_t)ram_ + q.driver;
        vu_send(VU_SET_VRING_ADDR, &va, sizeof(va));

        // Send kick fd
        uint64_t kick_msg = (uint64_t)i | (0ULL << 8); // no VHOST_USER_VRING_NOFD flag
        vu_send(VU_SET_VRING_KICK, &kick_msg, 8, &q.kick_fd, 1);

        // Send call fd
        uint64_t call_msg = (uint64_t)i;
        vu_send(VU_SET_VRING_CALL, &call_msg, 8, &q.call_fd, 1);

        // Enable the vring
        VhostUserVringState en = { (uint32_t)i, 1 };
        vu_send(VU_SET_VRING_ENABLE, &en, sizeof(en));
    }

    virtio_active_ = true;
    start_irq_thread();
    return true;
}

bool Vmm::start_virtiofsd(const char *shared_dir) {
    // Use a unique socket path per VMM instance to avoid stale connections
    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path),
             "/tmp/vmtainer-vhost-%d.sock", getpid());
    unlink(sock_path);
    // Also clean up the pid file that virtiofsd creates
    char pid_path[270];
    snprintf(pid_path, sizeof(pid_path), "%s.pid", sock_path);
    unlink(pid_path);

    virtiofsd_pid_ = fork();
    if (virtiofsd_pid_ < 0) { perror("fork virtiofsd"); return false; }

    if (virtiofsd_pid_ == 0) {
        // Close VMM fds to avoid leaking into virtiofsd
        if (kvm_fd_ >= 0)  close(kvm_fd_);
        if (vm_fd_ >= 0)   close(vm_fd_);
        if (vcpu_fd_ >= 0) close(vcpu_fd_);
        if (ram_memfd_ >= 0) close(ram_memfd_);
        // Suppress virtiofsd info-level logs
        setenv("RUST_LOG", "error", 1);
        execlp("/usr/libexec/virtiofsd", "virtiofsd",
               "--socket-path", sock_path,
               "--shared-dir", shared_dir,
               "--sandbox", "none",
               "--cache", "never",
               (char *)nullptr);
        perror("exec virtiofsd");
        _exit(1);
    }

    printf("[VMM] started virtiofsd (pid %d) sharing %s\n",
           virtiofsd_pid_, shared_dir);

    if (!vu_connect(sock_path)) {
        fprintf(stderr, "[VMM] failed to connect to virtiofsd\n");
        return false;
    }

    // Do early vhost-user handshake so features are known before guest probes
    if (!vu_early_init()) {
        fprintf(stderr, "[VMM] vhost-user early init failed\n");
        return false;
    }

    // Add the virtio_mmio device to the kernel command line
    char mmio_param[128];
    snprintf(mmio_param, sizeof(mmio_param),
             " virtio_mmio.device=0x%lx@0x%llx:%u",
             VIRTIO_MMIO_SIZE,
             (unsigned long long)VIRTIO_MMIO_GPA,
             VIRTIO_MMIO_IRQ);
    strncat(cmdline_, mmio_param, sizeof(cmdline_) - strlen(cmdline_) - 1);

    // Set up the fs config with tag "myfs"
    memset(&fs_config_, 0, sizeof(fs_config_));
    strncpy(fs_config_.tag, "myfs", sizeof(fs_config_.tag));
    fs_config_.num_request_queues = 1;

    printf("[VMM] virtiofs connected, tag='myfs'\n");
    return true;
}

bool Vmm::setup_virtio_fs() {
    // nothing extra needed -- MMIO traps are handled in run()
    return true;
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

int Vmm::run() {
    int consecutive_hlt_eintr = 0;
    for (;;) {
        // Check if virtiofsd has completed any requests
        check_virtio_irqs();

        if (ioctl(vcpu_fd_, KVM_RUN, 0) < 0) {
            if (errno == EINTR) {
                // SIGUSR1 from IRQ thread.  Check if the guest is stuck
                // in cli+hlt (shutdown).  Require 3 consecutive EINTR
                // with IF=0 to avoid false positives during brief cli
                // sections in kernel boot.
                struct kvm_regs regs;
                ioctl(vcpu_fd_, KVM_GET_REGS, &regs);
                if (!(regs.rflags & 0x200)) {
                    if (++consecutive_hlt_eintr >= 3)
                        return 0;
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

            // Virtio MMIO region
            if (addr >= VIRTIO_MMIO_GPA &&
                addr < VIRTIO_MMIO_GPA + VIRTIO_MMIO_SIZE) {
                uint64_t off = addr - VIRTIO_MMIO_GPA;
                if (kvm_run_->mmio.is_write)
                    virtio_mmio_write(off, kvm_run_->mmio.data, len);
                else
                    virtio_mmio_read(off, kvm_run_->mmio.data, len);
                break;
            }

            // Unknown MMIO -- return 0xff for reads
            if (!kvm_run_->mmio.is_write)
                memset(kvm_run_->mmio.data, 0xff, len);
            break;
        }

        case KVM_EXIT_HLT: {
            // Guest executed HLT.  If interrupts are disabled (IF=0),
            // the guest did cli+hlt which means shutdown.  Otherwise
            // it's a normal idle wait -- re-enter KVM_RUN.
            struct kvm_regs hlt_regs;
            ioctl(vcpu_fd_, KVM_GET_REGS, &hlt_regs);
            if (!(hlt_regs.rflags & 0x200))
                return 0; // shutdown (cli + hlt)
            break;
        }
        case KVM_EXIT_SHUTDOWN:
            return 0;

        case KVM_EXIT_FAIL_ENTRY:
            fprintf(stderr, "FAIL_ENTRY: 0x%llx\n",
                    (unsigned long long)kvm_run_->fail_entry.hardware_entry_failure_reason);
            return -1;

        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr, "INTERNAL_ERROR: %u\n", kvm_run_->internal.suberror);
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
// Snapshot -- restore (from in-memory Snapshot)
// ---------------------------------------------------------------------------

bool Vmm::restore_snapshot(const Snapshot &snap) {
    const auto &h = snap.hdr;
    if (h.magic != SNAP_MAGIC || h.version != SNAP_VERSION || h.ram_mb != ram_mb_) {
        fprintf(stderr, "bad snapshot\n");
        return false;
    }

    // Restore guest RAM
    memcpy(ram_, snap.ram, ram_bytes_);

    // Restore CPUID
    size_t csz = sizeof(kvm_cpuid2) + h.cpuid_nent * sizeof(kvm_cpuid_entry2);
    free(cpuid_);
    cpuid_ = (kvm_cpuid2 *)calloc(1, csz);
    cpuid_->nent = h.cpuid_nent;
    memcpy(cpuid_->entries, snap.cpuid.data(),
           h.cpuid_nent * sizeof(kvm_cpuid_entry2));

    // Restore order follows Firecracker conventions
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

    // If virtiofsd is connected, set up the virtqueues with the new instance
    if (vu_sock_ >= 0 && vdev_status_ & 0x4) {
        virtio_active_ = false; // Reset so vu_setup() runs
        vu_setup();
    }

    ignore_next_signal_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Snapshot -- file I/O (for boot --snapshot)
// ---------------------------------------------------------------------------

bool Vmm::save_snapshot_file(const char *path) {
    Snapshot snap;
    if (!save_snapshot(snap)) return false;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open snap"); return false; }

    write(fd, &snap.hdr, sizeof(snap.hdr));
    write(fd, snap.xsave, snap.hdr.xsave_size);
    write(fd, snap.cpuid.data(), snap.cpuid.size() * sizeof(kvm_cpuid_entry2));
    write(fd, snap.msrs.data(), snap.msrs.size() * sizeof(kvm_msr_entry));

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
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open snap"); return false; }

    Snapshot snap;
    if (::read(fd, &snap.hdr, sizeof(snap.hdr)) != sizeof(snap.hdr)) {
        perror("read hdr"); close(fd); return false;
    }
    if (::read(fd, snap.xsave, snap.hdr.xsave_size) != (ssize_t)snap.hdr.xsave_size) {
        perror("read xsave"); close(fd); return false;
    }

    snap.cpuid.resize(snap.hdr.cpuid_nent);
    size_t csz = snap.hdr.cpuid_nent * sizeof(kvm_cpuid_entry2);
    if (::read(fd, snap.cpuid.data(), csz) != (ssize_t)csz) {
        perror("read cpuid"); close(fd); return false;
    }

    snap.msrs.resize(snap.hdr.num_msrs);
    size_t msz = snap.hdr.num_msrs * sizeof(kvm_msr_entry);
    if (::read(fd, snap.msrs.data(), msz) != (ssize_t)msz) {
        perror("read msrs"); close(fd); return false;
    }

    snap.ram_size = snap.hdr.ram_mb * 1024ULL * 1024;
    snap.ram = mmap(nullptr, snap.ram_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t rd = 0;
    while (rd < snap.ram_size) {
        ssize_t n = ::read(fd, (uint8_t *)snap.ram + rd, snap.ram_size - rd);
        if (n <= 0) { perror("read mem"); close(fd); return false; }
        rd += n;
    }
    close(fd);

    bool ok = restore_snapshot(snap);
    munmap(snap.ram, snap.ram_size);
    return ok;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void Vmm::cleanup() {
    // Stop IRQ thread first
    if (irq_thread_running_.load()) {
        irq_thread_running_.store(false);
        if (irq_wakeup_fd_ >= 0) {
            uint64_t one = 1;
            ::write(irq_wakeup_fd_, &one, sizeof(one));
        }
        pthread_join(irq_thread_, nullptr);
    }
    if (irq_wakeup_fd_ >= 0) close(irq_wakeup_fd_);

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
    vu_sock_ = vu_backend_sock_ = -1;
    cpuid_ = nullptr;
    virtiofsd_pid_ = -1;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s boot   <bzImage> <initrd> --share <dir> [--snapshot <path>]\n"
        "  %s restore <snapshot> --share <dir> [--entrypoint <cmd>]\n"
        "  %s clone   <snapshot> <count> --share <dir> [--entrypoint <cmd>]\n",
        prog, prog, prog);
}

static void write_entrypoint_file(const char *share_dir, const char *entrypoint) {
    if (!share_dir || !entrypoint) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.entrypoint", share_dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, entrypoint, strlen(entrypoint));
        write(fd, "\n", 1);
        close(fd);
    }
}

static const char *find_arg(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

int main(int argc, char **argv) {
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];

    // ── boot ──
    if (strcmp(cmd, "boot") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        const char *kernel    = argv[2];
        const char *initrd    = argv[3];
        const char *share_dir = find_arg(argc, argv, "--share");
        const char *snap_path = find_arg(argc, argv, "--snapshot");

        Vmm vmm(64);
        if (!vmm.init()) return 1;

        if (share_dir) {
            if (!vmm.start_virtiofsd(share_dir)) return 1;
        }

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
            // snapshot signal but no --snapshot path: save in-memory
            // and print a message
            printf("[VMM] guest signalled snapshot (no --snapshot path, ignoring)\n");
            rc = vmm.run();
        }
        return (rc >= 0) ? 0 : 1;
    }

    // ── restore ──
    if (strcmp(cmd, "restore") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        const char *snap_path  = argv[2];
        const char *share_dir  = find_arg(argc, argv, "--share");
        const char *entrypoint = find_arg(argc, argv, "--entrypoint");

        write_entrypoint_file(share_dir, entrypoint);

        Vmm vmm(64);
        if (!vmm.init()) return 1;

        if (share_dir) {
            if (!vmm.start_virtiofsd(share_dir)) return 1;
            vmm.setup_virtio_fs();
        }

        if (!vmm.restore_snapshot_file(snap_path)) return 1;
        printf("[VMM] restored, running...\n");
        return (vmm.run() >= 0) ? 0 : 1;
    }

    // ── clone ──
    if (strcmp(cmd, "clone") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        const char *snap_path  = argv[2];
        int count = atoi(argv[3]);
        const char *share_dir  = find_arg(argc, argv, "--share");
        const char *entrypoint = find_arg(argc, argv, "--entrypoint");

        write_entrypoint_file(share_dir, entrypoint);

        // Load snapshot into memory once (parent process)
        Snapshot golden;
        {
            int fd = open(snap_path, O_RDONLY);
            if (fd < 0) { perror("open snap"); return 1; }
            if (::read(fd, &golden.hdr, sizeof(golden.hdr)) != sizeof(golden.hdr)) {
                perror("read hdr"); return 1;
            }
            ::read(fd, golden.xsave, golden.hdr.xsave_size);
            golden.cpuid.resize(golden.hdr.cpuid_nent);
            ::read(fd, golden.cpuid.data(),
                   golden.hdr.cpuid_nent * sizeof(kvm_cpuid_entry2));
            golden.msrs.resize(golden.hdr.num_msrs);
            ::read(fd, golden.msrs.data(),
                   golden.hdr.num_msrs * sizeof(kvm_msr_entry));
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
                // Child: each clone gets its own VM, virtiofsd, etc.
                Vmm vmm(golden.hdr.ram_mb);
                if (!vmm.init()) _exit(1);

                if (share_dir) {
                    if (!vmm.start_virtiofsd(share_dir)) _exit(1);
                    vmm.setup_virtio_fs();
                }

                if (!vmm.restore_snapshot(golden)) _exit(1);

                printf("[VMM clone %d] running\n", i);
                int rc = vmm.run();
                printf("[VMM clone %d] exited (%d)\n", i, rc);
                _exit((rc >= 0) ? 0 : 1);
            }
        }

        // Parent waits for all children
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
