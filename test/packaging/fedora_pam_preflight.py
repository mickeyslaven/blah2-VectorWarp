#!/usr/bin/env python3
"""CI-only, package-free probe for Fedora sudo/PAM on the Ubuntu Podman host.

No host account, PAM, shadow, capability, or AppArmor policy is modified.
Only the disposable test container receives a user and a narrowly scoped rule.
"""

import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid


IMAGE = ('registry.fedoraproject.org/fedora@sha256:'
         'c1e938afd5dfd7f172fac9168ba8e148fdf41c0517c04849072a15e4bd34eac6')
ROOT = Path(__file__).resolve().parents[2]
HELPER = re.compile(r'unix[-_]chkpwd', re.IGNORECASE)
POLICY_LINE = re.compile(r'\b(capability|include|profile|unix[-_]chkpwd)\b', re.IGNORECASE)
CI_PROFILE = re.compile(r'vectorwarp-fedora-ci-[0-9a-f]{32}\Z')


def command(args, *, output=None, check=True):
    """Run a fixed-argument command; keep build/diagnostic output in evidence."""
    if output is None:
        result = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False, timeout=180)
        if check and result.returncode:
            raise RuntimeError(f'{args[0]} failed (exit {result.returncode}): '
                               f'{result.stdout[-2000:]}')
        return result
    with output.open('w', encoding='utf-8') as stream:
        result = subprocess.run(args, text=True, stdout=stream,
                                stderr=subprocess.STDOUT, check=False, timeout=600)
    if check and result.returncode:
        raise RuntimeError(f'{args[0]} failed (exit {result.returncode}); see {output}')
    return result


def build_args(image_name):
    return ['podman', 'build', '--jobs=1', '--memory=2g', '--memory-swap=2g',
            '--cpu-period=100000', '--cpu-quota=50000',
            '--build-arg', f'BASE_IMAGE={IMAGE}', '-t', image_name,
            '-f', str(ROOT / 'test/packaging/Containerfile.service-rpm'),
            str(ROOT / 'test/packaging')]


def run_args(container_name, image_name):
    profile = os.environ.get('VECTORWARP_TEST_APPARMOR_PROFILE')
    if profile and not CI_PROFILE.fullmatch(profile):
        raise RuntimeError('Invalid CI-only AppArmor profile name')
    apparmor_option = f'apparmor={profile}' if profile else 'apparmor=unconfined'
    profile_label = [f'--label=vectorwarp-ci-apparmor={profile}'] if profile else []
    return ['podman', 'run', '-d', '--name', container_name, *profile_label,
            '--systemd=always', '--cgroupns=private', '--cap-add=SYS_ADMIN',
            '--security-opt=label=disable', f'--security-opt={apparmor_option}',
            '--ulimit', 'core=-1:-1', '--cpus=.5', '--memory=2g',
            '--memory-swap=2g', '--pids-limit=512', image_name]


def helper_kernel_lines(raw):
    """Never persist unrelated kernel messages; bound matching output."""
    return [line[:500] for line in raw.splitlines() if HELPER.search(line)][-100:]


def host_diagnostics(evidence, started):
    """Read only helper-specific host policy and post-probe kernel records."""
    lines = [f'probe_started_local={started}',
             f'probe_ended_utc={datetime.now(timezone.utc).isoformat()}']
    for label, args in (
        ('kernel', ['uname', '-r']),
        ('podman', ['podman', '--version']),
        ('oci_runtime', ['podman', 'info', '--format',
                         '{{.Host.OCIRuntime.Name}} {{.Host.OCIRuntime.Version}}']),
        ('apparmor_parser', ['apparmor_parser', '--version']),
    ):
        try:
            result = command(args, check=False)
            lines.append(f'{label}: ' + (result.stdout.strip().splitlines()[0]
                                      if result.returncode == 0 and result.stdout.strip()
                                      else f'unavailable (exit {result.returncode})'))
        except (OSError, subprocess.TimeoutExpired) as exc:
            lines.append(f'{label}: unavailable ({exc.__class__.__name__})')
    for path in (Path('/sys/module/apparmor/parameters/enabled'),
                 Path('/sys/kernel/security/apparmor/profiles')):
        try:
            value = path.read_text(errors='replace')
            selected = value.strip() if path.name == 'enabled' else '\n'.join(
                line for line in value.splitlines() if HELPER.search(line))
            lines.append(f'{path}: {selected or "no helper profile listed"}')
        except OSError as exc:
            lines.append(f'{path}: unavailable ({exc.__class__.__name__})')
    for path in (Path('/etc/apparmor.d/unix-chkpwd'),
                 Path('/etc/apparmor.d/local/unix-chkpwd')):
        try:
            selected = [f'{number}: {line[:300]}'
                        for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1)
                        if POLICY_LINE.search(line)]
            lines.append(f'{path}:\n' + ('\n'.join(selected[:100]) or '(no matching lines)'))
        except OSError as exc:
            lines.append(f'{path}: unavailable ({exc.__class__.__name__})')
    for label, args in (
        ('journalctl', ['journalctl', '-k', '--since', started, '-n', '1000',
                        '--no-pager', '-o', 'cat']),
        ('dmesg', ['dmesg', '--since', started, '--time-format=iso', '--color=never']),
    ):
        try:
            result = command(args, check=False)
            if result.returncode:
                lines.append(f'{label}: unavailable (exit {result.returncode})')
                continue
            matches = helper_kernel_lines(result.stdout)
            lines.append(f'{label}:\n' + ('\n'.join(matches) or '(no helper records)'))
            if matches:
                break
        except (OSError, subprocess.TimeoutExpired) as exc:
            lines.append(f'{label}: unavailable ({exc.__class__.__name__})')
    (evidence / 'host-pam.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def exec_in(container_name, *args, output=None, check=True):
    return command(['podman', 'exec', container_name, *args], output=output, check=check)


def probe(evidence):
    if os.geteuid() != 0:
        raise RuntimeError('Run with sudo on a disposable CI runner')
    evidence.mkdir(parents=True, exist_ok=True)
    name = f'vectorwarp-fedora-pam-{uuid.uuid4().hex[:12]}'
    image_name = f'localhost/{name}'
    # journalctl and dmesg both parse this local-wall-time form. ISO timestamps
    # with offsets/microseconds are rejected by some util-linux dmesg versions.
    started = datetime.now().astimezone().strftime('%Y-%m-%d %H:%M:%S')
    container_created = False
    image_created = False
    try:
        command(build_args(image_name), output=evidence / 'base-image.log')
        image_created = True
        command(run_args(name, image_name), output=evidence / 'container-id.txt')
        container_created = True
        expected_profile = os.environ.get('VECTORWARP_TEST_APPARMOR_PROFILE')
        if expected_profile:
            actual_profile = exec_in(name, 'cat', '/proc/self/attr/apparmor/current').stdout.strip()
            (evidence / 'apparmor-label.txt').write_text(actual_profile + '\n', encoding='utf-8')
            if actual_profile != expected_profile + ' (enforce)':
                raise RuntimeError('Fedora test container did not enter its CI AppArmor profile')
        # The installed-package container boots systemd before package install.
        for _ in range(60):
            state = exec_in(name, 'timeout', '5', 'systemctl', 'is-system-running', check=False)
            if state.stdout.strip() in ('running', 'degraded'):
                break
            command(['sleep', '1'])
        else:
            raise RuntimeError('Fedora test container did not finish boot')
        exec_in(name, 'dnf', '--assumeyes', '--setopt=install_weak_deps=False',
                'install', 'sudo', 'shadow-utils', output=evidence / 'dependencies.log')
        exec_in(name, 'useradd', '--create-home', '--shell', '/bin/sh',
                'vectorwarp-package-test')
        exec_in(name, 'bash', '-euc',
                "printf '%s\\n' 'vectorwarp-package-test ALL=(root) NOPASSWD: /usr/bin/true' "
                '> /etc/sudoers.d/vectorwarp-package-test; '
                'chmod 0440 /etc/sudoers.d/vectorwarp-package-test; visudo -c',
                output=evidence / 'grant.log')
        uid = exec_in(name, 'id', '-u', 'vectorwarp-package-test').stdout.strip()
        gid = exec_in(name, 'id', '-g', 'vectorwarp-package-test').stdout.strip()
        if not (uid.isdecimal() and gid.isdecimal() and int(uid) > 0 and int(gid) > 0):
            raise RuntimeError('Disposable account has invalid numeric UID/GID')
        exec_in(name, 'bash', '-c',
                'stat -c "shadow mode=%a owner=%U group=%G" /etc/shadow; '
                'getent passwd vectorwarp-package-test; '
                'getent shadow vectorwarp-package-test >/dev/null; '
                'printf "shadow lookup exit=%s\\n" "$?"; '
                'stat -c "helper mode=%a owner=%U group=%G" /usr/sbin/unix_chkpwd; '
                'rpm -q sudo pam', output=evidence / 'container-account.txt')
        result = exec_in(name, 'setpriv', f'--reuid={uid}', f'--regid={gid}',
                         '--init-groups', '--reset-env', '/usr/bin/sudo', '-n',
                         '--', '/usr/bin/true', output=evidence / 'sudo-pam.log',
                         check=False)
        if result.returncode:
            raise RuntimeError(f'Normal-user Fedora sudo/PAM failed (exit {result.returncode}); '
                               f'see {evidence / "sudo-pam.log"}')
    finally:
        already_failing = sys.exc_info()[0] is not None
        try:
            host_diagnostics(evidence, started)
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            print(f'Host PAM diagnostics unavailable: {exc}', file=sys.stderr)
        cleanup = []
        if container_created:
            cleanup.extend((['podman', 'stop', '--time', '10', name],
                            ['podman', 'rm', name]))
        if image_created:
            cleanup.append(['podman', 'rmi', image_name])
        cleanup_errors = []
        for args in cleanup:
            try:
                result = command(args, check=False)
                if result.returncode:
                    cleanup_errors.append(f'{args[1]} exited {result.returncode}')
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
                cleanup_errors.append(f'{args[1]} unavailable: {exc}')
        if cleanup_errors:
            message = 'Fedora PAM preflight cleanup failed: ' + '; '.join(cleanup_errors)
            if already_failing:
                print(message, file=sys.stderr)
            else:
                raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', required=True, type=Path)
    args = parser.parse_args()
    try:
        probe(args.evidence)
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print(f'Fedora PAM preflight failed: {exc}', file=sys.stderr)
        return 1
    print(f'Fedora normal-user sudo/PAM preflight passed; evidence: {args.evidence}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
