// vmm.cpp -- vmtainer: Minimal KVM VMM with snapshots, virtiofs, vhost-net
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
#include <net/if.h>
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
#include <linux/if_tun.h>
#include <poll.h>
#include <algorithm>
#include <linux/limits.h>
#include <linux/kvm.h>

// vhost-net struct and ioctl definitions (avoid including linux/vhost.h
// which pulls in linux/virtio_ring.h with C-incompatible casts for C++)

struct vhost_memory_region {
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;
    uint64_t flags_padding;
};

// Base struct for ioctl number encoding (matches kernel's flexible array size)
struct vhost_memory_base {
    uint32_t nregions;
    uint32_t padding;
};

// Usable struct with one region inline
struct vhost_memory {
    uint32_t nregions;
    uint32_t padding;
    struct vhost_memory_region regions[1];
};

struct vhost_vring_state {
    unsigned int index;
    unsigned int num;
};

struct vhost_vring_file {
    unsigned int index;
    int fd;
};

struct vhost_vring_addr {
    unsigned int index;
    unsigned int flags;
    uint64_t desc_user_addr;
    uint64_t used_user_addr;
    uint64_t avail_user_addr;
    uint64_t log_guest_addr;
};

// ioctl numbers must encode the correct struct size
#define VHOST_VIRTIO 0xAF
#define VHOST_GET_FEATURES      _IOR(VHOST_VIRTIO, 0x00, __u64)
#define VHOST_SET_FEATURES      _IOW(VHOST_VIRTIO, 0x00, __u64)
#define VHOST_SET_OWNER         _IO(VHOST_VIRTIO, 0x01)
#define VHOST_SET_MEM_TABLE     _IOW(VHOST_VIRTIO, 0x03, struct vhost_memory_base)
#define VHOST_SET_VRING_NUM     _IOW(VHOST_VIRTIO, 0x10, struct vhost_vring_state)
#define VHOST_SET_VRING_ADDR    _IOW(VHOST_VIRTIO, 0x11, struct vhost_vring_addr)
#define VHOST_SET_VRING_BASE    _IOW(VHOST_VIRTIO, 0x12, struct vhost_vring_state)
#define VHOST_SET_VRING_KICK    _IOW(VHOST_VIRTIO, 0x20, struct vhost_vring_file)
#define VHOST_SET_VRING_CALL    _IOW(VHOST_VIRTIO, 0x21, struct vhost_vring_file)
#define VHOST_NET_SET_BACKEND   _IOW(VHOST_VIRTIO, 0x30, struct vhost_vring_file)
#include <asm/bootparam.h>

#include "boot.hpp"
#include "bios_offsets.h"
#include "bios_rom.h"

// ---------------------------------------------------------------------------
// Debug logging (enabled with --debug flag)
// ---------------------------------------------------------------------------

static bool g_debug = false;

#define DBG(fmt, ...) do { \
    if (g_debug) fprintf(stderr, "[DBG] " fmt "\n", ##__VA_ARGS__); \
} while (0)

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

// Virtio-MMIO region for the virtio-net device (second device).
static constexpr uint64_t VIRTIO_NET_MMIO_GPA  = 0xd0002000ULL;
static constexpr uint64_t VIRTIO_NET_MMIO_SIZE = 0x1000;
static constexpr uint32_t VIRTIO_NET_MMIO_IRQ  = 6;

// Virtio device / vendor IDs
static constexpr uint32_t VIRTIO_DEV_FS     = 26;  // virtio type for virtiofs
static constexpr uint32_t VIRTIO_DEV_NET    = 1;   // virtio type for network
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

// Virtio-net feature bits
static constexpr uint64_t VIRTIO_NET_F_CSUM  = (1ULL << 0);
static constexpr uint64_t VIRTIO_NET_F_GUEST_CSUM = (1ULL << 1);
static constexpr uint64_t VIRTIO_NET_F_MAC   = (1ULL << 5);
static constexpr uint64_t VIRTIO_NET_F_STATUS = (1ULL << 16);
static constexpr uint32_t VIRTIO_NET_S_LINK_UP = 1;

// Virtio-net config space (matches kernel's struct virtio_net_config)
struct __attribute__((packed)) VirtioNetConfig {
    uint8_t  mac[6];
    uint16_t status;        // VIRTIO_NET_S_LINK_UP
    uint16_t max_vq_pairs;
    uint16_t mtu;
};

// Virtio-net header prepended to every packet in the virtqueue
struct __attribute__((packed)) VirtioNetHdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};
static_assert(sizeof(VirtioNetHdr) == 12, "virtio_net_hdr_v1 must be 12 bytes");

// Split virtqueue ring structures (for direct descriptor processing)
struct VringDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2

struct VringAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];    // variable length
};

struct VringUsedElem {
    uint32_t id;
    uint32_t len;
};

struct VringUsed {
    uint16_t flags;
    uint16_t idx;
    VringUsedElem ring[];  // variable length
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
#define SNAP_VERSION 7
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

    // Virtio-net device state
    uint32_t net_status;
    uint32_t net_drv_features_lo;
    uint32_t net_drv_features_hi;
    uint32_t net_num_vqs;
    struct {
        uint32_t num;
        uint32_t ready;
        uint64_t desc;
        uint64_t driver;
        uint64_t device;
    } net_vq_state[2]; // RX + TX
    uint8_t  net_mac[6];
    uint8_t  net_pad[2]; // alignment

    // Dirty page bitmap: 1 bit per 4K page (bitmap_bytes = ram_mb * 256 / 8)
    uint32_t bitmap_bytes;  // 0 if no bitmap
    uint32_t bitmap_pad;

    // Followed in memory / on disk by:
    //   xsave_buf[xsave_size]
    //   kvm_cpuid_entry2[cpuid_nent]
    //   kvm_msr_entry[num_msrs]
    //   dirty_bitmap[bitmap_bytes]   (if bitmap_bytes > 0)
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
    std::vector<uint8_t>    dirty_bitmap;          // 1 bit per 4K page
};

// ---------------------------------------------------------------------------
// VMM class
// ---------------------------------------------------------------------------

class Vmm {
public:
    explicit Vmm(size_t ram_mb) : ram_bytes_(ram_mb << 20), ram_mb_(ram_mb) {}
    ~Vmm() { cleanup(); }

    // Setup
    bool init(bool zero_ram = true);
    bool setup_bios();
    bool load_bzimage(const char *path);
    bool load_initrd(const char *path);
    bool setup_cpu();
    void set_shared_dir(const char *dir) { shared_dir_ = dir; }

    // Virtio-fs
    bool start_virtiofsd(const char *shared_dir);
    bool setup_virtio_fs(bool add_cmdline = false);

    // Virtio-net
    bool setup_virtio_net(const uint8_t mac[6], bool add_cmdline = true);
    bool connect_tap(const char *tap_name);

    // Run
    int run();  // 0 = halt, 1 = snapshot signal, -1 = error

    // Snapshots (in-memory)
    bool save_snapshot(Snapshot &snap);
    bool restore_snapshot(const Snapshot &snap);
    bool save_snapshot_file(const char *path);
    bool restore_snapshot_file(const char *path);

    friend void *net_thread_func(void *arg);

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

    // Virtio MMIO (virtiofs)
    void virtio_mmio_read(uint64_t off, uint8_t *data, uint32_t len);
    void virtio_mmio_write(uint64_t off, const uint8_t *data, uint32_t len);
    void virtio_kick(uint32_t qidx);
    void check_virtio_irqs();
    void start_irq_thread();
    static void *irq_thread_func(void *arg);

    // Virtio MMIO (net)
    void net_mmio_read(uint64_t off, uint8_t *data, uint32_t len);
    void net_mmio_write(uint64_t off, const uint8_t *data, uint32_t len);
    bool vhost_net_setup();  // wire up vhost-net kernel backend
    int  open_tap(const char *name);

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

    // Virtio-net device state
    static constexpr int NET_NUM_QUEUES = 2; // RX + TX
    Virtqueue  net_vqs_[NET_NUM_QUEUES];
    uint32_t   net_features_sel_ = 0;
    uint32_t   net_drv_features_sel_ = 0;
    uint64_t   net_drv_features_ = 0;
    uint32_t   net_status_       = 0;
    uint32_t   net_queue_sel_    = 0;
    uint32_t   net_irq_status_   = 0;
    VirtioNetConfig net_config_  = {};
    bool       net_active_       = false;
    int        tap_fd_           = -1;
    int        vhost_fd_         = -1;  // /dev/vhost-net fd (unused in userspace mode)
    int        net_wakeup_fd_    = -1;  // wakeup eventfd for net thread
    volatile bool net_thread_stop_ = false;

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
    bool use_hugetlb_ = false;

    // Number of threads for parallel snapshot restore memcpy (default 2)
    int copy_threads_ = 2;

public:
    void set_copy_threads(int n) { copy_threads_ = n > 0 ? n : 1; }
    void set_hugetlb(bool v) { use_hugetlb_ = v; }
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
    if (off != VIRTIO_MMIO_INTERRUPT_STATUS) // skip noisy irq status polls
        DBG("virtiofs mmio read: off=0x%lx len=%u", off, len);

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
        DBG("virtiofs STATUS: 0x%x -> 0x%x", old, val);
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

    // Retry connection with fast exponential backoff
    // virtiofsd typically ready within 10-50ms
    for (int us = 1000; us <= 200000; us = std::min(us * 2, 200000)) {
        if (connect(vu_sock_, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            DBG("vu_connect: connected after backoff=%dus", us);
            return true;
        }
        usleep(us);
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
    DBG("vu_early_init: starting vhost-user handshake");
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

    // Set up the fs config with tag "myfs"
    setup_virtio_fs();

    printf("[VMM] virtiofs connected, tag='myfs'\n");
    return true;
}

bool Vmm::setup_virtio_fs(bool add_cmdline) {
    // Set up fs config so MMIO reads return proper values
    memset(&fs_config_, 0, sizeof(fs_config_));
    strncpy(fs_config_.tag, "myfs", sizeof(fs_config_.tag));
    fs_config_.num_request_queues = 1;

    // Set default device features if not already set by vu_early_init
    if (vu_features_ == 0) {
        vu_features_ = VIRT_F_VERSION_1;
    }

    if (add_cmdline) {
        char mmio_param[128];
        snprintf(mmio_param, sizeof(mmio_param),
                 " virtio_mmio.device=0x%lx@0x%llx:%u",
                 VIRTIO_MMIO_SIZE,
                 (unsigned long long)VIRTIO_MMIO_GPA,
                 VIRTIO_MMIO_IRQ);
        strncat(cmdline_, mmio_param, sizeof(cmdline_) - strlen(cmdline_) - 1);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Virtio-net: TAP, MMIO transport, TX/RX
// ---------------------------------------------------------------------------

int Vmm::open_tap(const char *name) {
    int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/net/tun"); return -1; }

    struct ifreq ifr = {};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("TUNSETIFF");
        close(fd);
        return -1;
    }

    // Set non-blocking for the RX poll loop
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

// Set up the virtio-net device (MMIO transport + config space).
// Call during boot to register the device on the kernel cmdline.
// TAP fd is NOT opened here -- call connect_tap() separately for restore/clone.
bool Vmm::setup_virtio_net(const uint8_t mac[6], bool add_cmdline) {
    // Populate config space
    memcpy(net_config_.mac, mac, 6);
    net_config_.status = VIRTIO_NET_S_LINK_UP;
    net_config_.max_vq_pairs = 1;
    net_config_.mtu = 1500;

    if (add_cmdline) {
        char mmio_param[128];
        snprintf(mmio_param, sizeof(mmio_param),
                 " virtio_mmio.device=0x%lx@0x%llx:%u",
                 VIRTIO_NET_MMIO_SIZE,
                 (unsigned long long)VIRTIO_NET_MMIO_GPA,
                 VIRTIO_NET_MMIO_IRQ);
        strncat(cmdline_, mmio_param, sizeof(cmdline_) - strlen(cmdline_) - 1);
    }

    printf("[VMM] virtio-net: mac=%02x:%02x:%02x:%02x:%02x:%02x%s\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           add_cmdline ? " (cmdline)" : " (restore)");
    return true;
}

// Open a TAP device and connect it to the virtio-net backend.
// Call on restore/clone when network is needed.
bool Vmm::connect_tap(const char *tap_name) {
    DBG("connect_tap: name=%s", tap_name);
    tap_fd_ = open_tap(tap_name);
    if (tap_fd_ < 0) return false;
    DBG("connect_tap: fd=%d", tap_fd_);
    printf("[VMM] TAP connected: %s\n", tap_name);
    return true;
}

// MMIO read for the virtio-net device
void Vmm::net_mmio_read(uint64_t off, uint8_t *data, uint32_t len) {
    uint32_t val = 0;
    DBG("net mmio read: off=0x%lx len=%u", off, len);

    switch (off) {
    case VIRTIO_MMIO_MAGIC_VALUE:  val = 0x74726976; break; // "virt"
    case VIRTIO_MMIO_VERSION:      val = 2;          break;
    case VIRTIO_MMIO_DEVICE_ID:    val = VIRTIO_DEV_NET; break;
    case VIRTIO_MMIO_VENDOR_ID:    val = VIRTIO_VENDOR_ID; break;

    case VIRTIO_MMIO_DEVICE_FEATURES: {
        // Don't advertise VIRTIO_NET_F_STATUS: the driver will assume link is always up,
        // which avoids NO-CARRIER issues after snapshot/restore.
        uint64_t feat = VIRTIO_NET_F_MAC | VIRT_F_VERSION_1;
        if (net_features_sel_ == 0) val = (uint32_t)(feat & 0xFFFFFFFF);
        else if (net_features_sel_ == 1) val = (uint32_t)(feat >> 32);
        break;
    }
    case VIRTIO_MMIO_QUEUE_NUM_MAX: val = 256; break;
    case VIRTIO_MMIO_QUEUE_READY:
        if (net_queue_sel_ < NET_NUM_QUEUES) val = net_vqs_[net_queue_sel_].ready;
        break;
    case VIRTIO_MMIO_INTERRUPT_STATUS: val = net_irq_status_; break;
    case VIRTIO_MMIO_STATUS:       val = net_status_;  break;
    case VIRTIO_MMIO_CONFIG_GEN:   val = 0;           break;
    case VIRTIO_MMIO_SHM_LEN_LOW:
    case VIRTIO_MMIO_SHM_LEN_HIGH:
    case VIRTIO_MMIO_SHM_BASE_LOW:
    case VIRTIO_MMIO_SHM_BASE_HIGH: val = 0xFFFFFFFF; break;
    default:
        // Config space reads
        if (off >= VIRTIO_MMIO_CONFIG &&
            off < VIRTIO_MMIO_CONFIG + sizeof(net_config_)) {
            uint32_t cfg_off = off - VIRTIO_MMIO_CONFIG;
            memcpy(&val, (uint8_t *)&net_config_ + cfg_off, (len < 4) ? len : 4);
        }
        break;
    }
    memcpy(data, &val, (len < 4) ? len : 4);
}

// MMIO write for the virtio-net device
void Vmm::net_mmio_write(uint64_t off, const uint8_t *data, uint32_t len) {
    uint32_t val = 0;
    memcpy(&val, data, (len < 4) ? len : 4);

    switch (off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL: net_features_sel_ = val; break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL: net_drv_features_sel_ = val; break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (net_drv_features_sel_ == 0)
            net_drv_features_ = (net_drv_features_ & 0xFFFFFFFF00000000ULL) | val;
        else if (net_drv_features_sel_ == 1)
            net_drv_features_ = (net_drv_features_ & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
        break;
    case VIRTIO_MMIO_QUEUE_SEL: net_queue_sel_ = val; break;
    case VIRTIO_MMIO_QUEUE_NUM:
        if (net_queue_sel_ < NET_NUM_QUEUES) net_vqs_[net_queue_sel_].num = val;
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        if (net_queue_sel_ < NET_NUM_QUEUES) net_vqs_[net_queue_sel_].ready = val;
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        // Wake up the net thread to process queues
        if (net_active_ && net_wakeup_fd_ >= 0) {
            uint64_t one = 1;
            ::write(net_wakeup_fd_, &one, sizeof(one));
        }
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        net_irq_status_ &= ~val;
        if (net_irq_status_ == 0) {
            struct kvm_irq_level irq = {};
            irq.irq = VIRTIO_NET_MMIO_IRQ;
            irq.level = 0;
            ioctl(vm_fd_, KVM_IRQ_LINE, &irq);
        }
        break;
    case VIRTIO_MMIO_STATUS: {
        uint32_t old = net_status_;
        net_status_ = val;
        if (val == 0) {
            for (auto &q : net_vqs_) q = {};
            net_drv_features_ = 0;
            net_irq_status_ = 0;
        }
        // DRIVER_OK -> set up vhost-net kernel backend
        if ((val & 0x4) && !(old & 0x4)) {
            net_active_ = true;
            vhost_net_setup();
        }
        break;
    }
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        if (net_queue_sel_ < NET_NUM_QUEUES)
            net_vqs_[net_queue_sel_].desc = (net_vqs_[net_queue_sel_].desc & ~0xFFFFFFFFULL) | val;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        if (net_queue_sel_ < NET_NUM_QUEUES)
            net_vqs_[net_queue_sel_].desc = (net_vqs_[net_queue_sel_].desc & 0xFFFFFFFFULL)
                                          | ((uint64_t)val << 32);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        if (net_queue_sel_ < NET_NUM_QUEUES)
            net_vqs_[net_queue_sel_].driver = (net_vqs_[net_queue_sel_].driver & ~0xFFFFFFFFULL) | val;
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        if (net_queue_sel_ < NET_NUM_QUEUES)
            net_vqs_[net_queue_sel_].driver = (net_vqs_[net_queue_sel_].driver & 0xFFFFFFFFULL)
                                            | ((uint64_t)val << 32);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        if (net_queue_sel_ < NET_NUM_QUEUES)
            net_vqs_[net_queue_sel_].device = (net_vqs_[net_queue_sel_].device & ~0xFFFFFFFFULL) | val;
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        if (net_queue_sel_ < NET_NUM_QUEUES)
            net_vqs_[net_queue_sel_].device = (net_vqs_[net_queue_sel_].device & 0xFFFFFFFFULL)
                                            | ((uint64_t)val << 32);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Userspace virtio-net data path (Firecracker model).
// We process the virtqueues ourselves: TAP -> RX queue, TX queue -> TAP.
// A dedicated thread polls the TAP for incoming packets and the TX queue
// for outgoing packets.
// ---------------------------------------------------------------------------

// VringDesc, VringAvail, VringUsedElem, VringUsed already defined above.
// Forward declaration for net thread
void *net_thread_func(void *arg);

bool Vmm::vhost_net_setup() {
    if (tap_fd_ < 0) return true;  // no TAP -- nothing to do

    // Create net thread wakeup eventfd
    net_wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    // Start the network processing thread
    pthread_t net_thread;
    pthread_create(&net_thread, nullptr, net_thread_func, this);
    pthread_detach(net_thread);

    printf("[VMM] virtio-net: userspace data path active\n");
    return true;
}

void *net_thread_func(void *arg) {
    auto *vmm = (Vmm *)arg;
    DBG("net_thread: started, tap_fd=%d", vmm->tap_fd_);

    // Access the queues via the VMM's ram pointer
    auto gpa_to_hva = [&](uint64_t gpa) -> void * {
        if (gpa < vmm->ram_bytes_)
            return (uint8_t *)vmm->ram_ + gpa;
        return nullptr;
    };

    auto &rxq = vmm->net_vqs_[0]; // RX: host -> guest
    auto &txq = vmm->net_vqs_[1]; // TX: guest -> host

    uint16_t rx_last_used = 0;
    uint16_t tx_last_avail = 0;

    // Wait until the queues are ready
    while (!vmm->net_active_ || !rxq.ready || !txq.ready) {
        usleep(10000); // 10ms
        if (vmm->tap_fd_ < 0) return nullptr;
    }

    // Read initial indices from the rings
    auto *rx_avail = (VringAvail *)gpa_to_hva(rxq.driver);
    auto *rx_used  = (VringUsed *)gpa_to_hva(rxq.device);
    auto *rx_desc  = (VringDesc *)gpa_to_hva(rxq.desc);
    auto *tx_avail = (VringAvail *)gpa_to_hva(txq.driver);
    auto *tx_used  = (VringUsed *)gpa_to_hva(txq.device);
    auto *tx_desc  = (VringDesc *)gpa_to_hva(txq.desc);

    if (!rx_avail || !rx_used || !rx_desc || !tx_avail || !tx_used || !tx_desc) {
        fprintf(stderr, "[VMM] net_thread: invalid queue addresses\n");
        return nullptr;
    }

    // Sync: start from where the driver is
    tx_last_avail = tx_used->idx;
    rx_last_used = rx_used->idx;

    uint8_t pkt_buf[65536];
    struct pollfd pfds[2];
    pfds[0].fd = vmm->tap_fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = vmm->net_wakeup_fd_;
    pfds[1].events = POLLIN;

    while (!vmm->net_thread_stop_ && vmm->tap_fd_ >= 0) {
        bool did_work = false;

        // ---- TX: process guest -> host packets ----
        __sync_synchronize(); // rmb
        while (tx_last_avail != tx_avail->idx) {
            uint16_t desc_idx = tx_avail->ring[tx_last_avail % txq.num];
            // Gather all buffers in the descriptor chain
            uint8_t *out = pkt_buf;
            size_t total = 0;
            uint16_t cur = desc_idx;
            for (int chain = 0; chain < 64; chain++) {
                auto *d = &tx_desc[cur % txq.num];
                void *buf = gpa_to_hva(d->addr);
                if (buf && d->len > 0 && total + d->len < sizeof(pkt_buf)) {
                    memcpy(out + total, buf, d->len);
                    total += d->len;
                }
                if (!(d->flags & VRING_DESC_F_NEXT)) break;
                cur = d->next;
            }

            // Write to TAP (skip the virtio_net_hdr at the start)
            if (total > sizeof(VirtioNetHdr)) {
                uint8_t *eth = pkt_buf + sizeof(VirtioNetHdr);
                size_t eth_len = total - sizeof(VirtioNetHdr);
                ssize_t nw = ::write(vmm->tap_fd_, eth, eth_len);
                (void)nw;
            }

            // Mark used
            auto *ue = &tx_used->ring[tx_used->idx % txq.num];
            ue->id = desc_idx;
            ue->len = 0;
            __sync_synchronize(); // wmb
            tx_used->idx++;
            tx_last_avail++;
            did_work = true;
        }

        // Inject TX completion interrupt (edge-trigger: deassert then assert)
        if (did_work) {
            vmm->net_irq_status_ |= 1;
            struct kvm_irq_level irq = {};
            irq.irq = VIRTIO_NET_MMIO_IRQ;
            irq.level = 0;
            ioctl(vmm->vm_fd_, KVM_IRQ_LINE, &irq);
            irq.level = 1;
            ioctl(vmm->vm_fd_, KVM_IRQ_LINE, &irq);
        }

        // ---- RX: drain all available packets from TAP into guest ----
        {
            bool rx_did_work = false;
            __sync_synchronize();
            while (rx_last_used != rx_avail->idx) {
                ssize_t nr = ::read(vmm->tap_fd_, pkt_buf, sizeof(pkt_buf));
                if (nr <= 0) break; // no more packets

                uint16_t desc_idx = rx_avail->ring[rx_last_used % rxq.num];
                auto *d = &rx_desc[desc_idx % rxq.num];

                void *buf = gpa_to_hva(d->addr);
                if (!buf || !(d->flags & VRING_DESC_F_WRITE)) break;

                size_t total = 0;
                if (d->len >= sizeof(VirtioNetHdr) + (size_t)nr) {
                    memset(buf, 0, sizeof(VirtioNetHdr));
                    memcpy((uint8_t *)buf + sizeof(VirtioNetHdr), pkt_buf, nr);
                    total = sizeof(VirtioNetHdr) + nr;
                } else if (d->flags & VRING_DESC_F_NEXT) {
                    size_t hdr_len = std::min((uint32_t)sizeof(VirtioNetHdr), d->len);
                    memset(buf, 0, hdr_len);
                    auto *d2 = &rx_desc[d->next % rxq.num];
                    void *buf2 = gpa_to_hva(d2->addr);
                    if (buf2 && d2->len >= (uint32_t)nr) {
                        memcpy(buf2, pkt_buf, nr);
                        total = sizeof(VirtioNetHdr) + nr;
                    }
                }

                if (total > 0) {
                    auto *ue = &rx_used->ring[rx_used->idx % rxq.num];
                    ue->id = desc_idx;
                    ue->len = (uint32_t)total;
                    __sync_synchronize();
                    rx_used->idx++;
                    rx_last_used++;
                    rx_did_work = true;
                } else {
                    break;
                }
            }
            if (rx_did_work) {
                vmm->net_irq_status_ |= 1;
                struct kvm_irq_level irq = {};
                irq.irq = VIRTIO_NET_MMIO_IRQ;
                irq.level = 0;
                ioctl(vmm->vm_fd_, KVM_IRQ_LINE, &irq);
                irq.level = 1;
                ioctl(vmm->vm_fd_, KVM_IRQ_LINE, &irq);
                did_work = true;
            }
        }

        if (!did_work) {
            poll(pfds, 2, 10); // 10ms timeout
            if (pfds[1].revents & POLLIN) {
                uint64_t v;
                ::read(vmm->net_wakeup_fd_, &v, 8);
            }
        }
    }
    return nullptr;
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

    // Restore guest RAM -- sparse copy (skip zero pages).
    // The memfd starts zeroed, so we only copy non-zero (dirty) pages.
    // With a dirty bitmap we skip zero pages entirely (no reads).
    // Uses parallel memcpy with pre-faulted destination pages.
    {
        constexpr size_t PAGE = 4096;
        const uint8_t *src = (const uint8_t *)snap.ram;
        uint8_t *dst = (uint8_t *)ram_;
        size_t total_pages = ram_bytes_ / PAGE;

        if (!snap.dirty_bitmap.empty()) {
            // Fast path: bitmap-guided copy with run-length coalescing.
            // Instead of copying one 4KB page at a time, we detect
            // consecutive dirty pages and merge them into larger memcpy
            // calls.  This amortizes the per-page fault overhead.

            // If multiple threads requested, use parallel workers.
            struct CopyWork {
                const uint8_t *src;
                uint8_t *dst;
                const uint8_t *bitmap;
                size_t pg_start;
                size_t pg_end;
                size_t dirty_count;
            };

            auto copy_worker = [](void *arg) -> void * {
                auto *w = (CopyWork *)arg;
                size_t count = 0;
                size_t pg = w->pg_start;
                while (pg < w->pg_end) {
                    if (!(w->bitmap[pg / 8] & (1 << (pg & 7)))) { pg++; continue; }
                    // Found a dirty page -- scan for consecutive run
                    size_t run_start = pg;
                    while (pg < w->pg_end &&
                           (w->bitmap[pg / 8] & (1 << (pg & 7)))) {
                        pg++;
                    }
                    size_t run_len = pg - run_start;
                    count += run_len;
                    memcpy(w->dst + run_start * 4096,
                           w->src + run_start * 4096,
                           run_len * 4096);
                }
                w->dirty_count = count;
                return nullptr;
            };

            int NUM_COPY_THREADS = copy_threads_;
            std::vector<CopyWork> work(NUM_COPY_THREADS);
            std::vector<pthread_t> threads(NUM_COPY_THREADS);
            size_t pages_per = total_pages / NUM_COPY_THREADS;

            for (int t = 0; t < NUM_COPY_THREADS; t++) {
                work[t].src = src;
                work[t].dst = dst;
                work[t].bitmap = snap.dirty_bitmap.data();
                work[t].pg_start = t * pages_per;
                work[t].pg_end = (t == NUM_COPY_THREADS - 1) ? total_pages : (t + 1) * pages_per;
                work[t].dirty_count = 0;
                if (NUM_COPY_THREADS > 1)
                    pthread_create(&threads[t], nullptr, copy_worker, &work[t]);
            }

            size_t dirty_count = 0;
            if (NUM_COPY_THREADS == 1) {
                copy_worker(&work[0]);
                dirty_count = work[0].dirty_count;
            } else {
                for (int t = 0; t < NUM_COPY_THREADS; t++) {
                    pthread_join(threads[t], nullptr);
                    dirty_count += work[t].dirty_count;
                }
            }

            DBG("restore: bitmap copy %zu/%zu pages (%zuKB) [%d threads, coalesced]",
                dirty_count, total_pages, dirty_count * 4, NUM_COPY_THREADS);
        } else {
            // Slow path: scan each page
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

    // If virtiofsd is connected, set up the virtqueues with the new instance
    if (vu_sock_ >= 0 && vdev_status_ & 0x4) {
        virtio_active_ = false; // Reset so vu_setup() runs
        vu_setup();
    }

    // If TAP is connected and net was active, start the net thread
    if (tap_fd_ >= 0 && net_status_ & 0x4) {
        net_active_ = true;
        vhost_net_setup();
    }

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

    // Build dirty page bitmap (1 bit per 4K page)
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

    write(fd, &snap.hdr, sizeof(snap.hdr));
    write(fd, snap.xsave, snap.hdr.xsave_size);
    write(fd, snap.cpuid.data(), snap.cpuid.size() * sizeof(kvm_cpuid_entry2));
    write(fd, snap.msrs.data(), snap.msrs.size() * sizeof(kvm_msr_entry));
    write(fd, bitmap.data(), bm_bytes);

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

    // mmap the entire file for zero-copy access to the snapshot
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat snap"); close(fd); return false; }
    void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap snap"); close(fd); return false; }

    // Readahead only the header+metadata eagerly; RAM pages will be
    // faulted in selectively via the dirty bitmap.
    madvise(map, std::min((size_t)st.st_size, (size_t)(256 * 1024)), MADV_WILLNEED);

    clock_gettime(CLOCK_MONOTONIC, &tf1);

    const uint8_t *p = (const uint8_t *)map;
    size_t remain = st.st_size;

    // Parse header
    Snapshot snap;
    if (remain < sizeof(snap.hdr)) { munmap(map, st.st_size); close(fd); return false; }
    memcpy(&snap.hdr, p, sizeof(snap.hdr));
    p += sizeof(snap.hdr); remain -= sizeof(snap.hdr);

    // Parse xsave
    if (remain < snap.hdr.xsave_size) { munmap(map, st.st_size); close(fd); return false; }
    memcpy(snap.xsave, p, snap.hdr.xsave_size);
    p += snap.hdr.xsave_size; remain -= snap.hdr.xsave_size;

    // Parse cpuid
    size_t csz = snap.hdr.cpuid_nent * sizeof(kvm_cpuid_entry2);
    if (remain < csz) { munmap(map, st.st_size); close(fd); return false; }
    snap.cpuid.resize(snap.hdr.cpuid_nent);
    memcpy(snap.cpuid.data(), p, csz);
    p += csz; remain -= csz;

    // Parse msrs
    size_t msz = snap.hdr.num_msrs * sizeof(kvm_msr_entry);
    if (remain < msz) { munmap(map, st.st_size); close(fd); return false; }
    snap.msrs.resize(snap.hdr.num_msrs);
    memcpy(snap.msrs.data(), p, msz);
    p += msz; remain -= msz;

    // Parse dirty bitmap (if present)
    if (snap.hdr.bitmap_bytes > 0) {
        if (remain < snap.hdr.bitmap_bytes) { munmap(map, st.st_size); close(fd); return false; }
        snap.dirty_bitmap.resize(snap.hdr.bitmap_bytes);
        memcpy(snap.dirty_bitmap.data(), p, snap.hdr.bitmap_bytes);
        p += snap.hdr.bitmap_bytes; remain -= snap.hdr.bitmap_bytes;
    }

    // RAM: point directly into the mmap (avoid copy)
    snap.ram_size = snap.hdr.ram_mb * 1024ULL * 1024;
    if (remain < snap.ram_size) { munmap(map, st.st_size); close(fd); return false; }
    snap.ram = (void *)p;
    // Don't readahead RAM pages -- bitmap-guided copy will be selective
    madvise((void *)p, snap.ram_size, MADV_RANDOM);

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

    // Don't let ~Snapshot munmap -- we own the mapping
    snap.ram = nullptr;
    snap.ram_size = 0;
    munmap(map, st.st_size);
    close(fd);
    return ok;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void Vmm::cleanup() {
    // Signal net thread to stop and give it time to exit
    net_thread_stop_ = true;
    if (net_wakeup_fd_ >= 0) {
        uint64_t one = 1;
        ::write(net_wakeup_fd_, &one, sizeof(one));
        usleep(50000); // 50ms for thread to notice
        close(net_wakeup_fd_);
    }
    if (vhost_fd_ >= 0) close(vhost_fd_);
    if (tap_fd_ >= 0) close(tap_fd_);
    for (auto &q : net_vqs_) {
        if (q.kick_fd >= 0) close(q.kick_fd);
        if (q.call_fd >= 0) close(q.call_fd);
    }

    // Stop IRQ thread
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
    tap_fd_ = -1;
    vhost_fd_ = -1;
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

// ---------------------------------------------------------------------------
// Minimal JSON config parser (no dependencies)
// Parses our specific config format -- not a general JSON parser.
// ---------------------------------------------------------------------------

struct VmConfig {
    char rootfs[PATH_MAX] = {};
    char entrypoint[256]  = {};
    char hostname[64]     = {};
    char tap[IFNAMSIZ]    = {};
    char ip[32]           = {};
    char gateway[32]      = {};
    uint8_t mac[6]        = {};
    bool has_net          = false;
    // env vars: up to 32
    struct { char key[64]; char val[256]; } env[32];
    int num_env = 0;
};

// Skip whitespace in JSON string
static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// Extract a quoted string value, return pointer past closing quote
static const char *parse_string(const char *p, char *out, size_t maxlen) {
    p = skip_ws(p);
    if (*p != '"') return nullptr;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < maxlen - 1) {
        if (*p == '\\' && *(p+1)) { p++; } // simple escape
        out[i++] = *p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

// Parse MAC address "xx:xx:xx:xx:xx:xx"
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

    // Simple key-value extraction from JSON
    // Look for top-level keys
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

    // Parse net object
    const char *net = strstr(buf, "\"net\"");
    if (net) {
        // Find the { after "net":
        const char *p = net + 5;
        p = skip_ws(p);
        if (*p == ':') p = skip_ws(p + 1);
        if (*p == '{') {
            cfg.has_net = true;
            char mac_str[32] = {};
            // Parse within the net block -- find closing }
            const char *end = strchr(p, '}');
            if (!end) end = buf + n;
            size_t block_len = end - p + 1;
            char net_block[2048];
            if (block_len < sizeof(net_block)) {
                memcpy(net_block, p, block_len);
                net_block[block_len] = '\0';
                // Extract fields from net block
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

    // Parse env object (simple flat key-value)
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
                    strncpy(cfg.env[cfg.num_env].key, key, 63);
                    strncpy(cfg.env[cfg.num_env].val, val, 255);
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

// Write the config to the shared directory so the guest init can read it
static void write_config_to_share(const char *share_dir, const VmConfig &cfg) {
    if (!share_dir) return;

    // Write .entrypoint
    if (cfg.entrypoint[0]) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/.entrypoint", share_dir);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::write(fd, cfg.entrypoint, strlen(cfg.entrypoint));
            ::write(fd, "\n", 1);
            close(fd);
        }
    }

    // Write .vmconfig (simple key=value format the init script can source)
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
    for (int i = 0; i < cfg.num_env; i++)
        fprintf(f, "ENV_%s=%s\n", cfg.env[i].key, cfg.env[i].val);
    fclose(f);
}

int main(int argc, char **argv) {
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // Check for --debug flag anywhere in args
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

        Vmm vmm(64);
        if (!vmm.init()) return 1;

        if (share_dir) {
            if (!vmm.start_virtiofsd(share_dir)) return 1;
        }

        // Always register virtio-fs MMIO region on boot so the kernel
        // probes the device.  No virtiofsd running yet -- mount will
        // fail during boot, but restore/clone will start virtiofsd.
        vmm.setup_virtio_fs(true);

        // Always register virtio-net on boot so the kernel probes it.
        // No TAP connected yet -- packets go nowhere until restore/clone.
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
        const char *snap_path   = argv[2];
        const char *share_dir   = find_arg(argc, argv, "--share");
        const char *config_path = find_arg(argc, argv, "--config");
        const char *ct_str      = find_arg(argc, argv, "--copy-threads");
        // Legacy support
        const char *entrypoint  = find_arg(argc, argv, "--entrypoint");

        VmConfig cfg = {};
        if (config_path) {
            if (!parse_config(config_path, cfg)) return 1;
        } else if (entrypoint) {
            strncpy(cfg.entrypoint, entrypoint, sizeof(cfg.entrypoint) - 1);
        }

        // Config rootfs overrides --share if not explicitly given
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

        Vmm vmm(64);
        if (ct_str) vmm.set_copy_threads(atoi(ct_str));
        if (has_flag(argc, argv, "--hugetlb")) vmm.set_hugetlb(true);
        if (!vmm.init(false)) return 1;  // skip zeroing -- snapshot overwrites RAM
        long us_init = us_since(t0);

        long us_virtiofsd = 0;
        if (share_dir) {
            if (!vmm.start_virtiofsd(share_dir)) return 1;
            vmm.setup_virtio_fs();
            us_virtiofsd = us_since(t0) - us_init;
        }

        // Restore the virtio-net device state (no cmdline -- already in snapshot)
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

        printf("[VMM] restored in %.1fms  kvm_init=%.1fms virtiofsd=%.1fms tap=%.1fms snap=%.1fms\n",
               us_since(t0) / 1000.0,
               us_init / 1000.0,
               us_virtiofsd / 1000.0,
               (us_setup - us_init - us_virtiofsd) / 1000.0,
               us_snap / 1000.0);
        return (vmm.run() >= 0) ? 0 : 1;
    }

    // ── clone ──
    if (strcmp(cmd, "clone") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        const char *snap_path   = argv[2];
        int count = atoi(argv[3]);
        const char *share_dir   = find_arg(argc, argv, "--share");
        const char *config_path = find_arg(argc, argv, "--config");
        // Legacy support
        const char *entrypoint  = find_arg(argc, argv, "--entrypoint");

        VmConfig cfg = {};
        if (config_path) {
            if (!parse_config(config_path, cfg)) return 1;
        } else if (entrypoint) {
            strncpy(cfg.entrypoint, entrypoint, sizeof(cfg.entrypoint) - 1);
        }

        // Config rootfs overrides --share if not explicitly given
        if (!share_dir && cfg.rootfs[0])
            share_dir = cfg.rootfs;

        write_config_to_share(share_dir, cfg);

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
            if (golden.hdr.bitmap_bytes > 0) {
                golden.dirty_bitmap.resize(golden.hdr.bitmap_bytes);
                ::read(fd, golden.dirty_bitmap.data(), golden.hdr.bitmap_bytes);
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
                // Child: each clone gets its own VM, virtiofsd, etc.
                Vmm vmm(golden.hdr.ram_mb);
                if (!vmm.init(false)) _exit(1);

                if (share_dir) {
                    if (!vmm.start_virtiofsd(share_dir)) _exit(1);
                    vmm.setup_virtio_fs();
                }

                // For clone, each clone gets its own TAP + MAC + IP
                uint8_t clone_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
                if (cfg.has_net) {
                    memcpy(clone_mac, cfg.mac, 6);
                    clone_mac[5] = (uint8_t)(cfg.mac[5] + i);

                    char clone_tap[IFNAMSIZ];
                    snprintf(clone_tap, sizeof(clone_tap), "tap%d", i);

                    // Write per-clone config to shared dir
                    if (share_dir) {
                        VmConfig clone_cfg = cfg;
                        strncpy(clone_cfg.tap, clone_tap, IFNAMSIZ - 1);
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

                // Set up virtio-net device (no cmdline -- already in snapshot)
                vmm.setup_virtio_net(clone_mac, false);

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
