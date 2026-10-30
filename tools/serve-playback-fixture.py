#!/usr/bin/env python3
"""Serve two local, controlled media fixtures with HTTP Range support for PSP tests."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re

parser = argparse.ArgumentParser()
parser.add_argument('--video', type=Path, required=True)
parser.add_argument('--audio', type=Path, required=True)
parser.add_argument('--port', type=int, default=8765)
args = parser.parse_args()
files = {'/video.mp4': args.video.resolve(), '/audio.m4a': args.audio.resolve()}
for path in files.values():
    if not path.is_file(): parser.error(f'Missing fixture: {path}')

class Handler(BaseHTTPRequestHandler):
    def do_HEAD(self): self.serve(False)
    def do_GET(self): self.serve(True)
    def serve(self, body):
        path = files.get(self.path)
        if not path:
            self.send_error(404)
            return
        size = path.stat().st_size
        start, end, status = 0, size - 1, 200
        requested = self.headers.get('Range')
        if requested:
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', requested)
            if not match:
                self.send_error(416)
                return
            start = int(match[1])
            end = min(end, int(match[2])) if match[2] else end
            if start >= size or start > end:
                self.send_response(416)
                self.send_header('Content-Range', f'bytes */{size}')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            status = 206
        self.send_response(status)
        self.send_header('Content-Type', 'video/mp4' if self.path.endswith('.mp4') else 'audio/mp4')
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(end - start + 1))
        if status == 206: self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
        self.end_headers()
        print(f'{self.path} {status} {start}-{end}/{size}', flush=True)
        if not body: return
        try:
            with path.open('rb') as source:
                source.seek(start)
                left = end - start + 1
                while left:
                    chunk = source.read(min(65536, left))
                    if not chunk: break
                    self.wfile.write(chunk)
                    left -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass  # A seek intentionally abandons the previous range.
    def log_message(self, *args):
        pass

server = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
server.daemon_threads = True
print(f'Fixture server listening on port {args.port}', flush=True)
try: server.serve_forever()
except KeyboardInterrupt: pass
finally: server.server_close()
