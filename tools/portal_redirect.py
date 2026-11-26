#!/usr/bin/env python3
"""Captive-portal trigger for the KeyMod NCM link (port 80).

Runs on the Host next to the file manager (port 8080). The target's OS
connectivity probe (Windows NCSI, macOS captive.apple.com, Android
generate_204) arrives here because dnsmasq points every DNS name at the
Host. This server answers with a 302 redirect to the file manager, which
makes the OS flag the network as a captive portal and show its built-in
sign-in popup — no client-side install needed.

Run as a systemd system service (binds privileged port 80).
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORTAL_URL = "http://192.168.20.1:8080/"
BODY = ("<!doctype html><meta charset=utf-8>"
        "<p>Redirecting to the KeyMod file manager… "
        "<a href='%s'>%s</a></p>" % (PORTAL_URL, PORTAL_URL)).encode()


class PortalHandler(BaseHTTPRequestHandler):
    server_version = "KeyModPortal/1.0"

    def _respond(self):
        self.send_response(302)
        self.send_header("Location", PORTAL_URL)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(BODY)

    do_GET = _respond
    do_HEAD = _respond
    do_POST = _respond

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.address_string(), fmt % args), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--port", type=int, default=80)
    args = p.parse_args()
    server = ThreadingHTTPServer((args.bind, args.port), PortalHandler)
    print("portal redirect on %s:%d -> %s" % (args.bind, args.port, PORTAL_URL),
          flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
