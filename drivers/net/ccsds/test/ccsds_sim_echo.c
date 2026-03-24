// SPDX-License-Identifier: GPL-2.0
/*
 * ccsds_sim_echo.c - Userspace echo simulator for /dev/ccsdssim
 *
 * Reads ICMP echo requests from /dev/ccsdssim (TX path from ccsdsnet),
 * swaps src/dst IP, changes type to echo reply, recalculates checksums,
 * then writes the reply back (RX path into ccsdsnet).
 *
 * Usage:
 *   ./ccsds_sim_echo [/dev/ccsdssim]
 *
 * Exits on Ctrl-C.  Prints one line per handled packet to stderr.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#define DEVPATH  "/dev/ccsdssim"
#define BUFSIZE  1500

static volatile int g_stop;

static void sig_handler(int s) { (void)s; g_stop = 1; }

/* Standard one's-complement checksum. */
static uint16_t cksum(const void *data, int len)
{
	const uint16_t *p = (const uint16_t *)data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *p++;
		len -= 2;
	}
	if (len)
		sum += *(const uint8_t *)p;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~(uint16_t)sum;
}

int main(int argc, char *argv[])
{
	const char *devpath = (argc > 1) ? argv[1] : DEVPATH;
	uint8_t buf[BUFSIZE];
	int fd;
	ssize_t n;
	unsigned long handled = 0, skipped = 0;

	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		perror(devpath);
		return 1;
	}
	fprintf(stderr, "ccsds_sim_echo: opened %s, waiting for ICMP echo requests...\n",
		devpath);

	while (!g_stop) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN) {
				/* Queue empty – busy-wait with a short sleep. */
				nanosleep(&(struct timespec){0, 500000}, NULL);
				continue;
			}
			perror("read");
			break;
		}

		/* Validate: need at least an IPv4 header. */
		if (n < (ssize_t)sizeof(struct iphdr)) {
			skipped++;
			continue;
		}

		struct iphdr *iph = (struct iphdr *)buf;

		if (iph->version != 4) {
			/* IPv6 not handled by this simple echo sim. */
			skipped++;
			continue;
		}

		int ihl = iph->ihl * 4;

		if (iph->protocol != IPPROTO_ICMP ||
		    n < ihl + (ssize_t)sizeof(struct icmphdr)) {
			skipped++;
			continue;
		}

		struct icmphdr *icmph = (struct icmphdr *)(buf + ihl);

		if (icmph->type != ICMP_ECHO) {
			skipped++;
			continue;
		}

		/* --- Build echo reply --- */

		/* Swap source / destination IP. */
		uint32_t tmp_addr = iph->saddr;
		iph->saddr = iph->daddr;
		iph->daddr = tmp_addr;

		/* Reset TTL; recompute IP header checksum. */
		iph->ttl   = 64;
		iph->check = 0;
		iph->check = cksum(iph, ihl);

		/* Set ICMP type to echo reply; recompute ICMP checksum. */
		icmph->type     = ICMP_ECHOREPLY;
		icmph->checksum = 0;
		icmph->checksum = cksum(icmph, (int)(n - ihl));

		if (write(fd, buf, (size_t)n) != n) {
			perror("write");
			break;
		}

		handled++;

		/* Print src→dst for each handled ping. */
		char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &iph->saddr, src, sizeof(src));
		inet_ntop(AF_INET, &iph->daddr, dst, sizeof(dst));
		fprintf(stderr, "ccsds_sim_echo: echo reply  %s → %s  (id=%u seq=%u)\n",
			src, dst,
			ntohs(icmph->un.echo.id),
			ntohs(icmph->un.echo.sequence));
	}

	fprintf(stderr, "\nccsds_sim_echo: done.  handled=%lu  skipped=%lu\n",
		handled, skipped);
	close(fd);
	return 0;
}
