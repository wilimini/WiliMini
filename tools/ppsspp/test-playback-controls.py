#!/usr/bin/env python3
"""Replay production controls with native ME; requires a built test package and firmware."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
build = Path(os.environ.get('BILI_HEADLESS_CLIENT_DIR', root / 'build/playback-controls'))
package = Path(os.environ.get('BILI_HEADLESS_PACKAGE', root / 'build/playback-controls-dist/PSP/GAME/WiliMini'))
out = Path(os.environ.get('BILI_PLAYBACK_RESULTS', root / 'results/playback-controls'))
out.mkdir(parents=True, exist_ok=True)
media = root / 'build/playback-fixtures'
# Generate the same short, deterministic H.264/AAC sample as the playback suite.
media.mkdir(parents=True, exist_ok=True)
subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                'testsrc2=size=640x360:rate=15', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
                '-t', '8', '-c:v', 'libx264', '-profile:v', 'baseline', '-preset', 'ultrafast',
                '-g', '30', '-pix_fmt', 'yuv420p', '-b:v', '300k', '-c:a', 'aac', '-ac', '2',
                '-movflags', '+faststart', str(media / 'controls.mp4')], check=True)

cases = {
    'pause': ({}, '4:0x8:0.2,7:0x8:0.2,16:0x8:0.2', [3, 5, 6, 9], 18),
    'seek': ({}, '4:0x200:0.2,11:0x8:0.2', [3, 4.3, 6], 13),
    'paused-exit': ({'circle_confirm': True, 'english': True}, '4:0x2000:0.2,6:0x4000:0.2,10:0x8:0.2', [5], 12),
    'paused-seek': ({}, '4:0x8:0.2,5:0x200:0.2,8:0x2000:0.2,11:0x8:0.2', [6.5, 7.5], 13),
    'hold-preview': ({}, '4:0x20:1.2,11:0x8:0.2', [4.2, 4.8, 6.5], 13),
    'split-seek': ({'mode': 'split', 'audio': 'controls.mp4'}, '4:0x200:0.2,11:0x8:0.2', [6], 13),
    'menu': ({'danmaku': True}, '3.3:0x1000:0.15,3.6:0x20:0.15,3.9:0x4000:0.15,4.2:0x20:0.15,4.5:0x40:0.15,4.8:0x20:0.15,5.1:0x40:0.15,5.4:0x80:0.15,5.7:0x10:0.15,7.2:0x1000:0.15,9:0x2000:0.2,11:0x8:0.2', [3.5, 4.4, 4.8, 6.9, 8], 13),
    'quality-menu': ({}, '3.5:0x1000:0.15,3.8:0x4000:0.15,4.2:0x20:0.15,4.6:0x4000:0.15,10:0x8:0.2', [4.4], 12),
    'subtitles': ({'subtitles': True, 'start_paused': True}, '4:0x1000:0.15,4.3:0x80:0.15,4.6:0x4000:0.15,5:0x40:0.15,6:0x40:0.15,7.2:0x1000:0.15,8:0x8:0.15,10:0x20:0.15,12:0x2000:0.15,14:0x8:0.15', [5.5, 6.5, 9, 11.8], 16),
    'seek-cancel': ({}, '4:0x20:0.15,4.2:0x2000:0.15,7:0x2000:0.2,11:0x8:0.2', [4.1, 5], 13),
}
checks = {}
for name in sys.argv[1:] or cases:
    extra, presses, shots, duration = cases[name]
    stick = Path(tempfile.mkdtemp(prefix='controls-'+name+'-', dir=root / 'build/headless'))
    game = stick / 'PSP/GAME/WiliMini'
    shutil.copytree(package, game, ignore=shutil.ignore_patterns('cache', 'session.json', 'results'))
    shutil.copy2(media / 'controls.mp4', game / 'controls.mp4')
    config = dict(video='controls.mp4', mode='file', controls=True, buffer_frames=2)
    config.update(extra)
    (game / 'playback-test.json').write_text(json.dumps(config))
    env = dict(os.environ, BILI_HEADLESS_CLIENT_DIR=str(build), BILI_HEADLESS_PACKAGE=str(package),
               BILI_HEADLESS_MEMSTICK=str(stick), BILI_HEADLESS_TIMEOUT='180',
               BILI_HEADLESS_TIMEOUT_EMULATED=str(duration), BILI_HEADLESS_PRESS_SCRIPT=presses,
               BILI_HEADLESS_SCREENSHOT_AT=','.join(f'{t}:{out}/{name}-{t}.png' for t in shots),
               BILI_HEADLESS_DUMP_AUDIO=str(out / (name + '.wav')))
    with (out / (name + '.log')).open('w') as log:
        subprocess.run(['sh', str(root / 'tools/ppsspp/run-client-headless.sh')], env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    result = json.loads((game / 'playback-test-result.json').read_text())
    (out / (name + '.json')).write_text(json.dumps(dict(result=result, memstick=str(stick)), indent=2))
    checks[name+'_success'] = result['code'] == 0 and result['frames'] > 0
    samples = result['control_samples'][0]
    if name in ('pause', 'paused-exit'):
        paused = [x for x in samples if x['paused']]
        checks[name+'_clock_frozen'] = len(paused) >= 10 and len({x['position_ms'] for x in paused}) == 1
    if name == 'pause':
        checks['pause_resume_eof'] = result['eof'] == 1 and result['position_ms'] >= 7800
        checks['auto_hide'] = any(not x['visible'] for x in samples if not x['paused'])
    elif name in ('seek', 'split-seek'):
        checks[name+'_in_session'] = result['seek_count'] == 1 and result['last_seek_ms'] == 7000 and len(result['plays']) == 1
        checks[name+'_eof'] = result['eof'] == 1 and result['position_ms'] >= 7800
        checks[name+'_single_me_init'] = (out / (name + '.log')).read_text().count('MEM rgb_start') == 1
    elif name == 'menu':
        checks['menu_visible'] = any(x['menu'] for x in samples)
        checks['danmaku_toggle'] = any(not x['danmaku'] for x in samples)
        checks['density_and_opacity'] = any(x['density'] == 2 and x['opacity'] == 50 for x in samples)
    elif name == 'subtitles':
        checks['subtitle_tracks_selectable'] = {1, 2}.issubset({x['subtitle'] for x in samples})
        checks['subtitle_seek_retained'] = result['seek_count'] == 1 and len(result['plays']) == 1
        paused = [x for x in samples if x['paused']]
        checks['subtitle_pause_clock'] = len(paused) > 10 and len({x['position_ms'] for x in paused}) == 1
    elif name == 'quality-menu':
        checks['quality_selected'] = result['quality_requested'] == 32
    elif name == 'paused-seek':
        checks['paused_seek_in_session'] = result['seek_count'] == 1 and len(result['plays']) == 1
        checks['paused_seek_position'] = result['position_ms'] >= 7000 and result['cancelled'] == 1
    elif name == 'hold-preview':
        previews = [x['preview_ms'] for x in samples if x['seeking']]
        checks['hold_preview_moves'] = len(set(previews)) >= 3
        checks['hold_commits_once'] = result['seek_count'] == 1 and len(result['plays']) == 1
    else:
        checks[name+'_cancelled'] = result['cancelled'] == 1 and result['seek_count'] == 0
print(json.dumps(checks, indent=2))
(out / 'verification.json').write_text(json.dumps(checks, indent=2))
assert all(checks.values()), checks
