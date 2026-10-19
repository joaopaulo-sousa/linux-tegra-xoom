// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tegra20 AVP multimedia offload, ported from the NVIDIA/Android 2.6.39
 * tree (drivers/media/video/tegra/avp) as one module: the RPC core, the
 * local RPC device (/dev/tegra_rpc), the fd based semaphores
 * (/dev/tegra_sema), the memory handles (/dev/nvmap, /dev/knvmap) and the
 * AVP itself (/dev/tegra_avp: its kernel, the codec libraries it loads, the
 * services it asks of the CPU), which the NVIDIA multimedia libraries use.
 */
#include <linux/module.h>

#include "avp.h"
#include "trpc.h"

static int __init tegra_avp_module_init(void)
{
	int ret;

	ret = tegra_rpc_init();
	if (ret)
		return ret;

	ret = rpc_local_init();
	if (ret)
		goto err_local;

	/* the AVP device brings up nvmap */
	ret = tegra_avp_driver_register();
	if (ret)
		goto err_avp;

	return 0;

err_avp:
	rpc_local_exit();
err_local:
	tegra_rpc_exit();
	return ret;
}

static void __exit tegra_avp_module_exit(void)
{
	tegra_avp_driver_unregister();
	rpc_local_exit();
	tegra_rpc_exit();
}

module_init(tegra_avp_module_init);
module_exit(tegra_avp_module_exit);

MODULE_DESCRIPTION("Tegra20 AVP multimedia offload");
MODULE_LICENSE("GPL");
