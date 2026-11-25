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

## Verified

On 2026-11-25 against Fedora `192.168.20.1:8080`: browser GET 200,
file download, PUT upload (201), WebDAV `OPTIONS` headers, `PROPFIND`
207 XML, `MKCOL` 201, `DELETE` 204. The HID launcher was also used to
open the page on the target automatically.
