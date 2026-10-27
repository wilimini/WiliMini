#!/usr/bin/env python3
"""Replay real App navigation with synthetic API data; no decoder or live network."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
output = root / 'results/ui-review'
output.mkdir(parents=True, exist_ok=True)
(root / 'build/headless').mkdir(parents=True, exist_ok=True)
memstick = Path(tempfile.mkdtemp(prefix='production-ui-', dir=root / 'build/headless'))
game = memstick / 'PSP/GAME/WiliMini'
shutil.copytree(root / 'dist/PSP/GAME/WiliMini', game, ignore=shutil.ignore_patterns('cache', 'history.json', 'later.json'))
subprocess.run([sys.executable, str(root / 'scripts/seed-offline-ui.py'), str(memstick)], check=True)
shots = [(1.5,'home'),(5,'detail'),(8.5,'detail-part'),(13,'comments'),(16.5,'comments-scroll'),(22,'comments-end'),(26.5,'return'),(31.5,'mine'),(34.5,'mine-detail'),(38,'mine-return')]
presses = '2:0x4000:0.2,5.5:0x40:0.2,6:0x8000:0.2,9:0x200:0.2,14:0x40:1.5,17:0x200:0.2,20:0x40:1.5,23:0x2000:0.2,25:0x2000:0.2,27:0x80:0.2,28:0x40:0.2,29:0x4000:0.2,32:0x4000:0.2,35:0x2000:0.2'
env = dict(os.environ, BILI_HEADLESS_CLIENT_DIR=str(root/'build/client-offline'), BILI_HEADLESS_MEMSTICK=str(memstick), BILI_HEADLESS_TIMEOUT='90', BILI_HEADLESS_TIMEOUT_EMULATED='39', BILI_HEADLESS_PRESS_SCRIPT=presses, BILI_HEADLESS_SCREENSHOT_AT=','.join(f'{t}:{output}/app-flow-{name}.png' for t,name in shots))
subprocess.run(['sh', str(root / 'scripts/run-client-headless.sh')], env=env, check=True)
for _,name in shots:
    Image.open(output / f'app-flow-{name}.png').crop((0,0,480,272)).save(output / f'app-flow-{name}-display.png')
(output/'production-flow-manifest.json').write_text(json.dumps(dict(build='BILI_DEVICE_OFFLINE_UI; production App, synthetic API fixtures', memstick=str(memstick), presses=presses, screenshots=shots),indent=2)+'\n')
print(output / 'production-flow-manifest.json')

trace = [json.loads(line) for line in (game/'ui-trace.jsonl').read_text().splitlines()]
screen_path = []
for state in trace:
    if not state['busy'] and (not screen_path or screen_path[-1] != state['screen']):
        screen_path.append(state['screen'])
checks = {
    'detail_part_selection': any(s['screen']==1 and s['part']==1 for s in trace),
    'full_comments_loaded': any(s['screen']==2 and s['comments']==4 for s in trace),
    'comments_scroll': any(s['screen']==2 and s['scroll']>=72 for s in trace),
    'comments_pagination': any(s['screen']==2 and s['comments']==5 and s['end'] for s in trace),
    'later_persisted': len(json.loads((game/'later.json').read_text()))==1,
    'mine_detail_back': screen_path==[0,1,2,1,0,3,1,3],
    'offline_no_play_history': not (game/'history.json').exists(),
}
(output/'production-flow-verification.json').write_text(json.dumps({'checks':checks,'trace':trace},indent=2)+'\n')
assert all(checks.values()), checks
pairs = [('replica-01-display.png','app-flow-home-display.png','Home'), ('input-detail.png','app-flow-detail-display.png','Detail'), ('replica-09-display.png','app-flow-comments-display.png','Comments')]
sheet = Image.new('RGB',(960,3*300),(242,243,245))
draw = ImageDraw.Draw(sheet)
for row,(reference,actual,label) in enumerate(pairs):
    y=row*300
    draw.text((12,y+5),f'{label}: approved prototype',fill=(43,46,53))
    draw.text((492,y+5),f'{label}: production App / synthetic API fixture',fill=(43,46,53))
    sheet.paste(Image.open(output/reference).crop((0,0,480,272)),(0,y+28))
    sheet.paste(Image.open(output/actual),(480,y+28))
sheet.save(output/'production-prototype-comparison.png')
print('PASS:', ', '.join(checks))
