// SPDX-License-Identifier: GPL-2.0
/*
 * netdev.c - ccsdsnet: Linux net_device connected to the IP layer
 *
 * TX path: IP stack calls ndo_start_xmit → skb enqueued into tx_fifo
 *          → user-space simulator drains via /dev/ccsdssim read().
 *
 * RX path: sim.c write() builds an skb from raw IP data and calls
 *          netif_rx() directly — no intermediate queue needed.
 */
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/slab.h>
#include "ccsds.h"

/* Retrieve the shared context stored in netdev private area. */
static inline struct ccsds_ctx *ctx_from_dev(struct net_device *dev)
{
	return *(struct ccsds_ctx **)netdev_priv(dev);
}

/* ------------------------------------------------------------------ */
/*  net_device_ops                                                      */
/* ------------------------------------------------------------------ */

static int ccsds_ndo_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int ccsds_ndo_stop(struct net_device *dev)
{
	struct ccsds_ctx *ctx = ctx_from_dev(dev);

	netif_stop_queue(dev);
	/* Discard packets that were not yet read by the simulator. */
	ccsds_fifo_purge(&ctx->tx_fifo);
	return 0;
}

/**
 * ccsds_ndo_start_xmit - IP stack hands us a packet to transmit
 *
 * The skb is pushed onto tx_fifo.  Ownership transfers to the queue;
 * the simulator's read() will dequeue and free it.
 */
static netdev_tx_t ccsds_ndo_start_xmit(struct sk_buff *skb,
					  struct net_device *dev)
{
	struct ccsds_ctx *ctx = ctx_from_dev(dev);

	skb_queue_tail(&ctx->tx_fifo.queue, skb);
	ctx->stats.tx_packets++;
	ctx->stats.tx_bytes += skb->len;

	return NETDEV_TX_OK;
}

static void ccsds_ndo_get_stats64(struct net_device *dev,
				   struct rtnl_link_stats64 *s)
{
	struct ccsds_ctx *ctx = ctx_from_dev(dev);

	s->tx_packets = ctx->stats.tx_packets;
	s->tx_bytes   = ctx->stats.tx_bytes;
	s->rx_packets = ctx->stats.rx_packets;
	s->rx_bytes   = ctx->stats.rx_bytes;
	s->rx_dropped = ctx->stats.rx_dropped;
}

static const struct net_device_ops ccsds_netdev_ops = {
	.ndo_open        = ccsds_ndo_open,
	.ndo_stop        = ccsds_ndo_stop,
	.ndo_start_xmit  = ccsds_ndo_start_xmit,
	.ndo_get_stats64 = ccsds_ndo_get_stats64,
};

/* ------------------------------------------------------------------ */
/*  Device setup                                                        */
/* ------------------------------------------------------------------ */

/**
 * ccsds_setup - initialise the net_device fields
 *
 * No Ethernet header: we connect directly at the IP layer (like TUN,
 * not TAP).  ARPHRD_NONE + IFF_NOARP means the IP layer will not try
 * to resolve L2 addresses.
 */
static void ccsds_setup(struct net_device *dev)
{
	dev->type       = ARPHRD_NONE;
	dev->flags      = IFF_NOARP | IFF_POINTOPOINT;
	dev->mtu        = CCSDS_MTU;
	dev->netdev_ops = &ccsds_netdev_ops;
	/* No header_ops: IP header is the outermost header. */
}

/* ------------------------------------------------------------------ */
/*  Init / exit                                                         */
/* ------------------------------------------------------------------ */

int ccsds_netdev_init(struct ccsds_ctx *ctx)
{
	struct net_device *dev;
	struct ccsds_ctx **priv;
	int ret;

	/*
	 * Allocate net_device.  Private area holds a single pointer back
	 * to ctx so that net_device_ops callbacks can reach shared state.
	 */
	dev = alloc_netdev(sizeof(struct ccsds_ctx *), CCSDS_NET_NAME,
			   NET_NAME_ENUM, ccsds_setup);
	if (!dev)
		return -ENOMEM;

	priv  = netdev_priv(dev);
	*priv = ctx;
	ctx->netdev = dev;

	ret = register_netdev(dev);
	if (ret) {
		free_netdev(dev);
		ctx->netdev = NULL;
		return ret;
	}

	pr_info("ccsds: registered net_device %s\n", dev->name);
	return 0;
}

void ccsds_netdev_exit(struct ccsds_ctx *ctx)
{
	if (!ctx->netdev)
		return;
	unregister_netdev(ctx->netdev);
	free_netdev(ctx->netdev);
	ctx->netdev = NULL;
}
