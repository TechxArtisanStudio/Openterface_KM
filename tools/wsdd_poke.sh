#!/bin/sh
# Poke an address-change event on the KeyMod NCM interface so wsdd binds its
# 3702 sockets even when its initial netlink dump races the interface setup.
IFACE=$(ip -o link | grep -i '02:00:00:00:00:01' | sed -E 's/^[0-9]+: ([^:@]+).*/\1/' | head -1)
[ -n "$IFACE" ] || exit 1
exec /usr/sbin/ip addr change 192.168.20.1/24 dev "$IFACE"
