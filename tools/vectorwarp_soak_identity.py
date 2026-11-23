#!/usr/bin/python3
"""Read-only installed/runtime cohort provenance."""
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
NATIVE_HASH_PATHS = {"processor_sha256": "bin/blah2",
                     "gpu_worker_sha256": "bin/blah2-gpu-worker",
                     "portable_module_sha256": "bin/blah2-gpu-vulkan.so",
                     "wideband_module_sha256": "bin/blah2-gpu-wideband-vulkan.so",
                     "resident_module_sha256": "bin/blah2-gpu-resident-vulkan.so"}


def service_units() -> dict:
    output = subprocess.run(
        ["systemctl", "show", *UNITS,
         "--property=Id,User,ActiveState,MainPID,NRestarts,Result,ExecMainStartTimestampMonotonic,ExecStart,Environment,EnvironmentFiles,WorkingDirectory,BindPaths,BindReadOnlyPaths,RootDirectory,RootImage"],
        check=True, capture_output=True, text=True, timeout=8).stdout
    blocks = (dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
              for block in output.strip().split("\n\n"))
    return {values["Id"]: values for values in blocks if values.get("Id")}


def check_loader_environment(environment: dict, release: Path) -> None:
    # Reject foreign code, regardless of SDR/backend.
    for key in ("LD_PRELOAD", "LD_AUDIT", "NODE_OPTIONS", "NODE_PATH"):
        if environment.get(key):
            raise ValueError(f"cohort startup forbids {key}")
    if environment.get("LD_LIBRARY_PATH"):
        paths = environment["LD_LIBRARY_PATH"].split(":")
        if any(not path or Path(path).resolve(strict=True) != release / "bin" for path in paths):
            raise ValueError("library path outside candidate bin")


def check_arguments(unit: str, arguments: list[str], cwd: Path,
                    release: Path, config: Path) -> None:
    resolve = lambda value: (cwd / value).resolve(strict=True)
    if unit == "vectorwarp-api.service":
        if (len(arguments) != 3 or resolve(arguments[1]) != release / "api/server.js"
                or resolve(arguments[2]) != config or cwd != release):
            raise ValueError("API entrypoint, cwd or config outside cohort")
    elif (len(arguments) != 3 or resolve(arguments[0]) != release / "bin/blah2"
          or arguments[1] != "--config" or resolve(arguments[2]) != config):
        raise ValueError("processor entrypoint or config outside cohort")


def process_cwd(proc: Path, declared: Path) -> Path:
    try:
        return (proc / "cwd").resolve(strict=True)
    except PermissionError:
        # Root inside a capability-bounded container may not dereference a
        # different service user's cwd. The service plan and absolute runtime
        # arguments remain independently checked, so its declared cwd is the
        # only safe portable fallback.
        return declared


def check_candidate_access(release: Path, units: dict) -> None:
    """Prove daemon access despite privileged ExecCondition."""
    if any(values.get("User", "") in {"", "root", "0"} for values in units.values()):
        raise ValueError("cohort services must use unprivileged users")

    # A protected ancestor also prevents CHDIR/path lookup.
    paths = [(path, stat.S_IFDIR, stat.S_IXOTH, "traversable") for path in
             (release, *release.parents, release / "api", release / "bin")]
    paths += [(release / "api/server.js", stat.S_IFREG, stat.S_IROTH, "readable"),
              (release / "bin/blah2", stat.S_IFREG, stat.S_IROTH | stat.S_IXOTH, "readable and executable")]
    for path, kind, bits, purpose in paths:
        info = path.stat()
        if stat.S_IFMT(info.st_mode) != kind or info.st_mode & bits != bits:
            raise ValueError(f"candidate not {purpose} for service users: {path}")


def cohort_units(units: dict) -> tuple[str, dict]:
    if set(units) != set(UNITS):
        raise ValueError("both cohort units required")
    environment = dict(item.split("=", 1) for item in
                       shlex.split(units[UNITS[0]].get("Environment", "")))
    role = environment.get("VECTORWARP_NODE_ROLE") or "radar"
    if role not in {"radar", "coordinator"}:
        raise ValueError("invalid cohort node role")
    if role == "coordinator":
        processor = units[UNITS[1]]
        if processor.get("ActiveState") not in {"inactive", "failed"} or int(processor.get("MainPID") or 0):
            raise ValueError("coordinator must not run a local processor")
        return role, {UNITS[0]: units[UNITS[0]]}
    return role, units


def preflight(release: Path, config: Path, units: dict | None = None,
              check_immutable: bool = True, allow_legacy: bool = False) -> dict:
    """Validate installed and running service cohorts."""
    release, config = release.resolve(strict=True), config.resolve(strict=True)
    units = service_units() if units is None else units
    role, units = cohort_units(units)
    check_candidate_access(release, units)
    declared_hashes = check_declared_hashes(release, allow_legacy=allow_legacy)
    executables, modules, children, processes = {}, {}, {}, []
    for unit, values in units.items():
        for key in ("BindPaths", "BindReadOnlyPaths", "RootDirectory", "RootImage"):
            if values.get(key):
                raise ValueError(f"cohort startup forbids {key} for {unit}")
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
                raise ValueError("config environment may not select code")
        # daemon-reload changes the next plan, not the already running code.
        if values.get("ActiveState") == "active":
            proc = Path("/proc") / str(int(values["MainPID"]))
            processes.append(proc)
            running = (proc / "cmdline").read_bytes().decode().rstrip("\0").split("\0")
            check_arguments(unit, running, process_cwd(proc, cwd), release, config)
            if unit == "vectorwarp-processor.service" and (proc / "exe").resolve(strict=True) != release / "bin/blah2":
                raise ValueError("active processor differs from the candidate service plan")
            running_environment = dict(item.split("=", 1) for item in
                (proc / "environ").read_bytes().decode().rstrip("\0").split("\0") if "=" in item)
            check_loader_environment(running_environment, release)
            if unit == UNITS[0] and (running_environment.get("VECTORWARP_NODE_ROLE") or "radar") != role:
                raise ValueError("active API node role differs from the service plan")
            executables[unit] = {"path": str((proc / "exe").resolve(strict=True)), "sha256": file_hash(proc / "exe")}
            modules[unit], children[unit] = loaded_modules(proc, release), []
            pending, seen = [proc], set()
            while pending:
                for task in (pending.pop() / "task").iterdir():
                    for child in (task / "children").read_text().split():
                        if child in seen:
                            continue
                        seen.add(child)
                        if len(seen) > 64:
                            raise ValueError("too many cohort child processes")
                        child_proc = Path("/proc") / child
                        executable = (child_proc / "exe").resolve(strict=True)
                        if executable.parent != release / "bin":
                            raise ValueError("child executable outside candidate bin")
                        modules[unit][str(executable)] = file_hash(child_proc / "exe")
                        modules[unit].update(loaded_modules(child_proc, release))
                        children[unit].append({"pid": int(child), "path": str(executable),
                            "startTicks": (child_proc / "stat").read_text().rsplit(")", 1)[1].split()[19]})
                        pending.append(child_proc)
            children[unit].sort(key=lambda child: child["pid"])
    starts = [int(values["ExecMainStartTimestampMonotonic"]) for values in units.values()
              if values.get("ActiveState") == "active"]
    artifact = None
    if starts:
        if min(starts) <= 0:
            raise ValueError("active startup timestamp unavailable")
        # Node caches JS: the entire tree must predate both service launches.
        before = time.time_ns() - time.monotonic_ns() + min(starts) * 1000 if check_immutable else None
        artifact = artifact_hash(release, before)
        for proc in processes:
            if artifact_hash(release, before, process_root=proc / "root") != artifact:
                raise ValueError("process-root artifact differs from the installed cohort")
            if file_hash(proc / "root" / config.relative_to("/")) != file_hash(config):
                raise ValueError("process-root configuration differs from the installed configuration")
    return {"release": str(release), "config": str(config), "coherentServicePlan": True, "nodeRole": role,
            "verifiedDeclaredHashes": declared_hashes,
            "artifactHashProvenance": "installed-byte-pinned" if declared_hashes else "legacy-unpinned",
            "artifactSha256": artifact, "runtimeExecutables": executables,
            "runtimeModules": modules, "runtimeChildren": children}


def installed_hashes(release: Path) -> dict:
    """Pin the processor and optional GPU cohort."""
    release = release.resolve(strict=True)
    paths = {key: release / name for key, name in NATIVE_HASH_PATHS.items()}
    if not any(path.exists() for key, path in paths.items() if key != "processor_sha256"):
        paths = {"processor_sha256": paths["processor_sha256"]}
    for target in paths.values():
        target.resolve(strict=True).relative_to(release)
    return {key: file_hash(target) for key, target in paths.items()}


def check_declared_hashes(release: Path, allow_legacy: bool = False) -> dict:
    """Pins describe installed bytes, not build inputs."""
    manifest = release / ".vectorwarp-build"
    if not manifest.exists():
        if allow_legacy:
            return {}
        raise ValueError("artifact lacks installed-byte hash provenance")
    manifest.resolve(strict=True).relative_to(release)
    if manifest.stat().st_size > 65536:
        raise ValueError("build manifest exceeds provenance budget")
    before = file_hash(manifest)
    verified = {}
    for key, _, value in (line.partition("=") for line in manifest.read_text().splitlines()):
        if key in NATIVE_HASH_PATHS:
            if key in verified or not re.fullmatch(r"[a-f0-9]{64}", value):
                raise ValueError(f"invalid or duplicate declared artifact hash: {key}")
            verified[key] = value
    if not verified and allow_legacy:
        return {}
    actual = installed_hashes(release)
    if set(verified) != set(actual):
        raise ValueError("incomplete installed-byte hash cohort")
    for key, value in verified.items():
        if actual[key] != value:
            raise ValueError(f"declared artifact hash differs from installed bytes: {key}")
    if file_hash(manifest) != before:
        raise ValueError("build manifest changed during verification")
    return verified


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
            raise ValueError("loaded native module outside candidate bin directory")
        info = path.stat()
        major, minor = (int(value, 16) for value in fields[3].split(":"))
        same_inode = info.st_ino == int(fields[4])
        same_device = (os.major(info.st_dev), os.minor(info.st_dev)) == (major, minor)
        # Btrfs device aliases still require matching inode and map_files bytes.
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
    if not fields(before) == fields(after) == fields(current):
        raise ValueError(f"file changed during identity probe: {path}")
    return digest.hexdigest()


def artifact_hash(root: Path, latest_change_ns: int | None = None,
                  process_root: Path | None = None) -> str:
    root = root.resolve(strict=True)
    # Never resolve /proc/PID/root: doing so loses the process mount namespace.
    measured_root = process_root / root.relative_to("/") if process_root is not None else root
    if not measured_root.is_dir() or root == Path("/"):
        raise ValueError("artifact must be a specific directory")
    entries = []
    total_bytes = 0
    def unchanged_since_start(info):
        if latest_change_ns is not None and max(info.st_ctime_ns, info.st_mtime_ns) > latest_change_ns:
            raise ValueError("artifact changed after API/processor startup; restart the complete cohort")
    unchanged_since_start(measured_root.stat())
    def unreadable(error):
        raise error
    for directory, directories, files in os.walk(measured_root, followlinks=False, onerror=unreadable):
        for name in sorted(directories + files):
            path = Path(directory) / name
            info = path.lstat()
            unchanged_since_start(info)
            relative = path.relative_to(measured_root).as_posix()
            mode = stat.S_IMODE(info.st_mode)
            if path.is_symlink():
                (root / relative).resolve(strict=True).relative_to(root)
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
    _, required = cohort_units(units)
    if any(int(value["MainPID"]) <= 0 or value.get("ActiveState") != "active" for value in required.values()):
        raise ValueError("required cohort units must be running")
    result = preflight(release, config, units)
    if units != service_units() or selector.resolve(strict=True) != release:
        raise ValueError("service cohort changed during identity probe")
    disk = os.statvfs(disk_path)
    return {**result, "units": units,
            "disk": {"path": str(disk_path), "freeBytes": disk.f_bavail * disk.f_frsize},
            "hostIdSha256": file_hash(Path("/etc/machine-id")),
            "bootId": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "configSha256": file_hash(config)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("--preflight", "--allow-legacy-unpinned", "--print-installed-hashes"):
        parser.add_argument(flag, action="store_true")
    parser.add_argument("--release", type=Path, default=Path("/opt/vectorwarp/current"))
    parser.add_argument("--config", type=Path, default=Path("/etc/vectorwarp/config.yml"))
    parser.add_argument("--disk-path", type=Path, default=Path("/var/lib"))
    args = parser.parse_args()
    try:
        if args.print_installed_hashes:
            print("".join(f"{key}={value}\n" for key, value in installed_hashes(args.release).items()), end="")
            parser.exit()
        if args.allow_legacy_unpinned and not args.preflight:
            raise ValueError("legacy allowance requires preflight inspection")
        result = (preflight(args.release, args.config, allow_legacy=args.allow_legacy_unpinned) if args.preflight else
                  probe(args.release, args.config, args.disk_path))
        print(json.dumps(result, sort_keys=True))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(78, f"VectorWarp cohort rejected: {error}\n")
