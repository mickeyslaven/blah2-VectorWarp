#!/usr/bin/env python3
"""Disposable systemd acceptance; never mounts devices or modifies host services."""
import os
from pathlib import Path
import re
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
IMAGE = 'registry.fedoraproject.org/fedora@sha256:c1e938afd5dfd7f172fac9168ba8e148fdf41c0517c04849072a15e4bd34eac6'


def main():
    name = 'vectorwarp-kraken-kit-' + uuid.uuid4().hex
    image = 'localhost/' + name
    evidence = ROOT / 'build/kraken-sourcekit-evidence'
    evidence.mkdir(parents=True, exist_ok=True)
    profile = os.environ.get('VECTORWARP_TEST_APPARMOR_PROFILE', 'unconfined')
    assert profile == 'unconfined' or re.fullmatch(r'vectorwarp-fedora-ci-[0-9a-f]{32}', profile)
    created = False
    try:
        subprocess.run(['podman', 'build', '--build-arg', 'BASE_IMAGE=' + IMAGE, '-t', image,
            '-f', str(ROOT / 'test/packaging/Containerfile.kraken-sourcekit'), str(ROOT / 'test/packaging')], check=True)
        subprocess.run(['podman', 'run', '-d', '--name', name, '--systemd=always', '--cgroupns=private',
            '--cap-add=SYS_ADMIN', '--security-opt=label=disable', '--security-opt=apparmor=' + profile,
            '--label=vectorwarp-ci-apparmor=' + profile, '--cpus=2', '--memory=4g', '--memory-swap=4g',
            '--pids-limit=512', '-v', str(ROOT) + ':/src:ro', image], check=True)
        created = True
        with (evidence / 'acceptance.log').open('w') as log:
            subprocess.run(['podman', 'exec', '-e', 'VECTORWARP_DISPOSABLE_TEST_CONTAINER=1', name,
                'python3', '/src/test/packaging/kraken_sourcekit_systemd.py'], stdout=log, stderr=subprocess.STDOUT,
                check=True, timeout=1700)
    finally:
        if created:
            with (evidence / 'journal.log').open('w') as log:
                subprocess.run(['podman', 'exec', name, 'journalctl', '--no-pager', '-u',
                    'vectorwarp-kraken-build.service'], stdout=log, stderr=subprocess.STDOUT, check=False)
            subprocess.run(['podman', 'stop', '--time', '5', name], check=False)
            subprocess.run(['podman', 'rm', name], check=True)
        subprocess.run(['podman', 'rmi', image], check=False)


if __name__ == '__main__':
    main()
