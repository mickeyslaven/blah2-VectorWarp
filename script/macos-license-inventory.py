#!/usr/bin/env python3
"""Create a read-only provenance and notice inventory for macOS bundle inputs."""
import argparse
import hashlib
import json
import os
import re
import shutil
from pathlib import Path

NOTICE_NAMES = {"LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "COPYING.RUNTIME",
                "COPYING.LIB", "NOTICE", "NOTICE.txt", "COPYRIGHT", "COPYRIGHT.txt"}
NOTICE_PREFIXES = ("LICENSE.", "LICENSE-", "COPYING.", "COPYING-", "COPYRIGHT.",
                   "COPYRIGHT-", "NOTICE.", "NOTICE-")
NOTICE_ROOTS = ("share", "Resources", "licenses", "license", "doc", "docs")
SKIP_NOTICE_DIRS = {".git", "bin", "build", "include", "lib", "node_modules", "src", "source", "test", "tests"}
MAX_NOTICE_DEPTH = 5
MAX_NOTICE_BYTES = 4 * 1024 * 1024


def fail(message):
    raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cellar_identity(file):
    path = Path(file)
    if not path.is_absolute(): fail("--file must be an absolute path")
    path = path.resolve(strict=True)
    parts = path.parts
    try:
        index = parts.index("Cellar")
        formula, version = parts[index + 1:index + 3]
    except (ValueError, IndexError):
        fail("--file must resolve to a Homebrew Cellar formula/version path")
    if not formula or not version: fail("invalid Cellar formula/version path")
    return Path(*parts[:index + 3]), formula, version


def read_json_summary(path):
    summary = {"present": path.is_file(), "sha256": None, "bytes": None,
               "json": {"valid": False, "top_level_keys": []}}
    if not summary["present"]:
        return summary
    summary["sha256"] = sha256(path)
    summary["bytes"] = path.stat().st_size
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return summary
    summary["json"] = {"valid": True,
                       "top_level_keys": sorted(value)[:128] if isinstance(value, dict) else []}
    return summary


def recipe_metadata(recipe):
    if not recipe.is_file(): return {"present": False, "sha256": None, "urls": [], "source_sha256": [], "dependencies": []}
    text = recipe.read_text(encoding="utf-8", errors="replace")
    return {"present": True, "sha256": sha256(recipe),
            "urls": re.findall(r'^\s*url\s+["\']([^"\']+)', text, re.M),
            "source_sha256": re.findall(r'^\s*sha256\s+["\']([0-9a-fA-F]{64})', text, re.M),
            "dependencies": re.findall(r'^\s*depends_on\s+["\']([^"\']+)', text, re.M)}


def is_notice_name(name):
    upper = name.upper()
    return upper in {item.upper() for item in NOTICE_NAMES} or upper.startswith(NOTICE_PREFIXES)


def notice_files(root):
    """Scan bounded notice locations, never the whole installed source/dependency tree."""
    candidates = [root]
    candidates.extend(root / item for item in NOTICE_ROOTS if (root / item).is_dir())
    found = set()
    for base in candidates:
        for directory, names, filenames in os.walk(base, followlinks=False):
            current = Path(directory)
            try:
                depth = len(current.relative_to(base).parts)
            except ValueError:
                continue
            names[:] = sorted(name for name in names if name not in SKIP_NOTICE_DIRS and depth < MAX_NOTICE_DEPTH)
            if depth > MAX_NOTICE_DEPTH:
                names[:] = []
                continue
            for name in sorted(filenames):
                candidate = current / name
                if not is_notice_name(name) or candidate.is_symlink():
                    continue
                try:
                    if candidate.is_file() and candidate.stat().st_size <= MAX_NOTICE_BYTES:
                        found.add(candidate)
                except OSError:
                    continue
    return sorted(found, key=lambda item: item.relative_to(root).as_posix())


def copy_record(source, root, destination, copied):
    relative = source.relative_to(root)
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    copied.append({"path": relative.as_posix(), "sha256": sha256(target)})


def inventory(files, notices_dir=None):
    records = []
    for raw in files:
        root, formula, version = cellar_identity(raw)
        raw_path = Path(raw).resolve(strict=True)
        if not raw_path.exists(): fail(f"input does not exist: {raw}")
        recipe = root / ".brew" / f"{formula}.rb"
        # Homebrew keeps these two records at the keg root, not .brew.
        receipt = root / "INSTALL_RECEIPT.json"
        sbom = root / "sbom.spdx.json"
        notices = notice_files(root)
        copied = []
        if notices_dir:
            destination = Path(notices_dir) / f"{formula}-{version}"
            destination.mkdir(parents=True, exist_ok=True)
            # Raw receipt/SBOM records may contain private build paths. Export
            # only their summaries, retaining the originals in the local keg.
            for source in (recipe,):
                if source.is_file() and not source.is_symlink():
                    copy_record(source, root, destination, copied)
            for source in notices:
                copy_record(source, root, destination, copied)
        records.append({
          "input": raw_path.relative_to(root).as_posix(), "input_sha256": sha256(raw_path),
          "formula": formula, "version": version,
          "formula_recipe": {"path": f".brew/{formula}.rb", **recipe_metadata(recipe)},
          "install_receipt": {"path": "INSTALL_RECEIPT.json", **read_json_summary(receipt)},
          "sbom": {"path": "sbom.spdx.json", **read_json_summary(sbom)},
          "notice_files": [{"path": item.relative_to(root).as_posix(), "sha256": sha256(item)} for item in notices],
          "copied_files": copied,
          "missing_provenance": [item for item, present in (("formula recipe", recipe.is_file()),
             ("INSTALL_RECEIPT.json", receipt.is_file()), ("sbom.spdx.json", sbom.is_file()),
             ("license/notice file", bool(notices))) if not present]})
    return {"schema": 1, "purpose": "read-only standalone bundle dependency inventory",
      "redistribution_status": "not-reviewed-not-approved",
      "boundaries": ["No dependency binaries, SDKs, Heimdall source, corresponding source archives, patches, or resources are copied.",
        "Formula metadata and local notice copies do not establish redistribution rights or complete corresponding-source obligations."],
      "provenance_gaps": ["Formula parsing records only direct url, sha256 and depends_on declarations.",
        "It does not resolve formula resources, git revisions, patches, helper downloads, bottles, transitive dependencies, or dynamically loaded runtime closure.",
        "Receipt and SBOM values are summarized without preserving raw metadata, so host-local paths are not shipped."],
      "components": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", action="append", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--notices-dir")
    args = parser.parse_args()
    result = inventory(args.file, args.notices_dir)
    output = Path(args.output)
    if not output.is_absolute(): fail("--output must be an absolute path")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

if __name__ == "__main__":
    try: main()
    except ValueError as error: raise SystemExit(f"macos-license-inventory: {error}")
