/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nvmap user interface of the NVIDIA/Android 2.6.39 tree (mach/nvmap.h,
 * drivers/video/tegra/nvmap/nvmap_ioctl.h), which the NVIDIA (L4T) media
 * libraries use through /dev/nvmap and /dev/knvmap. The layout is ABI:
 * 32-bit ARM, unsigned long is 32 bits.
 */
#ifndef __TEGRA_AVP_NVMAP_H
#define __TEGRA_AVP_NVMAP_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define NVMAP_HEAP_SYSMEM		(1ul << 31)
#define NVMAP_HEAP_IOVMM		(1ul << 30)
#define NVMAP_HEAP_CARVEOUT_IRAM	(1ul << 29)
#define NVMAP_HEAP_CARVEOUT_GENERIC	(1ul << 0)
#define NVMAP_HEAP_CARVEOUT_MASK	(NVMAP_HEAP_IOVMM - 1)

#define NVMAP_HANDLE_UNCACHEABLE	(0x0ul << 0)
#define NVMAP_HANDLE_WRITE_COMBINE	(0x1ul << 0)
#define NVMAP_HANDLE_INNER_CACHEABLE	(0x2ul << 0)
#define NVMAP_HANDLE_CACHEABLE		(0x3ul << 0)
#define NVMAP_HANDLE_CACHE_FLAG		(0x3ul << 0)
#define NVMAP_HANDLE_SECURE		(0x1ul << 2)

enum {
	NVMAP_HANDLE_PARAM_SIZE = 1,
	NVMAP_HANDLE_PARAM_ALIGNMENT,
	NVMAP_HANDLE_PARAM_BASE,
	NVMAP_HANDLE_PARAM_HEAP,
};

enum {
	NVMAP_CACHE_OP_WB = 0,
	NVMAP_CACHE_OP_INV,
	NVMAP_CACHE_OP_WB_INV,
};

struct nvmap_create_handle {
	union {
		__u32 key;	/* ClaimPreservedHandle */
		__u32 id;	/* FromId */
		__u32 size;	/* CreateHandle */
	};
	__u32 handle;
};

struct nvmap_alloc_handle {
	__u32 handle;
	__u32 heap_mask;
	__u32 flags;
	__u32 align;
};

struct nvmap_map_caller {
	__u32 handle;		/* hmem */
	__u32 offset;		/* offset into hmem; should be page-aligned */
	__u32 length;		/* number of bytes to map */
	__u32 flags;
	unsigned long addr;	/* user pointer */
};

struct nvmap_rw_handle {
	unsigned long addr;	/* user pointer */
	__u32 handle;		/* hmem */
	__u32 offset;		/* offset into hmem */
	__u32 elem_size;	/* individual atom size */
	__u32 hmem_stride;	/* delta in bytes between atoms in hmem */
	__u32 user_stride;	/* delta in bytes between atoms in user */
	__u32 count;		/* number of atoms to copy */
};

struct nvmap_pin_handle {
	unsigned long handles;	/* array of handles to pin/unpin */
	unsigned long addr;	/* array of addresses to return */
	__u32 count;		/* number of entries in handles */
};

struct nvmap_handle_param {
	__u32 handle;
	__u32 param;
	unsigned long result;
};

struct nvmap_cache_op {
	unsigned long addr;
	__u32 handle;
	__u32 len;
	__s32 op;
};

#define NVMAP_IOC_MAGIC 'N'

#define NVMAP_IOC_CREATE	_IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_CLAIM		_IOWR(NVMAP_IOC_MAGIC, 1, struct nvmap_create_handle)
#define NVMAP_IOC_FROM_ID	_IOWR(NVMAP_IOC_MAGIC, 2, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC		_IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE		_IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_MMAP		_IOWR(NVMAP_IOC_MAGIC, 5, struct nvmap_map_caller)
#define NVMAP_IOC_WRITE		_IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ		_IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)
#define NVMAP_IOC_PARAM		_IOWR(NVMAP_IOC_MAGIC, 8, struct nvmap_handle_param)
#define NVMAP_IOC_PIN_MULT	_IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle)
#define NVMAP_IOC_UNPIN_MULT	_IOW(NVMAP_IOC_MAGIC, 11, struct nvmap_pin_handle)
#define NVMAP_IOC_CACHE		_IOW(NVMAP_IOC_MAGIC, 12, struct nvmap_cache_op)
#define NVMAP_IOC_GET_ID	_IOWR(NVMAP_IOC_MAGIC, 13, struct nvmap_create_handle)

#define NVMAP_IOC_MAXNR		(_IOC_NR(NVMAP_IOC_GET_ID))

#ifdef __KERNEL__
int tegra_nvmap_init(void);
void tegra_nvmap_exit(void);
#endif

#endif
