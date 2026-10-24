#!/usr/bin/env python3
"""Stage our source-build instructions, never upstream code or binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil


def stage(source, output):
    contract = runpy.run_path(str(source / 'script/build-kraken-macos.py'))
    files = ['script/build-kraken.py', 'script/build-kraken-macos.py',
             'script/vectorwarp-build-sdrplay.py', 'config/kraken-macos-sources.json']
    files += ['script/patches/' + name for group in contract['PATCHES'].values() for name in group]
    hashes = {}
    for name in files:
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
        target.chmod(0o644)
        hashes[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    identity = hashlib.sha256(json.dumps(hashes, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    (output / 'kit.json').write_text(json.dumps(dict(schema=1, kit_id=identity, files=hashes,
        redistribution='build-instructions-only', protocol='MCHT-v1;MCHQ-v2'), indent=2) + '\n')
    return identity


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    stage(args.source, args.output)
