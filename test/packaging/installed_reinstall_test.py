#!/usr/bin/env python3
"""Exercise real package hooks with a legacy sudoers fixture, in a container only.

This is a same-version reinstall/migration test, not a claim to test every
historical package upgrade. The SDK is absent and no receiver is started.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import sys


def run(*args):
    result = subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180,
                            env=os.environ | {'DEBIAN_FRONTEND': 'noninteractive'})
    return result.stdout.strip()


assert os.geteuid() == 0 and Path('/run/.containerenv').exists()
package = Path(sys.argv[1])
assert package in (Path('/tmp/package.deb'), Path('/tmp/package.rpm')) and package.is_file()
config = Path('/etc/vectorwarp/config.yml')
before = hashlib.sha256(config.read_bytes()).hexdigest()
# Do not repair a broken install hook by starting the API in the test. A fresh
# package must make its web interface available without an extra service command.
assert run('systemctl', 'is-enabled', 'vectorwarp-api.service') == 'enabled'
assert run('systemctl', 'is-active', 'vectorwarp-api.service') == 'active'
assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
pid = run('systemctl', 'show', '-p', 'MainPID', '--value', 'vectorwarp-api.service')
assert int(pid) > 0
sudoers = Path('/etc/sudoers.d/vectorwarp')
assert sudoers.is_file() and not sudoers.is_symlink()
preserved = '# Administrator note: preserve this customization.\nroot ALL=(ALL) ALL\n'
sudoers.write_text(preserved + 'Defaults:vectorwarp-api !requiretty\n'
    'vectorwarp-api ALL=(root) NOPASSWD: /usr/bin/systemctl start --no-block vectorwarp-sdrplay-build.service\n')
sudoers.chmod(0o440)
if package.suffix == '.deb':
    print(run('apt-get', '--yes', '--reinstall', '-o', 'Dpkg::Options::=--force-confold', 'install', str(package)))
else:
    print(run('dnf', '--assumeyes', 'reinstall', str(package)))
assert sudoers.read_text() == preserved, 'Retire the known grant, preserving the administrator content'
assert hashlib.sha256(config.read_bytes()).hexdigest() == before, 'Reinstall changed user configuration'
assert run('systemctl', 'show', '-p', 'MainPID', '--value', 'vectorwarp-api.service') == pid, \
    'Package unexpectedly interrupted a running API'
assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
print('PASS: fresh install starts only the API; reinstall retires the legacy grant, preserves config/customization and leaves processing stopped')
