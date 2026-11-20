#!/usr/bin/env python3
"""Configure-only checks for the shared GPU/benchmark processing graph."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
VKFFT_ROOT = os.environ.get("VKFFT_ROOT", "")
TOOLCHAIN = os.environ.get("CMAKE_TOOLCHAIN_FILE", "")
VCPKG_INSTALLED = os.environ.get("VCPKG_INSTALLED_DIR", "")
APPLICATION = False
PREFIX_PATH = ""
PRODUCTS = {"blah2RadarData", "blah2Detection", "blah2Tracking", "blah2Fusion"}
CANONICAL = {
    "blah2FixedExecutor",
    "blah2ProductionRawIq",
    "blah2Channelizer",
    "blah2MultiIlluminatorRadar",
    "blah2GpuProcess",
    "blah2GpuResidentProcess",
    "blah2GpuWidebandProcess",
    "blah2RadarDsp", "blah2Acceleration",
} | PRODUCTS
NUMERICAL_OWNERS = {
    **dict.fromkeys(("data/Map.cpp", "data/Detection.cpp", "data/Track.cpp"), "blah2RadarData"),
    **dict.fromkeys((f"process/detection/{name}.cpp" for name in
                    ("CfarDetector1D", "Centroid", "Interpolate")), "blah2Detection"),
    "process/tracker/Tracker.cpp": "blah2Tracking",
    **dict.fromkeys((f"process/fusion/{name}.cpp" for name in
                    ("Noncoherent", "CoherentBeamBank")), "blah2Fusion"),
    **dict.fromkeys(("process/ambiguity/Ambiguity.cpp", "process/clutter/WienerHopf.cpp",
                    "process/conditioning/ArrayReferenceSynthesizer.cpp",
                    "process/conditioning/AtscPilotProjector.cpp", "process/meta/HammingNumber.cpp",
                    "process/spectrum/SpectrumAnalyser.cpp"), "blah2RadarDsp"),
    "process/ambiguity/Acceleration.cpp": "blah2Acceleration",
}


class StandaloneCmakeTest(unittest.TestCase):
    def configure(self, entry, gpu, advanced="AUTO"):
        with tempfile.TemporaryDirectory(prefix="vectorwarp-standalone-cmake-") as path:
            build = Path(path)
            query = build / ".cmake/api/v1/query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            command = [
                "cmake", "-S", str(ROOT / entry), "-B", str(build),
                f"-DBLAH2_GPU={gpu}", f"-DBLAH2_GPU_ADVANCED={advanced}",
                "-DBUILD_TESTING=ON", "-DBLAH2_TEST_PHYSICAL_GPU=OFF",
            ]
            if gpu == "ON":
                command.append(f"-DVKFFT_ROOT={VKFFT_ROOT}")
            if TOOLCHAIN:
                command.append(f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}")
            if VCPKG_INSTALLED:
                command.append(f"-DVCPKG_INSTALLED_DIR={VCPKG_INSTALLED}")
            if PREFIX_PATH:
                command.append(f"-DCMAKE_PREFIX_PATH={PREFIX_PATH}")
            if entry == "bench":
                # Configuration only: this source tree supplies baseline file
                # names. Never run or publish this as an upstream comparison.
                command.append(f"-DUPSTREAM_ROOT={ROOT}")
            if entry == ".":
                command += ["-DBLAH2_KRAKEN_ONLY=ON", "-DBLAH2_BUILD_OFFLINE_TOOLS=ON",
                            "-DBLAH2_BUILD_BENCHMARKS=ON", f"-DBLAH2_OUTPUT_DIR={build / 'bin'}"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            reply = build / ".cmake/api/v1/reply"
            index = json.loads(next(reply.glob("index-*.json")).read_text())
            model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
            targets = {
                item["name"]: json.loads((reply / item["jsonFile"]).read_text())
                for item in model["configurations"][0]["targets"]
            }
            names = {target["id"]: name for name, target in targets.items()}

            def dependencies(name):
                return {names[item["id"]] for item in targets[name].get("dependencies", [])}

            for source, owner in NUMERICAL_OWNERS.items():
                if owner not in targets:
                    continue
                consumers = [name for name, target in targets.items() if name != "bench-upstream"
                             and any((ROOT / entry / item["path"]).resolve() == ROOT / "src" / source
                                     for item in target.get("sources", []))]
                self.assertEqual(consumers, [owner], f"{source} has parallel compilation owners")
            if entry in ("test/json", "test/detection"):
                self.assertTrue(PRODUCTS <= targets.keys())
                self.assertNotIn("blah2RadarDsp", targets)
                self.assertNotIn("blah2GpuProcess", targets)
                return
            if entry == "test/bearing":
                self.assertIn("blah2RadarDsp", dependencies("testBearingPhaseProjection"))
                self.assertNotIn("blah2GpuProcess", targets)
                return
            self.assertTrue(CANONICAL <= targets.keys(), CANONICAL - targets.keys())
            if entry == ".":
                self.assertNotIn("blah2IqData", targets, "Production must borrow the shared capture pool")
                self.assertIn("blah2CaptureCore", dependencies("blah2RadarDsp"))
                self.assertIn("blah2FixedExecutor", dependencies("blah2DirectPathCoherentReplay"))
                self.assertIn("blah2FixedExecutor", dependencies("weakEvidenceBootstrap"))
            else:
                self.assertIn("blah2IqData", dependencies("blah2RadarDsp"))
            self.assertTrue({"blah2RadarDsp", "blah2Detection", "blah2Tracking", "blah2Fusion"}
                            <= dependencies("blah2MultiIlluminatorRadar"))
            self.assertIn("blah2FixedExecutor", dependencies("blah2GpuWidebandProcess"))
            self.assertNotIn("blah2Channelizer", dependencies("blah2MultiIlluminatorRadar"))
            self.assertTrue({"blah2FixedExecutor", "blah2ProductionRawIq", "blah2GpuProcess",
                             "blah2GpuResidentProcess"} <= dependencies("blah2MultiIlluminatorRadar"))
            if gpu == "OFF":
                self.assertNotIn("blah2GpuVulkan", targets)
                self.assertNotIn("blah2GpuWorker", targets)
            else:
                self.assertIn("blah2GpuVulkan", targets)
                self.assertIn("blah2GpuWorker", targets)
                if advanced != "OFF":
                    self.assertIn("testGpuWidebandEndToEnd", targets)
                    self.assertIn("blah2MultiIlluminatorRadar",
                                  dependencies("testGpuWidebandEndToEnd"))
                    self.assertIn("blah2Channelizer", dependencies("testGpuWidebandEndToEnd"))
                    if os.uname().sysname == "Linux":
                        self.assertIn("blah2GpuResidentVulkan", targets)
                else:
                    self.assertNotIn("blah2GpuWidebandVulkan", targets)
                    self.assertNotIn("blah2GpuResidentVulkan", targets)
            for target in targets.values():
                for fragment in target.get("link", {}).get("commandFragments", []):
                    self.assertNotIn("-lblah2", fragment["fragment"],
                                     "An unresolved canonical target became an external library")
            if entry == "test/gpu":
                self.assertIn("testGpuRetiredWork", targets)
                registered = subprocess.run(
                    ["ctest", "--test-dir", str(build), "-N"],
                    capture_output=True, text=True, timeout=10)
                self.assertIn("gpuRetiredWorkRejected", registered.stdout)
            elif entry == "bench":
                self.assertTrue({"bench-fast", "bench-upstream"} <= targets.keys())
                self.assertIn("blah2MultiIlluminatorRadar", dependencies("bench-fast"))
                self.assertFalse(CANONICAL & dependencies("bench-upstream"))

    def test_lightweight_products(self):
        for entry in ("test/json", "test/detection"):
            with self.subTest(entry=entry):
                self.configure(entry, "OFF")

    def test_application_graphs(self):
        if not APPLICATION:
            self.skipTest("Supply --application when application dependencies are installed")
        self.configure("test/bearing", "OFF")
        self.configure(".", "OFF")
        if VKFFT_ROOT:
            self.configure(".", "ON")

    def test_cpu_graphs(self):
        for entry in ("test/gpu", "bench"):
            with self.subTest(entry=entry):
                self.configure(entry, "OFF")

    def test_gpu_graphs(self):
        if not VKFFT_ROOT:
            self.skipTest("Supply --vkfft-root or VKFFT_ROOT for GPU ON configurations")
        for entry in ("test/gpu", "bench"):
            for advanced in ("AUTO", "OFF"):
                with self.subTest(entry=entry, advanced=advanced):
                    self.configure(entry, "ON", advanced)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vkfft-root", default=VKFFT_ROOT)
    parser.add_argument("--toolchain", default=TOOLCHAIN)
    parser.add_argument("--vcpkg-installed-dir", default=VCPKG_INSTALLED)
    parser.add_argument("--application", action="store_true")
    parser.add_argument("--prefix-path", default=PREFIX_PATH)
    args, remaining = parser.parse_known_args()
    VKFFT_ROOT = args.vkfft_root
    TOOLCHAIN = args.toolchain
    VCPKG_INSTALLED = args.vcpkg_installed_dir
    APPLICATION = args.application
    PREFIX_PATH = args.prefix_path
    unittest.main(argv=[__file__, *remaining])
