#!/usr/bin/env python3
"""Install the deterministic VectorWarp release worker as a local LaunchAgent.

Run only on the owner's Mac after this script is merged to main. The public
repository never receives the signing identities or recipient private key.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LABEL = "com.vectorwarp.macos-release-agent"
SOURCE_SHA256 = "193456313f51fc6d330c4e8332beec4957adfad8a80e4a4d81dea6fd7fbb19cd"
BASE = Path.home() / "Library/Application Support/VectorWarp/ReleaseAgent"
PLIST = Path.home() / "Library/LaunchAgents" / (LABEL + ".plist")
CACHE = Path.home() / "Library/Caches/VectorWarp/release-agent"
ACCEPTED = Path.home() / "Library/Caches/VectorWarp/standalone-ci489-finalizer-review"
V019 = Path.home() / "Library/Caches/VectorWarp/release-v0.1.9"
PRIVATE = ROOT / "build/standalone-macos/private-transfer"
DIAGNOSTICS = ROOT / "build/standalone-macos"


def sha256(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def run(*args):
    return subprocess.run([str(item) for item in args], text=True,
                          capture_output=True, check=True).stdout.strip()


def check():
    if sys.platform != "darwin" or os.geteuid() == 0:
        raise ValueError("install this agent as the logged-in macOS user, not root")
    sources = {
        "recipient_key": PRIVATE / "recipient-key.pem",
        "recipient_certificate": PRIVATE / "recipient-cert.pem",
        "baseline_arm64_inventory": DIAGNOSTICS / "ci489-arm-diagnostics/standalone-provenance/inventory.json",
        "baseline_x86_64_inventory": DIAGNOSTICS / "ci489-x86-diagnostics/standalone-provenance/inventory.json",
        "baseline_notices": ACCEPTED / "VectorWarp.app/Contents/Resources/ThirdPartyNotices",
        "baseline_source_archive": V019 / "vectorwarp-0.1.9-macos-corresponding-source.tar.gz",
    }
    if not sources["recipient_key"].is_file() or sources["recipient_key"].stat().st_mode & 0o077:
        raise ValueError("recipient private key is missing or not owner-only")
    for name, path in sources.items():
        if name == "baseline_notices":
            if not path.is_dir():
                raise ValueError("accepted baseline notice directory is missing")
        elif not path.is_file():
            raise ValueError("required local input is missing: " + name)
    if sha256(sources["baseline_source_archive"]) != SOURCE_SHA256:
        raise ValueError("accepted v0.1.9 source archive hash differs")
    ids = run("security", "find-identity", "-v", "-p", "basic")
    for name in ("Developer ID Application: Mix Slaven (DJGHPX8T7R)",
                 "Developer ID Installer: Mix Slaven (DJGHPX8T7R)"):
        if name not in ids:
            raise ValueError("local signing identity is unavailable: " + name)
    run("xcrun", "notarytool", "history", "--keychain-profile", "VectorWarp-notary",
        "--output-format", "json")
    run("gh", "auth", "status")
    return sources


def install(sources):
    if PLIST.exists() or BASE.exists():
        raise ValueError("agent installation already exists; refusing to overwrite it")
    BASE.mkdir(parents=True, mode=0o700)
    (BASE / "baseline").mkdir(mode=0o700)
    (BASE / "private").mkdir(mode=0o700)
    (BASE / "logs").mkdir(mode=0o700)
    CACHE.mkdir(parents=True, mode=0o700, exist_ok=True)
    if CACHE.stat().st_mode & 0o077:
        raise ValueError("release cache is not owner-only")
    subprocess.run(["git", "clone", "--branch", "main", "--single-branch", "--no-tags",
                    "https://github.com/mickeyslaven/blah2-VectorWarp.git", str(BASE / "repo")],
                   check=True)
    if not (BASE / "repo/script/macos-release-agent.py").is_file():
        raise ValueError("merged main does not yet contain the release agent")
    staged = {}
    for name, source in sources.items():
        target = (BASE / "private" if name.startswith("recipient_") else BASE / "baseline") / source.name
        if name == "baseline_notices":
            target = BASE / "baseline/notices"
            shutil.copytree(source, target, symlinks=False)
        elif name == "baseline_source_archive":
            target = BASE / "baseline/corresponding-source-v0.1.9.tar.gz"
            os.link(source, target)
        else:
            if target.exists():
                target = target.with_name(name + "-" + target.name)
            shutil.copy2(source, target)
        if name == "recipient_key":
            target.chmod(0o600)
        staged[name] = str(target)
    configuration = {"checkout": str(BASE / "repo"), "cache": str(CACHE), **staged,
                     "application_identity": "Developer ID Application: Mix Slaven (DJGHPX8T7R)",
                     "installer_identity": "Developer ID Installer: Mix Slaven (DJGHPX8T7R)",
                     "team_id": "DJGHPX8T7R", "notary_profile": "VectorWarp-notary"}
    config_path = BASE / "config.json"
    config_path.write_text(json.dumps(configuration, sort_keys=True, indent=2) + "\n")
    config_path.chmod(0o600)
    check_result = subprocess.run([sys.executable, str(BASE / "repo/script/macos-release-agent.py"),
                                   "--config", str(config_path), "--check"],
                                  capture_output=True, text=True)
    if check_result.returncode:
        raise ValueError("agent dry-run failed: " + check_result.stderr.strip()[:500])
    PLIST.parent.mkdir(parents=True, exist_ok=True)
    data = {"Label": LABEL, "ProgramArguments": ["/usr/bin/python3",
            str(BASE / "repo/script/macos-release-agent.py"), "--config", str(config_path)],
            "StartInterval": 900, "RunAtLoad": True, "WorkingDirectory": str(BASE / "repo"),
            "EnvironmentVariables": {"PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"},
            "StandardOutPath": str(BASE / "logs/stdout.log"),
            "StandardErrorPath": str(BASE / "logs/stderr.log")}
    PLIST.write_bytes(plistlib.dumps(data))
    PLIST.chmod(0o600)
    run("launchctl", "bootstrap", f"gui/{os.getuid()}", PLIST)
    print(json.dumps({"installed": True, "label": LABEL, "config": str(config_path),
                      "plist": str(PLIST), "initial_check": check_result.stdout.strip()}, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify prerequisites without installing")
    args = parser.parse_args()
    sources = check()
    if args.check:
        print("local signing, notarization, GitHub auth and accepted baseline inputs are ready")
    else:
        install(sources)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("install-macos-release-agent: " + str(error), file=sys.stderr)
        raise SystemExit(1)
