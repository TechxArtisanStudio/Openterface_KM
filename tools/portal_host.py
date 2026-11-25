#!/usr/bin/env python3
"""KeyMod HID portal launcher + Host web service.

The portal page is served by THIS process (on the Host); no HTML is stored
in firmware. The device's L2 bridge lets the target PC reach the Host
directly at 192.168.20.1:<port>, so the page loads over plain HTTP.

Device-side the HID launcher types Win+R -> URL -> Enter into the target's
keyboard, which pops the browser on the portal page.

Usage:
    # serve page + arm the launcher (one-shot: page stays available)
    python3 portal_host.py --ble-addr <UUID> --html page.html --launch

    # only serve the page, do not inject keys
    python3 portal_host.py --ble-addr <UUID> --html page.html --no-launch

    # disarm a previous launcher
    python3 portal_host.py --ble-addr <UUID> --disarm
"""
import argparse
import asyncio
import http.server
import logging
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ble_eth_tunnel import BleEthClient, build_frame  # noqa: E402

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(message)s")
logger = logging.getLogger("portal-host")

CMD_PORTAL = 0x25
CMD_PORTAL_RESP = 0xA5

DEFAULT_HTML = """<!doctype html><html><head><meta charset="utf-8">
<title>KeyMod portal</title><style>body{font:16px system-ui;margin:3rem;
max-width:40rem;line-height:1.6}</style></head><body>
<h1>KeyMod is connected</h1>
<p>This page is served by the Host over the USB network bridge.</p>
</body></html>"""


class PortalHandler(http.server.BaseHTTPRequestHandler):
    html: bytes = DEFAULT_HTML.encode()

    def do_GET(self):  # noqa: N802
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(self.html)))
        self.end_headers()
        self.wfile.write(self.html)

    def log_message(self, fmt, *args):
        logger.info("http: " + fmt, *args)


async def _portal_cmd(client: BleEthClient, payload: bytes, timeout: float = 5.0) -> bytes:
    fut = asyncio.get_running_loop().create_future()
    client._portal_future = fut
    await client._send_frame(build_frame(0, CMD_PORTAL, payload))
    try:
        return await asyncio.wait_for(fut, timeout=timeout)
    except asyncio.TimeoutError:
        client._portal_future = None
        raise TimeoutError("PORTAL command timed out")


async def run(args):
    html = Path(args.html).read_bytes() if args.html else DEFAULT_HTML.encode()

    # Host web service — bound to the NCM interface IP that the target can reach.
    server = http.server.ThreadingHTTPServer((args.bind, args.port), PortalHandler)
    PortalHandler.html = html
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = args.url or f"http://{args.bind}:{args.port}/"
    logger.info("portal page at %s (%d bytes, HTML stays on Host)", url, len(html))

    client = BleEthClient(ble_address=args.ble_addr)
    await client.connect()
    try:
        if args.disarm:
            await _portal_cmd(client, bytes([2, 0]))
            logger.info("HID launcher disarmed")
            return

        if not args.no_launch:
            if len(url.encode()) > 96:
                raise SystemExit("URL too long (max 96 bytes)")
            st = await _portal_cmd(client, bytes([1, len(url)]) + url.encode())
            if st[0]:
                raise SystemExit(f"SET_URL failed: 0x{st[0]:02X}")
            st = await _portal_cmd(client, bytes([2, 1]))
            if st[0]:
                raise SystemExit(f"LAUNCH failed: 0x{st[0]:02X}")
            logger.info("HID launcher armed: Win+R -> %s -> Enter", url)
        else:
            logger.info("--no-launch: page served, no keys injected")
    finally:
        await client.disconnect()
        if args.disarm:
            server.shutdown()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ble-addr", required=True)
    p.add_argument("--html", help="path to portal HTML page")
    p.add_argument("--url", help="URL to type (default: http://<bind>:<port>/)")
    p.add_argument("--bind", default="192.168.20.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--no-launch", action="store_true")
    p.add_argument("--disarm", action="store_true")
    asyncio.run(run(p.parse_args()))


if __name__ == "__main__":
    main()
