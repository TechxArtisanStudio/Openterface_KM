#!/bin/sh
# find_host_nic.sh [-m] — echo the KeyMod host-facing USB NIC (or its MAC).
#
# The firmware derives every MAC from the chip's unique ID, so a unit's address
# is stable but not known in advance: only two bytes are predictable
#
#     02 : xx xx xx xx : 01
#
#   02  locally administered + unicast
#   01  the "host NIC" role byte (see APP/include/mac_uid.h)
#
# Matching on that shape (instead of the old fixed 02:00:00:00:00:01) keeps
# these helper scripts working across units, and across the legacy fixed
# address, which matches the same pattern.
#
#   -m  print the MAC instead of the interface name
#
# Exit status 1 (and no output) when no such NIC is plugged in.
set -u

want_mac=0
[ "${1:-}" = "-m" ] && want_mac=1

# One ip link line per interface: "3: enx0285009ed101: <...> link/ether 02:85:..:01 brd ..."
line=$(ip -o link | tr 'A-Z' 'a-z' |
       sed -nE 's/^[0-9]+: (([^:@]+))[@:].*link\/ether (02(:[0-9a-f][0-9a-f]){4}:01).*/\3 \2/p' |
       head -1)

[ -n "$line" ] || {
    echo "keymod host NIC (02:xx:xx:xx:xx:01) not found" >&2
    exit 1
}

if [ "$want_mac" = 1 ]; then
    echo "${line%% *}"
else
    echo "${line##* }"
fi
