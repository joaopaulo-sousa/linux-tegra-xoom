// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/nvhost-ctrl for the NVIDIA (L4T) media libraries: the control node
 * of the downstream host1x driver (drivers/video/tegra/host), reimplemented
 * on top of the mainline host1x driver.
 *
 * Syncpoint reads, increments and waits go to the mainline syncpoints with
 * the same ids. The module mutexes are kept in software: on Tegra20 nothing
 * else uses the host1x hardware locks, so they only have to exclude the
 * clients of this node from each other. Module register access is not
 * provided; requests for it are logged.
 */
#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/host1x.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include "nvhost_ctrl.h"

#define NVHOST_NB_MLOCKS	16	/* Tegra20 */

static bool nvhost_debug;
module_param(nvhost_debug, bool, 0644);
MODULE_PARM_DESC(nvhost_debug, "log every nvhost-ctrl request");

#define DBG(fmt, ...) do {						\
	if (nvhost_debug)						\
		pr_info("nvhost-ctrl: " fmt, ##__VA_ARGS__);		\
} while (0)

struct nvhost_ctrl_ctx {
	unsigned long locks;		/* module mutexes held by this file */
};

static struct platform_device *host1x_pdev;
static struct host1x *host1x;
static DEFINE_SPINLOCK(mlock_lock);
static unsigned long mlocks;		/* module mutexes held by anyone */

static struct host1x_syncpt *nvhost_syncpt(u32 id)
{
	return host1x_syncpt_get_by_id_noref(host1x, id);
}

static int nvhost_ctrl_wait(u32 id, u32 thresh, s32 timeout_ms, u32 *value)
{
	struct host1x_syncpt *sp = nvhost_syncpt(id);
	long timeout;
	int err;

	if (!sp)
		return -EINVAL;

	if (timeout_ms == NVHOST_NO_TIMEOUT)
		timeout = -1;
	else
		timeout = msecs_to_jiffies(timeout_ms);

	err = host1x_syncpt_wait(sp, thresh, timeout, value);
	DBG("wait id %u thresh %u timeout %d -> %d (value %u)\n",
	    id, thresh, timeout_ms, err, value ? *value : 0);
	return err;
}

static int nvhost_ctrl_mutex(struct nvhost_ctrl_ctx *ctx,
			     struct nvhost_ctrl_module_mutex_args *args)
{
	int err = 0;

	if (args->id >= NVHOST_NB_MLOCKS || args->lock > 1)
		return -EINVAL;

	spin_lock(&mlock_lock);
	if (args->lock && !test_bit(args->id, &ctx->locks)) {
		/* id 0 is the host busy count of the original */
		if (args->id && test_bit(args->id, &mlocks))
			err = -EBUSY;
		else {
			__set_bit(args->id, &mlocks);
			__set_bit(args->id, &ctx->locks);
		}
	} else if (!args->lock && test_bit(args->id, &ctx->locks)) {
		if (args->id)
			__clear_bit(args->id, &mlocks);
		__clear_bit(args->id, &ctx->locks);
	}
	spin_unlock(&mlock_lock);

	DBG("mutex id %u lock %u -> %d\n", args->id, args->lock, err);
	return err;
}

static long nvhost_ctrl_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	struct nvhost_ctrl_ctx *ctx = filp->private_data;
	union {
		struct nvhost_get_param_args param;
		struct nvhost_ctrl_syncpt_read_args read;
		struct nvhost_ctrl_syncpt_incr_args incr;
		struct nvhost_ctrl_syncpt_wait_args wait;
		struct nvhost_ctrl_syncpt_waitex_args waitex;
		struct nvhost_ctrl_module_mutex_args mutex;
		struct nvhost_ctrl_module_regrdwr_args regrdwr;
	} buf;
	struct host1x_syncpt *sp;
	int err = 0;

	if (_IOC_TYPE(cmd) != NVHOST_IOCTL_MAGIC ||
	    _IOC_SIZE(cmd) > sizeof(buf))
		return -ENOTTY;

	if ((_IOC_DIR(cmd) & _IOC_WRITE) &&
	    copy_from_user(&buf, (void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	switch (cmd) {
	case NVHOST_IOCTL_CTRL_SYNCPT_READ:
		sp = nvhost_syncpt(buf.read.id);
		if (!sp)
			return -EINVAL;
		buf.read.value = host1x_syncpt_read(sp);
		DBG("read id %u -> %u\n", buf.read.id, buf.read.value);
		break;
	case NVHOST_IOCTL_CTRL_SYNCPT_INCR:
		sp = nvhost_syncpt(buf.incr.id);
		if (!sp)
			return -EINVAL;
		err = host1x_syncpt_incr(sp);
		DBG("incr id %u -> %d\n", buf.incr.id, err);
		break;
	case NVHOST_IOCTL_CTRL_SYNCPT_WAIT:
		err = nvhost_ctrl_wait(buf.wait.id, buf.wait.thresh,
				       buf.wait.timeout, NULL);
		break;
	case NVHOST_IOCTL_CTRL_SYNCPT_WAITEX:
		err = nvhost_ctrl_wait(buf.waitex.id, buf.waitex.thresh,
				       buf.waitex.timeout, &buf.waitex.value);
		break;
	case NVHOST_IOCTL_CTRL_MODULE_MUTEX:
		err = nvhost_ctrl_mutex(ctx, &buf.mutex);
		break;
	case NVHOST_IOCTL_CTRL_MODULE_REGRDWR:
		pr_info_ratelimited("nvhost-ctrl: %s: module %u regs %ux%u not provided\n",
				    current->comm, buf.regrdwr.id,
				    buf.regrdwr.num_offsets,
				    buf.regrdwr.block_size);
		return -EINVAL;
	case NVHOST_IOCTL_CTRL_GET_VERSION:
		buf.param.value = NVHOST_SUBMIT_VERSION_MAX_SUPPORTED;
		DBG("get version\n");
		break;
	default:
		pr_info_ratelimited("nvhost-ctrl: %s: unknown ioctl %#x\n",
				    current->comm, cmd);
		return -ENOTTY;
	}

	if (!err && (_IOC_DIR(cmd) & _IOC_READ) &&
	    copy_to_user((void __user *)arg, &buf, _IOC_SIZE(cmd)))
		return -EFAULT;

	return err;
}

static int nvhost_ctrl_open(struct inode *inode, struct file *filp)
{
	struct nvhost_ctrl_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;

	DBG("open by %s\n", current->comm);
	filp->private_data = ctx;
	return nonseekable_open(inode, filp);
}

static int nvhost_ctrl_release(struct inode *inode, struct file *filp)
{
	struct nvhost_ctrl_ctx *ctx = filp->private_data;

	spin_lock(&mlock_lock);
	mlocks &= ~(ctx->locks & ~BIT(0));
	spin_unlock(&mlock_lock);

	kfree(ctx);
	return 0;
}

static const struct file_operations nvhost_ctrl_fops = {
	.owner		= THIS_MODULE,
	.open		= nvhost_ctrl_open,
	.release	= nvhost_ctrl_release,
	.unlocked_ioctl	= nvhost_ctrl_ioctl,
};

static struct miscdevice nvhost_ctrl_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "nvhost-ctrl",
	.fops	= &nvhost_ctrl_fops,
	.mode	= 0666,
};

int tegra_nvhost_ctrl_init(void)
{
	struct device_node *np;
	int err;

	np = of_find_compatible_node(NULL, NULL, "nvidia,tegra20-host1x");
	if (!np)
		return -ENODEV;
	host1x_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!host1x_pdev)
		return -ENODEV;

	/* the host1x driver keeps its struct host1x as driver data */
	host1x = platform_get_drvdata(host1x_pdev);
	if (!host1x) {
		err = -ENODEV;
		goto err_put;
	}

	err = misc_register(&nvhost_ctrl_dev);
	if (err)
		goto err_put;
	return 0;

err_put:
	put_device(&host1x_pdev->dev);
	return err;
}

void tegra_nvhost_ctrl_exit(void)
{
	misc_deregister(&nvhost_ctrl_dev);
	put_device(&host1x_pdev->dev);
}
