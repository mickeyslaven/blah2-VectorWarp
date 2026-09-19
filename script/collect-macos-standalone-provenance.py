#!/usr/bin/env python3
"""Bind a staged standalone runtime to sanitized Homebrew provenance records.

This is collection evidence for local development review, never publication
approval. Missing records and mismatched inputs are written as explicit gaps so
the development runtime path stays usable while release review remains blocked.
"""
import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def fail(message):
    raise ValueError(message)


def load_inventory():
    spec = importlib.util.spec_from_file_location(
        "macos_license_inventory", ROOT / "script" / "macos-license-inventory.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def relative_file(value):
    path = Path(value)
    if not value or path.is_absolute() or ".." in path.parts:
        fail("dependency file must be a non-empty, relative path without parent traversal")
    return path


def simple_component(value):
    if not isinstance(value, str) or not value or value in (".", "..") or "/" in value or "\\" in value:
        fail("formula and version must be simple path components")
    return value


def valid_hex(value, length):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{%d}" % length, value) is not None


def valid_minimum_os(value):
    return isinstance(value, str) and re.fullmatch(r"\d+\.\d+(?:\.\d+)?", value) is not None


def sanitized_component(component):
    # The underlying inventory intentionally records receipt/SBOM top-level
    # keys for local inspection. A CI diagnostic needs only their presence,
    # hash, size, and JSON validity; key names can themselves be sensitive.
    result = {key: component[key] for key in ("input", "input_sha256", "formula", "version",
              "formula_recipe", "notice_files", "copied_files", "missing_provenance")}
    for name in ("install_receipt", "sbom"):
        record = component[name]
        result[name] = {key: record[key] for key in ("path", "present", "sha256", "bytes")}
        result[name]["json_valid"] = record["json"]["valid"]
    return result


def collect(runtime, cellar, notices_dir):
    runtime = Path(runtime).resolve(strict=True)
    cellar = Path(cellar).resolve(strict=True)
    metadata_path = runtime / "standalone.json"
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"invalid runtime standalone.json: {error}")
    if metadata.get("schema") != 1 or not isinstance(metadata.get("dependencies"), list):
        fail("runtime standalone.json lacks a schema-1 dependency list")
    if not valid_hex(metadata.get("source_id"), 40) and not valid_hex(metadata.get("source_id"), 64):
        fail("runtime standalone.json has invalid source_id")
    if not valid_hex(metadata.get("source_revision"), 40):
        fail("runtime standalone.json has invalid source_revision")
    if metadata.get("arch") not in ("arm64", "x86_64"):
        fail("runtime standalone.json has invalid arch")
    if not valid_minimum_os(metadata.get("minimum_os")):
        fail("runtime standalone.json has invalid minimum_os")

    bindings, gaps, representatives = [], [], {}
    for index, dependency in enumerate(metadata["dependencies"]):
        record = {"index": index}
        if not isinstance(dependency, dict):
            gaps.append({**record, "reason": "dependency is not an object"})
            continue
        formula, version = dependency.get("formula"), dependency.get("version")
        expected, file_name = dependency.get("sha256"), dependency.get("file")
        if not all(isinstance(item, str) and item for item in (formula, version, expected, file_name)):
            gaps.append({**record, "reason": "dependency lacks formula, version, file, or sha256"})
            continue
        try:
            simple_component(formula)
            simple_component(version)
            relative = relative_file(file_name)
            if not valid_hex(expected, 64):
                raise ValueError("dependency sha256 is invalid")
        except ValueError:
            # Do not reflect malformed fields into a public diagnostic.
            gaps.append({**record, "reason": "dependency metadata is invalid"})
            continue
        try:
            root = (cellar / formula / version).resolve(strict=True)
            root.relative_to(cellar)
        except (OSError, ValueError):
            gaps.append({**record, "formula": formula, "version": version, "file": file_name,
                         "expected_sha256": expected, "reason": "declared keg is unavailable"})
            continue
        try:
            source = (root / relative).resolve(strict=True)
            source.relative_to(root)
            if not source.is_file():
                raise ValueError("source is not a regular file")
            actual = sha256(source)
        except OSError:
            gaps.append({**record, "formula": formula, "version": version, "file": file_name,
                         "expected_sha256": expected, "reason": "declared keg file is unavailable"})
            continue
        except ValueError:
            gaps.append({**record, "formula": formula, "version": version, "file": file_name,
                         "expected_sha256": expected, "reason": "declared keg file resolves outside its formula version"})
            continue
        binding = {**record, "formula": formula, "version": version, "file": file_name,
                   "expected_sha256": expected, "actual_sha256": actual}
        if actual != expected:
            binding["status"] = "hash-mismatch"
            gaps.append({**binding, "reason": "staged dependency hash does not match the keg input"})
            bindings.append(binding)
            continue
        binding["status"] = "matched"
        bindings.append(binding)
        representatives.setdefault((formula, version), source)

    inventory = load_inventory()
    components = []
    try:
        result = inventory.inventory([str(path) for _, path in sorted(representatives.items())], notices_dir)
        components = [sanitized_component(component) for component in result["components"]]
        for component in components:
            for item in component.get("missing_provenance", []):
                gaps.append({"formula": component["formula"], "version": component["version"],
                             "reason": f"missing {item}"})
    except (OSError, ValueError):
        gaps.append({"reason": "license inventory failed"})

    return {
        "schema": 1,
        "purpose": "sanitized standalone runtime input provenance inventory",
        "redistribution_status": "not-reviewed-not-approved",
        "source_id": metadata["source_id"],
        "source_revision": metadata["source_revision"],
        "arch": metadata["arch"],
        "minimum_os": metadata["minimum_os"],
        "runtime_manifest_sha256": sha256(metadata_path),
        "dependency_count": len(metadata["dependencies"]),
        "bindings": bindings,
        "components": components,
        "gaps": gaps,
        "status": "inventory-complete" if not gaps else "inventory-complete-with-gaps",
        "boundaries": [
            "Raw Homebrew INSTALL_RECEIPT.json and sbom.spdx.json are not copied.",
            "This inventory does not establish redistribution rights, source closure, or release approval.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--cellar", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--notices-dir", required=True, type=Path)
    args = parser.parse_args()
    if not args.output.is_absolute() or not args.notices_dir.is_absolute():
        fail("--output and --notices-dir must be absolute paths")
    result = collect(args.runtime, args.cellar, args.notices_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    try:
        main()
    except ValueError as error:
        raise SystemExit(f"collect-macos-standalone-provenance: {error}")
