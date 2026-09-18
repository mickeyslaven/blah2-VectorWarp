#!/usr/bin/env python3
"""Render a public Homebrew-tap candidate from a verified GitHub commit archive.

The caller downloads the archive in a trusted workflow. This tool never contacts
the network or publishes a tap, release, or bottle.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
from urllib.parse import urlparse


TAP = "mickeyslaven/vectorwarp"
SOURCE_REPOSITORY = "mickeyslaven/blah2-VectorWarp"
ALLOWED_ROOT_DIRS = {".github", "Formula", "api", "bench", "cmake", "config", "contrib", "docs", "html", "lib", "packaging", "script", "src", "test"}
ALLOWED_ROOT_FILES = {".gitattributes", ".gitignore", "CMakeLists.txt", "CMakePresets.json", "Doxyfile", "LICENSE", "README.md"}
DENIED_ROOTS = {".git", "bin", "build", "data", "node_modules", "private", "sdk", "secrets"}
DENIED_FILE_SUFFIXES = {".dmg", ".dylib", ".mchq", ".pkg", ".so"}
VERSION = re.compile(r"^[0-9]+(?:\.[0-9]+){1,3}(?:-[0-9A-Za-z.-]+)?$")
COMMIT_URL = re.compile(r"^https://github\.com/mickeyslaven/blah2-VectorWarp/archive/([0-9a-f]{40})\.tar\.gz$")


def fail(message: str) -> None:
    raise ValueError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_commit(source_url: str) -> str:
    parsed = urlparse(source_url)
    if parsed.query or parsed.fragment:
        fail("--source-url must not contain a query or fragment")
    match = COMMIT_URL.fullmatch(source_url)
    if not match:
        fail("--source-url must be https://github.com/mickeyslaven/blah2-VectorWarp/archive/<40-lowercase-hex>.tar.gz")
    return match.group(1)


def relative_member(name: str, commit: str) -> PurePosixPath | None:
    raw_name = name[:-1] if name.endswith("/") else name
    raw_parts = raw_name.split("/")
    if not raw_name or any(part in {"", ".", ".."} for part in raw_parts):
        fail(f"unsafe archive member path: {name!r}")
    path = PurePosixPath(raw_name)
    if path.is_absolute():
        fail(f"unsafe archive member path: {name!r}")
    root = f"blah2-VectorWarp-{commit}"
    if path.parts == (root,):
        return None
    if len(path.parts) < 2 or path.parts[0] != root:
        fail(f"archive member must be rooted at {root}: {name}")
    relative = PurePosixPath(*path.parts[1:])
    if relative.parts[0].lower() in DENIED_ROOTS:
        fail(f"excluded archive member was selected: {name}")
    if relative.parts[0] not in ALLOWED_ROOT_DIRS:
        if len(relative.parts) != 1 or relative.parts[0] not in ALLOWED_ROOT_FILES:
            fail(f"unexpected archive member outside the public allowlist: {name}")
    filename = relative.name.lower()
    if filename.startswith("sdrplay_rsp_api") or any(filename.endswith(suffix) for suffix in DENIED_FILE_SUFFIXES):
        fail(f"excluded SDK, binary, or capture member was selected: {name}")
    return relative


def safe_symlink(member: tarfile.TarInfo, relative: PurePosixPath) -> None:
    target = PurePosixPath(member.linkname)
    if not member.linkname or target.is_absolute():
        fail(f"unsafe symlink in source archive: {relative}")
    parts = list(relative.parent.parts)
    for part in target.parts:
        if part in {"", "."}:
            continue
        if part == "..":
            if not parts:
                fail(f"symlink escapes source archive: {relative}")
            parts.pop()
        else:
            parts.append(part)
    if not parts:
        fail(f"unsafe symlink in source archive: {relative}")


def templates_from_archive(source_archive: Path, commit: str) -> dict[str, str]:
    wanted = {"Formula/vectorwarp.rb.in", "Formula/vectorwarp-heimdall.rb.in"}
    templates: dict[str, str] = {}
    seen: set[str] = set()
    try:
        with tarfile.open(source_archive, "r:gz") as archive:
            for member in archive.getmembers():
                relative = relative_member(member.name, commit)
                if relative is None or member.isdir():
                    continue
                key = str(relative)
                if key in seen:
                    fail(f"duplicate source archive member: {key}")
                seen.add(key)
                if member.issym() or member.islnk():
                    if member.islnk():
                        fail(f"hard links are not permitted in the source archive: {relative}")
                    safe_symlink(member, relative)
                    continue
                if not member.isfile():
                    fail(f"source archive contains unsupported member type: {relative}")
                if key in wanted:
                    stream = archive.extractfile(member)
                    if stream is None:
                        fail(f"cannot read formula template: {key}")
                    templates[key] = stream.read().decode("utf-8")
    except tarfile.TarError as error:
        fail(f"cannot read --source-archive as gzip tar: {error}")
    missing = wanted - templates.keys()
    if missing:
        fail(f"source archive lacks required formula templates: {', '.join(sorted(missing))}")
    return templates


def render(template: str, source_url: str, checksum: str, version: str, revision: int) -> str:
    content = template.replace("@SOURCE_URL@", source_url).replace("@SOURCE_SHA256@", checksum)
    content = content.replace("@FORMULA_REVISION@", str(revision))
    content, count = re.subn(r'(?m)^  version ".*"$', f'  version "{version}"', content, count=1)
    content = content.replace("vectorwarp/local/vectorwarp-heimdall", f"{TAP}/vectorwarp-heimdall")
    content = content.replace('desc "Local passive-radar settings and processor application"',
                              'desc "Passive-radar settings and processor application"')
    content = content.replace('desc "Native local KrakenSDR controller for VectorWarp development"',
                              'desc "Native KrakenSDR controller for VectorWarp"')
    if count != 1 or any(item in content for item in ("@SOURCE_", "@FORMULA_REVISION@", "vectorwarp/local", "file://", "macos-dev", "for VectorWarp development")):
        fail("formula template retained local-only or unresolved content")
    return content


def readme(version: str, revision: int, commit: str, checksum: str) -> str:
    return f"""# VectorWarp Homebrew tap

Generated from immutable main commit `{commit}`, with version `{version}` and
Homebrew revision `{revision}`. Source SHA-256:

```
{checksum}
```

The main-branch publishing workflow builds and tests both formulas on Apple
Silicon before updating this tap. The formulas build from source; no bottles
are produced. Intel macOS remains experimental and is not qualified by the
Apple Silicon job. See the [validation limits](https://github.com/{SOURCE_REPOSITORY}/blob/{commit}/docs/MACOS_TEST_MATRIX.md).

## Install

Install Homebrew and Xcode Command Line Tools first, then:

```sh
brew tap {TAP}
brew install vectorwarp
vectorwarp
```

Homebrew's full name is `{TAP}/vectorwarp` (`owner/tap/package`). Once the
tap is added, the short package names work for installation and updates.

Choose a receiver or replay in Settings, then **Save & Restart**. The app
includes the local USB Kraken companion and open UHD/HackRF adapters. The
companion fetches pinned public upstream sources; the Suite's repository-wide
redistribution terms remain unresolved, so companion bottles are not supplied.
VectorWarp never fetches, bundles, or installs the proprietary SDRplay SDK.

## Update

```sh
brew update
brew upgrade vectorwarp vectorwarp-heimdall
```

Both components build from source. Settings and recordings stay in your
application-support directory. After upgrading either component, restart a
manually started instance with `vectorwarp restart`. If you registered a
Homebrew service, use `brew services restart vectorwarp` instead.

## Everyday commands

For a manually started instance, `vectorwarp` (or `vectorwarp open`),
`vectorwarp start`, `vectorwarp stop`,
`vectorwarp restart`, `vectorwarp status`, `vectorwarp logs` and
`vectorwarp help` have the same everyday roles as on Linux. Opening the web
interface alone does not start radar processing.

The Mac launcher does not currently implement `vectorwarp version`; use
`brew list --versions vectorwarp vectorwarp-heimdall` to inspect installed
versions. Package and login-service management use Homebrew and `brew services`,
rather than Linux `apt`, `dnf` or `systemctl` commands. No `sudo` is needed for
the per-user Mac service.

## Optional login service

After configuring a receiver or replay, register the per-user service with:

```sh
brew services start vectorwarp
```

Use `brew services stop vectorwarp` to keep a supervised instance stopped,
and `brew services restart vectorwarp` to restart it. A plain `vectorwarp stop`
stops the child processes but leaves the supervisor running, so it starts them
again. `vectorwarp status` and `vectorwarp logs` work for either launch method.

## Remove

```sh
brew services stop vectorwarp
vectorwarp stop
brew uninstall vectorwarp
brew uninstall vectorwarp-heimdall
brew untap {TAP}
```

Configuration and recordings remain in your application-support directory.
To migrate from the development `vectorwarp/local` tap, stop its service and
instance, uninstall both local formulas, and untap `vectorwarp/local` before
installing this tap. Do not rely on an upgrade to change tap ownership.
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--revision", type=int, required=True)
    parser.add_argument("--output-tap-dir", type=Path, required=True)
    args = parser.parse_args()
    if not VERSION.fullmatch(args.version):
        fail("--version must be a release version without spaces or path characters")
    if args.revision < 1:
        fail("--revision must be a positive integer")
    commit = source_commit(args.source_url)
    source_archive = args.source_archive.resolve()
    if not source_archive.is_file():
        fail("--source-archive must name an existing downloaded archive")
    checksum = sha256(source_archive)
    templates = templates_from_archive(source_archive, commit)
    output = args.output_tap_dir.resolve()
    formula_dir = output / "Formula"
    formula_dir.mkdir(parents=True, exist_ok=True)
    app = render(templates["Formula/vectorwarp.rb.in"], args.source_url, checksum, args.version, args.revision)
    companion = render(templates["Formula/vectorwarp-heimdall.rb.in"], args.source_url, checksum, args.version, args.revision)
    readme_text = readme(args.version, args.revision, commit, checksum)
    (formula_dir / "vectorwarp.rb").write_text(app, encoding="utf-8")
    (formula_dir / "vectorwarp-heimdall.rb").write_text(companion, encoding="utf-8")
    (output / "README.md").write_text(readme_text, encoding="utf-8")
    manifest = {"source_commit": commit, "source_url": args.source_url, "source_sha256": checksum,
                "version": args.version, "revision": args.revision,
                "files": {"Formula/vectorwarp.rb": hashlib.sha256(app.encode()).hexdigest(),
                          "Formula/vectorwarp-heimdall.rb": hashlib.sha256(companion.encode()).hexdigest(),
                          "README.md": hashlib.sha256(readme_text.encode()).hexdigest()}}
    (output / "homebrew-release.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Prepared tap candidate: {output}")
    print(f"Commit: {commit}; source SHA-256: {checksum}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(f"package-homebrew-release: {error}")
