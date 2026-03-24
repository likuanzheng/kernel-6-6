// SPDX-License-Identifier: GPL-2.0
/*
 * ccsds_sim_echo.c - Userspace echo simulator for /dev/ccsdssim
 *
 * Reads ICMP/ICMPv6 packets from /dev/ccsdssim (TX path from ccsdsnet),
 * handles echo requests and NDP neighbor solicitations, then writes replies
 * back (RX path into ccsdsnet).
 *
 * IPv4: ICMPv4 echo request  -> echo reply
 * IPv6: ICMPv6 echo request  -> echo reply
 *       ICMPv6 neighbor sol  -> neighbor advertisement
 *
 * Each echo request/reply is hex-dumped to stderr with protocol-header
 * boundaries clearly labelled.
 *
 * Usage:
 *   ./ccsds_sim_echo [/dev/ccsdssim]
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
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

#define DEVPATH  "/dev/ccsdssim"
#define BUFSIZE  1500

static volatile int g_stop;

static void sig_handler(int s) { (void)s; g_stop = 1; }

/* ── Checksum helpers ────────────────────────────────────────────────────── */

static uint16_t cksum(const void *data, int len)
{
	const uint16_t *p = (const uint16_t *)data;
	uint32_t sum = 0;

	while (len > 1) { sum += *p++; len -= 2; }
	if (len) sum += *(const uint8_t *)p;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return ~(uint16_t)sum;
}

static uint16_t icmp6_cksum(const struct ip6_hdr *ip6h,
			     const void *data, int len)
{
	struct {
		struct in6_addr src;
		struct in6_addr dst;
		uint32_t        upper_len;
		uint8_t         zeros[3];
		uint8_t         next;
	} pseudo;
	const uint16_t *p;
	uint32_t sum = 0;
	int i, l;

	pseudo.src       = ip6h->ip6_src;
	pseudo.dst       = ip6h->ip6_dst;
	pseudo.upper_len = htonl((uint32_t)len);
	memset(pseudo.zeros, 0, sizeof(pseudo.zeros));
	pseudo.next      = IPPROTO_ICMPV6;

	p = (const uint16_t *)&pseudo;
	for (i = 0; i < (int)(sizeof(pseudo) / 2); i++) sum += p[i];

	p = (const uint16_t *)data;
	for (l = len; l > 1; l -= 2) sum += *p++;
	if (l) sum += *(const uint8_t *)p;

	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return ~(uint16_t)sum;
}

/* ── Hex dump ────────────────────────────────────────────────────────────── */

/*
 * Print one labelled section of a packet in hex, 16 bytes per row.
 *
 * Example output:
 *   |-- IP Header      [ 20 bytes] --|
 *   45 00 00 54 12 34 40 00 40 01 f0 fe 0a 63 01 01
 *   0a 63 01 02
 */
static void hexdump_section(const char *label, const uint8_t *data, int len)
{
	int i;

	fprintf(stderr, "  |-- %-14s [%3d bytes] --|\n", label, len);
	for (i = 0; i < len; i++) {
		if (i % 16 == 0)
			fprintf(stderr, "  ");
		fprintf(stderr, "%02x ", data[i]);
		if ((i + 1) % 16 == 0 || i == len - 1)
			fprintf(stderr, "\n");
	}
}

static void dump_ipv4(const char *direction,
		      const char *src, const char *dst,
		      uint16_t id, uint16_t seq,
		      const uint8_t *buf, ssize_t n, int ihl)
{
	int icmp_hdr_len  = (int)sizeof(struct icmphdr);
	int payload_len   = (int)n - ihl - icmp_hdr_len;

	fprintf(stderr, "[IPv4 %s] %s -> %s  (id=%u seq=%u)\n",
		direction, src, dst, id, seq);
	hexdump_section("IP Header",    buf,                         ihl);
	hexdump_section("ICMP Header",  buf + ihl,                   icmp_hdr_len);
	if (payload_len > 0)
		hexdump_section("Payload", buf + ihl + icmp_hdr_len, payload_len);
}

static void dump_ipv6(const char *direction,
		      const char *src, const char *dst,
		      uint16_t id, uint16_t seq,
		      const uint8_t *buf, ssize_t n)
{
	int ip6_hdr_len   = (int)sizeof(struct ip6_hdr);
	int icmp6_hdr_len = (int)sizeof(struct icmp6_hdr);
	int payload_len   = (int)n - ip6_hdr_len - icmp6_hdr_len;

	fprintf(stderr, "[IPv6 %s] %s -> %s  (id=%u seq=%u)\n",
		direction, src, dst, id, seq);
	hexdump_section("IPv6 Header",   buf,                                  ip6_hdr_len);
	hexdump_section("ICMPv6 Header", buf + ip6_hdr_len,                   icmp6_hdr_len);
	if (payload_len > 0)
		hexdump_section("Payload",   buf + ip6_hdr_len + icmp6_hdr_len, payload_len);
}

/* ── IPv4 / ICMPv4 ───────────────────────────────────────────────────────── */

static int handle_ipv4(int fd, uint8_t *buf, ssize_t n,
		       unsigned long *handled, unsigned long *skipped)
{
	struct iphdr   *iph;
	struct icmphdr *icmph;
	int ihl;
	uint32_t tmp;
	uint16_t id, seq;
	char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];

	if (n < (ssize_t)sizeof(struct iphdr)) { (*skipped)++; return 0; }

	iph = (struct iphdr *)buf;
	ihl = iph->ihl * 4;

	if (iph->protocol != IPPROTO_ICMP ||
	    n < ihl + (ssize_t)sizeof(struct icmphdr)) {
		(*skipped)++;
		return 0;
	}

	icmph = (struct icmphdr *)(buf + ihl);
	if (icmph->type != ICMP_ECHO) { (*skipped)++; return 0; }

	/* ── Print request (before modification) ── */
	id  = ntohs(icmph->un.echo.id);
	seq = ntohs(icmph->un.echo.sequence);
	inet_ntop(AF_INET, &iph->saddr, src, sizeof(src));
	inet_ntop(AF_INET, &iph->daddr, dst, sizeof(dst));
	fprintf(stderr, "\n");
	dump_ipv4("ICMP Echo Request", src, dst, id, seq, buf, n, ihl);

	/* ── Build echo reply ── */
	tmp = iph->saddr; iph->saddr = iph->daddr; iph->daddr = tmp;
	iph->ttl = 64; iph->check = 0; iph->check = cksum(iph, ihl);

	icmph->type     = ICMP_ECHOREPLY;
	icmph->checksum = 0;
	icmph->checksum = cksum(icmph, (int)(n - ihl));

	/* ── Print reply (after modification) ── */
	inet_ntop(AF_INET, &iph->saddr, src, sizeof(src));
	inet_ntop(AF_INET, &iph->daddr, dst, sizeof(dst));
	dump_ipv4("ICMP Echo Reply  ", src, dst, id, seq, buf, n, ihl);

	if (write(fd, buf, (size_t)n) != n) { perror("write"); return -1; }

	(*handled)++;
	return 0;
}

/* ── IPv6 / ICMPv6 ───────────────────────────────────────────────────────── */

static int send_neighbor_advert(int fd, const struct ip6_hdr *req_ip6h,
				const struct nd_neighbor_solicit *ns)
{
	struct {
		struct ip6_hdr          ip6h;
		struct nd_neighbor_advert na;
	} pkt;
	int na_len = (int)sizeof(struct nd_neighbor_advert);
	char tgt[INET6_ADDRSTRLEN];

	memset(&pkt, 0, sizeof(pkt));

	pkt.ip6h.ip6_vfc  = 0x60;
	pkt.ip6h.ip6_plen = htons((uint16_t)na_len);
	pkt.ip6h.ip6_nxt  = IPPROTO_ICMPV6;
	pkt.ip6h.ip6_hlim = 255;
	pkt.ip6h.ip6_src  = ns->nd_ns_target;
	pkt.ip6h.ip6_dst  = req_ip6h->ip6_src;

	pkt.na.nd_na_hdr.icmp6_type  = ND_NEIGHBOR_ADVERT;
	pkt.na.nd_na_hdr.icmp6_code  = 0;
	pkt.na.nd_na_flags_reserved  =
		htonl(ND_NA_FLAG_SOLICITED | ND_NA_FLAG_OVERRIDE);
	pkt.na.nd_na_target          = ns->nd_ns_target;
	pkt.na.nd_na_hdr.icmp6_cksum = 0;
	pkt.na.nd_na_hdr.icmp6_cksum =
		icmp6_cksum(&pkt.ip6h, &pkt.na, na_len);

	if (write(fd, &pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) {
		perror("write NA");
		return -1;
	}

	inet_ntop(AF_INET6, &ns->nd_ns_target, tgt, sizeof(tgt));
	fprintf(stderr, "ccsds_sim_echo: ICMPv6 NA sent  target=%s\n", tgt);
	return 0;
}

static int handle_ipv6(int fd, uint8_t *buf, ssize_t n,
		       unsigned long *handled, unsigned long *skipped)
{
	struct ip6_hdr   *ip6h    = (struct ip6_hdr *)buf;
	struct icmp6_hdr *icmp6h;
	int ip6_hdr_len = (int)sizeof(struct ip6_hdr);
	int icmp6_len;
	struct in6_addr tmp;
	uint16_t id, seq;
	char src[INET6_ADDRSTRLEN], dst[INET6_ADDRSTRLEN];

	if (n < ip6_hdr_len + (ssize_t)sizeof(struct icmp6_hdr)) {
		(*skipped)++;
		return 0;
	}
	if (ip6h->ip6_nxt != IPPROTO_ICMPV6) { (*skipped)++; return 0; }

	icmp6h    = (struct icmp6_hdr *)(buf + ip6_hdr_len);
	icmp6_len = (int)(n - ip6_hdr_len);

	switch (icmp6h->icmp6_type) {

	case ND_NEIGHBOR_SOLICIT:
		if (n < ip6_hdr_len + (ssize_t)sizeof(struct nd_neighbor_solicit)) {
			(*skipped)++;
			return 0;
		}
		if (send_neighbor_advert(fd, ip6h,
					 (struct nd_neighbor_solicit *)icmp6h) < 0)
			return -1;
		(*handled)++;
		return 0;

	case ICMP6_ECHO_REQUEST:
		/* ── Print request (before modification) ── */
		id  = ntohs(icmp6h->icmp6_dataun.icmp6_un_data16[0]);
		seq = ntohs(icmp6h->icmp6_dataun.icmp6_un_data16[1]);
		inet_ntop(AF_INET6, &ip6h->ip6_src, src, sizeof(src));
		inet_ntop(AF_INET6, &ip6h->ip6_dst, dst, sizeof(dst));
		fprintf(stderr, "\n");
		dump_ipv6("ICMPv6 Echo Request", src, dst, id, seq, buf, n);

		/* ── Build echo reply ── */
		tmp           = ip6h->ip6_src;
		ip6h->ip6_src = ip6h->ip6_dst;
		ip6h->ip6_dst = tmp;
		ip6h->ip6_hlim = 64;

		icmp6h->icmp6_type  = ICMP6_ECHO_REPLY;
		icmp6h->icmp6_cksum = 0;
		icmp6h->icmp6_cksum = icmp6_cksum(ip6h, icmp6h, icmp6_len);

		/* ── Print reply (after modification) ── */
		inet_ntop(AF_INET6, &ip6h->ip6_src, src, sizeof(src));
		inet_ntop(AF_INET6, &ip6h->ip6_dst, dst, sizeof(dst));
		dump_ipv6("ICMPv6 Echo Reply  ", src, dst, id, seq, buf, n);

		if (write(fd, buf, (size_t)n) != n) { perror("write"); return -1; }

		(*handled)++;
		return 0;

	default:
		(*skipped)++;
		return 0;
	}
}

/* ── main ────────────────────────────────────────────────────────────────── */

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
	if (fd < 0) { perror(devpath); return 1; }

	fprintf(stderr,
		"ccsds_sim_echo: opened %s, waiting for packets...\n", devpath);

	while (!g_stop) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN) {
				nanosleep(&(struct timespec){0, 500000}, NULL);
				continue;
			}
			perror("read");
			break;
		}
		if (n < 1) { skipped++; continue; }

		switch ((buf[0] >> 4) & 0xf) {
		case 4:
			if (handle_ipv4(fd, buf, n, &handled, &skipped) < 0)
				goto out;
			break;
		case 6:
			if (handle_ipv6(fd, buf, n, &handled, &skipped) < 0)
				goto out;
			break;
		default:
			skipped++;
			break;
		}
	}
out:
	fprintf(stderr, "\nccsds_sim_echo: done.  handled=%lu  skipped=%lu\n",
		handled, skipped);
	close(fd);
	return 0;
}
