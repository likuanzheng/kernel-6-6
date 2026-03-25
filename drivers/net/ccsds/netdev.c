// SPDX-License-Identifier: GPL-2.0
/*
 * netdev.c - ccsdsnet: Linux net_device connected to the IP layer
 *
 * TX path: IP stack calls ndo_start_xmit → skb enqueued into tx_fifo
 *          → user-space simulator drains via /dev/ccsdssim read().
 *
 * RX path: sim.c write() enqueues skb into rx_fifo and schedules NAPI;
 *          ccsds_napi_poll() drains rx_fifo → napi_gro_receive().
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
/*  NAPI poll — RX path drain                                           */
/* ------------------------------------------------------------------ */

/**
 * ccsds_napi_poll - drain rx_fifo and deliver packets to the IP stack
 *
 * Called by the NAPI subsystem (softirq context) after napi_schedule()
 * is triggered by sim_write().  Dequeues up to @budget skbs from
 * rx_fifo and hands each one to netif_receive_skb().
 */
static int ccsds_napi_poll(struct napi_struct *napi, int budget)
{
	struct ccsds_ctx *ctx = container_of(napi, struct ccsds_ctx, napi);
	struct sk_buff *skb;
	int done = 0;

	while (done < budget) {
		skb = skb_dequeue(&ctx->rx_fifo.queue);
		if (!skb)
			break;
		ctx->stats.rx_packets++;
		ctx->stats.rx_bytes += skb->len;
		napi_gro_receive(napi, skb);
		done++;
	}

	/* If we exhausted the budget the queue may still have packets;
	 * return budget to let NAPI reschedule.  Otherwise complete. */
	if (done < budget)
		napi_complete_done(napi, done);

	return done;
}

/* ------------------------------------------------------------------ */
/*  net_device_ops                                                      */
/* ------------------------------------------------------------------ */

static int ccsds_ndo_open(struct net_device *dev)
{
	struct ccsds_ctx *ctx = ctx_from_dev(dev);

	napi_enable(&ctx->napi);
	netif_start_queue(dev);
	return 0;
}

static int ccsds_ndo_stop(struct net_device *dev)
{
	struct ccsds_ctx *ctx = ctx_from_dev(dev);

	netif_stop_queue(dev);
	napi_disable(&ctx->napi);
	/* Discard packets that were not yet consumed in either direction. */
	ccsds_fifo_purge(&ctx->tx_fifo);
	ccsds_fifo_purge(&ctx->rx_fifo);
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

	/* Register the NAPI instance that will drain rx_fifo.
	 * Must be done before register_netdev() so the poll function
	 * is ready before the device can be opened. */
	netif_napi_add(dev, &ctx->napi, ccsds_napi_poll);

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
	/* unregister_netdev() closes the device first (ndo_stop →
	 * napi_disable), so netif_napi_del() is safe afterwards. */
	unregister_netdev(ctx->netdev);
	netif_napi_del(&ctx->napi);
	free_netdev(ctx->netdev);
	ctx->netdev = NULL;
}
