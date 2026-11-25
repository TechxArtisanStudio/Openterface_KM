#!/usr/bin/env python3
"""Host file server for the KeyMod target PC.

Serves a dedicated directory over the USB NCM bridge so the target PC can
browse/download files in a browser and map the share as a WebDAV drive.
No firmware involvement: the device only forwards Ethernet frames.

Features:
  - HTTP GET: directory listing + file download
  - HTTP POST/PUT: upload (multipart form and raw body)
  - WebDAV: OPTIONS, PROPFIND, MKCOL, PUT, GET, DELETE, MOVE, COPY,
    LOCK/UNLOCK (null-lock stub) so Windows can map it as a network drive.

Safe by construction: every path is resolved and confined to --root.

Usage:
    python3 host_file_server.py --root ~/SharedWithTarget \
        --bind 192.168.20.1 --port 8080
"""
import argparse
import datetime
import email.utils
import html
import logging
import mimetypes
import os
import shutil
import urllib.parse
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(message)s")
logger = logging.getLogger("host-file-server")

DAV_NS = "DAV:"
ET.register_namespace("D", DAV_NS)


def dav_tag(name: str) -> str:
    return f"{{{DAV_NS}}}{name}"


class FileServerHandler(BaseHTTPRequestHandler):
    root: Path = Path(".")
    allow_write: bool = True
    server_version = "KeyModFileServer/1.0"

    # ---------- helpers ----------
    def _path(self) -> Path | None:
        """Resolve the request path to a file under root, or None on escape."""
        raw = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
        rel = raw.lstrip("/")
        target = (self.root / rel).resolve()
        try:
            target.relative_to(self.root.resolve())
        except ValueError:
            return None
        return target

    def _send(self, code: int, body: bytes = b"", extra: dict | None = None):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def _send_file(self, path: Path):
        ctype = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        size = path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if self.command == "HEAD":
            return
        with open(path, "rb") as f:
            while chunk := f.read(64 * 1024):
                self.wfile.write(chunk)

    def _listing(self, path: Path, href_prefix: str) -> bytes:
        rows = []
        if path != self.root.resolve():
            rows.append(('<a href="../">../</a>', "-", "-"))
        for entry in sorted(path.iterdir(), key=lambda p: (p.is_file(), p.name.lower())):
            name = html.escape(entry.name)
            qname = urllib.parse.quote(entry.name)
            rows.append((f'<a href="{qname}{"/" if entry.is_dir() else ""}">{name}{"/" if entry.is_dir() else ""}</a>',
                         "-" if entry.is_dir() else str(entry.stat().st_size),
                         datetime.datetime.fromtimestamp(entry.stat().st_mtime)
                         .strftime("%Y-%m-%d %H:%M")))
        body = ["<!doctype html><html><head><meta charset=utf-8><title>Shared files</title>",
                "<style>body{font:15px system-ui;margin:2rem}td{padding:2px 12px}</style></head><body>",
                f"<h2>Shared files: {html.escape(href_prefix)}</h2>",
                "<table>", "<tr><th>Name</th><th>Size</th><th>Modified</th></tr>"]
        for link, size, mtime in rows:
            body.append(f"<tr><td>{link}</td><td>{size}</td><td>{mtime}</td></tr>")
        body.append("</table><hr><p>Upload: <form method=POST enctype=multipart/form-data>"
                    "<input type=file name=file><button>Upload</button></form></p></body></html>")
        return "\n".join(body).encode()

    # ---------- WebDAV PROPFIND ----------
    def _propfind_body(self, path: Path, href: str, depth: str) -> bytes:
        root = ET.Element(dav_tag("multistatus"))

        def add(p: Path, h: str):
            resp = ET.SubElement(root, dav_tag("response"))
            ET.SubElement(resp, dav_tag("href")).text = h
            propstat = ET.SubElement(resp, dav_tag("propstat"))
            prop = ET.SubElement(propstat, dav_tag("prop"))
            ET.SubElement(prop, dav_tag("displayname")).text = p.name or "/"
            ET.SubElement(prop, dav_tag("getcontentlength")).text = (
                "0" if p.is_dir() else str(p.stat().st_size))
            ET.SubElement(prop, dav_tag("getlastmodified")).text = (
                email.utils.formatdate(p.stat().st_mtime, usegmt=True))
            ET.SubElement(prop, dav_tag("resourcetype")).append(
                ET.Element(dav_tag("collection")) if p.is_dir() else ET.Element(dav_tag("")))
            ET.SubElement(propstat, dav_tag("status")).text = "HTTP/1.1 200 OK"

        add(path, href)
        if depth == "1" and path.is_dir():
            for child in sorted(path.iterdir(), key=lambda x: x.name.lower()):
                child_href = href.rstrip("/") + "/" + urllib.parse.quote(child.name)
                add(child, child_href + ("/" if child.is_dir() else ""))
        return ET.tostring(root, encoding="utf-8", xml_declaration=True)

    # ---------- WebDAV LOCK/UNLOCK ----------
    def _lock_body(self, token: str) -> bytes:
        root = ET.Element(dav_tag("prop"))
        lock = ET.SubElement(root, dav_tag("lockdiscovery"))
        active = ET.SubElement(lock, dav_tag("activelock"))
        ET.SubElement(active, dav_tag("locktype")).append(ET.Element(dav_tag("write")))
        ET.SubElement(active, dav_tag("lockscope")).append(ET.Element(dav_tag("exclusive")))
        ET.SubElement(active, dav_tag("depth")).text = "0"
        ET.SubElement(active, dav_tag("owner")).text = "KeyMod"
        ET.SubElement(active, dav_tag("locktoken")).append(
            ET.Element(dav_tag("href"))).text = token
        ET.SubElement(active, dav_tag("timeout")).text = "Second-3600"
        return ET.tostring(root, encoding="utf-8", xml_declaration=True)

    # ---------- handlers ----------
    def do_OPTIONS(self):  # noqa: N802
        self._send(200, extra={
            "DAV": "1,2", "MS-Author-Via": "DAV", "Allow":
            "OPTIONS, GET, HEAD, POST, PUT, DELETE, PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK",
        })

    def do_PROPFIND(self):  # noqa: N802
        path = self._path()
        if path is None:
            self._send(403)
            return
        if not path.exists():
            self._send(404)
            return
        depth = self.headers.get("Depth", "1")
        href = urllib.parse.quote(urllib.parse.urlsplit(self.path).path)
        body = self._propfind_body(path, href, depth)
        self._send(207, body, {"Content-Type": "application/xml; charset=utf-8"})

    def do_GET(self):  # noqa: N802
        path = self._path()
        if path is None:
            self._send(403)
            return
        if path.is_dir():
            self._send(200, self._listing(path, urllib.parse.unquote(
                urllib.parse.urlsplit(self.path).path)),
                {"Content-Type": "text/html; charset=utf-8"})
        elif path.is_file():
            self._send_file(path)
        else:
            self._send(404)

    def do_HEAD(self):  # noqa: N802
        self.do_GET()

    def _read_body(self) -> bytes:
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n) if n else b""

    def do_PUT(self):  # noqa: N802
        if not self.allow_write:
            self._send(403)
            return
        path = self._path()
        if path is None or path.is_dir():
            self._send(403)
            return
        data = self._read_body()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        logger.info("uploaded %s (%d bytes)", path.relative_to(self.root), len(data))
        self._send(201, extra={"Location": self.path})

    def do_POST(self):  # noqa: N802
        if not self.allow_write:
            self._send(403)
            return
        path = self._path()
        if path is None:
            self._send(403)
            return
        body = self._read_body()
        # Minimal multipart/form-data parser for the browser upload form.
        ctype = self.headers.get("Content-Type", "")
        if "multipart/form-data" in ctype:
            boundary = ctype.split("boundary=", 1)[-1].strip().strip('"')
            for part in body.split(f"--{boundary}".encode()):
                if b"name=\"file\"" in part and b"\r\n\r\n" in part:
                    head, data = part.split(b"\r\n\r\n", 1)
                    fname = ""
                    for line in head.decode("utf-8", "replace").splitlines():
                        if "filename=" in line:
                            fname = line.split("filename=", 1)[1].strip().strip('"')
                    if fname:
                        dest = path / os.path.basename(fname)
                        dest.write_bytes(data.rstrip(b"\r\n"))
                        logger.info("uploaded %s (%d bytes)", dest.relative_to(self.root),
                                    dest.stat().st_size)
            self._send(200, b"uploaded", {"Content-Type": "text/plain"})
        else:
            self.do_PUT()

    def do_MKCOL(self):  # noqa: N802
        if not self.allow_write:
            self._send(403)
            return
        path = self._path()
        if path is None or path.exists():
            self._send(405 if path and path.exists() else 403)
            return
        path.mkdir(parents=True)
        self._send(201)

    def do_DELETE(self):  # noqa: N802
        if not self.allow_write:
            self._send(403)
            return
        path = self._path()
        if path is None or not path.exists():
            self._send(404)
            return
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink()
        logger.info("deleted %s", path.relative_to(self.root))
        self._send(204)

    def do_MOVE(self):  # noqa: N802
        self._copy_or_move(copy=False)

    def do_COPY(self):  # noqa: N802
        self._copy_or_move(copy=True)

    def _copy_or_move(self, copy: bool):
        if not self.allow_write:
            self._send(403)
            return
        src = self._path()
        dest_url = self.headers.get("Destination", "")
        dest_raw = urllib.parse.unquote(urllib.parse.urlsplit(dest_url).path)
        dest = (self.root / dest_raw.lstrip("/")).resolve()
        try:
            dest.relative_to(self.root.resolve())
        except ValueError:
            self._send(403)
            return
        if src is None or not src.exists():
            self._send(404)
            return
        if copy:
            if src.is_dir():
                shutil.copytree(src, dest, dirs_exist_ok=True)
            else:
                shutil.copy2(src, dest)
        else:
            shutil.move(str(src), str(dest))
        self._send(201 if copy else 204)

    def do_LOCK(self):  # noqa: N802
        token = f"opaquelocktoken:keymod-{os.getpid():x}"
        self._send(200, self._lock_body(token), {
            "Content-Type": "application/xml; charset=utf-8", "Lock-Token": f"<{token}>",
        })

    def do_UNLOCK(self):  # noqa: N802
        self._send(204)

    def log_message(self, fmt, *args):
        logger.info("%s - %s", self.address_string(), fmt % args)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", default=str(Path.home() / "SharedWithTarget"))
    p.add_argument("--bind", default="192.168.20.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--read-only", action="store_true")
    args = p.parse_args()

    root = Path(args.root).expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    FileServerHandler.root = root
    FileServerHandler.allow_write = not args.read_only
    server = ThreadingHTTPServer((args.bind, args.port), FileServerHandler)
    logger.info("serving %s at http://%s:%d/ (write=%s)",
                root, args.bind, args.port, not args.read_only)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()


if __name__ == "__main__":
    main()
