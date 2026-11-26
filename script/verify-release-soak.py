#!/usr/bin/env python3
"""Promote existing CI packages, never rebuild a candidate after its live soak.

The protected release approver attests the supplied host evidence. This verifier
checks its consistency and binds it to independently downloaded CI package bytes;
it does not pretend an unsigned host JSON document is a remote attestation.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "mickeyslaven/blah2-VectorWarp"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


identity = module("soak_identity", ROOT / "tools/vectorwarp_soak_identity.py")
monitor = module("soak_monitor", ROOT / "tools/vectorwarp_two_node_soak.py")
artifact_plan = module("artifact_plan", ROOT / "script/artifact-plan.py")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    require(path.stat().st_size <= 1024 * 1024, "oversized release evidence")
    return json.loads(path.read_text())


def candidate(run, source, run_id, current_run):
    require(re.fullmatch(r"[0-9a-f]{40}", source), "invalid source commit")
    require(re.fullmatch(r"[1-9][0-9]*", run_id or ""), "candidate run ID is required")
    require(run_id != current_run, "cannot promote a new build from the signing run")
    require(str(run.get("id")) == run_id and run.get("conclusion") == "success"
            and run.get("status") == "completed", "candidate run did not succeed")
    require(run.get("event") == "workflow_dispatch" and run.get("head_branch") == "main"
            and run.get("head_sha") == source, "candidate must be an explicit main build of this source")
    require(run.get("path") == ".github/workflows/release-packages.yml"
            and run.get("repository", {}).get("full_name") == REPOSITORY
            and run.get("head_repository", {}).get("full_name") == REPOSITORY,
            "candidate workflow or repository mismatch")
    require(type(run.get("run_attempt")) is int and run["run_attempt"] >= 1,
            "candidate attempt is missing")
    return {"id": run_id, "attempt": run["run_attempt"], "sourceCommit": source,
            "repository": REPOSITORY, "workflow": run["path"]}


def checked_number(value, name, minimum=0):
    require(type(value) in (int, float) and math.isfinite(value) and value >= minimum,
            f"invalid {name}")
    return value


def receipt(envelope, run, source):
    raw = envelope.get("bindingJson")
    require(isinstance(raw, str) and len(raw.encode()) <= 32768, "exact bindingJson bytes required")
    with tempfile.TemporaryDirectory(prefix="vectorwarp-binding-") as directory:
        path = Path(directory) / "binding.json"
        path.write_bytes(raw.encode())
        binding = monitor.load_binding(str(path))
    require(binding["sourceCommit"] == source, "soak source mismatch")
    require(str(binding.get("candidateRunId")) == run["id"]
            and binding.get("candidateRunAttempt") == run["attempt"], "soak candidate run mismatch")
    summary = envelope.get("summary", {})
    require(summary.get("schema") == 3, "unsupported soak summary")
    for key in ("releaseAccepted", "startupAccepted", "steadyStateAccepted", "latestHealthy"):
        require(summary.get(key) is True, f"soak requires {key}")
    require(summary.get("diagnosticWindow") is False, "diagnostic soak cannot qualify release")
    for key in ("latestFailures", "startupFailures"):
        require(summary.get(key) == [], f"soak has {key}")
    bound = summary.get("releaseBinding", {})
    require(bound.get("expected") == binding and bound.get("sha256") ==
            hashlib.sha256(raw.encode()).hexdigest(), "soak raw binding mismatch")
    for key, filename in (("monitorSha256", "vectorwarp_two_node_soak.py"),
                          ("probeSha256", "vectorwarp_soak_identity.py")):
        require(bound.get(key) == identity.file_hash(ROOT / "tools" / filename),
                "soak monitor/probe differs from frozen source")
    window = summary.get("cleanWindow", {})
    policy = window.get("policy", {})
    duration = checked_number(policy.get("cleanWindowSeconds"), "clean window", monitor.DEFAULT_CLEAN_WINDOW_SECONDS)
    gap = checked_number(policy.get("maximumSampleGapSeconds"), "sample gap", 1)
    require(gap <= 180, "soak observation gaps exceed release policy")
    clean = checked_number(window.get("cleanSeconds"), "clean duration", duration)
    last = checked_number(window.get("lastSampleEpoch"), "last sample")
    start = checked_number(window.get("cleanSinceEpoch"), "clean start")
    require(abs(last - start - clean) < 0.001, "inconsistent clean window")
    require(type(window.get("cleanSamples")) is int
            and window["cleanSamples"] >= math.ceil(clean / gap) + 1, "insufficient soak samples")
    require(window.get("startupAccepted") is True and window.get("steadyStateAccepted") is True
            and window.get("startupFailures") == [], "clean window did not qualify")
    latest = summary.get("latest", {})
    require(latest.get("epoch") == last and latest.get("healthy") is True
            and latest.get("failures") == [], "latest soak sample did not qualify")
    require(set(latest.get("nodes", {})) == set(binding["nodes"]),
            "latest soak node set differs from the reviewed topology")
    require(monitor.counter(latest.get("monitorFreeBytes")) and
            latest["monitorFreeBytes"] >= binding["monitorMinimumFreeBytes"],
            "latest monitor disk reserve did not qualify")
    require(not monitor.verify_regression(latest.get("regressionEvidence", {}), bound,
                last, checked_number(summary.get("startedEpoch"), "cohort start")),
            "latest regression evaluation did not qualify")
    for name, node in binding["nodes"].items():
        filename = node.get("packageFilename", "")
        require(re.fullmatch(r"vectorwarp[-_][A-Za-z0-9_.+-]+\.(deb|rpm)", filename),
                "soak package filename required")
        require(re.fullmatch(r"[0-9a-f]{64}", node.get("packageSha256", "")),
                "soak package SHA256 required")
        measured = latest.get("nodes", {}).get(name, {})
        require(not monitor.verify_binding(name, measured.get("status", {}),
                    measured.get("process", {}), binding), "latest identity differs from soak binding")
    return binding


def unpack(package, destination):
    # These are authenticated same-source CI outputs. Never run package scripts.
    if package.suffix == ".deb":
        subprocess.run(["dpkg-deb", "--extract", str(package), str(destination)],
                       check=True, timeout=120, stdout=subprocess.DEVNULL)
    elif package.suffix == ".rpm":
        with tempfile.TemporaryFile() as archive:
            subprocess.run(["rpm2cpio", str(package)], stdout=archive, check=True, timeout=120)
            archive.seek(0)
            subprocess.run(["cpio", "--extract", "--make-directories", "--no-absolute-filenames",
                            "--quiet"], stdin=archive, cwd=destination, check=True, timeout=120)
    else:
        raise ValueError("unsupported candidate package")


def payload(package, source, version):
    with tempfile.TemporaryDirectory(prefix="vectorwarp-release-payload-") as directory:
        root = Path(directory)
        unpack(package.resolve(), root)
        release = root / "opt/vectorwarp/current"
        lines = (release / ".vectorwarp-build").read_text().splitlines()
        require(lines.count(f"source_commit={source}") == 1
                and sum(line.startswith("source_commit=") for line in lines) == 1
                and lines.count("source_clean=true") == 1
                and sum(line.startswith("source_clean=") for line in lines) == 1,
                "package is not a clean build of the frozen source")
        build = read_json(release / "BUILD-RECEIPT.json")
        require(build.get("sourceCommit") == source and build.get("version") == version,
                "package build receipt mismatch")
        return {"artifactSha256": identity.artifact_hash(release),
                "runtimeSha256": {
                    "vectorwarp-processor.service": identity.file_hash(release / "bin/blah2"),
                    "vectorwarp-api.service": identity.file_hash(root / "opt/vectorwarp/runtime/node/bin/node")}}


def inventory(packages, manifest, source, version):
    result = {}
    require(manifest.get("source_commit") == source and manifest.get("version") == version,
            "aggregate package source/version mismatch")
    for entry in manifest.get("packages", []):
        name = entry.get("filename", "")
        require(re.fullmatch(r"vectorwarp[-_][A-Za-z0-9_.+-]+\.(deb|rpm)", name)
                and name not in result, "invalid or duplicate package filename")
        package = packages / entry.get("repository_path", name)
        require(package.resolve().is_relative_to(packages.resolve()), "package escapes artifact directory")
        require(identity.file_hash(package) == entry["sha256"] and package.stat().st_size == entry["size"],
                "candidate package bytes differ from manifest")
        result[name] = {"packageSha256": entry["sha256"], **payload(package, source, version)}
    require(set(result) == artifact_plan.release_filenames(version),
            "promotion requires the complete exact package target matrix")
    return result


def bind_packages(binding, packages):
    for name, node in binding["nodes"].items():
        package = packages.get(node["packageFilename"], {})
        require(package.get("packageSha256") == node["packageSha256"], f"{name}: candidate package changed")
        require(package.get("artifactSha256") == node["artifactSha256"],
                f"{name}: installed tree was not the candidate package payload")
        for unit, digest in package["runtimeSha256"].items():
            require(node["runtimeExecutables"][unit]["sha256"] == digest,
                    f"{name}: running executable was not supplied by candidate package")


def signed_packages(before, after):
    require(before.keys() == after.keys(), "signed package set changed")
    result = {}
    for name, old in before.items():
        new = after[name]
        require(old["artifactSha256"] == new["artifactSha256"]
                and old["runtimeSha256"] == new["runtimeSha256"], "signing changed candidate payload")
        require(name.endswith(".rpm") or old["packageSha256"] == new["packageSha256"],
                "signing changed an immutable Debian package")
        result[name] = {**old, "signedPackageSha256": new["packageSha256"]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("candidate", "verify", "signed"))
    parser.add_argument("--run", type=Path, default=Path("candidate-run.json"))
    parser.add_argument("--packages", type=Path, default=Path("packages"))
    parser.add_argument("--manifest", type=Path, default=Path("packages/package-manifest.json"))
    parser.add_argument("--output", type=Path, default=Path("release-soak-provenance.json"))
    args = parser.parse_args()
    if args.mode == "signed":
        result = read_json(args.output)
        actual = inventory(args.packages, read_json(args.manifest), result["source_commit"], result["version"])
        result["packages"] = signed_packages(result["packages"], actual)
    else:
        source, version = os.environ["GITHUB_SHA"], os.environ["VERSION"]
        run = candidate(read_json(args.run), source, os.environ.get("CANDIDATE_RUN_ID"),
                        os.environ["GITHUB_RUN_ID"])
        if args.mode == "candidate":
            print(json.dumps(run, sort_keys=True))
            return
        raw = os.environ.get("VECTORWARP_SOAK_RECEIPT", "")
        require(len(raw.encode()) <= 65536, "oversized soak receipt")
        envelope = json.loads(raw)
        binding = receipt(envelope, run, source)
        packages = inventory(args.packages, read_json(args.manifest), source, version)
        bind_packages(binding, packages)
        result = {"schema": 1, "source_commit": source, "version": version,
                  "candidateRun": run, "soakEvidence": envelope, "packages": packages,
                  "authorization": "Explicit release-signing environment approval of host evidence"}
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
