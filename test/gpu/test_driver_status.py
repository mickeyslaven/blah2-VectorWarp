"""Diagnostic worker ABI/escaping/fault tests; no Vulkan or physical GPU."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

worker = pathlib.Path(sys.argv[1])


def invoke(executable=worker, extra=None, argument='--driver-status'):
    env = dict(os.environ)
    env.update(extra or {})
    result = subprocess.run([str(executable), argument], env=env,
        text=True, capture_output=True, timeout=5)
    return result.returncode, json.loads(result.stdout) if result.stdout else None


code, status = invoke()
assert code == 0 and status['qualification'] == 'not-run'
assert status['devices'][0]['name'] == 'fixture?"GPU\\'
assert len(status['devices'][0]['bounded']) == 240
for environment in ['VW_FIXTURE_DRIVER_THROW', 'VW_FIXTURE_DRIVER_OVERSIZE']:
    code, status = invoke(extra={environment: '1'})
    assert code == 1 and status['available'] is False and status['qualification'] == 'not-run'
    assert len(status['error']) <= 240 and '\n' not in status['error']
with tempfile.TemporaryDirectory(prefix='vectorwarp-driver-status-fixture-') as directory:
    linked = pathlib.Path(directory) / 'linked-gpu-worker'
    linked.symlink_to(worker.resolve())
    code, status = invoke(linked)
    assert code == 0 and status['devices'][0]['name'] == 'fixture?"GPU\\'
    isolated = pathlib.Path(directory) / 'blah2-gpu-worker'
    shutil.copy2(worker, isolated)
    code, status = invoke(isolated)
    assert code == 1 and not status['available'] and status['devices'] == []
assert invoke(argument='--unknown')[0] == 2
print('Driver diagnostics: six offline ABI/fault/escaping/symlink cases PASS; no hardware opened')
