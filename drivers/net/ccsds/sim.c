// SPDX-License-Identifier: GPL-2.0
/*
 * sim.c - ccsdssim: character device /dev/ccsdssim
 *
 * read()  – Non-blocking.  Returns exactly one complete IP packet from
 *           tx_fifo (packets queued by ccsdsnet's ndo_start_xmit).
 *           Returns -EAGAIN immediately when the queue is empty.
 *           Returns -ENOBUFS if the caller's buffer is smaller than
 *           the queued packet (packet is put back at the head).
 *
 * write() – Accepts one raw IP packet (IPv4 or IPv6, including IP
 *           header).  Injects it into the IP stack via netif_rx().
 */
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/slab.h>
#include "ccsds.h"

/* ------------------------------------------------------------------ */
/*  file_operations                                                      */
/* ------------------------------------------------------------------ */

static int sim_open(struct inode *inode, struct file *filp)
{
	/* No per-open state; bind filp->private_data to global ctx. */
	filp->private_data = g_ccsds;
	return 0;
}

/**
 * sim_read - dequeue one packet from tx_fifo and copy to user space
 *
 * Non-blocking: returns -EAGAIN if the queue is empty.
 * Each call delivers exactly one complete IP packet.
 */
static ssize_t sim_read(struct file *filp, char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct ccsds_ctx *ctx = filp->private_data;
	struct sk_buff *skb;
	ssize_t ret;

	skb = skb_dequeue(&ctx->tx_fifo.queue);
	if (!skb)
		return -EAGAIN;

	if (count < skb->len) {
		/* Put the packet back so the caller can retry with a
		 * larger buffer; preserve queue ordering. */
		skb_queue_head(&ctx->tx_fifo.queue, skb);
		return -ENOBUFS;
	}

	ret = (ssize_t)skb->len;
	if (copy_to_user(buf, skb->data, skb->len))
		ret = -EFAULT;

	kfree_skb(skb);
	return ret;
}

/**
 * sim_write - receive one raw IP packet from user space and inject into stack
 *
 * The caller must supply a complete IP packet starting at the IP header
 * (no additional framing).  IPv4/IPv6 is auto-detected from the version
 * nibble.
 */
static ssize_t sim_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct ccsds_ctx *ctx = filp->private_data;
	struct sk_buff *skb;
	__be16 proto;
	u8 version_byte;
	int rc;

	/* Minimum: a valid IPv4 header is 20 bytes; IPv6 is 40 bytes.
	 * We check the stricter lower bound here and trust the IP stack
	 * to validate the rest. */
	if (count < sizeof(struct iphdr))
		return -EINVAL;
	if (count > CCSDS_MTU)
		return -EMSGSIZE;

	/* Peek at the IP version nibble (top 4 bits of first byte). */
	if (get_user(version_byte, (const u8 __user *)buf))
		return -EFAULT;

	switch (version_byte >> 4) {
	case 4:
		proto = htons(ETH_P_IP);
		break;
	case 6:
		if (count < sizeof(struct ipv6hdr))
			return -EINVAL;
		proto = htons(ETH_P_IPV6);
		break;
	default:
		return -EINVAL;
	}

	skb = dev_alloc_skb(count);
	if (!skb)
		return -ENOMEM;

	if (copy_from_user(skb_put(skb, count), buf, count)) {
		kfree_skb(skb);
		return -EFAULT;
	}

	skb->dev      = ctx->netdev;
	skb->protocol = proto;
	skb_reset_network_header(skb);

	/*
	 * netif_rx() takes ownership of skb unconditionally.
	 * NET_RX_DROP means the backlog was full; we still report
	 * success to the caller (the write was accepted) but track drops.
	 */
	rc = netif_rx(skb);
	if (rc == NET_RX_DROP) {
		ctx->stats.rx_dropped++;
	} else {
		ctx->stats.rx_packets++;
		ctx->stats.rx_bytes += count;
	}

	return (ssize_t)count;
}

static int sim_release(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;
	return 0;
}

static const struct file_operations sim_fops = {
	.owner   = THIS_MODULE,
	.open    = sim_open,
	.read    = sim_read,
	.write   = sim_write,
	.release = sim_release,
	.llseek  = no_llseek,
};

/* ------------------------------------------------------------------ */
/*  Init / exit                                                         */
/* ------------------------------------------------------------------ */

int ccsds_sim_init(struct ccsds_ctx *ctx)
{
	int ret;

	ret = alloc_chrdev_region(&ctx->devno, 0, 1, CCSDS_SIM_NAME);
	if (ret) {
		pr_err("ccsds: alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	cdev_init(&ctx->cdev, &sim_fops);
	ctx->cdev.owner = THIS_MODULE;

	ret = cdev_add(&ctx->cdev, ctx->devno, 1);
	if (ret) {
		pr_err("ccsds: cdev_add failed: %d\n", ret);
		goto err_unregister;
	}

	/* Create sysfs class and device so udev generates /dev/ccsdssim. */
	ctx->cls = class_create(CCSDS_SIM_NAME);
	if (IS_ERR(ctx->cls)) {
		ret = PTR_ERR(ctx->cls);
		pr_err("ccsds: class_create failed: %d\n", ret);
		goto err_cdev;
	}

	ctx->dev_node = device_create(ctx->cls, NULL, ctx->devno,
				      NULL, CCSDS_SIM_NAME);
	if (IS_ERR(ctx->dev_node)) {
		ret = PTR_ERR(ctx->dev_node);
		pr_err("ccsds: device_create failed: %d\n", ret);
		goto err_class;
	}

	pr_info("ccsds: /dev/%s created (major %d)\n",
		CCSDS_SIM_NAME, MAJOR(ctx->devno));
	return 0;

err_class:
	class_destroy(ctx->cls);
	ctx->cls = NULL;
err_cdev:
	cdev_del(&ctx->cdev);
err_unregister:
	unregister_chrdev_region(ctx->devno, 1);
	return ret;
}

void ccsds_sim_exit(struct ccsds_ctx *ctx)
{
	if (ctx->dev_node) {
		device_destroy(ctx->cls, ctx->devno);
		ctx->dev_node = NULL;
	}
	if (ctx->cls) {
		class_destroy(ctx->cls);
		ctx->cls = NULL;
	}
	cdev_del(&ctx->cdev);
	unregister_chrdev_region(ctx->devno, 1);
}
