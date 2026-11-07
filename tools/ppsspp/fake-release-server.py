#!/usr/bin/env python3
"""Local HTTPS stand-in for GitHub/Codeberg release APIs (tools/ppsspp/test-update-flow.py).

  /github/releases/latest    always 503 (the client must not fall back from it)
  /codeberg/releases/latest  Forgejo-style JSON: no digest, SHA256SUMS asset instead
  /dl/<name>                 302 to /cdn/<name> (redirects must be followed)
  /cdn/<name>                the zip, with HTTP Range support

Every request line and Range header is appended to REQUEST_LOG.
"""
import hashlib
import http.server
import json
import ssl
import sys
import threading

zip_path, cert, key, log_path, tag = sys.argv[1:6]
port = int(sys.argv[6]) if len(sys.argv) > 6 else 8443
data = open(zip_path, 'rb').read()
name = f'WiliMini-{tag}.zip'
base = f'https://127.0.0.1:{port}'
sums = f'{hashlib.sha256(data).hexdigest()}  {name}\n'.encode()
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def note(self):
        with lock, open(log_path, 'a') as f:
            f.write(f'{self.command} {self.path} range={self.headers.get("Range", "-")} ua={self.headers.get("User-Agent", "-")}\n')

    def reply(self, status, body=b'', ctype='application/octet-stream', extra=()):
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def do_GET(self):
        self.note()
        p = self.path
        if p == '/github/releases/latest':
            return self.reply(503, b'down', 'text/plain')
        if p == '/codeberg/releases/latest':
            release = {'tag_name': tag, 'name': f'WiliMini {tag}', 'draft': False, 'prerelease': False,
                       'body': "## What's new\n- Update test release\n- Served by the local fake server",
                       'assets': [{'name': name, 'size': len(data), 'browser_download_url': f'{base}/dl/{name}'},
                                  {'name': 'SHA256SUMS', 'size': len(sums), 'browser_download_url': f'{base}/dl/SHA256SUMS'}]}
            return self.reply(200, json.dumps(release).encode(), 'application/json')
        if p.startswith('/dl/'):
            return self.reply(302, b'', 'text/plain', [('Location', base + '/cdn/' + p[4:])])
        if p == '/cdn/SHA256SUMS':
            return self.reply(200, sums, 'text/plain')
        if p == f'/cdn/{name}':
            rng = self.headers.get('Range')
            if rng and rng.startswith('bytes=') and rng.endswith('-'):
                start = int(rng[6:-1])
                if start >= len(data):
                    return self.reply(416, b'', 'text/plain', [('Content-Range', f'bytes */{len(data)}')])
                return self.reply(206, data[start:], 'application/zip',
                                  [('Content-Range', f'bytes {start}-{len(data) - 1}/{len(data)}')])
            return self.reply(200, data, 'application/zip')
        self.reply(404, b'not found', 'text/plain')


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True


server = Server(('127.0.0.1', port), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
print('listening', port, flush=True)
server.serve_forever()
