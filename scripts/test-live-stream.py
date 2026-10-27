#!/usr/bin/env python3
"""Exercise the actual live transport with a local HTTP server and host PSP shims."""
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import subprocess
import threading
root=Path(__file__).resolve().parents[1]
out=root/'build/live-stream-tests';out.mkdir(parents=True,exist_ok=True)
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        self.send_response(200);self.end_headers()
        block=bytes((i*37+11)%251 for i in range(251*256))
        try:
            remaining=5*1024*1024+731
            while remaining:
                chunk=block[:min(remaining,len(block))]
                self.wfile.write(chunk);remaining-=len(chunk)
        except (BrokenPipeError,ConnectionResetError):pass
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
try:
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-pthread',
        '-I'+str(root/'tests/host_psp'),'-I'+str(root/'client'),'-I'+str(root/'decoder/include'),
        str(root/'tests/live_stream_test.c'),str(root/'client/live_stream.c'),'-lcurl','-o',str(out/'live-stream-test')],check=True)
    subprocess.run([str(out/'live-stream-test'),f'http://127.0.0.1:{server.server_port}/live.flv'],check=True,timeout=30)
finally:server.shutdown();server.server_close()
