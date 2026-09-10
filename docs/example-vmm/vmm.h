#ifndef VMM_H
#define VMM_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <linux/kvm.h>   /* KVM API constants and structures */

/* Program name printed in every log line. */
#define PROGRAM_NAME "example-vmm"

/* Default guest configuration. */
#define GUEST_MEM_DEFAULT_MB   128
#define GUEST_NR_VCPUS_DEFAULT 2
#define GUEST_CMDLINE_DEFAULT  "console=ttyS0,115200n8 earlyprintk=serial,0x3f8,115200 nokaslr"

/*
 * Global debug flag.  -d on the command line sets this to 1.
 * DBG() messages are only printed when this is non-zero.
 */
extern int g_debug;

/*
 * LOG() always writes to stderr and flushes, so learners can follow the
 * VMM step-by-step as it opens KVM, creates the VM, loads the kernel, etc.
 */
#define LOG(fmt, ...) do {                                              \
    fprintf(stderr, "[" PROGRAM_NAME "] " fmt "\n", ##__VA_ARGS__);    \
    fflush(stderr);                                                     \
} while (0)

/*
 * DBG() is like LOG() but includes the function name and is suppressed unless
 * -d was passed.  Use this for low-level vCPU or exit details.
 */
#define DBG(fmt, ...) do {                                              \
    if (g_debug) {                                                      \
        fprintf(stderr, "[" PROGRAM_NAME " DEBUG %s] " fmt "\n",       \
                __func__, ##__VA_ARGS__);                               \
        fflush(stderr);                                                 \
    }                                                                   \
} while (0)

/*
 * One guest physical memory region + one set of vCPUs.
 * In this skeleton everything is a single contiguous allocation starting at
 * guest physical address 0.
 */
struct vmm {
    int      kvm_fd;      /* /dev/kvm file descriptor                */
    int      vm_fd;       /* VM file descriptor                      */
    size_t   mem_size;    /* guest RAM size in bytes                 */
    uint8_t *mem;         /* host pointer to guest RAM               */
    int      nr_vcpus;    /* number of vCPUs requested (>=2)         */
    struct vcpu **vcpus;  /* array of vCPU pointers                  */

    /* Simple 16550A state used by the real-mode serial console. */
    uint8_t  uart_lcr;    /* line control: DLAB bit 7                 */
    uint8_t  uart_dll;    /* divisor latch low                        */
    uint8_t  uart_dlh;    /* divisor latch high                       */
    uint8_t  uart_ier;    /* interrupt enable                         */
    uint8_t  uart_fcr;    /* FIFO control                             */
    uint8_t  uart_mcr;    /* modem control                            */
    uint8_t  uart_msr;    /* modem status                             */
    uint8_t  uart_scr;    /* scratch                                  */
    uint8_t  uart_lsr;    /* line status: always 0x60 (tx empty)      */

    /* Flat disk image used by the simple IDE controller. */
    int      disk_fd;            /* host disk image fd                  */
    uint64_t disk_size;          /* image size in bytes                 */
    uint32_t disk_num_sectors;   /* image size / 512                    */

    /* ATA/IDE primary controller state. */
    uint8_t  ide_status;
    uint8_t  ide_error;
    uint8_t  ide_nsect;          /* 0x1f2 sector count                  */
    uint8_t  ide_lba_low;
    uint8_t  ide_lba_mid;
    uint8_t  ide_lba_high;
    uint8_t  ide_drive_head;
    uint8_t  ide_command;
    uint8_t  ide_control;
    uint32_t ide_lba;            /* current 28-bit LBA                  */
    uint16_t ide_data[256];      /* one 512-byte sector buffer          */
    int      ide_data_pos;       /* next word index for in/out          */
    int      ide_data_len;       /* words available in ide_data         */

    /* Virtio-MMIO block device state. */
    uint32_t vio_status;
    uint32_t vio_isr;
    uint32_t vio_queue_sel;
    uint32_t vio_guest_page_size;
    uint32_t vio_queue_num;
    uint32_t vio_queue_align;
    uint64_t vio_queue_desc;
    uint64_t vio_queue_avail;
    uint64_t vio_queue_used;
    uint32_t vio_queue_pfn;
    uint16_t vio_used_idx;
    uint8_t  vio_notify;         /* set when queue notify is triggered  */
    uint8_t  vio_config[128];    /* virtio-blk config space             */
    pthread_mutex_t vio_mutex;
};

/*
 * Per-vCPU state.  Each vCPU is run in its own pthread and shares the same
 * guest RAM through struct vmm.
 */
struct vcpu {
    int           id;        /* 0-based vCPU number                  */
    int           fd;        /* vCPU file descriptor                 */
    struct kvm_run *run;     /* mmap'd run control region            */
    size_t        mmap_size; /* size of the kvm_run mapping          */
    pthread_t     thread;    /* the thread executing this vCPU       */
    bool          running;   /* set to false to stop the run loop    */
    struct vmm    *vmm;      /* back-pointer to the VM               */
    bool          is_bsp;    /* true for vCPU0, the boot CPU         */
};

/* vmm lifecycle */
struct vmm *vmm_create(size_t mem_size, int nr_vcpus);
void        vmm_destroy(struct vmm *vmm);
int         vmm_map_memory(struct vmm *vmm);
int         vmm_load_disk(struct vmm *vmm, const char *path);

/* vCPU lifecycle and run loop */
struct vcpu *vcpu_create(struct vmm *vmm, int vcpu_id);
void         vcpu_destroy(struct vcpu *vcpu);
int          vcpu_init_regs(struct vcpu *vcpu);
int          vcpu_set_debug_regs(struct vcpu *vcpu);
void        *vcpu_thread(void *arg);

/* multi-vCPU startup / shutdown helpers */
void vmm_run_all(struct vmm *vmm);
void vmm_wait_all(struct vmm *vmm);

/* VM-exit handlers */
void vmm_handle_io(struct vcpu *vcpu);
void vmm_handle_mmio(struct vcpu *vcpu);

#endif /* VMM_H */
