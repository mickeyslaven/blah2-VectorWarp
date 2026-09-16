#!/usr/bin/env python3
"""Offline workflow regressions; the real UID boundary tests require root."""

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = ROOT / "script/prepare-receiver-build.sh"
WORKFLOW = ROOT / ".github/workflows/receiver-compatibility.yml"
DOWNLOAD_PIN = "d3f86a106a0bac45b974a628896c90dbdf5c8093"  # upstream v4.3.0


class WorkflowContractTests(unittest.TestCase):
    def test_download_action_matches_the_verified_release_pin(self):
        pins = re.findall(r"uses: actions/download-artifact@(\S+)", WORKFLOW.read_text())
        self.assertEqual(pins, [DOWNLOAD_PIN, DOWNLOAD_PIN])
        for workflow in (ROOT / ".github/workflows").glob("*.yml"):
            for pin in re.findall(r"uses: [^\s@]+@(\S+)", workflow.read_text()):
                self.assertRegex(pin, r"^[0-9a-f]{40}$", workflow.name)

    def test_candidate_stays_unprivileged_and_outside_runner_paths(self):
        workflow = WORKFLOW.read_text()
        self.assertIn("sudo bash vectorwarp/script/prepare-receiver-build.sh", workflow)
        self.assertIn('RUNNER_TEMP="$build_root"', workflow)
        self.assertIn('CANDIDATE_DIR="$build_root/candidate"', workflow)
        self.assertIn('VECTORWARP_DIR="$build_root/vectorwarp"', workflow)
        self.assertIn('sudo --user="$build_user" --set-home /usr/bin/env -i', workflow)
        self.assertIn('[[ -w "$HOME" && ! -w "$VECTORWARP_DIR" ]]', workflow)
        self.assertNotIn('sudo chown -R "$build_user:$build_user" candidate', workflow)
        self.assertNotIn("pull_request_target:", workflow)
        self.assertIn("persist-credentials: false", workflow)

    def test_uhd_mpmd_dependencies_and_system_python_are_available(self):
        workflow = WORKFLOW.read_text()
        for package in ("libprotobuf-dev", "protobuf-compiler", "libgrpc++-dev",
                        "protobuf-compiler-grpc", "python3-mako", "python3-ruamel.yaml"):
            self.assertIn(package, workflow)
        self.assertIn("-DPYTHON_EXECUTABLE=/usr/bin/python3", workflow)
        self.assertNotIn("-DENABLE_MPMD=OFF", workflow)


@unittest.skipUnless(os.geteuid() == 0 and shutil.which("setpriv"),
                     "requires root and setpriv for an actual non-root build fixture")
class BuildIdentityTests(unittest.TestCase):
    def setUp(self):
        self.source = tempfile.TemporaryDirectory(prefix="vectorwarp-private-runner-")
        self.private = pathlib.Path(self.source.name)
        self.private.chmod(0o700)
        self.candidate = self.private / "candidate"
        host = self.candidate / "host"
        host.mkdir(parents=True)
        (host / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\nproject(permission_fixture NONE)\n")
        self.vectorwarp = self.private / "vectorwarp"
        script = self.vectorwarp / "script/build-native.sh"
        script.parent.mkdir(parents=True)
        script.write_text("#!/bin/sh\nexit 0\n")
        script.chmod(0o755)
        (self.vectorwarp / "api").mkdir()
        (self.vectorwarp / "api/package.json").write_text("{}\n")
        self.stage = None

    def tearDown(self):
        if self.stage is not None:
            self.assertEqual(self.stage.parent, pathlib.Path("/tmp"))
            self.assertTrue(self.stage.name.startswith("vectorwarp-receiver-build."))
            shutil.rmtree(self.stage)
        self.source.cleanup()

    def as_builder(self, command, *args, check=True):
        return subprocess.run(
            ["setpriv", "--reuid=65534", "--regid=65534", "--clear-groups",
             "--no-new-privs", command, *map(str, args)],
            text=True, capture_output=True, check=check)

    def stage_sources(self):
        result = subprocess.run(
            ["bash", str(HELPER), str(self.candidate), str(self.vectorwarp),
             "65534", "65534"], text=True, capture_output=True, check=True)
        self.stage = pathlib.Path(result.stdout.strip())

    def test_private_runner_source_is_inaccessible_but_staged_cmake_configures(self):
        self.assertNotEqual(self.as_builder(
            "test", "-r", self.candidate / "host/CMakeLists.txt", check=False).returncode, 0)
        self.stage_sources()
        self.as_builder("test", "-r", self.stage / "candidate/host/CMakeLists.txt")
        self.as_builder("test", "-x", self.stage / "vectorwarp/script/build-native.sh")
        self.assertEqual(self.private.stat().st_mode & 0o777, 0o700)
        if shutil.which("cmake"):
            self.as_builder("cmake", "-S", self.stage / "candidate/host",
                            "-B", self.stage / "candidate-build")

    def test_trusted_source_is_read_only_but_home_and_outputs_are_writable(self):
        self.stage_sources()
        for name in ("home", "candidate", "receiver-sdk", "candidate-build",
                     "vectorwarp-build", "vectorwarp-deps"):
            self.as_builder("touch", self.stage / name / "write-test")
        for path in (self.stage, self.stage / "vectorwarp", self.stage / "vectorwarp/script"):
            self.assertNotEqual(self.as_builder("test", "-w", path, check=False).returncode, 0)
        self.assertNotEqual(self.as_builder(
            "sh", "-c", 'echo changed >> "$1"', "sh",
            self.stage / "vectorwarp/script/build-native.sh", check=False).returncode, 0)
        # build-native copies source files before npm writes/removes package files.
        target = self.stage / "vectorwarp-build/api"
        self.as_builder("cp", "-a", self.stage / "vectorwarp/api", target)
        self.as_builder("touch", target / "package-lock.json")
        self.as_builder("rm", target / "package.json")

    def test_root_build_identity_is_rejected_before_staging(self):
        for uid, gid in (("0", "65534"), ("65534", "0"), ("root", "root")):
            with self.subTest(uid=uid, gid=gid):
                result = subprocess.run(
                    ["bash", str(HELPER), str(self.candidate), str(self.vectorwarp), uid, gid],
                    text=True, capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")

    def test_candidate_symlinks_do_not_change_external_file_permissions(self):
        outside = self.private / "outside"
        outside.write_text("not a build input\n")
        outside.chmod(0o600)
        (self.candidate / "external-link").symlink_to(outside)
        before = outside.stat()
        self.stage_sources()
        after = outside.stat()
        self.assertEqual((before.st_uid, before.st_gid, before.st_mode),
                         (after.st_uid, after.st_gid, after.st_mode))
        self.assertTrue((self.stage / "candidate/external-link").is_symlink())

    def test_exact_git_preflight_leaves_inaccessible_inherited_working_directory(self):
        commits = []
        for source in (self.candidate, self.vectorwarp):
            subprocess.run(["git", "init", "-q", str(source)], check=True)
            subprocess.run([
                "git", "-C", str(source), "-c", "user.name=Fixture",
                "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false",
                "-c", "core.hooksPath=/dev/null", "commit", "--allow-empty", "-qm", "fixture",
            ], check=True)
            commits.append(subprocess.check_output(
                ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip())
        self.stage_sources()
        # Execute the actual workflow's Git/permission preflight, not a copy.
        preflight = textwrap.dedent(WORKFLOW.read_text().split("<<'BUILD'\n", 1)[1]
                                   .split("          node --version", 1)[0])
        leave_private_cwd = 'cd "$RUNNER_TEMP"\n'
        self.assertTrue(preflight.startswith(leave_private_cwd))
        command = [
            "setpriv", "--reuid=65534", "--regid=65534", "--clear-groups",
            "--no-new-privs", "/usr/bin/env", "-i", f"PATH={os.environ['PATH']}",
            f"HOME={self.stage}/home", f"RUNNER_TEMP={self.stage}",
            f"CANDIDATE_DIR={self.stage}/candidate", f"VECTORWARP_DIR={self.stage}/vectorwarp",
            f"SOURCE_COMMIT={commits[0]}", f"VECTORWARP_COMMIT={commits[1]}",
            "bash", "-seuo", "pipefail",
        ]
        old = subprocess.run(command, input=preflight.removeprefix(leave_private_cwd),
                             cwd=self.private, text=True, capture_output=True)
        self.assertNotEqual(old.returncode, 0)
        # Git versions differ between "failed to stat" and "error reading .git".
        self.assertIn(str(self.private), old.stderr)
        subprocess.run(command, input=preflight, cwd=self.private,
                       text=True, capture_output=True, check=True)


if __name__ == "__main__":
    unittest.main()
