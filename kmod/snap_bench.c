/*
 * snap_bench.c - Benchmark vmtainer snapshot restore from:
 *
 *   1. /dev/vmtainer_snap (vmalloc-backed, always-resident memory)
 *   2. Regular file-backed mmap
 *   3. read() into a userspace buffer
 *
 * Each restore uses the same sparse-copy algorithm vmtainer uses:
 * parse the snapshot header, read the dirty bitmap, and copy only
 * the 4 KB pages whose bit is set.
 *
 * Build: gcc -O2 -D_GNU_SOURCE -o snap_bench snap_bench.c
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/memfd.h>

#include "vmtainer_snap.h"

#define DEV_PATH	"/dev/vmtainer_snap"
#define PAGE_SZ		4096

/*
 * vmtainer snapshot magic and header field offsets.
 * The SnapshotHeader struct is ~4000 bytes, but we only need a few
 * fields to locate the bitmap and RAM.  We read them at fixed byte
 * offsets to avoid depending on linux/kvm.h in the benchmark tool.
 */
#define SNAP_MAGIC	0x48594C54534E4150ULL  /* "PANSTLYH" le */
#define OFF_MAGIC	0
#define OFF_VERSION	8
#define OFF_RAM_MB	12
#define OFF_CPUID_NENT	3812
#define OFF_NUM_MSRS	3816
#define OFF_XSAVE_SIZE	3820
#define OFF_BM_BYTES	3992
#define HDR_SIZE	4000

/* Sizes of variable-length entries following the header */
#define SIZEOF_CPUID_ENTRY	40  /* sizeof(kvm_cpuid_entry2) */
#define SIZEOF_MSR_ENTRY	16  /* sizeof(kvm_msr_entry) */

struct snap_layout {
	uint64_t	total_size;
	uint64_t	page_size;
	uint64_t	bitmap_offset;
	uint64_t	bitmap_size;
	uint64_t	ram_offset;
	uint64_t	ram_size;
};

static void die(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

static uint64_t ns_now(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		die("clock_gettime");
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double ns_to_ms(uint64_t ns)
{
	return (double)ns / 1e6;
}

static void print_stats(const char *label, const uint64_t *times, int n)
{
	uint64_t min = times[0], max = times[0], sum = 0;
	for (int i = 0; i < n; i++) {
		if (times[i] < min) min = times[i];
		if (times[i] > max) max = times[i];
		sum += times[i];
	}
	double avg = ns_to_ms(sum) / n;
	printf("%-30s min=%7.3f ms  avg=%7.3f ms  max=%7.3f ms  (%d runs)\n",
	       label, ns_to_ms(min), avg, ns_to_ms(max), n);
}

static int memfd_create_sys(const char *name, unsigned int flags)
{
	#ifdef __NR_memfd_create
	return (int)syscall(__NR_memfd_create, name, flags);
	#else
	(void)name; (void)flags;
	errno = ENOSYS;
	return -1;
	#endif
}

static void *alloc_guest_ram(uint64_t size)
{
	int fd = memfd_create_sys("guest_ram", MFD_CLOEXEC);
	if (fd < 0)
		die("memfd_create");

	if (ftruncate(fd, (off_t)size) < 0)
		die("ftruncate guest_ram");

	void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		die("mmap guest_ram");

	close(fd);
	return p;
}

static void reset_guest_ram(void *p, uint64_t size)
{
	/*
	 * Drop the destination pages so the next restore has to fault them
	 * back in, just like a freshly created guest RAM memfd.
	 */
	if (madvise(p, (size_t)size, MADV_DONTNEED) < 0)
		die("madvise MADV_DONTNEED");
}

static void copy_all(int fd_in, void *dst, size_t len)
{
	char *d = dst;
	size_t left = len;
	while (left > 0) {
		ssize_t n = read(fd_in, d, left);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("read snapshot");
		}
		if (n == 0) {
			fprintf(stderr, "unexpected EOF after %zu bytes\n",
				len - left);
			exit(EXIT_FAILURE);
		}
		d += n;
		left -= n;
	}
}

static int read_all(int fd, void *buf, size_t len)
{
	char *b = buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n = read(fd, b, left);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		b += n;
		left -= n;
	}
	return (left == 0) ? 0 : -1;
}

/*
 * Parse a real vmtainer snapshot (magic 0x48594C54534E4150, version 7).
 * Layout: SnapshotHeader(4000B) | xsave[xsave_size] |
 *         kvm_cpuid_entry2[cpuid_nent] | kvm_msr_entry[num_msrs] |
 *         dirty_bitmap[bitmap_bytes] | guest_ram[ram_mb * 1M]
 */
static int parse_snapshot(const uint8_t *base, size_t file_size,
			  struct snap_layout *out)
{
	memset(out, 0, sizeof(*out));
	out->page_size = PAGE_SZ;
	out->total_size = file_size;

	if (file_size < (size_t)HDR_SIZE)
		return -1;

	uint64_t magic;
	uint32_t version, ram_mb, cpuid_nent, num_msrs, xsave_size, bm_bytes;
	memcpy(&magic,      base + OFF_MAGIC,      sizeof(magic));
	memcpy(&version,    base + OFF_VERSION,     sizeof(version));
	memcpy(&ram_mb,     base + OFF_RAM_MB,      sizeof(ram_mb));
	memcpy(&cpuid_nent, base + OFF_CPUID_NENT,  sizeof(cpuid_nent));
	memcpy(&num_msrs,   base + OFF_NUM_MSRS,    sizeof(num_msrs));
	memcpy(&xsave_size, base + OFF_XSAVE_SIZE,  sizeof(xsave_size));
	memcpy(&bm_bytes,   base + OFF_BM_BYTES,    sizeof(bm_bytes));

	if (magic != SNAP_MAGIC) {
		fprintf(stderr, "bad magic: 0x%016lx (expected 0x%016lx)\n",
			(unsigned long)magic, (unsigned long)SNAP_MAGIC);
		return -1;
	}
	printf("  header: ver=%u ram=%uMB cpuid=%u msrs=%u xsave=%u bitmap=%u\n",
	       version, ram_mb, cpuid_nent, num_msrs, xsave_size, bm_bytes);

	uint64_t bm_off = HDR_SIZE + xsave_size
			+ (uint64_t)cpuid_nent * SIZEOF_CPUID_ENTRY
			+ (uint64_t)num_msrs * SIZEOF_MSR_ENTRY;
	uint64_t ram_off = bm_off + bm_bytes;
	uint64_t ram_size = (uint64_t)ram_mb * 1024 * 1024;

	if (ram_off + ram_size > file_size) {
		fprintf(stderr, "snapshot truncated: ram_off=%lu + ram=%lu > file=%zu\n",
			(unsigned long)ram_off, (unsigned long)ram_size, file_size);
		return -1;
	}

	out->bitmap_offset = bm_off;
	out->bitmap_size = bm_bytes;
	out->ram_offset = ram_off;
	out->ram_size = ram_size;

	return 0;
}

/*
 * Sparse restore: copy only the dirty 4 KB pages from the source RAM blob
 * to the destination guest RAM.
 */
static void sparse_restore(const uint8_t *src_ram, uint8_t *dest,
			   const uint8_t *bitmap, uint64_t bitmap_size,
			   uint64_t ram_size, uint64_t page_size)
{
	uint64_t pages = ram_size / page_size;
	uint64_t max_pages = bitmap_size * 8;

	if (pages > max_pages)
		pages = max_pages;

	for (uint64_t i = 0; i < pages; i++) {
		uint64_t byte = i / 8;
		unsigned int bit = (unsigned int)(i % 8);
		if (bitmap[byte] & (1U << bit))
			memcpy(dest + i * page_size,
			       src_ram + i * page_size,
			       (size_t)page_size);
	}
}

static void bench_kmod(const struct snap_layout *layout,
		       const uint8_t *kmod_base, uint8_t *dest,
		       int iterations)
{
	const uint8_t *bitmap = kmod_base + layout->bitmap_offset;
	const uint8_t *src_ram = kmod_base + layout->ram_offset;
	uint64_t *times = calloc(iterations, sizeof(*times));
	if (!times)
		die("calloc times");

	for (int i = 0; i < iterations; i++) {
		reset_guest_ram(dest, layout->ram_size);
		uint64_t t0 = ns_now();
		sparse_restore(src_ram, dest, bitmap, layout->bitmap_size,
			       layout->ram_size, layout->page_size);
		uint64_t t1 = ns_now();
		times[i] = t1 - t0;
	}

	print_stats("kmod mmap (warm)", times, iterations);
	free(times);
}

static void bench_file_mmap(const char *path, size_t file_size,
			    const struct snap_layout *layout,
			    uint8_t *dest, int iterations, int cold)
{
	int fd;
	uint64_t *times = calloc(iterations, sizeof(*times));
	if (!times)
		die("calloc times");

	if (cold) {
		for (int i = 0; i < iterations; i++) {
			fd = open(path, O_RDONLY | O_CLOEXEC);
			if (fd < 0)
				die("open snapshot for mmap");

			/* Try to drop the file from the page cache. */
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

			const uint8_t *src = (const uint8_t *)mmap(NULL, file_size,
							    PROT_READ,
							    MAP_PRIVATE, fd, 0);
			if (src == MAP_FAILED)
				die("mmap snapshot");

			reset_guest_ram(dest, layout->ram_size);
			uint64_t t0 = ns_now();
			sparse_restore(src + layout->ram_offset, dest,
				     src + layout->bitmap_offset,
				     layout->bitmap_size, layout->ram_size,
				     layout->page_size);
			uint64_t t1 = ns_now();
			times[i] = t1 - t0;

			munmap((void *)src, file_size);
			close(fd);
		}
	} else {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			die("open snapshot for mmap");

		const uint8_t *src = (const uint8_t *)mmap(NULL, file_size,
						    PROT_READ, MAP_PRIVATE,
						    fd, 0);
		if (src == MAP_FAILED)
			die("mmap snapshot");

		for (int i = 0; i < iterations; i++) {
			reset_guest_ram(dest, layout->ram_size);
			uint64_t t0 = ns_now();
			sparse_restore(src + layout->ram_offset, dest,
				     src + layout->bitmap_offset,
				     layout->bitmap_size, layout->ram_size,
				     layout->page_size);
			uint64_t t1 = ns_now();
			times[i] = t1 - t0;
		}

		munmap((void *)src, file_size);
		close(fd);
	}

	print_stats(cold ? "file mmap (cold)" : "file mmap (warm)",
		    times, iterations);
	free(times);
}

static void bench_file_read(const char *path, size_t file_size,
			    const struct snap_layout *layout,
			    uint8_t *dest, int iterations, int cold)
{
	uint64_t *times = calloc(iterations, sizeof(*times));
	if (!times)
		die("calloc times");

	if (cold) {
		for (int i = 0; i < iterations; i++) {
			int fd = open(path, O_RDONLY | O_CLOEXEC);
			if (fd < 0)
				die("open snapshot for read");

			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

			/* Anonymous buffer so read() faults in fresh pages. */
			uint8_t *buf = (uint8_t *)mmap(NULL, file_size,
						       PROT_READ | PROT_WRITE,
						       MAP_PRIVATE | MAP_ANONYMOUS,
						       -1, 0);
			if (buf == MAP_FAILED)
				die("mmap anonymous read buffer");

			if (read_all(fd, buf, file_size) < 0)
				die("read snapshot into buffer");
			close(fd);

			reset_guest_ram(dest, layout->ram_size);
			uint64_t t0 = ns_now();
			sparse_restore(buf + layout->ram_offset, dest,
				     buf + layout->bitmap_offset,
				     layout->bitmap_size, layout->ram_size,
				     layout->page_size);
			uint64_t t1 = ns_now();
			times[i] = t1 - t0;

			munmap(buf, file_size);
		}
	} else {
		uint8_t *buf = (uint8_t *)mmap(NULL, file_size,
					       PROT_READ | PROT_WRITE,
					       MAP_PRIVATE | MAP_ANONYMOUS,
					       -1, 0);
		if (buf == MAP_FAILED)
			die("mmap anonymous read buffer");

		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			die("open snapshot for read");
		if (read_all(fd, buf, file_size) < 0)
			die("read snapshot into buffer");
		close(fd);

		for (int i = 0; i < iterations; i++) {
			reset_guest_ram(dest, layout->ram_size);
			uint64_t t0 = ns_now();
			sparse_restore(buf + layout->ram_offset, dest,
				     buf + layout->bitmap_offset,
				     layout->bitmap_size, layout->ram_size,
				     layout->page_size);
			uint64_t t1 = ns_now();
			times[i] = t1 - t0;
		}

		munmap(buf, file_size);
	}

	print_stats(cold ? "file read (cold)" : "file read (warm)",
		    times, iterations);
	free(times);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <golden.snap>\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *snap_path = argv[1];

	struct stat st;
	if (stat(snap_path, &st) < 0)
		die("stat snapshot");
	size_t file_size = st.st_size;

	/*
	 * 1. Open the kernel device, allocate a buffer, and mmap it.
	 */
	int dev = open(DEV_PATH, O_RDWR | O_CLOEXEC);
	if (dev < 0)
		die("open " DEV_PATH);

	size_t alloc_size = file_size;
	/* Ensure enough room for the full snapshot. */

	if (ioctl(dev, VMTAINER_SNAP_ALLOC, &alloc_size) < 0)
		die("VMTAINER_SNAP_ALLOC");

	struct vmtainer_snap_info info;
	if (ioctl(dev, VMTAINER_SNAP_INFO, &info) < 0)
		die("VMTAINER_SNAP_INFO");

	uint8_t *kmod_base = (uint8_t *)mmap(NULL, (size_t)info.size,
					     PROT_READ | PROT_WRITE,
					     MAP_SHARED, dev, 0);
	if (kmod_base == MAP_FAILED)
		die("mmap " DEV_PATH);

	/*
	 * 2. Copy the snapshot file into the kernel buffer.  This is the
	 *    one-time "load" step that vmtainer boot --snapshot would do.
	 */
	{
		int fd = open(snap_path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			die("open snapshot for kmod load");

		uint64_t t0 = ns_now();
		copy_all(fd, kmod_base, file_size);
		uint64_t t1 = ns_now();
		printf("loaded snapshot into %s in %.3f ms (size=%zu bytes)\n",
		       DEV_PATH, ns_to_ms(t1 - t0), file_size);
		close(fd);
	}

	/*
	 * 3. Parse snapshot layout (header + dirty bitmap + RAM).
	 */
	struct snap_layout layout;
	if (parse_snapshot(kmod_base, file_size, &layout) < 0) {
		fprintf(stderr, "failed to parse snapshot header/layout\n");
		return EXIT_FAILURE;
	}

	printf("snapshot layout: page_size=%lu bitmap_off=%lu bitmap_sz=%lu "
	       "ram_off=%lu ram_sz=%lu\n",
	       (unsigned long)layout.page_size,
	       (unsigned long)layout.bitmap_offset,
	       (unsigned long)layout.bitmap_size,
	       (unsigned long)layout.ram_offset,
	       (unsigned long)layout.ram_size);

	if (layout.ram_size == 0) {
		fprintf(stderr, "snapshot has no RAM region\n");
		return EXIT_FAILURE;
	}

	/*
	 * 4. Create the guest RAM memfd.  The restore benchmark copies dirty
	 *    pages into this region.
	 */
	uint8_t *guest_ram = (uint8_t *)alloc_guest_ram(layout.ram_size);

	const int iterations = 10;

	printf("\n=== Snapshot restore benchmarks ===\n");
	bench_kmod(&layout, kmod_base, guest_ram, iterations);
	bench_file_mmap(snap_path, file_size, &layout, guest_ram,
			iterations, /*cold=*/0);
	bench_file_mmap(snap_path, file_size, &layout, guest_ram,
			iterations, /*cold=*/1);
	bench_file_read(snap_path, file_size, &layout, guest_ram,
			iterations, /*cold=*/0);
	bench_file_read(snap_path, file_size, &layout, guest_ram,
			iterations, /*cold=*/1);

	munmap(kmod_base, (size_t)info.size);
	close(dev);
	munmap(guest_ram, (size_t)layout.ram_size);

	return 0;
}
