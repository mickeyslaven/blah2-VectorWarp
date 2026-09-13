#!/usr/bin/env python3
"""Bind the local adapter kit to the final RPM core, after all distro BRP steps.

Build-time only: never install or execute receiver software. The finished RPM
is independently checked again by package-native.sh.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def finalize(buildroot):
    root = Path(buildroot).resolve(strict=True)
    if root == Path('/'):
        raise ValueError('a separate RPM build root is required')

    def inside(relative):
        path = (root / relative).resolve(strict=True)
        path.relative_to(root)
        if not path.is_file():
            raise ValueError(f'not a regular staged file: {relative}')
        return path

    metadata = inside('opt/vectorwarp/PACKAGE-METADATA')
    if metadata.stat().st_size > 16384:
        raise ValueError('oversized package metadata')
    fields = dict(line.split('=', 1) for line in metadata.read_text().splitlines() if '=' in line)
    kit_relative = 'opt/vectorwarp/current/receiver-source/rspduo/kit.json'
    if fields.get('backend') == 'open-test' and fields.get('test_only') == 'true':
        if (root / kit_relative).exists():
            raise ValueError('test-only package unexpectedly contains a local RSPduo kit')
        return
    if (fields.get('backend'), fields.get('test_only'), fields.get('local_build_receivers')) != (
            'all', 'false', 'RspDuo'):
        raise ValueError('unexpected package receiver profile')
    kit = inside(kit_relative)
    if kit.stat().st_size > 1024 * 1024:
        raise ValueError('oversized local RSPduo kit metadata')
    value = json.loads(kit.read_text(encoding='utf-8'))
    if (not isinstance(value, dict) or type(value.get('schema')) is not int or value['schema'] != 1 or
            value.get('receiver') != 'RspDuo' or not isinstance(value.get('sources'), dict) or
            not value['sources'] or not isinstance(value.get('core_sha256'), str) or
            not re.fullmatch(r'[0-9a-f]{64}', value['core_sha256'])):
        raise ValueError('invalid local RSPduo kit metadata')
    for name, expected in value['sources'].items():
        if not isinstance(name, str) or Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('invalid local kit source path')
        source = inside(str(Path(kit_relative).parent / name))
        source.relative_to(kit.parent)
        if digest(source) != expected:
            raise ValueError(f'RPM processing changed a local kit source: {name}')
    core = inside('opt/vectorwarp/current/bin/libblah2-capture-core.so.1')
    value['core_sha256'] = digest(core)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=kit.parent,
                                         prefix='.kit-', delete=False) as stream:
            temporary = Path(stream.name)
            os.fchmod(stream.fileno(), stat.S_IMODE(kit.stat().st_mode))
            json.dump(value, stream, sort_keys=True, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, kit)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print('Final RPM core bound to local RSPduo kit after distro post-processing')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: finalize-rspduo-kit.py RPM_BUILD_ROOT')
    try:
        finalize(sys.argv[1])
    except (OSError, ValueError, TypeError) as error:
        raise SystemExit(f'finalize-rspduo-kit: {error}')
