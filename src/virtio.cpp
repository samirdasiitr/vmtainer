#include "vmm.hpp"

// ---------------------------------------------------------------------------
// Virtio MMIO transport (virtiofs)
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
        ssize_t nw = ::write(q.kick_fd, &one, sizeof(one));
        (void)nw;
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

        int ret = poll(fds, nfds, 10); // 10ms timeout (allows rapid HLT detection)

        // Check wakeup fd (shutdown signal)
        if (ret > 0 && fds[nfds - 1].revents & POLLIN) {
            uint64_t v;
            ssize_t nr = ::read(vmm->irq_wakeup_fd_, &v, 8);
            (void)nr;
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
    for (int us = 50; us <= 50000; us = std::min(us * 2, 50000)) {
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

    // 2. Protocol features
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
bool Vmm::vu_setup() {
    if (virtio_active_ || vu_sock_ < 0) return true;

    // Set up each virtqueue
    for (int i = 0; i < NUM_QUEUES; i++) {
        auto &q = vqs_[i];
        if (q.num == 0 || !q.ready) continue;

        if (q.call_fd >= 0) close(q.call_fd);
        if (q.kick_fd >= 0) close(q.kick_fd);

        q.kick_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        q.call_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        VhostUserVringState vs = { (uint32_t)i, q.num };
        vu_send(VU_SET_VRING_NUM, &vs, sizeof(vs));

        VhostUserVringState base = { (uint32_t)i, 0 };
        vu_send(VU_SET_VRING_BASE, &base, sizeof(base));

        VhostUserVringAddr va = {};
        va.index          = i;
        va.desc_user_addr  = (uint64_t)ram_ + q.desc;
        va.used_user_addr  = (uint64_t)ram_ + q.device;
        va.avail_user_addr = (uint64_t)ram_ + q.driver;
        vu_send(VU_SET_VRING_ADDR, &va, sizeof(va));

        uint64_t kick_msg = (uint64_t)i | (0ULL << 8);
        vu_send(VU_SET_VRING_KICK, &kick_msg, 8, &q.kick_fd, 1);

        uint64_t call_msg = (uint64_t)i;
        vu_send(VU_SET_VRING_CALL, &call_msg, 8, &q.call_fd, 1);

        VhostUserVringState en = { (uint32_t)i, 1 };
        vu_send(VU_SET_VRING_ENABLE, &en, sizeof(en));
    }

    virtio_active_ = true;
    start_irq_thread();
    return true;
}

bool Vmm::start_virtiofsd(const char *shared_dir) {
    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path),
             "/tmp/vmtainer-vhost-%d.sock", getpid());
    unlink(sock_path);
    char pid_path[270];
    snprintf(pid_path, sizeof(pid_path), "%s.pid", sock_path);
    unlink(pid_path);

    virtiofsd_pid_ = fork();
    if (virtiofsd_pid_ < 0) { perror("fork virtiofsd"); return false; }

    if (virtiofsd_pid_ == 0) {
        if (kvm_fd_ >= 0)  close(kvm_fd_);
        if (vm_fd_ >= 0)   close(vm_fd_);
        if (vcpu_fd_ >= 0) close(vcpu_fd_);
        if (ram_memfd_ >= 0) close(ram_memfd_);
        setenv("RUST_LOG", "error", 1);
        execlp("/usr/libexec/virtiofsd", "virtiofsd",
               "--socket-path", sock_path,
               "--shared-dir", shared_dir,
               "--sandbox", "none",
               "--cache", "auto",
               (char *)nullptr);
        perror("exec virtiofsd");
        _exit(1);
    }

    if (timing_entrypoint_) printf("[+%6.2fms] ", ms_since_start());
    printf("[VMM] started virtiofsd (pid %d) sharing %s\n",
           virtiofsd_pid_, shared_dir);

    if (!vu_connect(sock_path)) {
        fprintf(stderr, "[VMM] failed to connect to virtiofsd\n");
        return false;
    }

    if (!vu_early_init()) {
        fprintf(stderr, "[VMM] vhost-user early init failed\n");
        return false;
    }

    setup_virtio_fs();

    if (timing_entrypoint_) printf("[+%6.2fms] ", ms_since_start());
    printf("[VMM] virtiofs connected, tag='myfs'\n");
    return true;
}

bool Vmm::setup_virtio_fs(bool add_cmdline) {
    memset(&fs_config_, 0, sizeof(fs_config_));
    strncpy(fs_config_.tag, "myfs", sizeof(fs_config_.tag));
    fs_config_.num_request_queues = 1;

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

    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

bool Vmm::setup_virtio_net(const uint8_t mac[6], bool add_cmdline) {
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

    if (timing_entrypoint_) printf("[+%6.2fms] ", ms_since_start());
    printf("[VMM] virtio-net: mac=%02x:%02x:%02x:%02x:%02x:%02x%s\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           add_cmdline ? " (cmdline)" : " (restore)");
    return true;
}

bool Vmm::connect_tap(const char *tap_name) {
    DBG("connect_tap: name=%s", tap_name);
    tap_fd_ = open_tap(tap_name);
    if (tap_fd_ < 0) return false;
    DBG("connect_tap: fd=%d", tap_fd_);
    if (timing_entrypoint_) printf("[+%6.2fms] ", ms_since_start());
    printf("[VMM] TAP connected: %s\n", tap_name);
    return true;
}

void Vmm::net_mmio_read(uint64_t off, uint8_t *data, uint32_t len) {
    uint32_t val = 0;
    DBG("net mmio read: off=0x%lx len=%u", off, len);

    switch (off) {
    case VIRTIO_MMIO_MAGIC_VALUE:  val = 0x74726976; break; // "virt"
    case VIRTIO_MMIO_VERSION:      val = 2;          break;
    case VIRTIO_MMIO_DEVICE_ID:    val = VIRTIO_DEV_NET; break;
    case VIRTIO_MMIO_VENDOR_ID:    val = VIRTIO_VENDOR_ID; break;

    case VIRTIO_MMIO_DEVICE_FEATURES: {
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
        if (off >= VIRTIO_MMIO_CONFIG &&
            off < VIRTIO_MMIO_CONFIG + sizeof(net_config_)) {
            uint32_t cfg_off = off - VIRTIO_MMIO_CONFIG;
            memcpy(&val, (uint8_t *)&net_config_ + cfg_off, (len < 4) ? len : 4);
        }
        break;
    }
    memcpy(data, &val, (len < 4) ? len : 4);
}

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
        if (net_active_ && net_wakeup_fd_ >= 0) {
            uint64_t one = 1;
            ssize_t nw = ::write(net_wakeup_fd_, &one, sizeof(one));
            (void)nw;
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
// Userspace virtio-net data path
// ---------------------------------------------------------------------------

bool Vmm::vhost_net_setup() {
    if (tap_fd_ < 0) return true;

    net_wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    pthread_create(&net_thread_, nullptr, net_thread_func, this);
    net_thread_running_ = true;

    if (timing_entrypoint_) printf("[+%6.2fms] ", ms_since_start());
    printf("[VMM] virtio-net: userspace data path active\n");
    return true;
}

void *net_thread_func(void *arg) {
    auto *vmm = (Vmm *)arg;
    DBG("net_thread: started, tap_fd=%d", vmm->tap_fd_);

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

            // Write to TAP (skip virtio_net_hdr)
            if (total > sizeof(VirtioNetHdr)) {
                uint8_t *eth = pkt_buf + sizeof(VirtioNetHdr);
                size_t eth_len = total - sizeof(VirtioNetHdr);
                ssize_t nw = ::write(vmm->tap_fd_, eth, eth_len);
                (void)nw;
            }

            auto *ue = &tx_used->ring[tx_used->idx % txq.num];
            ue->id = desc_idx;
            ue->len = 0;
            __sync_synchronize(); // wmb
            tx_used->idx++;
            tx_last_avail++;
            did_work = true;
        }

        if (did_work) {
            vmm->net_irq_status_ |= 1;
            struct kvm_irq_level irq = {};
            irq.irq = VIRTIO_NET_MMIO_IRQ;
            irq.level = 0;
            ioctl(vmm->vm_fd_, KVM_IRQ_LINE, &irq);
            irq.level = 1;
            ioctl(vmm->vm_fd_, KVM_IRQ_LINE, &irq);
        }

        // ---- RX: drain available packets from TAP into guest ----
        {
            bool rx_did_work = false;
            __sync_synchronize();
            while (rx_last_used != rx_avail->idx) {
                ssize_t nr = ::read(vmm->tap_fd_, pkt_buf, sizeof(pkt_buf));
                if (nr <= 0) break;

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
            poll(pfds, 2, 10);
            if (pfds[1].revents & POLLIN) {
                uint64_t v;
                ssize_t nr = ::read(vmm->net_wakeup_fd_, &v, 8);
                (void)nr;
            }
        }
    }
    return nullptr;
}
