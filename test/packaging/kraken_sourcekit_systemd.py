#!/usr/bin/env python3
"""Destructive fixture setup ONLY inside an explicitly disposable no-device container."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    assert os.environ.get('VECTORWARP_DISPOSABLE_TEST_CONTAINER') == '1'
    assert os.geteuid() == 0
    subprocess.run(['systemd-detect-virt', '--container'], check=True)
    assert not Path('/dev/bus/usb').exists(), 'Never run this fixture with receiver access'
    prefix = Path('/opt/vectorwarp')
    assert not prefix.exists(), 'Fixture requires a clean container'
    current = prefix / 'current'
    (prefix / 'libexec').mkdir(parents=True)
    (current / 'share/heimdall').mkdir(parents=True)
    stage = runpy.run_path(str(ROOT / 'script/stage-kraken-kit.py'))['stage']
    identity = stage(ROOT, current / 'receiver-source/kraken')
    for name, destination in (
        ('vectorwarp-build-kraken.py', prefix / 'libexec/vectorwarp-build-kraken'),
        ('vectorwarp-build-sdrplay.py', prefix / 'libexec/vectorwarp-build-sdrplay'),
        ('vectorwarp', prefix / 'libexec/vectorwarp'),
        ('vectorwarp-heimdall.py', current / 'share/heimdall/service.py')):
        destination.write_text((ROOT / 'script' / name).read_text().replace('@PREFIX@', str(prefix)).replace('@SYSCONFDIR@', '/etc/vectorwarp'))
        destination.chmod(0o755)
    Path('/etc/sysusers.d').mkdir(exist_ok=True)
    Path('/etc/tmpfiles.d').mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'contrib/systemd/vectorwarp.sysusers', '/etc/sysusers.d/vectorwarp.conf')
    shutil.copy2(ROOT / 'contrib/systemd/vectorwarp.tmpfiles', '/etc/tmpfiles.d/vectorwarp.conf')
    unit = Path('/etc/systemd/system/vectorwarp-kraken-build.service')
    unit.write_text((ROOT / 'contrib/systemd/vectorwarp-kraken-build.service.in').read_text().replace('@PREFIX@', str(prefix)))
    for command in (['systemd-sysusers'], ['systemd-tmpfiles', '--create'], ['systemctl', 'daemon-reload']):
        subprocess.run(command, check=True)
    subprocess.run(['/usr/bin/python3', '-I', str(prefix / 'libexec/vectorwarp'), 'build-kraken'], check=True, timeout=1650)
    companion = runpy.run_path(str(current / 'share/heimdall/service.py'))['companion_root'](current)
    assert str(companion).startswith('/var/lib/vectorwarp-adapters/kraken/' + identity + '/generations/')
    assert companion.stat().st_uid == 0
    assert (companion / 'bin/heimdall').stat().st_uid == 0
    receipt = json.loads((companion / 'share/heimdall/build.json').read_text())
    assert receipt['hardwareTested'] is False
    assert receipt['artifacts']['bin/heimdall'] == hashlib.sha256((companion / 'bin/heimdall').read_bytes()).hexdigest()
    assert not list(Path('/var/lib/vectorwarp-adapters/kraken').glob('.build-*'))
    # Wrong kit must fail without touching the retained, known-good generation.
    pins = current / 'receiver-source/kraken/config/kraken-macos-sources.json'
    original = pins.read_bytes(); pins.write_bytes(b'{}')
    failed = subprocess.run(['systemctl', 'start', 'vectorwarp-kraken-build.service'])
    assert failed.returncode != 0
    pins.write_bytes(original)
    assert runpy.run_path(str(current / 'share/heimdall/service.py'))['companion_root'](current) == companion
    assert not Path('/dev/bus/usb').exists()
    print(json.dumps(dict(ok=True, kit_id=identity, companion=str(companion),
        sourceReceipt=receipt, hardwareTested=False, checks=['real-systemd-sandbox',
        'unprivileged-compiler', 'root-owned-publication', 'kit-mismatch-fails', 'prior-generation-retained', 'no-usb']), indent=2))


if __name__ == '__main__':
    main()
