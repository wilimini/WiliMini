#!/usr/bin/env python3
"""Extract encrypted ME images from the standard PSP 6.61 updater."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
UPDATER_SHA256 = 'dc23a6dabdaed40bbfaad811d1f170346b911ba9750478fe5928d9f10c94f552'
TOOL_URL = 'https://github.com/John-K/pspdecrypt.git'
TOOL_REV = 'c156627db7634d395c380c0a9589130f603307fc'
UPDATER_URL = 'https://archive.org/download/psp_ofw_firmwares/PSP/661.PBP'
IMAGES = ('meimg.img', 'me_t2img.img')
IMAGE_SHA256 = {
    'meimg.img': '127d3a38c7df24f2aaf3221da5959cb663f024512b4bd7c56cbc69b34c14733e',
    'me_t2img.img': 'e7f5d4bfd3e0086e887d54d19fb85802385a7f40e4086649a1ebd58a664f99f6',
}


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--pbp', type=Path, help='local standard PSP 6.61 update PBP (not PSP Go)')
    source.add_argument('--url', default=UPDATER_URL, help='HTTPS updater URL (default: Internet Archive PSP 6.61 mirror)')
    parser.add_argument('--pspdecrypt', type=Path, help='existing host pspdecrypt executable; skip build')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/me-firmware/flash0')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error(f'output already exists: {output}; choose a new directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='me-firmware-', dir=output.parent) as temporary:
        work = Path(temporary)
        pbp = args.pbp.resolve() if args.pbp else work / '661.PBP'
        if not args.pbp:
            if not args.url.startswith('https://'):
                parser.error('--url requires HTTPS')
            print('Downloading updater...', flush=True)
            with urllib.request.urlopen(args.url, timeout=60) as response, pbp.open('wb') as target:
                if not response.url.startswith('https://'):
                    raise ValueError('updater redirected to a non-HTTPS URL')
                shutil.copyfileobj(response, target)
        if digest(pbp) != UPDATER_SHA256:
            raise ValueError('updater SHA-256 mismatch; expected the standard PSP 6.61 updater')
        if args.pspdecrypt:
            tool = args.pspdecrypt.resolve()
        else:
            checkout = work / 'pspdecrypt'
            run('git', 'clone', TOOL_URL, checkout)
            run('git', '-C', checkout, 'checkout', '--detach', TOOL_REV)
            run('git', '-C', checkout, 'submodule', 'update', '--init', '--recursive')
            # Homebrew OpenSSL is keg-only; use its reported prefix, never a fixed machine path.
            flags = []
            if sys.platform == 'darwin' and shutil.which('brew'):
                prefix = subprocess.run(['brew', '--prefix', 'openssl@3'], capture_output=True, text=True)
                if prefix.returncode == 0:
                    openssl = Path(prefix.stdout.strip())
                    flags = [f'CPPFLAGS=-I{openssl / "include"}', f'EXTRA_FLAG=-L{openssl / "lib"}']
            run('make', '-j', str(min(os.cpu_count() or 1, 8)), *flags, cwd=checkout)
            tool = checkout / 'pspdecrypt'
        extracted = work / 'extracted'
        # Keep ME images encrypted: PPSSPP performs their decryption at boot.
        run(tool, '-A', '-e', '-O', extracted, pbp)
        staged = work / 'flash0'
        resources = staged / 'kd/resource'
        resources.mkdir(parents=True)
        hashes = {}
        for name in IMAGES:
            image = extracted / 'F0/kd/resource' / name
            if digest(image) != IMAGE_SHA256[name]:
                raise ValueError(f'{name} SHA-256 mismatch')
            shutil.copyfile(image, resources / name)
            hashes[name] = digest(image)
        (staged / 'source.json').write_text(json.dumps({
            'firmware': '6.61', 'updater_sha256': UPDATER_SHA256,
            'extractor': str(tool) if args.pspdecrypt else TOOL_URL,
            'extractor_revision': None if args.pspdecrypt else TOOL_REV,
            'images_sha256': hashes,
        }, indent=2) + '\n')
        staged.rename(output)
    print(f'Firmware ready: {output}')
    print(f'BILI_HEADLESS_FIRMWARE_DIR="{output}" sh tools/ppsspp/run-client-headless.sh')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'error: {error}')
