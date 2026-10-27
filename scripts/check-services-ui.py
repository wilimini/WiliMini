#!/usr/bin/env python3
"""Exercise production App search/auth with offline fixtures in PPSSPP Headless."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'build/services-ui-check'
out.mkdir(parents=True, exist_ok=True)
package = root / 'build/services-offline-dist/PSP/GAME/WiliMini'
client = root / 'build/client-services-offline'
checks = {}


def replay(name, events, shots, duration, fixtures=None):
    memstick = Path(tempfile.mkdtemp(prefix=name + '-', dir=out))
    game = memstick / 'PSP/GAME/WiliMini'
    shutil.copytree(package, game)
    subprocess.run([sys.executable, str(root / 'scripts/seed-offline-ui.py'), str(memstick)],
                   check=True, stdout=subprocess.DEVNULL)
    for file, body in (fixtures or {}).items():
        (game / 'cache' / file).write_text(json.dumps(body))
    env = dict(os.environ, BILI_HEADLESS_CLIENT_DIR=str(client),
               BILI_HEADLESS_PACKAGE=str(package), BILI_HEADLESS_MEMSTICK=str(memstick),
               BILI_HEADLESS_TIMEOUT='90', BILI_HEADLESS_TIMEOUT_EMULATED=str(duration),
               BILI_HEADLESS_PRESS_SCRIPT=','.join(f'{t}:{hex(key)}:0.2' for t, key in events),
               BILI_HEADLESS_SCREENSHOT_AT=','.join(f'{t}:{out}/{name}-{label}.png' for t, label in shots))
    with (out / f'{name}.log').open('w') as log:
        subprocess.run(['sh', str(root / 'scripts/run-client-headless.sh')], env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    trace = [json.loads(line) for line in (game / 'ui-trace.jsonl').read_text().splitlines()]
    return trace


qr = {'code': 0, 'data': {'url': 'https://passport.bilibili.com/h5-app/passport/login/scan?key=offline-ui-example',
                         'qrcode_key': 'offline-ui-example'}}
for name, code, state in [('waiting', 86101, 2), ('confirm', 86090, 3), ('expired', 86038, 4)]:
    trace = replay(name, [(2, 0x100), (4, 0x4000), (10, 0x2000)],
                   [(8, 'login'), (11, 'back')], 12,
                   {'login_generate.json': qr, 'login_poll.json': {'code': 0, 'data': {'code': code}}})
    checks[name] = any(s['screen'] == 4 and s['auth_state'] == state and not s['busy'] for s in trace)
    checks[name + '_back'] = trace[-1]['screen'] == 3
    checks[name + '_stops_polling'] = not any(s['busy'] for s in trace[max(i for i, s in enumerate(trace) if s['screen'] == 3):])

trace = replay('qr-refresh', [(2, 0x100), (4, 0x4000), (8, 0x1000), (11, 0x2000)],
               [(9, 'renewed')], 13,
               {'login_generate.json': qr, 'login_poll.json': {'code': 0, 'data': {'code': 86038}}})
stable = [state for state in trace if not state['busy']]
checks['expired_qr_refreshes'] = any(
    stable[i-1]['auth_state'] == 4 and state['auth_state'] == 2
    for i, state in enumerate(stable) if i > 0)

trace = replay('auth-error', [(2, 0x100), (4, 0x4000), (10, 0x2000)], [(8, 'login')], 12,
               {'login_generate.json': qr, 'login_poll.json': {'code': -352}})
checks['auth_error_visible'] = any(s['screen'] == 4 and s['auth_error'] and not s['busy'] for s in trace)

events = [(2,0x200),(3,0x200),(7,0x40),(8,0x4000),(12,0x40),(13,0x4000),
          (17,0x40),(18,0x40),(19,0x40),(23,0x4000),(27,0x2000),(31,0x2000),
          (32,0x40),(33,0x4000),(37,0x2000),(38,0x40),(39,0x40),(40,0x40),
          (41,0x4000),(45,0x2000),(46,0x40),(47,0x4000)]
trace = replay('search', events, [(6,'topics'),(22,'page2'),(44,'empty'),(50,'error')], 52)
checks['search_topics'] = any(s['tab'] == 2 and s['topics'] == 6 for s in trace)
checks['search_page2'] = any(s['tab'] == 2 and s['cards'] == 6 and s['selection'] == 4 for s in trace)
checks['search_empty'] = any(s['tab'] == 2 and s['search_view'] == 2 and not s['cards'] and not s['busy'] and not s['search_error'] for s in trace)
checks['search_error'] = any(s['search_error'] and not s['busy'] for s in trace)

# A failed second page must preserve the four cards already on screen.
from urllib.parse import quote
failed_page = f'search_{quote("旅行记录", safe="")}_2.json'
trace = replay('page-error', events[:10], [(22,'preserved')], 24, {failed_page: {'code': -352}})
checks['page_error_preserves_cards'] = any(s['cards'] == 4 and s['search_error'] and not s['busy'] for s in trace)
(out / 'verification.json').write_text(json.dumps(checks, indent=2))
assert all(checks.values()), checks
print('PASS:', ', '.join(checks))
print(out)
