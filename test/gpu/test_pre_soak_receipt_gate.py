"""Data-only gate regressions; these fixtures are NEVER physical receipts."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import pre_soak_receipt_gate as gate


def fixture():
    policy = {"schema": "vectorwarp-pre-soak-policy-v1", "status": "approved-before-measurement",
              "minimumEchoRetention": .8, "minimumRecordedNegativeSeconds": 10,
              "minimumEvaluatedCells": 100, "maximumObservedFalseAlarmFraction": .01,
              "maximumFalseTracksPerHour": 0, "maximumPfaUpperBound": .02, "minimumPfaConfidence": .95}
    values = {}
    def retain(value):
        body = json.dumps(value, sort_keys=True).encode()
        key = hashlib.sha256(body).hexdigest(); values[key] = value; return key
    raw = retain({"unitFixtureOnly": True})
    analysis = retain({"schema": "vectorwarp-recorded-negative-pfa-v1", "syntheticOnly": False,
        "methodReviewed": True, "method": "exact-binomial-independent-trials", "methodSourceSha256": "e" * 64,
        "independenceEvidenceSha256": raw, "estimate": 0., "upperBound": .01, "confidence": .95,
        "evaluatedCells": 100, "falseAlarmCells": 0})
    receipt = {"schema": "vectorwarp-pre-soak-radar-evidence-v1", "sourceSha": "a" * 40,
        "artifacts": {"blah2": "b" * 64}, "policySha256": "c" * 64, "passed": True,
        "syntheticOnly": False, "measurementContract": "paired-recorded-iq-and-adjudicated-whole-pass-negatives-v1",
        "echoMeasurements": [], "negativeMeasurements": []}
    for device in ("fp32", "fp64"):
        for mode in ("fixed-weights", "refit-weights"):
            for doppler in (-200, 200):
                receipt["echoMeasurements"].append(retain({"sourceKind": "recorded-iq", "exactAcquisition": True,
                    "recordingSha256": "d" * 64, "acquisitionSha256": "e" * 64, "configSha256": "f" * 64,
                    "executionSha256": raw, "productsSha256": raw, "deviceClass": device, "gpuExecuted": True,
                    "filterPolicy": mode, "dopplerHz": doppler, "unfilteredControl": [[0, 0]],
                    "checkpointSha256": "a"*64, "warmPrefixSha256": "b"*64,
                    "unfilteredInjected": [[1, 0]], "filteredControl": [[0, 0]], "filteredInjected": [[1, 0]],
                    "matchedDetections": 1, "matchedOperationalTracks": 1}))
        receipt["negativeMeasurements"].append(retain({"deviceClass": device,
            "sourceKind": "adjudicated-recorded-negative", "exactAcquisition": True,
            "wholePassDisjoint": True, "truthUsedForInference": False, "gpuExecuted": True,
            "adjudicationSha256": raw, "executionSha256": raw, "productsSha256": raw,
            "pfaAnalysisSha256": analysis, "windows": [{"recordingSha256": "d" * 64,
                "acquisitionSha256": "e" * 64, "captureEpoch": 7, "firstSample": 0, "endSample": 10000,
                "sampleRate": 1000, "evaluatedCells": 100, "falseAlarmCells": 0,
                "falseTracks": 0, "baselineFalseTracks": 0}]}))
    return receipt, policy, values


class PreSoakGateTest(unittest.TestCase):
    def check(self, receipt, policy, values):
        gate.validate(receipt, "a" * 40, {"blah2": "b" * 64}, policy, "c" * 64, values.__getitem__)

    def test_current_candidate_is_explicitly_not_pre_soak_qualified(self):
        with self.assertRaisesRegex(AssertionError, "not been approved"):
            gate.validate_policy(json.loads(gate.POLICY_PATH.read_text()))

    def test_optimization_cannot_remove_physical_acceptance_predicates(self):
        result = subprocess.run([sys.executable, "-O", "-c", "import pre_soak_receipt_gate"],
            cwd=Path(gate.__file__).parent, capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("assertions disabled", result.stderr)

    def test_retention_requires_actual_differential_results_and_both_refit_controls(self):
        receipt, policy, values = fixture(); self.check(receipt, policy, values)
        item = values[receipt["echoMeasurements"][0]]
        for field, value in (("filteredInjected", [[.1, 0]]), ("gpuExecuted", False),
                             ("matchedOperationalTracks", 0), ("unfilteredInjected", [[0, 0]])):
            bad = copy.deepcopy(values); bad[receipt["echoMeasurements"][0]][field] = value
            with self.assertRaises(AssertionError): self.check(receipt, policy, bad)
        bad = copy.deepcopy(values)
        bad[receipt["echoMeasurements"][0]]["filteredControl"] *= 2
        bad[receipt["echoMeasurements"][0]]["filteredInjected"] *= 2
        with self.assertRaisesRegex(AssertionError, "same observation shape"):
            self.check(receipt, policy, bad)
        receipt["echoMeasurements"].pop()
        with self.assertRaisesRegex(AssertionError, "signed-Doppler echo retention"):
            self.check(receipt, policy, values)

    def test_null_pfa_synthetic_exposure_and_double_counted_passes_fail(self):
        receipt, policy, values = fixture()
        negative = receipt["negativeMeasurements"][0]
        for field, value in (("sourceKind", "synthetic-null"), ("wholePassDisjoint", False),
                             ("pfaAnalysisSha256", None)):
            bad = copy.deepcopy(values); bad[negative][field] = value
            with self.assertRaises(AssertionError): self.check(receipt, policy, bad)
        bad = copy.deepcopy(values); bad[negative]["windows"] *= 2
        with self.assertRaisesRegex(AssertionError, "exposure reused"):
            self.check(receipt, policy, bad)
        bad = copy.deepcopy(values); bad[values[negative]["pfaAnalysisSha256"]]["upperBound"] = None
        with self.assertRaisesRegex(AssertionError, "Null/nonfinite measured PFA"):
            self.check(receipt, policy, bad)
        for field, value in (("endSample", 1), ("falseTracks", 1), ("falseAlarmCells", 10)):
            bad = copy.deepcopy(values); bad[negative]["windows"][0][field] = value
            with self.assertRaises(AssertionError): self.check(receipt, policy, bad)
        bad = copy.deepcopy(values); del bad[negative]["windows"][0]["captureEpoch"]
        with self.assertRaisesRegex(AssertionError, "exact epoch lineage"):
            self.check(receipt, policy, bad)

    def test_bounded_checkpoint_is_not_operational_pre_soak_qualification(self):
        receipt, policy, values = fixture()
        fixed = receipt["echoMeasurements"][0]
        for field, value in (("qualificationScope", "bounded-clutter-replay-v1"),
                             ("checkpointSha256", ""), ("warmPrefixSha256", "")):
            bad = copy.deepcopy(values);bad[fixed][field] = value
            with self.assertRaisesRegex(AssertionError, "operational|checkpoint"):
                self.check(receipt, policy, bad)

    def test_candidate_and_retained_evidence_are_bound(self):
        receipt, policy, values = fixture()
        for field, value in (("sourceSha", "f" * 40), ("artifacts", {}), ("policySha256", "f" * 64)):
            bad = copy.deepcopy(receipt); bad[field] = value
            with self.assertRaises(AssertionError): self.check(bad, policy, values)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); policy_path = root / "policy.json"
            policy_bytes = json.dumps(policy).encode(); policy_path.write_bytes(policy_bytes)
            receipt["policySha256"] = hashlib.sha256(policy_bytes).hexdigest()
            with mock.patch.object(gate, "POLICY_PATH", policy_path):
                with self.assertRaisesRegex(AssertionError, "Missing exact-candidate"):
                    gate.validate_directory(root, "a" * 40, {"blah2": "b" * 64})
                (root / "pre-soak-radar-evidence.json").write_text(json.dumps(receipt))
                with self.assertRaisesRegex(AssertionError, "Missing retained"):
                    gate.validate_directory(root, "a" * 40, {"blah2": "b" * 64})
                evidence = root / "pre-soak-evidence"; evidence.mkdir()
                (evidence / f'{receipt["echoMeasurements"][0]}.json').write_text('{"tampered":true}')
                with self.assertRaisesRegex(AssertionError, "digest differs"):
                    gate.validate_directory(root, "a" * 40, {"blah2": "b" * 64})

    def test_downloaded_fp64_subtree_requires_all_retained_measurements(self):
        receipt, policy, values = fixture()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy_path = root / "policy.json"
            policy_bytes = json.dumps(policy).encode(); policy_path.write_bytes(policy_bytes)
            receipt["policySha256"] = hashlib.sha256(policy_bytes).hexdigest()
            downloaded = root / "gpu-receipts"
            subtree = downloaded / "fp64" / "pre-soak"
            evidence = subtree / "pre-soak-evidence"
            evidence.mkdir(parents=True)
            (subtree / "pre-soak-radar-evidence.json").write_text(json.dumps(receipt))
            for digest, value in values.items():
                (evidence / f"{digest}.json").write_text(json.dumps(value, sort_keys=True))
            with mock.patch.object(gate, "POLICY_PATH", policy_path):
                gate.validate_directory(downloaded, "a" * 40, {"blah2": "b" * 64})
                shared = values[receipt["echoMeasurements"][0]]["executionSha256"]
                (evidence / f"{shared}.json").unlink()
                with self.assertRaisesRegex(AssertionError, "Missing retained"):
                    gate.validate_directory(downloaded, "a" * 40, {"blah2": "b" * 64})


if __name__ == "__main__":
    unittest.main()
