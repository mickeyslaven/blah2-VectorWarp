#!/usr/bin/python3
"""Read-only Linux identity probe for the final VectorWarp live soak.

The artifact digest covers names, modes, symlink targets and all regular-file
bytes. Configuration and running executables are measured separately. The
probe deliberately fails on external symlinks or concurrently changing files.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess


UNITS = ("vectorwarp-api.service", "vectorwarp-processor.service")
MAX_FILES = 30000
MAX_BYTES = 2 * 1024**3


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size > MAX_BYTES:
            raise ValueError(f"not a bounded regular file: {path}")
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        after = os.fstat(stream.fileno())
    current = path.stat()
    fields = lambda value: (value.st_dev, value.st_ino, value.st_size,
                            value.st_mtime_ns, value.st_ctime_ns, value.st_mode)
    if fields(before) != fields(after) or fields(after) != fields(current):
        raise ValueError(f"file changed during identity probe: {path}")
    return digest.hexdigest()


def artifact_hash(root: Path) -> str:
    root = root.resolve(strict=True)
    if not root.is_dir() or root == Path("/"):
        raise ValueError("artifact must be a specific directory")
    entries = []
    total_bytes = 0
    def unreadable(error):
        raise error
    for directory, directories, files in os.walk(root, followlinks=False, onerror=unreadable):
        for name in sorted(directories + files):
            path = Path(directory) / name
            info = path.lstat()
            relative = path.relative_to(root).as_posix()
            mode = stat.S_IMODE(info.st_mode)
            if path.is_symlink():
                path.resolve(strict=True).relative_to(root)
                entry = [relative, mode, "symlink", os.readlink(path)]
            elif stat.S_ISDIR(info.st_mode):
                entry = [relative, mode, "directory"]
            elif stat.S_ISREG(info.st_mode):
                total_bytes += info.st_size
                if total_bytes > MAX_BYTES:
                    raise ValueError("artifact exceeds identity probe byte budget")
                entry = [relative, mode, "file", file_hash(path)]
            else:
                raise ValueError(f"special file in artifact: {relative}")
            entries.append(entry)
            if len(entries) > MAX_FILES:
                raise ValueError("artifact exceeds identity probe file budget")
    if not entries:
        raise ValueError("empty artifact")
    encoded = json.dumps(sorted(entries), ensure_ascii=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode()).hexdigest()


def probe(release: Path = Path("/opt/vectorwarp/current"),
          config: Path = Path("/etc/vectorwarp/config.yml")) -> dict:
    release = release.resolve(strict=True)
    config = config.resolve(strict=True)
    output = subprocess.run(
        ["systemctl", "show", *UNITS,
         "--property=Id,ActiveState,MainPID,NRestarts,Result,ExecMainStartTimestampMonotonic"],
        check=True, capture_output=True, text=True, timeout=8).stdout
    units = {}
    for block in output.strip().split("\n\n"):
        values = dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
        if values.get("Id"):
            units[values["Id"]] = values
    if set(units) != set(UNITS):
        raise ValueError("both running VectorWarp units must be observable")
    executables = {}
    for unit, values in units.items():
        pid = int(values["MainPID"])
        if pid <= 0 or values.get("ActiveState") != "active":
            raise ValueError(f"unit is not running: {unit}")
        proc = Path("/proc") / str(pid)
        arguments = (proc / "cmdline").read_bytes().decode().rstrip("\0").split("\0")
        cwd = (proc / "cwd").resolve(strict=True)
        paths = {(cwd / argument).resolve() for argument in arguments[1:]
                 if argument and not argument.startswith("-")}
        if config not in paths:
            raise ValueError(f"unit does not use the bound config: {unit}")
        executable = (proc / "exe").resolve(strict=True)
        if unit == "vectorwarp-api.service":
            if release / "api/server.js" not in paths:
                raise ValueError("API does not run from the bound artifact")
        elif executable != release / "bin/blah2":
            raise ValueError("processor does not run from the bound artifact")
        executables[unit] = {"path": str(executable), "sha256": file_hash(proc / "exe")}
    return {"release": str(release), "units": units,
            "bootId": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "artifactSha256": artifact_hash(release), "configSha256": file_hash(config),
            "runtimeExecutables": executables}


if __name__ == "__main__":
    print(json.dumps(probe(), sort_keys=True))
