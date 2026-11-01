#!/usr/bin/env python3
"""Export and re-verify the exact source inputs fetched by a macOS CI build."""
import argparse, gzip, hashlib, io, json, re, stat, tarfile, zipfile
from pathlib import Path

MAX_FILE = 1024 * 1024 * 1024
SQLITE_NOTICE_SHA256 = "44ca9f793055c8e32fc65f65a4f5bcf813a33f5bdaaa084067dd617a4ed3cc70"

def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()

def regular_files(root):
    root = root.resolve(strict=True)
    result = {}
    for path in sorted(root.rglob("*")):
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        try:
            source = path.resolve(strict=True) if stat.S_ISLNK(mode) else path
        except OSError as error:
            raise ValueError(f"source closure contains a broken link: {path}") from error
        if not source.is_file() or source.stat().st_size > MAX_FILE:
            raise ValueError(f"source closure contains a special or oversized file: {path}")
        relative = path.relative_to(root).as_posix()
        if not relative or relative.startswith("/") or ".." in Path(relative).parts or relative in result:
            raise ValueError("unsafe or duplicate source-closure path")
        result[relative] = {"sha256": digest(source), "size": source.stat().st_size}
    return result

def add_file(archive, name, path):
    info = tarfile.TarInfo(name); info.size = path.stat().st_size; info.mode = 0o644; info.mtime = 0
    with path.open("rb") as stream:
        archive.addfile(info, stream)

def add_bytes(archive, name, value):
    info = tarfile.TarInfo(name); info.size = len(value); info.mode = 0o644; info.mtime = 0
    archive.addfile(info, io.BytesIO(value))

def contains_boost_license(root):
    marker = b"Boost Software License - Version 1.0"
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        try:
            with tarfile.open(path, "r:*") as source:
                for member in source:
                    if member.isfile() and member.size <= 1024 * 1024 and "license" in member.name.lower():
                        stream = source.extractfile(member)
                        if stream is not None and marker in stream.read():
                            return True
            continue
        except tarfile.ReadError:
            pass
        try:
            with zipfile.ZipFile(path) as source:
                for member in source.infolist():
                    if member.file_size <= 1024 * 1024 and "license" in member.filename.lower() and marker in source.read(member):
                        return True
        except zipfile.BadZipFile:
            pass
    return False

def main():
    parser = argparse.ArgumentParser()
    for name in ("inventory", "runtime-manifest", "formula-inputs", "source-cache", "recipes", "notices",
                 "header-versions", "node-source", "vkfft-source", "sqlite-notice", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--source-id", required=True)
    parser.add_argument("--run-id", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_id) or not args.run_id.isdigit():
        raise ValueError("invalid source or workflow identity")
    inventory = json.loads(args.inventory.read_text())
    runtime = json.loads(args.runtime_manifest.read_text())
    if (inventory.get("schema") != 1 or inventory.get("arch") != args.arch or
            inventory.get("source_id") != args.source_id or runtime.get("arch") != args.arch or
            runtime.get("source_id") != args.source_id):
        raise ValueError("closure inputs do not bind the candidate")
    runtime_formulas = {}
    for component in inventory.get("components", []):
        if component.get("formula") in {"grpc", "protobuf"}:
            raise ValueError("forbidden MPMD dependency")
        recipe = component.get("formula_recipe") or {}
        key = "|".join((component.get("formula", ""), component.get("version", ""), component.get("input", "")))
        if not all(key.split("|")) or key in runtime_formulas:
            raise ValueError("invalid or duplicate formula identity")
        runtime_formulas[key] = {"recipe_sha256": recipe.get("sha256"),
            "notices": sorted((item["path"], item["sha256"]) for item in component.get("notice_files", [])),
            "input_sha256": component.get("input_sha256")}
    trees = {name: regular_files(path) for name, path in
             (("source-cache", args.source_cache), ("recipes", args.recipes), ("notices", args.notices))}
    formula_inputs = json.loads(args.formula_inputs.read_text())
    if (formula_inputs.get("schema") != 2 or formula_inputs.get("arch") != args.arch or
            formula_inputs.get("source_id") != args.source_id or str(formula_inputs.get("run_id")) != args.run_id):
        raise ValueError("formula inputs do not bind the candidate")
    owned = {"recipes": set(), "source-cache": set()}; identities = set(); structured = {}
    for formula in formula_inputs.get("formulas", []):
        identity = formula.get("id")
        if identity != "|".join((formula.get("formula", ""), formula.get("version", ""))) or identity in identities:
            raise ValueError("invalid or duplicate structured formula identity")
        identities.add(identity); input_ids = set()
        recipe = formula.get("recipe") or {}; path = recipe.get("path", "")
        expected_path = f"recipes/{formula['formula']}/{formula['version']}/formula.rb"
        if path != expected_path:
            raise ValueError("formula recipe path is not canonical")
        relative = path.removeprefix("recipes/")
        if trees["recipes"].get(relative) != {"sha256": recipe.get("sha256"), "size": recipe.get("size")}:
            raise ValueError("formula recipe ownership differs")
        owned["recipes"].add(relative)
        for item in formula.get("inputs", []):
            input_id = item.get("id")
            if not input_id or input_id in input_ids:
                raise ValueError("invalid or duplicate formula input identity")
            input_ids.add(input_id)
            if item.get("kind") == "embedded-patch":
                if not re.fullmatch(r"[0-9a-f]{64}", item.get("embedded_sha256", "")) or item.get("payload"):
                    raise ValueError("invalid embedded patch claim")
                continue
            payload = item.get("payload") or {}; payload_path = payload.get("path", "")
            prefix = f"source-cache/{formula['formula']}/{formula['version']}/"
            if not payload_path.startswith(prefix) or ".." in Path(payload_path).parts:
                raise ValueError("formula payload path is not canonical")
            relative = payload_path.removeprefix("source-cache/")
            if trees["source-cache"].get(relative) != {"sha256": payload.get("sha256"), "size": payload.get("size")}:
                raise ValueError("formula payload ownership differs")
            owned["source-cache"].add(relative)
        structured[identity] = formula
    if set(trees["recipes"]) != owned["recipes"] or set(trees["source-cache"]) != owned["source-cache"]:
        raise ValueError("formula input tree contains missing or unowned members")
    by_identity = {(value["formula"], value["version"]): value for value in structured.values()}
    for key, claim in runtime_formulas.items():
        formula, version, unused = key.split("|", 2)
        value = by_identity.get((formula, version))
        if not value or "runtime" not in value.get("usage", []) or value["recipe"]["sha256"] != claim["recipe_sha256"]:
            raise ValueError("runtime formula is not owned by structured inputs")
    expected_headers = {"asio", "cpp-httplib", "eigen", "rapidjson", "vulkan-headers"}
    actual_headers = {value["formula"] for value in structured.values() if "compile-header" in value.get("usage", [])}
    if actual_headers != expected_headers:
        raise ValueError("compile-header formula closure differs")
    allowed = {("boost", "missing license/notice file"), ("sqlite", "missing license/notice file")}
    unresolved = [item for item in inventory.get("gaps", []) if "index" not in item and
                  (item.get("formula"), item.get("reason")) not in allowed]
    if unresolved:
        raise ValueError("source closure has unresolved provenance gaps")
    if digest(args.sqlite_notice) != SQLITE_NOTICE_SHA256:
        raise ValueError("SQLite public-domain notice changed")
    if not contains_boost_license(args.source_cache):
        raise ValueError("Boost source license was not exported")
    fixed = {"inventory.json": args.inventory, "runtime-manifest.json": args.runtime_manifest,
        "formula-inputs.json": args.formula_inputs,
        "header-input-versions.txt": args.header_versions, "node-source.tar.gz": args.node_source,
        "vkfft-source.tar.gz": args.vkfft_source, "sqlite-public-domain.html": args.sqlite_notice}
    manifest = {"schema": 1, "source_id": args.source_id, "run_id": args.run_id, "arch": args.arch,
        "runtime_manifest_sha256": digest(args.runtime_manifest), "runtime_formulas": runtime_formulas,
        "formula_inputs": formula_inputs,
        "gaps_resolved": ["node-pseudo-input", "boost-source-license", "sqlite-public-domain"],
        "trees": trees, "files": {name: {"sha256": digest(path), "size": path.stat().st_size}
                                    for name, path in fixed.items()}}
    manifest_bytes = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
    with args.output.open("xb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="", compresslevel=1, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|") as archive:
            add_bytes(archive, "VectorWarp-source-closure/MANIFEST.json", manifest_bytes)
            for name, path in sorted(fixed.items()): add_file(archive, "VectorWarp-source-closure/" + name, path)
            for tree_name, root in (("source-cache", args.source_cache), ("recipes", args.recipes), ("notices", args.notices)):
                for relative in sorted(trees[tree_name]):
                    add_file(archive, f"VectorWarp-source-closure/{tree_name}/{relative}", root / relative)
    print(json.dumps({"archive": str(args.output), "sha256": digest(args.output),
        "formula_count": len(structured), "source_file_count": len(trees["source-cache"])}, sort_keys=True))

if __name__ == "__main__": main()
