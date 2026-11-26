#!/usr/bin/env bash
# Captive-portal setup on the Host (Fedora) for the KeyMod NCM link.
#
# Mimics a hotel/Starbucks captive portal: the target installs nothing.
#   - Firmware provides DHCP (portal IP as DNS+gateway) and hijacks every
#     DNS query crossing the bridge (see ncm_bridge.c portal config).
#   - This script makes the HOST side persistent:
#       1. NetworkManager profile bound to the device MAC (survives USB
#          renames/replugs) holding 192.168.20.1/24.
#       2. nginx server block on port 80 redirecting the OS connectivity
#         probe to the file manager on 8080.
#
# Do NOT run dnsmasq on this link: the firmware already serves DHCP there.
#
# Idempotent — safe to re-run. Requires root:
#   sudo tools/setup_captive_portal.sh
set -euo pipefail

HOST_MAC="02:00:00:00:00:01"     # firmware host-facing NIC MAC
HOST_IP="192.168.20.1"
NGINX_CONF="/etc/nginx/conf.d/keymod-portal.conf"

if [[ $EUID -ne 0 ]]; then
    echo "run with sudo" >&2
    exit 1
fi

echo "[1/3] NetworkManager profile (MAC-bound, survives renames)"
if ! nmcli -g connection.id show keymod-ncm >/dev/null 2>&1; then
    nmcli con add type ethernet ifname "*" mac "$HOST_MAC" \
        con-name keymod-ncm ipv4.method manual \
        ipv4.addresses "$HOST_IP/24" ipv6.method disabled autoconnect yes
fi
nmcli con up keymod-ncm || true

echo "[2/3] nginx portal redirect"
if ! command -v nginx >/dev/null; then
    dnf install -y nginx
fi
cat > "$NGINX_CONF" <<EOF
# KeyMod captive portal: the firmware DNS hijack sends every name to
# $HOST_IP, so the target OS connectivity probe (Windows NCSI,
# macOS captive.apple.com, Android generate_204) lands here and is
# redirected to the file manager.
server {
    listen $HOST_IP:80;
    server_name _;
    return 302 http://$HOST_IP:8080/;
}
EOF
nginx -t
systemctl enable --now nginx >/dev/null 2>&1 || true
systemctl reload nginx

echo "[3/3] Self test"
IFACE="$(ip -o link | grep -i "$HOST_MAC" | sed -E 's/^[0-9]+: ([^:@]+).*/\1/')"
sleep 1
curl -s --noproxy "*" -m 5 -o /dev/null \
    -w "probe: HTTP %{http_code} -> %{redirect_url}\n" \
    "http://$HOST_IP/connecttest.txt"
echo "Interface: ${IFACE:-unknown}  Portal redirect: OK"
echo "On next plug-in the target OS should show its captive-portal popup."
