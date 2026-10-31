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
REVIEWED_NPM_LOCK_SHA256 = "0fa96e32fbe379ee7f51e26904fa4c0865273d8bec881f8beb53aa05177a1849"
PROXY_ADDR_REVIEW = {
    "old_version": "2.0.7",
    "old_resolved": "https://registry.npmjs.org/proxy-addr/-/proxy-addr-2.0.7.tgz",
    "old_integrity": "sha512-llQsMLSUDUPT44jdrU/O37qlnifitDP+ZwrmmZcoSKyLKvtZxpyV0n2/bD/N4tBAAZ/gJEdZU7KMraoK1+XYAg==",
    "version": "2.0.8",
    "resolved": "https://registry.npmjs.org/proxy-addr/-/proxy-addr-2.0.8.tgz",
    "integrity": "sha512-5nnx0yGyVUcY6t9RnWcARWtwT9F1D8O9rt08htPvnd49W1IgZtmLkhu9WfMzQj1cFxjHIO6connUNVW5k7AVyQ==",
    "archive_sha256": "ffcc8055b78b0852b2889a426ad3a3003b60bc79df704faca194157efa2b2587",
    "license_sha256": "a7996721249a136fbcfc1c201bcb5f414f38a701d4a24f28abe65e16cfcce813",
}
NODE_REVIEW = {
    "version": "24.22.1",
    "base_url": "https://nodejs.org/dist/v24.22.1",
    "source_sha256": "001773d9bd53fbd27a25efd42aa6a9693ebfe500998ca9bec635e9b6ee79bc91",
    "darwin_arm64_sha256": "89c682593d76cd44233fa53b1563264e7de9cbb189cad169135c5b4fadfbc0a7",
    "darwin_x64_sha256": "d132557e7ee2b85af08b0c8c34338e65c6430f2238d01f727728ef42dc83b035",
    "binary_sha256": {
        "arm64": "6f32069480ec14d52b6d7b1f3c7124cfbbc46203abf5ddf5c65b127435f3f329",
        "x86_64": "cc6f41f72e3eb4460154952e56a35afdd96b6995cea51cd25e637355a2955a51",
    },
    "staged_binary_sha256": {
        "arm64": "614a59e57b84783a088d8f1da0d1d2d7023348d04a4453794d6ce0ac4328c269",
        "x86_64": "632ad205dfab5ba73e3eb91fe257de5eb986c42ba57368b31ba71a061d76d121",
    },
    "license_sha256": "5888dbb9a1d2b18f2c3e6c5f6af1b39de658372b402a0577b002777f14c62ace",
}
NODE_GAP = {"index": 0, "reason": "dependency lacks formula, version, file, or sha256"}
HEADER_VERSIONS = {"asio": "1.38.2", "rapidjson": "1.1.0",
                   "vulkan-headers": "1.4.357.0"}
CPP_HEADER = {
    "arm64": ("0.54.1", "7310f5312e1423830d649b38ed028e9db86303a979ccbfdbd1c4b1574f422dfb"),
    "x86_64": ("0.53.1", "185af9587e270de9a3bfee234c6740f02e82265da33c7a41f97e02ee42f979d2"),
}
CPP_LICENSE_SHA256 = "4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b"
VECTORWARP_UHD_REVIEW = {
    "formula": "vectorwarp-uhd",
    "version": "4.11.0.0",
    "input": "lib/libuhd.4.11.0.dylib",
    "formula_sha256": "9a938ae4e10cf0e699ff95e072f303fe1aa3333246b078e8aa223b01e533038d",
    "sources": (
        ("uhd-4.11.0.0.tar.gz",
         "https://github.com/EttusResearch/uhd/archive/refs/tags/v4.11.0.0.tar.gz",
         "1e53faec13ea2be9dd8f765956157d1434f41fa0a124fac8f7b2340f8445b026"),
        ("mako-1.4.3.tar.gz",
         "https://files.pythonhosted.org/packages/5a/09/e07c4b5579a79f4b16f8d4f29f6c54514ac787c4ad506b8c4f28a0e6b0bf/mako-1.4.3.tar.gz",
         "cd6537fe88d5fec315c55c2f8529bc4ce7a9a352ad7db3eeaa6a66e2dd4ec37a"),
        ("markupsafe-3.0.4.tar.gz",
         "https://files.pythonhosted.org/packages/38/9b/e422a865e1d5d57d0e509b4e0bf1c1a70a7f6382c29a5aa428df994c8bc8/markupsafe-3.0.4.tar.gz",
         "2e9ad7dd851bf45fab9f75cbff4cb493fee9979e8d8c7c9c3ee119022518edd6"),
        ("uhd-macos-namespace-2dc0ecf.patch",
         "https://github.com/EttusResearch/uhd/commit/2dc0ecf9288a2556da762fd6693600f1572ecd2d.patch?full_index=1",
         "4e6bf9735767e245c87b57221c54e623cb76de3f9fa957787435b1ba20a2065c"),
    ),
    "notices": (
        ("LICENSE.md", "94cc36ada2641d037980bf6c2418a750ac10a115ae11de5a5cda6177769dbd4c"),
        ("share/doc/uhd/LICENSE", "70bf7e79c8cd73a81f97ce81745ea0719a617eebe299a61868165daeae71fff2"),
    ),
}
BUILD_PIN = "066a17c17068c0f11c9298d848c2976c71fad1c1"
SENSITIVE_PATHS = ("CMakeLists.txt", "cmake/", "script/build-macos.sh",
                   "packaging/homebrew/", ".github/workflows/macos-standalone.yml",
                   "third_party/", "third-party/", "vendor/")
REVIEW_PURPOSE = "first-party-build-only"
REVIEW_ARCHIVE_MEMBER = "VectorWarp-corresponding-source/build-source-review.json"
REVIEW_MAX_BYTES = 64 * 1024
REVIEW_PATH = re.compile(
    r"(?:CMakeLists\.txt|script/build-macos\.sh|packaging/homebrew/vectorwarp-uhd\.rb|"
    r"\.github/workflows/macos-standalone\.yml|cmake/ReceiverCohort\.h\.in|"
    r"cmake/[A-Za-z0-9][A-Za-z0-9._-]*\.cmake)")


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


def verify_source_claim_transition(candidate, prior, arch):
    """Accept removed runtime inputs, but never new or changed source claims."""
    candidate_claims = source_claims(candidate)
    prior_claims = source_claims(prior)
    forbidden = {"grpc", "protobuf"}
    if any(key[0] in forbidden for key in candidate_claims):
        raise ValueError(f"{arch} standalone runtime unexpectedly contains the UHD MPMD dependency stack")
    expected_notices = tuple(VECTORWARP_UHD_REVIEW["notices"])
    expected_copies = tuple(sorted((
        (f".brew/{VECTORWARP_UHD_REVIEW['formula']}.rb", VECTORWARP_UHD_REVIEW["formula_sha256"]),
        *expected_notices,
    )))
    expected_uhd_claim = (
        VECTORWARP_UHD_REVIEW["formula_sha256"],
        tuple(sorted(item[2] for item in VECTORWARP_UHD_REVIEW["sources"])),
        expected_notices,
        expected_copies,
    )
    legacy_uhd_present = any(key[0] == "uhd" for key in prior_claims)
    changed = []
    for key, claim in candidate_claims.items():
        if prior_claims.get(key) == claim:
            continue
        reviewed_key = (VECTORWARP_UHD_REVIEW["formula"], VECTORWARP_UHD_REVIEW["version"],
                        VECTORWARP_UHD_REVIEW["input"])
        if key == reviewed_key and legacy_uhd_present and claim == expected_uhd_claim:
            continue
        changed.append(key)
    if changed:
        raise ValueError(
            f"{arch} formula source/recipe/notice inputs changed; new source review required")


def node_environment(checkout):
    path = Path(checkout) / "packaging/node-runtime.env"
    values = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or key in values:
            raise ValueError("invalid pinned Node runtime environment")
        values[key] = value
    expected = {
        "NODE_VERSION": NODE_REVIEW["version"],
        "NODE_X64_SHA256": "6c172a9ab6718f7b801fb5a7bd486af2ee5e32758086a04b84fcce2aa77095e6",
        "NODE_ARM64_SHA256": "dbabf7a1cbf947e6059ffad2c27b80041db97dea1e553b62afdee7f13fca336e",
        "NODE_DARWIN_X64_SHA256": NODE_REVIEW["darwin_x64_sha256"],
        "NODE_DARWIN_ARM64_SHA256": NODE_REVIEW["darwin_arm64_sha256"],
        "NODE_SOURCE_SHA256": NODE_REVIEW["source_sha256"],
        "NODE_BASE_URL": NODE_REVIEW["base_url"],
    }
    if values != expected:
        raise ValueError("pinned Node runtime or source inputs changed; new source review required")
    return values


def verify_node_runtimes(checkout, runtimes):
    node_environment(checkout)
    for arch, runtime in runtimes.items():
        node_path = runtime / "bin/node"
        manifest = read_json(runtime / "standalone.json")
        dependencies = manifest.get("dependencies")
        node = dependencies[0] if isinstance(dependencies, list) and dependencies else None
        if (node_path.is_symlink() or not node_path.is_file() or
                sha256(node_path) != NODE_REVIEW["staged_binary_sha256"][arch] or
                manifest.get("node_version") != NODE_REVIEW["version"] or
                node is None or set(node) != {"file", "sha256"} or
                node.get("file") != "node" or
                node.get("sha256") != NODE_REVIEW["binary_sha256"][arch]):
            raise ValueError(f"{arch} runtime Node provenance differs from reviewed inputs")


def fetch_node_source(output):
    archive = output / f"node-v{NODE_REVIEW['version']}.tar.gz"
    url = f"{NODE_REVIEW['base_url']}/{archive.name}"
    with urllib.request.urlopen(url, timeout=60) as response, archive.open("xb") as stream:
        shutil.copyfileobj(response, stream)
    if sha256(archive) != NODE_REVIEW["source_sha256"]:
        raise ValueError("Node source archive hash differs from reviewed input")
    member_name = f"node-v{NODE_REVIEW['version']}/LICENSE"
    with tarfile.open(archive, "r:gz") as source:
        member = source.extractfile(member_name)
        if member is None:
            raise ValueError("Node source archive has no LICENSE")
        license_bytes = member.read()
    if hashlib.sha256(license_bytes).hexdigest() != NODE_REVIEW["license_sha256"]:
        raise ValueError("Node source license differs from reviewed notice")
    license_path = output / "node-LICENSE"
    license_path.write_bytes(license_bytes)
    return archive, license_path


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


def fetch_vectorwarp_uhd_sources(output):
    sources = {}
    for name, url, expected in VECTORWARP_UHD_REVIEW["sources"]:
        target = output / name
        with urllib.request.urlopen(url, timeout=120) as response, target.open("xb") as stream:
            shutil.copyfileobj(response, stream)
        if sha256(target) != expected:
            raise ValueError(f"reviewed VectorWarp UHD input differs: {name}")
        sources[name] = target
    notices = {}
    with tarfile.open(sources["uhd-4.11.0.0.tar.gz"], "r:gz") as archive:
        for relative, expected in VECTORWARP_UHD_REVIEW["notices"]:
            member = archive.extractfile("uhd-4.11.0.0/" + relative)
            if member is None:
                raise ValueError(f"reviewed VectorWarp UHD source has no {relative}")
            content = member.read()
            if hashlib.sha256(content).hexdigest() != expected:
                raise ValueError(f"reviewed VectorWarp UHD notice differs: {relative}")
            target = output / ("vectorwarp-uhd-" + relative.replace("/", "-"))
            target.write_bytes(content)
            notices[relative] = target
    return sources, notices


def reviewed_npm_lock_delta(baseline_raw, candidate_raw):
    """Accept only the reviewed proxy-addr 2.0.7 -> 2.0.8 lock transition."""
    if hashlib.sha256(baseline_raw).hexdigest() != BASELINE_NPM_LOCK_SHA256:
        raise ValueError("baseline npm dependency lock changed")
    if hashlib.sha256(candidate_raw).hexdigest() != REVIEWED_NPM_LOCK_SHA256:
        raise ValueError("npm dependency lock changed; new source/notices review required")
    baseline = json.loads(baseline_raw)
    candidate = json.loads(candidate_raw)
    package_path = "node_modules/proxy-addr"
    old = ((baseline.get("packages") or {}).get(package_path) or {})
    new = ((candidate.get("packages") or {}).get(package_path) or {})
    if (old.get("version") != PROXY_ADDR_REVIEW["old_version"] or
            old.get("license") != "MIT" or
            new != {
                "version": PROXY_ADDR_REVIEW["version"],
                "resolved": PROXY_ADDR_REVIEW["resolved"],
                "integrity": PROXY_ADDR_REVIEW["integrity"],
                "license": "MIT",
                "dependencies": {"forwarded": "0.2.0", "ipaddr.js": "1.9.1"},
                "engines": {"node": ">= 0.10"},
                "funding": {"type": "opencollective",
                            "url": "https://opencollective.com/express"},
            }):
        raise ValueError("reviewed proxy-addr lock entry changed")
    old_copy = json.loads(json.dumps(baseline))
    old_copy["packages"][package_path] = new
    if old_copy != candidate:
        raise ValueError("npm dependency lock contains an unreviewed change")


def reconstructed_baseline_npm_lock(candidate_raw):
    """Reconstruct the reviewed baseline without depending on Git history.

    Release CI intentionally uses shallow checkouts. The complete candidate and
    reconstructed baseline remain independently pinned by their raw SHA-256
    digests, while reviewed_npm_lock_delta proves the sole structural change.
    """
    if hashlib.sha256(candidate_raw).hexdigest() != REVIEWED_NPM_LOCK_SHA256:
        raise ValueError("npm dependency lock changed; new source/notices review required")
    candidate = json.loads(candidate_raw)
    package_path = "node_modules/proxy-addr"
    packages = candidate.get("packages")
    if not isinstance(packages, dict) or package_path not in packages:
        raise ValueError("reviewed proxy-addr lock entry is missing")
    baseline = json.loads(json.dumps(candidate))
    baseline["packages"][package_path] = {
        "version": PROXY_ADDR_REVIEW["old_version"],
        "resolved": PROXY_ADDR_REVIEW["old_resolved"],
        "integrity": PROXY_ADDR_REVIEW["old_integrity"],
        "license": "MIT",
        "dependencies": {"forwarded": "0.2.0", "ipaddr.js": "1.9.1"},
        "engines": {"node": ">= 0.10"},
    }
    raw = (json.dumps(baseline, indent=2) + "\n").encode()
    if hashlib.sha256(raw).hexdigest() != BASELINE_NPM_LOCK_SHA256:
        raise ValueError("reconstructed baseline npm dependency lock changed")
    return raw


def fetch_proxy_addr_source(output):
    archive = output / "proxy-addr-2.0.8.tgz"
    with urllib.request.urlopen(PROXY_ADDR_REVIEW["resolved"], timeout=60) as response, \
            archive.open("xb") as stream:
        shutil.copyfileobj(response, stream)
    if sha256(archive) != PROXY_ADDR_REVIEW["archive_sha256"]:
        raise ValueError("proxy-addr source archive hash differs from reviewed input")
    with tarfile.open(archive, "r:gz") as source:
        package_stream = source.extractfile("package/package.json")
        license_stream = source.extractfile("package/LICENSE")
        if package_stream is None or license_stream is None:
            raise ValueError("proxy-addr source archive is incomplete")
        metadata = json.load(package_stream)
        license_sha = hashlib.sha256(license_stream.read()).hexdigest()
    if (metadata.get("name") != "proxy-addr" or
            metadata.get("version") != PROXY_ADDR_REVIEW["version"] or
            metadata.get("license") != "MIT" or
            license_sha != PROXY_ADDR_REVIEW["license_sha256"]):
        raise ValueError("proxy-addr source identity or license differs from review")
    return archive


def verify_inventory(path, baseline, runtime, source_id, arch, allow_node_gap=False):
    candidate = read_json(path)
    prior = read_json(baseline)
    if (candidate.get("source_id") != source_id or candidate.get("arch") != arch or
            candidate.get("runtime_manifest_sha256") != sha256(runtime / "standalone.json")):
        raise ValueError(f"{arch} CI inventory does not bind the candidate runtime")
    if prior.get("source_id") != BASELINE_SOURCE_ID or prior.get("arch") != arch:
        raise ValueError(f"{arch} baseline inventory identity changed")
    verify_source_claim_transition(candidate, prior, arch)
    expected_gaps = list(prior.get("gaps") or [])
    if allow_node_gap:
        expected_gaps.insert(0, NODE_GAP)
    if candidate.get("gaps") != expected_gaps or candidate.get("status") != prior.get("status"):
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
    """Validate reviewed native-build post-images and return immutable bytes."""
    if not isinstance(source_id, str) or not re.fullmatch(r"[0-9a-f]{40}", source_id):
        raise ValueError("review receipt source commit is invalid")
    guarded = sorted(guarded)
    if not guarded or len(set(guarded)) != len(guarded) or any(not REVIEW_PATH.fullmatch(path) for path in guarded):
        raise ValueError("review receipt contains an unsafe guarded build path")
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
    try:
        git("cat-file", "-e", BASELINE_SOURCE_ID + "^{commit}")
    except subprocess.CalledProcessError:
        git("fetch", "--no-tags", "origin", BASELINE_SOURCE_ID)
    candidate_lock = (checkout / "api/package-lock.json").read_bytes()
    baseline_lock = reconstructed_baseline_npm_lock(candidate_lock)
    reviewed_npm_lock_delta(baseline_lock, candidate_lock)
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


def rebind_notices(baseline, output, source_id, runtimes, node_license, uhd_notices):
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to replace notices output")
    validated_baseline_notices(baseline)
    shutil.copytree(baseline, output, symlinks=False)
    index = read_json(output / "INDEX.json")
    proxy_records = [item for item in index.get("api_packages", [])
                     if item.get("name") == "proxy-addr"]
    if (len(proxy_records) != 1 or
            proxy_records[0].get("version") != PROXY_ADDR_REVIEW["old_version"] or
            proxy_records[0].get("license") != "MIT" or
            proxy_records[0].get("notice_files_in_each_runtime") !=
            ["api/node_modules/proxy-addr/LICENSE"]):
        raise ValueError("baseline proxy-addr notice identity changed")
    proxy_records[0]["version"] = PROXY_ADDR_REVIEW["version"]
    for arch, runtime in runtimes.items():
        package_json = runtime / "api/node_modules/proxy-addr/package.json"
        license_path = runtime / "api/node_modules/proxy-addr/LICENSE"
        package_value = read_json(package_json)
        if (package_value.get("name") != "proxy-addr" or
                package_value.get("version") != PROXY_ADDR_REVIEW["version"] or
                package_value.get("license") != "MIT" or
                sha256(license_path) != PROXY_ADDR_REVIEW["license_sha256"]):
            raise ValueError(f"{arch} proxy-addr runtime identity or notice differs from review")
    matches = [item for item in index.get("input_notices", [])
               if item.get("file") == "embedded/cpp-httplib/LICENSE"]
    matches[0]["origin"] += "; cpp-httplib-0.53.1.tar.gz::cpp-httplib-0.53.1/LICENSE (Intel)"
    node_target = output / f"embedded/node-{NODE_REVIEW['version']}/LICENSE"
    node_target.parent.mkdir(parents=True)
    shutil.copyfile(node_license, node_target)
    index.setdefault("input_notices", []).append({
        "file": node_target.relative_to(output).as_posix(),
        "origin": f"node-v{NODE_REVIEW['version']}.tar.gz::node-v{NODE_REVIEW['version']}/LICENSE",
        "sha256": NODE_REVIEW["license_sha256"],
    })
    for relative, expected in VECTORWARP_UHD_REVIEW["notices"]:
        source = uhd_notices[relative]
        target = output / "embedded" / "vectorwarp-uhd-4.11.0.0" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        index.setdefault("input_notices", []).append({
            "file": target.relative_to(output).as_posix(),
            "origin": f"uhd-4.11.0.0.tar.gz::uhd-4.11.0.0/{relative}",
            "sha256": expected,
        })
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


def source_archive(args, source_id, runtimes, output, intel_cpp, proxy_addr, node_source,
                   uhd_sources, review=None):
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
             "proxy-addr-2.0.8.tgz": proxy_addr,
             f"node-v{NODE_REVIEW['version']}.tar.gz": node_source,
             "runtime-arm64-standalone.json": runtimes["arm64"] / "standalone.json",
             "runtime-x86_64-standalone.json": runtimes["x86_64"] / "standalone.json",
             "inventory-arm64.json": Path(args.arm64_inventory),
             "inventory-x86_64.json": Path(args.x86_64_inventory),
             "header-input-versions-arm64.txt": Path(args.arm64_header_versions),
             "header-input-versions-x86_64.txt": Path(args.x86_64_header_versions),
             "cpp-httplib-formula-arm64.rb": Path(args.arm64_cpp_formula),
             "cpp-httplib-formula-x86_64.rb": Path(args.x86_64_cpp_formula),
             "vectorwarp-uhd.rb": Path(args.checkout) / "packaging/homebrew/vectorwarp-uhd.rb",
             **uhd_sources}
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
              "with the same MIT license text as the 0.54.1 arm64 notice. The reviewed npm lock "
              "transition changes only proxy-addr 2.0.7 to 2.0.8; its exact registry source archive "
              "and unchanged MIT license are included. The official Node runtime is pinned to "
              f"{NODE_REVIEW['version']}; its exact upstream source archive and license are included. "
              "Both architectures use the reviewed VectorWarp UHD 4.11.0.0 formula with MPMD disabled "
              "and B200/B210 enabled. Its upstream archive, build-only Python resources, macOS patch, "
              "formula and notices are included; gRPC and protobuf are not part of the runtime. "
              "Extract the baseline kit, "
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
    proxy_addr.unlink()
    node_source.unlink()
    for path in uhd_sources.values():
        path.unlink()
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
    verify_node_runtimes(Path(args.checkout), runtimes)
    review_receipt = getattr(args, "review_receipt", None)
    changed = verify_checkout(Path(args.checkout), source_id, review_receipt)
    guarded = [name for name in changed if any(name == prefix or name.startswith(prefix)
                                               for prefix in SENSITIVE_PATHS)]
    review = (reviewed_build_changes(Path(args.checkout), source_id, guarded, review_receipt)
              if review_receipt is not None else None)
    for arch in package.ARCHES:
        verify_inventory(getattr(args, f"{arch}_inventory"),
                         getattr(args, f"baseline_{arch}_inventory"), runtimes[arch], source_id,
                         arch, allow_node_gap=True)
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
        proxy_addr = fetch_proxy_addr_source(output)
        node_source, node_license = fetch_node_source(output)
        uhd_sources, uhd_notices = fetch_vectorwarp_uhd_sources(output)
        rebind_notices(Path(args.baseline_notices), output / "notices", source_id, runtimes,
                       node_license, uhd_notices)
        node_license.unlink()
        for path in uhd_notices.values():
            path.unlink()
        if review is not None:
            persist_review(output, review)
        archive = source_archive(args, source_id, runtimes, output, intel_cpp, proxy_addr,
                                 node_source, uhd_sources, review)
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
