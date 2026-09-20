#!/usr/bin/env python3
"""Rebind reviewed macOS source/notices only when build inputs are unchanged.

This is deliberately a narrow fast path. New formula recipes, source hashes,
embedded-header versions, npm lock data, or build wiring require a new source
review; this program must not silently publish an old source kit for them.
"""
import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("macos_packager", ROOT / "script/package-macos-standalone.py")
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)
BASELINE_SOURCE_SHA256 = "193456313f51fc6d330c4e8332beec4957adfad8a80e4a4d81dea6fd7fbb19cd"
BASELINE_SOURCE_ID = "f8135ac947a88dd416b0f0ae8d3d891dd546da1d"
BASELINE_NPM_LOCK_SHA256 = "de38adb43b306b81ee4aac67dbc913927a6dfef1bd062d3a8bb8659956518c2d"
HEADER_VERSIONS = {"asio": "1.38.2", "rapidjson": "1.1.0",
                   "vulkan-headers": "1.4.357.0"}
CPP_HEADER = {
    "arm64": ("0.54.1", "7310f5312e1423830d649b38ed028e9db86303a979ccbfdbd1c4b1574f422dfb"),
    "x86_64": ("0.53.1", "185af9587e270de9a3bfee234c6740f02e82265da33c7a41f97e02ee42f979d2"),
}
CPP_LICENSE_SHA256 = "4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b"
BUILD_PIN = "066a17c17068c0f11c9298d848c2976c71fad1c1"
SENSITIVE_PATHS = ("CMakeLists.txt", "cmake/", "script/build-macos.sh",
                   "third_party/", "third-party/", "vendor/")
REVIEW_PURPOSE = "first-party-build-only"
REVIEW_ARCHIVE_MEMBER = "VectorWarp-corresponding-source/build-source-review.json"
REVIEW_MAX_BYTES = 64 * 1024
REVIEW_PATH = re.compile(r"(?:CMakeLists\.txt|cmake/[A-Za-z0-9][A-Za-z0-9._-]*\.cmake)")


def sha256(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text())


def source_claims(inventory):
    if inventory.get("schema") != 1 or not isinstance(inventory.get("components"), list):
        raise ValueError("invalid CI dependency inventory")
    result = {}
    for component in inventory["components"]:
        recipe = component.get("formula_recipe") or {}
        key = (component.get("formula"), component.get("version"), component.get("input"))
        claim = (recipe.get("sha256"), tuple(sorted(recipe.get("source_sha256") or [])),
                 tuple(sorted((item["path"], item["sha256"])
                              for item in component.get("notice_files", []))),
                 tuple(sorted((item["path"], item["sha256"])
                              for item in component.get("copied_files", []))))
        if not all(isinstance(value, str) and value for value in key) or key in result:
            raise ValueError("invalid or duplicate CI formula identity")
        if not re.fullmatch(r"[0-9a-f]{64}", claim[0] or "") or not claim[1]:
            raise ValueError("CI formula source/recipe evidence is incomplete")
        result[key] = claim
    if not result:
        raise ValueError("empty CI dependency inventory")
    return result


def parse_header_versions(path, arch):
    result = {}
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if len(parts) != 2 or parts[0] in result:
            raise ValueError("invalid or duplicate Homebrew header version")
        result[parts[0]] = parts[1]
    if (any(result.get(name) != version for name, version in HEADER_VERSIONS.items()) or
            result.get("cpp-httplib") != CPP_HEADER[arch][0]):
        raise ValueError("embedded-header input version changed; new source review required")
    return result


def verify_cpp_formula(path, arch):
    recipe = Path(path).read_text()
    version, expected_sha = CPP_HEADER[arch]
    expected_url = f"https://github.com/yhirose/cpp-httplib/archive/refs/tags/v{version}.tar.gz"
    url = re.findall(r'^\s*url "([^"]+)"\s*$', recipe, re.MULTILINE)
    sha = re.findall(r'^\s*sha256 "([0-9a-f]{64})"\s*$', recipe, re.MULTILINE)
    license_name = re.findall(r'^\s*license "([^"]+)"\s*$', recipe, re.MULTILINE)
    if ("class CppHttplib < Formula" not in recipe or
            url != [expected_url] or sha != [expected_sha] or license_name != ["MIT"]):
        raise ValueError(f"{arch} cpp-httplib formula source, version, or license changed")
    return expected_url


def fetch_intel_cpp_source(output, url, baseline_notices):
    archive = output / "cpp-httplib-0.53.1.tar.gz"
    with urllib.request.urlopen(url, timeout=60) as response, archive.open("xb") as stream:
        shutil.copyfileobj(response, stream)
    if sha256(archive) != CPP_HEADER["x86_64"][1]:
        raise ValueError("Intel cpp-httplib source archive hash differs from formula")
    with tarfile.open(archive, "r:gz") as source:
        member = source.extractfile("cpp-httplib-0.53.1/LICENSE")
        if member is None:
            raise ValueError("Intel cpp-httplib source has no LICENSE")
        license_sha = hashlib.sha256(member.read()).hexdigest()
    if (license_sha != CPP_LICENSE_SHA256 or
            sha256(baseline_notices / "embedded/cpp-httplib/LICENSE") != license_sha):
        raise ValueError("Intel cpp-httplib license differs from reviewed notice")
    return archive


def verify_inventory(path, baseline, runtime, source_id, arch):
    candidate = read_json(path)
    prior = read_json(baseline)
    if (candidate.get("source_id") != source_id or candidate.get("arch") != arch or
            candidate.get("runtime_manifest_sha256") != sha256(runtime / "standalone.json")):
        raise ValueError(f"{arch} CI inventory does not bind the candidate runtime")
    if prior.get("source_id") != BASELINE_SOURCE_ID or prior.get("arch") != arch:
        raise ValueError(f"{arch} baseline inventory identity changed")
    if source_claims(candidate) != source_claims(prior):
        raise ValueError(f"{arch} formula source/recipe/notice inputs changed; new source review required")
    if candidate.get("gaps") != prior.get("gaps") or candidate.get("status") != prior.get("status"):
        raise ValueError(f"{arch} CI provenance has new gaps or review status")


def review_json(raw):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("review receipt has duplicate JSON keys")
            result[key] = value
        return result
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("review receipt is not valid UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise ValueError("review receipt must be a JSON object")
    return value


def owner_only_bytes(path):
    path = Path(path)
    before = path.lstat()
    if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid() or
            before.st_mode & 0o077 or before.st_size > REVIEW_MAX_BYTES):
        raise ValueError("review receipt must be a bounded owner-only regular file")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        after = os.fstat(descriptor)
        if (not stat.S_ISREG(after.st_mode) or after.st_uid != os.getuid() or
                after.st_mode & 0o077 or after.st_size != before.st_size or
                (after.st_dev, after.st_ino) != (before.st_dev, before.st_ino)):
            raise ValueError("review receipt changed while being read")
        raw = os.read(descriptor, REVIEW_MAX_BYTES + 1)
    finally:
        os.close(descriptor)
    if len(raw) != before.st_size or len(raw) > REVIEW_MAX_BYTES:
        raise ValueError("review receipt exceeds its bounded size")
    return raw


def reviewed_build_changes(checkout, source_id, guarded, receipt_path):
    """Validate reviewed CMake post-images for this candidate and return immutable bytes."""
    if not isinstance(source_id, str) or not re.fullmatch(r"[0-9a-f]{40}", source_id):
        raise ValueError("review receipt source commit is invalid")
    guarded = sorted(guarded)
    if not guarded or len(set(guarded)) != len(guarded) or any(not REVIEW_PATH.fullmatch(path) for path in guarded):
        raise ValueError("review receipt cannot approve non-CMake guarded paths")
    raw = owner_only_bytes(receipt_path)
    record = review_json(raw)
    if set(record) != {"schema", "purpose", "source_id", "baseline_source_id", "files"}:
        raise ValueError("review receipt has an unexpected schema")
    reviewed_source_id = record["source_id"]
    if (record["schema"] != 1 or record["purpose"] != REVIEW_PURPOSE or
            not isinstance(reviewed_source_id, str) or not re.fullmatch(r"[0-9a-f]{40}", reviewed_source_id) or
            record["baseline_source_id"] != BASELINE_SOURCE_ID):
        raise ValueError("review receipt does not bind a reviewed source and baseline")
    try:
        reviewed_type = subprocess.check_output(["git", "-C", str(checkout), "cat-file", "-t",
                                                 reviewed_source_id], stderr=subprocess.DEVNULL, text=True).strip()
    except subprocess.CalledProcessError as error:
        raise ValueError("review receipt reviewed source commit is unavailable") from error
    if reviewed_type != "commit":
        raise ValueError("review receipt reviewed source must identify a commit directly")
    if subprocess.run(["git", "-C", str(checkout), "merge-base", "--is-ancestor",
                       reviewed_source_id, source_id], capture_output=True, check=False).returncode:
        raise ValueError("review receipt source is not an ancestor of the candidate")
    reviewed_changed = subprocess.check_output(
        ["git", "-C", str(checkout), "diff", "--name-only", BASELINE_SOURCE_ID, reviewed_source_id],
        text=True).splitlines()
    reviewed_guarded = sorted(name for name in reviewed_changed
                              if any(name == prefix or name.startswith(prefix)
                                     for prefix in SENSITIVE_PATHS))
    candidate_changed = subprocess.check_output(
        ["git", "-C", str(checkout), "diff", "--name-only", BASELINE_SOURCE_ID, source_id],
        text=True).splitlines()
    candidate_guarded = sorted(name for name in candidate_changed
                               if any(name == prefix or name.startswith(prefix)
                                      for prefix in SENSITIVE_PATHS))
    if reviewed_guarded != guarded or candidate_guarded != guarded:
        raise ValueError("review receipt guarded paths differ from the candidate")
    files = record["files"]
    if not isinstance(files, dict) or set(files) != set(guarded):
        raise ValueError("review receipt files do not exactly match guarded changes")
    for path, expected in files.items():
        if not REVIEW_PATH.fullmatch(path) or not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("review receipt has an unsafe path or invalid file digest")
        for commit, label in ((reviewed_source_id, "reviewed"), (source_id, "candidate")):
            try:
                blob = subprocess.check_output(["git", "-C", str(checkout), "show", commit + ":" + path],
                                               stderr=subprocess.DEVNULL)
            except subprocess.CalledProcessError as error:
                raise ValueError("review receipt path is absent from " + label + " source: " + path) from error
            if hashlib.sha256(blob).hexdigest() != expected:
                raise ValueError("review receipt file digest differs from " + label + " Git blob: " + path)
    metadata = {"sha256": hashlib.sha256(raw).hexdigest(), "archive_member": REVIEW_ARCHIVE_MEMBER,
                "source_id": source_id, "baseline_source_id": BASELINE_SOURCE_ID}
    if reviewed_source_id != source_id:
        metadata["reviewed_source_id"] = reviewed_source_id
    return {"record": record, "raw": raw, "metadata": metadata}


def verify_checkout(checkout, source_id, review_receipt=None):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(checkout), *args], text=True).strip()
    if git("rev-parse", "HEAD") != source_id or git("status", "--porcelain"):
        raise ValueError("source checkout is not clean at the exact candidate commit")
    if sha256(checkout / "api/package-lock.json") != BASELINE_NPM_LOCK_SHA256:
        raise ValueError("npm dependency lock changed; new source/notices review required")
    try:
        git("cat-file", "-e", BASELINE_SOURCE_ID + "^{commit}")
    except subprocess.CalledProcessError:
        git("fetch", "--no-tags", "origin", BASELINE_SOURCE_ID)
    changed = git("diff", "--name-only", BASELINE_SOURCE_ID, source_id).splitlines()
    # Signing entitlements/package wiring are included in the current first-party
    # archive; only native dependency/build inputs block source-kit reuse here.
    guarded = [name for name in changed if any(name == prefix or name.startswith(prefix)
                                               for prefix in SENSITIVE_PATHS)]
    if guarded:
        if review_receipt is None:
            raise ValueError("build or vendored source changed; new source review required: " + ", ".join(guarded[:8]))
        reviewed_build_changes(checkout, source_id, guarded, review_receipt)
    elif review_receipt is not None:
        raise ValueError("review receipt is present without guarded build changes")
    workflow = (checkout / ".github/workflows/macos-standalone.yml").read_text()
    if BUILD_PIN not in workflow or "brew install cmake ninja pkgconf asio rapidyaml cpp-httplib" not in workflow:
        raise ValueError("hosted build inputs or VkFFT pin changed")
    return changed


def rebind_notices(baseline, output, source_id, runtimes):
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to replace notices output")
    validated_baseline_notices(baseline)
    shutil.copytree(baseline, output, symlinks=False)
    index = read_json(output / "INDEX.json")
    matches = [item for item in index.get("input_notices", [])
               if item.get("file") == "embedded/cpp-httplib/LICENSE"]
    matches[0]["origin"] += "; cpp-httplib-0.53.1.tar.gz::cpp-httplib-0.53.1/LICENSE (Intel)"
    index["source_id"] = source_id
    (output / "INDEX.json").write_text(json.dumps(index, sort_keys=True, indent=2) + "\n")
    notice = {"schema": 1, "source_id": source_id,
              "review_status": "automated-unchanged-dependency-inputs",
              "runtime_manifest_sha256": {arch: sha256(path / "standalone.json")
                                          for arch, path in runtimes.items()},
              "files": {p.relative_to(output).as_posix(): sha256(p)
                        for p in output.rglob("*") if p.is_file() and p.name != "notices.json"}}
    (output / "notices.json").write_text(json.dumps(notice, sort_keys=True, indent=2) + "\n")
    package.validated_notices(output, {arch: read_json(path / "standalone.json")
                                       for arch, path in runtimes.items()}, runtimes)


def validated_baseline_notices(baseline):
    baseline = Path(baseline)
    old = read_json(baseline / "notices.json")
    if old.get("source_id") != BASELINE_SOURCE_ID or old.get("schema") != 1:
        raise ValueError("unexpected baseline notices identity")
    before = {p.relative_to(baseline).as_posix(): sha256(p)
              for p in baseline.rglob("*") if p.is_file() and p.name != "notices.json"}
    if before != old.get("files"):
        raise ValueError("baseline notices content differs from its reviewed manifest")
    index = read_json(baseline / "INDEX.json")
    if index.get("source_id") != BASELINE_SOURCE_ID:
        raise ValueError("unexpected baseline notice index")
    matches = [item for item in index.get("input_notices", [])
               if item.get("file") == "embedded/cpp-httplib/LICENSE"]
    if len(matches) != 1 or matches[0].get("sha256") != CPP_LICENSE_SHA256:
        raise ValueError("reviewed cpp-httplib notice index changed")


def add_bytes(archive, name, content):
    info = tarfile.TarInfo(name)
    info.size = len(content)
    info.mode = 0o644
    info.mtime = 0
    archive.addfile(info, io.BytesIO(content))


def add_file(archive, name, source):
    info = tarfile.TarInfo(name)
    info.size = source.stat().st_size
    info.mode = 0o644
    info.mtime = 0
    with source.open("rb") as stream:
        archive.addfile(info, stream)


def source_archive(args, source_id, runtimes, output, intel_cpp, review=None):
    baseline = Path(args.baseline_source_archive)
    if sha256(baseline) != BASELINE_SOURCE_SHA256:
        raise ValueError("accepted baseline source archive changed")
    first_party = output / "VectorWarp-current-source.tar.gz"
    with first_party.open("xb") as stream:
        subprocess.run(["git", "-C", str(args.checkout), "archive", "--format=tar.gz",
                        "--prefix=VectorWarp-current-source/", source_id], check=True, stdout=stream)
    items = {"dependency-source-baseline-v0.1.9.tar.gz": baseline,
             "VectorWarp-current-source.tar.gz": first_party,
             "cpp-httplib-0.53.1.tar.gz": intel_cpp,
             "runtime-arm64-standalone.json": runtimes["arm64"] / "standalone.json",
             "runtime-x86_64-standalone.json": runtimes["x86_64"] / "standalone.json",
             "inventory-arm64.json": Path(args.arm64_inventory),
             "inventory-x86_64.json": Path(args.x86_64_inventory),
             "header-input-versions-arm64.txt": Path(args.arm64_header_versions),
             "header-input-versions-x86_64.txt": Path(args.x86_64_header_versions),
             "cpp-httplib-formula-arm64.rb": Path(args.arm64_cpp_formula),
             "cpp-httplib-formula-x86_64.rb": Path(args.x86_64_cpp_formula)}
    if review is not None:
        items["build-source-review.json"] = output / "public-build-source/build-source-review.json"
    manifest = {"schema": 1, "source_id": source_id, "version": args.version,
                "baseline_source_id": BASELINE_SOURCE_ID,
                "files": {name: {"sha256": sha256(path), "size": path.stat().st_size}
                          for name, path in items.items()}}
    readme = ("Current first-party source is VectorWarp-current-source.tar.gz at commit " + source_id +
              ". The nested v0.1.9 source kit supplies unchanged dependency sources, recipes, patches, "
              "licenses and build instructions. It is reused only after both architecture inventories, "
              "embedded-header versions and npm lock passed exact-input checks. Intel uses "
              "cpp-httplib 0.53.1; its separately verified source archive is alongside this README, "
              "with the same MIT license text as the 0.54.1 arm64 notice. Extract the baseline kit, "
              "then use the current first-party checkout for the build; do not use its historical "
              "f813 first-party archive. Current runtime manifests and CI inventories are included "
              "for provenance. No signing key, SDK, runtime binary or notarization credential is here." +
              (" build-source-review.json records the reviewed first-party CMake delta for this tag.\n"
               if review is not None else "\n"))
    archive_path = output / f"vectorwarp-{args.version}-macos-corresponding-source.tar.gz"
    manifest_bytes = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
    with archive_path.open("xb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="",
                                                      compresslevel=1, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|") as archive:
            add_bytes(archive, "VectorWarp-corresponding-source/README.txt", readme.encode())
            add_bytes(archive, "VectorWarp-corresponding-source/MANIFEST.json", manifest_bytes)
            for name, path in sorted(items.items()):
                add_file(archive, "VectorWarp-corresponding-source/" + name, path)
    expected = {name: record["sha256"] for name, record in manifest["files"].items()}
    expected["README.txt"] = hashlib.sha256(readme.encode()).hexdigest()
    expected["MANIFEST.json"] = hashlib.sha256(manifest_bytes).hexdigest()
    seen = set()
    with tarfile.open(archive_path, "r|gz") as archive:
        for member in archive:
            if not member.isfile() or not member.name.startswith("VectorWarp-corresponding-source/"):
                raise ValueError("unsafe corresponding-source archive member")
            name = member.name.removeprefix("VectorWarp-corresponding-source/")
            if name not in expected or name in seen:
                raise ValueError("unexpected or duplicate corresponding-source member")
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError("unreadable corresponding-source member")
            value = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                value.update(block)
            if value.hexdigest() != expected[name]:
                raise ValueError("corresponding-source member digest differs: " + name)
            seen.add(name)
    if seen != set(expected):
        raise ValueError("corresponding-source archive is missing members")
    first_party.unlink()
    intel_cpp.unlink()
    return archive_path


def verify_inputs(args):
    baseline_source = Path(args.baseline_source_archive)
    if sha256(baseline_source) != BASELINE_SOURCE_SHA256:
        raise ValueError("accepted baseline source archive changed")
    validated_baseline_notices(Path(args.baseline_notices))
    runtimes = {arch: Path(getattr(args, arch)).resolve(strict=True) for arch in package.ARCHES}
    manifests = {arch: package.audit_runtime(path, arch) for arch, path in runtimes.items()}
    ids = {value["source_id"] for value in manifests.values()}
    if len(ids) != 1:
        raise ValueError("the two runtimes have different source commits")
    source_id = ids.pop()
    review_receipt = getattr(args, "review_receipt", None)
    changed = verify_checkout(Path(args.checkout), source_id, review_receipt)
    guarded = [name for name in changed if any(name == prefix or name.startswith(prefix)
                                               for prefix in SENSITIVE_PATHS)]
    review = (reviewed_build_changes(Path(args.checkout), source_id, guarded, review_receipt)
              if review_receipt is not None else None)
    for arch in package.ARCHES:
        verify_inventory(getattr(args, f"{arch}_inventory"),
                         getattr(args, f"baseline_{arch}_inventory"), runtimes[arch], source_id, arch)
        parse_header_versions(getattr(args, f"{arch}_header_versions"), arch)
        verify_cpp_formula(getattr(args, f"{arch}_cpp_formula"), arch)
    return runtimes, source_id, review


def persist_review(output, review):
    directory = output / "public-build-source"
    directory.mkdir(mode=0o700)
    directory.chmod(0o700)
    path = directory / "build-source-review.json"
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        offset = 0
        while offset < len(review["raw"]):
            offset += os.write(descriptor, review["raw"][offset:])
    finally:
        os.close(descriptor)
    if sha256(path) != review["metadata"]["sha256"]:
        raise ValueError("persisted review receipt digest differs")
    return path


def prepare(args):
    output = Path(args.output)
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to replace existing release input directory")
    runtimes, source_id, review = verify_inputs(args)
    output.mkdir(parents=True)
    try:
        intel_cpp = fetch_intel_cpp_source(
            output, verify_cpp_formula(args.x86_64_cpp_formula, "x86_64"), Path(args.baseline_notices))
        rebind_notices(Path(args.baseline_notices), output / "notices", source_id, runtimes)
        if review is not None:
            persist_review(output, review)
        archive = source_archive(args, source_id, runtimes, output, intel_cpp, review)
        receipt = {"schema": 1, "source_id": source_id,
            "version": args.version, "source_archive": archive.name,
            "source_archive_sha256": sha256(archive), "source_archive_size": archive.stat().st_size,
            "notices_sha256": sha256(output / "notices/notices.json")}
        if review is not None:
            receipt["build_source_review"] = review["metadata"]
        (output / "receipt.json").write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
    except Exception:
        shutil.rmtree(output)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("arm64", "x86_64", "arm64-inventory", "x86_64-inventory",
                 "arm64-header-versions", "x86_64-header-versions",
                 "arm64-cpp-formula", "x86_64-cpp-formula",
                 "baseline-arm64-inventory", "baseline-x86_64-inventory",
                 "baseline-notices", "baseline-source-archive", "checkout", "version", "output"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--review-receipt", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("version must be semver")
    try:
        prepare(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
