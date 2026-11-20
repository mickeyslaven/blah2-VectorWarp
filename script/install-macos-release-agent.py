#!/usr/bin/env python3
"""Install the deterministic VectorWarp release worker as a local LaunchAgent.

Run only on the owner's Mac after this script is merged to main. The public
repository never receives the signing identities or recipient private key.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LABEL = "com.vectorwarp.macos-release-agent"
BASE = Path.home() / "Library/Application Support/VectorWarp/ReleaseAgent"
PLIST = Path.home() / "Library/LaunchAgents" / (LABEL + ".plist")
CACHE = Path.home() / "Library/Caches/VectorWarp/release-agent"
PRIVATE = ROOT / "build/standalone-macos/private-transfer"


def run(*args):
    return subprocess.run([str(item) for item in args], text=True,
                          capture_output=True, check=True).stdout.strip()


def check(args):
    if sys.platform != "darwin" or os.geteuid() == 0:
        raise ValueError("install this agent as the logged-in macOS user, not root")
    sources = {
        "recipient_key": PRIVATE / "recipient-key.pem",
        "recipient_certificate": PRIVATE / "recipient-cert.pem",
        "input_manifest": args.input_manifest.absolute(),
        "current_notices": args.notices.absolute(),
    }
    if args.sources is not None:
        sources["public_sources"] = args.sources.absolute()
    if not sources["recipient_key"].is_file() or sources["recipient_key"].stat().st_mode & 0o077:
        raise ValueError("recipient private key is missing or not owner-only")
    for name, path in sources.items():
        if path.is_symlink():
            raise ValueError("local inputs must not be symlinks")
        if name in {"current_notices", "public_sources"}:
            if not path.is_dir():
                raise ValueError("current reviewed input directory is missing")
        elif not path.is_file():
            raise ValueError("required local input is missing: " + name)
    if sources["input_manifest"].stat().st_mode & 0o077:
        raise ValueError("current input manifest must be owner-only")
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
    (BASE / "inputs").mkdir(mode=0o700)
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
        target = (BASE / "private" if name.startswith("recipient_") else BASE / "inputs") / name
        if name in {"current_notices", "public_sources"}:
            shutil.copytree(source, target, symlinks=False)
        else:
            shutil.copy2(source, target)
        if name in {"recipient_key", "input_manifest"}:
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
    parser.add_argument("--input-manifest", type=Path, required=True)
    parser.add_argument("--notices", type=Path, required=True)
    parser.add_argument("--sources", type=Path)
    args = parser.parse_args()
    sources = check(args)
    if args.check:
        print("local signing, notarization, GitHub auth and current reviewed inputs are ready")
    else:
        install(sources)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("install-macos-release-agent: " + str(error), file=sys.stderr)
        raise SystemExit(1)
