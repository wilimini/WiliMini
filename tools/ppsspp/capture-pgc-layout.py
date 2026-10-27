#!/usr/bin/env python3
"""Replay the approved poster/episode layouts through the production App."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from PIL import Image

root = Path(__file__).resolve().parents[2]
out = root / 'results/ui-pgc-layout'
out.mkdir(parents=True, exist_ok=True)
memstick = Path(tempfile.mkdtemp(prefix='pgc-layout-', dir=root/'build/headless'))
game = memstick/'PSP/GAME/WiliMini'
package = root/'build/pgc-layout-dist/PSP/GAME/WiliMini'
shutil.copytree(package, game, ignore=shutil.ignore_patterns('cache', 'session.json', 'ui-trace.jsonl'))
subprocess.run([sys.executable, str(root/'tools/ppsspp/seed-offline-ui.py'), str(memstick)], check=True)
spec = importlib.util.spec_from_file_location('seed', root/'tools/ppsspp/seed-community-ui.py')
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)
fixture = seed.seed(game)
(game/'session.json').unlink()
titles = ['间谍过家家', '工作细胞', 'JOJO的奇妙冒险 黄金之风', '宇崎学妹想要玩！']
ids = [41410, 24588, 25681, 33800]
items = [dict(season_id=sid, title=title, cover='https://example.invalid/poster.jpg',
              new_ep=dict(index_show=status))
         for sid, title, status in zip(ids, titles, ['全25话', '全14话', '全39话', '全12话'])]
items += [dict(items[0], season_id=999, title='第二排作品')]
fixture(seed.api+'/pgc/page/pc/bangumi/tab', [('is_refresh', '1'), ('cursor', '')],
        dict(modules=[dict(items=items)], has_next=False, next_cursor=''))
episodes = [dict(id=33+i, aid=900001+i, cid=600001+i, bvid='BV1kRn36oEvr',
                 title=str(i+1), long_title=title, duration=1450000,
                 cover='https://example.invalid/episode.jpg')
            for i, title in enumerate(['枭行动', '寻找妻子', '准备考试', '名门学校面试', '合格与否', '好朋友大作战'])]
for sid in (41410, 999):
    fixture(seed.api+'/pgc/view/pc/season', [('season_id', str(sid))],
            dict(title='间谍过家家', new_ep=dict(desc='全25话'), styles=['日常', '搞笑'],
                 evaluate='每个人都拥有不想被他人知晓的一面。这是用于正式页面布局回放的演示简介。',
                 episodes=episodes, user_status=dict(last_ep_id=34, last_time=632)))
assets = root/'tests/fixtures/pgc'
for sid in ids:
    shutil.copy2(assets/f'{sid}.jpg', game/'cache'/f'pgc_season_{sid}.jpg')
shutil.copy2(assets/'41410.jpg', game/'cache/pgc_season_999.jpg')
for i in range(6):
    shutil.copy2(assets/f'episode-{i%3}.jpg', game/'cache'/f'pgc_episode_{33+i}.jpg')
events = [(2,0x200),(3,0x200),(6,0x200),(8,0x4000),(11,0x20),(13,0x80),
          (14,0x4000),(18,0x40),(21,0x40),(22,0x40),(25,0x20),(28,0x80),
          (29,0x2000),(32,0x40)]
shots = [(10,'posters'),(12,'poster-focus'),(17,'episodes'),(20,'episode-progress'),
         (24,'episode-scroll'),(27,'synopsis'),(31,'returned'),(34,'second-row')]
env = dict(os.environ, BILI_HEADLESS_CLIENT_DIR=str(root/'build/pgc-layout'),
           BILI_HEADLESS_PACKAGE=str(package), BILI_HEADLESS_MEMSTICK=str(memstick),
           BILI_HEADLESS_TIMEOUT='120', BILI_HEADLESS_TIMEOUT_EMULATED='35',
           BILI_HEADLESS_PRESS_SCRIPT=','.join(f'{t}:{hex(k)}:0.15' for t,k in events),
           BILI_HEADLESS_SCREENSHOT_AT=','.join(f'{t}:{out}/{label}.png' for t,label in shots))
with (out/'replay.log').open('w') as log:
    subprocess.run(['sh', str(root/'tools/ppsspp/run-client-headless.sh')], env=env,
                   check=True, stdout=log, stderr=subprocess.STDOUT)
trace = [json.loads(line) for line in (game/'ui-trace.jsonl').read_text().splitlines()]
for _, label in shots:
    Image.open(out/f'{label}.png').crop((0,0,480,272)).save(out/f'{label}-display.png')
states = [s for s in trace if not s['busy']]
checks = {
    'requests': not any(s.get('browse_error') for s in trace),
    'posters': any(s.get('browse_kind') == 10 and s.get('browse_items') == 5 for s in states),
    'horizontal_focus': any(s.get('browse_kind') == 10 and s.get('browse_selection') == 1 for s in states),
    'episode_first': any(s.get('browse_kind') == 12 and s.get('browse_selection') == 1 for s in states),
    'episode_scroll': any(s.get('browse_kind') == 12 and s.get('browse_selection') == 4 for s in states),
    'synopsis_tab': any(s.get('browse_kind') == 12 and s.get('browse_selection') == 0 for s in states),
    'vertical_grid': states[-1].get('browse_kind') == 10 and states[-1].get('browse_selection') == 4,
}
(out/'verification.json').write_text(json.dumps(dict(checks=checks, memstick=str(memstick), events=events), indent=2))
(out/'trace.json').write_text(json.dumps(trace, indent=2))
print(json.dumps(checks, indent=2))
assert all(checks.values()), checks
