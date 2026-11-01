#!/usr/bin/env python3
"""Exercise the installed SDRplay compiler child under its real systemd policy.

This is deliberately an installed-package regression test.  It does not read an
SDRplay SDK, contact hardware, or publish an adapter.  A root-owned fixture
compiler records the child state produced by ``compile_module`` then invokes the
host C++ compiler on a tiny independent source file.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import pwd
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import uuid


def fail(message: str) -> None:
    raise AssertionError(message)


def run(argv, *, check=True, timeout=180):
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    if check and result.returncode:
        fail(f"command failed ({result.returncode}): {' '.join(argv)}\n{result.stdout[-8000:]}")
    return result


def installed_unit() -> Path:
    result = run(['systemctl', 'show', '--property=FragmentPath', '--value',
                  'vectorwarp-sdrplay-build.service'])
    path = Path(result.stdout.strip())
    if not path.is_file():
        fail(f'installed SDRplay build unit was not found: {path}')
    return path


def service_properties(unit: Path, work: Path) -> list[str]:
    """Carry every service policy setting into a transient test service.

    ExecStart and unit Conditions are intentionally not copied.  The test
    supplies its fixture command and cannot depend on a proprietary SDK kit.
    ReadWritePaths is intentionally narrowed to this unique /run directory.
    """
    section = None
    values: list[str] = []
    for raw in unit.read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        if not line or line.startswith(('#', ';')):
            continue
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1]
            continue
        if section != 'Service' or '=' not in line:
            continue
        key, value = line.split('=', 1)
        if key in ('ExecStart', 'ReadWritePaths'):
            continue
        values.append(f'{key}={value}')
    if not any(value.startswith('NoNewPrivileges=') for value in values):
        fail('SDRplay build unit has no NoNewPrivileges policy')
    if not any(value.startswith('CapabilityBoundingSet=') for value in values):
        fail('SDRplay build unit has no capability bounding policy')
    if not any(value == 'AmbientCapabilities=CAP_SETUID' for value in values):
        fail('SDRplay build unit does not explicitly carry CAP_SETUID ambiently')
    values.append(f'ReadWritePaths={work}')
    return values


RUNNER = r'''
import importlib.util, json, os, pathlib, pwd, sys

work = pathlib.Path(sys.argv[1])
helper_path = work / 'vectorwarp-build-sdrplay.py'
spec = importlib.util.spec_from_file_location('vectorwarp_sdrplay_fixture', helper_path)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
user = pwd.getpwnam(sys.argv[2])
kit = work / 'kit'
for source in ('src/capture/ReceiverFactory.cpp', 'src/capture/rspduo/RspDuo.cpp'):
    path = kit / source
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('// fixture; compiler shim deliberately does not consume this SDK source\n')
(kit / 'generated').mkdir(parents=True, exist_ok=True)
(work / 'include').mkdir(exist_ok=True)
(work / 'fake-core').write_bytes(b'fixture')
(work / 'fake-sdk').write_bytes(b'fixture')
build = work / 'build'
os.chown(build, user.pw_uid, user.pw_gid)
helper.KIT = kit
helper.INCLUDE = work / 'include'
helper.CORE = work / 'fake-core'
helper.LIBRARY = work / 'fake-sdk'
helper.COMPILER = sys.argv[3]
helper.BUILD_USER = sys.argv[2]
manifest = {'compiler': {'cxx_flags': []}}
try:
    helper.compile_module(manifest, {}, build / 'module.so')
    helper.validate_output(build / 'module.so')
except Exception as error:
    (work / 'runner-error.txt').write_text(f'{type(error).__name__}: {error}\n')
    raise
'''


SHIM = r'''#!/usr/bin/python3
import json, os, pathlib, resource, sys
work = pathlib.Path(%(work)r)
user = %(user)r
status = {}
for line in pathlib.Path('/proc/self/status').read_text().splitlines():
    key, _, value = line.partition(':')
    if key in ('CapPrm', 'CapEff', 'CapAmb', 'NoNewPrivs', 'Seccomp'):
        status[key] = value.strip()
record = {
    'uid': os.getresuid(), 'gid': os.getresgid(), 'groups': os.getgroups(),
    'limits': {name: resource.getrlimit(value) for name, value in (
        ('CPU', resource.RLIMIT_CPU), ('AS', resource.RLIMIT_AS),
        ('FSIZE', resource.RLIMIT_FSIZE), ('NPROC', resource.RLIMIT_NPROC))},
    'status': status, 'argv': sys.argv[1:],
}
(work / 'build' / 'child.json').write_text(json.dumps(record, sort_keys=True))
output = sys.argv[sys.argv.index('-o') + 1]
os.execv('/usr/bin/c++', ['/usr/bin/c++', '-shared', '-fPIC', str(work / 'tiny.cpp'), '-o', output])
'''


def write_fixtures(work: Path, helper: Path, build_user: str, compiler: Path) -> None:
    copied = work / 'vectorwarp-build-sdrplay.py'
    shutil.copyfile(helper, copied)
    copied.chmod(0o755)
    for path in (copied, work):
        os.chown(path, 0, 0)
    work.chmod(0o755)
    (work / 'runner.py').write_text(textwrap.dedent(RUNNER), encoding='utf-8')
    (work / 'tiny.cpp').write_text('extern "C" int vectorwarp_fixture() { return 7; }', encoding='utf-8')
    (work / 'build').mkdir()
    shim = compiler
    shim.write_text(SHIM % {'work': str(work), 'user': build_user}, encoding='utf-8')
    for path in (work / 'runner.py', work / 'tiny.cpp', shim):
        os.chown(path, 0, 0)
        path.chmod(0o755 if path == shim else 0o644)


def invoke(unit: str, properties: list[str], work: Path, user: str, compiler: Path,
           *, ambient: bool, cpu_limit: str | None) -> subprocess.CompletedProcess:
    args = ['systemd-run', '--wait', '--collect', f'--unit={unit}']
    for value in properties:
        if not ambient and value.startswith(('AmbientCapabilities=', 'CapabilityBoundingSet=')):
            continue
        args.extend(('--property', value))
    if not ambient:
        args.extend(('--property', 'AmbientCapabilities='))
        args.extend(('--property', 'CapabilityBoundingSet=CAP_CHOWN CAP_FOWNER CAP_SETGID CAP_KILL'))
    if cpu_limit:
        args.extend(('--property', f'LimitCPU={cpu_limit}'))
    args.extend(('/usr/bin/python3', '-B', '-I', str(work / 'runner.py'), str(work), user,
                 str(compiler)))
    return run(args, check=False)


def journal(unit: str) -> str:
    result = run(['journalctl', '--no-pager', '--output=short-precise', '-b', '-n', '60', '-u', unit],
                 check=False, timeout=5)
    return result.stdout[-12000:]


def unit_state(unit: str) -> str:
    result = run(['systemctl', 'show', '--property=LoadState', '--property=ActiveState',
                  '--property=MainPID', unit], check=False, timeout=5)
    return result.stdout.strip() or f'exit={result.returncode}'


def account_threads(uid: int) -> int:
    """Avoid running a setuid regression against an unexpectedly busy account."""
    count = 0
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            status = (entry / 'status').read_text()
            real_uid = next(line.split()[1] for line in status.splitlines() if line.startswith('Uid:'))
            if int(real_uid) == uid:
                count += len(list((entry / 'task').iterdir()))
        except (FileNotFoundError, PermissionError, StopIteration, IndexError):
            continue
    return count


def assert_success(work: Path, build_user: str, expected_cpu: int) -> None:
    child = json.loads((work / 'build/child.json').read_text(encoding='utf-8'))
    account = pwd.getpwnam(build_user)
    if child['uid'] != [account.pw_uid] * 3 or child['gid'] != [account.pw_gid] * 3:
        fail(f'compiler child identity was not fully dropped: {child}')
    if child['groups'] != []:
        fail(f'compiler child retained supplementary groups: {child["groups"]}')
    for key in ('CapPrm', 'CapEff', 'CapAmb'):
        if int(child['status'].get(key, '1'), 16) != 0:
            fail(f'compiler child retained {key}: {child["status"]}')
    if child['status'].get('NoNewPrivs') != '1' or child['status'].get('Seccomp') != '2':
        fail(f'compiler child did not retain systemd NNP/seccomp policy: {child["status"]}')
    limits = child['limits']
    if tuple(limits['CPU']) != (expected_cpu, expected_cpu):
        fail(f'compiler CPU limit was not capped to inherited ceiling: {limits["CPU"]}')
    expected = {'AS': (1024 * 1024 * 1024,) * 2, 'FSIZE': (32 * 1024 * 1024,) * 2,
                'NPROC': (32, 32)}
    for name, value in expected.items():
        if tuple(limits[name]) != value:
            fail(f'compiler {name} limit mismatch: {limits[name]}')
    elf = run(['/usr/bin/readelf', '-h', str(work / 'build/module.so')]).stdout
    if 'Type:' not in elf or 'DYN (Shared object file)' not in elf:
        fail('fixture compiler did not produce a shared ELF')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--helper', type=Path, default=Path('/opt/vectorwarp/libexec/vectorwarp-build-sdrplay'))
    parser.add_argument('--unit', type=Path)
    parser.add_argument('--build-user', default='vectorwarp-build')
    args = parser.parse_args()
    if sys.platform != 'linux' or os.geteuid() != 0:
        fail('sdrplay child setup test must run explicitly as root on Linux')
    if platform.machine() not in ('x86_64', 'aarch64', 'arm64'):
        fail(f'unsupported Linux architecture for SDRplay child setup test: {platform.machine()}')
    if not args.helper.is_file():
        fail(f'installed SDRplay build helper was not found: {args.helper}')
    account = pwd.getpwnam(args.build_user)
    if account.pw_uid == 0 or account.pw_gid == 0:
        fail('SDRplay child setup test requires a non-root build user and group')
    before_threads = account_threads(account.pw_uid)
    if before_threads >= 30:
        fail(f'build user {args.build_user} already has {before_threads} real-UID threads')
    unit_file = args.unit or installed_unit()
    if not unit_file.is_file():
        fail(f'SDRplay build unit was not found: {unit_file}')
    work = Path(tempfile.mkdtemp(prefix='vectorwarp-sdrplay-test-', dir='/run'))
    base = 'vectorwarp-sdrplay-test-' + uuid.uuid4().hex
    compiler = Path('/usr/local/bin') / (base + '-cxx')
    logs: list[str] = []
    units: list[str] = []
    problem: BaseException | None = None
    cleaned = False
    identity = 'machine={} helper={} helper_sha256={} unit={} unit_sha256={} build_user={} prior_realuid_threads={}'.format(
        platform.machine(), args.helper, hashlib.sha256(args.helper.read_bytes()).hexdigest(),
        unit_file, hashlib.sha256(unit_file.read_bytes()).hexdigest(), args.build_user, before_threads)
    try:
        write_fixtures(work, args.helper, args.build_user, compiler)
        properties = service_properties(unit_file, work)
        normal = base + '-normal.service'
        units.append(normal)
        result = invoke(normal, properties, work, args.build_user, compiler, ambient=True, cpu_limit=None)
        logs.append(f'{normal} rc={result.returncode} state-after-collect={unit_state(normal)}')
        if result.returncode:
            fail(f'ambient-capability compiler run failed:\n{logs[-1]}')
        assert_success(work, args.build_user, 90)
        shutil.rmtree(work / 'build')
        (work / 'build').mkdir()
        cpu = base + '-cpu.service'
        units.append(cpu)
        result = invoke(cpu, properties, work, args.build_user, compiler, ambient=True, cpu_limit='10:30')
        logs.append(f'{cpu} rc={result.returncode} state-after-collect={unit_state(cpu)}')
        if result.returncode:
            fail(f'stricter inherited CPU compiler run failed:\n{logs[-1]}')
        assert_success(work, args.build_user, 10)
        shutil.rmtree(work / 'build')
        (work / 'build').mkdir()
        negative = base + '-without-setuid.service'
        units.append(negative)
        result = invoke(negative, properties, work, args.build_user, compiler, ambient=False, cpu_limit=None)
        negative_log = journal(negative)
        logs.append(f'{negative} rc={result.returncode} state-after-collect={unit_state(negative)}')
        if result.returncode == 0 or not re.search(r'compiler child setup failed at setuid', negative_log):
            fail(f'CAP_SETUID-removal run failed for the wrong reason:\n{logs[-1]}')
        if (work / 'build/child.json').exists() or (work / 'build/module.so').exists():
            fail('CAP_SETUID-removal run reached the compiler fixture')
        logs.append('normal ambient and inherited CPU 10:30 child setup passed; CAP_SETUID removal failed at setuid')
    except BaseException as error:
        problem = error
    finally:
        cleanup_errors = []
        for unit in units:
            try:
                run(['systemctl', 'stop', unit], check=False, timeout=5)
                run(['systemctl', 'reset-failed', unit], check=False, timeout=5)
                state = unit_state(unit)
                logs.append(f'cleanup {unit}: {state}')
                fields = dict(line.split('=', 1) for line in state.splitlines() if '=' in line)
                gone = fields.get('LoadState') == 'not-found'
                stopped = fields.get('ActiveState') == 'inactive' and fields.get('MainPID') == '0'
                if not (gone and stopped):
                    cleanup_errors.append(f'{unit} remained live: {state}')
                logs.append(f'final journal {unit}:\n{journal(unit)}')
            except BaseException as error:
                cleanup_errors.append(f'{unit} cleanup failed: {error}')
        if not cleanup_errors:
            try:
                shutil.rmtree(work)
                if work.exists():
                    cleanup_errors.append(f'workdir survived cleanup: {work}')
                else:
                    cleaned = True
            except BaseException as error:
                cleanup_errors.append(f'workdir cleanup failed: {work}: {error}')
        try:
            compiler.unlink(missing_ok=True)
        except BaseException as error:
            cleanup_errors.append(f'compiler fixture cleanup failed: {compiler}: {error}')
        if cleanup_errors:
            logs.extend('cleanup error: ' + error for error in cleanup_errors)
            if problem is None:
                problem = AssertionError('; '.join(cleanup_errors))
        for log in logs:
            print(log)
        print(identity)
        print(f'cleanup workdir={work} removed={cleaned} compiler_removed={not compiler.exists()}')
    if problem is not None:
        raise problem


if __name__ == '__main__':
    main()
