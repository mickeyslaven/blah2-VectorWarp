#!/usr/bin/env python3
"""Deterministic local Mac release worker; run from launchd, never GitHub Actions.

Main merges build encrypted runtimes on disposable hosted Macs. An intentional
version tag creates the Linux draft. This worker completes that draft locally
only if the exact CI/source, signatures, notarization and asset gates pass.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REPO = "mickeyslaven/blah2-VectorWarp"
BASE_URL = "https://mickeyslaven.github.io/blah2-VectorWarp/"
MIN_VERSION = (0, 1, 9)
ARCHES = ("arm64", "x86_64")
RELEASE_ASSETS = ("macos-release.json",)


def command(*args, cwd=None, capture=True):
    result = subprocess.run([str(item) for item in args], cwd=cwd, text=True,
                            capture_output=capture, check=True)
    return result.stdout.strip() if capture else ""


def json_command(*args, cwd=None):
    return json.loads(command(*args, cwd=cwd))


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def version(tag):
    match = re.fullmatch(r"v([0-9]+)\.([0-9]+)\.([0-9]+)", tag)
    return tuple(map(int, match.groups())) if match else None


def choose_release(releases):
    candidates = [entry for entry in releases if version(entry.get("tagName", "")) and
                  version(entry["tagName"]) > MIN_VERSION and not entry.get("isPrerelease")]
    return max(candidates, key=lambda entry: version(entry["tagName"])) if candidates else None


def choose_run(runs, commit):
    matches = [entry for entry in runs if entry.get("headSha") == commit and
               entry.get("event") == "push" and entry.get("conclusion") == "success"]
    return max(matches, key=lambda entry: entry["databaseId"]) if matches else None


def config(path):
    value = json.loads(Path(path).read_text())
    required = {"checkout", "cache", "recipient_key", "recipient_certificate",
                "baseline_arm64_inventory", "baseline_x86_64_inventory",
                "baseline_notices", "baseline_source_archive", "application_identity",
                "installer_identity", "team_id", "notary_profile"}
    if set(value) != required or value["team_id"] != "DJGHPX8T7R":
        raise ValueError("invalid local agent configuration")
    for name in required - {"application_identity", "installer_identity", "team_id", "notary_profile"}:
        value[name] = Path(value[name]).expanduser().resolve()
    if not value["recipient_key"].is_file() or value["recipient_key"].stat().st_mode & 0o077:
        raise ValueError("recipient private key is missing or not owner-only")
    return value


def notify(message):
    body = message[:180].replace("\n", " ")
    subprocess.run(["osascript", "-e", "display notification " + json.dumps(body) +
                    ' with title "VectorWarp Mac release"'], capture_output=True)


def state_file(cache, tag):
    root = cache / tag
    root.mkdir(parents=True, exist_ok=True)
    return root / "state.json"


def read_state(path, commit):
    if path.exists():
        data = json.loads(path.read_text())
        if data.get("commit") != commit:
            raise ValueError("local release state belongs to another commit")
        return data
    return {"commit": commit, "stage": "new"}


def save_state(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=".state-",
                                     delete=False) as stream:
        temporary = Path(stream.name)
        os.chmod(temporary, 0o600)
        json.dump(data, stream, sort_keys=True, indent=2)
        stream.write("\n")
    os.replace(temporary, path)


def release_list():
    return json_command("gh", "release", "list", "--limit", "100", "--json",
                        "tagName,isDraft,isPrerelease")


def release_detail(tag):
    return json_command("gh", "release", "view", tag, "--json",
                        "tagName,isDraft,isPrerelease,assets")


def gh_runs(workflow, limit=50):
    return json_command("gh", "run", "list", "--workflow", workflow,
                        "--event", "push", "--limit", str(limit), "--json",
                        "databaseId,headSha,headBranch,event,status,conclusion")


def trusted_tag(checkout, tag):
    command("git", "-C", checkout, "fetch", "--no-tags", "origin", "main",
            "refs/tags/" + tag + ":refs/tags/" + tag)
    commit = command("git", "-C", checkout, "rev-parse", "refs/tags/" + tag + "^{commit}")
    main = command("git", "-C", checkout, "rev-parse", "refs/remotes/origin/main")
    subprocess.run(["git", "-C", str(checkout), "merge-base", "--is-ancestor", commit, main],
                   check=True, capture_output=True)
    return commit


def required_linux_assets(detail, tag):
    names = {entry["name"] for entry in detail["assets"]}
    number = tag[1:]
    expected = {f"vectorwarp_{number}-1_{distro}_{arch}.deb"
                for distro in ("ubuntu22.04", "ubuntu24.04", "ubuntu26.04", "debian13")
                for arch in ("amd64", "arm64")}
    expected.update(f"vectorwarp-{number}-1.fc44.{arch}.rpm" for arch in ("x86_64", "aarch64"))
    expected.update(("SHA256SUMS", "SHA256SUMS.asc", "package-manifest.json"))
    if not expected <= names:
        raise ValueError(f"{tag} lacks the complete Linux draft")


def artifact_names(run_id):
    data = json_command("gh", "api", f"repos/{REPO}/actions/runs/{run_id}/artifacts")
    return {entry["name"] for entry in data.get("artifacts", []) if not entry.get("expired")}


def check_candidate(settings):
    entry = choose_release(release_list())
    if not entry:
        return None
    tag = entry["tagName"]
    detail = release_detail(tag)
    names = {asset["name"] for asset in detail["assets"]}
    if (not detail["isDraft"] and
            {f"vectorwarp-{tag[1:]}-macos-universal.pkg",
             f"vectorwarp-{tag[1:]}-macos-corresponding-source.tar.gz",
             "macos-release.json"} <= names):
        state = settings["cache"] / tag / "state.json"
        if not state.exists() or json.loads(state.read_text()).get("stage") == "complete":
            return None
    required_linux_assets(detail, tag)
    commit = trusted_tag(settings["checkout"], tag)
    linux_run = choose_run(gh_runs("release-packages.yml"), commit)
    if not linux_run:
        return {"tag": tag, "commit": commit, "waiting": "Linux release run"}
    mac_run = choose_run(gh_runs("macos-standalone.yml"), commit)
    if not mac_run:
        return {"tag": tag, "commit": commit, "waiting": "hosted Mac runtime run"}
    run_id = mac_run["databaseId"]
    names = artifact_names(run_id)
    expected = {f"standalone-encrypted-{arch}-{commit}" for arch in ARCHES}
    expected.update(f"standalone-runtime-diagnostics-{arch}" for arch in ARCHES)
    if not expected <= names:
        raise ValueError("successful hosted Mac run lacks encrypted runtimes or diagnostics")
    return {"tag": tag, "commit": commit, "mac_run": run_id,
            "linux_run": linux_run["databaseId"], "draft": detail["isDraft"]}


def ensure_checkout(settings, candidate, work):
    source = work / "source"
    if not source.exists():
        command("git", "-C", settings["checkout"], "worktree", "add", "--detach",
                source, candidate["commit"])
    if command("git", "-C", source, "rev-parse", "HEAD") != candidate["commit"]:
        raise ValueError("candidate source checkout identity changed")
    return source


def download_and_decrypt(settings, candidate, work):
    runtimes = {}
    diagnostics = {}
    for arch in ARCHES:
        root = work / "ci" / arch
        root.mkdir(parents=True, exist_ok=True)
        encrypted = root / "encrypted"
        diagnostic = root / "diagnostic"
        if not (encrypted / f"standalone-{arch}.cms").is_file() or not (
                encrypted / f"standalone-{arch}.transfer.json").is_file():
            command("gh", "run", "download", str(candidate["mac_run"]), "--name",
                    f"standalone-encrypted-{arch}-{candidate['commit']}", "--dir", encrypted)
        if not (diagnostic / "standalone-provenance/inventory.json").is_file() or not (
                diagnostic / "standalone-provenance/header-input-versions.txt").is_file():
            command("gh", "run", "download", str(candidate["mac_run"]), "--name",
                    f"standalone-runtime-diagnostics-{arch}", "--dir", diagnostic)
        runtime = root / "runtime"
        if not runtime.exists():
            temporary = root / "runtime-pending"
            if temporary.exists():
                shutil.rmtree(temporary)
            command(sys.executable, work / "source/script/transfer-macos-runtime.py", "decrypt",
                    "--input", encrypted / f"standalone-{arch}.cms",
                    "--metadata", encrypted / f"standalone-{arch}.transfer.json",
                    "--key", settings["recipient_key"],
                    "--certificate", settings["recipient_certificate"],
                    "--output", temporary, "--expected-source-id", candidate["commit"],
                    "--expected-arch", arch, "--expected-run-id", str(candidate["mac_run"]))
            os.replace(temporary, runtime)
        audit = json_command(sys.executable, work / "source/script/package-macos-standalone.py",
                             "audit", "--runtime", runtime)
        if audit.get("source_id") != candidate["commit"] or audit.get("arch") != arch:
            raise ValueError("decrypted runtime no longer matches trusted candidate")
        runtimes[arch] = runtime
        diagnostics[arch] = diagnostic / "standalone-provenance"
    return runtimes, diagnostics


def prepare_inputs(settings, candidate, work, source, runtimes, diagnostics):
    output = work / "release-inputs"
    if not output.exists():
        command(sys.executable, source / "script/prepare-macos-release-inputs.py",
                "--arm64", runtimes["arm64"], "--x86_64", runtimes["x86_64"],
                "--arm64-inventory", diagnostics["arm64"] / "inventory.json",
                "--x86_64-inventory", diagnostics["x86_64"] / "inventory.json",
                "--arm64-header-versions", diagnostics["arm64"] / "header-input-versions.txt",
                "--x86_64-header-versions", diagnostics["x86_64"] / "header-input-versions.txt",
                "--baseline-arm64-inventory", settings["baseline_arm64_inventory"],
                "--baseline-x86_64-inventory", settings["baseline_x86_64_inventory"],
                "--baseline-notices", settings["baseline_notices"],
                "--baseline-source-archive", settings["baseline_source_archive"],
                "--checkout", source, "--version", candidate["tag"][1:], "--output", output)
    receipt = json.loads((output / "receipt.json").read_text())
    archive = output / receipt["source_archive"]
    if (receipt["source_id"] != candidate["commit"] or
            receipt["source_archive_sha256"] != digest(archive) or
            receipt["notices_sha256"] != digest(output / "notices/notices.json")):
        raise ValueError("release input receipt differs from staged source or notices")
    return output, archive


def sign_package(settings, candidate, work, source, runtimes, inputs):
    assembled = work / "assembled"
    if not assembled.exists():
        pending = work / "assembled-pending"
        if pending.exists():
            shutil.rmtree(pending)
        command(sys.executable, source / "script/package-macos-standalone.py", "assemble",
                "--arm64", runtimes["arm64"], "--x86_64", runtimes["x86_64"],
                "--notices-dir", inputs / "notices", "--version", candidate["tag"][1:],
                "--output", pending)
        os.replace(pending, assembled)
    signed = work / "signed"
    if not signed.exists():
        pending = work / "signed-pending"
        if pending.exists():
            shutil.rmtree(pending)
        command(sys.executable, source / "script/sign-macos-standalone.py", "--app",
                assembled / "VectorWarp.app", "--output", pending, "--developer-id",
                "--application-identity", settings["application_identity"],
                "--installer-identity", settings["installer_identity"],
                "--team-id", settings["team_id"])
        os.replace(pending, signed)
    package = signed / "VectorWarp-signed.pkg"
    if not package.is_file():
        raise ValueError("signed installer missing")
    signature = command("pkgutil", "--check-signature", package)
    if "Developer ID Installer" not in signature or settings["team_id"] not in signature:
        raise ValueError("installer signature does not match the configured team")
    return package


def notarize(settings, package, state, state_path):
    submission = state.get("notary_submission_id")
    if not submission:
        result = json_command("xcrun", "notarytool", "submit", package,
                              "--keychain-profile", settings["notary_profile"],
                              "--output-format", "json")
        submission = result.get("id")
        if not isinstance(submission, str) or not re.fullmatch(r"[0-9a-f-]{36}", submission):
            raise ValueError("Apple did not return a valid notarization submission ID")
        state["notary_submission_id"] = submission
        save_state(state_path, state)
    result = json_command("xcrun", "notarytool", "info", submission,
                          "--keychain-profile", settings["notary_profile"],
                          "--output-format", "json")
    if result.get("status") == "In Progress":
        return None
    if result.get("status") != "Accepted":
        raise ValueError("Apple notarization was not accepted: " + str(result.get("status")))
    if not state.get("stapled"):
        command("xcrun", "stapler", "staple", package)
        state["stapled"] = True
        save_state(state_path, state)
    command("xcrun", "stapler", "validate", package)
    result = subprocess.run(["spctl", "--assess", "--type", "install", "--verbose=4", str(package)],
                            capture_output=True, text=True, check=True)
    assessment = result.stdout + result.stderr
    if "Notarized Developer ID" not in assessment:
        raise ValueError("Gatekeeper did not accept a Notarized Developer ID installer")
    return submission


def release_files(settings, candidate, work, package, archive, submission):
    staging = work / "upload"
    staging.mkdir(exist_ok=True)
    version_text = candidate["tag"][1:]
    pkg_target = staging / f"vectorwarp-{version_text}-macos-universal.pkg"
    source_target = staging / f"vectorwarp-{version_text}-macos-corresponding-source.tar.gz"
    for source, target in ((package, pkg_target), (archive, source_target)):
        if not target.exists():
            os.link(source, target)
        if target.stat().st_size != source.stat().st_size or digest(target) != digest(source):
            raise ValueError("staged release asset differs from its verified input")
    receipt = {"schema": 1, "version": version_text, "publication_commit": candidate["commit"],
               "runtime_source_id": candidate["commit"], "filename": pkg_target.name,
               "sha256": digest(pkg_target), "size": pkg_target.stat().st_size,
               "source_archive": {"filename": source_target.name, "sha256": digest(source_target),
                                  "size": source_target.stat().st_size},
               "apple_team_id": settings["team_id"], "notary_status": "Accepted",
               "notary_submission_id": submission, "gatekeeper": "Notarized Developer ID"}
    receipt_path = staging / "macos-release.json"
    encoded = json.dumps(receipt, sort_keys=True, indent=2) + "\n"
    if receipt_path.exists() and receipt_path.read_text() != encoded:
        raise ValueError("existing local release receipt differs")
    if not receipt_path.exists():
        receipt_path.write_text(encoded)
    return (pkg_target, source_target, receipt_path)


def upload_and_publish(candidate, assets):
    tag = candidate["tag"]
    detail = release_detail(tag)
    required_linux_assets(detail, tag)
    remote = {entry["name"]: entry for entry in detail["assets"]}
    for path in assets:
        item = remote.get(path.name)
        if item is None:
            if not detail["isDraft"]:
                raise ValueError("release is already public without complete Mac assets")
            command("gh", "release", "upload", tag, path)
            detail = release_detail(tag)
            remote = {entry["name"]: entry for entry in detail["assets"]}
            item = remote.get(path.name)
        if (not item or item.get("digest") != "sha256:" + digest(path) or
                item.get("size") != path.stat().st_size):
            raise ValueError("remote release asset digest/size mismatch: " + path.name)
    if detail["isDraft"]:
        command("gh", "release", "edit", tag, "--draft=false")


def verify_public(tag):
    page = urllib.request.urlopen(BASE_URL + "index.html?release=" + tag, timeout=20).read().decode()
    pkg = f"vectorwarp-{tag[1:]}-macos-universal.pkg"
    source = f"vectorwarp-{tag[1:]}-macos-corresponding-source.tar.gz"
    return f"Install VectorWarp {tag[1:]}" in page and pkg in page and source in page


def process(settings, candidate):
    tag, commit = candidate["tag"], candidate["commit"]
    path = state_file(settings["cache"], tag)
    state = read_state(path, commit)
    if state.get("stage") == "complete":
        return "complete"
    if state.get("stage") == "published":
        if verify_public(tag):
            state["stage"] = "complete"
            save_state(path, state)
            return "complete"
        return "waiting for public Pages deployment"
    if "waiting" in candidate:
        return "waiting for " + candidate["waiting"]
    work = path.parent
    source = ensure_checkout(settings, candidate, work)
    runtimes, diagnostics = download_and_decrypt(settings, candidate, work)
    inputs, archive = prepare_inputs(settings, candidate, work, source, runtimes, diagnostics)
    pkg = sign_package(settings, candidate, work, source, runtimes, inputs)
    submission = notarize(settings, pkg, state, path)
    if not submission:
        return "waiting for Apple notarization"
    assets = release_files(settings, candidate, work, pkg, archive, submission)
    upload_and_publish(candidate, assets)
    state["stage"] = "published"
    save_state(path, state)
    if verify_public(tag):
        state["stage"] = "complete"
        save_state(path, state)
        return "complete"
    return "waiting for public Pages deployment"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="read-only candidate report")
    args = parser.parse_args()
    settings = config(args.config)
    if not args.check:
        if command("git", "-C", settings["checkout"], "status", "--porcelain"):
            raise ValueError("agent checkout has local changes; refusing to update trusted main")
        command("git", "-C", settings["checkout"], "pull", "--ff-only", "origin", "main")
    settings["cache"].mkdir(parents=True, mode=0o700, exist_ok=True)
    if settings["cache"].stat().st_mode & 0o077:
        raise ValueError("release cache is not owner-only")
    lock = settings["cache"] / "agent.lock"
    with lock.open("a+") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("another local release-agent run is active")
            return
        candidate = check_candidate(settings)
        if not candidate:
            print("no new release candidate")
            return
        if args.check:
            print(json.dumps(candidate, sort_keys=True))
            return
        result = process(settings, candidate)
        print(candidate["tag"] + ": " + result)
        if result == "complete":
            notify(candidate["tag"] + " Mac release and public download verified")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print("macos-release-agent: " + str(error), file=sys.stderr)
        notify("Mac release needs attention: " + str(error))
        raise SystemExit(1)
