#!/usr/bin/env python3
"""
HTTP server with CGI support that disables browser caching.

Used by 'make htags-serve' so that htags-regenerated HTML is always
fetched fresh from disk, without stale-cache issues.  CGI support is
required for htags dynamic features: reverse references and symbol search
(both served via cgi-bin/global.cgi).

Usage:
    python3 scripts/nohttpserver.py [port] [bind]

Defaults: port=8000, bind=127.0.0.1
"""

import http.server
import sys


class NoCacheHandler(http.server.CGIHTTPRequestHandler):
    @staticmethod
    def _content_type_with_utf8(value):
        ctype = value.strip()
        lower = ctype.lower()

        if ";" in ctype:
            return value
        if lower.startswith("text/"):
            return ctype + "; charset=utf-8"
        if lower in {
            "application/javascript",
            "application/json",
            "application/xml",
            "image/svg+xml",
        }:
            return ctype + "; charset=utf-8"
        return value

    def send_header(self, keyword, value):
        if keyword.lower() == "content-type":
            value = self._content_type_with_utf8(value)
        super().send_header(keyword, value)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, fmt, *args):
        # Suppress per-request noise; keep error output.
        if args and str(args[1]) >= "400":
            super().log_message(fmt, *args)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    bind = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    http.server.test(HandlerClass=NoCacheHandler, port=port, bind=bind)
