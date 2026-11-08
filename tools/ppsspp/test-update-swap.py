#!/usr/bin/env python3
"""Run the loader's update swap and the client's commit under PPSSPP (issue #26).

Stages a fake update in <memstick>/PSP/GAME/WiliMini/pending/ the way the client
does (files + MANIFEST last), starts the production package through EBOOT's
loader, then checks the memstick. Two scenarios:

  apply     new files are swapped in, old ones kept as .bak, the client reaches its
            UI and commits (update.state and pending/MANIFEST disappear)
  rollback  a staged update whose trial boots are used up is rolled back

PPSSPP is not a PSP: this checks the PSP file API calls through the emulator,
not real Memory Stick behaviour. Needs PPSSPPHeadless and a built package.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
out = Path(os.environ.get('BILI_UPDATE_RESULTS', root / 'results/update-swap'))
out.mkdir(parents=True, exist_ok=True)
(root / 'build/headless').mkdir(parents=True, exist_ok=True)
package = Path(os.environ.get('BILI_HEADLESS_PACKAGE', root / 'dist/PSP/GAME/WiliMini'))

# Files the fake update replaces. EBOOT.PBP carries the same bytes: it is the
# file the loader cannot replace while running, which is the point of the test.
MARK_NEW = b'{"update-test":"new"}\n'


def read(path):
    return path.read_bytes() if path.exists() else None


def prepare(name):
    memstick = Path(tempfile.mkdtemp(prefix='update-' + name + '-', dir=root / 'build/headless'))
    subprocess.run([sys.executable, str(root / 'tools/ppsspp/seed-ui-fixture.py'), str(memstick)], check=True)
    game = memstick / 'PSP/GAME/WiliMini'
    shutil.copytree(package, game, dirs_exist_ok=True)
    return memstick, game


def stage(game, files):
    """What stageRelease() leaves behind: every file under pending/, MANIFEST last."""
    for rel, data in files.items():
        target = game / 'pending' / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (game / 'pending/MANIFEST').write_text(''.join(rel + '\n' for rel in files))


def launch(memstick, name):
    shot = out / (name + '.png')
    shot.unlink(missing_ok=True)
    modal = out / (name + '-notice.png')
    modal.unlink(missing_ok=True)
    env = dict(os.environ, BILI_HEADLESS_MEMSTICK=str(memstick), BILI_HEADLESS_PLAYER='ppsspp-sceMpeg',
               BILI_HEADLESS_TIMEOUT='200', BILI_HEADLESS_LOG='1',
               # The update message waits for a button: look at it, press confirm, look again.
               BILI_HEADLESS_PRESS_SCRIPT='20:0x4000:0.2',
               BILI_HEADLESS_SCREENSHOT_AT=f'12:{modal},26:{shot}')
    with (out / (name + '.log')).open('w') as handle:
        subprocess.run(['sh', str(root / 'tools/ppsspp/run-client-headless.sh')], env=env,
                       stdout=handle, stderr=subprocess.STDOUT)


failures = []


def check(cond, message):
    print(('ok     ' if cond else 'FAILED ') + message)
    if not cond:
        failures.append(message)


# ---- apply ----
memstick, game = prepare('apply')
old_build = read(game / 'build.json')
old_eboot = read(game / 'EBOOT.PBP')
stage(game, {'EBOOT.PBP': old_eboot, 'build.json': MARK_NEW, 'extra/new-file.txt': b'hello\n'})
launch(memstick, 'apply')
check(not (game / 'pending/build.json').exists(), 'apply: staged files removed after the copy')
check(read(game / 'build.json') == MARK_NEW, 'apply: build.json replaced by the staged file')
check(read(game / 'build.json.bak') == old_build, 'apply: old build.json kept as build.json.bak')
check(read(game / 'EBOOT.PBP.bak') is not None, 'apply: EBOOT.PBP was swapped (EBOOT.PBP.bak exists)')
check(read(game / 'extra/new-file.txt') == b'hello\n', 'apply: new file in a new directory installed')
check(read(game / 'settings.json') is not None, 'apply: settings.json untouched')
check(not (game / 'pending/MANIFEST').exists(), 'apply: pending/MANIFEST removed after commit')
check(not (game / 'update.state').exists(), 'apply: update.state removed after the client committed')
check(not (game / 'update.notice').exists(), 'apply: update.notice deleted once the message was dismissed')
check(not (game / 'update.failed').exists(), 'apply: no update.failed')
print('--- apply: update.log ---')
print((read(game / 'update.log') or b'(none)').decode(errors='replace'))
shutil.rmtree(memstick, ignore_errors=True)

# ---- rollback: a version that already used its trial boots ----
memstick, game = prepare('rollback')
old_build = read(game / 'build.json')
stage(game, {'build.json': MARK_NEW})
# State after an install whose client never committed: new file live, old one in .bak.
shutil.move(game / 'build.json', game / 'build.json.bak')
(game / 'pending/build.json').rename(game / 'build.json')
(game / 'build.json').write_bytes(MARK_NEW)
(game / 'build.json.bak').write_bytes(old_build)
(game / 'update.state').write_text('TRIAL 3\n')
launch(memstick, 'rollback')
check(read(game / 'build.json') == old_build, 'rollback: previous build.json restored')
check(read(game / 'update.failed') is not None, 'rollback: update.failed written')
check(not (game / 'update.state').exists() and not (game / 'pending/MANIFEST').exists(), 'rollback: state cleaned up')
check(not (game / 'update.notice').exists(), 'rollback: update.notice deleted once the message was dismissed')
print('--- rollback: update.log ---')
print((read(game / 'update.log') or b'(none)').decode(errors='replace'))
shutil.rmtree(memstick, ignore_errors=True)

if failures:
    sys.exit('update swap test failed: ' + '; '.join(failures))
print('update swap test passed')
