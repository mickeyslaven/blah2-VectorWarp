#!/usr/bin/env python3
"""Assemble macOS source/notices from one owner-reviewed current input manifest.

No historical source kit, dependency transition, network fetch, signing key or
implicit approval participates. Inputs are supplied offline and independently
bound to both architectures. Historical receipts remain external evidence.
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

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("macos_packager", ROOT / "script/package-macos-standalone.py")
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)
HEADER_INPUT_NAMES = {"asio", "cpp-httplib", "eigen", "rapidjson", "vulkan-headers"}
REVIEW_MAX_BYTES = 64 * 1024
SOURCE_CLOSURE_MAX_MEMBER = 1024 * 1024 * 1024
GITHUB_RELEASE_ASSET_LIMIT = 2 * 1024 * 1024 * 1024
REVIEW_PURPOSE = "current-macos-release-inputs"
REVIEW_ARCHIVE_MEMBER = "VectorWarp-corresponding-source/input-review.json"

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


def validate_formula_input_ownership(value, trees, source_id, arch, run_id):
    if (value.get("schema") != 2 or value.get("source_id") != source_id or value.get("arch") != arch or
            str(value.get("run_id")) != str(run_id)):
        raise ValueError("formula inputs do not bind the candidate")
    owned = {"recipes": set(), "source-cache": set()}; identities = set()
    for formula in value.get("formulas", []):
        identity = formula.get("id")
        if identity != "|".join((formula.get("formula", ""), formula.get("version", ""))) or identity in identities:
            raise ValueError("invalid structured formula identity")
        identities.add(identity); input_ids = set()
        recipe = formula.get("recipe") or {}; path = recipe.get("path", "")
        if path != f"recipes/{formula['formula']}/{formula['version']}/formula.rb":
            raise ValueError("noncanonical structured recipe path")
        relative = path.removeprefix("recipes/")
        if trees.get("recipes", {}).get(relative) != {"sha256": recipe.get("sha256"), "size": recipe.get("size")}:
            raise ValueError("structured recipe member differs")
        owned["recipes"].add(relative)
        for item in formula.get("inputs", []):
            input_id = item.get("id")
            if not input_id or input_id in input_ids: raise ValueError("duplicate structured input")
            input_ids.add(input_id)
            if item.get("kind") == "embedded-patch":
                if not re.fullmatch(r"[0-9a-f]{64}", item.get("embedded_sha256", "")) or item.get("payload"):
                    raise ValueError("invalid embedded patch")
                continue
            payload = item.get("payload") or {}; payload_path = payload.get("path", "")
            prefix = f"source-cache/{formula['formula']}/{formula['version']}/"
            if not payload_path.startswith(prefix) or ".." in Path(payload_path).parts:
                raise ValueError("noncanonical structured payload path")
            relative = payload_path.removeprefix("source-cache/")
            if trees.get("source-cache", {}).get(relative) != {"sha256": payload.get("sha256"), "size": payload.get("size")}:
                raise ValueError("structured payload member differs")
            owned["source-cache"].add(relative)
    if set(trees.get("recipes", {})) != owned["recipes"] or set(trees.get("source-cache", {})) != owned["source-cache"]:
        raise ValueError("structured formula closure has missing or unowned members")
    return {item["id"]: item for item in value["formulas"]}


def verify_source_closure(path, inventory_path, runtime_path, source_id, arch, run_id):
    """Stream-verify a CI-exported exact source closure and its candidate bindings."""
    inventory = read_json(inventory_path)
    expected_formulas = {}
    for key, value in source_claims(inventory).items():
        expected_formulas["|".join(key)] = {
            "recipe_sha256": value[0],
            "notices": [list(item) for item in value[2]],
            "input_sha256": next(item["input_sha256"] for item in inventory["components"]
                                 if (item["formula"], item["version"], item["input"]) == key),
        }
    manifest = None; formula_inputs_file = None
    seen = set()
    with tarfile.open(path, "r|gz") as archive:
        for member in archive:
            prefix = "VectorWarp-source-closure/"
            if (not member.isfile() or not member.name.startswith(prefix) or
                    member.size > SOURCE_CLOSURE_MAX_MEMBER):
                raise ValueError("unsafe source-closure member")
            relative = member.name.removeprefix(prefix)
            if not relative or relative in seen or ".." in Path(relative).parts:
                raise ValueError("duplicate or unsafe source-closure path")
            seen.add(relative)
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError("unreadable source-closure member")
            if relative == "MANIFEST.json":
                manifest = json.load(stream)
                continue
            if manifest is None:
                raise ValueError("source-closure manifest must be first")
            if "/" in relative:
                group, name = relative.split("/", 1)
                expected = (manifest.get("trees", {}).get(group, {}).get(name))
            else:
                expected = manifest.get("files", {}).get(relative)
            if not isinstance(expected, dict) or expected.get("size") != member.size:
                raise ValueError("unexpected source-closure member")
            digest_value = hashlib.sha256()
            if relative == "formula-inputs.json":
                content = stream.read()
                if len(content) != member.size: raise ValueError("truncated formula-input manifest")
                digest_value.update(content); formula_inputs_file = json.loads(content)
            else:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest_value.update(block)
            if digest_value.hexdigest() != expected.get("sha256"):
                raise ValueError("source-closure member hash differs")
    if (manifest is None or manifest.get("schema") != 1 or manifest.get("arch") != arch or
            manifest.get("source_id") != source_id or str(manifest.get("run_id")) != str(run_id) or
            manifest.get("runtime_manifest_sha256") != sha256(runtime_path) or
            manifest.get("runtime_formulas") != expected_formulas or
            manifest.get("formula_inputs") != formula_inputs_file or
            set(manifest.get("gaps_resolved", [])) !=
            {"node-pseudo-input", "boost-source-license", "sqlite-public-domain"}):
        raise ValueError("source closure does not bind the candidate")
    structured = validate_formula_input_ownership(formula_inputs_file, manifest.get("trees", {}), source_id, arch, run_id)
    for key, claim in expected_formulas.items():
        formula, version, unused = key.split("|", 2)
        value = structured.get(f"{formula}|{version}")
        if not value or "runtime" not in value.get("usage", []) or value["recipe"]["sha256"] != claim["recipe_sha256"]:
            raise ValueError("runtime formula differs from structured closure")
    expected_paths = {"MANIFEST.json", *manifest.get("files", {})}
    for group, records in manifest.get("trees", {}).items():
        expected_paths.update(f"{group}/{name}" for name in records)
    if seen != expected_paths:
        raise ValueError("source closure is incomplete")
    return manifest


def read_header_versions(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if len(parts) != 2 or parts[0] in result:
            raise ValueError("invalid or duplicate Homebrew header version")
        result[parts[0]] = parts[1]
    return result


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


def verify_reviewed_headers_in_closure(headers, cpp_formula, closure, arch):
    """Bind externally reported compile headers to the verified source closure."""
    formulas = {item.get("id"): item for item in closure.get("formula_inputs", {}).get("formulas", [])}
    if len(formulas) != len(closure.get("formula_inputs", {}).get("formulas", [])):
        raise ValueError(f"{arch} source closure has duplicate formula identities")
    compile_headers = [(item.get("formula"), item.get("version")) for item in formulas.values()
                       if "compile-header" in item.get("usage", [])]
    if len({name for name, unused in compile_headers}) != len(compile_headers):
        raise ValueError(f"{arch} source closure has duplicate compile-header formulas")
    closure_headers = dict(compile_headers)
    if set(headers) != HEADER_INPUT_NAMES or closure_headers != headers:
        raise ValueError(f"{arch} reviewed header inputs differ from source closure")
    cpp = formulas.get(f"cpp-httplib|{headers.get('cpp-httplib', '')}")
    if cpp is None or cpp.get("recipe", {}).get("sha256") != sha256(cpp_formula):
        raise ValueError(f"{arch} cpp-httplib formula differs from source closure")


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


def validate_release_asset_size(path):
    size = Path(path).stat().st_size
    if size >= GITHUB_RELEASE_ASSET_LIMIT:
        raise ValueError(
            f"corresponding-source archive is {size} bytes; GitHub requires release assets "
            f"smaller than {GITHUB_RELEASE_ASSET_LIMIT} bytes")


def file_record(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"release input must be a regular file: {path}")
    return {"sha256": sha256(path), "size": path.stat().st_size}


def input_files(args, runtimes):
    files = {"api-package-lock.json": Path(args.checkout) / "api/package-lock.json",
             "node-runtime.env": Path(args.checkout) / "packaging/node-runtime.env",
             "notices-manifest.json": Path(args.notices) / "notices.json",
             "notices-index.json": Path(args.notices) / "INDEX.json"}
    for arch, runtime in runtimes.items():
        files[f"runtime-{arch}-standalone.json"] = runtime / "standalone.json"
        for suffix, attribute in (("inventory.json", "inventory"), ("source-closure.tar.gz", "closure"),
                                  ("header-versions.txt", "header_versions"), ("cpp-formula.rb", "cpp_formula")):
            files[f"{arch}-{suffix}"] = Path(getattr(args, f"{arch}_{attribute}"))
    return files


def reviewed_inputs(path, source_id, run_id, version, files, sources):
    raw = owner_only_bytes(path)
    record = review_json(raw)
    if (set(record) != {"schema", "purpose", "source_id", "run_id", "version", "files", "sources"} or
            record.get("schema") != 1 or record.get("purpose") != REVIEW_PURPOSE or
            not re.fullmatch(r"[0-9a-f]{40}", source_id) or record.get("source_id") != source_id or
            not re.fullmatch(r"[1-9][0-9]*", str(run_id)) or record.get("run_id") != str(run_id) or
            record.get("version") != version or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version)):
        raise ValueError("current input review does not bind the exact candidate")
    if record["files"] != {name: file_record(path) for name, path in files.items()}:
        raise ValueError("reviewed current input bytes differ")
    declared = record["sources"]
    if not isinstance(declared, dict):
        raise ValueError("reviewed public source inventory must be an object")
    actual = {}
    if sources is not None:
        sources = Path(sources)
        if sources.is_symlink() or not sources.is_dir():
            raise ValueError("public sources must be a real directory")
        for path in sources.iterdir():
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]*\.(?:tar\.gz|tgz|zip|patch|rb)", path.name):
                raise ValueError("unexpected public source filename")
            actual[path.name] = file_record(path)
    if actual != declared:
        raise ValueError("reviewed public source inventory differs")
    return {"record": record, "raw": raw, "metadata": {
        "sha256": hashlib.sha256(raw).hexdigest(), "archive_member": REVIEW_ARCHIVE_MEMBER,
        "source_id": source_id, "run_id": str(run_id)}}


def verify_notice_coverage(notices, runtimes, inventories):
    """Require current API/native notice coverage rather than inheriting old claims."""
    index = read_json(Path(notices) / "INDEX.json")
    records = index.get("api_packages")
    if not isinstance(records, list):
        raise ValueError("current notices require an API package index")
    for item in index.get("input_notices", []):
        relative = item.get("file", "")
        if (not isinstance(relative, str) or not relative or Path(relative).is_absolute() or
                ".." in Path(relative).parts or
                file_record(Path(notices) / relative)["sha256"] != item.get("sha256")):
            raise ValueError("native notice index differs from reviewed file bytes")
    for arch, runtime in runtimes.items():
        if index.get("source_id") != read_json(runtime / "standalone.json")["source_id"]:
            raise ValueError("notice index source differs from the current runtime")
        expected = {name: item for name, item in
                    read_json(runtime / "api/package-lock.json").get("packages", {}).items()
                    if name and not item.get("dev")}
        found = set()
        for item in records:
            paths = item.get("notice_files_in_each_runtime", [])
            if not paths:
                raise ValueError("API package has no reviewed runtime notice")
            for relative in paths:
                if (not isinstance(relative, str) or not relative.startswith("api/node_modules/") or
                        ".." in Path(relative).parts):
                    raise ValueError("unsafe runtime API notice")
                path = runtime / relative
                if path.is_symlink() or not path.is_file():
                    raise ValueError("missing runtime API notice")
                directory = path.parent
                while directory != runtime / "api" and not (directory / "package.json").is_file():
                    directory = directory.parent
                metadata = read_json(directory / "package.json")
                key = directory.relative_to(runtime / "api").as_posix()
                if (key not in expected or metadata.get("name") != item.get("name") or
                        metadata.get("version") != item.get("version") or
                        metadata.get("license") != item.get("license") or
                        expected[key].get("version") != item.get("version")):
                    raise ValueError("API notice index differs from the current runtime")
                found.add(key)
        if found != set(expected):
            raise ValueError("API notice coverage is incomplete")
        hashes = {item.get("sha256") for item in index.get("input_notices", [])}
        for component in inventories[arch]["components"]:
            if any(item["sha256"] not in hashes for item in component.get("notice_files", [])):
                raise ValueError("native dependency notice coverage is incomplete")


def verify_node_source(pin_path, manifest, closure):
    pins = {}
    for line in Path(pin_path).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        name, separator, value = line.partition("=")
        if not separator or name in pins:
            raise ValueError("invalid or duplicate Node pin")
        pins[name] = value
    if (not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", pins.get("NODE_VERSION", "")) or
            not re.fullmatch(r"[0-9a-f]{64}", pins.get("NODE_SOURCE_SHA256", "")) or
            manifest.get("node_version") != pins["NODE_VERSION"] or
            closure.get("files", {}).get("node-source.tar.gz", {}).get("sha256") != pins["NODE_SOURCE_SHA256"]):
        raise ValueError("runtime Node version or corresponding source differs from reviewed pins")


def verify_inputs(args):
    runtimes = {arch: Path(getattr(args, arch)).resolve(strict=True) for arch in package.ARCHES}
    manifests = {arch: package.audit_runtime(path, arch) for arch, path in runtimes.items()}
    ids = {value["source_id"] for value in manifests.values()}
    if len(ids) != 1:
        raise ValueError("the two runtimes have different source commits")
    source_id = ids.pop()
    checkout = Path(args.checkout)
    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(checkout), *arguments], text=True).strip()
    if git("rev-parse", "HEAD") != source_id or git("status", "--porcelain", "--untracked-files=normal"):
        raise ValueError("source checkout is not clean at the exact candidate commit")
    files = input_files(args, runtimes)
    review = reviewed_inputs(args.input_manifest, source_id, args.run_id, args.version, files,
                             getattr(args, "sources", None))
    package.validated_notices(args.notices, manifests, runtimes)
    inventories = {}
    for arch, runtime in runtimes.items():
        inventory = inventories[arch] = read_json(getattr(args, f"{arch}_inventory"))
        if (inventory.get("source_id") != source_id or inventory.get("arch") != arch or
                inventory.get("runtime_manifest_sha256") != sha256(runtime / "standalone.json")):
            raise ValueError("CI inventory does not bind the candidate runtime")
        if any(item.get("formula") in {"grpc", "protobuf"} for item in inventory["components"]):
            raise ValueError("standalone runtime contains the unsupported UHD MPMD stack")
        closure = verify_source_closure(getattr(args, f"{arch}_closure"),
            getattr(args, f"{arch}_inventory"), runtime / "standalone.json", source_id, arch, args.run_id)
        verify_node_source(files["node-runtime.env"], manifests[arch], closure)
        allowed = {("boost", "missing license/notice file"), ("sqlite", "missing license/notice file")}
        for gap in inventory.get("gaps", []):
            if gap == {"index": 0, "reason": "dependency lacks formula, version, file, or sha256"}:
                continue
            if (gap.get("formula"), gap.get("reason")) not in allowed:
                raise ValueError("CI inventory has unresolved provenance gaps")
        verify_reviewed_headers_in_closure(read_header_versions(getattr(args, f"{arch}_header_versions")),
            Path(getattr(args, f"{arch}_cpp_formula")), closure, arch)
        if (runtime / "api/package-lock.json").read_bytes() != files["api-package-lock.json"].read_bytes():
            raise ValueError("runtime npm lock differs from reviewed source")
    verify_notice_coverage(args.notices, runtimes, inventories)
    return runtimes, source_id, review


def source_archive(args, source_id, runtimes, output, review):
    first_party = output / "VectorWarp-current-source.tar.gz"
    with first_party.open("xb") as stream:
        subprocess.run(["git", "-C", str(args.checkout), "archive", "--format=tar.gz",
                        "--prefix=VectorWarp-current-source/", source_id], check=True, stdout=stream)
    files = input_files(args, runtimes)
    # Recheck approved bytes immediately before archiving, then independently
    # stream-verify the completed archive. Approval is never inferred or regenerated.
    current = reviewed_inputs(args.input_manifest, source_id, args.run_id, args.version, files,
                              getattr(args, "sources", None))
    if current["raw"] != review["raw"]:
        raise ValueError("current input review changed before archival")
    items = {**files, "VectorWarp-current-source.tar.gz": first_party,
             "input-review.json": output / "input-review.json"}
    if getattr(args, "sources", None):
        items.update({f"public-sources/{name}": Path(args.sources) / name for name in review["record"]["sources"]})
    manifest = {"schema": 2, "source_id": source_id, "version": args.version,
                "input_review_sha256": review["metadata"]["sha256"],
                "files": {name: file_record(path) for name, path in items.items()}}
    expected_inputs = {**review["record"]["files"],
                       **{f"public-sources/{name}": record for name, record in review["record"]["sources"].items()}}
    if any(manifest["files"][name] != record for name, record in expected_inputs.items()):
        raise ValueError("source inputs changed before archival")
    if manifest["files"]["input-review.json"]["sha256"] != review["metadata"]["sha256"]:
        raise ValueError("persisted current input review changed before archival")
    readme = ("Current first-party source, both exact architecture source closures, runtime manifests, "
              "inventories, header inputs and reviewed public source inputs are bound by MANIFEST.json. "
              "input-review.json is the operator-reviewed current-input manifest; historical release "
              "receipts are retained separately and are not current source inputs. No signing credentials "
              "or proprietary receiver SDKs belong in these public inputs.\n").encode()
    metadata = {"README.txt": readme, "MANIFEST.json": (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()}
    archive_path = output / f"vectorwarp-{args.version}-macos-corresponding-source.tar.gz"
    with archive_path.open("xb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="",
                                                      compresslevel=1, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|") as archive:
            for name, content in metadata.items():
                add_bytes(archive, "VectorWarp-corresponding-source/" + name, content)
            for name, path in sorted(items.items()):
                add_file(archive, "VectorWarp-corresponding-source/" + name, path)
    validate_release_asset_size(archive_path)
    expected = dict(manifest["files"])
    expected.update({name: {"sha256": hashlib.sha256(content).hexdigest(), "size": len(content)}
                     for name, content in metadata.items()})
    seen = set()
    with tarfile.open(archive_path, "r|gz") as archive:
        for member in archive:
            prefix = "VectorWarp-corresponding-source/"
            name = member.name.removeprefix(prefix)
            if (not member.isfile() or not member.name.startswith(prefix) or name not in expected or
                    name in seen or member.size != expected[name]["size"]):
                raise ValueError("unexpected or duplicate corresponding-source member")
            stream = archive.extractfile(member)
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
            if digest.hexdigest() != expected[name]["sha256"]:
                raise ValueError("corresponding-source member digest differs")
            seen.add(name)
    if seen != set(expected):
        raise ValueError("corresponding-source archive is missing members")
    first_party.unlink()
    return archive_path


def prepare(args):
    output = Path(args.output)
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to replace existing release input directory")
    runtimes, source_id, review = verify_inputs(args)
    package.separate_output(output, [Path(args.checkout), Path(args.notices), Path(args.input_manifest),
                                   *([Path(args.sources)] if getattr(args, "sources", None) else []),
                                   *runtimes.values(), *input_files(args, runtimes).values()])
    output.mkdir(parents=True)
    try:
        descriptor = os.open(output / "input-review.json", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(review["raw"])
        shutil.copytree(args.notices, output / "notices", symlinks=False)
        package.validated_notices(output / "notices",
            {arch: read_json(path / "standalone.json") for arch, path in runtimes.items()}, runtimes)
        if sha256(output / "notices/notices.json") != review["record"]["files"]["notices-manifest.json"]["sha256"]:
            raise ValueError("notices changed during staging")
        archive = source_archive(args, source_id, runtimes, output, review)
        receipt = {"schema": 2, "source_id": source_id, "version": args.version,
                   "source_archive": archive.name, "source_archive_sha256": sha256(archive),
                   "source_archive_size": archive.stat().st_size,
                   "notices_sha256": sha256(output / "notices/notices.json"),
                   "input_review": review["metadata"]}
        (output / "receipt.json").write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
    except Exception:
        shutil.rmtree(output)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("arm64", "x86_64", "arm64-inventory", "x86_64-inventory",
                 "arm64-closure", "x86_64-closure", "run-id",
                 "arm64-header-versions", "x86_64-header-versions",
                 "arm64-cpp-formula", "x86_64-cpp-formula",
                 "input-manifest", "notices", "checkout", "version", "output"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--sources", type=Path, help="optional offline reviewed public source directory")
    args = parser.parse_args()
    try:
        prepare(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
