# Portal and file access from the target PC

Status: **canonical** (replaces `portal-host-served-page.md` and
`target-host-file-access.md`)
Last verified: 2026-11-26, firmware `5b3aff4`, host services on Fedora,
target Windows 11 with **nothing installed**.

This is the host-side half of the side channel: what runs on the machine that
owns the dongle, and what the target PC sees. Firmware behaviour (DHCP, DNS
interception, greeting window) lives in [ncm-bridge.md](ncm-bridge.md).

---

## 1. The chain, end to end

```
target PC                                  host (owns the dongle)
   │ DHCP DISCOVER ───────────────────────► firmware replies .2 / no gateway / DNS .1
   │ DNS dns.msftncsi.com ─────────────────► firmware answers .1  (first visit only)
   │ HTTP /connecttest.txt ────────────────► nginx .1:80 → 200 "Microsoft Connect Test"
   │ OS decides: network needs sign-in
   │ opens browser at http://192.168.20.1/ ► nginx 302 → :8080
   │ GET :8080/ ───────────────────────────► host_file_server.py → file manager
   │ \\KEYMOD-HOST\SharedWithTarget ───────► Samba share
```

Everything after the DHCP step is ordinary HTTP/SMB over the emulated
Ethernet segment. The firmware never proxies, never NATs, and never speaks
HTTP itself.

---

## 2. Host services

Four units, all bound to the bridge address rather than `0.0.0.0`:

| Unit | Listens on | Purpose |
|---|---|---|
| `nginx` (`/etc/nginx/conf.d/keymod-portal.conf`) | `192.168.20.1:80` | NCSI probe bodies + redirect to the file manager |
| `dnsmasq` (`/etc/dnsmasq.d/keymod-bridge.conf`) | `192.168.20.1:53` | real resolver for lookups that outlive the greeting |
| `keymod-files.service` | `192.168.20.1:8080` | file manager + WebDAV |
| `wsdd-ncm.service` (+ `wsdd-ncm-watch.service`) | UDP 3702 | makes the host appear in the Windows *Network* view |

### nginx: answer probes, do not redirect them

```nginx
server {
    listen 192.168.20.1:80;
    server_name _;

    location = /connecttest.txt { default_type text/plain;
        add_header Cache-Control "no-store" always;
        return 200 "Microsoft Connect Test"; }     # Windows NCSI
    location = /ncsi.txt        { default_type text/plain;
        add_header Cache-Control "no-store" always;
        return 200 "Microsoft NCSI"; }
    location = /generate_204    { return 204; }     # Android / ChromeOS
    location /                  { return 302 http://192.168.20.1:8080/; }
}
```

The probe paths must return the **exact body** each OS expects. Redirecting
them keeps Windows convinced the link is still a captive portal and it
relaunches its portal browser (`msftconnecttest.com/redirect` → MSN) on every
poll — the "Edge popup storm" symptom.

macOS is the exception that benefits from the redirect: it shows its
captive-network sheet once per join, and `/` → `:8080` is what puts the file
manager on screen.

### dnsmasq: the resolver the DHCP option promises

```conf
bind-dynamic
listen-address=192.168.20.1
port=53
no-resolv
server=192.168.100.1     # the host's real uplink resolver
cache-size=1000
domain-needed
bogus-priv
```

While the greeting is armed the firmware advertises `DNS = 192.168.20.1`. If
nothing listens there, every lookup after the window black-holes and the
target looks "connected but broken". Firewalld needs an explicit allow:

```bash
firewall-cmd --permanent --zone=FedoraWorkstation \
  --add-rich-rule='rule family=ipv4 source address=192.168.20.0/24 port port=53 protocol=udp accept'
firewall-cmd --permanent --zone=FedoraWorkstation \
  --add-rich-rule='rule family=ipv4 source address=192.168.20.0/24 port port=53 protocol=tcp accept'
firewall-cmd --reload
```

Note what dnsmasq is **not** doing: it does not forward the target's traffic.
There is no NAT for `192.168.20.0/24` and there must not be one — the link is a
side channel, and NAT makes the target prefer it over its own uplink.

### keymod-files.service

```ini
[Unit]
Description=KeyMod portal file manager / WebDAV on the NCM bridge
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
Type=simple
ExecStart=/usr/bin/python3 /home/bbot/projects/Openterface/Openterface_KM/tools/host_file_server.py \
          --root /home/bbot/SharedWithTarget --bind 192.168.20.1 --port 8080
Restart=always
RestartSec=5
User=bbot

[Install]
WantedBy=multi-user.target
```

`Restart=always` plus `StartLimitIntervalSec=0` matters: the bridge address
does not exist until the dongle enumerates, so the first bind fails on every
boot and the unit must keep retrying instead of giving up.

`--read-only` disables uploads, edits and deletes. Every requested path is
resolved and confined to `--root`, so the target cannot escape the share.

### wsdd: discovery that survives re-enumeration

`wsdd` binds the interface *by name*; every replug renames it
(`enp0s21f0u1u2` → `enp0s21f0u5u2`) and the daemon dies with
`Bad file descriptor`. `wsdd-ncm-watch.service` polls every few seconds,
finds the interface by MAC `02:00:00:00:00:01`, and restarts `wsdd-ncm` when
the name changes. The host advertises `KEYMOD-HOST` (`wsdd -n` and Samba
`netbios name`).

Discovery traffic only crosses the bridge because the firmware relays IPv4
broadcasts and multicast; if the *Network* view is empty but
`\\KEYMOD-HOST` works, check the discovery counters (`0x33/0x13` offset 9) and
the watcher service.

---

## 3. What the target PC actually does

Opening `http://192.168.20.1:8080/` gives a three-view file manager (icons,
list, details) over `~/SharedWithTarget`:

| Operation | Method | Result |
|---|---|---|
| Browse directory | GET | HTML listing |
| Download file | GET (`?dl=1` forces attachment) | file, guessed MIME type |
| Upload file | PUT / POST | 201 / 200 |
| Create folder | MKCOL | 201 |
| Delete file or tree | DELETE | 204 |
| Move / copy | MOVE / COPY | 204 / 201 |
| WebDAV discovery | PROPFIND / OPTIONS | 207 / 200 |

UI notes worth keeping: the icon *is* a link in every view (only the name used
to be), the whole tile/row is clickable except the download and delete
buttons, and directory listings are served with `Cache-Control: no-store` so
an updated page never comes back stale. The page itself is currently
Chinese-only: its three view buttons are, left to right, icons, list and
details, with details selected by default.

For bulk use prefer SMB: `\\KEYMOD-HOST\SharedWithTarget` is writable, does
not need the WebDAV redirector, and does not add a `192.168.20.1@8080` entry
to the Network view. WebDAV remains available on demand
(`\\192.168.20.1@8080\DavWWWRoot` — both the leading `\\` and the trailing
`\DavWWWRoot` are required).

---

## 4. Zero-install contract

The target must work after a plain plug-in. That means:

* **Driver**: `UsbNcm.sys` (Microsoft, inbox, 10.0.22621.x). The firmware
  declares `USB\MS_COMP_WINNCM`, so Windows binds its own NCM driver. Nothing
  is downloaded or installed.
* **No agents**: no scheduled tasks, no scripts in `%ProgramData%` or the user
  profile, no desktop shortcuts, no services, no mapped drives, no saved
  credentials.
* **One registry value is unavoidable for SMB**: `AllowInsecureGuestAuth = 1`
  under `HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters`,
  because the share is `guest ok = yes` and Windows 11 blocks guest SMB by
  default. This is a system setting, not an installation. The alternative is
  a real Samba account, which costs a one-time credential prompt.
* **No auto-open helper on the target.** Earlier builds polled `192.168.20.2`
  from a scheduled task and launched the browser — that violated the contract
  and has been removed. The supported ways to open the page are: the OS
  captive-portal window (driven entirely by the firmware greeting), a manual
  `\\KEYMOD-HOST`, or the firmware's own HID launcher below.

### HID launcher (the zero-install auto-open)

`ble_eth_protocol.c` command `0x25` controls a small state machine on the
boot-keyboard interface that types `Win+R`, the configured URL, then `Enter`
on the target:

| Sub-command | Payload | Effect |
|---|---|---|
| `1` set URL | `[1][len][url]` | store the URL (`HID_URL_MAX` limit) |
| `2` enable | `[2][0\|1]` | arm / disarm the launcher |
| `3` status | `[3]` | reply `[status][enabled][url_len][state][trig]`, 5 bytes |

This is the only auto-open path that installs nothing on the target.

---

## 5. Troubleshooting

| Symptom | Likely cause | Check |
|---|---|---|
| `169.254.x.x` on the target | DHCP never answered — discovery relay ran first | firmware must be ≥ `5b3aff4`; otherwise `tcpdump udp port 67` shows DISCOVER with no OFFER |
| Wi-Fi traffic dies when the dongle is plugged in | DHCP advertised a gateway | `ipconfig /all` must show an empty default gateway |
| Target has an address but no name resolves | no resolver on `192.168.20.1:53`, or firewalld blocks it | `dig @192.168.20.1 <name>` from the target side; `ss -lunp \| grep :53` |
| Portal page opens but 8080 is blank | `keymod-files` not running (host rebooted) | `systemctl status keymod-files`, `curl -I http://192.168.20.1:8080/` |
| Edge/MSN popup storm | NCSI probes redirected instead of answered | probe paths return 200 bodies; firmware greeting latched once |
| *Network* view spins forever | wsdd bound to a stale interface name | `wsdd-ncm-watch` active; `tcpdump udp port 3702` |
| *Network* view is empty, UNC works | no gateway ⇒ "unidentified network" ⇒ Public profile | expected; use `\\KEYMOD-HOST` |
| Share prompts for credentials | guest SMB blocked | `AllowInsecureGuestAuth = 1` |
| Flash fails mid-write, `error -71` | hub dropped the device | root port; xHCI unbind/bind |

---

## 6. Related documents

* [ncm-bridge.md](ncm-bridge.md) — DHCP, DNS greeting, diagnostics, config.
* `tools/host_file_server.py` — the file manager / WebDAV server itself.
* `tools/setup_target_share.sh` — Samba share and `KEYMOD-HOST` naming.
* `tools/enable_windows_discovery.ps1` — legacy helper that set the Private
  profile, discovery services and `AllowInsecureGuestAuth` in one go (kept for
  reference; not part of the zero-install path).
