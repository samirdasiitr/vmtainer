#pragma once

#include <cstdint>
#include <sys/ioctl.h>
#include <linux/types.h>

// ---------------------------------------------------------------------------
// vhost-net struct and ioctl definitions (avoid including linux/vhost.h
// which pulls in linux/virtio_ring.h with C-incompatible casts for C++)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Virtio MMIO constants
// ---------------------------------------------------------------------------

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
#define VHOST_USER_FLAG_REPLY      (1u << 2)
#define VHOST_USER_FLAG_NEED_REPLY (1u << 3)
#define VHOST_USER_VERSION         1

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
