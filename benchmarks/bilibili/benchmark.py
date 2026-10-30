#!/usr/bin/env python3
"""Frozen Bilibili popular corpus; stream copy only, no credentials stored."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import urllib.parse
import urllib.request
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
UA = 'Mozilla/5.0'
REFERER = 'https://www.bilibili.com/'


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')


def api(endpoint, **params):
    url = 'https://api.bilibili.com/' + endpoint + '?' + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={'User-Agent': UA, 'Referer': REFERER})
    with urllib.request.urlopen(req, timeout=40) as response:
        data = json.load(response)
    if data['code'] != 0:
        raise RuntimeError(f"{endpoint}: API code {data['code']}")
    return data['data']


def command(args):
    # Errors can contain signed CDN URLs: never persist them in the corpus.
    try:
        proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f'{args[0]} timed out') from None
    if proc.returncode:
        raise RuntimeError(f'{args[0]} failed (exit {proc.returncode})')
    return proc.stdout


def ffmpeg(*args):
    return command(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-y', *map(str, args)])


def remote_cut(url, start, duration, output, kind):
    ffmpeg('-user_agent', UA, '-headers', 'Referer: ' + REFERER + '\r\n',
           '-ss', f'{start:.6f}', '-i', url, '-t', str(duration),
           '-map', f'0:{kind}:0', '-c', 'copy', output)


def probe(path):
    return json.loads(command(['ffprobe', '-v', 'error', '-show_format', '-show_streams',
                              '-count_packets', '-of', 'json', str(path)]))


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def media_info(path):
    p = probe(path)
    s = p['streams'][0]
    return dict(file=path.name, bytes=path.stat().st_size, sha256=sha256(path),
                duration=float(p['format']['duration']),
                stream={k: s[k] for k in ('codec_name', 'profile', 'width', 'height', 'pix_fmt',
                        'r_frame_rate', 'avg_frame_rate', 'sample_rate', 'channels', 'nb_read_packets',
                        'start_time', 'duration', 'time_base', 'has_b_frames') if k in s})


def trim_video_tail(path):
    # Stream-copy -t uses DTS. B-frame reordering otherwise leaks pictures past
    # ten seconds. Keep a decoding-order prefix, ending before the first PTS>=10;
    # never remove an interior reference packet needed by retained B pictures.
    packets = json.loads(command(['ffprobe', '-v', 'error', '-show_packets',
        '-show_entries', 'packet=pts_time', '-of', 'json', str(path)]))['packets']
    count = next((i for i, p in enumerate(packets) if float(p['pts_time']) >= 10), len(packets))
    trimmed = path.with_name('trimmed.mp4')
    ffmpeg('-i', path, '-map', '0:v:0', '-frames:v', str(count), '-c', 'copy', trimmed)
    trimmed.replace(path)


def validate_clip(folder, item):
    for kind in ('video', 'audio'):
        expected = item[kind]
        path = folder / expected['file']
        if sha256(path) != expected['sha256']:
            raise RuntimeError(f'{item["id"]}: {kind} checksum mismatch')
        info = media_info(path)
        if not 9.9 <= info['duration'] <= 10.1:
            raise RuntimeError(f'{item["id"]}: {kind} duration {info["duration"]}')
        s = info['stream']
        if kind == 'video':
            if s['codec_name'] != 'h264' or s['pix_fmt'] != 'yuv420p' or s['width'] > 1280 or s['height'] > 720:
                raise RuntimeError('Unsupported video')
        elif s['codec_name'] != 'aac' or s['channels'] != 2 or int(s['sample_rate']) not in (44100, 48000):
            raise RuntimeError('Unsupported audio')
        # Decode the entire extracted stream, including startup references.
        ffmpeg('-xerror', '-i', path, '-f', 'null', '-')
    packets = json.loads(command(['ffprobe', '-v', 'error', '-select_streams', 'v',
        '-show_packets', '-show_entries', 'packet=pts_time,flags', '-of', 'json',
        str(folder / item['video']['file'])]))['packets']
    if not packets or 'K' not in packets[0]['flags'] or abs(float(packets[0]['pts_time'])) > 0.1:
        raise RuntimeError('Clip must begin with a keyframe at timestamp zero')


def collect(args):
    args.data.mkdir(parents=True, exist_ok=True)
    snapshot_path = args.snapshot
    if not snapshot_path.exists():
        popular = api('x/web-interface/popular', ps=50, pn=1)['list']
        write_json(snapshot_path, dict(captured_at=datetime.now(timezone.utc).isoformat(),
            endpoint='https://api.bilibili.com/x/web-interface/popular?ps=50&pn=1',
            selection='first 20 compatible entries from frozen candidates; first video page',
            videos=[dict(rank=i + 1, bvid=v['bvid'], aid=v['aid'], cid=v['cid'],
                title=v['title'], list_duration=v['duration']) for i, v in enumerate(popular)]))
    snapshot = json.loads(snapshot_path.read_text())
    if len(snapshot['videos']) < 20 or len({v['bvid'] for v in snapshot['videos']}) != len(snapshot['videos']):
        raise RuntimeError('Snapshot must contain 20 distinct videos')
    items, failures, excluded = [], [], []
    for v in snapshot['videos']:
        if len(items) == 20:
            break
        case_id = f'{v["rank"]:02d}-{v["bvid"]}'
        folder = args.data / case_id
        folder.mkdir(exist_ok=True)
        metadata = folder / 'metadata.json'
        try:
            if metadata.exists():
                item = json.loads(metadata.read_text())
                validate_clip(folder, item)
            else:
                # View metadata gives first-page duration, unlike a multipart list total.
                page = api('x/web-interface/view', bvid=v['bvid'])['pages'][0]
                if page['cid'] != v['cid']:
                    raise RuntimeError('First-page CID changed since snapshot')
                dash = api('x/player/playurl', bvid=v['bvid'], cid=page['cid'],
                           qn=16, fnval=16, fnver=0, fourk=0)['dash']
                video = next(x for x in dash['video'] if x['id'] == 16 and x['codecs'].startswith('avc1'))
                audio = min((x for x in dash['audio'] if x['codecs'].startswith('mp4a')), key=lambda x: x['bandwidth'])
                desired = max(0, (page['duration'] - 10) / 2)
                window = max(0, desired - 12)
                with tempfile.TemporaryDirectory(dir=folder) as temp:
                    temp_video = Path(temp) / 'window.mp4'
                    remote_cut(video.get('baseUrl', video.get('base_url')), window, 36, temp_video, 'v')
                    frames = json.loads(command(['ffprobe', '-v', 'error', '-select_streams', 'v',
                        '-skip_frame', 'nokey', '-show_frames', '-show_entries', 'frame=pts_time',
                        '-of', 'json', str(temp_video)]))['frames']
                    keys = [float(f['pts_time']) for f in frames if float(f['pts_time']) >= 0]
                    key = min(keys, key=lambda t: abs(window + t - desired))
                    start = window + key
                    ffmpeg('-ss', f'{key:.6f}', '-i', temp_video, '-t', '10',
                           '-map', '0:v:0', '-c', 'copy', folder / 'video.mp4')
                    trim_video_tail(folder / 'video.mp4')
                    remote_cut(audio.get('baseUrl', audio.get('base_url')), start, 10, folder / 'audio.m4a', 'a')
                item = dict(id=case_id, **v, page_cid=page['cid'], page_duration=page['duration'],
                    requested_start_s=desired, source_start_s=start, requested_duration_s=10,
                    midpoint_offset_s=start-desired, selection=dict(video_id=video['id'],
                    video_codec=video['codecs'], video_bandwidth=video['bandwidth'],
                    audio_id=audio['id'], audio_codec=audio['codecs'], audio_bandwidth=audio['bandwidth']),
                    video=media_info(folder / 'video.mp4'), audio=media_info(folder / 'audio.m4a'))
                validate_clip(folder, item)
                write_json(metadata, item)
            items.append(item)
            print(f'{case_id}: OK {item["video"]["duration"]:.3f}s / {item["audio"]["duration"]:.3f}s', flush=True)
        except Exception as error:
            if str(error) == 'Unsupported audio':
                excluded.append(dict(id=case_id, reason='Original DASH audio is not stereo AAC at 44.1/48 kHz; no transcoding'))
                print(case_id + ': excluded (unsupported original audio)', flush=True)
                continue
            failures.append(dict(id=case_id, error=type(error).__name__ + ': ' + str(error)))
            print(f'{case_id}: FAILED {type(error).__name__}: {error}', flush=True)
    manifest = dict(schema=1, captured_at=snapshot['captured_at'],
                    extraction='H.264 360p + lowest-bandwidth AAC DASH; stream copy, keyframe near midpoint',
                    ffmpeg=command(['ffmpeg', '-version']).decode().splitlines()[0],
                    complete=len(items) == 20, items=items, excluded=excluded, failures=failures)
    write_json(args.data / 'manifest.json', manifest)
    # A reviewable lock with hashes and durable public video IDs; no expiring CDN URLs.
    write_json(HERE / 'corpus.lock.json', manifest)
    if failures or len(items) != 20:
        raise RuntimeError(f'{len(items)}/20 samples ready; rerun collect to retry')


def verify(args):
    manifest = json.loads((args.data / 'manifest.json').read_text())
    if not manifest['complete'] or len(manifest['items']) != 20:
        raise RuntimeError('Incomplete corpus')
    for item in manifest['items']:
        validate_clip(args.data / item['id'], item)
        print(item['id'] + ': verified', flush=True)


def stage(args):
    manifest = json.loads((args.data / 'manifest.json').read_text())
    if not manifest['complete'] or len(manifest['items']) != 20:
        raise RuntimeError('Incomplete corpus')
    if args.destination.exists():
        raise RuntimeError('Destination must be new (keeps existing results/account data intact)')
    if not (args.package / 'EBOOT.PBP').is_file():
        raise RuntimeError('Missing complete benchmark package')
    shutil.copytree(args.package, args.destination,
                    ignore=shutil.ignore_patterns('cache', 'results', 'session.json', 'wifi-profile.txt'))
    cases = []
    for item in manifest['items']:
        folder = args.destination / 'benchmark' / item['id']
        folder.mkdir(parents=True)
        for kind in ('video', 'audio'):
            src = args.data / item['id'] / item[kind]['file']
            if sha256(src) != item[kind]['sha256']:
                raise RuntimeError(f'Checksum mismatch: {src}')
            shutil.copy2(src, folder / src.name)
        cases.append(dict(id=item['id'], video=f'benchmark/{item["id"]}/video.mp4',
                          audio=f'benchmark/{item["id"]}/audio.m4a'))
    shutil.copy2(args.data / 'manifest.json', args.destination / 'benchmark-manifest.json')
    write_json(args.destination / 'benchmark-build.json', dict(
        files={p.name: sha256(p) for p in args.package.iterdir() if p.is_file()},
        device_runner_sha256=sha256(HERE / 'device_runner.hpp')))
    write_json(args.destination / 'benchmark.json', dict(cases=cases, repeat=args.repeat,
        profile=args.profile, buffer_frames=args.buffers, environment=args.environment))
    print(args.destination)


def run(args):
    args.exit_at = args.exit_at or (20 * args.repeat * 15 + 30)
    args.output = args.output.resolve()
    args.destination = args.output / 'memstick/PSP/GAME/WiliMini'
    args.package = args.package.resolve()
    stage(args)
    env = dict(os.environ, PPSSPP_HEADLESS=str(args.runner.resolve()),
        BILI_HEADLESS_CLIENT_DIR=str(args.build.resolve()), BILI_HEADLESS_PACKAGE=str(args.package),
        BILI_HEADLESS_MEMSTICK=str(args.output / 'memstick'), BILI_HEADLESS_MODEL=args.model,
        BILI_HEADLESS_PLAYER='psp-native', BILI_HEADLESS_FIRMWARE_DIR=str(args.firmware.resolve()),
        BILI_HEADLESS_TIMEOUT=str(args.timeout), BILI_HEADLESS_TIMEOUT_EMULATED=str(args.exit_at + 15),
        BILI_HEADLESS_PRESS_SCRIPT=f'{args.exit_at}:0x8:0.2',
        BILI_HEADLESS_SCREENSHOT_AT=f'7:{args.output}/playback.png', BILI_HEADLESS_LOG='0')
    # Clear inherited profiler switches so baseline and profiling are explicit.
    env.pop('BILI_HEADLESS_JIT_PROFILE', None)
    env.pop('BILI_HEADLESS_CPU', None)
    if args.jit_profile:
        env.update(BILI_HEADLESS_CPU='jit-ir', BILI_HEADLESS_JIT_PROFILE=str(args.output / 'jit-profile.txt'))
    write_json(args.output / 'host-environment.json', dict(
        runner_sha256=sha256(args.runner), runner_version=command([str(args.runner), '--version']).decode().strip(),
        launcher_sha256=sha256(ROOT / 'tools/ppsspp/run-client-headless.sh'),
        device_runner_sha256=sha256(HERE / 'device_runner.hpp'),
        executable_sha256={p.name: sha256(p) for p in args.package.glob('*.prx')},
        config=json.loads((args.destination / 'benchmark.json').read_text()),
        jit_profile=args.jit_profile, exit_at=args.exit_at, timeout=args.timeout))
    with (args.output / 'emulator.log').open('w') as log:
        subprocess.run(['sh', str(ROOT / 'tools/ppsspp/run-client-headless.sh')],
                       env=env, stdout=log, stderr=subprocess.STDOUT, check=True,
                       timeout=args.timeout + 30)
    timed_out = 'TIMEOUT (' in (args.output / 'emulator.log').read_text(errors='replace')
    write_json(args.output / 'execution.json', dict(timed_out=timed_out, completed_process=True))
    args.game = args.destination
    args.output = args.output / 'report'
    report(args)
    if timed_out:
        raise RuntimeError('Emulator timed out; inspect logs and increase --exit-at/--timeout')


def report(args):
    records = [json.loads(line) for line in (args.game / 'benchmark-results/index.jsonl').read_text().splitlines()]
    rows, hot = [], []
    for record in records:
        base = args.game / record['log_root'] / 'results'
        summaries = list(base.glob('run-*/summary.json'))
        if len(summaries) != 1:
            raise RuntimeError(f'Expected one summary for {record["id"]}')
        path = summaries[0]
        s = json.loads(path.read_text())
        valid = (record['code'] == 0 and record['eof'] == 1 and not record['cancelled']
                 and s['error_code'] == 0 and s.get('audio_error', -1) == 0 and s['frames'] > 0
                 and s.get('audio_eof') == 1 and s.get('audio_submitted_samples', 0) > 0
                 and s.get('backend') == 'psp-native' and s.get('me_render') == 2
                 and s.get('me_submitted', 0) > 0 and s.get('me_submitted') == s.get('me_completed')
                 and not any(s.get(k, 0) for k in ('me_mismatches', 'cabac_test_failures',
                            'qpel_integer_test_failures', 'packed_chroma_test_failures',
                            'approx_qpel_test_failures', 'nearest_chroma_test_failures', 'audio_vfpu_failures'))
                 and not s.get('log_truncated') and not s.get('packet_records_truncated'))
        rows.append(dict(id=record['id'], repeat=record['repeat'], valid=valid,
            backend=s['backend'], profiling=s['profiling'], frames=s['frames'],
            cabac_movn=s.get('cabac_movn', 0), me_cache=s.get('me_cache', 0),
            me_full_cache=s.get('me_full_cache', 0),
            me_render_queue=s.get('me_render_queue', 0),
            integer_unaligned_copy=s.get('integer_unaligned_copy', 0),
            unrolled_avg=s.get('unrolled_avg', 0),
            wall_us=record['wall_us'], decode_us=s['decode_us'], render_us=s['render_us'],
            measured_us=s['measured_us'], media_us=s['media_us'],
            presented=s.get('rgb_presented', ''), dropped=s.get('rgb_dropped', ''),
            catchup_packets=s.get('client_catchup_packets', 0),
            audio_decode_us=s.get('audio_decode_us', 0), audio_convert_us=s.get('audio_convert_us', 0),
            audio_underruns=s.get('audio_underruns', 0), rgb_mean_late_us=s.get('rgb_mean_late_us', 0)))
        profile = path.parent / 'profile.csv'
        if s['profiling'] and (not profile.is_file() or not (path.parent / 'packets.csv').is_file()):
            rows[-1]['valid'] = False
        if profile.exists():
            for counter in csv.DictReader(profile.open()):
                calls, samples = int(counter['calls']), int(counter['samples'])
                if samples:
                    # Timings overlap across inclusive parents/children; never sum into a pie chart.
                    estimate = max(0, int(counter['sampled_us']) - samples * int(counter['timer_pair_us'])) * calls / samples
                    hot.append(dict(id=record['id'], repeat=record['repeat'], stage=counter['stage'],
                                    samples=samples, estimated_us=round(estimate, 2)))
    medians = []
    for case_id in sorted({r['id'] for r in rows}):
        group = [r for r in rows if r['id'] == case_id]
        medians.append(dict(id=case_id, plays=len(group), valid=all(r['valid'] for r in group),
            **{'median_' + k: statistics.median(r[k] for r in group)
               for k in ('wall_us', 'decode_us', 'render_us', 'frames', 'catchup_packets')}))
    args.output.mkdir(parents=True, exist_ok=True)
    for filename, data in [('scores.csv', rows), ('medians.csv', medians), ('hotspots.csv', sorted(hot, key=lambda x: -x['estimated_us']))]:
        if data:
            with (args.output / filename).open('w') as f:
                writer = csv.DictWriter(f, fieldnames=list(data[0]))
                writer.writeheader()
                writer.writerows(data)
        elif (args.output / filename).exists():
            (args.output / filename).unlink()
    config = json.loads((args.game / 'benchmark.json').read_text())
    expected = {(c['id'], i) for c in config['cases'] for i in range(1, config['repeat'] + 1)}
    actual = {(r['id'], r['repeat']) for r in rows}
    complete = (actual == expected and len(rows) == len(expected) and all(r['valid'] for r in rows)
                and all(bool(r['profiling']) == config['profile'] for r in rows))
    write_json(args.output / 'validation.json', dict(complete=complete, plays=len(rows),
               environment=config['environment'], valid=sum(r['valid'] for r in rows)))
    print(f'{len(rows)} plays, complete={complete}; {args.output}')
    if not complete:
        raise RuntimeError('Incomplete or failed benchmark')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    for name in ('collect', 'verify', 'stage', 'run'):
        p = sub.add_parser(name)
        p.add_argument('--data', type=Path, default=HERE / 'data')
        if name == 'collect':
            p.add_argument('--snapshot', type=Path, default=HERE / 'popular.snapshot.json')
        if name in ('stage', 'run'):
            p.add_argument('--package', type=Path, required=True)
            if name == 'stage':
                p.add_argument('--destination', type=Path, required=True)
            p.add_argument('--repeat', type=int, choices=range(1, 9), default=3)
            p.add_argument('--buffers', type=int, choices=(2, 4, 8), default=2)
            p.add_argument('--profile', action='store_true')
            p.add_argument('--environment', required=True, help='Model/CFW or emulator commit/config, storage, build flags')
        if name == 'run':
            p.add_argument('--build', type=Path, default=ROOT / 'build/benchmark')
            p.add_argument('--runner', type=Path, default=ROOT / 'build/ppsspp-headless/PPSSPPHeadless')
            p.add_argument('--firmware', type=Path, required=True)
            p.add_argument('--model', choices=('1000', '2000', '3000'), default='3000')
            p.add_argument('--output', type=Path, required=True)
            p.add_argument('--timeout', type=int, default=600, help='Wall-clock limit in seconds')
            p.add_argument('--exit-at', type=int, default=0, help='Emulated second to press START; default 20*repeat*15+30')
            p.add_argument('--jit-profile', action='store_true', help='Use jit-ir and export guest block profile')
    p = sub.add_parser('report')
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    globals()[args.action](args)


if __name__ == '__main__':
    main()
