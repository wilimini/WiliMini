#!/usr/bin/env python3
"""Replay settings, storage, help and license pages using an isolated PPSSPP package."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[2]
output = root/'results/ui-settings'
output.mkdir(parents=True, exist_ok=True)
package = root/'build/settings-dist/PSP/GAME/WiliMini'
(root/'build/headless').mkdir(parents=True, exist_ok=True)
memstick = Path(tempfile.mkdtemp(prefix='settings-ui-', dir=root/'build/headless'))
game = memstick/'PSP/GAME/WiliMini'
shutil.copytree(package, game, ignore=shutil.ignore_patterns('cache','settings.json','history.json','later.json','session.json','ui-trace.jsonl'))
subprocess.run([sys.executable,str(root/'tools/ppsspp/seed-offline-ui.py'),str(memstick)],check=True)
(game/'later.json').write_text('[]')
(game/'cache/protected.json').write_text('{"keep":true}')
shutil.copy(root/'client/assets/wiliwili/08.png',game/'cache/protected.png')
protected = [game/'cache/home.json',game/'cache/protected.json',game/'cache/protected.png',game/'later.json',game/'search-history.json']
before = {str(path):path.read_bytes() for path in protected}
shots = [(7,'light'),(11,'dark'),(20,'large'),(25,'home-covers-off'),(34,'storage'),(38,'confirm-clear'),(44,'cleared'),(50,'help'),(56,'about'),(60,'licenses'),(65,'license-text'),(69,'license-scroll'),(75,'final')]
events = [(2,0x80,.2),(3,0x10,.2),(4,0x4000,.2),(8,0x20,.8),(12,0x40,.2),(13,0x20,.2),(14,0x40,.2),(15,0x20,.2),(16,0x40,.2),(17,0x20,.2),(21,0x2000,.2),(22,0x20,.2),(26,0x80,.2),(27,0x10,.2),(28,0x4000,.2),(29,0x40,.2),(30,0x40,.2),(31,0x4000,.2),(35,0x4000,.2),(39,0x2000,.2),(40,0x4000,.2),(41,0x4000,.2),(45,0x2000,.2),(46,0x40,.2),(47,0x4000,.2),(51,0x2000,.2),(52,0x40,.2),(53,0x4000,.2),(57,0x4000,.2),(61,0x40,.2),(62,0x4000,.2),(66,0x40,1.5),(70,0x2000,.2),(71,0x2000,.2),(72,0x2000,.2)]

def run(shots, events, duration, prefix='settings'):
    presses = ','.join(f'{time}:{hex(button)}:{hold}' for time,button,hold in events)
    env = dict(os.environ, BILI_HEADLESS_CLIENT_DIR=str(root/'build/settings-offline'),BILI_HEADLESS_PACKAGE=str(package),BILI_HEADLESS_MEMSTICK=str(memstick),BILI_HEADLESS_TIMEOUT='90',BILI_HEADLESS_TIMEOUT_EMULATED=str(duration),BILI_HEADLESS_PRESS_SCRIPT=presses,BILI_HEADLESS_SCREENSHOT_AT=','.join(f'{time}:{output}/{prefix}-{name}.png' for time,name in shots))
    subprocess.run(['sh',str(root/'tools/ppsspp/run-client-headless.sh')],env=env,check=True)
    for _,name in shots:
        Image.open(output/f'{prefix}-{name}.png').crop((0,0,480,272)).save(output/f'{prefix}-{name}-display.png')
run(shots,events,76)
trace=[json.loads(line) for line in (game/'ui-trace.jsonl').read_text().splitlines()]
settings=json.loads((game/'settings.json').read_text())
checks={
    'all_setting_changes':any(s['dark'] and s['body_size']==16 and not s['show_covers'] and s['slow_repeat'] for s in trace),
    'held_value_changes_once':sum(a['screen']==5 and b['screen']==5 and a['setting']==0 and b['setting']==0 and a['dark']!=b['dark'] for a,b in zip(trace,trace[1:]))==1,
    'storage_before':any(s['screen']==6 and s['cover_files']==6 for s in trace),
    'clear_confirmation':any(s['screen']==6 and s['clearing_covers'] and s['cover_files']==6 for s in trace),
    'cancel_retains_files':any(a['clearing_covers'] and b['screen']==6 and not b['clearing_covers'] and b['cover_files']==6 for a,b in zip(trace,trace[1:])),
    'covers_cleared':not list((game/'cache').glob('*.jpg')) and any(s['cover_generation']==1 for s in trace),
    'other_data_preserved':all(path.read_bytes()==before[str(path)] for path in protected),
    'help_about_licenses':all(any(s['screen']==page for s in trace) for page in (7,8,9,10)),
    'license_scroll':any(s['screen']==10 and s['document_scroll']>=100 for s in trace),
    'settings_persisted':settings==dict(version=1,dark_theme=True,body_size=16,show_covers=False,slow_repeat=True),
}
restart_offset=len(trace)
run([(2,'home'),(8,'settings')],[(3,0x80,.2),(4,0x10,.2),(5,0x4000,.2)],9,'restart')
all_trace=[json.loads(line) for line in (game/'ui-trace.jsonl').read_text().splitlines()]
restart=all_trace[restart_offset:]
checks['restart_restores_preferences']=any(s['screen']==5 and s['dark'] and s['body_size']==16 and not s['show_covers'] and s['slow_repeat'] for s in restart)
run([(5,'detail'),(9,'comments'),(13,'comments-scroll')],[(2,0x4000,.2),(6,0x200,.2),(10,0x40,1.5)],14,'large-text')
all_trace=[json.loads(line) for line in (game/'ui-trace.jsonl').read_text().splitlines()]
checks['large_comments_scroll']=any(s['screen']==2 and s['body_size']==16 and s['scroll']>=100 for s in all_trace)
for name in ('settings-light','settings-dark','restart-home'):
    pixel=Image.open(output/f'{name}-display.png').getpixel((479,0))[:3]
    expected=(242,243,245) if name=='settings-light' else (35,37,42)
    checks[f'{name}_palette']=pixel==expected
(output/'verification.json').write_text(json.dumps(dict(checks=checks,trace=all_trace),indent=2)+'\n')
(output/'manifest.json').write_text(json.dumps(dict(package=str(package),memstick=str(memstick),shots=shots,events=events),indent=2)+'\n')
sheet=Image.new('RGB',(960,300),(242,243,245))
draw=ImageDraw.Draw(sheet)
draw.text((12,5),'Approved settings prototype',fill=(43,46,53))
draw.text((492,5),'Production settings: connected actions and saved values',fill=(43,46,53))
sheet.paste(Image.open(root/'results/ui-review/replica-14-display.png').crop((0,0,480,272)),(0,28))
sheet.paste(Image.open(output/'settings-light-display.png'),(480,28))
sheet.save(output/'prototype-comparison.png')
assert all(checks.values()),checks
print('PASS:', ', '.join(checks))
