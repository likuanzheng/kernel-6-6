// SPDX-License-Identifier: GPL-2.0
/*
 * main.c - CCSDS virtual network driver: module lifecycle
 *
 * Initialises the shared ccsds_ctx, then registers ccsdsnet (netdev)
 * and ccsdssim (chardev) in order.  Teardown runs in reverse.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include "ccsds.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ccsds");
MODULE_DESCRIPTION("CCSDS virtual net interface (ccsdsnet) + simulator chardev (ccsdssim)");

struct ccsds_ctx *g_ccsds;

static int __init ccsds_module_init(void)
{
	int ret;

	g_ccsds = kzalloc(sizeof(*g_ccsds), GFP_KERNEL);
	if (!g_ccsds)
		return -ENOMEM;

	ccsds_fifo_init(&g_ccsds->tx_fifo);

	ret = ccsds_netdev_init(g_ccsds);
	if (ret) {
		pr_err("ccsds: netdev init failed: %d\n", ret);
		goto err_free;
	}

	ret = ccsds_sim_init(g_ccsds);
	if (ret) {
		pr_err("ccsds: sim chardev init failed: %d\n", ret);
		goto err_netdev;
	}

	pr_info("ccsds: ccsdsnet + /dev/ccsdssim ready\n");
	return 0;

err_netdev:
	ccsds_netdev_exit(g_ccsds);
err_free:
	kfree(g_ccsds);
	g_ccsds = NULL;
	return ret;
}

static void __exit ccsds_module_exit(void)
{
	ccsds_sim_exit(g_ccsds);
	ccsds_netdev_exit(g_ccsds);
	ccsds_fifo_purge(&g_ccsds->tx_fifo);
	kfree(g_ccsds);
	g_ccsds = NULL;
	pr_info("ccsds: unloaded\n");
}

module_init(ccsds_module_init);
module_exit(ccsds_module_exit);
