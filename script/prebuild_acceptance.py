#!/usr/bin/env python3
"""Run the bounded source gate before a configured all-target native build.

This is a source/contract gate.  Its receipt deliberately does not claim RF,
physical-device, or radar-behaviour qualification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
NATIVE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".cmake"}
FAST_CHECKS = (
    ("prebuild_contract", ("python3", "-m", "unittest", "discover", "-s", "test/script",
                           "-p", "test_prebuild_acceptance.py")),
    ("repository_budget", ("python3", "script/check_repository_budget.py")),
    ("canonical_architecture", ("python3", "-m", "unittest", "discover", "-s", "test/script",
                                "-p", "test_canonical_processing_architecture.py")),
    ("ci_contract", ("python3", "test/packaging/test_ci_workflows.py")),
    ("api_contract", ("node", "--test", "api/heimdall-timing.test.js")),
    ("effective_radar_plan", ("node", "--test", "api/effective-radar-plan.test.js")),
    ("ui_contract", ("node", "test/ui/deployment.test.js")),
)


def native_path(path):
    item = Path(path)
    return (item.name == "CMakeLists.txt" or item.suffix in NATIVE_SUFFIXES
            or item.parts[:1] in (("src",), ("core",), ("cmake",)))


def invoke(command, root):
    result = subprocess.run(command, cwd=root, text=True, capture_output=True)
    return {"command": list(command), "returncode": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def source_hash(root):
    result = subprocess.run(("git", "ls-files", "--cached", "--others",
                             "--exclude-standard", "-z"), cwd=root, capture_output=True)
    if result.returncode:
        return None
    digest = hashlib.sha256()
    for encoded in sorted(item for item in result.stdout.split(b"\0") if item):
        path = encoded.decode("utf-8")
        target = root / path
        digest.update(encoded + b"\0")
        if not target.exists() and not target.is_symlink():
            digest.update(b"<deleted>")
        else:
            digest.update(target.readlink().as_posix().encode() if target.is_symlink()
                          else target.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def changed_paths(root):
    paths = set()
    for command in (("git", "diff", "--name-only", "--cached"),
                    ("git", "diff", "--name-only"),
                    ("git", "ls-files", "--others", "--exclude-standard")):
        result = subprocess.run(command, cwd=root, text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError("cannot inspect changed paths: " + result.stderr.strip())
        paths.update(line for line in result.stdout.splitlines() if line)
    return sorted(paths)


def write_receipt(path, receipt):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")


def main(argv=None, root=ROOT, command=invoke, changes=changed_paths):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="already configured CMake build directory")
    parser.add_argument("--native-target", action="append", default=[],
                        help="affected native target; repeat for each target")
    parser.add_argument("--native-test", action="append", default=[],
                        help="affected CTest name; repeat for each focused test")
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--skip-target-build", action="store_true",
                        help="run source checks only (for script tests)")
    args = parser.parse_args(argv)
    targets = [target for target in args.native_target if target.strip()]
    tests = [test for test in args.native_test if test.strip()]
    receipt = {"kind": "prebuild_acceptance", "source_hash": source_hash(root),
               "scope": "source and contract checks only; not radar or hardware qualification",
               "commands": [], "outcome": "failed"}
    try:
        changed = changes(root)
        receipt["changed_paths"] = changed
        if any(native_path(path) for path in changed) and (not targets or not tests):
            raise ValueError("native changes require non-empty --native-target and --native-test declarations")
        checks = [("diff_check_cached", ("git", "diff", "--check", "--cached")),
                  ("diff_check", ("git", "diff", "--check")), *FAST_CHECKS]
        if not args.skip_target_build:
            cache = args.build_dir / "CMakeCache.txt"
            enabled = cache.is_file() and re.search(
                r"^CMAKE_COMPILE_WARNING_AS_ERROR(?::[^=]+)?=ON$", cache.read_text(), re.M)
            if not enabled:
                raise ValueError("configured build must set CMAKE_COMPILE_WARNING_AS_ERROR=ON")
            checks.extend((f"native_target:{target}",
                           ("cmake", "--build", str(args.build_dir), "--target", target))
                          for target in targets)
            checks.extend((f"native_test:{test}",
                           ("ctest", "--test-dir", str(args.build_dir), "-R", f"^{re.escape(test)}$",
                            "--output-on-failure", "--no-tests=error")) for test in tests)
        for name, line in checks:
            result = command(line, root)
            result["name"] = name
            receipt["commands"].append(result)
            if result["returncode"]:
                raise RuntimeError(f"{name} failed")
        receipt["outcome"] = "passed"
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        receipt["error"] = str(error)
        return 1
    finally:
        write_receipt(args.receipt, receipt)


if __name__ == "__main__":
    sys.exit(main())
