#!/usr/bin/python3 -I
"""Retire only the obsolete VectorWarp API sudo grant on package upgrade."""
import os
import pathlib
import stat
import subprocess
import sys
import tempfile

SUDOERS = pathlib.Path('/etc/sudoers.d/vectorwarp')
OLD_GRANT = b'vectorwarp-api ALL=(root) NOPASSWD: /usr/bin/systemctl start --no-block vectorwarp-sdrplay-build.service\n'
OLD_DEFAULT = b'Defaults:vectorwarp-api !requiretty\n'


def retired_content(raw):
    if OLD_GRANT not in raw.splitlines(keepends=True):
        if b'vectorwarp-api' in raw and b'NOPASSWD' in raw:
            raise ValueError('an unrecognized VectorWarp API sudo grant remains; review it locally')
        return None
    lines = [line for line in raw.splitlines(keepends=True) if line not in (OLD_GRANT, OLD_DEFAULT)]
    updated = b''.join(lines) or b'# No VectorWarp web API sudo grant is required.\n'
    if b'vectorwarp-api' in updated:
        raise ValueError('additional VectorWarp API sudo rules exist; review them locally')
    return updated


def migrate(path=SUDOERS, verify=None):
    path = pathlib.Path(path)
    parent = path.parent
    for item in (parent, *parent.parents):
        info = item.stat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError('sudoers directory is not trusted; review the legacy grant locally')
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return 'absent'
    with os.fdopen(fd, 'rb') as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o440 or info.st_size > 16384:
            raise ValueError('sudoers file is not a bounded root-owned 0440 file; review it locally')
        raw = source.read(16385)
        if len(raw) > 16384:
            raise ValueError('sudoers file is oversized; review it locally')
    updated = retired_content(raw)
    if updated is None:
        return 'unchanged'
    if verify is None:
        verify = lambda candidate: subprocess.run(['/usr/sbin/visudo', '-cf', str(candidate)],
                                                   check=True, timeout=5, stdout=subprocess.DEVNULL,
                                                   stderr=subprocess.DEVNULL)
    temp_fd, temp_name = tempfile.mkstemp(prefix='.vectorwarp-sudoers-', dir=parent)
    try:
        with os.fdopen(temp_fd, 'wb') as target:
            target.write(updated)
            target.flush()
            os.fchmod(target.fileno(), 0o440)
            os.fsync(target.fileno())
        verify(pathlib.Path(temp_name))
        current = path.lstat()
        if (current.st_dev, current.st_ino, current.st_mtime_ns, current.st_size) != (
                info.st_dev, info.st_ino, info.st_mtime_ns, info.st_size):
            raise ValueError('sudoers file changed during migration; review it locally')
        os.replace(temp_name, path)
        directory_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        return 'retired'
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)


if __name__ == '__main__':
    try:
        if len(sys.argv) != 1 or os.geteuid() != 0:
            raise ValueError('run the installed root-owned migration helper without arguments')
        print('VectorWarp sudoers:', migrate())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'VectorWarp sudoers migration needs administrator review: {error}', file=sys.stderr)
        sys.exit(1)
