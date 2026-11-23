"""Regression checks for the bounded prebuild source gate."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("prebuild_acceptance", ROOT / "script/prebuild_acceptance.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class PrebuildAcceptanceTests(unittest.TestCase):
    def invoke(self, paths, targets=(), tests=()):
        calls = []
        def command(line, root):
            calls.append(line)
            return {"command": list(line), "returncode": 0, "stdout": "", "stderr": ""}
        with tempfile.TemporaryDirectory() as temporary:
            receipt = Path(temporary) / "receipt.json"
            code = GATE.main(["--build-dir", temporary, "--receipt", str(receipt),
                              "--skip-target-build", *(item for target in targets
                              for item in ("--native-target", target)), *(item for test in tests
                              for item in ("--native-test", test))], ROOT, command,
                             lambda root: paths)
            return code, calls, json.loads(receipt.read_text())

    def test_native_changes_fail_closed_without_a_target(self):
        code, calls, receipt = self.invoke(["src/process/example.cpp"])
        self.assertEqual(code, 1)
        self.assertEqual(calls, [])
        self.assertIn("native changes require", receipt["error"])

    def test_native_changes_require_a_focused_test(self):
        code, calls, receipt = self.invoke(["src/process/example.cpp"], ("blah2",))
        self.assertEqual(code, 1)
        self.assertEqual(calls, [])
        self.assertIn("--native-test", receipt["error"])

    def test_targeted_gate_records_fast_contract_commands(self):
        code, calls, receipt = self.invoke(["README.md"], ("blah2",), ("testTracker",))
        self.assertEqual(code, 0)
        self.assertEqual(calls[:2], [("git", "diff", "--check", "--cached"),
                                     ("git", "diff", "--check")])
        self.assertEqual([row["name"] for row in receipt["commands"]],
                         ["diff_check_cached", "diff_check", "prebuild_contract", "repository_budget",
                          "canonical_architecture", "ci_contract", "api_contract", "effective_radar_plan", "frontend_plan", "ui_contract"])
        self.assertEqual(receipt["outcome"], "passed")

    def test_frontend_changes_require_native_api_parity(self):
        calls = []
        def command(line, root):
            calls.append(line)
            return {"command": list(line), "returncode": 0, "stdout": "", "stderr": ""}
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text("CMAKE_COMPILE_WARNING_AS_ERROR:BOOL=ON\n")
            code = GATE.main(["--build-dir", temporary, "--receipt", str(build / "receipt.json"),
                              "--native-target", "blah2", "--native-test", "testTracker"],
                             ROOT, command, lambda root: ["src/blah2.cpp"])
        self.assertEqual(code, 0)
        self.assertTrue(any(line[-1] == "testFrontendPlan" for line in calls))
        self.assertTrue(any("^frontendPlanParity$" in line for line in calls))

    def test_empty_target_is_not_a_declaration(self):
        code, calls, receipt = self.invoke(["cmake/NewOwner.cmake"], (" ",), (" ",))
        self.assertEqual(code, 1)
        self.assertEqual(calls, [])
        self.assertIn("native changes require", receipt["error"])

    def test_strict_native_warning_blocks_tests_and_preserves_diagnostic(self):
        calls = []
        diagnostic = "error: loop variable creates a copy [-Werror=range-loop-construct]"
        def command(line, root):
            calls.append(line)
            compiling = line[:2] == ("cmake", "--build")
            return {"command": list(line), "returncode": int(compiling),
                    "stdout": "", "stderr": diagnostic if compiling else ""}
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text("CMAKE_COMPILE_WARNING_AS_ERROR:BOOL=ON\n")
            receipt = build / "receipt.json"
            code = GATE.main(["--build-dir", temporary, "--receipt", str(receipt),
                              "--native-target", "testCfarEfficiency",
                              "--native-test", "cfarExactOutput"], ROOT, command,
                             lambda root: ["test/unit/process/detection/TestCfarEfficiency.cpp"])
            result = json.loads(receipt.read_text())
        self.assertEqual(code, 1)
        self.assertEqual(result["outcome"], "failed")
        self.assertEqual(result["commands"][-1]["name"], "native_target:testCfarEfficiency")
        self.assertEqual(result["commands"][-1]["stderr"], diagnostic)
        self.assertFalse(any(line[0] == "ctest" for line in calls))

    def test_untracked_native_files_are_changed_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(("git", "init", "-q"), cwd=root, check=True)
            (root / "src").mkdir()
            (root / "src" / "new.cpp").write_text("int value;\n")
            self.assertEqual(GATE.changed_paths(root), ["src/new.cpp"])


if __name__ == "__main__":
    unittest.main()
