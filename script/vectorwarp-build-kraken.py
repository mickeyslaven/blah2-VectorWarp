#!/usr/bin/python3 -I
"""Explicit local source build. No paths/URLs/flags are accepted from callers.

The pinned downloads and compiler run as a dedicated non-radio account. Only
bounded regular outputs are copied into immutable, root-owned generations.
Nothing starts a receiver or modifies a system driver. Failed builds leave the
previous generation intact; package rollback selects its own exact kit ID.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import resource
import runpy
import shutil
import signal
import stat
import sys
import tempfile

PREFIX = Path('@PREFIX@')
OUTROOT = Path('/var/lib/vectorwarp-adapters/kraken')
MAX_FILE = 32 * 1024 * 1024


def require(ok, message):
    if not ok:
        raise ValueError(message)


def kit_identity(kit):
    files = kit.get('files', {})
    require(kit.get('schema') == 1 and files and
            kit.get('redistribution') == 'build-instructions-only' and
            kit.get('protocol') == 'MCHT-v1;MCHQ-v2', 'Unsupported Kraken source kit')
    identity = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    require(kit.get('kit_id') == identity, 'Kraken kit identity mismatch')
    return identity


def copy_regular(source, target):
    """No symlink traversal, special files, unbounded data, or file races."""
    parent = source.parent
    while parent != parent.parent:
        require(not parent.is_symlink(), 'Build output ancestor is a symlink')
        parent = parent.parent
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and 0 < before.st_size <= MAX_FILE,
                'Invalid build output')
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as out:
            total = 0
            while True:
                block = os.read(fd, 65536)
                if not block:
                    break
                total += len(block)
                require(total <= before.st_size, 'Build output grew')
                out.write(block)
            out.flush(); os.fsync(out.fileno())
        after = os.fstat(fd)
        require(total == before.st_size and
                (before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'Build output changed')
        target.chmod(0o755 if target.name == 'heimdall' else 0o644)
    finally:
        os.close(fd)


def build():
    require(os.geteuid() == 0, 'Use vectorwarp build-kraken with administrator authentication')
    kitdir = PREFIX / 'current/receiver-source/kraken'
    # The bootstrap module is package-owned, not supplied by the caller or build.
    sandbox = runpy.run_path(str(PREFIX / 'libexec/vectorwarp-build-sdrplay'))
    trusted, sha = sandbox['trusted'], sandbox['sha']
    trusted(kitdir / 'kit.json')
    kit = sandbox['read_json'](kitdir / 'kit.json')
    identity = kit_identity(kit)
    for name, expected in kit['files'].items():
        require(not Path(name).is_absolute() and '..' not in Path(name).parts, 'Unsafe kit path')
        trusted(kitdir / name, symlinks=False)
        require(sha(kitdir / name) == expected, 'Kraken kit changed: ' + name)
    trusted(OUTROOT, regular=False, symlinks=False)
    lockfd = os.open('/run/vectorwarp-kraken-build.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o644)
    with os.fdopen(lockfd, 'r+') as lock:
        info = os.fstat(lock.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022, 'Untrusted build lock')
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        user = pwd.getpwnam('vectorwarp-kraken-build')
        require(user.pw_uid != 0 and user.pw_gid != 0, 'Compiler identity must be unprivileged')
        work = Path(tempfile.mkdtemp(prefix='.build-', dir=OUTROOT))
        staging = None
        try:
            os.chown(work, user.pw_uid, user.pw_gid)
            def drop():
                os.setgroups([]); os.setgid(user.pw_gid); os.setuid(user.pw_uid)
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
                resource.setrlimit(resource.RLIMIT_FSIZE, (256 * 1024 * 1024,) * 2)
            bounded = sandbox['bounded']
            bounded.__globals__['MAX_OUTPUT'] = 2 * 1024 * 1024
            code, output = bounded(['/usr/bin/python3', '-I', str(kitdir / 'script/build-kraken.py'),
                '--output-dir', str(work / 'artifact'), '--build-dir', str(work / 'build'), '--jobs', '2'], 1500, drop)
            print(output.decode(errors='replace')[-65536:], flush=True)
            require(code == 0, 'Kraken source build failed; inspect journalctl -u vectorwarp-kraken-build')
            # bounded() reaps ALL descendants, including escaped process groups.
            os.chown(work, 0, 0, follow_symlinks=False); work.chmod(0o700)
            for name, expected in kit['files'].items():
                require(sha(kitdir / name) == expected, 'Kit changed during build')
            base = OUTROOT / identity
            base.mkdir(mode=0o755, exist_ok=True); trusted(base, regular=False, symlinks=False)
            (base / 'generations').mkdir(mode=0o755, exist_ok=True)
            trusted(base / 'generations', regular=False, symlinks=False)
            staging = Path(tempfile.mkdtemp(prefix='.publish-', dir=base / 'generations'))
            files = ['bin/heimdall', 'share/heimdall/index.html', 'share/heimdall/build.json', 'share/heimdall/sbom.cdx.json']
            files += ['share/heimdall/licenses/' + name for name in
                ('rtlsdr-COPYING', 'uwebsockets-LICENSE', 'usockets-LICENSE', 'pffft-LICENSE.txt')]
            for name in files:
                copy_regular(work / 'artifact' / name, staging / name)
            receipt = json.loads((staging / 'share/heimdall/build.json').read_text())
            require(receipt.get('sourceManifestSha256') == kit['files']['config/kraken-macos-sources.json'], 'Source receipt mismatch')
            expected_patches = {Path(name).name: digest for name, digest in kit['files'].items()
                                if name.startswith('script/patches/')}
            require(receipt.get('patches') == expected_patches, 'Patch receipt mismatch')
            verify = runpy.run_path(str(PREFIX / 'current/share/heimdall/service.py'))['verify']
            verify(staging)
            require((staging / 'bin/heimdall').read_bytes()[:4] == b'\x7fELF', 'Companion is not ELF')
            (staging / 'kit-id').write_text(identity + '\n')
            generation = sha(staging / 'share/heimdall/build.json')
            final = base / 'generations' / generation
            if final.exists():
                trusted(final / 'bin/heimdall'); trusted(final / 'share/heimdall/build.json')
                require(sha(final / 'share/heimdall/build.json') == generation, 'Existing receipt changed')
                verify(final)
            else:
                staging.chmod(0o755); staging.rename(final); staging = None
            sandbox['swap_current'](base, final)
            print('Kraken companion built locally; no receiver started. kit=' + identity)
        finally:
            if staging is not None:
                shutil.rmtree(staging)
            # Only this freshly allocated path, never the adapter/generation root.
            shutil.rmtree(work)


if __name__ == '__main__':
    try:
        require(sys.argv[1:] == ['build'], 'usage: vectorwarp-build-kraken build')
        def stopped(signum, frame):
            # Enter bounded()'s descendant-reap and build()'s cleanup finally
            # blocks on a normal systemd stop, without publishing partial work.
            raise RuntimeError('Local Kraken source build stopped before completion')
        signal.signal(signal.SIGTERM, stopped)
        launcher = runpy.run_path(str(PREFIX / 'libexec/vectorwarp'))
        with launcher['upgrade_guard']():
            build()
    except Exception as error:
        sys.exit('vectorwarp-build-kraken: ' + str(error))
