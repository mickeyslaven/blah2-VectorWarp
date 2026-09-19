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
from pathlib import Path
import re
import shutil
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
    document = read_json(path)
    formulas = document.get("formulae") if isinstance(document, dict) else None
    if not isinstance(formulas, list) or len(formulas) != 1:
        raise ValueError("missing single cpp-httplib Homebrew formula")
    formula = formulas[0]
    version, expected_sha = CPP_HEADER[arch]
    source = formula.get("urls", {}).get("stable", {})
    expected_url = f"https://github.com/yhirose/cpp-httplib/archive/refs/tags/v{version}.tar.gz"
    if (formula.get("name") != "cpp-httplib" or
            formula.get("versions", {}).get("stable") != version or
            source.get("url") != expected_url or source.get("checksum") != expected_sha or
            formula.get("license") != "MIT"):
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


def verify_checkout(checkout, source_id):
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
        raise ValueError("build or vendored source changed; new source review required: " + ", ".join(guarded[:8]))
    workflow = (checkout / ".github/workflows/macos-standalone.yml").read_text()
    if BUILD_PIN not in workflow or "brew install cmake ninja pkgconf asio rapidyaml cpp-httplib" not in workflow:
        raise ValueError("hosted build inputs or VkFFT pin changed")
    return changed


def rebind_notices(baseline, output, source_id, runtimes):
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to replace notices output")
    old = read_json(baseline / "notices.json")
    if old.get("source_id") != BASELINE_SOURCE_ID or old.get("schema") != 1:
        raise ValueError("unexpected baseline notices identity")
    before = {p.relative_to(baseline).as_posix(): sha256(p)
              for p in baseline.rglob("*") if p.is_file() and p.name != "notices.json"}
    if before != old.get("files"):
        raise ValueError("baseline notices content differs from its reviewed manifest")
    shutil.copytree(baseline, output, symlinks=False)
    index = read_json(output / "INDEX.json")
    if index.get("source_id") != BASELINE_SOURCE_ID:
        raise ValueError("unexpected baseline notice index")
    matches = [item for item in index.get("input_notices", [])
               if item.get("file") == "embedded/cpp-httplib/LICENSE"]
    if len(matches) != 1 or matches[0].get("sha256") != CPP_LICENSE_SHA256:
        raise ValueError("reviewed cpp-httplib notice index changed")
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


def source_archive(args, source_id, runtimes, output, intel_cpp):
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
             "cpp-httplib-formula-arm64.json": Path(args.arm64_cpp_formula),
             "cpp-httplib-formula-x86_64.json": Path(args.x86_64_cpp_formula)}
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
              "for provenance. No signing key, SDK, runtime binary or notarization credential is here.\n")
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


def prepare(args):
    output = Path(args.output)
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to replace existing release input directory")
    runtimes = {arch: Path(getattr(args, arch)).resolve(strict=True) for arch in package.ARCHES}
    manifests = {arch: package.audit_runtime(path, arch) for arch, path in runtimes.items()}
    ids = {value["source_id"] for value in manifests.values()}
    if len(ids) != 1:
        raise ValueError("the two runtimes have different source commits")
    source_id = ids.pop()
    verify_checkout(Path(args.checkout), source_id)
    for arch in package.ARCHES:
        verify_inventory(getattr(args, f"{arch}_inventory"),
                         getattr(args, f"baseline_{arch}_inventory"), runtimes[arch], source_id, arch)
        parse_header_versions(getattr(args, f"{arch}_header_versions"), arch)
        verify_cpp_formula(getattr(args, f"{arch}_cpp_formula"), arch)
    output.mkdir(parents=True)
    try:
        intel_cpp = fetch_intel_cpp_source(
            output, verify_cpp_formula(args.x86_64_cpp_formula, "x86_64"), Path(args.baseline_notices))
        rebind_notices(Path(args.baseline_notices), output / "notices", source_id, runtimes)
        archive = source_archive(args, source_id, runtimes, output, intel_cpp)
        (output / "receipt.json").write_text(json.dumps({"schema": 1, "source_id": source_id,
            "version": args.version, "source_archive": archive.name,
            "source_archive_sha256": sha256(archive), "source_archive_size": archive.stat().st_size,
            "notices_sha256": sha256(output / "notices/notices.json")}, sort_keys=True, indent=2) + "\n")
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
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("version must be semver")
    try:
        prepare(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
