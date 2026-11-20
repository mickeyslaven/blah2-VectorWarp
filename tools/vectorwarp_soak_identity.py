#!/usr/bin/python3
"""Read-only Linux identity probe for the final VectorWarp live soak.

The artifact digest covers names, modes, symlink targets and all regular-file
bytes. Configuration and running executables are measured separately. The
probe deliberately fails on external symlinks or concurrently changing files.
"""
from __future__ import annotations

import hashlib
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import time


UNITS = ("vectorwarp-api.service", "vectorwarp-processor.service")
MAX_FILES = 30000
MAX_BYTES = 2 * 1024**3


def service_units() -> dict:
    output = subprocess.run(
        ["systemctl", "show", *UNITS,
         "--property=Id,User,ActiveState,MainPID,NRestarts,Result,ExecMainStartTimestampMonotonic,ExecStart,Environment,EnvironmentFiles,WorkingDirectory"],
        check=True, capture_output=True, text=True, timeout=8).stdout
    units = {}
    for block in output.strip().split("\n\n"):
        values = dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
        if values.get("Id"):
            units[values["Id"]] = values
    if set(units) != set(UNITS):
        raise ValueError("both VectorWarp units must be observable")
    return units


def check_loader_environment(environment: dict, release: Path) -> None:
    # No SDR/backend exceptions: a candidate cannot borrow another build's DSO.
    for key in ("LD_PRELOAD", "LD_AUDIT", "NODE_OPTIONS", "NODE_PATH"):
        if environment.get(key):
            raise ValueError(f"cohort startup forbids {key}")
    if environment.get("LD_LIBRARY_PATH"):
        paths = environment["LD_LIBRARY_PATH"].split(":")
        if any(not path or Path(path).resolve(strict=True) != release / "bin" for path in paths):
            raise ValueError("library search path is outside the candidate bin directory")


def check_arguments(unit: str, arguments: list[str], cwd: Path,
                    release: Path, config: Path) -> None:
    resolve = lambda value: (cwd / value).resolve(strict=True)
    if unit == "vectorwarp-api.service":
        if (len(arguments) != 3 or resolve(arguments[1]) != release / "api/server.js"
                or resolve(arguments[2]) != config or cwd != release):
            raise ValueError("API entrypoint, working directory and config must use the candidate cohort")
    elif (len(arguments) != 3 or resolve(arguments[0]) != release / "bin/blah2"
          or arguments[1] != "--config" or resolve(arguments[2]) != config):
        raise ValueError("processor entrypoint and config must use the candidate cohort")


def check_candidate_access(release: Path, units: dict) -> None:
    """Require modes that work for both distinct unprivileged service users.

    ExecCondition runs with elevated privileges, so its successful path lookup
    cannot prove that either daemon can enter the candidate.  Use the ``other``
    mode bits deliberately: they are independent of this host's account and
    group layout, and therefore attest access for each distinct service user.
    """
    users = {unit: units[unit].get("User", "") for unit in UNITS}
    if not all(users.values()) or any(user in {"root", "0"} for user in users.values()):
        raise ValueError("cohort services must use unprivileged users")

    def require(path: Path, bits: int, purpose: str, directory: bool = False) -> None:
        info = path.stat()
        if ((directory and not stat.S_ISDIR(info.st_mode))
                or (not directory and not stat.S_ISREG(info.st_mode))
                or stat.S_IMODE(info.st_mode) & bits != bits):
            raise ValueError(f"candidate is not {purpose} for both service users: {path}")

    # Every ancestor is part of a CHDIR/path lookup; checking only the release
    # itself would still let a protected parent break activation.
    for directory in (release, *release.parents):
        require(directory, stat.S_IXOTH, "traversable", directory=True)
    require(release / "api", stat.S_IXOTH, "traversable", directory=True)
    require(release / "bin", stat.S_IXOTH, "traversable", directory=True)
    require(release / "api/server.js", stat.S_IROTH, "readable")
    require(release / "bin/blah2", stat.S_IROTH | stat.S_IXOTH, "readable and executable")


def preflight(release: Path, config: Path, units: dict | None = None,
              check_immutable: bool = True) -> dict:
    """Read-only ExecCondition: inspect merged units, including local overrides.

    Complex/wrapped ExecStart forms are refused rather than guessed. This gate
    never changes services or links; package and native installs share it.
    """
    release, config = release.resolve(strict=True), config.resolve(strict=True)
    units = service_units() if units is None else units
    if set(units) != set(UNITS):
        raise ValueError("both VectorWarp units must be observable")
    check_candidate_access(release, units)
    for unit in UNITS:
        values = units[unit]
        match = re.fullmatch(r"\{ path=([^;]+?) ; argv\[\]=(.*?) ; ignore_errors=.*? \}",
                             values.get("ExecStart", ""))
        if not match or "\\" in match[2] or " ; " in match[2]:
            raise ValueError(f"unverifiable ExecStart for {unit}")
        arguments = shlex.split(match[2])
        if not arguments or Path(match[1]).resolve(strict=True) != Path(arguments[0]).resolve(strict=True):
            raise ValueError(f"ExecStart path/arguments disagree for {unit}")
        cwd = Path(values.get("WorkingDirectory") or "/").resolve(strict=True)
        check_arguments(unit, arguments, cwd, release, config)
        environment = dict(item.split("=", 1) for item in shlex.split(values.get("Environment", "")))
        check_loader_environment(environment, release)
        # The package's transient config-preservation file cannot select code.
        files = values.get("EnvironmentFiles", "")
        if files and files != "/run/vectorwarp-api-preserve-config.env (ignore_errors=yes)":
            raise ValueError(f"unreviewed EnvironmentFiles for {unit}")
        if files:
            filename = Path("/run/vectorwarp-api-preserve-config.env")
            if filename.exists() and any(line and not re.fullmatch(
                    r"VECTORWARP_PRESERVE_CONFIG_BYTES_UNTIL_UPTIME=[0-9]+", line)
                    for line in filename.read_text().splitlines()):
                raise ValueError("config preservation environment may not select code")
        # A daemon-reload may have changed the plan while an old API/processor
        # is still running. Validate the active peer, not just its next command.
        if values.get("ActiveState") == "active":
            proc = Path("/proc") / str(int(values["MainPID"]))
            running = (proc / "cmdline").read_bytes().decode().rstrip("\0").split("\0")
            check_arguments(unit, running, (proc / "cwd").resolve(strict=True), release, config)
            if unit == "vectorwarp-processor.service" and (proc / "exe").resolve(strict=True) != release / "bin/blah2":
                raise ValueError("active processor differs from the candidate service plan")
            loaded_modules(proc, release)
    starts = [int(values["ExecMainStartTimestampMonotonic"]) for values in units.values()
              if values.get("ActiveState") == "active"]
    if check_immutable and starts:
        if min(starts) <= 0:
            raise ValueError("active service startup timestamp is unavailable")
        artifact_hash(release, time.time_ns() - time.monotonic_ns() + min(starts) * 1000)
    return {"release": str(release), "config": str(config), "coherentServicePlan": True}


def loaded_modules(proc: Path, release: Path) -> dict:
    """Measure project DSOs actually mapped, rejecting stale/deleted inodes."""
    modules = {}
    for line in (proc / "maps").read_text().splitlines():
        fields = line.split(None, 5)
        if len(fields) != 6 or not fields[5].startswith("/"):
            continue
        name = Path(fields[5]).name
        if not (name.startswith(("libblah2-", "blah2-")) and ".so" in name):
            continue
        path = Path(fields[5]).resolve(strict=True)
        if path.parent != release / "bin":
            raise ValueError("loaded native module is outside the candidate bin directory")
        info = path.stat()
        major, minor = (int(value, 16) for value in fields[3].split(":"))
        same_inode = info.st_ino == int(fields[4])
        same_device = (os.major(info.st_dev), os.minor(info.st_dev)) == (major, minor)
        # Btrfs can expose the mapped backing device in /proc/PID/maps while
        # stat reports the mounted subvolume device.  Preserve the inode gate
        # and, only for that device-number disagreement, bind the exact mapped
        # object to the release bytes through the kernel's map_files handle.
        mapped = proc / "map_files" / fields[0]
        mapped_bytes_match = same_inode and not same_device
        if mapped_bytes_match:
            try:
                mapped_bytes_match = mapped.stat().st_ino == info.st_ino and \
                    file_hash(mapped) == file_hash(path)
            except (OSError, ValueError):
                mapped_bytes_match = False
        if not same_inode or not (same_device or mapped_bytes_match):
            raise ValueError("loaded module differs from its on-disk inode")
        if str(path) not in modules:
            modules[str(path)] = file_hash(path)
    return modules


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


def artifact_hash(root: Path, latest_change_ns: int | None = None) -> str:
    root = root.resolve(strict=True)
    if not root.is_dir() or root == Path("/"):
        raise ValueError("artifact must be a specific directory")
    entries = []
    total_bytes = 0
    def unchanged_since_start(info):
        if latest_change_ns is not None and max(info.st_ctime_ns, info.st_mtime_ns) > latest_change_ns:
            raise ValueError("artifact changed after API/processor startup; restart the complete cohort")
    unchanged_since_start(root.stat())
    def unreadable(error):
        raise error
    for directory, directories, files in os.walk(root, followlinks=False, onerror=unreadable):
        for name in sorted(directories + files):
            path = Path(directory) / name
            info = path.lstat()
            unchanged_since_start(info)
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
          config: Path = Path("/etc/vectorwarp/config.yml"),
          disk_path: Path = Path("/var/lib")) -> dict:
    selector = release
    release = selector.resolve(strict=True)
    config = config.resolve(strict=True)
    units = service_units()
    preflight(release, config, units, check_immutable=False)
    executables = {}
    modules = {}
    children = {}
    for unit, values in units.items():
        pid = int(values["MainPID"])
        if pid <= 0 or values.get("ActiveState") != "active":
            raise ValueError(f"unit is not running: {unit}")
        proc = Path("/proc") / str(pid)
        arguments = (proc / "cmdline").read_bytes().decode().rstrip("\0").split("\0")
        cwd = (proc / "cwd").resolve(strict=True)
        check_arguments(unit, arguments, cwd, release, config)
        environment = dict(item.split("=", 1) for item in
                           (proc / "environ").read_bytes().decode().rstrip("\0").split("\0") if "=" in item)
        check_loader_environment(environment, release)
        executable = (proc / "exe").resolve(strict=True)
        if unit == "vectorwarp-processor.service" and executable != release / "bin/blah2":
            raise ValueError("processor does not run from the bound artifact")
        executables[unit] = {"path": str(executable), "sha256": file_hash(proc / "exe")}
        modules[unit] = loaded_modules(proc, release)
        children[unit] = []
        # GPU workers are in the processor process tree; their executable and
        # modules must come from this same frozen artifact as well.
        pending = [proc]
        seen = set()
        while pending:
            parent = pending.pop()
            for task in (parent / "task").iterdir():
                for child in (task / "children").read_text().split():
                    if child in seen:
                        continue
                    seen.add(child)
                    if len(seen) > 64:
                        raise ValueError("too many cohort child processes")
                    child_proc = Path("/proc") / child
                    child_exe = (child_proc / "exe").resolve(strict=True)
                    if child_exe.parent != release / "bin":
                        raise ValueError("child executable is outside the candidate bin directory")
                    modules[unit][str(child_exe)] = file_hash(child_proc / "exe")
                    modules[unit].update(loaded_modules(child_proc, release))
                    start_ticks = (child_proc / "stat").read_text().rsplit(")", 1)[1].split()[19]
                    children[unit].append({"pid": int(child), "startTicks": start_ticks,
                                           "path": str(child_exe)})
                    pending.append(child_proc)
        children[unit].sort(key=lambda child: child["pid"])
    starts = [int(values["ExecMainStartTimestampMonotonic"]) for values in units.values()]
    if min(starts) <= 0:
        raise ValueError("service startup timestamp is unavailable")
    # Node can retain JS modules from before an in-place package replacement.
    # A path/hash measured now alone cannot attest that cached code. Require
    # the complete immutable tree to predate both service launches as well.
    latest_change_ns = time.time_ns() - time.monotonic_ns() + min(starts) * 1000
    artifact = artifact_hash(release, latest_change_ns=latest_change_ns)
    if units != service_units() or selector.resolve(strict=True) != release:
        raise ValueError("service cohort changed during identity probe")
    disk = os.statvfs(disk_path)
    return {"release": str(release), "units": units,
            "disk": {"path": str(disk_path), "freeBytes": disk.f_bavail * disk.f_frsize},
            "hostIdSha256": file_hash(Path("/etc/machine-id")),
            "bootId": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "artifactSha256": artifact, "configSha256": file_hash(config),
            "runtimeExecutables": executables, "runtimeModules": modules,
            "runtimeChildren": children}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--release", type=Path, default=Path("/opt/vectorwarp/current"))
    parser.add_argument("--config", type=Path, default=Path("/etc/vectorwarp/config.yml"))
    parser.add_argument("--disk-path", type=Path, default=Path("/var/lib"))
    args = parser.parse_args()
    try:
        result = (preflight(args.release, args.config) if args.preflight else
                  probe(args.release, args.config, args.disk_path))
        print(json.dumps(result, sort_keys=True))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(78, f"VectorWarp cohort rejected: {error}\n")
