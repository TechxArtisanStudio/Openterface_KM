#!/bin/sh
# Resolve the KeyMod NCM interface by its stable MAC at start time, so the
# unit survives USB re-enumeration renames (enp0s21f0uXuY -> ...).
IFACE=$(ip -o link | grep -i '02:00:00:00:00:01' | sed -E 's/^[0-9]+: ([^:@]+).*/\1/' | head -1)
[ -n "$IFACE" ] || { echo "keymod NCM interface (MAC 02:00:00:00:00:01) not found" >&2; exit 1; }
echo "wsdd-iface: using $IFACE"
exec /usr/sbin/wsdd -i "$IFACE" -4 -n KEYMOD-HOST -w WORKGROUP
