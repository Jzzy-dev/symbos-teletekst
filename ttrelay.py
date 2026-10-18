#!/usr/bin/env python3
"""
ttrelay.py - plain-HTTP to HTTPS relay for NOS Teletekst on MSX / SymbOS
             (MSXTTPP for MSX-DOS, TELETEKS for SymbOS)

The MSX (DenYoNet, GR8NET, openMSX UNAPI, ...) has no TLS, but
teletekst-data.nos.nl only serves over HTTPS. Run this on any PC / Raspberry Pi
on your network; the MSX sends plain HTTP here and this relay fetches the
page from NOS over HTTPS and passes it back.

Usage:   python3 ttrelay.py [port]          (default port 8080)
Test:    curl -i http://<this-machine-ip>:8080/json/100

Needs only Python 3.7+, no extra packages.
"""
import sys
import urllib.request
import urllib.error
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

UPSTREAM = "https://teletekst-data.nos.nl"
TIMEOUT = 15


class Relay(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"      # what the MSX speaks; no chunking
    server_version = "ttrelay/1.0"

    def do_GET(self):
        # Ignore the Host header (it may be our own IP) and always forward to
        # NOS; only /json/... is passed through. Accepted request forms:
        #   GET /json/100                                   (MSXTTPP, TELETEKS)
        #   GET http://teletekst-data.nos.nl/json/100       (proxy style)
        #   GET /https://teletekst-data.nos.nl/json/100     (SymbOS HTTP_GET proxy style)
        path = self.path
        i = path.find("/json/")
        if i < 0:
            self._reply(404, b"not found\n", "text/plain")
            return
        path = path[i:]
        req = urllib.request.Request(UPSTREAM + path, headers={
            "User-Agent": "Mozilla/5.0 (MSX Teletekst relay)",
            "Accept": "application/json",
            "Accept-Encoding": "identity",
        })
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
                body = r.read()
                status = r.status
                ctype = r.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as e:
            body, status = e.read(), e.code
            ctype = e.headers.get("Content-Type", "text/plain")
        except Exception as e:
            self._reply(502, ("relay error: %s\n" % e).encode(), "text/plain")
            return
        self._reply(status, body, ctype)

    def _reply(self, status, body, ctype):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print("ttrelay: listening on port %d, forwarding to %s" % (port, UPSTREAM))
    ThreadingHTTPServer(("0.0.0.0", port), Relay).serve_forever()
