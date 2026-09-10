#pragma once

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
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
#include <algorithm>
#include <linux/limits.h>
#include <linux/kvm.h>
#include <asm/bootparam.h>

#include "boot.hpp"
#include "bios_offsets.h"
#include "bios_rom.h"
#include "virtio.hpp"

// ---------------------------------------------------------------------------
// Debug logging (enabled with --debug flag)
// ---------------------------------------------------------------------------

extern bool g_debug;

#define DBG(fmt, ...) do { \
    if (g_debug) fprintf(stderr, "[DBG] " fmt "\n", ##__VA_ARGS__); \
} while (0)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// MMIO device for guest -> VMM snapshot signalling.
static constexpr uint64_t MMIO_SIGNAL_GPA   = 0xd0000000ULL;
static constexpr uint32_t MMIO_SIGNAL_MAGIC = 0x48594C54U; // "HYLT"

#define SNAP_MAGIC   0x48594C54534E4150ULL
#define SNAP_VERSION 7
#define MAX_MSRS     256
#define XSAVE_SIZE   8192

// ---------------------------------------------------------------------------
// Snapshot header -- all KVM state for one vCPU + VM
// ---------------------------------------------------------------------------

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
// Minimal JSON config parser struct
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

// ---------------------------------------------------------------------------
// VMM class
// ---------------------------------------------------------------------------

void *net_thread_func(void *arg);

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

    void set_copy_threads(int n) { copy_threads_ = n > 0 ? n : 1; }
    void set_hugetlb(bool v) { use_hugetlb_ = v; }
    void set_cmd_start(const struct timespec &ts) { t_cmd_start_ = ts; timing_entrypoint_ = true; }
    void set_vcpu_start(const struct timespec &ts) { t_vcpu_start_ = ts; }
    double time_to_entrypoint_ms() const { return time_to_entrypoint_ms_; }

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
                          " nokaslr tsc=reliable clocksource=tsc lpj=3392000 pci=off";

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
    pthread_t  net_thread_       = 0;
    bool       net_thread_running_ = false;
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

    // Entrypoint timing measurement
    struct timespec t_cmd_start_ = {};
    struct timespec t_vcpu_start_ = {};
    bool timing_entrypoint_ = false;
    bool entrypoint_measured_ = false;
    std::string serial_line_buf_;
    double time_to_entrypoint_ms_ = 0.0;
};
