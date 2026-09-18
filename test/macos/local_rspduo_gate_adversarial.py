#!/usr/bin/env python3
"""Run against an explicitly prepared macOS local-kit artifact; no hardware."""
import json, os, pathlib, shutil, subprocess, sys

root = pathlib.Path(os.environ['VECTORWARP_LOCAL_RSP_ROOT']).resolve()
state = pathlib.Path(os.environ['VECTORWARP_LOCAL_RSP_STATE']).resolve()
binary = root / 'bin/blah2'; current = state / 'adapters/rspduo/current'
env = dict(os.environ, VECTORWARP_MACOS_ROOT=str(root), VECTORWARP_MACOS_STATE=str(state))
def status():
    value = json.loads(subprocess.check_output([binary, '--receiver-status'], text=True, env=env))
    return next(item for item in value['receivers'] if item['receiver'] == 'RspDuo')
def rejected(label):
    result = status()
    assert not result['moduleLoadable'], (label, result)

assert status()['moduleLoadable']
generation = current.resolve(); receipt = generation / 'receipt.json'; original = receipt.read_bytes()
try:
    for value in (b'{', b'null', b'[]', b'{"schema":2}'):
        receipt.write_bytes(value); rejected('malformed receipt')
    data = json.loads(original)
    for key in ('module_sha256', 'core_sha256'):
        changed = dict(data); changed[key] = '0' * 64; receipt.write_text(json.dumps(changed)); rejected(key)
    changed = dict(data); changed['sdk'] = dict(data['sdk']); changed['sdk']['library'] = '0' * 64; receipt.write_text(json.dumps(changed)); rejected('sdk library hash')
finally:
    receipt.write_bytes(original)
assert status()['moduleLoadable']
print('macOS local RSPduo gate adversarial checks passed')
