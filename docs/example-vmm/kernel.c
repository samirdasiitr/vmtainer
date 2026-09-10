/*
 * kernel.c
 *
 * Loads an x86 bzImage and an optional initrd into the guest memory that has
 * already been mapped by the VMM.  This file demonstrates the Linux x86
 * boot protocol: setup_header at 0x1f1 in the boot sector, real-mode setup
 * copied to 0x10000, protected payload at 0x100000, and an E820 map placed
 * in the zero-page at 0x10000.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vmm.h"
#include "kernel.h"

/*
 * Read an entire file into a heap buffer.  Returns 0 on success and sets
 * *out / *out_size.  On error returns -1 and logs.
 */
static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG("cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        LOG("fstat %s failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    *out_size = (size_t)st.st_size;
    *out = malloc(*out_size);
    if (!*out) {
        LOG("malloc for %s failed", path);
        close(fd);
        return -1;
    }

    size_t off = 0;
    while (off < *out_size) {
        ssize_t n = read(fd, *out + off, *out_size - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOG("read %s failed: %s", path, strerror(errno));
            free(*out);
            close(fd);
            return -1;
        }
        if (n == 0) {
            /* File truncated? Stop with what we have. */
            *out_size = off;
            break;
        }
        off += (size_t)n;
    }

    close(fd);
    LOG("read %s (%zu bytes)", path, *out_size);
    return 0;
}

/*
 * Place the kernel command line in low guest memory and point the setup_header
 * at it.  The buffer is below the setup code so it is safely out of the way.
 */
static void install_cmdline(struct vmm *vmm, struct setup_header *sh,
                            const char *cmdline)
{
    /*
     * The command line lives at 0x20000.  It must be below the setup heap,
     * and the pointer is a 32-bit linear address.
     */
    const uint32_t cmdline_load = 0x20000;
    size_t len = strlen(cmdline) + 1;

    if (len > 0x1000) {
        LOG("command line too long, truncating");
        len = 0x1000;
    }

    memset(vmm->mem + cmdline_load, 0, 0x1000);
    memcpy(vmm->mem + cmdline_load, cmdline, len - 1);
    vmm->mem[cmdline_load + len - 1] = '\0';

    sh->cmd_line_ptr = cmdline_load;
    sh->cmdline_size = (uint32_t)len;
    LOG("command line at 0x%08x: %s", cmdline_load, cmdline);
}

/*
 * The main bzImage / initrd loader.  See Documentation/x86/boot.txt in the
 * Linux source for the full boot protocol.
 */
int kernel_load(struct vmm *vmm, const char *kernel_path,
                const char *initrd_path, const char *cmdline)
{
    uint8_t *kbuf = NULL;
    size_t ksize = 0;

    LOG("loading kernel from %s", kernel_path);
    if (read_file(kernel_path, &kbuf, &ksize) < 0)
        return -1;

    /* A bzImage must be at least large enough to contain the setup header. */
    if (ksize < 0x270) {
        LOG("kernel image too small");
        free(kbuf);
        return -1;
    }

    struct setup_header *sh = (struct setup_header *)(kbuf + 0x1F1);

    if (sh->boot_flag != 0xAA55) {
        LOG("kernel missing boot signature 0xAA55 (got 0x%04x)",
            sh->boot_flag);
        free(kbuf);
        return -1;
    }

    if (memcmp(sh->magic, "HdrS", 4) != 0) {
        LOG("kernel missing HdrS magic");
        free(kbuf);
        return -1;
    }

    LOG("bzImage protocol version 0x%04x", sh->version);
    if (sh->version < 0x0200) {
        LOG("protocol version 0x%04x too old", sh->version);
        free(kbuf);
        return -1;
    }

    /*
     * setup_sects is the number of 512-byte sectors after the boot sector.
     * If it is 0 the build treated it as 4, so the total setup size is
     * (setup_sects + 1) * 512.
     */
    unsigned int setup_sects = sh->setup_sects ? sh->setup_sects : 4;
    size_t setup_size = (setup_sects + 1) * 512ULL;
    if (setup_size > ksize) {
        LOG("setup size %zu larger than file %zu", setup_size, ksize);
        free(kbuf);
        return -1;
    }
    LOG("setup_sects=%u setup_size=%zu", setup_sects, setup_size);

    /*
     * The protected (compressed) payload follows the real-mode setup in
     * the bzImage file.  Use the setup size as the payload offset, matching
     * how vmtainer's loader works for this kernel image.
     */
    size_t payload_offset = setup_size;
    size_t payload_length = ksize - payload_offset;

    if (payload_length == 0) {
        LOG("kernel has no protected payload");
        free(kbuf);
        return -1;
    }
    LOG("payload offset=%zu length=%zu", payload_offset, payload_length);

    /*
     * Copy the real-mode setup to 0x10000 (0x1000:0 in real-mode segments).
     * Copy the compressed / protected payload to 0x100000.
     */
    memcpy(vmm->mem + 0x10000, kbuf, setup_size);
    memcpy(vmm->mem + 0x100000, kbuf + payload_offset, payload_length);
    free(kbuf);

    /*
     * Now that the setup is in guest memory, point gsh at the in-guest
     * setup_header at 0x10000 + 0x1F1.
     */
    struct setup_header *gsh = (struct setup_header *)(vmm->mem + 0x10000 + 0x1F1);

    /* --- load the initrd if provided ------------------------------ */
    uint32_t initrd_load = 0;
    uint32_t initrd_size = 0;

    if (initrd_path) {
        uint8_t *ibuf = NULL;
        size_t isize = 0;

        if (read_file(initrd_path, &ibuf, &isize) < 0)
            return -1;

        if (isize > vmm->mem_size) {
            LOG("initrd bigger than guest memory");
            free(ibuf);
            return -1;
        }

        /*
         * Place the initrd near the top of memory, 4 KiB aligned, and make
         * sure it does not overlap the protected kernel (which lives at
         * 0x100000 and is at most a few megabytes for a tiny kernel).
         */
        uint64_t top = vmm->mem_size;
        uint64_t start = (top - isize) & ~0xFFFULL;
        uint64_t min_start = 0x100000ULL + payload_length + 0x100000ULL;

        if (start < min_start || start < 0x100000ULL) {
            LOG("not enough guest memory for initrd");
            free(ibuf);
            return -1;
        }

        if (start + isize > gsh->initrd_addr_max && gsh->initrd_addr_max) {
            LOG("warning: initrd address 0x%08llx above initrd_addr_max 0x%08x",
                (unsigned long long)(start + isize), gsh->initrd_addr_max);
        }

        initrd_load = (uint32_t)start;
        initrd_size = (uint32_t)isize;

        memcpy(vmm->mem + start, ibuf, isize);
        free(ibuf);
        LOG("initrd at 0x%08x size %u", initrd_load, initrd_size);
    }

    /* --- set up the setup_header ---------------------------------- */
    gsh->type_of_loader = 0xFF;          /* generic / custom loader   */
    gsh->loadflags = (gsh->loadflags & ~0x02) | 0x81; /* LOADED_HIGH | CAN_USE_HEAP, no KASLR */
    gsh->code32_start = 0x100000;        /* protected kernel start    */
    gsh->vid_mode = 0;

    /*
     * The setup heap starts after the real-mode setup and ends at
     * 0x10000 + heap_end_ptr.  0xfe00 means the heap ends just before
     * the command line at 0x20000, matching vmtainer's layout.
     */
    gsh->heap_end_ptr = 0xfe00;

    if (gsh->kernel_alignment == 0)
        gsh->kernel_alignment = 0x1000;

    /* Install the command line.  The E820 map lives in the EBDA at
     * 0x9fc00 and is consumed by the BIOS int15h/ah=e820 handler. */
    install_cmdline(vmm, gsh, cmdline);

    /* Finally, record the initrd location in the header. */
    gsh->ramdisk_image = initrd_load;
    gsh->ramdisk_size  = initrd_size;

    LOG("kernel setup complete; entry at 0x100000, real-mode at 0x10000");
    return 0;
}
