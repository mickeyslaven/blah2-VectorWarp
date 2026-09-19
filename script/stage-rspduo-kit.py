#!/usr/bin/env python3
"""Stage only the allowlisted VectorWarp RSPduo source kit, never a vendor SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

SOURCES = {
    'src/capture/ReceiverFactory.cpp', 'src/capture/ReceiverModule.h',
    'src/capture/Source.h', 'src/capture/Recording.h', 'src/capture/PairedCpiQueue.h', 'src/capture/PairedCpiSource.h',
    'src/capture/kraken/HeimdallFrame.h', 'src/data/IqData.h',
    'src/capture/rspduo/RspDuo.cpp', 'src/capture/rspduo/RspDuo.h',
    'src/capture/rspduo/SampleSequence.h', 'src/capture/rspduo/SdkSampleClock.h',
    'src/capture/rspduo/UsbMode.h', 'LICENSE', 'generated/ReceiverCohort.h',
}


def sha256(path):
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(65536), b''):
            digest.update(block)
    return digest.hexdigest()


def stage(source, generated, core, output):
    plan = json.loads((generated / 'rspduo-plan.json').read_text())
    if set(plan['sources']) != SOURCES:
        raise ValueError('RSPduo source kit must contain exactly the reviewed source allowlist')
    if output.exists():
        raise ValueError('RSPduo kit destination already exists; stage in a fresh artifact')
    originals = {}
    for name, expected in plan['sources'].items():
        path = generated / 'ReceiverCohort.h' if name == 'generated/ReceiverCohort.h' else source / name
        if path.is_symlink() or not path.is_file() or sha256(path) != expected:
            raise ValueError(f'RSPduo kit source changed or is not a regular file: {name}')
        originals[name] = path
    plan['core_sha256'] = sha256(core)
    output.mkdir(parents=True)
    for name, path in originals.items():
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
        destination.chmod(0o644)
        if sha256(destination) != plan['sources'][name]:
            raise ValueError(f'RSPduo kit copy failed verification: {name}')
    (output / 'kit.json').write_text(json.dumps(plan, indent=2, sort_keys=True) + '\n')
    return plan


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'generated', 'core', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    stage(args.source, args.generated, args.core, args.output)
