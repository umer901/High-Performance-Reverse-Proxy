#!/usr/bin/env python3

import argparse
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import signal
import socket
import sys
import subprocess
import tempfile
import threading
import time


class TestBackendServer(ThreadingHTTPServer):
    allow_reuse_address = True


class BackendHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/healthz":
            self._write(200, b"ok\n")
            return
        body = json.dumps({"backend": self.server.backend_name, "path": self.path}).encode()
        self._write(200, body)

    def log_message(self, fmt, *args):
        return

    def _write(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def free_port():
    with contextlib.closing(socket.socket()) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_backend(name):
    server = TestBackendServer(("127.0.0.1", 0), BackendHandler)
    server.backend_name = name
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def http_get(port, path="/"):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        request = f"GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode()
        sock.sendall(request)
        chunks = []
        while True:
            data = sock.recv(65536)
            if not data:
                break
            chunks.append(data)
        return b"".join(chunks)


def wait_for_backend(port):
    deadline = time.time() + 5
    last_error = None
    while time.time() < deadline:
        try:
            if b" 200 " in http_get(port, "/healthz"):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"backend on port {port} did not become ready; last_error={last_error!r}")


def wait_for_proxy(port):
    deadline = time.time() + 5
    last = b""
    while time.time() < deadline:
        try:
            response = http_get(port, "/ready")
            last = response[:200]
            if b" 200 " in response:
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"proxy did not become ready; last response={last!r}")


def stop_process(proc):
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()

    proxy_port = free_port()
    metrics_port = free_port()
    backends = [start_backend(f"backend-{i}") for i in range(2)]
    backend_ports = [backend.server_address[1] for backend in backends]
    for port in backend_ports:
        wait_for_backend(port)

    config = f"""
listen: "127.0.0.1:{proxy_port}"
metrics_listen: "127.0.0.1:{metrics_port}"
workers: 1
load_balancing:
  strategy: "round_robin"
limits:
  max_client_connections: 100
  client_header_timeout_ms: 1000
  upstream_connect_timeout_ms: 500
  request_timeout_ms: 3000
  max_buffer_bytes: 262144
backends:
  - name: "backend-0"
    url: "http://127.0.0.1:{backend_ports[0]}"
    health_check:
      path: "/healthz"
      interval_ms: 200
      timeout_ms: 100
      unhealthy_threshold: 1
      healthy_threshold: 1
  - name: "backend-1"
    url: "http://127.0.0.1:{backend_ports[1]}"
    health_check:
      path: "/healthz"
      interval_ms: 200
      timeout_ms: 100
      unhealthy_threshold: 1
      healthy_threshold: 1
"""
    with tempfile.NamedTemporaryFile("w", delete=False) as cfg:
        cfg.write(config)
        cfg_path = cfg.name

    proc = subprocess.Popen([args.binary, "--config", cfg_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        try:
            wait_for_proxy(proxy_port)
        except Exception:
            stop_process(proc)
            try:
                _, stderr = proc.communicate(timeout=1)
            except subprocess.TimeoutExpired:
                proc.kill()
                _, stderr = proc.communicate()
            sys.stderr.write(stderr.decode(errors="replace"))
            raise
        seen = set()
        for i in range(6):
            response = http_get(proxy_port, f"/item/{i}")
            assert b" 200 " in response, response
            if b"backend-0" in response:
                seen.add("backend-0")
            if b"backend-1" in response:
                seen.add("backend-1")
        assert seen == {"backend-0", "backend-1"}, seen

        metrics = http_get(metrics_port, "/metrics")
        assert b"hprp_requests_total" in metrics, metrics
        assert b"hprp_backend_healthy" in metrics, metrics
    finally:
        stop_process(proc)
        os.unlink(cfg_path)
        for backend in backends:
            backend.shutdown()
            backend.server_close()


if __name__ == "__main__":
    main()
