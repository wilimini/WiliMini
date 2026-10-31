#!/usr/bin/env python3
"""Exercise the production handle lifecycle against a local HTTP/1.1 server."""
import http.server
import os
from pathlib import Path
import shlex
import subprocess
import threading

root = Path(__file__).resolve().parents[1]
out = root / "build/host-http"
out.mkdir(parents=True, exist_ok=True)
flags = shlex.split(subprocess.check_output(
    ["curl-config", "--cflags", "--libs"], text=True))
subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
    "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(root / "client"),
    str(root / "tests/http_transport_test.c"), "-o", str(out / "http-test"),
] + flags, check=True)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        self.respond()

    def do_GET(self):
        self.respond()

    def respond(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        first = self.path == "/0"
        valid = ((self.command == "POST" and body == b"test=body" and
                  self.headers.get("Cookie") == "test=private") if first else
                 (self.command == "GET" and not body and not self.headers.get("Cookie")))
        self.send_response(200 if valid else 400)
        payload = b"abort this response" if self.path == "/4" else b"ok"
        self.send_header("Content-Length", str(len(payload)))
        # Must not become a persistent cookie on the next request.
        self.send_header("Set-Cookie", "server=private; Path=/")
        if self.path == "/2":
            self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run([str(out / "http-test"),
                        f"http://127.0.0.1:{server.server_port}"], check=True)
    finally:
        server.shutdown()
        thread.join()
