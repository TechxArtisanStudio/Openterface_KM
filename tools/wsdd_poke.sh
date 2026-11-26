#!/bin/sh
# Poke an address-change event on the KeyMod NCM interface so wsdd binds its
# 3702 sockets even when its initial netlink dump races the interface setup.
IFACE=$(sh "$(dirname "$0")/find_host_nic.sh") || exit 1
exec /usr/sbin/ip addr change 192.168.20.1/24 dev "$IFACE"
