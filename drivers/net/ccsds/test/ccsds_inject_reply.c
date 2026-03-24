// SPDX-License-Identifier: GPL-2.0
/*
 * ccsds_inject_reply - inject an ICMP echo reply into /dev/ccsdssim
 *
 * Usage: ccsds_inject_reply <dst_ip> <src_ip>
 *   dst_ip  local IP address (receiver of the reply, e.g. 10.99.1.1)
 *   src_ip  peer  IP address (sender  of the reply, e.g. 10.99.1.2)
 *
 * Builds a minimal IPv4/ICMP echo reply packet with correct checksums
 * and writes it to /dev/ccsdssim so the kernel receives it on ccsdsnet.
 *
 * Replaces the python3 inject_icmp_reply() helper in ccsds_test.sh,
 * allowing the test suite to run on a BusyBox-only rootfs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#define DEVPATH "/dev/ccsdssim"

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
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <dst_ip> <src_ip>\n", argv[0]);
		return 1;
	}

	const char *dst_ip = argv[1];	/* local: receiver */
	const char *src_ip = argv[2];	/* peer:  sender   */

	static const char payload[] = "ccsdstest";
	int payload_len = (int)(sizeof(payload) - 1);
	int ihl      = (int)sizeof(struct iphdr);
	int icmp_len = (int)sizeof(struct icmphdr) + payload_len;
	int total    = ihl + icmp_len;

	unsigned char pkt[1500];
	memset(pkt, 0, (size_t)total);

	/* IPv4 header */
	struct iphdr *iph = (struct iphdr *)pkt;
	iph->version  = 4;
	iph->ihl      = 5;
	iph->tot_len  = htons((uint16_t)total);
	iph->id       = htons(0x0001);
	iph->ttl      = 64;
	iph->protocol = IPPROTO_ICMP;
	if (inet_pton(AF_INET, src_ip, &iph->saddr) != 1) {
		fprintf(stderr, "bad src ip: %s\n", src_ip);
		return 1;
	}
	if (inet_pton(AF_INET, dst_ip, &iph->daddr) != 1) {
		fprintf(stderr, "bad dst ip: %s\n", dst_ip);
		return 1;
	}
	iph->check = cksum(iph, ihl);

	/* ICMP echo reply */
	struct icmphdr *icmph = (struct icmphdr *)(pkt + ihl);
	icmph->type             = ICMP_ECHOREPLY;
	icmph->code             = 0;
	icmph->un.echo.id       = htons(0x1234);
	icmph->un.echo.sequence = htons(1);
	memcpy(pkt + ihl + (int)sizeof(struct icmphdr), payload,
	       (size_t)payload_len);
	icmph->checksum = cksum(icmph, icmp_len);

	int fd = open(DEVPATH, O_WRONLY);
	if (fd < 0) {
		perror(DEVPATH);
		return 1;
	}

	ssize_t n = write(fd, pkt, (size_t)total);
	close(fd);

	if (n != (ssize_t)total) {
		fprintf(stderr, "short write: %zd/%d\n", n, total);
		return 1;
	}

	printf("injected %d-byte ICMP reply %s -> %s\n", total, src_ip, dst_ip);
	return 0;
}
