/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ccsds.h - Shared context between ccsdsnet (netdev) and ccsdssim (chardev)
 *
 * Architecture:
 *
 *   IP layer
 *      ↕  ndo_start_xmit / netif_receive_skb
 *   [ccsdsnet]  net_device "ccsdsnet"
 *      ↕  tx_fifo / rx_fifo (sk_buff_head, one per direction)
 *   [ccsdssim]  char device  /dev/ccsdssim
 *      ↕  read() / write()
 *   User-space simulator
 *
 * TX (IP → sim): ndo_start_xmit enqueues skb into tx_fifo;
 *                sim read() dequeues one complete packet, non-blocking.
 *
 * RX (sim → IP): sim write() builds skb and enqueues into rx_fifo,
 *                then schedules NAPI; the NAPI poll in netdev.c drains
 *                rx_fifo and delivers each skb via napi_gro_receive().
 */
#ifndef _CCSDS_H
#define _CCSDS_H

#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/wait.h>
#include <linux/cdev.h>

#define CCSDS_NET_NAME  "ccsdsnet"
#define CCSDS_SIM_NAME  "ccsdssim"
#define CCSDS_MTU       1500

/**
 * struct ccsds_fifo - one-directional packet FIFO between netdev and chardev
 * @queue: sk_buff queue (thread-safe, has its own spinlock)
 * @wait:  wait queue for blocking readers (reserved, not used for non-blocking)
 */
struct ccsds_fifo {
	struct sk_buff_head  queue;
	wait_queue_head_t    wait;
};

/**
 * struct ccsds_ctx - global module context shared by netdev and chardev
 * @netdev:   the "ccsdsnet" net_device
 * @cdev:     the "ccsdssim" character device
 * @devno:    allocated device number (major:minor)
 * @cls:      sysfs class for auto-creating /dev/ccsdssim
 * @dev_node: sysfs device node
 * @tx_fifo:  TX queue: IP stack → ccsdsnet → ccsdssim → user-space
 * @rx_fifo:  RX queue: user-space → ccsdssim → ccsdsnet → IP stack
 * @napi:     NAPI instance that drains rx_fifo into the IP stack
 * @stats:    device statistics (shared, updated from both sides)
 */
struct ccsds_ctx {
	struct net_device    *netdev;

	struct cdev           cdev;
	dev_t                 devno;
	struct class         *cls;
	struct device        *dev_node;

	struct ccsds_fifo     tx_fifo;
	struct ccsds_fifo     rx_fifo;
	struct napi_struct    napi;

	struct net_device_stats stats;
};

extern struct ccsds_ctx *g_ccsds;

static inline void ccsds_fifo_init(struct ccsds_fifo *f)
{
	skb_queue_head_init(&f->queue);
	init_waitqueue_head(&f->wait);
}

static inline void ccsds_fifo_purge(struct ccsds_fifo *f)
{
	skb_queue_purge(&f->queue);
}

int  ccsds_netdev_init(struct ccsds_ctx *ctx);
void ccsds_netdev_exit(struct ccsds_ctx *ctx);
int  ccsds_sim_init(struct ccsds_ctx *ctx);
void ccsds_sim_exit(struct ccsds_ctx *ctx);

#endif /* _CCSDS_H */
