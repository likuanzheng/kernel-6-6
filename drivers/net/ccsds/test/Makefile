# Makefile for CCSDS userspace test tools
# Usage:
#   make           – build all test binaries
#   make clean     – remove built files
#   make test      – build then run ccsds_test.sh (requires root)
#
# Cross-compilation: set CC to your cross-compiler, e.g.
#   make CC=aarch64-linux-gnu-gcc

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -D_GNU_SOURCE -static

PROGS   := ccsds_sim_echo ccsds_sim_read ccsds_inject_reply

.PHONY: all clean test

all: $(PROGS)

ccsds_sim_echo: ccsds_sim_echo.c
	$(CC) $(CFLAGS) -o $@ $<

ccsds_sim_read: ccsds_sim_read.c
	$(CC) $(CFLAGS) -o $@ $<

ccsds_inject_reply: ccsds_inject_reply.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS) sim_echo.log

test: $(PROGS)
	sh ccsds_test.sh
