/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nvhost control interface of the NVIDIA L4T 3.1 tree (linux/nvhost_ioctl.h),
 * which the NVIDIA (L4T) media libraries open as /dev/nvhost-ctrl. The layout
 * is ABI: 32-bit ARM, pointers are 32 bits.
 */
#ifndef __TEGRA_AVP_NVHOST_CTRL_H
#define __TEGRA_AVP_NVHOST_CTRL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define NVHOST_NO_TIMEOUT			(-1)
#define NVHOST_SUBMIT_VERSION_MAX_SUPPORTED	2

struct nvhost_get_param_args {
	__u32 value;
};

struct nvhost_ctrl_syncpt_read_args {
	__u32 id;
	__u32 value;
};

struct nvhost_ctrl_syncpt_incr_args {
	__u32 id;
};

struct nvhost_ctrl_syncpt_wait_args {
	__u32 id;
	__u32 thresh;
	__s32 timeout;		/* ms */
};

struct nvhost_ctrl_syncpt_waitex_args {
	__u32 id;
	__u32 thresh;
	__s32 timeout;		/* ms */
	__u32 value;
};

struct nvhost_ctrl_module_mutex_args {
	__u32 id;
	__u32 lock;
};

struct nvhost_ctrl_module_regrdwr_args {
	__u32 id;
	__u32 num_offsets;
	__u32 block_size;
	__u32 offsets;		/* user pointer */
	__u32 values;		/* user pointer */
	__u32 write;
};

#define NVHOST_IOCTL_MAGIC 'H'

#define NVHOST_IOCTL_CTRL_SYNCPT_READ	\
	_IOWR(NVHOST_IOCTL_MAGIC, 1, struct nvhost_ctrl_syncpt_read_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_INCR	\
	_IOW(NVHOST_IOCTL_MAGIC, 2, struct nvhost_ctrl_syncpt_incr_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAIT	\
	_IOW(NVHOST_IOCTL_MAGIC, 3, struct nvhost_ctrl_syncpt_wait_args)
#define NVHOST_IOCTL_CTRL_MODULE_MUTEX	\
	_IOWR(NVHOST_IOCTL_MAGIC, 4, struct nvhost_ctrl_module_mutex_args)
#define NVHOST_IOCTL_CTRL_MODULE_REGRDWR	\
	_IOWR(NVHOST_IOCTL_MAGIC, 5, struct nvhost_ctrl_module_regrdwr_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX	\
	_IOWR(NVHOST_IOCTL_MAGIC, 6, struct nvhost_ctrl_syncpt_waitex_args)
#define NVHOST_IOCTL_CTRL_GET_VERSION	\
	_IOR(NVHOST_IOCTL_MAGIC, 7, struct nvhost_get_param_args)

#ifdef __KERNEL__
int tegra_nvhost_ctrl_init(void);
void tegra_nvhost_ctrl_exit(void);
#endif

#endif
