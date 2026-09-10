/*
 * Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL.
 * Unauthorized copying, reproduction, distribution, or modification of this
 * file, via any medium, is strictly prohibited.
 * All rights reserved.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>

#define VMM_MMIO_GPA   0xd0000000ULL
#define VMM_MMIO_MAGIC 0x48594C54U

extern char **environ;

static int kmsg_fd = -1;

static void log_msg(const char *msg) {
    if (kmsg_fd >= 0) {
        char buf[1024];
        int len = snprintf(buf, sizeof(buf), "%s\n", msg);
        if (len > 0) {
            ssize_t nw = write(kmsg_fd, buf, (size_t)len);
            (void)nw;
        }
    }
}

static void prefix_to_mask(int prefix, char *out, size_t out_sz) {
    if (prefix < 0) prefix = 0;
    if (prefix > 32) prefix = 32;
    uint32_t mask = (prefix == 0) ? 0 : (~0U << (32 - prefix));
    struct in_addr in;
    in.s_addr = htonl(mask);
    inet_ntop(AF_INET, &in, out, out_sz);
}

static void setup_network(const char *ip_cidr, const char *gw_str) {
    if (!ip_cidr || !ip_cidr[0]) return;

    char ip_str[64] = {};
    char mask_str[64] = "255.255.255.0";
    snprintf(ip_str, sizeof(ip_str), "%s", ip_cidr);

    char *slash = strchr(ip_str, '/');
    if (slash) {
        *slash = '\0';
        int prefix = atoi(slash + 1);
        prefix_to_mask(prefix, mask_str, sizeof(mask_str));
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct ifreq ifr = {};
    snprintf(ifr.ifr_name, IFNAMSIZ, "eth0");

    // 1. Bring interface UP
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) >= 0) {
        ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
        ioctl(sock, SIOCSIFFLAGS, &ifr);
    }

    // 2. Set IP
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip_str, &sin->sin_addr) == 1) {
        ioctl(sock, SIOCSIFADDR, &ifr);
    }

    // 3. Set Netmask
    if (inet_pton(AF_INET, mask_str, &sin->sin_addr) == 1) {
        ioctl(sock, SIOCSIFNETMASK, &ifr);
    }

    // 4. Set Default Route
    if (gw_str && gw_str[0]) {
        struct rtentry rt = {};
        struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
        struct sockaddr_in *gw  = (struct sockaddr_in *)&rt.rt_gateway;
        struct sockaddr_in *mask = (struct sockaddr_in *)&rt.rt_genmask;

        dst->sin_family = AF_INET;
        mask->sin_family = AF_INET;
        gw->sin_family = AF_INET;

        if (inet_pton(AF_INET, gw_str, &gw->sin_addr) == 1) {
            rt.rt_flags = RTF_UP | RTF_GATEWAY;
            rt.rt_dev = (char *)"eth0";
            ioctl(sock, SIOCADDRT, &rt);
        }
    }

    close(sock);
}

int main() {
    // 1. Initial mounts before snapshot
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mkdir("/tmp", 0777);
    mkdir("/mnt", 0755);
    mkdir("/share", 0755);

    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    log_msg("VMTAINER: init starting");

    // Rebind stdio to devtmpfs /dev/console if present
    int con_fd = open("/dev/console", O_RDWR);
    if (con_fd >= 0) {
        if (con_fd != 0) dup2(con_fd, 0);
        if (con_fd != 1) dup2(con_fd, 1);
        if (con_fd != 2) dup2(con_fd, 2);
        if (con_fd > 2) close(con_fd);
    }

    // 2. Signal VMM snapshot
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (mem_fd >= 0) {
        volatile uint32_t *mmio = (volatile uint32_t *)mmap(
            NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
            mem_fd, (off_t)VMM_MMIO_GPA);
        if (mmio != MAP_FAILED) {
            log_msg("VMTAINER: signalling snapshot");
            *mmio = VMM_MMIO_MAGIC; // <-- VMM traps here and takes snapshot!
            munmap((void *)mmio, 4096);
        }
        close(mem_fd);
    }

    // =========================================================================
    // RESTORE EXECUTION RESUMES HERE!
    // =========================================================================

    struct timespec tr0, tr1, tr2, tr3;
    clock_gettime(CLOCK_MONOTONIC, &tr0);

    // 3. Mount virtiofs
    if (mount("myfs", "/share", "virtiofs", 0, NULL) != 0) {
        log_msg("VMTAINER: mount failed");
        reboot(RB_HALT_SYSTEM);
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &tr1);

    // 4. Read entrypoint from /share/.entrypoint
    char entrypoint[1024] = {};
    int ep_fd = open("/share/.entrypoint", O_RDONLY);
    if (ep_fd >= 0) {
        ssize_t nr = read(ep_fd, entrypoint, sizeof(entrypoint) - 1);
        if (nr > 0) {
            entrypoint[nr] = '\0';
            char *nl = strchr(entrypoint, '\n');
            if (nl) *nl = '\0';
            nl = strchr(entrypoint, '\r');
            if (nl) *nl = '\0';
        }
        close(ep_fd);
    }

    // 5. Read config from /share/.vmconfig
    char hostname[256] = {};
    char net_ip[64] = {};
    char net_gw[64] = {};

    int cfg_fd = open("/share/.vmconfig", O_RDONLY);
    if (cfg_fd >= 0) {
        char cbuf[2048] = {};
        ssize_t c_nr = read(cfg_fd, cbuf, sizeof(cbuf) - 1);
        close(cfg_fd);
        if (c_nr > 0) {
            cbuf[c_nr] = '\0';
            char *line = strtok(cbuf, "\r\n");
            while (line) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p != '#' && *p != '\0') {
                    if (strncmp(p, "export ", 7) == 0) p += 7;
                    char *eq = strchr(p, '=');
                    if (eq) {
                        *eq = '\0';
                        char *key = p;
                        char *val = eq + 1;
                        size_t vlen = strlen(val);
                        if (vlen >= 2 && (val[0] == '"' || val[0] == '\'') && val[vlen - 1] == val[0]) {
                            val[vlen - 1] = '\0';
                            val++;
                        }
                        if (strcmp(key, "HOSTNAME") == 0) {
                            snprintf(hostname, sizeof(hostname), "%s", val);
                        } else if (strcmp(key, "NET_IP") == 0) {
                            snprintf(net_ip, sizeof(net_ip), "%s", val);
                        } else if (strcmp(key, "NET_GW") == 0) {
                            snprintf(net_gw, sizeof(net_gw), "%s", val);
                        } else {
                            setenv(key, val, 1);
                            if (strncmp(key, "ENV_", 4) == 0) {
                                setenv(key + 4, val, 1);
                            }
                        }
                    }
                }
                line = strtok(NULL, "\r\n");
            }
        }
    }

    // Default environment variables
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0);
    setenv("HOME", "/root", 0);
    setenv("TERM", "xterm", 0);

    // 6. Set hostname
    if (hostname[0]) {
        if (sethostname(hostname, strlen(hostname)) != 0) {
            // ignore
        }
        int h_fd = open("/proc/sys/kernel/hostname", O_WRONLY | O_CLOEXEC);
        if (h_fd >= 0) {
            ssize_t nw = write(h_fd, hostname, strlen(hostname)); (void)nw;
            close(h_fd);
        }
    }

    // 7. Configure network
    if (net_ip[0]) {
        setup_network(net_ip, net_gw);
    }
    clock_gettime(CLOCK_MONOTONIC, &tr2);

    // Fallback if no entrypoint
    if (!entrypoint[0]) {
        log_msg("VMTAINER: no entrypoint, halting");
        reboot(RB_HALT_SYSTEM);
        return 0;
    }

    // 8. Prepare container filesystems inside /share
    mkdir("/share/proc", 0755);
    mkdir("/share/sys", 0755);
    mkdir("/share/dev", 0755);
    mkdir("/share/tmp", 0777);
    mkdir("/share/dev/pts", 0755);
    mkdir("/share/dev/shm", 0777);

    mount("/proc", "/share/proc", NULL, MS_BIND, NULL);
    mount("/sys",  "/share/sys",  NULL, MS_BIND, NULL);
    mount("/dev",  "/share/dev",  NULL, MS_BIND, NULL);
    mount("devpts", "/share/dev/pts", "devpts", 0, NULL);

    chmod("/share/dev/null", 0666);
    chmod("/share/dev/zero", 0666);
    chmod("/share/dev/console", 0666);
    chmod("/share/dev/tty", 0666);
    chmod("/share/dev/ttyS0", 0666);
    chmod("/share/dev/urandom", 0666);
    clock_gettime(CLOCK_MONOTONIC, &tr3);

    double ms_mount = (tr1.tv_sec - tr0.tv_sec) * 1000.0 + (tr1.tv_nsec - tr0.tv_nsec) / 1000000.0;
    double ms_cfg   = (tr2.tv_sec - tr1.tv_sec) * 1000.0 + (tr2.tv_nsec - tr1.tv_nsec) / 1000000.0;
    double ms_prep  = (tr3.tv_sec - tr2.tv_sec) * 1000.0 + (tr3.tv_nsec - tr2.tv_nsec) / 1000000.0;

    char run_msg[1100];
    snprintf(run_msg, sizeof(run_msg), "VMTAINER: running entrypoint: %s [guest: mount=%.2fms cfg=%.2fms prep=%.2fms]",
             entrypoint, ms_mount, ms_cfg, ms_prep);
    log_msg(run_msg);

    // 9. Fork and execute entrypoint inside chroot
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork entrypoint");
        reboot(RB_HALT_SYSTEM);
        return 1;
    }

    if (pid == 0) {
        if (chroot("/share") != 0) { perror("chroot"); _exit(1); }
        if (chdir("/") != 0) { perror("chdir"); _exit(1); }

        char exec_cmd[1200];
        snprintf(exec_cmd, sizeof(exec_cmd), "exec %s", entrypoint);
        char *argv_sh[] = { (char *)"/bin/sh", (char *)"-c", exec_cmd, NULL };
        execve("/bin/sh", argv_sh, environ);

        // Fallback for distroless (no /bin/sh): tokenize entrypoint
        char ep_buf[1024];
        snprintf(ep_buf, sizeof(ep_buf), "%s", entrypoint);
        char *d_argv[64];
        int d_argc = 0;
        char *token = strtok(ep_buf, " \t\r\n");
        while (token && d_argc < 63) {
            d_argv[d_argc++] = token;
            token = strtok(NULL, " \t\r\n");
        }
        d_argv[d_argc] = NULL;
        if (d_argc > 0) {
            execve(d_argv[0], d_argv, environ);
        }

        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    char exit_msg[64];
    snprintf(exit_msg, sizeof(exit_msg), "VMTAINER: entrypoint exited (%d)", rc);
    log_msg(exit_msg);

    sync();
    reboot(RB_POWER_OFF);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
    return rc;
}
