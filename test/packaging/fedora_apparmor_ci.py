#!/usr/bin/env python3
"""Run one disposable Fedora CI test under its own temporary AppArmor profile.

The Ubuntu host's unix-chkpwd profile denies dac_override during Fedora PAM
account checks. This does not change that host profile or the installed package:
it keeps the already-unconfined outer CI test OS in one named profile across exec.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid


PROFILE_ENV = 'VECTORWARP_TEST_APPARMOR_PROFILE'


def apparmor_enabled():
    marker = Path('/sys/module/apparmor/parameters/enabled')
    try:
        return marker.read_text().strip().upper().startswith('Y')
    except FileNotFoundError:
        return False


def profile_source(name):
    # AppArmor 4.0's default_allow is implemented as unconfined; that mode
    # still attaches a matching host executable profile on exec. Use a normal
    # enforce-mode profile with broad permissions equivalent to the already
    # unconfined disposable OUTER CI test OS, and explicitly inherit on exec.
    # This cannot add kernel capabilities beyond Podman's existing bounding set.
    return (f'abi <abi/4.0>,\nprofile {name} '
            'flags=(attach_disconnected,mediate_deleted) {\n'
            '  file,\n  network,\n  capability,\n  mount,\n  umount,\n'
            '  pivot_root,\n  ptrace,\n  signal,\n  unix,\n  dbus,\n'
            '  userns,\n  mqueue,\n  io_uring,\n  /** ix,\n}\n')


def run_child(command, environment):
    return subprocess.run(command, env=environment, check=False).returncode


def run_with_profile(command):
    if os.geteuid() != 0:
        raise RuntimeError('Run explicitly with sudo on an ephemeral CI runner')
    environment = os.environ.copy()
    environment.pop(PROFILE_ENV, None)
    if not apparmor_enabled():
        return run_child(command, environment)
    if not Path('/usr/sbin/apparmor_parser').is_file():
        raise RuntimeError('AppArmor is enabled, but /usr/sbin/apparmor_parser is missing')
    name = 'vectorwarp-fedora-ci-' + uuid.uuid4().hex
    temp_root = os.environ.get('RUNNER_TEMP') or None
    with tempfile.TemporaryDirectory(prefix='vectorwarp-apparmor-', dir=temp_root) as directory:
        profile_path = Path(directory) / 'profile'
        profile_path.write_text(profile_source(name), encoding='utf-8')
        profile_path.chmod(0o600)
        # -Q checks syntax without loading policy. -a errors on an existing
        # name rather than replacing any host profile. -K avoids host cache writes.
        subprocess.run(['/usr/sbin/apparmor_parser', '-Q', '-K', str(profile_path)], check=True)
        subprocess.run(['/usr/sbin/apparmor_parser', '-a', '-K', str(profile_path)], check=True)
        environment[PROFILE_ENV] = name
        child_status = 1
        try:
            child_status = run_child(command, environment)
        finally:
            # The child fixture owns its container. Never unload this profile
            # while one of its exact, uniquely labeled containers still exists.
            # A query failure also leaves the profile loaded on the disposable
            # runner so an unknown live container cannot lose its policy.
            remaining = subprocess.run(
                ['podman', 'ps', '-aq', '--filter',
                 f'label=vectorwarp-ci-apparmor={name}'],
                check=True, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE)
            if remaining.stdout.strip():
                raise RuntimeError('CI test container still exists; retaining its '
                                   'temporary AppArmor profile until runner teardown')
            subprocess.run(['/usr/sbin/apparmor_parser', '-R', '-K', str(profile_path)],
                           check=True)
        return child_status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('child command required after --')
    try:
        return run_with_profile(command)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f'Fedora CI AppArmor profile failed: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
