#!/usr/bin/env python3
"""Decode real controlled MP4/DASH/FLV media in the production PSP player."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
build = Path(os.environ.get('BILI_HEADLESS_CLIENT_DIR', root / 'build/playback-test'))
package = Path(os.environ.get('BILI_HEADLESS_PACKAGE', root / 'build/playback-test-dist/PSP/GAME/WiliMini'))
out = Path(os.environ.get('BILI_PLAYBACK_RESULTS', root / 'results/playback-community'))
media = root / 'build/playback-fixtures'
backend = os.environ.get('BILI_HEADLESS_PLAYER', 'psp-native')
if backend not in ('psp-native', 'ppsspp-sceMpeg'):
    raise ValueError('BILI_HEADLESS_PLAYER must be psp-native or ppsspp-sceMpeg')
out.mkdir(parents=True, exist_ok=True)
media.mkdir(parents=True, exist_ok=True)
(root / 'build/headless').mkdir(parents=True, exist_ok=True)

def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', *args], check=True)

ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=15',
       '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
       '-t', '8', '-c:v', 'libx264', '-profile:v', 'baseline', '-preset', 'ultrafast',
       '-g', '30', '-pix_fmt', 'yuv420p', '-b:v', '300k', '-c:a', 'aac', '-ac', '2',
       '-movflags', '+faststart', str(media / 'muxed.mp4'))
ffmpeg('-i', str(media / 'muxed.mp4'), '-c', 'copy', '-an', str(media / 'video.mp4'))
ffmpeg('-i', str(media / 'muxed.mp4'), '-c', 'copy', '-vn', str(media / 'audio.m4a'))
ffmpeg('-i', str(media / 'audio.m4a'), '-t', '4', '-c', 'copy', str(media / 'short-audio.m4a'))
ffmpeg('-i', str(media / 'muxed.mp4'), '-c', 'copy', '-f', 'flv', str(media / 'live.flv'))
ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=24',
       '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
       '-t', '4', '-c:v', 'libx264', '-profile:v', 'high', '-g', '24', '-bf', '3',
       '-pix_fmt', 'yuv420p', '-b:v', '400k', '-c:a', 'aac', '-ac', '2',
       '-movflags', '+faststart', str(media / 'high.mp4'))

cases = {
    'muxed-resume': dict(video='muxed.mp4', mode='file', start_ms=3000),
    'dash-resume': dict(video='video.mp4', audio='audio.m4a', mode='split', start_ms=3000),
    'flv-live': dict(video='live.flv', mode='live', start_ms=0),
    'muxed-cancel': dict(video='muxed.mp4', mode='file', start_ms=3000),
    # Native ME publishes asynchronously; its first-frame callback means queued,
    # so a screenshot at that callback is only an HLE decoder check.
    'high-bframes': dict(video='high.mp4', mode='source', start_ms=0,
                        capture_frame=backend == 'ppsspp-sceMpeg'),
    'repeat': dict(video='muxed.mp4', mode='file', start_ms=0, repeat=3),
    'short-audio': dict(video='video.mp4', audio='short-audio.m4a', mode='split', start_ms=0),
}
checks = {}
for name, config in cases.items():
    memstick = Path(tempfile.mkdtemp(prefix=name+'-', dir=root/'build/headless'))
    game = memstick / 'PSP/GAME/WiliMini'
    shutil.copytree(package, game, ignore=shutil.ignore_patterns('cache', 'session.json', 'results'))
    for file in media.iterdir():
        shutil.copy2(file, game / file.name)
    (game/'playback-test.json').write_text(json.dumps(config))
    env = dict(os.environ, BILI_HEADLESS_CLIENT_DIR=str(build), BILI_HEADLESS_PACKAGE=str(package),
               BILI_HEADLESS_MEMSTICK=str(memstick), BILI_HEADLESS_TIMEOUT='180',
               BILI_HEADLESS_TIMEOUT_EMULATED='40' if name == 'repeat' else '24',
               BILI_HEADLESS_SCREENSHOT_AT=f'4:{out}/{name}.png')
    if name.endswith('cancel'):
        env['BILI_HEADLESS_PRESS_SCRIPT'] = '4:0x2000:0.2'
    with (out/(name+'.log')).open('w') as log:
        subprocess.run(['sh', str(root/'tools/ppsspp/run-client-headless.sh')], env=env, check=True,
                       stdout=log, stderr=subprocess.STDOUT)
    result = json.loads((game/'playback-test-result.json').read_text())
    summaries = [json.loads(p.read_text()) for p in sorted((game/'results').glob('run-*/summary.json'))]
    summary = summaries[0]
    (out/(name+'.json')).write_text(json.dumps(dict(result=result, summary=summary, memstick=str(memstick)), indent=2))
    checks[name+'_decode'] = result['code'] == 0 and result['frames'] > 0 and summary['audio_error'] == 0
    checks[name+'_backend'] = all(s.get('backend') == backend for s in summaries)
    if backend == 'psp-native':
        checks[name+'_me_jobs'] = all(s.get('me_render') == 2 and
            s.get('me_submitted', 0) > 0 and s.get('me_completed') == s.get('me_submitted')
            for s in summaries)
    checks[name+'_present'] = summary.get('rgb_presented', result['frames']) > 0 and summary['audio_submitted_samples'] > 0
    if config['start_ms']:
        checks[name+'_resume'] = 3_000_000 <= summary['first_video_us'] < 3_100_000 and 3_000_000 <= summary['audio_first_pts_us'] < 3_100_000
    if name.endswith('cancel'):
        checks[name+'_cancel'] = result['cancelled'] == 1 and 3000 < result['position_ms'] < 7000
    else:
        checks[name+'_eof'] = result['eof'] == 1 and result['position_ms'] >= (3900 if name == 'high-bframes' else 7800)
    if name == 'repeat':
        checks[name+'_cleanup'] = len(result['plays']) == len(summaries) == 3 and all(
            p['code'] == 0 and p['frames'] > 0 and p['eof'] == 1 for p in result['plays'])
        checks[name+'_heap_released'] = all(p['heap_after'] <= p['heap_before']
            for p in result['plays'][1:])
    if config.get('capture_frame'):
        frame = (game/'playback-first-frame.ppm').read_bytes()
        pixels = frame.split(b'\n', 3)[3]
        checks[name+'_pixels'] = len(pixels) == 480 * 272 * 3 and len(set(pixels)) > 32
        shutil.copy2(game/'playback-first-frame.ppm', out/(name+'.ppm'))
(out/'verification.json').write_text(json.dumps(checks, indent=2))
print(json.dumps(checks, indent=2))
assert all(checks.values()), checks
