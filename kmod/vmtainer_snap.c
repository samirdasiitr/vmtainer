/*
 * vmtainer_snap.c - Character device that keeps a VM snapshot in
 * vmalloc()-allocated, always-resident kernel memory and maps it into
 * userspace with remap_vmalloc_range().
 *
 * The device is intended to be used by vmtainer's restore path as a
 * faster alternative to file-backed mmap: the snapshot pages are
 * kernel-allocated and never paged out, so restoring a micro-VM from
 * /dev/vmtainer_snap avoids filesystem/page-cache/page-fault overhead.
 *
 * Target kernel: Linux 6.10
 */

#define pr_fmt(fmt) "vmtainer_snap: " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "vmtainer_snap.h"

/*
 * Per-file-descriptor state.  Every open() gets its own allocation so
 * multiple userspace processes/threads can use the device concurrently.
 */
struct vmtainer_snap_ctx {
	void		*buf;
	size_t		size;
	struct mutex	lock;
};

static int vmtainer_snap_open(struct inode *inode, struct file *filp)
{
	struct vmtainer_snap_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	filp->private_data = ctx;
	pr_info("opened fd %p\n", filp);
	return 0;
}

static int vmtainer_snap_release(struct inode *inode, struct file *filp)
{
	struct vmtainer_snap_ctx *ctx = filp->private_data;

	if (!ctx)
		return 0;

	mutex_lock(&ctx->lock);
	if (ctx->buf) {
		vfree(ctx->buf);
		ctx->buf = NULL;
		ctx->size = 0;
	}
	mutex_unlock(&ctx->lock);

	kfree(ctx);
	pr_info("released fd %p\n", filp);
	return 0;
}

static long vmtainer_snap_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	struct vmtainer_snap_ctx *ctx = filp->private_data;
	int ret = 0;

	if (!ctx)
		return -EINVAL;

	switch (cmd) {
	case VMTAINER_SNAP_ALLOC: {
		unsigned long req_size;
		void *new_buf;
		size_t aligned_size;

		if (copy_from_user(&req_size, (void __user *)arg,
				   sizeof(req_size)))
			return -EFAULT;

		if (req_size == 0)
			return -EINVAL;

		aligned_size = PAGE_ALIGN(req_size);
		if (aligned_size == 0)
			return -EINVAL;

		mutex_lock(&ctx->lock);

		/* Replace any previous allocation on this fd. */
		if (ctx->buf) {
			vfree(ctx->buf);
			ctx->buf = NULL;
			ctx->size = 0;
		}

		/*
		 * vmalloc_user() is used instead of vmalloc() so the area is
		 * marked VM_USERMAP and can be exported to userspace through
		 * remap_vmalloc_range().  The pages are normal kernel RAM
		 * pages and are not swapable; they remain resident until the
		 * device is closed.
		 */
		new_buf = vmalloc_user(aligned_size);
		if (!new_buf) {
			mutex_unlock(&ctx->lock);
			pr_err("failed to allocate %zu bytes\n", aligned_size);
			return -ENOMEM;
		}

		/* Don't leak old kernel memory to userspace. */
		memset(new_buf, 0, aligned_size);

		ctx->buf = new_buf;
		ctx->size = aligned_size;
		mutex_unlock(&ctx->lock);

		pr_info("allocated %zu bytes for fd %p (requested %lu)\n",
			aligned_size, filp, req_size);
		break;
	}

	case VMTAINER_SNAP_INFO: {
		struct vmtainer_snap_info info = { 0 };

		mutex_lock(&ctx->lock);
		info.size = ctx->size;
		info.phys_addr = 0UL;
		mutex_unlock(&ctx->lock);

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		break;
	}

	default:
		ret = -ENOTTY;
	}

	return ret;
}

static int vmtainer_snap_mmap(struct file *filp,
				  struct vm_area_struct *vma)
{
	struct vmtainer_snap_ctx *ctx = filp->private_data;
	unsigned long wanted;
	int ret = -EINVAL;

	if (!ctx)
		return -EINVAL;

	/* Only whole-buffer mmap from offset 0 is supported. */
	if (vma->vm_pgoff != 0)
		return -EINVAL;

	wanted = vma->vm_end - vma->vm_start;

	mutex_lock(&ctx->lock);

	if (!ctx->buf) {
		pr_err("mmap on fd %p before allocation\n", filp);
		ret = -EINVAL;
		goto out_unlock;
	}

	if (wanted > ctx->size) {
		pr_err("mmap size %lu exceeds allocation %zu\n", wanted, ctx->size);
		ret = -EINVAL;
		goto out_unlock;
	}

	/*
	 * The buffer was allocated with vmalloc_user(), which is backed by
	 * struct pages.  remap_vmalloc_range() maps those pages directly
	 * into the calling process' address space, giving userspace zero-copy
	 * read/write access to the kernel-resident snapshot.
	 */
	vm_flags_set(vma, VM_DONTEXPAND);
	ret = remap_vmalloc_range(vma, ctx->buf, 0);
	if (ret) {
		pr_err("remap_vmalloc_range failed for fd %p: %d\n",
		       filp, ret);
		goto out_unlock;
	}

	pr_info("mapped %lu bytes for fd %p\n", wanted, filp);

out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

static const struct file_operations vmtainer_snap_fops = {
	.owner		= THIS_MODULE,
	.open		= vmtainer_snap_open,
	.release	= vmtainer_snap_release,
	.unlocked_ioctl	= vmtainer_snap_ioctl,
	.mmap		= vmtainer_snap_mmap,
	.llseek		= no_llseek,
};

static struct miscdevice vmtainer_snap_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "vmtainer_snap",
	.fops	= &vmtainer_snap_fops,
	.mode	= 0666,
};

static int __init vmtainer_snap_init(void)
{
	int ret;

	ret = misc_register(&vmtainer_snap_misc);
	if (ret) {
		pr_err("misc_register failed: %d\n", ret);
		return ret;
	}

	pr_info("device /dev/%s registered\n", vmtainer_snap_misc.name);
	return 0;
}

static void __exit vmtainer_snap_exit(void)
{
	misc_deregister(&vmtainer_snap_misc);
	pr_info("device /dev/%s unregistered\n", vmtainer_snap_misc.name);
}

module_init(vmtainer_snap_init);
module_exit(vmtainer_snap_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vmtainer developers");
MODULE_DESCRIPTION("vmtainer resident-memory snapshot device");
