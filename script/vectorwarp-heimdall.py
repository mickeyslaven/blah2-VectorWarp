#!/usr/bin/env python3
"""Run only the companion shipped in this release; never search PATH for Heimdall."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import secrets
import sys


def verify(root):
    receipt = json.loads((root / 'share/heimdall/build.json').read_text())
    if (receipt.get('schema') != 2 or receipt.get('driverLinkage') != 'pinned-static' or
            receipt.get('protocol') != 'MCHT-v1;MCHQ-v2'):
        raise ValueError('This release lacks a verified Linux Kraken companion')
    expected = receipt.get('artifacts', {}).get('bin/heimdall')
    binary = root / 'bin/heimdall'
    if not expected or hashlib.sha256(binary.read_bytes()).hexdigest() != expected:
        raise ValueError('Bundled Heimdall does not match its build receipt')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'run'])
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--node', required=True)
    parser.add_argument('--state-dir', type=Path, default=Path('/run/vectorwarp-heimdall'))
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    adapter = runpy.run_path(str(root / 'share/heimdall/controller.py'))
    # Check applicability before verifying the companion: replay/other receivers
    # do not acquire USB merely because the processor wants this service.
    import subprocess
    expression = "const r=require(process.argv[1]).readConfig(process.argv[2]);if(r.readError)throw Error(r.readError);process.stdout.write(JSON.stringify(r.config.capture));"
    capture = json.loads(subprocess.check_output([args.node, '-e', expression,
        str(root / 'api/config-store.js'), str(args.config)], timeout=5))
    if (capture.get('replay', {}).get('state') is not False or
            capture.get('device', {}).get('type') != 'Kraken' or
            capture.get('device', {}).get('heimdall', {}).get('host') != '127.0.0.1'):
        return 1 if args.action == 'check' else 0
    verify(root)
    profile = adapter['read_profile'](args.config, root, args.node)
    if args.action == 'check':
        return 0
    # Ignore ambient executable overrides; immutable release is authoritative.
    os.environ.update(VECTORWARP_MACOS_ROOT=str(root), VECTORWARP_MACOS_NODE=args.node,
        VECTORWARP_MACOS_HEIMDALL_EXECUTABLE=str(root / 'bin/heimdall'),
        VECTORWARP_MACOS_HEIMDALL_HTML=str(root / 'share/heimdall/index.html'))
    return adapter['main'](['run-local-v1', '--config', str(args.config),
        '--bind-host', '127.0.0.1', '--state-dir', str(args.state_dir),
        '--ready-file', str(args.state_dir / 'ready.json'), '--instance-token', secrets.token_hex(16),
        '--iq-port', str(profile['iq']), '--control-port', str(profile['control'])])


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print(f'vectorwarp-heimdall: {error}', file=sys.stderr)
        sys.exit(255)
