#!/usr/bin/env python3
"""Replay production community pages in an isolated PPSSPP memstick."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from PIL import Image,ImageDraw
root=Path(__file__).resolve().parents[1]
out=root/'results/ui-community';out.mkdir(parents=True,exist_ok=True)
package=root/'build/community-offline-dist/PSP/GAME/WiliMini'
spec=importlib.util.spec_from_file_location('community_seed',root/'scripts/seed-community-ui.py')
seed=importlib.util.module_from_spec(spec);spec.loader.exec_module(seed)
scenarios={
 'dynamic': ([(2,0x80),(3,0x40),(4,0x4000),(7,0x40),(8,0x4000),(11,0x40),(12,0x4000),(15,0x2000),(17,0x2000),(19,0x200),(22,0x2000)],[(6,'feed'),(10,'detail'),(14,'video'),(20,'pagination'),(23,'returned')],24),
 'inbox': ([(2,0x80),(3,0x40),(4,0x40),(5,0x40),(6,0x4000),(9,0x4000),(12,0x2000),(14,0x40),(15,0x40),(16,0x40),(17,0x4000),(20,0x4000),(23,0x4000)],[(8,'menu'),(11,'notifications'),(19,'sessions'),(22,'conversation'),(25,'message')],26),
 'pgc': ([(2,0x200),(3,0x200),(6,0x200),(8,0x4000),(11,0x4000),(14,0x2000),(16,0x2000),(18,0x40),(19,0x40),(20,0x4000),(23,0x4000),(26,0x4000)],[(7,'discover'),(10,'list'),(13,'season'),(22,'areas'),(25,'rooms'),(28,'room')],29),
 'comments': ([(2,0x4000),(5,0x200),(8,0x4000),(11,0x4000),(14,0x2000),(16,0x2000)],[(7,'list'),(10,'actions'),(13,'replies'),(17,'returned')],18),
 'space': ([(2,0x80),(3,0x40),(4,0x4000),(7,0x40),(8,0x4000),(11,0x40),(12,0x40),(13,0x4000),(16,0x4000),(19,0x2000)],[(10,'dynamic'),(15,'profile'),(18,'uploads'),(20,'returned')],21),
 'cloud': ([(2,0x80),(3,0x40),(4,0x40),(5,0x4000),(8,0x8000)],[(7,'mine'),(10,'menu')],11),
 'video-actions': ([(2,0x4000),(5,0x100),(8,0x40),(9,0x40),(10,0x4000),(13,0x2000),(15,0x2000)],[(7,'menu'),(12,'folders'),(16,'returned')],17),
}
checks={};manifest={}
for name,(events,shots,duration) in scenarios.items():
    memstick=Path(tempfile.mkdtemp(prefix='community-'+name+'-',dir=root/'build/headless'))
    game=memstick/'PSP/GAME/WiliMini'
    shutil.copytree(package,game,ignore=shutil.ignore_patterns('cache','session.json','history.json','later.json','ui-trace.jsonl'))
    subprocess.run([sys.executable,str(root/'scripts/seed-offline-ui.py'),str(memstick)],check=True,stdout=subprocess.DEVNULL)
    seed.seed(game)
    env=dict(os.environ,BILI_HEADLESS_CLIENT_DIR=str(root/'build/community-offline'),BILI_HEADLESS_PACKAGE=str(package),BILI_HEADLESS_MEMSTICK=str(memstick),BILI_HEADLESS_TIMEOUT='120',BILI_HEADLESS_TIMEOUT_EMULATED=str(duration),
        BILI_HEADLESS_PRESS_SCRIPT=','.join(f'{time}:{hex(key)}:0.15' for time,key in events),BILI_HEADLESS_SCREENSHOT_AT=','.join(f'{time}:{out}/{name}-{label}.png' for time,label in shots))
    with (out/f'{name}.log').open('w') as log:subprocess.run(['sh',str(root/'scripts/run-client-headless.sh')],env=env,check=True,stdout=log,stderr=subprocess.STDOUT)
    trace=[json.loads(line) for line in (game/'ui-trace.jsonl').read_text().splitlines()]
    (out/f'{name}-trace.json').write_text(json.dumps(trace,indent=2))
    for _,label in shots:Image.open(out/f'{name}-{label}.png').crop((0,0,480,272)).save(out/f'{name}-{label}-display.png')
    kinds={state.get('browse_kind') for state in trace if not state['busy'] and not state.get('browse_error')}
    expected={'dynamic':{3,4},'inbox':{2,7,8,9},'pgc':{0,10,12,13,14,15},'comments':{24,25},'space':{3,4,5,6},'cloud':{1},'video-actions':{31,32}}[name]
    checks[name+'_pages']=expected<=kinds
    checks[name+'_requests']=not any(state.get('browse_error') for state in trace)
    if name=='dynamic':checks['dynamic_next_page']=any(state.get('browse_kind')==3 and state.get('browse_page')==2 and state.get('browse_items')==3 for state in trace)
    if name=='comments':checks['comments_return']=trace[-1]['screen']==2 and trace[-1]['comments']==4
    if name=='space':checks['space_return']=trace[-1].get('browse_kind')==5
    manifest[name]=dict(memstick=str(memstick),events=events,screenshots=shots)
(out/'verification.json').write_text(json.dumps(dict(checks=checks,scenarios=manifest),indent=2))
print(json.dumps(checks,indent=2))
assert all(checks.values()),checks
