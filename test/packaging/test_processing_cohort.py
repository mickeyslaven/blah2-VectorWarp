#!/usr/bin/env python3
"""Regression checks for native processor/GPU cohort validation."""

from pathlib import Path
import json
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "script/check-processing-cohort.py"


class ProcessingCohortTest(unittest.TestCase):
    def artifact(self, resident: bool, wideband: bool, modules: tuple[bool, bool, bool]):
        temporary = tempfile.TemporaryDirectory(prefix="vectorwarp-processing-cohort-")
        root = Path(temporary.name)
        bindir = root / "bin"
        bindir.mkdir()
        report = json.dumps({"schema": 1, "residentGpu": resident, "widebandGpu": wideband})
        processor = bindir / "blah2"
        processor.write_text(f"#!/bin/sh\nprintf '%s\\n' '{report}'\n", encoding="utf-8")
        processor.chmod(0o755)
        for name, present in zip(("blah2-gpu-worker", "blah2-gpu-resident-vulkan.so",
                                  "blah2-gpu-wideband-vulkan.so"), modules):
            if present:
                (bindir / name).write_bytes(b"fixture")
        return temporary, root

    def check(self, root: Path, mode: str = "AUTO") -> subprocess.CompletedProcess[str]:
        return subprocess.run(["python3", str(CHECKER), "--artifact", str(root),
                               "--gpu-mode", mode], text=True, capture_output=True, check=False)

    def test_accepts_complete_gpu_and_cpu_fallback_cohorts(self):
        for resident, modules, mode in ((True, (True, True, True), "AUTO"),
                                         (True, (True, True, True), "ON"),
                                         (False, (False, False, False), "AUTO"),
                                         (False, (False, False, False), "OFF")):
            with self.subTest(resident=resident, mode=mode):
                temporary, root = self.artifact(resident, resident, modules)
                with temporary:
                    result = self.check(root, mode)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_stale_processor_partial_modules_and_one_sided_capability(self):
        cases = (
            (False, False, (True, True, True)),
            (True, True, (True, True, False)),
            (True, False, (True, True, True)),
        )
        for resident, wideband, modules in cases:
            with self.subTest(resident=resident, wideband=wideband, modules=modules):
                temporary, root = self.artifact(resident, wideband, modules)
                with temporary:
                    self.assertNotEqual(self.check(root).returncode, 0)

    def test_gpu_on_rejects_cpu_fallback(self):
        temporary, root = self.artifact(False, False, (False, False, False))
        with temporary:
            self.assertNotEqual(self.check(root, "ON").returncode, 0)


if __name__ == "__main__":
    unittest.main()
