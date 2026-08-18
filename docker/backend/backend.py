#!/usr/bin/env python3

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import argparse
import json
import os
import time


class Handler(BaseHTTPRequestHandler):
    server_version = "hprp-test-backend/0.1"

    def do_GET(self):
        if self.path == "/healthz":
            self._write(200, b"ok\n", "text/plain")
            return

        delay_ms = int(os.environ.get("BACKEND_DELAY_MS", "0"))
        if delay_ms:
            time.sleep(delay_ms / 1000.0)

        body = json.dumps(
            {
                "backend": os.environ.get("BACKEND_NAME", "backend"),
                "path": self.path,
                "pid": os.getpid(),
            }
        ).encode()
        self._write(200, body, "application/json")

    def log_message(self, fmt, *args):
        return

    def _write(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9001)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
