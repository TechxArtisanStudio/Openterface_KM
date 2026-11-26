#!/bin/sh
# Resolve the KeyMod NCM interface at start time, so the unit survives USB
# re-enumeration renames (enp0s21f0uXuY -> ...).  The MAC is UID-derived per
# unit now, so match the firmware's shape 02:xx:xx:xx:xx:01.
IFACE=$(sh "$(dirname "$0")/find_host_nic.sh")
echo "wsdd-iface: using $IFACE"
exec /usr/sbin/wsdd -i "$IFACE" -4 -n KEYMOD-HOST -w WORKGROUP
