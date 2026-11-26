# Host-served portal page on the target PC

> **Superseded.** See [portal-and-file-access.md](portal-and-file-access.md) for the current portal and file-access setup.

## Architecture

The portal page lives **only on the Host** — no HTML is stored in firmware.
The target PC reaches it over plain HTTP through the KeyMod USB NCM bridge:

```
Windows target (192.168.20.2)
  |  USB CDC-NCM (target-facing port)
  v
KeyMod CH32V208 — NCM L2 bridge (transparent frame forwarding + proxy ARP)
  |  USB CDC-NCM (host-facing port)
  v
Fedora Host (192.168.20.1) — portal HTTP service
```

For the "auto-popup" path, the device also types a URL into the target's
boot-keyboard interface (Win+R -> URL -> Enter) over BLE command `0x25`.

## Firmware pieces

- `APP/ncm_bridge.c`: proxy ARP for the peer IP and bidirectional frame
  forwarding between the two NCM ports. Counters (`g_bridge_arp_proxy`,
  `g_bridge_host_to_target`, `g_bridge_target_to_host`) are exported at the
  tail of the NCM diagnostics block (0x33/0x10).
- `APP/hid_launcher.c`: `CMD_PORTAL` (0x25) subcommands:
  - `1 [len][url]` — set launcher URL (max 96 bytes)
  - `2 [0|1]` — disarm / arm the typing sequence
  - `3` — query status `[enabled, url_len, state, triggers]`
  Only the 96-byte URL buffer is in firmware; no page content.

## Host usage

Serve a page and arm the launcher (one-shot):

```bash
python3 tools/portal_host.py --ble-addr <UUID> --html portal.html --launch
# default URL: http://192.168.20.1:8080/
```

Serve the page without injecting keys (captive-probe fallback only):

```bash
python3 tools/portal_host.py --ble-addr <UUID> --html portal.html --no-launch
```

Disarm a previous launcher:

```bash
python3 tools/portal_host.py --ble-addr <UUID> --disarm
```

## Deployment notes

- Bind the portal HTTP service to `192.168.20.1` (the host-facing NCM
  address), on a port the target can reach. Port 8080 avoids conflicts
  with nginx on port 80.
- The browser may attempt HTTPS upgrades on some URLs (HSTS/HTTPS-First);
  use the full `http://host:port/` URL and, if needed, a local mDNS name
  that is not on the browser's HSTS list.
- Verified on 2026-11-25: 25/25 tunnel regression, Host pings target 3/3,
  target-originated traffic visible in the Host web-server access log.
