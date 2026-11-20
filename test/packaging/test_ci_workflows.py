#!/usr/bin/env python3
"""Offline workflow routing, publication, YAML and shell contract checks."""
import ast
import fnmatch
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github/workflows"


class WorkflowLoader(yaml.BaseLoader):
    # BaseLoader preserves GitHub's YAML 1.2 `on` key and boolean spellings.
    def construct_mapping(self, node, deep=False):
        result = {}
        for key_node, value_node in node.value:
            key = self.construct_object(key_node, deep=deep)
            if key in result:
                raise ValueError(f"duplicate YAML key: {key}")
            result[key] = self.construct_object(value_node, deep=deep)
        return result


def workflow(name):
    return yaml.load((WORKFLOWS / name).read_text(), Loader=WorkflowLoader)


def condition(expression, event, ref="refs/heads/main", **inputs):
    """Evaluate the small comparison-only subset used by publication guards."""
    values = {
        "github.event_name": event, "github.ref": ref,
        "github.ref_type": "tag" if ref.startswith("refs/tags/") else "branch",
        "github.repository": "mickeyslaven/blah2-VectorWarp",
        "inputs.publish": False, "inputs.require_gpu_receipts": False,
        "needs.validate.outputs.tagged_release": "true",
        **{f"inputs.{key}": value for key, value in inputs.items()},
    }
    expression = expression.removeprefix("${{").removesuffix("}}").strip()
    expression = re.sub(r"(?:github|inputs|needs)(?:\.[A-Za-z0-9_-]+)+",
                        lambda match: repr(values[match.group()]), expression)
    expression = expression.replace("&&", " and ").replace("||", " or ")
    tree = ast.parse("(" + expression + ")", mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.Compare, ast.Constant, ast.And,
               ast.Or, ast.Eq, ast.NotEq, ast.Call, ast.Name, ast.Load)
    if any(not isinstance(node, allowed) for node in ast.walk(tree)):
        raise ValueError("unsupported workflow condition")
    return bool(eval(compile(tree, "<workflow condition>", "eval"),
                     {"__builtins__": {}, "startsWith": str.startswith}))


class WorkflowContracts(unittest.TestCase):
    def test_all_yaml_actions_reusable_jobs_and_shell_blocks(self):
        reusable_keys = {"name", "uses", "with", "secrets", "strategy", "needs",
                         "if", "concurrency", "permissions"}
        for path in sorted(WORKFLOWS.glob("*.yml")):
            with self.subTest(workflow=path.name):
                document = workflow(path.name)
                self.assertIn("on", document)
                self.assertEqual(document["permissions"].get("contents"), "read")
                self.assertNotIn("pull_request_target", document["on"])
                for name, job in document["jobs"].items():
                    if "uses" in job:
                        self.assertLessEqual(job.keys(), reusable_keys)
                        target = job["uses"]
                        if target.startswith("./"):
                            self.assertTrue((ROOT / target).is_file())
                            self.assertIn("workflow_call", workflow(Path(target).name)["on"])
                    for step in job.get("steps", []):
                        if "uses" in step:
                            self.assertRegex(step["uses"], r"^[\w./-]+@[0-9a-f]{40}$")
                        if "run" in step:
                            result = subprocess.run(["bash", "-n"], input=step["run"],
                                                    text=True, capture_output=True, timeout=5)
                            self.assertEqual(result.returncode, 0,
                                             f"{path.name}/{name}/{step.get('name')}: {result.stderr}")

    def test_ordinary_checks_have_main_and_pr_coverage_without_signing_environments(self):
        for name in ("ci.yml", "macos.yml", "macos-standalone.yml", "homebrew.yml",
                     "release-packages.yml", "kraken-bundled.yml"):
            document = workflow(name)
            self.assertIn("pull_request", document["on"], name)
            self.assertIn("main", document["on"]["push"]["branches"], name)
        package = workflow("release-packages.yml")["jobs"]["package"]
        self.assertNotIn("environment", package)
        self.assertEqual(package["strategy"]["matrix"], "${{ fromJSON(needs.validate.outputs.matrix) }}")
        matrix = json.loads(subprocess.check_output(["python3", str(ROOT / "script/artifact-plan.py"), "matrix"], text=True))
        self.assertEqual(len(matrix["include"]), 11)
        self.assertEqual(sum("script/artifact-plan.py native-package" in step.get("run", "")
                             for step in package["steps"]), 3)

    def test_publication_requires_explicit_dispatch_opt_in(self):
        for name, job_name, ref in (
            ("homebrew.yml", "publish", "refs/heads/main"),
            ("release-packages.yml", "sign-and-draft-release", "refs/tags/v1.2.3"),
            ("publish-package-repository.yml", "publish-pages", "refs/heads/main"),
        ):
            document = workflow(name)
            self.assertEqual(document["on"]["workflow_dispatch"]["inputs"]["publish"]["default"], "false")
            guard = document["jobs"][job_name]["if"]
            for event in ("pull_request", "push", "schedule", "release"):
                for opt_in in (False, True):
                    self.assertFalse(condition(guard, event, ref, publish=opt_in), (name, event))
            self.assertFalse(condition(guard, "workflow_dispatch", ref, publish=False), name)
            self.assertTrue(condition(guard, "workflow_dispatch", ref, publish=True), name)
            for step in document["jobs"][job_name].get("steps", []):
                if step.get("uses", "").startswith("actions/checkout@"):
                    self.assertEqual(step.get("with", {}).get("persist-credentials"), "false")

    def test_offline_qualification_contracts_run_on_both_hosted_platforms(self):
        for name, job_name in (("ci.yml", "test"), ("macos.yml", "local-runtime")):
            job = workflow(name)["jobs"][job_name]
            with self.subTest(workflow=name):
                step = next(step for step in job["steps"] if step.get("name") ==
                            "Run offline replay and qualification contract regressions")
                self.assertNotIn("if", step)
                self.assertNotIn("continue-on-error", step)
                self.assertNotIn("continue-on-error", job)
                expected = [f"python3 -m unittest discover -s test/{directory} -p {suite}.py"
                            for directory, suite in (
                                ("gpu", "test_recorded_iq_qualification"),
                                ("script", "test_replay_session"),
                                ("script", "test_run_isolated_iq_replay"),
                                ("script", "test_observer_allocation_contract"))]
                self.assertEqual(step["run"].splitlines(), expected)
                source = "\n".join(step.get("run", "") for step in job["steps"])
                self.assertNotIn("test/gpu/recorded_iq_qualification.py", source)

    def test_signing_downloads_only_packages_and_rechecks_remote_tag(self):
        jobs = workflow("release-packages.yml")["jobs"]
        signing = jobs["sign-and-draft-release"]
        download = next(step for step in signing["steps"]
                        if step.get("uses", "").startswith("actions/download-artifact@"))
        matched = [step["with"]["path"] for step in jobs["package"]["steps"]
                   if step.get("uses", "").startswith("actions/upload-artifact@")
                   and fnmatch.fnmatchcase(step["with"]["name"], download["with"]["pattern"])]
        self.assertEqual(matched, ["dist/"])
        self.assertEqual(download["with"]["run-id"], "${{ inputs.candidate_run_id }}")
        self.assertNotIn("package", signing["needs"])
        self.assertEqual(jobs["package"]["if"], "always() && inputs.publish != true")
        source = "\n".join(step.get("run", "") for step in signing["steps"])
        self.assertLess(source.index("verify-release-soak.py verify"), source.index("gpg --batch --import"))
        self.assertIn("verify-release-soak.py signed", source)
        self.assertIn("release-soak-provenance.json", source)
        publish = signing["steps"][-1]["run"]
        self.assertLess(publish.index('git fetch --no-tags origin "refs/tags/${TAG}"'),
                        publish.index('gh release create'))
        self.assertIn("git rev-parse 'FETCH_HEAD^{commit}'", publish)
        self.assertIn("GITHUB_RUN_ID", publish)

    def test_linux_receipts_do_not_deadlock_pr_main_or_unsigned_dry_runs(self):
        for name in ("ci.yml", "release-packages.yml"):
            guard = workflow(name)["jobs"]["qualified-linux-gpu"]["if"]
            for event in ("push", "pull_request", "workflow_dispatch"):
                self.assertFalse(condition(guard, event), (name, event))
        self.assertTrue(condition(workflow("ci.yml")["jobs"]["qualified-linux-gpu"]["if"],
                                  "workflow_dispatch", require_gpu_receipts=True))
        self.assertTrue(condition(workflow("release-packages.yml")["jobs"]["qualified-linux-gpu"]["if"],
                                  "workflow_dispatch", "refs/tags/v1.2.3", publish=True))

    def test_publication_authenticates_all_hosted_workflows_before_credentials(self):
        for name, job_name, credential in (
            ("release-packages.yml", "sign-and-draft-release", "gpg --batch --import"),
            ("homebrew.yml", "publish", "HOMEBREW_TAP_DEPLOY_KEY"),
        ):
            job = workflow(name)["jobs"][job_name]
            self.assertEqual(job["permissions"]["actions"], "read")
            steps = job["steps"]
            gate_index = next(index for index, step in enumerate(steps)
                              if "verify-release-ci.py" in step.get("run", ""))
            credential_index = next(index for index, step in enumerate(steps)
                                    if credential in step.get("run", ""))
            self.assertLess(gate_index, credential_index)
            self.assertEqual(steps[gate_index]["env"]["GH_TOKEN"], "${{ github.token }}")
        signing = workflow("release-packages.yml")["jobs"]["sign-and-draft-release"]
        checksum = next(step["run"] for step in signing["steps"] if step.get("name") ==
                        "Collect final signed release assets and checksums")
        self.assertIn("release-ci-provenance.json", checksum)
        repository = workflow("publish-package-repository.yml")["jobs"]["rebuild-repository"]
        download = next(step["run"] for step in repository["steps"] if step.get("name") ==
                        "Download and verify reviewed release package assets")
        self.assertLess(download.index("gpgv --keyring"),
                        download.index("--pattern release-ci-provenance.json"))
        self.assertLess(download.index("--pattern release-ci-provenance.json"),
                        download.index("sha256sum --check --strict"))

    def test_main_and_pr_package_versions_allow_real_upgrade_without_publication(self):
        step = next(step for step in workflow("release-packages.yml")["jobs"]["validate"]["steps"]
                    if step.get("id") == "version")
        with tempfile.TemporaryDirectory(prefix="vectorwarp-ci-contract-") as directory:
            root = Path(directory)
            fake_git = root / "git"
            fake_git.write_text('#!/bin/sh\ncase "$1" in\nshow-ref) exit 0;;\n'
                                'rev-list|rev-parse) echo "$GITHUB_SHA";;\n*) exit 90;;\nesac\n')
            fake_git.chmod(0o755)
            for event, ref, opt_in, expected in (
                ("push", "refs/heads/main", "false", "9999.0.0"),
                ("pull_request", "refs/pull/1/merge", "false", "9999.0.0"),
                ("workflow_dispatch", "refs/heads/main", "false", "1.2.3"),
                ("workflow_dispatch", "refs/tags/v1.2.3", "true", "1.2.3"),
                ("workflow_dispatch", "refs/heads/main", "true", None),
            ):
                output = root / "output"
                output.write_text("")
                environment = {**os.environ, "PATH": f"{root}:{os.environ['PATH']}",
                    "GITHUB_SHA": "a" * 40, "GITHUB_REF": ref, "GITHUB_EVENT_NAME": event,
                    "GITHUB_REF_TYPE": "tag" if ref.startswith("refs/tags/") else "branch",
                    "GITHUB_REF_NAME": ref.removeprefix("refs/heads/").removeprefix("refs/tags/"),
                    "PUBLISH": opt_in, "INPUT_VERSION": "1.2.3", "GITHUB_OUTPUT": str(output)}
                result = subprocess.run(["bash", "-euo", "pipefail", "-c", step["run"]],
                                        env=environment, text=True, capture_output=True, timeout=5)
                if expected is None:
                    self.assertNotEqual(result.returncode, 0)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f"version={expected}\n", output.read_text())

    def test_hosted_builds_cannot_imply_physical_gpu_acceptance(self):
        builder = (ROOT / "script/build-macos.sh").read_text()
        self.assertIn("PHYSICAL_GPU_TESTS=OFF", builder)
        self.assertIn("--physical-gpu-test) RUN_TESTS=true; PHYSICAL_GPU_TESTS=ON", builder)
        for name in ("ci.yml", "macos.yml", "macos-standalone.yml"):
            source = (WORKFLOWS / name).read_text()
            self.assertNotIn("test/gpu/product_parity_test.py", source)
            self.assertNotIn("--physical-gpu-test", source)
        qualification = workflow("gpu-qualification.yml")
        self.assertEqual(set(qualification["on"]), {"workflow_dispatch"})
        self.assertIn("environment", qualification["jobs"]["qualify"])
        self.assertEqual(qualification["jobs"]["qualify"]["strategy"]["max-parallel"], "1")

    def test_linux_resident_temporal_parity_is_explicit_cpu_only_vulkan(self):
        job = workflow("ci.yml")["jobs"]["test"]
        source = "\n".join(step.get("run", "") for step in job["steps"])
        self.assertIn("mesa-vulkan-drivers", source)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", source)
        self.assertIn("-DBLAH2_TEST_SOFTWARE_VULKAN=ON", source)
        self.assertIn("ctest --test-dir build/ci --output-on-failure --no-tests=error", source)
        cmake = (ROOT / "cmake/GpuProcessTests.cmake").read_text()
        self.assertRegex(cmake, r'option\(BLAH2_TEST_SOFTWARE_VULKAN [^\n]+ OFF\)')
        self.assertIn("COMMAND testGpuTbdFractional --software", cmake)
        self.assertIn("COMMAND testGpuTrackerCorrectness --software", cmake)
        self.assertIn("set_tests_properties(residentReplaySnapshotCpuContract PROPERTIES TIMEOUT 60)", cmake)
        self.assertIn("COMMAND testClutterReplaySoftware --software", cmake)
        self.assertIn("add_test(NAME clutterReplayCheckpointCpuContract COMMAND testClutterReplayCheckpoint)", cmake)
        clutter = (ROOT / "test/gpu/TestClutterReplaySoftware.cpp").read_text()
        self.assertIn("candidate.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU", clutter)
        self.assertIn("physical_gpu_execution=0", clutter)
        for name in ("residentReplayShaderCpuCompile", "residentWireCpuBoundary",
                     "residentProcessCpuBoundary", "residentReplaySnapshotCpuContract",
                     "gpuResultJsonCpuCompatibility"):
            self.assertIn("add_test(NAME " + name, cmake)


if __name__ == "__main__":
    unittest.main()
