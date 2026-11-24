#!/usr/bin/env python3
"""Canonical artifacts; legacy install only."""
import argparse
import fnmatch
import json
import os
from pathlib import Path
import re
import runpy
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLAN = json.loads((ROOT / "packaging/artifact-plan.json").read_text())
IDENTITY = runpy.run_path(str(ROOT / "tools/vectorwarp_soak_identity.py"))


def key_values(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or key in result:
            raise ValueError(f"artifact manifest has duplicate or invalid {key} fields")
        result[key] = value
    return result


def profile(name, with_rspduo=False):
    if name not in PLAN["profiles"]:
        raise ValueError("artifact has an invalid backend")
    value = PLAN["profiles"][name]
    compiled, local = list(value["compiled"]), list(value["local"])
    if with_rspduo:
        compiled = [name for name in PLAN["receivers"] if name in compiled or name == "RspDuo"]
        local = []
    return compiled, local


def matrix():
    return {"include": [
        {"runner": PLAN["linux_arches"][arch]["runner"], "distro": name, "format": spec["format"],
         "arch": PLAN["linux_arches"][arch][spec["format"]], "node_arch": arch, "distro_env": name,
         **({"image": spec["image"]} if "image" in spec else {})}
        for name, spec in PLAN["linux_distros"].items() for arch in spec["arches"]]}


def release_targets(version=None):
    """The introduction date preserves immutable historical package receipts."""
    return {(spec["format"], spec["id"], spec["version"]):
            (spec["codename"] or None, {PLAN["linux_arches"][arch][spec["format"]] for arch in spec["arches"]})
            for spec in PLAN["linux_distros"].values()
            if version is None or tuple(map(int, version.split("."))) >= tuple(map(int, spec.get("introduced", "0.0.0").split(".")))}


def release_filenames(version):
    return {f"vectorwarp_{version}-1_{distro}{release}_{arch}.deb" if kind == "deb" else
            f"vectorwarp-{version}-1.fc{release}.{arch}.rpm"
            for (kind, distro, release), (_, arches) in release_targets(version).items() for arch in arches}


def checkout(name, directory, offline=False, dry_run=False):
    dependency = PLAN["dependencies"][name]
    commit = dependency["commit"]
    directory = Path(directory)
    def run(*argv):
        if dry_run:
            print("+ " + shlex.join(map(str, argv)))
        else:
            subprocess.run(argv, check=True)
    if not (directory / ".git").is_dir():
        if offline: raise ValueError(f"offline dependency is absent: {directory}")
        run("git", "clone", "--filter=blob:none", dependency["url"], str(directory))
    if not dry_run and subprocess.run(["git", "-C", str(directory), "cat-file", "-e", commit + "^{commit}"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        if offline: raise ValueError(f"offline dependency lacks pinned commit {commit}: {directory}")
        run("git", "-C", str(directory), "fetch", "--depth", "1", "origin", commit)
    run("git", "-C", str(directory), "checkout", "--detach", commit)
    if not dry_run and subprocess.check_output(["git", "-C", str(directory), "rev-parse", "HEAD"], text=True).strip() != commit:
        raise ValueError(f"dependency did not resolve to pinned commit: {directory}")


def require(root, paths, executable=False):
    for name in paths:
        path = root / name
        if not path.is_file() or (executable and not os.access(path, os.X_OK)):
            raise ValueError(f"artifact is incomplete or not executable: {name}")


def validate(root, level, backend=None, test_only=False):
    root = Path(root)
    require(root, PLAN["required"]["base"])
    require(root, ["bin/blah2"], executable=True)
    if any((root / name).exists() or (root / name).is_symlink() for name in PLAN["forbidden"]):
        raise ValueError("historical mixed DSP workers are not part of the unified runtime artifact")
    if level != "install" and any((root / "bin").glob("test*")):
        raise ValueError("release artifact must not contain test executables")
    if level != "build":
        require(root, [".vectorwarp-build"])
    fields = key_values(root / ".vectorwarp-build") if level != "build" else {"backend": backend}
    if level == "package" and ("test_only" not in fields or "compiled_receivers" not in fields):
        raise ValueError("release artifact requires an explicit receiver and test-only manifest")
    backend = fields.get("backend")
    expected, expected_local = profile(backend)
    if level != "build":
        if not re.fullmatch(r"[A-Za-z0-9._:-]+", fields.get("build_id", "")):
            raise ValueError("artifact has an invalid build_id")
        marked = fields.get("test_only", "false")
        if marked not in ("true", "false") or (marked == "true") != (backend == "open-test"):
            raise ValueError("only open-test artifacts must be explicitly marked test-only")
        raw = fields.get("compiled_receivers")
        if raw is None:
            # Existing installed kraken/all artifacts predate receiver manifests.
            if level != "install" or backend not in ("kraken", "all"):
                raise ValueError("selected-backend artifact lacks compiled_receivers")
        else:
            names = raw.split(",")
            if len(set(names)) != len(names):
                raise ValueError("artifact has duplicate compiled receiver")
            if set(names) != set(expected):
                raise ValueError("artifact backend and compiled_receivers disagree")
        local = fields.get("local_build_receivers", "")
        if local not in ("", "RspDuo"):
            raise ValueError("artifact has an invalid local_build_receivers list")
        if backend == "all" and raw is not None:
            if local.split(",") != expected_local:
                raise ValueError("stable artifact must have three compiled receivers and the RSPduo local build kit")
            require(root, ["receiver-source/rspduo/kit.json", "systemd/vectorwarp-sdrplay-build.service.in"])
            require(root, ["libexec/vectorwarp-build-sdrplay.py"], executable=True)
        fields.update(compiled_receivers=",".join(expected), local_build_receivers=local, test_only=marked)
    if level in ("build", "package"):
        require(root, PLAN["required"]["build"])
        require(root, [f"bin/blah2-receiver-{PLAN['receivers'][name]}.so"
                       for name in expected if PLAN["receivers"][name]])
    if level == "package":
        if backend != ("open-test" if test_only else "all") or fields.get("gpu") != "AUTO":
            raise ValueError("published packages require the unified receiver and CPU plus Vulkan AUTO build")
        require(root, PLAN["required"]["package"])
        try:
            receipt = json.loads((root / "BUILD-RECEIPT.json").read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise ValueError("artifact has an invalid build receipt") from error
        if set(receipt) != {"schema", "version", "sourceCommit", "package"} or \
           receipt.get("schema") != 1 or receipt.get("version") != fields.get("build_id") or \
           receipt.get("sourceCommit") != fields.get("source_commit") or \
           receipt.get("package") != "native-candidate":
            raise ValueError("artifact build receipt disagrees with its immutable manifest")
        require(root, ["bin/blah2-gpu-worker", "libexec/vectorwarp", "libexec/vectorwarp-quiesce",
                       "libexec/vectorwarp-build-kraken.py"], executable=True)
        for notice in PLAN["notices"]:
            path = root / "licenses" / (notice + ".txt")
            if not path.is_file() or not path.stat().st_size:
                raise ValueError(f"artifact lacks third-party notice: {notice}")
        for path in root.rglob("*"):
            if any(fnmatch.fnmatch(path.name.lower(), pattern)
                   for pattern in ("*libsdrplay*", "sdrplay_api*.h", "sdrplay*.run")):
                raise ValueError("release artifact must not contain the SDRplay vendor SDK or runtime")
    if level != "build":
        hashes = IDENTITY["check_declared_hashes"](root.resolve(), allow_legacy=level == "install")
        fields["artifact_hash_provenance"] = "installed-byte-pinned" if hashes else "legacy-unpinned"
    return fields


def native_package(args, root=None, package_script=None, output_dir=None):
    """One Linux package lifecycle; callers supply only reviewed OS bindings."""
    root = ROOT if root is None else Path(root)
    output = Path(output_dir) if output_dir is not None else root / 'dist'
    pins = key_values(root / PLAN["node_pins"])
    version = pins["NODE_VERSION"]
    name = f"node-v{version}-linux-{args.node_arch}"
    with tempfile.TemporaryDirectory(prefix="vectorwarp-package-node-") as temporary:
        directory = Path(temporary)
        archive = directory / (name + ".tar.xz")
        subprocess.run(["curl", "--fail", "--location", "--proto", "=https", "--tlsv1.2",
                        pins["NODE_BASE_URL"] + "/" + archive.name, "--output", str(archive)], check=True)
        if IDENTITY["file_hash"](archive) != pins[f"NODE_{args.node_arch.upper()}_SHA256"]:
            raise ValueError("downloaded Node archive differs from reviewed pin")
        subprocess.run(["tar", "-C", temporary, "-xf", str(archive)], check=True)
        node = directory / name
        environment = {**os.environ, "PATH": str(node / "bin") + os.pathsep + os.environ["PATH"]}
        subprocess.run(["bash", str(root / "script/build-native.sh"), "--backend", "all", "--gpu", "auto",
                        "--build-dir", "build/package", "--jobs", "2", "--test"], cwd=root, env=environment, check=True)
        subprocess.run(["bash", str(package_script or root / "script/package-native.sh"), "--format", args.format,
                        "--artifact", "build/package/artifact", "--node-runtime", str(node),
                        "--version", args.version, "--distro", args.distro,
                        "--output-dir", str(output_dir) if output_dir is not None else "dist"],
                       cwd=root, env=environment, check=True)
        packages = list(output.glob("*." + args.format))
        if len(packages) != 1:
            raise ValueError("package output is not one exact native artifact")
        subprocess.run(["bash", str(root / "script/smoke-native-package.sh"), "--package", str(packages[0])],
                       cwd=root, env=environment, check=True)
        return packages[0]


def stage(source, output, platform):
    source, output = Path(source).resolve(strict=True), Path(output).absolute()
    entries = PLAN["stage"][platform]
    if platform != "replay-support":
        entries = PLAN["stage"]["common"] + entries
    if output == source or output == Path("/") or output in source.parents:
        raise ValueError("artifact output must be separate from source")
    for src, dst, mode, *options in entries:
        origin, destination = source / src, output / dst
        if output == origin or origin in output.parents:
            raise ValueError("artifact output overlaps staged source")
        if any(path.is_symlink() for path in (output, destination, *destination.parents)):
            raise ValueError("artifact staging destination must not follow symlinks")
        destination.parent.mkdir(parents=True, exist_ok=True)
        # macOS members roll back individually; Linux owns the outer cohort.
        temporary = Path(tempfile.mkdtemp(prefix=".artifact-", dir=destination.parent))
        staged, previous = temporary / "next", temporary / "previous"
        try:
            if mode is None:
                shutil.copytree(origin, staged, symlinks=True,
                                ignore=shutil.ignore_patterns(*(options[0] if options else [])))
                if src == "docs":
                    for path in (staged / "benchmarks").rglob("*"):
                        if path.is_file() and any(path.match(pattern) for pattern in PLAN["benchmark_excludes"]):
                            path.unlink()
            else:
                shutil.copy2(origin, staged)
                staged.chmod(int(mode, 8))
            if destination.exists():
                destination.rename(previous)
            try:
                staged.rename(destination)
            except Exception:
                if previous.exists():
                    previous.rename(destination)
                raise
        finally:
            # Preserve the old member if both publication and restoration fail.
            if not previous.exists() or destination.exists():
                shutil.rmtree(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pin = commands.add_parser("pin")
    pin.add_argument("name", choices=[*PLAN["dependencies"], "node"])
    dependency = commands.add_parser("checkout")
    dependency.add_argument("name", choices=PLAN["dependencies"])
    dependency.add_argument("directory", type=Path)
    dependency.add_argument("--offline", action="store_true")
    dependency.add_argument("--dry-run", action="store_true")
    commands.add_parser("matrix")
    distribution = commands.add_parser("distribution")
    distribution.add_argument("name", choices=PLAN["linux_distros"])
    target = commands.add_parser("target")
    target.add_argument("name", choices=PLAN["linux_distros"])
    target.add_argument("--format", choices=("deb", "rpm"), required=True)
    target.add_argument("--node-arch", choices=PLAN["linux_arches"], required=True)
    target.add_argument("--os-id", required=True)
    target.add_argument("--os-version", required=True)
    target.add_argument("--os-codename", default="")
    choose = commands.add_parser("profile")
    choose.add_argument("name", choices=PLAN["profiles"])
    choose.add_argument("--with-rspduo", action="store_true")
    copying = commands.add_parser("stage")
    copying.add_argument("--source", type=Path, default=ROOT)
    copying.add_argument("--output", type=Path, required=True)
    copying.add_argument("--platform", choices=("linux", "macos", "replay-support"), required=True)
    check = commands.add_parser("validate")
    check.add_argument("root", type=Path)
    check.add_argument("--level", choices=("build", "install", "package"), required=True)
    check.add_argument("--backend", choices=PLAN["profiles"])
    check.add_argument("--test-only", action="store_true")
    release = commands.add_parser("native-package")
    release.add_argument("--format", choices=("deb", "rpm"), required=True)
    release.add_argument("--distro", choices=PLAN["linux_distros"], required=True)
    release.add_argument("--version", required=True)
    release.add_argument("--node-arch", choices=("x64", "arm64"), required=True)
    args = parser.parse_args()
    try:
        if args.command == "pin":
            print(key_values(ROOT / PLAN["node_pins"])["NODE_VERSION"] if args.name == "node"
                  else PLAN["dependencies"][args.name]["commit"])
        elif args.command == "checkout":
            checkout(args.name, args.directory, args.offline, args.dry_run)
        elif args.command == "matrix":
            print(json.dumps(matrix(), separators=(",", ":")))
        elif args.command == "distribution":
            spec = PLAN["linux_distros"][args.name]
            print(spec["id"], spec["version"], spec["format"])
        elif args.command == "target":
            spec = PLAN["linux_distros"][args.name]
            if (args.format != spec["format"] or args.node_arch not in spec["arches"] or
                    args.os_id != spec["id"] or args.os_version != spec["version"] or
                    (spec["id"] == "debian" and args.os_codename != spec["codename"])):
                raise ValueError("package must be built on its exact reviewed distribution and architecture")
            print(spec["id"], spec["version"], spec["codename"] or "-", PLAN["linux_arches"][args.node_arch][args.format])
        elif args.command == "profile":
            compiled, local = profile(args.name, args.with_rspduo)
            print(*["ON" if name in compiled else "OFF" for name in ("RspDuo", "Usrp", "HackRF")],
                  "ON" if local else "OFF", ",".join(compiled), ",".join(local) or "-",
                  str(args.name != "all").lower())
        elif args.command == "stage":
            stage(args.source, args.output, args.platform)
        elif args.command == "native-package":
            native_package(args)
        else:
            fields = validate(args.root, args.level, args.backend, args.test_only)
            if args.level == "install":
                print(*[fields[name] or "-" for name in ("build_id", "backend", "compiled_receivers",
                                                        "local_build_receivers", "test_only")])
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
