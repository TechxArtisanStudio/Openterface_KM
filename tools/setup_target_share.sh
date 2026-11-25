#!/usr/bin/env bash
# Configure the Fedora Host to publish ~/SharedWithTarget on the Windows
# target's "Network" view over the KeyMod NCM bridge.
#
#   sudo tools/setup_target_share.sh [interface]
#
# Default interface is the USB NCM data interface used by the target PC
# (enp0s21f0u1u2). The script is idempotent: it only appends the share stanza
# and the systemd unit once, then restarts the services.
set -euo pipefail

IFACE="${1:-enp0s21f0u1u2}"
SHARE_DIR="${SHARE_DIR:-/home/bbot/SharedWithTarget}"
# Unique product identity: never use a generic name (e.g. FEDORA) — any
# machine with the same hostname on an adjacent network steals name
# resolution and the Network-view entry stops working.
NETBIOS_NAME="${NETBIOS_NAME:-KEYMOD-HOST}"
WORKGROUP="${WORKGROUP:-WORKGROUP}"
SMB_CONF="/etc/samba/smb.conf"
WSDD_UNIT="/etc/systemd/system/wsdd-ncm.service"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0 $*" >&2
    exit 1
fi

command -v smbd >/dev/null || { echo "smbd not installed (dnf install samba)" >&2; exit 1; }
command -v nmbd >/dev/null || { echo "nmbd not installed (dnf install samba)" >&2; exit 1; }
command -v wsdd >/dev/null || { echo "wsdd not installed" >&2; exit 1; }
ip link show "$IFACE" >/dev/null 2>&1 || { echo "interface $IFACE not found" >&2; exit 1; }

echo "[1/6] Preparing $SHARE_DIR"
mkdir -p "$SHARE_DIR"
chown bbot:bbot "$SHARE_DIR" || true
chmod 0775 "$SHARE_DIR"

echo "[2/6] Configuring Samba global options and share"
if ! grep -q '^[[:space:]]*map to guest' "$SMB_CONF"; then
    sed -i '/^\[global\]/a map to guest = Bad User' "$SMB_CONF"
fi
sed -i -E "s/^([[:space:]]*)workgroup[[:space:]]*=.*/\1workgroup = ${WORKGROUP}/" "$SMB_CONF"
if ! grep -q '^[[:space:]]*netbios name' "$SMB_CONF"; then
    sed -i "/^\[global\]/a netbios name = ${NETBIOS_NAME}" "$SMB_CONF"
fi
if ! grep -q '^\[SharedWithTarget\]' "$SMB_CONF"; then
    cat >> "$SMB_CONF" <<EOF

[SharedWithTarget]
    comment = KeyMod target share (Host files)
    path = ${SHARE_DIR}
    browseable = yes
    guest ok = yes
    read only = no
    force user = bbot
    force group = bbot
    create mask = 0664
    directory mask = 0775
EOF
fi
testparm -s >/dev/null

echo "[3/6] Installing wsdd systemd unit on $IFACE"
cat > "$WSDD_UNIT" <<EOF
[Unit]
Description=WS-Discovery on KeyMod NCM interface
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/sbin/wsdd -i ${IFACE} -4 -n ${NETBIOS_NAME} -w ${WORKGROUP}
# wsdd 0.8 misses the initial netlink address dump on some boots, so poke an
# address event after startup; wsdd then binds 3702 to the interface address.
ExecStartPost=/usr/sbin/ip addr change 192.168.20.1/24 dev ${IFACE}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload

echo "[4/6] Opening firewall ports"
if systemctl is-active --quiet firewalld; then
    ZONE="$(firewall-cmd --get-zone-of-interface "$IFACE" 2>/dev/null || true)"
    [ -n "$ZONE" ] || ZONE="FedoraWorkstation"
    firewall-cmd --permanent --zone="$ZONE" --add-service=samba >/dev/null
    for port in 3702/udp 5357/tcp 1900/udp 5353/udp; do
        firewall-cmd --permanent --zone="$ZONE" --add-port="$port" >/dev/null 2>&1 || true
    done
    firewall-cmd --reload >/dev/null
else
    echo "firewalld inactive; skipping firewall changes"
fi

echo "[5/6] Allowing Samba to read the Home directory (SELinux)"
if command -v setsebool >/dev/null 2>&1; then
    setsebool -P samba_enable_home_dirs on
fi
if command -v semanage >/dev/null 2>&1; then
    semanage fcontext -a -t samba_share_t "${SHARE_DIR}(/.*)?" 2>/dev/null || true
    restorecon -R "$SHARE_DIR" >/dev/null 2>&1 || true
fi

echo "[6/6] Starting smb, nmb and wsdd-ncm"
systemctl enable --now smb.service nmb.service wsdd-ncm.service >/dev/null
systemctl --no-pager --plain status smb.service nmb.service wsdd-ncm.service | grep -E 'Loaded|Active|ExecStart' || true

echo "Done. Windows target should now see ${NETBIOS_NAME} (share: SharedWithTarget)."
