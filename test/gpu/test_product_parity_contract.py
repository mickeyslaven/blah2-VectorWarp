#!/usr/bin/env python3
"""Cheap negative controls for the physical acceptance harness; no GPU/build."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "test/gpu" / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


parity = module("product_parity_test")
verify = module("verify_qualification_receipts")
DEVICE = {"id": "1:2:0", "name": "fixture GPU", "shaderFloat64": False}


def frames(mode, temporal=False):
    result = []
    for n in range(1, 13):
        value = {"nCpi": n, "timestamp": n, "captureSamplePositionAvailable": True,
                 "captureEpoch": 7, "captureFirstEpochSample": n * 40000,
                 "captureEndEpochSample": (n + 1) * 40000}
        for stage in ("acceleration", "widebandAcceleration" if temporal else "clutterAcceleration"):
            value[stage] = {"active": "cpu" if mode == "cpu" else
                "vulkan-wideband" if stage == "widebandAcceleration" else
                "vulkan-resident" if mode == "gpu-resident" else "vulkan",
                "state": "active" if stage == "widebandAcceleration" else "ready",
                "gpuExecuted": mode != "cpu", "cpuExecuted": mode == "cpu", "device": DEVICE["name"]}
        result.append(value)
    return result


def receipt(feature_class):
    value = {"schema": "vectorwarp-gpu-product-parity-v2", "passed": True,
        "qualificationScope": "two-channel-products", "featureContractSha256": verify.CONTRACT_SHA256,
        "sourceSha": "a" * 40, "system": "Linux", "deviceClass": feature_class,
        "device": {**DEVICE, "shaderFloat64": feature_class == "fp64"},
        "nativeCapabilities": {"schema": 1, "portableGpu": True, "residentGpu": True, "widebandGpu": True},
        "receiverCapabilities": {"schema": 1, "hardwareProbed": False,
            "receivers": [{"receiver": name, "compiled": True, "moduleLoadable": True} for name in parity.RECEIVERS]},
        "artifacts": {name: "b" * 64 for name in ("blah2", "blah2-gpu-worker",
            "blah2-gpu-vulkan.so", "blah2-gpu-wideband-vulkan.so", "blah2-gpu-resident-vulkan.so",
            "blah2-receiver-usrp.so", "blah2-receiver-hackrf.so")},
        "tolerance": {"absolute": parity.ABS_TOLERANCE, "relative": parity.REL_TOLERANCE},
        "fixtures": {name: {"iq": "c" * 64, "lineage": "d" * 64} for name in ("False", "True")}, "cases": []}
    for receiver in parity.RECEIVERS:
        for temporal, modes in ((False, ("cpu", "gpu", "gpu-resident")), (True, ("cpu", "gpu", "gpu-resident"))):
            for mode in modes:
                case = {"receiver": receiver, "mode": mode, "temporal": temporal}
                if mode == "gpu-resident" and feature_class == "fp32":
                    case.update(refused=True, reason="shaderFloat64=0")
                else:
                    case.update(processorGeneration=f"{receiver}-{mode}-{temporal}", processorPid=123,
                                configSha256="f" * 64, productsSha256="e" * 64, radarPlanSha256="a" * 64)
                    case["execution"] = [{key: frame[key] for key in
                        ("nCpi", "captureEpoch", "captureFirstEpochSample", "captureEndEpochSample")} |
                        {"stages": {key: frame[key] for key in ("acceleration",
                            "widebandAcceleration" if temporal else "clutterAcceleration")}}
                        for frame in frames(mode, temporal)]
                value["cases"].append(case)
    return value


def feature_receipt(baseline):
    """Schema fixture only: never written as a physical qualification receipt."""
    value = {key: copy.deepcopy(baseline[key]) for key in
             ("sourceSha", "system", "deviceClass", "device", "artifacts", "featureContractSha256")}
    value.update(schema="vectorwarp-unified-gpu-features-v1", passed=True, scenarios=[])
    for name, contract in verify.required_scenarios(value["deviceClass"]).items():
        scenario = {"id": name, "settings": copy.deepcopy(contract["settings"]),
                    "passed": True, "observedFrames": 32, "processorGeneration": name}
        scenario.update({field: "c" * 64 for field in ("configSha256", "fixtureSha256", "lineageSha256",
            "executionSha256", "productsSha256", "oracleSha256")})
        scenario["checks"] = {check: {"passed": True, "evidenceSha256": "d" * 64} for check in
                              set(verify.REQUIREMENTS["commonChecks"]) | set(contract["checks"])}
        value["scenarios"].append(scenario)
    return value


class ProductParityContractTest(unittest.TestCase):
    def test_receipt_refuses_missing_or_backend_changed_native_plan(self):
        for change in (None, "b" * 64):
            value = receipt("fp64")
            value["cases"][1]["radarPlanSha256"] = change
            with self.assertRaisesRegex(AssertionError, "plan identity"):
                verify.validate(value, "a" * 40, "fp64")

    def test_cpu_cohort_needs_no_gpu_artifact_but_cannot_claim_gpu_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "blah2"
            binary.write_bytes(b"CPU-only processor fixture")
            binding = parity.artifacts.prepare(binary, False)
            self.assertEqual(set(binding["artifacts"]), {"processor"})
            result = parity.artifacts.validate(binding, "", 123, require_worker=False)
            self.assertTrue(result["passed"])
            self.assertEqual(result["loaded"], [])
            with self.assertRaisesRegex(ValueError, "missing GPU worker"):
                parity.artifacts.validate(binding, "", 123, require_worker=True)

    def test_release_requires_complete_enabled_gpu_cohort(self):
        valid = receipt("fp64")
        verify.validate(valid, "a" * 40, "fp64")
        # Missing/partial artifacts and GPU-OFF cannot qualify a GPU release.
        for name in ("blah2", "blah2-gpu-worker", "blah2-gpu-vulkan.so",
                     "blah2-gpu-resident-vulkan.so", "blah2-gpu-wideband-vulkan.so"):
            with self.subTest(missing=name):
                broken = copy.deepcopy(valid)
                del broken["artifacts"][name]
                with self.assertRaises(AssertionError):
                    verify.validate(broken, "a" * 40, "fp64")
        for capability in ("portableGpu", "residentGpu", "widebandGpu"):
            for value in (False, None, 1):
                with self.subTest(capability=capability, value=value):
                    broken = copy.deepcopy(valid)
                    broken["nativeCapabilities"][capability] = value
                    with self.assertRaisesRegex(AssertionError, "required native stage not compiled"):
                        verify.validate(broken, "a" * 40, "fp64")

    def test_receiver_identity_cannot_change_the_normalized_processing_graph(self):
        for temporal in (False, True):
            baseline = None
            for receiver in parity.RECEIVERS:
                config = parity.replay.config(receiver, 2, Path('/fixture.blah2iq'), 3000, {}, temporal_lane=temporal)
                config['process'].setdefault('reference_synthesis', {'mode': 'dedicated', 'channels': [0]})
                if baseline is not None:
                    self.assertEqual(config['process'], baseline, receiver)
                baseline = config['process']
        processor = (ROOT / 'src/blah2.cpp').read_text()
        # Only the RSPduo acquisition queue asks for its native paired storage;
        # the normalized processor must not select a receiver-specific DSP.
        receiver_branches = [line.strip() for line in processor.splitlines() if
                            __import__('re').search(r'type\s*(?:==|!=)\s*"(?:Kraken|Usrp|HackRF|RspDuo)"', line)]
        self.assertEqual(receiver_branches, ['const bool pairedQueueRequested = !state && type == "RspDuo" &&'])
        contract = (ROOT / 'src/process/pipeline/RadarPipeline.h').read_text()
        for receiver in parity.RECEIVERS:
            self.assertNotIn('"' + receiver + '"', contract)
        for adapter in ('resolveRadarPlan(', 'radarPlan.residentDetector(', 'radarPlan.residentTracker('):
            self.assertIn(adapter, processor)
        # Recovery is configured by the canonical product adapter, not by
        # duplicate tracker orchestration in the ingress/publication owner.
        self.assertIn('result.configureRecovery(config_.recovery);', contract)
        source = (ROOT / 'test/gpu/product_parity_test.py').read_text()
        self.assertIn('compare(identity_baseline, cpu["products"], f"{receiver}.receiverIdentity", exact=True)', source)

    def test_temporal_fixture_enables_cancellation_and_preserves_the_target(self):
        for receiver in parity.RECEIVERS:
            config = parity.replay.config(receiver, 2, Path('/fixture.blah2iq'), 3000, {}, temporal_lane=True)
            clutter = config['process']['clutter']
            self.assertIs(clutter['enable'], True)
            self.assertLessEqual(clutter['delayMin'], 0)
            self.assertGreater(clutter['delayMax'], 0)
            self.assertLess(clutter['delayMax'], 8)
        with tempfile.TemporaryDirectory(prefix='vectorwarp-corpus-contract-') as temporary:
            root = Path(temporary)
            baseline, cluttered, repeated = (root / name for name in ('baseline', 'cluttered', 'repeated'))
            for path, amplitude in ((baseline, 0), (cluttered, .25), (repeated, .25)):
                parity.replay.write_target_blah2iq(path, 2000, 204640000,
                    frame_samples=40, delay_samples=8, stationary_clutter=amplitude)
            self.assertEqual(cluttered.read_bytes(), repeated.read_bytes())
            original, observed = baseline.read_bytes(), cluttered.read_bytes()
            samples = 240
            for index in range(samples):
                reference = complex(*struct.unpack_from('<ff', original, 80 + index * 8))
                before = complex(*struct.unpack_from('<ff', original, 80 + (samples + index) * 8))
                after = complex(*struct.unpack_from('<ff', observed, 80 + (samples + index) * 8))
                self.assertLess(abs(after - before - .25 * reference), 1e-7)

    def test_python_optimization_cannot_disable_acceptance(self):
        for script in ("product_parity_test.py", "verify_qualification_receipts.py"):
            result = subprocess.run([sys.executable, "-O", str(ROOT / "test/gpu" / script), "--help"],
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("assertions disabled", result.stderr)

    def test_every_product_and_lineage_field_matters(self):
        expected = {"map": [[1.2]], "detections": {"delay": [1.2]},
                    "tracks": {"data": [{"id": "A", "n": 2}]},
                    "integration": {"support_sequences": [2, 1]}, "first_input_sample": 10000000}
        parity.compare(expected, copy.deepcopy(expected))
        for key, replacement in (("map", [[1.3]]), ("detections", {"delay": []}),
            ("tracks", {"data": [{"id": "B", "n": 2}]}),
            ("integration", {"support_sequences": [2, 0]}), ("first_input_sample", 10000001)):
            changed = copy.deepcopy(expected)
            changed[key] = replacement
            with self.subTest(key=key), self.assertRaises(AssertionError):
                parity.compare(expected, changed)

    def test_float_tolerance_is_bounded_and_nonfinite_is_rejected(self):
        parity.compare(1.0, 1.01)
        for value in (1.1, float("nan"), float("inf"), True):
            with self.assertRaises(AssertionError): parity.compare(1.0, value)
        with self.assertRaises(AssertionError): parity.compare(1.0, 1.01, exact=True)

    def test_device_classes_are_required_not_optional(self):
        report = {"version": 1, "available": True, "qualification": "not-run", "devices": [DEVICE]}
        self.assertEqual(parity.select_device(report, "fp32"), DEVICE)
        for kind, selector in (("fp64", None), ("fp32", "wrong")):
            with self.assertRaises(AssertionError): parity.select_device(report, kind, selector)

    def test_execution_requires_every_frame_and_no_steady_cpu_oracle(self):
        parity.assert_execution(frames("gpu"), [], "gpu", False, DEVICE)
        for mutation in (lambda f: f.pop(), lambda f: f[1].update(nCpi=1),
            lambda f: f[0].update(captureSamplePositionAvailable=False),
            lambda f: f[-1]["acceleration"].update(cpuExecuted=True),
            lambda f: f[-1]["clutterAcceleration"].update(gpuExecuted=False),
            lambda f: f[-1]["acceleration"].update(device="other")):
            broken = frames("gpu")
            mutation(broken)
            with self.assertRaises(AssertionError):
                parity.assert_execution(broken, [], "gpu", False, DEVICE)

    def test_pair_requires_same_artifact_and_complete_matrix(self):
        pair = [receipt("fp32"), receipt("fp64")]
        features = [feature_receipt(item) for item in pair]
        verify.validate_pair(pair, "a" * 40, features)
        for mutation in (lambda p: p.pop(), lambda p: p[1]["cases"].pop(),
            lambda p: p[1]["artifacts"].update(blah2="f" * 64),
            lambda p: p[0]["device"].update(shaderFloat64=True),
            lambda p: p[0].update(sourceSha="f" * 40),
            lambda p: p[0]["cases"][2].update(reason="resident-module-not-built"),
            lambda p: p[1]["cases"][1]["execution"][-1]["stages"]["acceleration"].update(cpuExecuted=True)):
            broken = copy.deepcopy(pair)
            mutation(broken)
            with self.assertRaises(AssertionError): verify.validate_pair(broken, "a" * 40, features)

    def test_baseline_alone_fails_closed_with_exact_missing_scenarios(self):
        pair = [receipt("fp32"), receipt("fp64")]
        with self.assertRaisesRegex(AssertionError, "Missing unified feature receipts") as failure:
            verify.validate_pair(pair, "a" * 40)
        for scenario in verify.REQUIREMENTS["scenarios"]:
            self.assertIn(scenario["id"], str(failure.exception))
        self.assertEqual(len(pair[0]["cases"]), 24)
        self.assertEqual(len(pair[1]["cases"]), 24)
        for item in pair:
            for receiver in parity.RECEIVERS:
                self.assertTrue(any(case["receiver"] == receiver and case["mode"] == "gpu-resident"
                                    and case["temporal"] is True for case in item["cases"]))

    def test_feature_receipts_require_bound_measured_evidence_and_all_checks(self):
        pair = [receipt("fp32"), receipt("fp64")]
        features = [feature_receipt(item) for item in pair]
        for mutation in (
            lambda f: f.pop(), lambda f: f.append(copy.deepcopy(f[0])),
            lambda f: f[1].update(sourceSha="f" * 40),
            lambda f: f[1].update(featureContractSha256="f" * 64),
            lambda f: f[1]["device"].update(id="other"),
            lambda f: f[1]["artifacts"].update(blah2="f" * 64),
            lambda f: f[1]["scenarios"].pop(),
            lambda f: f[1]["scenarios"].append(copy.deepcopy(f[1]["scenarios"][0])),
            lambda f: f[0]["scenarios"][0].update(observedFrames=4),
            lambda f: f[1]["scenarios"][0].update(settings={}),
            lambda f: f[1]["scenarios"][0].update(productsSha256=""),
            lambda f: f[1]["scenarios"][0]["checks"].pop("numerical-oracle"),
            lambda f: f[1]["scenarios"][0]["checks"]["numerical-oracle"].update(passed=False),
            lambda f: f[1]["scenarios"][0]["checks"]["numerical-oracle"].update(evidenceSha256=""),
        ):
            broken = copy.deepcopy(features)
            mutation(broken)
            with self.assertRaises(AssertionError): verify.validate_pair(pair, "a" * 40, broken)

    def test_feature_evidence_files_are_retained_and_hash_checked(self):
        value = feature_receipt(receipt("fp64"))
        body = json.dumps({"fixture": "unit-test-only"}).encode()
        digest = hashlib.sha256(body).hexdigest()
        for scenario in value["scenarios"]:
            for field in ("executionSha256", "productsSha256", "oracleSha256"):
                scenario[field] = digest
            for check in scenario["checks"].values():
                check["evidenceSha256"] = digest
        with tempfile.TemporaryDirectory(prefix="feature-receipt-contract-") as temporary:
            root = Path(temporary)
            path = root / "gpu-unified-features-fp64.json"
            with self.assertRaisesRegex(AssertionError, "missing retained"):
                verify.validate_evidence_files(value, path)
            evidence = root / "gpu-feature-evidence" / f"{digest}.json"
            evidence.parent.mkdir()
            evidence.write_bytes(body)
            verify.validate_evidence_files(value, path)
            evidence.write_bytes(b"{}")
            with self.assertRaisesRegex(AssertionError, "evidence changed"):
                verify.validate_evidence_files(value, path)

    def test_requirements_cli_is_descriptive_and_does_not_emit_pass(self):
        result = subprocess.run([sys.executable, str(ROOT / "test/gpu/verify_qualification_receipts.py"),
                                 "--requirements"], capture_output=True, text=True, check=True, timeout=5)
        definitions = json.loads(result.stdout)
        self.assertEqual(definitions["featureContractSha256"], verify.CONTRACT_SHA256)
        self.assertEqual(definitions["scenarios"], verify.REQUIREMENTS["scenarios"])
        self.assertNotIn("passed", definitions)


if __name__ == "__main__":
    unittest.main()
