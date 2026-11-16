#!/usr/bin/env python3
"""Cheap negative controls for the physical acceptance harness; no GPU/build."""
import copy
import importlib.util
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
    value = {"schema": "vectorwarp-gpu-product-parity-v1", "passed": True,
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
        for temporal, modes in ((False, ("cpu", "gpu", "gpu-resident")), (True, ("cpu", "gpu"))):
            for mode in modes:
                case = {"receiver": receiver, "mode": mode, "temporal": temporal}
                if mode == "gpu-resident" and feature_class == "fp32":
                    case.update(refused=True, reason="shaderFloat64=0")
                else:
                    case.update(processorGeneration=f"{receiver}-{mode}-{temporal}", processorPid=123,
                                configSha256="f" * 64, productsSha256="e" * 64)
                    case["execution"] = [{key: frame[key] for key in
                        ("nCpi", "captureEpoch", "captureFirstEpochSample", "captureEndEpochSample")} |
                        {"stages": {key: frame[key] for key in ("acceleration",
                            "widebandAcceleration" if temporal else "clutterAcceleration")}}
                        for frame in frames(mode, temporal)]
                value["cases"].append(case)
    return value


class ProductParityContractTest(unittest.TestCase):
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
        verify.validate_pair(pair, "a" * 40)
        for mutation in (lambda p: p.pop(), lambda p: p[1]["cases"].pop(),
            lambda p: p[1]["artifacts"].update(blah2="f" * 64),
            lambda p: p[0]["device"].update(shaderFloat64=True),
            lambda p: p[0].update(sourceSha="f" * 40),
            lambda p: p[0]["cases"][2].update(reason="resident-module-not-built"),
            lambda p: p[1]["cases"][1]["execution"][-1]["stages"]["acceleration"].update(cpuExecuted=True)):
            broken = copy.deepcopy(pair)
            mutation(broken)
            with self.assertRaises(AssertionError): verify.validate_pair(broken, "a" * 40)


if __name__ == "__main__":
    unittest.main()
