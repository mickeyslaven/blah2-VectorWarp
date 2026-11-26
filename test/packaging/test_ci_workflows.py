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
    def test_release_cycle_evidence_is_gated_in_ci_and_packages(self):
        ordinary = (WORKFLOWS / "ci.yml").read_text()
        for suite in ("test/script/test_release_regression.py", "test/script/test_export_cycles.py",
                      "test/script/test_operational_acceptance.py", "test/script/test_functional_acceptance.py"):
            self.assertIn(suite, ordinary)
        release = (WORKFLOWS / "release-packages.yml").read_text()
        self.assertIn("script/verify-release-ci.py", release)
        self.assertNotIn("test/recording/processor_replay_test.py", release)

    def test_setup_node_receives_reviewed_version_from_executable_step_output(self):
        expected = subprocess.check_output(
            ["python3", str(ROOT / "script/artifact-plan.py"), "pin", "node"], text=True).strip()
        with tempfile.TemporaryDirectory(prefix="vectorwarp-node-workflow-") as directory:
            workspace = Path(directory)
            (workspace / "script").symlink_to(ROOT / "script", target_is_directory=True)
            (workspace / "vectorwarp").symlink_to(ROOT, target_is_directory=True)
            for path in sorted(WORKFLOWS.glob("*.yml")):
                for name, job in workflow(path.name)["jobs"].items():
                    preceding = {}
                    for step in job.get("steps", []):
                        if step.get("uses", "").startswith("actions/setup-node@"):
                            with self.subTest(workflow=path.name, job=name):
                                self.assertNotIn("node-version-file", step["with"])
                                reference = re.fullmatch(
                                    r"\$\{\{ steps\.([\w-]+)\.outputs\.([\w-]+) \}\}",
                                    step["with"]["node-version"])
                                self.assertIsNotNone(reference)
                                producer, key = reference.groups()
                                output = workspace / "step output"
                                output.write_text("")
                                subprocess.run(["bash", "-euo", "pipefail", "-c", preceding[producer]["run"]],
                                               cwd=workspace, env={**os.environ, "GITHUB_OUTPUT": str(output)},
                                               check=True, capture_output=True, text=True, timeout=10)
                                self.assertEqual(dict(line.split("=", 1) for line in
                                                      output.read_text().splitlines())[key], expected)
                        if "id" in step:
                            preceding[step["id"]] = step

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
                     "kraken-bundled.yml"):
            document = workflow(name)
            self.assertIn("pull_request", document["on"], name)
            self.assertIn("main", document["on"]["push"]["branches"], name)
        release = workflow("release-packages.yml")
        self.assertIn("pull_request", release["on"])
        self.assertNotIn("push", release["on"])
        self.assertEqual(release["concurrency"]["cancel-in-progress"],
                         "${{ github.event_name == 'pull_request' }}")
        self.assertEqual(release["jobs"]["verify"]["timeout-minutes"], "10")
        self.assertIn("github.event_name != 'pull_request'", release["jobs"]["package"]["if"])
        self.assertEqual(workflow("macos-standalone.yml")["jobs"]["runtime"]["if"],
                         "github.event_name != 'pull_request'")
        self.assertIn("github.event_name != 'pull_request'", workflow("homebrew.yml")["jobs"]["formula"]["if"])
        linux = workflow("ci.yml")["jobs"]["test"]
        once = {"Test GPU and receiver build contracts",
                "Run offline replay and qualification contract regressions",
                "Run offline Pi packaging and diagnostics checks",
                "Run repository generator unit and ephemeral integration tests",
                "Run API and UI unit tests", "Exercise real unprivileged broker requests with NoNewPrivileges",
                "Run staged native-package acceptance checks", "Generate immutable upstream review artifact",
                "Upload review-only upstream comparison", "Check all browser routes and kiosk-responsive states"}
        self.assertEqual({step["name"] for step in linux["steps"] if step.get("if") ==
                          "matrix.runner == 'ubuntu-24.04'"}, once)
        package = workflow("release-packages.yml")["jobs"]["package"]
        self.assertNotIn("environment", package)
        self.assertEqual(package["strategy"]["matrix"], "${{ fromJSON(needs.validate.outputs.matrix) }}")
        matrix = json.loads(subprocess.check_output(["python3", str(ROOT / "script/artifact-plan.py"), "matrix"], text=True))
        self.assertEqual(len(matrix["include"]), 12)
        self.assertEqual(sum("script/artifact-plan.py native-package" in step.get("run", "")
                             for step in package["steps"]), 3)

    def test_release_entrypoints_and_review_assets_have_one_contract_owner(self):
        for name in ("build-native", "install-native", "package-native", "install-release",
                     "smoke-native-package", "test-installed-package"):
            subprocess.run(["bash", "-n", str(ROOT / f"script/{name}.sh")], check=True)
        compile((ROOT / "script/build-package-repository.py").read_text(),
                "script/build-package-repository.py", "exec")
        release = (WORKFLOWS / "release-packages.yml").read_text()
        for value in ("release-assets/package-manifest.json", "release-assets/repository-manifest.json",
                      "cp repository/repository-manifest.json release-assets/package-manifest.json",
                      "cp repository/repository-manifest.json release-assets/repository-manifest.json",
                      "release-assets/upstream-diff.txt release-assets/UPSTREAM_BASELINE.json",
                      "script/upstream-diff.py --baseline docs/UPSTREAM_BASELINE.json"):
            self.assertIn(value, release)
        ordinary = (WORKFLOWS / "ci.yml").read_text()
        for value in ("NODE_PATH: ${{ github.workspace }}/api/node_modules",
                      "npm run test:dom --prefix api", "node test/ui/deployment.test.js",
                      "python3 test/packaging/test_ci_workflows.py -v"):
            self.assertIn(value, ordinary)
        release = (WORKFLOWS / "release-packages.yml").read_text()
        self.assertIn("python3 test/packaging/test_ci_workflows.py -v", release)
        self.assertIn("script/verify-release-ci.py", release)
        for duplicate in ("npm ci --prefix api", "cmake --build build/ci",
                          "test/recording/processor_replay_test.py"):
            self.assertNotIn(duplicate, release)
        for source in (ordinary, release):
            self.assertNotIn("yaml.safe_load", source, "Workflow syntax is checked by this shared owner")
        helper = (ROOT / "script/upstream-diff.py").read_text()
        for value in ("git diff --stat", "git diff --name-status", "https://github.com/{upstream}.git",
                      "moving branch"):
            self.assertIn(value, helper)

    def test_cpu_package_gate_precedes_all_target_build_with_strict_warnings(self):
        job = workflow("ci.yml")["jobs"]["test"]
        source = "\n".join(step.get("run", "") for step in job["steps"])
        gate = source.index("script/prebuild_acceptance.py")
        self.assertLess(source.index("npm ci --prefix api"), gate)
        self.assertLess(source.index("-DCMAKE_COMPILE_WARNING_AS_ERROR=ON"), gate)
        self.assertLess(gate, source.index("cmake --build build/ci"))
        self.assertIn("-DBLAH2_GPU=ON", source)
        self.assertNotIn("--skip-target-build", source)
        gate_run = next(step["run"] for step in job["steps"]
                        if "script/prebuild_acceptance.py" in step.get("run", ""))
        # Execute the real shell assembly without invoking builds or devices.
        probe = 'python3() { printf "%s\\n" "$@"; };\n'
        args = subprocess.check_output(["bash", "-euc", probe + gate_run],
                                       text=True, timeout=5).splitlines()
        self.assertEqual(args[:7], ["script/prebuild_acceptance.py", "--build-dir", "build/ci",
                                   "--receipt", "build/ci/prebuild-acceptance.json",
                                   "--native-target", "blah2"])
        expected = (
            ("testGpuTrackerAdmission", "gpuTrackerCapacityAdmission"),
            ("testTracker", "testTracker"), ("testDetectionMath", "testDetectionMath"),
            ("testGpuTrackerCorrectness", "residentTrackerSoftwareVulkanParity"),
            ("testMultiIlluminatorRadar", "multiIlluminatorRadar"),
            ("testFrontendPlan", "frontendPlanParity"),
            ("testGpuTbdFractional", "residentTemporalSoftwareVulkanParity"),
            ("testGpuResidentWire", "residentWireCpuBoundary"),
            ("testResidentReplaySnapshot", "residentReplaySnapshotCpuContract"),
            ("testResidentReplayShaders", "residentReplayShaderCpuCompile"),
            ("testTrackBeforeDetect", "trackBeforeDetectMath"),
            ("testGpuResultJson", "gpuResultJsonCpuCompatibility"),
            ("testResidentReplayInspector", "residentReplayInspectorCpuContract"),
            ("testDirectPathCoherentReplay", "directPathCoherentReplayCpuContract"),
        )
        self.assertEqual(args[7:], [argument for target, test in expected
                                   for argument in ("--native-target", target, "--native-test", test)])
        evidence = next(s for s in job["steps"] if
                        "build/ci/prebuild-acceptance.json" in s.get("with", {}).get("path", ""))
        self.assertEqual(evidence["if"], "always()")
        release = workflow("release-packages.yml")["jobs"]["verify"]
        receipt = next(step for step in release["steps"] if
                       step.get("name") == "Require successful exact-source hosted qualification")
        self.assertIn("script/verify-release-ci.py", receipt["run"])

    def test_native_packages_do_not_require_unshipped_api_dependencies(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn("NAME frontendPlanNativeContract", cmake)
        self.assertIn('if(EXISTS "${PROJECT_SOURCE_DIR}/api/node_modules/js-yaml/index.js")', cmake)
        self.assertLess(cmake.index("NAME frontendPlanNativeContract"),
                        cmake.index("NAME frontendPlanParity"))
        package = workflow("release-packages.yml")["jobs"]["package"]
        self.assertIn("verify", package["needs"],
                      "Every package build must depend on exact-source API/native parity")

    def test_macos_installs_api_dependencies_before_native_ctest(self):
        job = workflow("macos.yml")["jobs"]["local-runtime"]
        steps = job["steps"]
        install = next(index for index, step in enumerate(steps)
                       if "npm ci --prefix api" in step.get("run", ""))
        build = next(index for index, step in enumerate(steps)
                     if "script/build-macos.sh" in step.get("run", ""))
        self.assertLessEqual(install, build)
        build_run = steps[build].get("run", "")
        self.assertLess(build_run.index("npm ci --prefix api"),
                        build_run.index("script/build-macos.sh"),
                        "frontendPlanParity requires the API dependency tree during CTest")
        self.assertEqual(sum("npm ci --prefix api" in step.get("run", "") for step in steps), 1)

    def test_full_browser_route_matrix_is_required_on_linux_and_both_macs(self):
        for name, job_name in (("ci.yml", "test"), ("macos.yml", "local-runtime")):
            job = workflow(name)["jobs"][job_name]
            step = next(s for s in job["steps"] if "node " in s.get("run", "") and
                        "all-routes-layout.cjs" in s["run"])
            self.assertNotIn("continue-on-error", step)
            self.assertEqual(step["env"]["VECTORWARP_LAYOUT_INTERACTIONS"], "1")
            self.assertTrue(step["env"]["VECTORWARP_LAYOUT_SCREENSHOTS"].endswith("/screenshots"))
            self.assertTrue(step["env"]["VECTORWARP_LAYOUT_REPORT"].endswith("/browser-layout.json"))
            measured_timeout_limit = 15 if name == "ci.yml" else 25
            self.assertLessEqual(int(step["timeout-minutes"]), measured_timeout_limit)
            self.assertNotIn("VECTORWARP_LAYOUT_ROUTES", step["run"], "Do not select a partial route subset")
            self.assertNotIn("VECTORWARP_LAYOUT_ROUTES", step["env"])
            self.assertIn("chromium.executablePath()", step["run"])
            self.assertIn("playwright install", step["run"])
            self.assertNotIn("--only-shell", step["run"], "CDP runner launches the full Chromium binary")
            self.assertIn("fusion-responsive.cjs", step["run"],
                          "The portrait fusion kiosk needs its real-browser regression")
            self.assertIn("statistics.cjs", step["run"],
                          "The statistics kiosk needs its real-browser regression")
            self.assertIn("VECTORWARP_SCREENSHOT_DIR", step["run"],
                          "Retain fusion-kiosk screenshots with route evidence")
            self.assertLess(step["run"].index("fusion-responsive.cjs"),
                            step["run"].index("all-routes-layout.cjs"))
            self.assertLess(step["run"].index("statistics.cjs"),
                            step["run"].index("all-routes-layout.cjs"))
            if name == "ci.yml":
                self.assertEqual(step["if"], "matrix.runner == 'ubuntu-24.04'")
                self.assertIn("patch", " ".join(s.get("run", "") for s in job["steps"]
                                                if "apt-get install" in s.get("run", "")))
            else:
                self.assertEqual(step["if"], "matrix.runner == 'macos-15'")
                runners = job["strategy"]["matrix"]["runner"]
                self.assertIn("github.event_name == 'pull_request'", runners)
                self.assertIn('["macos-15","macos-15-intel"]', runners)
            evidence = next(s for s in job["steps"] if s.get("with", {}).get("path", "").endswith("browser-acceptance/"))
            self.assertIn("always()", evidence["if"])
            self.assertEqual(int(evidence["with"]["retention-days"]), 14)

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
                if name == "ci.yml":
                    self.assertEqual(step["if"], "matrix.runner == 'ubuntu-24.04'")
                else:
                    self.assertNotIn("if", step)
                self.assertNotIn("continue-on-error", step)
                self.assertNotIn("continue-on-error", job)
                expected = [f"python3 -m unittest discover -s test/{directory} -p {suite}.py"
                            for directory, suite in (
                                ("gpu", "test_recorded_iq_qualification"),
                                ("script", "test_replay_session"),
                                ("script", "test_run_isolated_iq_replay"),
                                ("script", "test_observer_allocation_contract"))] + [
                    "python3 test/script/test_release_regression.py -v",
                    "python3 test/script/test_operational_acceptance.py -v",
                    "python3 test/script/test_functional_acceptance.py -v",
                    "python3 test/script/test_export_cycles.py -v"]
                self.assertEqual(step["run"].splitlines(), expected)
                source = "\n".join(step.get("run", "") for step in job["steps"])
                self.assertNotIn("test/gpu/recorded_iq_qualification.py", source)

    def test_signing_downloads_only_packages_and_rechecks_remote_tag(self):
        jobs = workflow("release-packages.yml")["jobs"]
        signing = jobs["sign-and-draft-release"]
        self.assertIn(repr(signing['name']), (ROOT / 'script/macos-release-agent.py').read_text(),
                      'The local Mac completer must authenticate the actual successful signing job')
        download = next(step for step in signing["steps"]
                        if step.get("uses", "").startswith("actions/download-artifact@"))
        matched = [step["with"]["path"] for step in jobs["package"]["steps"]
                   if step.get("uses", "").startswith("actions/upload-artifact@")
                   and fnmatch.fnmatchcase(step["with"]["name"], download["with"]["pattern"])]
        self.assertEqual(matched, ["dist/"])
        self.assertEqual(download["with"]["run-id"], "${{ inputs.candidate_run_id }}")
        self.assertNotIn("package", signing["needs"])
        self.assertEqual(jobs["package"]["if"],
                         "always() && github.event_name != 'pull_request' && inputs.publish != true")
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

    def test_physical_qualification_does_not_depend_on_unapproved_research_bounds(self):
        jobs = workflow("gpu-qualification.yml")["jobs"]
        steps = jobs["build"]["steps"]
        self.assertTrue(steps[0]["uses"].startswith("actions/checkout@"))
        self.assertEqual(steps[0]["with"]["ref"], "${{ github.sha }}")
        self.assertEqual(jobs["build"]["needs"], "authorize")
        self.assertIn("build", jobs["qualify"]["needs"])
        source = (WORKFLOWS / "gpu-qualification.yml").read_text()
        for research in ("pre_soak_receipt_gate.py", "recorded_iq_qualification.py", "PRE_SOAK_PLAN"):
            self.assertNotIn(research, source)
        validator = (ROOT / "test/gpu/verify_qualification_receipts.py").read_text()
        self.assertNotIn("pre_soak_receipt_gate", validator)
        self.assertIn("validate_pair(", validator)
        self.assertIn("validate_evidence_files(receipt, path)", validator)

    def test_physical_products_and_unified_evidence_reach_both_receipt_gates(self):
        jobs = workflow("gpu-qualification.yml")["jobs"]
        steps = jobs["qualify"]["steps"]
        producers = [step for step in steps if "test/gpu/unified_physical_corpus.py" in step.get("run", "")]
        self.assertEqual(len(producers), 1)
        producer = producers[0]
        self.assertNotIn("continue-on-error", producer)
        self.assertIn("test/gpu/product_parity_test.py", producer["run"])
        self.assertIn('--source-sha "$GITHUB_SHA"', producer["run"])
        for diagnostic_mode in ("--golden-only", "--policy-comparison", "--capture"):
            self.assertNotIn(diagnostic_mode, producer["run"])
        upload = next(step for step in steps if step.get("uses", "").startswith("actions/upload-artifact@"))
        self.assertGreater(steps.index(upload), steps.index(producer))
        self.assertEqual(upload["if"], "always()")
        self.assertIn("build/gpu-feature-evidence/", upload["with"]["path"].splitlines())
        downloads = [step["with"] for step in jobs["acceptance"]["steps"]
                     if step.get("uses", "").startswith("actions/download-artifact@")]
        fp64_artifact = upload["with"]["name"].replace("${{ matrix.device_class }}", "fp64")
        self.assertTrue(any(fnmatch.fnmatchcase(fp64_artifact, step.get("pattern", "")) for step in downloads))
        acceptance = "\n".join(step.get("run", "") for step in jobs["acceptance"]["steps"])
        self.assertIn("verify_qualification_receipts.py --receipts build/gpu-receipts", acceptance)
        release = "\n".join(step.get("run", "") for step in workflow("gpu-receipt-gate.yml")["jobs"]["receipt"]["steps"])
        self.assertIn("for feature in ('fp32', 'fp64'):", release)
        self.assertIn("f'gpu-product-parity-{feature}-{sha}'", release)
        self.assertIn("verify_qualification_receipts.py --receipts build/gpu-receipts", release)

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
