#!/usr/bin/env python3
"""Replay search landing, history, results, pagination and error through the real App."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[2]
output = root / 'results/ui-review'
output.mkdir(parents=True, exist_ok=True)
memstick = Path(tempfile.mkdtemp(prefix='search-ui-', dir=root / 'build/headless'))
game = memstick / 'PSP/GAME/WiliMini'
shutil.copytree(root/'dist/PSP/GAME/WiliMini',game,ignore=shutil.ignore_patterns('cache','history.json','later.json','ui-trace.jsonl'))
subprocess.run([sys.executable,str(root/'tools/ppsspp/seed-offline-ui.py'),str(memstick)],check=True)
shots = [(6,'landing'),(11,'history'),(16,'results'),(22,'next-page'),(26,'detail'),(30,'return'),(44,'empty'),(50,'error'),(60,'history-updated'),(64,'history-deleted'),(69,'home-restored'),(76,'keyboard')]
events = [(2,0x200),(3,0x200),(7,0x40),(8,0x4000),(12,0x40),(13,0x4000),(17,0x40),(18,0x40),(19,0x40),(23,0x4000),(27,0x2000),(31,0x2000),(32,0x40),(33,0x4000),(37,0x2000),(38,0x40),(39,0x40),(40,0x40),(41,0x4000),(45,0x2000),(46,0x40),(47,0x4000),(51,0x2000),(52,0x10),(53,0x10),(54,0x10),(55,0x10),(56,0x10),(57,0x4000),(61,0x1000),(65,0x2000),(66,0x2000),(70,0x200),(71,0x200),(72,0x10),(73,0x4000)]
presses = ','.join(f'{time}:{hex(button)}:0.2' for time,button in events)
env = dict(os.environ,BILI_HEADLESS_CLIENT_DIR=str(root/'build/client-offline'),BILI_HEADLESS_MEMSTICK=str(memstick),BILI_HEADLESS_TIMEOUT='90',BILI_HEADLESS_TIMEOUT_EMULATED='78',BILI_HEADLESS_PRESS_SCRIPT=presses,BILI_HEADLESS_SCREENSHOT_AT=','.join(f'{time}:{output}/app-search-{name}.png' for time,name in shots))
subprocess.run(['sh',str(root/'tools/ppsspp/run-client-headless.sh')],env=env,check=True)
for _,name in shots:
    Image.open(output/f'app-search-{name}.png').crop((0,0,480,272)).save(output/f'app-search-{name}-display.png')
trace=[json.loads(line) for line in (game/'ui-trace.jsonl').read_text().splitlines()]
history=json.loads((game/'search-history.json').read_text())
checks={
    'trending_loaded':any(s['tab']==2 and s['topics']==6 and s['search_view']==0 for s in trace),
    'history_selected':any(s['tab']==2 and s['search_view']==1 and s['selection']==1 for s in trace),
    'results_loaded':any(s['tab']==2 and s['search_view']==2 and s['cards']==4 and not s['busy'] for s in trace),
    'pagination_appended':any(s['tab']==2 and s['search_view']==2 and s['cards']==6 and s['selection']==4 for s in trace),
    'detail_return_focus':any(s['tab']==2 and s['screen']==1 and s['selection']==4 for s in trace),
    'empty_distinct_from_error':any(s['tab']==2 and s['search_view']==2 and not s['cards'] and not s['busy'] and not s['search_error'] for s in trace),
    'api_error_visible':any(s['search_error'] and not s['busy'] for s in trace),
    'history_persisted_and_deleted':history==['没有结果的关键词','秋日旅行与沿途风景','旅行记录','掌机游戏','游戏开发'],
    'no_playback_history':not (game/'history.json').exists(),
}
(output/'search-flow-verification.json').write_text(json.dumps({'checks':checks,'history':history,'trace':trace},ensure_ascii=False,indent=2)+'\n')
(output/'search-flow-manifest.json').write_text(json.dumps(dict(build='BILI_DEVICE_OFFLINE_UI; real App; synthetic API fixtures',memstick=str(memstick),screenshots=shots,presses=presses),indent=2)+'\n')
sheet=Image.new('RGB',(960,300),(242,243,245))
draw=ImageDraw.Draw(sheet)
draw.text((12,5),'Search: approved prototype',fill=(43,46,53))
draw.text((492,5),'Search: production App / synthetic API fixture',fill=(43,46,53))
sheet.paste(Image.open(output/'replica-02-display.png').crop((0,0,480,272)),(0,28))
sheet.paste(Image.open(output/'app-search-landing-display.png'),(480,28))
sheet.save(output/'search-prototype-comparison.png')
assert all(checks.values()),checks
print('PASS:',', '.join(checks))
