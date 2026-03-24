// SPDX-License-Identifier: GPL-2.0
/*
 * ccsds_sim_read - read one packet from /dev/ccsdssim (non-blocking)
 *
 * Prints the IP version number (4 or 6) to stdout.
 *
 * Exit codes:
 *   0  - got a packet; IP version printed to stdout
 *   1  - queue empty (EAGAIN)
 *   2  - error
 *
 * Used by ccsds_test.sh to replace the python3 sim_read_one() helper,
 * allowing the test suite to run on a BusyBox-only rootfs.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define DEVPATH  "/dev/ccsdssim"
#define BUFSIZE  1500

int main(int argc, char *argv[])
{
	const char *devpath = (argc > 1) ? argv[1] : DEVPATH;
	unsigned char buf[BUFSIZE];
	int fd;
	ssize_t n;

	fd = open(devpath, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror(devpath);
		return 2;
	}

	n = read(fd, buf, sizeof(buf));
	close(fd);

	if (n < 0) {
		if (errno == EAGAIN)
			return 1;	/* queue empty */
		perror("read");
		return 2;
	}

	if (n >= 1)
		printf("%d\n", (buf[0] >> 4) & 0xf);

	return 0;
}
