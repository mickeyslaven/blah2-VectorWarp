#!/usr/bin/env python3
"""Offline negative controls for exact-candidate promotion (no build or device)."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("release_soak", ROOT / "script/verify-release-soak.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
SOURCE = "a" * 40
HASH = "b" * 64


def fixture():
    run = {"id": "123", "attempt": 1}
    node = {"release": "/opt/vectorwarp/current", "artifactSha256": HASH,
            "endpoint": "http://127.0.0.1:3000", "ssh": None,
            "disk": {"path": "/var/lib", "minimumFreeBytes": 200_000_000_000},
            "qualifications": {"radar-behavior": HASH, "physical-gpu": HASH},
            "configSha256": HASH, "packageFilename": "vectorwarp_1.2.3_amd64.deb",
            "packageSha256": HASH,
            "hostIdSha256": "c" * 64,
            "acceleration": "vulkan", "clutterAcceleration": "vulkan", "maximumCadenceMs": 600,
            "runtimeModules": {unit: {} for unit in gate.identity.UNITS},
            "runtimeExecutables": {unit: {"path": "/bin/" + unit, "sha256": HASH}
                                   for unit in gate.identity.UNITS}}
    binding = {"schema": 3, "sourceCommit": SOURCE, "candidateRunId": "123",
               "monitorMinimumFreeBytes": 200_000_000_000,
               "regressionPolicySha256": "d" * 64,
               "candidateRunAttempt": 1, "nodes": {name: copy.deepcopy(node)
                                                  for name in ("homeportal", "hp")}}
    binding["nodes"]["hp"]["hostIdSha256"] = "e" * 64
    raw = json.dumps(binding)
    summary = {"schema": 3, "startedEpoch": 13600, "releaseAccepted": True, "startupAccepted": True,
               "steadyStateAccepted": True, "latestHealthy": True,
               "diagnosticWindow": False, "latestFailures": [], "startupFailures": [],
               "releaseBinding": {"expected": binding, "sha256": hashlib.sha256(raw.encode()).hexdigest(),
                   "monitorSha256": gate.identity.file_hash(ROOT / "tools/vectorwarp_two_node_soak.py"),
                   "probeSha256": gate.identity.file_hash(ROOT / "tools/vectorwarp_soak_identity.py")},
               "cleanWindow": {"policy": {"cleanWindowSeconds": 86400, "maximumSampleGapSeconds": 180},
                   "cleanSeconds": 86400, "cleanSamples": 1441, "lastSampleEpoch": 100000,
                   "cleanSinceEpoch": 13600, "startupAccepted": True, "startupFailures": [],
                   "steadyStateAccepted": True},
               "latest": {"epoch": 100000, "healthy": True, "failures": [], "nodes": {},
                   "monitorFreeBytes": 300_000_000_000,
                   "regressionEvidence": {"schema": 1, "bindingSha256": hashlib.sha256(raw.encode()).hexdigest(),
                       "policySha256": "d" * 64,
                       "nodes": {name: {"retiredCpis": 0, "retiredSamples": 0,
                           "qualifications": node["qualifications"]} for name in binding["nodes"]},
                       "evidenceSha256": "f" * 64, "evaluationStartEpoch": 13600,
                       "evaluatedThroughEpoch": 100000, "regressionDetected": False}}}
    for name, expected in binding["nodes"].items():
        summary["latest"]["nodes"][name] = {"status": {"build": {"commit": SOURCE},
            "loadedRevision": HASH, "configRevision": HASH}, "process": copy.deepcopy(expected)}
        summary["latest"]["nodes"][name]["process"]["disk"] = {
            "path": "/var/lib", "freeBytes": 300_000_000_000}
    return {"bindingJson": raw, "summary": summary}, run, binding


class ExactCandidatePromotion(unittest.TestCase):
    def test_candidate_must_be_successful_same_source_main_workflow(self):
        valid = {"id": 123, "run_attempt": 1, "status": "completed", "conclusion": "success",
                 "event": "workflow_dispatch", "head_branch": "main", "head_sha": SOURCE,
                 "path": ".github/workflows/release-packages.yml",
                 "repository": {"full_name": gate.REPOSITORY},
                 "head_repository": {"full_name": gate.REPOSITORY}}
        self.assertEqual(gate.candidate(valid, SOURCE, "123", "456")["id"], "123")
        for key, value in (("event", "pull_request"), ("head_sha", "c" * 40),
                           ("head_branch", "other"), ("conclusion", "failure"),
                           ("path", "untrusted.yml"), ("run_attempt", 0),
                           ("head_repository", {"full_name": "fork/repo"})):
            with self.subTest(key=key), self.assertRaises(ValueError):
                gate.candidate({**valid, key: value}, SOURCE, "123", "456")
        with self.assertRaises(ValueError):
            gate.candidate(valid, SOURCE, "123", "123")

    def test_full_24_hour_receipt_is_accepted(self):
        envelope, run, binding = fixture()
        self.assertEqual(gate.receipt(envelope, run, SOURCE), binding)

    def test_receipt_matches_arbitrary_reviewed_topology_and_qualification_set(self):
        for count in (1, 2, 3):
            envelope, run, binding = fixture()
            latest = envelope["summary"]["latest"]
            template = copy.deepcopy(binding["nodes"]["homeportal"])
            measured = copy.deepcopy(latest["nodes"]["homeportal"])
            binding["nodes"] = {}; latest["nodes"] = {}
            regression = latest["regressionEvidence"]; regression["nodes"] = {}
            for index in range(count):
                name = f"site-{index}"
                binding["nodes"][name] = copy.deepcopy(template)
                binding["nodes"][name]["hostIdSha256"] = f"{index + 1:064x}"
                latest["nodes"][name] = copy.deepcopy(measured)
                latest["nodes"][name]["process"]["hostIdSha256"] = f"{index + 1:064x}"
                regression["nodes"][name] = {"retiredCpis": 0, "retiredSamples": 0,
                                           "qualifications": template["qualifications"]}
            envelope["bindingJson"] = json.dumps(binding)
            digest = hashlib.sha256(envelope["bindingJson"].encode()).hexdigest()
            envelope["summary"]["releaseBinding"].update(expected=binding, sha256=digest)
            regression["bindingSha256"] = digest
            self.assertEqual(gate.receipt(envelope, run, SOURCE), binding)
            for field, value in (("nodes", {}), ("monitorFreeBytes", 0)):
                bad = copy.deepcopy(envelope); bad["summary"]["latest"][field] = value
                with self.assertRaises(ValueError): gate.receipt(bad, run, SOURCE)
            bad = copy.deepcopy(envelope)
            bad["summary"]["latest"]["regressionEvidence"]["nodes"]["site-0"]["qualifications"] = {}
            with self.assertRaises(ValueError): gate.receipt(bad, run, SOURCE)

    def test_missing_short_diagnostic_or_inconsistent_soak_is_rejected(self):
        for field, value in (("releaseAccepted", False), ("startupAccepted", False),
                             ("steadyStateAccepted", False), ("latestHealthy", False),
                             ("diagnosticWindow", True), ("startupFailures", ["loss"]),
                             ("latestFailures", ["fault"])):
            envelope, run, _ = fixture()
            envelope["summary"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                gate.receipt(envelope, run, SOURCE)
        for field, value in (("cleanSeconds", 86399), ("cleanSamples", 2),
                             ("cleanSinceEpoch", 1), ("startupAccepted", False)):
            envelope, run, _ = fixture()
            envelope["summary"]["cleanWindow"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                gate.receipt(envelope, run, SOURCE)
        for field, value in (("cleanWindowSeconds", 60), ("maximumSampleGapSeconds", 181),
                             ("maximumSampleGapSeconds", float("nan"))):
            envelope, run, _ = fixture()
            envelope["summary"]["cleanWindow"]["policy"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                gate.receipt(envelope, run, SOURCE)

    def test_binding_source_run_monitor_and_live_identity_are_immutable(self):
        for case in ("raw", "source", "run", "attempt", "monitor", "probe", "live"):
            envelope, run, _ = fixture()
            source = SOURCE
            if case == "raw": envelope["bindingJson"] += "\n"
            if case == "source": source = "c" * 40
            if case == "run": run["id"] = "789"
            if case == "attempt": run["attempt"] = 2
            if case in ("monitor", "probe"):
                envelope["summary"]["releaseBinding"][case + "Sha256"] = "c" * 64
            if case == "live":
                envelope["summary"]["latest"]["nodes"]["hp"]["process"]["artifactSha256"] = "c" * 64
            with self.subTest(case=case), self.assertRaises(ValueError):
                gate.receipt(envelope, run, source)

    def test_signing_gate_requires_fresh_bound_nonregressing_evaluation(self):
        for key, value in (("bindingSha256", "0" * 64), ("policySha256", "0" * 64),
                           ("evaluatedThroughEpoch", 99000), ("regressionDetected", True),
                           ("nodes", {"homeportal": {"retiredCpis": 0, "retiredSamples": 0}})):
            envelope, run, _ = fixture()
            envelope["summary"]["latest"]["regressionEvidence"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                gate.receipt(envelope, run, SOURCE)

    def test_rebuild_or_deployment_mutation_cannot_replace_soaked_package(self):
        _, _, binding = fixture()
        name = binding["nodes"]["hp"]["packageFilename"]
        package = {"packageSha256": HASH, "artifactSha256": HASH,
                   "runtimeSha256": {unit: HASH for unit in gate.identity.UNITS}}
        gate.bind_packages(binding, {name: package})
        for field in ("packageSha256", "artifactSha256", "runtimeSha256"):
            modified = copy.deepcopy(package)
            modified[field] = ({unit: "c" * 64 for unit in gate.identity.UNITS}
                               if field == "runtimeSha256" else "c" * 64)
            with self.subTest(field=field), self.assertRaises(ValueError):
                gate.bind_packages(binding, {name: modified})

    def test_signatures_may_change_only_rpm_container_bytes(self):
        payload = {"packageSha256": HASH, "artifactSha256": HASH,
                   "runtimeSha256": {unit: HASH for unit in gate.identity.UNITS}}
        before = {"vectorwarp.rpm": payload, "vectorwarp.deb": copy.deepcopy(payload)}
        after = copy.deepcopy(before)
        after["vectorwarp.rpm"]["packageSha256"] = "c" * 64
        result = gate.signed_packages(before, after)
        self.assertEqual(result["vectorwarp.rpm"]["packageSha256"], HASH)
        for name, field in (("vectorwarp.rpm", "artifactSha256"),
                            ("vectorwarp.deb", "packageSha256")):
            changed = copy.deepcopy(after)
            changed[name][field] = "d" * 64
            with self.subTest(name=name, field=field), self.assertRaises(ValueError):
                gate.signed_packages(before, changed)

    def test_payload_measures_real_tree_and_rejects_dirty_source(self):
        def extract(package, root):
            release = root / "opt/vectorwarp/current"
            (release / "bin").mkdir(parents=True)
            (release / "bin/blah2").write_bytes(b"candidate processor")
            (release / ".vectorwarp-build").write_text(f"source_commit={SOURCE}\nsource_clean=true\n")
            (release / "BUILD-RECEIPT.json").write_text(json.dumps({"sourceCommit": SOURCE, "version": "1.2.3"}))
            node = root / "opt/vectorwarp/runtime/node/bin/node"
            node.parent.mkdir(parents=True)
            node.write_bytes(b"private runtime")
        with patch.object(gate, "unpack", side_effect=extract):
            first = gate.payload(Path("candidate.deb"), SOURCE, "1.2.3")
            self.assertRegex(first["artifactSha256"], r"^[0-9a-f]{64}$")
            with self.assertRaises(ValueError):
                gate.payload(Path("candidate.deb"), "c" * 40, "1.2.3")
        def dirty(package, root):
            extract(package, root)
            (root / "opt/vectorwarp/current/.vectorwarp-build").write_text(
                f"source_commit={SOURCE}\nsource_clean=false\n")
        with patch.object(gate, "unpack", side_effect=dirty), self.assertRaises(ValueError):
            gate.payload(Path("candidate.deb"), SOURCE, "1.2.3")

    def test_inventory_checks_actual_bytes_not_only_sidecar_claim(self):
        with tempfile.TemporaryDirectory(prefix="vectorwarp-promotion-test-") as directory:
            root = Path(directory)
            entries = []
            for number in range(11):
                name = f"vectorwarp_1.2.3_target{number}.deb"
                (root / name).write_bytes(b"candidate package")
                entries.append({"filename": name, "sha256": gate.identity.file_hash(root / name),
                                "size": (root / name).stat().st_size})
            manifest = {"source_commit": SOURCE, "version": "1.2.3", "packages": entries}
            with patch.object(gate, "payload", return_value={"artifactSha256": HASH}):
                self.assertEqual(len(gate.inventory(root, manifest, SOURCE, "1.2.3")), 11)
                (root / entries[0]["filename"]).write_bytes(b"same-SHA rebuild is not the same package")
                with self.assertRaises(ValueError):
                    gate.inventory(root, manifest, SOURCE, "1.2.3")


if __name__ == "__main__":
    unittest.main()
