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

# The NIC's MAC is UID-derived per unit (shape 02:xx:xx:xx:xx:01), so read it
# off the plugged-in unit instead of hardcoding the old fixed address.
HOST_MAC="$(sh "$(dirname "$0")/find_host_nic.sh" -m)"     # firmware host-facing NIC MAC
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
# $HOST_IP, so the target OS connectivity probe lands here.
#
# The OS probes are answered with the exact bodies they expect instead of a
# redirect: the firmware only *spoofs* the NCSI DNS probe (dns.msftncsi.com)
# during the greeting window after a plug-in, and once it stops, a 302 here
# would keep Windows convinced the link is still a captive portal - it then
# relaunches the portal browser (msftconnecttest.com/redirect -> msn.com) on
# every poll. A real 200 lets the connectivity state settle.
server {
    listen $HOST_IP:80;
    server_name _;

    # Windows NCSI
    location = /connecttest.txt {
        default_type text/plain;
        add_header Cache-Control "no-store" always;
        return 200 "Microsoft Connect Test";
    }
    location = /ncsi.txt {
        default_type text/plain;
        add_header Cache-Control "no-store" always;
        return 200 "Microsoft NCSI";
    }
    # macOS captive probe expects this exact document to mean "no portal"
    location = /hotspot-detect.html {
        default_type text/html;
        add_header Cache-Control "no-store" always;
        return 200 "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    }
    # Android / ChromeOS
    location = /generate_204 {
        return 204;
    }

    # Portal URL the OS opens while a portal is still detected, plus any
    # address the user typed: show the file manager.
    location / {
        return 302 http://$HOST_IP:8080/;
    }
}
EOF
nginx -t
systemctl enable --now nginx >/dev/null 2>&1 || true
systemctl reload nginx

echo "[3/3] Self test"
IFACE="$(sh "$(dirname "$0")/find_host_nic.sh")"
sleep 1
curl -s --noproxy "*" -m 5 -o /dev/null \
    -w "NCSI probe: HTTP %{http_code} (expect 200)\n" \
    "http://$HOST_IP/connecttest.txt"
curl -s --noproxy "*" -m 5 -o /dev/null \
    -w "portal URL: HTTP %{http_code} -> %{redirect_url} (expect 302)\n" \
    "http://$HOST_IP/redirect"
echo "Interface: ${IFACE:-unknown}  Portal redirect: OK"
echo "On next plug-in the target OS should show its captive-portal popup once."
