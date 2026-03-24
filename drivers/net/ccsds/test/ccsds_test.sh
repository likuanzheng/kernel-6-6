#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# ccsds_test.sh - End-to-end test for ccsdsnet + ccsdssim
#
# Test cases:
#   T1  Module load:          insmod ccsds.ko
#   T2  Char device:          /dev/ccsdssim is a character device
#   T3  Net device:           ccsdsnet appears in ip link
#   T4  Interface up:         ip link set ccsdsnet up + IPv4/IPv6 addr add
#   T5  TX path:              ICMP echo request readable from /dev/ccsdssim
#   T6  RX path:              raw IP packet written to /dev/ccsdssim reaches stack
#   T7  IPv4 ping loopback:   ping (ICMPv4) with sim_echo -> replies received
#   T8  IPv6 ping loopback:   ping6 (ICMPv6) with sim_echo -> replies received
#   T9  Stats:                tx_packets / rx_packets non-zero after T7+T8
#   T10 Module unload:        rmmod succeeds, netdev + chardev gone
#
# Prerequisites:
#   - Run as root
#   - ccsds.ko, ccsds_sim_echo, ccsds_sim_read, ccsds_inject_reply must exist
#     (run 'make' in the test/ directory first)
#   - ip, ping, awk available (standard BusyBox tools, no python3 required)

set -eu

# ── Paths ────────────────────────────────────────────────────────────────────
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
KO="$SCRIPT_DIR/ccsds.ko"
SIM_ECHO="$SCRIPT_DIR/ccsds_sim_echo"
SIM_READ="$SCRIPT_DIR/ccsds_sim_read"
INJECT_REPLY="$SCRIPT_DIR/ccsds_inject_reply"
SIM_DEV="/dev/ccsdssim"
NET_DEV="ccsdsnet"
LOCAL_IP="10.99.1.1"
PEER_IP="10.99.1.2"
PREFIX=24
IPv6_LOCAL="fd00::1"
IPv6_PEER="fd00::2"
IPv6_PREFIX=64

# ── Counters ─────────────────────────────────────────────────────────────────
PASS=0
FAIL=0
SIM_PID=""

# ── Helpers ──────────────────────────────────────────────────────────────────
green='\033[0;32m'; red='\033[0;31m'; reset='\033[0m'
pass() { printf "${green}[PASS]${reset} %s\n" "$1"; PASS=$((PASS + 1)); }
fail() { printf "${red}[FAIL]${reset} %s\n" "$1"; FAIL=$((FAIL + 1)); }

require_root() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "ERROR: must be run as root"
		exit 1
	fi
}

require_files() {
	local missing=0
	for f in "$KO" "$SIM_ECHO" "$SIM_READ" "$INJECT_REPLY"; do
		if [ ! -f "$f" ]; then
			echo "ERROR: missing $f  (run 'make' in the test/ directory)"
			missing=1
		fi
	done
	[ "$missing" -eq 0 ] || exit 1
}

cleanup() {
	if [ -n "$SIM_PID" ] && kill -0 "$SIM_PID" 2>/dev/null; then
		kill "$SIM_PID" 2>/dev/null || true
		wait "$SIM_PID" 2>/dev/null || true
		SIM_PID=""
	fi
	ip link set "$NET_DEV" down 2>/dev/null || true
	rmmod ccsds 2>/dev/null || true
}
trap cleanup EXIT

# ─────────────────────────────────────────────────────────────────────────────
#  Tests
# ─────────────────────────────────────────────────────────────────────────────

require_root
require_files

echo "========================================"
echo " CCSDS driver test suite"
echo "========================================"

# T1 – Module load
echo
echo "--- T1: module load ---"
insmod "$KO"
if lsmod | grep -q '^ccsds '; then
	pass "ccsds module loaded"
else
	fail "ccsds not in lsmod"
fi

# T2 – /dev/ccsdssim exists
echo
echo "--- T2: /dev/ccsdssim exists ---"
if [ -c "$SIM_DEV" ]; then
	pass "/dev/ccsdssim is a character device"
else
	fail "/dev/ccsdssim missing or not a chardev"
fi

# T3 – Net device registered
echo
echo "--- T3: ccsdsnet registered ---"
if ip link show "$NET_DEV" >/dev/null 2>&1; then
	pass "ccsdsnet found in ip link"
else
	fail "ccsdsnet not found"
fi

# T4 – Bring up interface
echo
echo "--- T4: interface up ---"
ip link set "$NET_DEV" up
ip addr add "${LOCAL_IP}/${PREFIX}" dev "$NET_DEV" 2>/dev/null || true
# For ARPHRD_NONE point-to-point devices the state shows as UNKNOWN even when
# up; check the flags word (<...>) instead.
if ip link show "$NET_DEV" | head -1 | grep -q ',UP[,>]'; then
	pass "ccsdsnet is UP  IPv4=$LOCAL_IP/$PREFIX"
else
	fail "ccsdsnet not UP"
fi

# T5 – TX path: packet flows IP -> sim
echo
echo "--- T5: TX path (IP stack -> /dev/ccsdssim) ---"
# Trigger a TX packet: one ping that will time out (no responder yet).
ping -c 1 -W 1 "$PEER_IP" >/dev/null 2>&1 &
PING_PID=$!
sleep 1
kill "$PING_PID" 2>/dev/null || true
wait "$PING_PID" 2>/dev/null || true

# ccsds_sim_read prints the IP version (4/6) to stdout; exits 1 if EAGAIN.
# Loop to skip any queued IPv6 packets (e.g. RS auto-sent when IPv6 is loaded).
TX_OK=0
tries=0
while [ $tries -lt 8 ]; do
	VERSION=$("$SIM_READ" 2>/dev/null) || break
	tries=$((tries + 1))
	if [ "$VERSION" = "4" ]; then TX_OK=1; break; fi
done
if [ "$TX_OK" -eq 1 ]; then
	pass "TX: read IPv4 packet from /dev/ccsdssim"
else
	fail "TX: no IPv4 packet in /dev/ccsdssim (queue empty or only IPv6)"
fi

# T6 – RX path: sim -> IP stack
echo
echo "--- T6: RX path (/dev/ccsdssim -> IP stack) ---"
"$INJECT_REPLY" "$LOCAL_IP" "$PEER_IP" >/dev/null 2>&1 || true
sleep 1
RX=$(cat /sys/class/net/"$NET_DEV"/statistics/rx_packets 2>/dev/null || echo 0)
if [ "$RX" -gt 0 ]; then
	pass "RX: rx_packets=$RX after injection"
else
	fail "RX: rx_packets still 0 after injection"
fi

# T7 – IPv4 ping loopback via sim_echo
echo
echo "--- T7: IPv4 ping loopback via ccsds_sim_echo ---"
: > "$SCRIPT_DIR/sim_echo.log"
"$SIM_ECHO" "$SIM_DEV" 2>>"$SCRIPT_DIR/sim_echo.log" &
SIM_PID=$!
sleep 1  # give it time to open the device

PING_OUT=$(ping -c 4 -W 2 "$PEER_IP" 2>&1) || true
# GNU ping:    "3 received,"         -> $(i-1) is the count
# BusyBox ping: "3 packets received," -> $(i-1)="packets", $(i-2) is the count
RECEIVED=$(printf '%s\n' "$PING_OUT" | \
	awk '/received/ {
	    for (i=1; i<=NF; i++) {
	        if ($i ~ /^received/) {
	            n = $(i-1) ~ /^[0-9]/ ? $(i-1)+0 : $(i-2)+0
	            print n; exit
	        }
	    }
	}')
RECEIVED=${RECEIVED:-0}

kill "$SIM_PID" 2>/dev/null || true
wait "$SIM_PID" 2>/dev/null || true
SIM_PID=""

cat "$SCRIPT_DIR/sim_echo.log" >&2

if [ "${RECEIVED:-0}" -gt 0 ]; then
	pass "IPv4 ping loopback: $RECEIVED/4 replies received"
else
	fail "IPv4 ping loopback: 0 replies"
fi

# T8 – IPv6 ping loopback via sim_echo
echo
echo "--- T8: IPv6 ping loopback via ccsds_sim_echo ---"
if ! command -v ping6 >/dev/null 2>&1; then
	echo "SKIP: ping6 not available"
else
	# IPv6 may be a loadable module; ensure it is loaded and the address
	# is assigned before starting the test.  modprobe is a no-op when
	# IPv6 is already loaded or built-in.
	modprobe ipv6 2>/dev/null || true
	ip addr add "${IPv6_LOCAL}/${IPv6_PREFIX}" dev "$NET_DEV" 2>/dev/null || true

	# Snapshot rx_packets before the test so we can measure the delta.
	RX_BEFORE=$(cat /sys/class/net/"$NET_DEV"/statistics/rx_packets \
		    2>/dev/null || echo 0)

	"$SIM_ECHO" "$SIM_DEV" 2>>"$SCRIPT_DIR/sim_echo.log" &
	SIM_PID=$!
	sleep 1

	# ping6 output format varies across BusyBox builds; verify the test
	# through sim_echo reply count and rx_packets delta instead.
	ping6 -c 4 -W 2 "$IPv6_PEER" >/dev/null 2>&1 || true
	sleep 1  # wait for any in-flight replies to be processed

	kill "$SIM_PID" 2>/dev/null || true
	wait "$SIM_PID" 2>/dev/null || true
	SIM_PID=""

	cat "$SCRIPT_DIR/sim_echo.log" >&2

	RX_AFTER=$(cat /sys/class/net/"$NET_DEV"/statistics/rx_packets \
		   2>/dev/null || echo 0)
	RX_DELTA=$((RX_AFTER - RX_BEFORE))
	# Count echo replies sim_echo sent back (each is one "ICMPv6 Echo Reply" line).
	REPLIED=$(grep -c "ICMPv6 Echo Reply" "$SCRIPT_DIR/sim_echo.log" \
		  2>/dev/null || true)
	REPLIED=${REPLIED:-0}

	if [ "${REPLIED:-0}" -gt 0 ] && [ "$RX_DELTA" -ge "${REPLIED:-0}" ]; then
		pass "IPv6 ping loopback: sim_echo replied=$REPLIED  rx_delta=$RX_DELTA"
	else
		fail "IPv6 ping loopback: sim_echo replied=$REPLIED  rx_delta=$RX_DELTA"
	fi
fi

# T9 – Statistics
echo
echo "--- T9: driver statistics ---"
TX_PKT=$(cat /sys/class/net/"$NET_DEV"/statistics/tx_packets 2>/dev/null || echo 0)
RX_PKT=$(cat /sys/class/net/"$NET_DEV"/statistics/rx_packets 2>/dev/null || echo 0)

if [ "$TX_PKT" -gt 0 ] && [ "$RX_PKT" -gt 0 ]; then
	pass "Stats: tx_packets=$TX_PKT  rx_packets=$RX_PKT"
elif [ "$TX_PKT" -gt 0 ]; then
	fail "Stats: tx_packets=$TX_PKT but rx_packets=$RX_PKT"
else
	fail "Stats: tx_packets=$TX_PKT  rx_packets=$RX_PKT (both should be > 0)"
fi

# T10 – Module unload
echo
echo "--- T10: module unload ---"
ip link set "$NET_DEV" down 2>/dev/null || true
rmmod ccsds 2>/dev/null
if ! lsmod | grep -q '^ccsds '; then
	pass "ccsds module unloaded"
else
	fail "ccsds still present in lsmod"
fi

if [ ! -e "$SIM_DEV" ] && ! ip link show "$NET_DEV" >/dev/null 2>&1; then
	pass "cleanup: /dev/ccsdssim and ccsdsnet both gone"
else
	fail "cleanup: stale device nodes after rmmod"
fi

# ── Summary ──────────────────────────────────────────────────────────────────
echo
echo "========================================"
printf " Results: ${green}%d passed${reset}  ${red}%d failed${reset}\n" "$PASS" "$FAIL"
echo "========================================"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
