/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Media server interface of the NVIDIA L4T 3.1 tree
 * (include/linux/tegra_mediaserver.h), used by the NVIDIA (L4T) media
 * libraries through /dev/tegra_mediaserver. The layout is ABI: 32-bit ARM.
 *
 * Copyright (c) 2011, NVIDIA Corporation.
 */
#ifndef _TEGRA_MEDIASERVER_H
#define _TEGRA_MEDIASERVER_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define TEGRA_MEDIASERVER_MAGIC 'm'
#define TEGRA_MEDIASERVER_IOCTL_ALLOC \
	_IOWR(TEGRA_MEDIASERVER_MAGIC, 0x40, \
	      union tegra_mediaserver_alloc_info)

enum tegra_mediaserver_resource_type {
	TEGRA_MEDIASERVER_RESOURCE_BLOCK = 0,
	TEGRA_MEDIASERVER_RESOURCE_IRAM,
};

enum tegra_mediaserver_block_type {
	TEGRA_MEDIASERVER_BLOCK_AUDDEC = 0,
	TEGRA_MEDIASERVER_BLOCK_VIDDEC,
};

enum tegra_mediaserver_iram_type {
	TEGRA_MEDIASERVER_IRAM_SCRATCH = 0,
	TEGRA_MEDIASERVER_IRAM_SHARED,
};

struct tegra_mediaserver_block_info {
	int nvmm_block_handle;
	int avp_block_handle;
	int avp_block_library_handle;
	int service_handle;
	int service_library_handle;
};

struct tegra_mediaserver_iram_info {
	unsigned long rm_handle;
	int physical_address;
};

union tegra_mediaserver_alloc_info {
	struct {
		int tegra_mediaserver_resource_type;

		union {
			struct tegra_mediaserver_block_info block;

			struct {
				int tegra_mediaserver_iram_type;
				int alignment;
				size_t size;
			} iram;
		} u;
	} in;

	struct {
		union {
			struct {
				int count;
			} block;

			struct tegra_mediaserver_iram_info iram;
		} u;
	} out;
};

#define TEGRA_MEDIASERVER_IOCTL_FREE \
	_IOR(TEGRA_MEDIASERVER_MAGIC, 0x41, union tegra_mediaserver_free_info)

union tegra_mediaserver_free_info {
	struct {
		int tegra_mediaserver_resource_type;

		union {
			int nvmm_block_handle;
			int iram_rm_handle;
		} u;
	} in;
};

#define TEGRA_MEDIASERVER_IOCTL_UPDATE_BLOCK_INFO	\
	_IOR(TEGRA_MEDIASERVER_MAGIC, 0x45, \
	     union tegra_mediaserver_update_block_info)

union tegra_mediaserver_update_block_info {
	struct tegra_mediaserver_block_info in;
};

#ifdef __KERNEL__
int tegra_mediaserver_init(void);
void tegra_mediaserver_exit(void);
#endif

#endif
