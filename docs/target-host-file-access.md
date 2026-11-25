# Target file access to Host (HTTP + WebDAV)

The portal HTTP service has been replaced by a file server that exposes a
dedicated Host directory to the Windows target over the USB NCM bridge.
No firmware changes are needed — the device only forwards Ethernet frames.

## Start the service

On the Host (Fedora), put files in `~/SharedWithTarget`, then run:

```bash
python3 tools/host_file_server.py \
    --root ~/SharedWithTarget \
    --bind 192.168.20.1 --port 8080
```

By default the share is writable. Add `--read-only` to prevent uploads and
edits. Every requested path is resolved and confined to `--root`, so the
target cannot escape the shared directory.

A persistent systemd-run instance is already active in this session:

```bash
systemctl --user status host-files2.service
```

To make it survive reboots, install `tools/host_file_server.py` as a user
systemd unit with `WorkingDirectory` set to the repo root.

## Access from the Windows target

- **Browser**: open `http://192.168.20.1:8080/` — you get a directory
  listing, can click to download, and can upload via the form on the page.
- **Map as a network drive**: in File Explorer choose "Map network drive"
  and enter `http://192.168.20.1:8080/`, or in the address bar use
  `\\192.168.20.1@8080\DavWWWRoot`.
  Typing only `192.168.20.1@8080` is **not** a valid path; the leading `\\`
  and trailing `\DavWWWRoot` are both required. Run
  `powershell -ExecutionPolicy Bypass -File tools\map_webdav_drive.ps1` on
  the target to set the WebClient HTTP auth level, restart WebClient and map
  `Z:` to that exact UNC path. Since the SMB share is discoverable, prefer
  `\\FEDORA\SharedWithTarget` for normal file access — it is writable and
  does not depend on the WebDAV client.

The server implements the WebDAV methods Windows needs: `OPTIONS`
(advertises `DAV: 1,2` and `MS-Author-Via: DAV`), `PROPFIND`, `MKCOL`,
`PUT`, `GET`, `DELETE`, `MOVE`, `COPY`, and a minimal null-`LOCK`/`UNLOCK`.

## Supported operations

| Operation | Method | Result |
|---|---|---|
| Browse directory | GET | HTML listing with links |
| Download file | GET | file with guessed MIME type |
| Upload file | PUT / POST | 201 / 200 |
| Create folder | MKCOL | 201 |
| Delete file/tree | DELETE | 204 |
| Move / copy | MOVE / COPY | 204 / 201 |
| WebDAV discovery | PROPFIND / OPTIONS | 207 / 200 |

## Discovering the Host in Windows "Network"

Direct `http://192.168.20.1:8080/` works without discovery, but the Host
does not appear in the Windows Network view unless the discovery protocols
can cross the NCM bridge. Three pieces are needed:

1. **Firmware**: `APP/ncm_bridge.c` forwards IPv4 broadcast/multicast
   (NetBIOS, LLMNR, WS-Discovery, SSDP) and IPv6 multicast between the Host
   and Target NCM ports, with correct multicast MAC translation. Earlier
   firmware only forwarded unicast frames addressed to a peer IP, so
   discovery never reached the Host.
2. **Host services**: on Fedora run the idempotent setup script:

   ```bash
   sudo tools/setup_target_share.sh enp0s21f0u1u2
   ```

   It enables a guest-accessible SMB share named `SharedWithTarget` pointing
   at `~/SharedWithTarget`, starts `smb`, `nmb` and `wsdd` on the NCM
   interface, opens the firewall for SMB/WS-Discovery and allows Samba to
   read the Home directory under SELinux.
3. **Windows target**: run the companion script in an elevated PowerShell:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\enable_windows_discovery.ps1
   ```

   It sets the NCM adapter to **Private**, starts `fdPHost` and `FDResPub`,
   and enables the Network Discovery and File/Printer Sharing firewall
   groups. Windows defaults unknown networks to Public, which disables
   discovery even when the Host is answering correctly.

After a Target reboot that still has the NCM adapter connected, the Host
should appear as `FEDORA` in File Explorer -> Network. Open the share with
`\\FEDORA\SharedWithTarget` (guest, no credentials). This SMB path is the
reliable read/write path; the WebDAV mapped drive remains available but
keeps the write limitation documented below.

If the Host is missing: re-run both scripts, verify `wsdd-ncm.service` is
active on Fedora, and check that the Windows NCM adapter is Private. The
guest share is intended for this isolated USB link only.

## Verified on Windows target (2026-11-25)

**Windows Network discovery** (after firmware `d98b484` + `a751e2a`):
- `net view \\FEDORA` lists `SharedWithTarget` as a Disk resource.
- `Get-SmbConnection` shows `FEDORA\SharedWithTarget` at SMB 3.1.1.
- Share listing, file read and a test write from the Windows target into
  `\\FEDORA\SharedWithTarget` all succeeded. Run
  `powershell -ExecutionPolicy Bypass -File tools\verify_windows_discovery.ps1`
  to repeat the check.
- Full BLE-ETH tunnel regression after flashing: **25 PASS / 0 FAIL**.

**Firmware flashing note:** the dev-kit control board listens at **460800**
baud; `tools/ctrl_board.py` was corrected from 115200, which silently
dropped every BOOT/power command.

**Working over the mapped WebDAV drive:**
- `net use Z: http://192.168.20.1:8080/` maps successfully (WebClient
  service must be running: `Start-Service WebClient`).
- Directory listing and file download work: `Get-ChildItem Z:\` shows
  files, `Get-Content Z:\README.txt` returns the exact Host content.
- Folder creation works (`New-Item -ItemType Directory Z:\newdir`).

**Reliable write path:** use the browser at `http://192.168.20.1:8080/`
and the upload form on the page (POST multipart). Verified to transfer
files intact. Raw `PUT` from curl also works (201 with full body).

**Known limitation:** writing *through the mapped WebDAV drive* from
Windows (e.g. `Out-File`/`Copy-Item` to `Z:\`) is not reliable with this
minimal stdlib server. The Windows WebDAV client sends an empty `PUT`
then a `LOCK` and stalls waiting for a write signal the minimal handler
does not provide; files end up 0 bytes. The server implements `LOCK`,
`UNLOCK` and `PROPPATCH`, but full Windows write semantics would require
a mature WebDAV implementation. For editing Host files from the target,
use the browser upload form instead.

Raw HTTP verification on Fedora: browser GET 200, file download, PUT
upload (201), WebDAV `OPTIONS` headers (`DAV: 1,2`, `MS-Author-Via: DAV`),
`PROPFIND` 207 XML, `MKCOL` 201, `DELETE` 204.
