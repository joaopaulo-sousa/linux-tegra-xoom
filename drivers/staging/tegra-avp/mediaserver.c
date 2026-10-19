// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/tegra_mediaserver, ported from the NVIDIA L4T 3.1 tree
 * (drivers/media/video/tegra/mediaserver).
 *
 * While anyone has the node open, the kernel keeps the NVMM manager
 * (nvmm_manager.axf) running on the AVP and connected to the local port
 * NVMM_MANAGER_SRV. The media libraries register the codec blocks they
 * start on the AVP, so that the blocks of a process that dies are torn
 * down: the manager is told and their libraries are unloaded. The node also
 * hands out the IRAM area the codecs share.
 *
 * Copyright (C) 2011 NVIDIA Corp.
 */
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "avp.h"
#include "nvmap.h"
#include "tegra_avp.h"
#include "tegra_mediaserver.h"
#include "trpc.h"

#define NVMM_MANAGER_LIB		"nvmm_manager.axf"
#define NVMM_MANAGER_PORT		"NVMM_MANAGER_SRV"
#define NVMM_MANAGER_CONNECT_MS		50000
#define NVMM_MANAGER_MSG_ABNORMAL_TERM	1

static bool mediasrv_debug;
module_param(mediasrv_debug, bool, 0644);
MODULE_PARM_DESC(mediasrv_debug, "log every media server request");

#define DBG(fmt, ...) do {						\
	if (mediasrv_debug)						\
		pr_info("mediaserver: " fmt, ##__VA_ARGS__);		\
} while (0)

struct mediasrv_block {
	struct list_head entry;
	struct tegra_mediaserver_block_info block;
};

/* one open file */
struct mediasrv_node {
	struct list_head blocks;
	int nr_iram_shared;
};

static struct {
	struct mutex lock;
	struct nvmap_client *nvmap;
	int nr_nodes;
	int nr_blocks;

	/* the manager, while there are nodes */
	struct avp_info *avp;
	struct trpc_endpoint *ep;
	u32 lib;
	struct work_struct drain;

	/* the shared IRAM area, while referenced */
	struct tegra_mediaserver_iram_info iram;
	int nr_iram_shared;
} ms;

/*
 * Nobody waits for messages from the manager; drop them so they don't pile
 * up on the port.
 */
static void mediasrv_drain(struct work_struct *work)
{
	u32 msg[TEGRA_RPC_MAX_MSG_LEN / sizeof(u32)];
	int len;

	mutex_lock(&ms.lock);
	while (ms.ep && (len = rpc_local_recv(ms.ep, msg, sizeof(msg))) > 0)
		DBG("manager message of %d bytes, type %u\n", len, msg[0]);
	mutex_unlock(&ms.lock);
}

static void mediasrv_notify_recv(struct trpc_endpoint *ep)
{
	schedule_work(&ms.drain);
}

static struct trpc_ep_ops mediasrv_ep_ops = {
	.notify_recv	= mediasrv_notify_recv,
};

static void mediasrv_manager_stop(void)
{
	if (ms.lib)
		tegra_avp_unload_lib(ms.avp, ms.lib);
	ms.lib = 0;
	if (ms.ep)
		trpc_close(ms.ep);
	ms.ep = NULL;
	if (ms.avp)
		tegra_avp_put(ms.avp);
	ms.avp = NULL;
}

static int mediasrv_manager_start(void)
{
	struct tegra_avp_lib lib = { .name = NVMM_MANAGER_LIB };
	/* an opaque cookie for the manager; the original passed its pointer */
	u32 cookie = (u32)(uintptr_t)&ms;
	int ret;

	ms.avp = tegra_avp_get();
	if (IS_ERR(ms.avp)) {
		ret = PTR_ERR(ms.avp);
		ms.avp = NULL;
		return ret;
	}

	ms.ep = rpc_local_create(NVMM_MANAGER_PORT, &mediasrv_ep_ops, NULL);
	if (IS_ERR(ms.ep)) {
		ret = PTR_ERR(ms.ep);
		ms.ep = NULL;
		goto err;
	}

	lib.args = (__force void __user *)&cookie;
	lib.args_len = sizeof(cookie);
	ret = tegra_avp_load_lib(ms.avp, &lib);
	if (ret)
		goto err;
	ms.lib = lib.handle;

	ret = trpc_connect(ms.ep, NVMM_MANAGER_CONNECT_MS);
	if (ret) {
		pr_err("mediaserver: can't connect to the NVMM manager (%d)\n",
		       ret);
		goto err;
	}
	return 0;

err:
	mediasrv_manager_stop();
	return ret;
}

static void mediasrv_iram_release(void)
{
	nvmap_handle_unpin(ms.nvmap, ms.iram.rm_handle);
	nvmap_handle_free(ms.nvmap, ms.iram.rm_handle);
	ms.iram.rm_handle = 0;
	ms.iram.physical_address = 0;
}

static int mediasrv_open(struct inode *inode, struct file *file)
{
	struct mediasrv_node *node;
	int ret = 0;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;
	INIT_LIST_HEAD(&node->blocks);

	mutex_lock(&ms.lock);
	if (!ms.nr_nodes)
		ret = mediasrv_manager_start();
	if (!ret)
		ms.nr_nodes++;
	mutex_unlock(&ms.lock);

	if (ret) {
		kfree(node);
		return ret;
	}

	DBG("open by %s\n", current->comm);
	file->private_data = node;
	return nonseekable_open(inode, file);
}

static int mediasrv_release(struct inode *inode, struct file *file)
{
	struct mediasrv_node *node = file->private_data;
	struct mediasrv_block *block, *tmp;
	u32 msg[2];
	int ret;

	mutex_lock(&ms.lock);

	list_for_each_entry_safe(block, tmp, &node->blocks, entry) {
		struct tegra_mediaserver_block_info *b = &block->block;

		pr_info("mediaserver: block 0x%08x (AVP block 0x%08x) left open, terminating it\n",
			b->nvmm_block_handle, b->avp_block_handle);

		msg[0] = NVMM_MANAGER_MSG_ABNORMAL_TERM;
		msg[1] = b->avp_block_handle;
		ret = rpc_local_send(ms.ep, msg, sizeof(msg));
		if (ret)
			pr_err("mediaserver: abnormal termination message failed (%d)\n",
			       ret);

		if (b->avp_block_library_handle)
			tegra_avp_unload_lib(ms.avp, b->avp_block_library_handle);
		if (b->service_library_handle)
			tegra_avp_unload_lib(ms.avp, b->service_library_handle);

		ms.nr_blocks--;
		list_del(&block->entry);
		kfree(block);
	}

	ms.nr_iram_shared -= node->nr_iram_shared;
	if (ms.iram.rm_handle && !ms.nr_iram_shared)
		mediasrv_iram_release();

	kfree(node);
	if (!--ms.nr_nodes)
		mediasrv_manager_stop();

	mutex_unlock(&ms.lock);
	return 0;
}

static int mediasrv_alloc_iram(struct mediasrv_node *node,
			       union tegra_mediaserver_alloc_info *info)
{
	size_t size = PAGE_ALIGN(info->in.u.iram.size);
	size_t align = max_t(size_t, info->in.u.iram.alignment, PAGE_SIZE);
	dma_addr_t addr;
	u32 id;
	int ret;

	/* the original refuses scratch IRAM as well */
	if (info->in.u.iram.tegra_mediaserver_iram_type !=
	    TEGRA_MEDIASERVER_IRAM_SHARED)
		return -EINVAL;

	if (!ms.nr_iram_shared) {
		ret = nvmap_handle_create(ms.nvmap, size, &id);
		if (ret)
			return ret;
		ret = nvmap_handle_alloc(ms.nvmap, id, NVMAP_HEAP_CARVEOUT_IRAM,
					 align, NVMAP_HANDLE_WRITE_COMBINE);
		if (!ret)
			ret = nvmap_handle_pin(ms.nvmap, id, &addr);
		if (!ret)
			ret = nvmap_handle_share(ms.nvmap, id);
		if (ret) {
			nvmap_handle_free(ms.nvmap, id);
			return ret;
		}
		ms.iram.rm_handle = id;
		ms.iram.physical_address = addr;
		DBG("shared IRAM: %zu bytes at %pad, handle %u\n",
		    size, &addr, id);
	}

	memset(&info->out, 0, sizeof(info->out));
	info->out.u.iram = ms.iram;
	ms.nr_iram_shared++;
	node->nr_iram_shared++;
	return 0;
}

static int mediasrv_alloc(struct mediasrv_node *node,
			  union tegra_mediaserver_alloc_info *info)
{
	struct mediasrv_block *block;

	switch (info->in.tegra_mediaserver_resource_type) {
	case TEGRA_MEDIASERVER_RESOURCE_BLOCK:
		block = kzalloc(sizeof(*block), GFP_KERNEL);
		if (!block)
			return -ENOMEM;
		block->block = info->in.u.block;
		list_add(&block->entry, &node->blocks);
		DBG("block 0x%08x registered\n", block->block.nvmm_block_handle);

		memset(&info->out, 0, sizeof(info->out));
		info->out.u.block.count = ++ms.nr_blocks;
		return 0;
	case TEGRA_MEDIASERVER_RESOURCE_IRAM:
		return mediasrv_alloc_iram(node, info);
	default:
		return -EINVAL;
	}
}

static struct mediasrv_block *mediasrv_find_block(struct mediasrv_node *node,
						  int nvmm_block_handle)
{
	struct mediasrv_block *block;

	list_for_each_entry(block, &node->blocks, entry)
		if (block->block.nvmm_block_handle == nvmm_block_handle)
			return block;
	return NULL;
}

static void mediasrv_free(struct mediasrv_node *node,
			  union tegra_mediaserver_free_info *info)
{
	struct mediasrv_block *block;

	switch (info->in.tegra_mediaserver_resource_type) {
	case TEGRA_MEDIASERVER_RESOURCE_BLOCK:
		block = mediasrv_find_block(node, info->in.u.nvmm_block_handle);
		DBG("block 0x%08x freed%s\n", info->in.u.nvmm_block_handle,
		    block ? "" : " (unknown)");
		/* the block count is not decremented, as in the original */
		if (block) {
			list_del(&block->entry);
			kfree(block);
		}
		break;
	case TEGRA_MEDIASERVER_RESOURCE_IRAM:
		if (info->in.u.iram_rm_handle != ms.iram.rm_handle ||
		    !node->nr_iram_shared)
			break;
		node->nr_iram_shared--;
		if (!--ms.nr_iram_shared)
			mediasrv_iram_release();
		break;
	}
}

static long mediasrv_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct mediasrv_node *node = file->private_data;
	void __user *uarg = (void __user *)arg;
	union {
		union tegra_mediaserver_alloc_info alloc;
		union tegra_mediaserver_free_info free;
		union tegra_mediaserver_update_block_info update;
	} buf;
	struct mediasrv_block *block;
	int ret = 0;

	switch (cmd) {
	case TEGRA_MEDIASERVER_IOCTL_ALLOC:
	case TEGRA_MEDIASERVER_IOCTL_FREE:
	case TEGRA_MEDIASERVER_IOCTL_UPDATE_BLOCK_INFO:
		break;
	default:
		pr_info_ratelimited("mediaserver: %s: unknown ioctl %#x\n",
				    current->comm, cmd);
		return -ENOTTY;
	}

	if (copy_from_user(&buf, uarg, _IOC_SIZE(cmd)))
		return -EFAULT;

	mutex_lock(&ms.lock);
	switch (cmd) {
	case TEGRA_MEDIASERVER_IOCTL_ALLOC:
		ret = mediasrv_alloc(node, &buf.alloc);
		break;
	case TEGRA_MEDIASERVER_IOCTL_FREE:
		mediasrv_free(node, &buf.free);
		break;
	case TEGRA_MEDIASERVER_IOCTL_UPDATE_BLOCK_INFO:
		block = mediasrv_find_block(node, buf.update.in.nvmm_block_handle);
		if (block)
			block->block = buf.update.in;
		else
			ret = -EINVAL;
		DBG("block 0x%08x: AVP block 0x%08x lib 0x%08x, service 0x%08x lib 0x%08x (%d)\n",
		    buf.update.in.nvmm_block_handle, buf.update.in.avp_block_handle,
		    buf.update.in.avp_block_library_handle,
		    buf.update.in.service_handle,
		    buf.update.in.service_library_handle, ret);
		break;
	}
	mutex_unlock(&ms.lock);

	if (!ret && cmd == TEGRA_MEDIASERVER_IOCTL_ALLOC &&
	    copy_to_user(uarg, &buf.alloc, sizeof(buf.alloc)))
		ret = -EFAULT;
	return ret;
}

static const struct file_operations mediasrv_fops = {
	.owner		= THIS_MODULE,
	.open		= mediasrv_open,
	.release	= mediasrv_release,
	.unlocked_ioctl	= mediasrv_ioctl,
};

static struct miscdevice mediasrv_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "tegra_mediaserver",
	.fops	= &mediasrv_fops,
};

int tegra_mediaserver_init(void)
{
	int ret;

	mutex_init(&ms.lock);
	INIT_WORK(&ms.drain, mediasrv_drain);

	ms.nvmap = nvmap_client_create();
	if (!ms.nvmap)
		return -ENOMEM;

	ret = misc_register(&mediasrv_dev);
	if (ret)
		nvmap_client_destroy(ms.nvmap);
	return ret;
}

void tegra_mediaserver_exit(void)
{
	misc_deregister(&mediasrv_dev);
	cancel_work_sync(&ms.drain);
	nvmap_client_destroy(ms.nvmap);
}
