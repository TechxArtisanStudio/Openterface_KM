#!/usr/bin/env python3
"""Host file server for the KeyMod target PC.

Serves a dedicated directory over the USB NCM bridge so the target PC can
browse/download files in a browser and map the share as a WebDAV drive.
No firmware involvement: the device only forwards Ethernet frames.

Features:
  - HTTP GET: file-manager web UI (drag-and-drop upload with progress,
    download with Content-Disposition attachment, mkdir, delete)
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
import json
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
        if "dl=1" in urllib.parse.urlsplit(self.path).query:
            quoted = urllib.parse.quote(path.name)
            self.send_header("Content-Disposition",
                             f"attachment; filename*=UTF-8''{quoted}")
        self.end_headers()
        if self.command == "HEAD":
            return
        with open(path, "rb") as f:
            while chunk := f.read(64 * 1024):
                self.wfile.write(chunk)

    @staticmethod
    def _human_size(n: int) -> str:
        for unit in ("B", "KB", "MB", "GB", "TB"):
            if n < 1024 or unit == "TB":
                return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
            n /= 1024
        return f"{n:.1f} TB"

    # ---------- web UI ----------
    PAGE_TMPL = r"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>KeyMod 文件管理器</title>
<style>
  :root { --bd:#e3e6ea; --hd:#f5f7fa; --tx:#1f2937; --mut:#6b7280; --ac:#2563eb; }
  * { box-sizing:border-box }
  body { margin:0; font:14px/1.5 system-ui,"Segoe UI","Microsoft YaHei",sans-serif;
         color:var(--tx); background:#fafbfc; min-height:100vh }
  header { display:flex; align-items:center; gap:10px; padding:10px 20px;
           background:#fff; border-bottom:1px solid var(--bd); position:sticky; top:0; z-index:5 }
  header h1 { font-size:16px; margin:0; font-weight:600 }
  header .crumb { margin-left:6px; color:var(--mut); flex:1; overflow:hidden;
                  text-overflow:ellipsis; white-space:nowrap }
  header .crumb a { color:var(--ac); text-decoration:none }
  .seg { display:flex; border:1px solid var(--bd); border-radius:6px; overflow:hidden }
  .seg button { border:none; border-radius:0; padding:6px 12px; background:#fff; font-size:13px }
  .seg button.on { background:var(--ac); color:#fff }
  button { font:inherit; padding:6px 14px; border:1px solid var(--bd); border-radius:6px;
           background:#fff; cursor:pointer; color:var(--tx) }
  button:hover { background:var(--hd); border-color:#c9cfd6 }
  button.danger { color:#b91c1c; border-color:#f3c4c4 }
  button.danger:hover { background:#fef2f2 }
  main { max-width:960px; margin:18px auto; padding:0 16px }
  .tip { color:var(--mut); font-size:12.5px; margin:0 0 10px }
  table { width:100%; border-collapse:collapse; background:#fff;
          border:1px solid var(--bd); border-radius:8px; overflow:hidden }
  th, td { padding:8px 12px; text-align:left; border-bottom:1px solid var(--bd) }
  th { background:var(--hd); font-weight:600; font-size:13px; color:var(--mut) }
  tr:last-child td { border-bottom:none }
  tr:hover td { background:#f8fafc }
  td.name a, td.nm a { color:var(--tx); text-decoration:none }
  td.name a:hover, td.nm a:hover { color:var(--ac); text-decoration:underline }
  td.size, td.mtime { color:var(--mut); white-space:nowrap; font-size:13px }
  td.acts { white-space:nowrap; text-align:right }
  td.acts a { margin-right:10px; color:var(--ac); text-decoration:none; font-size:13px }
  td.acts a:hover { text-decoration:underline }
  td.acts button { padding:3px 10px; font-size:12.5px }
  #iconview { display:none; grid-template-columns:repeat(auto-fill,minmax(108px,1fr));
              gap:6px; background:#fff; border:1px solid var(--bd); border-radius:8px; padding:14px }
  .tile { position:relative; text-align:center; padding:12px 4px 8px; border-radius:8px; cursor:pointer }
  .tile:hover { background:var(--hd) }
  .tile .ic { font-size:40px; line-height:1 }
  a.ic { display:inline-block; color:inherit; text-decoration:none; cursor:pointer }
  a.ic:hover { text-decoration:none }
  .tile .nm { font-size:12.5px; margin-top:6px; word-break:break-all; color:var(--tx) }
  .tile .nm a { color:inherit; text-decoration:none }
  .tile .mini { position:absolute; top:2px; right:2px; display:none; gap:2px }
  .tile:hover .mini { display:flex }
  .mini a, .mini button { font-size:11.5px; padding:1px 5px; background:#fff;
                          border:1px solid var(--bd); border-radius:4px; text-decoration:none; color:var(--ac) }
  .mini button { color:#b91c1c }
  #listview { display:none; background:#fff; border:1px solid var(--bd); border-radius:8px; overflow:hidden }
  .lrow { display:flex; align-items:center; gap:10px; padding:7px 14px;
          border-bottom:1px solid var(--bd); font-size:13.5px }
  .lrow:last-child { border-bottom:none }
  .lrow:hover { background:#f8fafc }
  .lrow .nm { flex:1; overflow:hidden; text-overflow:ellipsis; white-space:nowrap }
  .lrow .nm a { color:var(--tx); text-decoration:none }
  .lrow .nm a:hover { color:var(--ac) }
  .lrow .sz { color:var(--mut); font-size:12.5px; white-space:nowrap }
  .lrow .del { visibility:hidden }
  .lrow:hover .del { visibility:visible }
  #progress { margin-top:12px }
  .prow { display:flex; align-items:center; gap:10px; background:#fff; border:1px solid var(--bd);
          border-radius:6px; padding:6px 10px; margin-top:6px; font-size:13px }
  .prow .pn { flex:1; overflow:hidden; text-overflow:ellipsis; white-space:nowrap }
  .prow .bar { width:180px; height:8px; background:var(--bd); border-radius:4px; overflow:hidden }
  .prow .bar i { display:block; height:100%; width:0; background:var(--ac) }
  .prow .st { width:60px; text-align:right; color:var(--mut) }
  .prow.ok .st { color:#15803d } .prow.err .st { color:#b91c1c }
  #overlay { display:none; position:fixed; inset:0; background:rgba(37,99,235,.08);
             z-index:50; align-items:center; justify-content:center }
  #overlay .box { border:2px dashed var(--ac); background:#fff; color:var(--ac);
                  padding:36px 60px; border-radius:12px; font-size:18px; pointer-events:none }
</style></head>
<body>
<header>
  <h1>&#128193; KeyMod 文件管理器</h1>
  <span class="crumb">__CRUMB__</span>
  <div class="seg">
    <button id="v-icon" onclick="setView('icon' )" title="图标视图">&#128444;&#65039; 图标</button>
    <button id="v-list" onclick="setView('list' )" title="列表视图">&#9776; 列表</button>
    <button id="v-detail" onclick="setView('detail')" title="详情视图">&#9883; 详情</button>
  </div>
  <button onclick="mkdirPrompt()">新建文件夹</button>
  <button onclick="document.getElementById('f').click()">上传文件</button>
  <input id="f" type="file" multiple hidden>
</header>
<main>
  <p class="tip">将文件拖入页面任意位置即可上传；点击图标或文件名即可进入文件夹 / 下载文件。</p>
  <div id="iconview"></div>
  <div id="listview"></div>
  <table id="detailview">
    <thead><tr><th>名称</th><th>大小</th><th>修改时间</th><th></th></tr></thead>
    <tbody id="detailbody"></tbody>
  </table>
  <div id="progress"></div>
</main>
<div id="overlay"><div class="box">&#11014;&#65039; 松开鼠标上传到当前目录</div></div>
<script id="filedata" type="application/json">__FILEDATA__</script>
<script>
const CUR = "__CURDIR__";
let FILES = JSON.parse(document.getElementById('filedata').textContent);
FILES.unshift({n:"..", d:true, s:0, m:0, up:true});

const ICONS = { dir:"\u{1F4C1}", img:"\u{1F5BC}\uFE0F", audio:"\u{1F3B5}", video:"\u{1F3AC}",
                pdf:"\u{1F4D5}", zip:"\u{1F4E6}", code:"\u{1F4DC}", text:"\u{1F4C4}", file:"\u{1F4C4}" };
function iconOf(f) {
  if (f.d) return ICONS.dir;
  const e = f.n.split('.').pop().toLowerCase();
  if (['png','jpg','jpeg','gif','bmp','webp','svg','ico'].includes(e)) return ICONS.img;
  if (['mp3','wav','flac','ogg','m4a'].includes(e)) return ICONS.audio;
  if (['mp4','mkv','avi','mov','webm'].includes(e)) return ICONS.video;
  if (e === 'pdf') return ICONS.pdf;
  if (['zip','rar','7z','tar','gz','bz2','xz'].includes(e)) return ICONS.zip;
  if (['c','h','py','js','ts','json','xml','html','css','sh','md'].includes(e)) return ICONS.code;
  return ICONS.file;
}
function humanSize(n) {
  if (!n) return '-';
  const u = ['B','KB','MB','GB','TB']; let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return i ? n.toFixed(1) + ' ' + u[i] : n + ' ' + u[i];
}
function fmtTime(ts) {
  if (!ts) return '';
  const d = new Date(ts * 1000), p = x => String(x).padStart(2, '0');
  return d.getFullYear() + '-' + p(d.getMonth()+1) + '-' + p(d.getDate()) + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
}
function esc(s) { const d = document.createElement('div'); d.textContent = s; return d.innerHTML; }
function hrefOf(f) { return f.up ? '../' : CUR + encodeURIComponent(f.n) + (f.d ? '/' : '?dl=1'); }
function pathOf(f) { return CUR + encodeURIComponent(f.n); }

function renderViews() {
  const iv = document.getElementById('iconview');
  const lv = document.getElementById('listview');
  const tb = document.getElementById('detailbody');
  iv.innerHTML = ''; lv.innerHTML = ''; tb.innerHTML = '';
  FILES.forEach(f => {
    const ic = iconOf(f), name = esc(f.n), href = hrefOf(f);
    // icon view
    const tile = document.createElement('div');
    tile.className = 'tile';
    tile.innerHTML = '<a class="ic" href="' + href + '">' + (f.up ? '&#128193;' : ic) + '</a>' +
      '<div class="nm"><a href="' + href + '">' + name + '</a></div>';
    // Anywhere on the tile opens the entry; the download / delete buttons keep their own click.
    tile.onclick = e => { if (!e.target.closest('a,button')) location.href = href; };
    if (!f.up) {
      tile.innerHTML += '<div class="mini"><a href="' + href + '" download>&#11015;</a>' +
        '<button onclick="delP(\'' + encodeURIComponent(f.n) + '\')">&#10005;</button></div>';
    }
    iv.appendChild(tile);
    // list view
    const row = document.createElement('div');
    row.className = 'lrow';
    row.innerHTML = '<a class="ic" href="' + href + '">' + (f.up ? '&#128193;' : ic) + '</a>' +
      '<span class="nm"><a href="' + href + '">' + name + '</a></span>' +
      '<span class="sz">' + (f.d ? '' : humanSize(f.s)) + '</span>';
    row.onclick = e => { if (!e.target.closest('a,button')) location.href = href; };
    if (!f.up) {
      row.innerHTML += '<a class="del" href="' + href + '" download>下载</a>' +
        '<button class="del danger" onclick="delP(\'' + encodeURIComponent(f.n) + '\')">删除</button>';
    }
    lv.appendChild(row);
    // detail view
    const tr = document.createElement('tr');
    if (f.up) {
      tr.innerHTML = '<td class="name"><a href="../">&#128193; ../</a></td>' +
        '<td class="size"></td><td class="mtime"></td><td class="acts"></td>';
    } else {
      tr.innerHTML = '<td class="name"><a href="' + href + '">' + ic + ' ' + name + '</a></td>' +
        '<td class="size">' + (f.d ? '-' : humanSize(f.s)) + '</td>' +
        '<td class="mtime">' + fmtTime(f.m) + '</td>' +
        '<td class="acts"><a href="' + href + '" download>下载</a>' +
        '<button class="danger" onclick="delP(\'' + encodeURIComponent(f.n) + '\')">删除</button></td>';
    }
    tb.appendChild(tr);
  });
}
function setView(v) {
  localStorage.setItem('km_view', v);
  document.getElementById('iconview').style.display  = v === 'icon'  ? 'grid' : 'none';
  document.getElementById('listview').style.display  = v === 'list'  ? 'block' : 'none';
  document.getElementById('detailview').style.display = v === 'detail' ? 'table' : 'none';
  ['icon','list','detail'].forEach(x =>
    document.getElementById('v-' + x).classList.toggle('on', x === v));
}
renderViews();
setView(localStorage.getItem('km_view') || 'detail');

// drag & drop upload
const overlay = document.getElementById('overlay');
let dragDepth = 0;
document.addEventListener('dragenter', e => { e.preventDefault(); dragDepth++; overlay.style.display = 'flex'; });
document.addEventListener('dragover', e => e.preventDefault());
document.addEventListener('dragleave', e => { if (--dragDepth <= 0) { dragDepth = 0; overlay.style.display = 'none'; } });
document.addEventListener('drop', e => {
  e.preventDefault(); dragDepth = 0; overlay.style.display = 'none';
  uploadFiles([...e.dataTransfer.files]);
});
document.getElementById('f').addEventListener('change', e => {
  uploadFiles([...e.target.files]); e.target.value = '';
});
function uploadFiles(files) {
  if (!files.length) return;
  const box = document.getElementById('progress');
  let pending = files.length;
  files.forEach(file => {
    const row = document.createElement('div');
    row.className = 'prow';
    row.innerHTML = '<span class="pn"></span><span class="bar"><i></i></span><span class="st">0%</span>';
    row.querySelector('.pn').textContent = file.name;
    box.appendChild(row);
    const xhr = new XMLHttpRequest();
    xhr.open('PUT', CUR + encodeURIComponent(file.name));
    xhr.upload.addEventListener('progress', e => {
      if (e.lengthComputable) {
        const pct = Math.round(e.loaded / e.total * 100);
        row.querySelector('.bar i').style.width = pct + '%';
        row.querySelector('.st').textContent = pct + '%';
      }
    });
    xhr.addEventListener('load', () => {
      const ok = xhr.status >= 200 && xhr.status < 300;
      row.classList.add(ok ? 'ok' : 'err');
      row.querySelector('.st').textContent = ok ? '完成' : ('失败 ' + xhr.status);
      if (ok && --pending === 0) setTimeout(() => location.reload(), 600);
    });
    xhr.addEventListener('error', () => { row.classList.add('err'); row.querySelector('.st').textContent = '网络错误'; if (--pending === 0) setTimeout(() => location.reload(), 600); });
    xhr.send(file);
  });
}
function mkdirPrompt() {
  const name = prompt('新文件夹名称：');
  if (!name) return;
  fetch(CUR + encodeURIComponent(name), { method: 'MKCOL' })
    .then(r => { if (!r.ok) alert('创建失败 (HTTP ' + r.status + ')'); location.reload(); });
}
function delP(qname) {
  if (!confirm('确定删除 ' + decodeURIComponent(qname) + ' ？')) return;
  fetch(CUR + qname, { method: 'DELETE' })
    .then(r => { if (!r.ok) alert('删除失败 (HTTP ' + r.status + ')'); location.reload(); });
}
</script>
</body></html>
"""

    def _listing(self, path: Path, href_prefix: str) -> bytes:
        root = self.root.resolve()
        rel_parts: list[str] = []
        if path != root:
            try:
                rel_parts = path.resolve().relative_to(root).parts
            except ValueError:
                rel_parts = []

        # breadcrumb
        crumbs = ['<a href="/">根目录</a>']
        acc = ""
        for part in rel_parts:
            acc += "/" + urllib.parse.quote(part)
            crumbs.append(f'<a href="{acc}/">{html.escape(part)}</a>')
        crumb = " / ".join(crumbs)

        files = [{"n": e.name, "d": e.is_dir(), "s": 0 if e.is_dir() else e.stat().st_size,
                  "m": int(e.stat().st_mtime)}
                 for e in sorted(path.iterdir(), key=lambda p: (p.is_file(), p.name.lower()))]
        filedata = json.dumps(files, ensure_ascii=False).replace("</", "<\\/")

        # Absolute path prefix so browser-relative links never double up
        cur_dir = "/" + "/".join(urllib.parse.quote(p) for p in rel_parts)
        cur_dir = (cur_dir + "/") if len(cur_dir) > 1 else "/"

        page = (self.PAGE_TMPL
                .replace("__CRUMB__", crumb)
                .replace("__FILEDATA__", filedata)
                .replace("__CURDIR__", cur_dir))
        return page.encode()
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
        locktoken = ET.SubElement(active, dav_tag("locktoken"))
        ET.SubElement(locktoken, dav_tag("href")).text = f"<{token}>"
        ET.SubElement(active, dav_tag("timeout")).text = "Second-3600"
        return ET.tostring(root, encoding="utf-8", xml_declaration=True)

    # ---------- handlers ----------
    def do_OPTIONS(self):  # noqa: N802
        self._send(200, extra={
            "DAV": "1,2", "MS-Author-Via": "DAV", "Allow":
            "OPTIONS, GET, HEAD, POST, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, MOVE, COPY, LOCK, UNLOCK",
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
                # The page is generated per directory and its own script changes
                # with it; caching only ever serves a stale UI.
                {"Content-Type": "text/html; charset=utf-8",
                 "Cache-Control": "no-store"})
        elif path.is_file():
            self._send_file(path)
        else:
            self._send(404)

    def do_HEAD(self):  # noqa: N802
        self.do_GET()

    def _read_body(self) -> bytes:
        logger.info("BODY TE=%r CL=%r Expect=%r",
                    self.headers.get("Transfer-Encoding"),
                    self.headers.get("Content-Length"), self.headers.get("Expect"))
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            chunks = []
            while True:
                line = self.rfile.readline().strip()
                if not line:
                    continue
                size = int(line.split(b";", 1)[0], 16)
                if size == 0:
                    self.rfile.readline()
                    break
                chunks.append(self.rfile.read(size))
                self.rfile.read(2)
            return b"".join(chunks)
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

    def do_PROPPATCH(self):  # noqa: N802
        # Windows WebDAV writes the actual file bytes in the PROPPATCH body
        # (after an empty PUT created the file). Capture those bytes.
        self._read_body()
        root = ET.Element(dav_tag("multistatus"))
        resp = ET.SubElement(root, dav_tag("response"))
        ET.SubElement(resp, dav_tag("href")).text = urllib.parse.quote(
            urllib.parse.urlsplit(self.path).path)
        propstat = ET.SubElement(resp, dav_tag("propstat"))
        ET.SubElement(propstat, dav_tag("status")).text = "HTTP/1.1 200 OK"
        self._send(207, ET.tostring(root, encoding="utf-8", xml_declaration=True),
                   {"Content-Type": "application/xml; charset=utf-8"})

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
