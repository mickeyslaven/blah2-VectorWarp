"""Failure/recovery tests for install-release.sh.

The full-path tests replace curl, install, apt-get and systemctl with recorders.
They never write system repository files, invoke a package manager, or touch a
service. OpenPGP fixtures are ephemeral test keys only.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
INSTALLER = ROOT / "script/install-release.sh"
FUTURE = 2_000_000_000


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, timeout=30,
                          check=False, **kwargs)


class InstallerFailureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for command in ("bash", "gpg", "date", "dpkg", "install", "mktemp", "stat"):
            if not shutil.which(command):
                raise RuntimeError(f"installer tests require {command}; no silent skips")
        cls.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp-installer-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.root.chmod(0o755)
        cls.installer = cls.root / "install-release.sh"
        shutil.copy2(INSTALLER, cls.installer)
        cls.installer.chmod(0o755)
        cls.keys = cls.root / "keys"
        cls.keys.mkdir(mode=0o755)
        cls.public, cls.secret, cls.fingerprint = cls.make_key("one")
        cls.public_two, _, cls.fingerprint_two = cls.make_key("two")
        cls.expired, _, cls.expired_fingerprint = cls.make_key("expired", "20200101T000000")
        cls.multiple = cls.keys / "multiple.asc"
        cls.multiple.write_bytes(cls.public.read_bytes() + cls.public_two.read_bytes())
        cls.multiple.chmod(0o644)
        cls.oversized = cls.keys / "oversized.asc"
        cls.oversized.write_bytes(b"x" * (1024 * 1024 + 1))
        cls.oversized.chmod(0o644)
        cls.empty = cls.keys / "empty.asc"
        cls.empty.touch(mode=0o644)
        cls.symlink = cls.keys / "public-link.asc"
        cls.symlink.symlink_to(cls.public)
        cls.fake_bin = cls.root / "fake-bin"
        cls.fake_bin.mkdir(mode=0o755)
        cls.write_executable("apt-get", """
            printf 'apt-get %s\n' "$*" >>"$TEST_LOG"
            case "${1:-}" in
              update) exit "${TEST_APT_UPDATE_EXIT:-0}" ;;
              install) exit "${TEST_APT_INSTALL_EXIT:-0}" ;;
            esac
        """)
        cls.write_executable("install", """
            case " $* " in
              *' /usr/share/keyrings/'*|*' /etc/apt/sources.list.d/'*|*' /etc/yum.repos.d/'*)
                printf 'install-system %s\n' "$*" >>"$TEST_LOG"
                exit "${TEST_INSTALL_EXIT:-0}" ;;
              *) exec /usr/bin/install "$@" ;;
            esac
        """)
        cls.write_executable("systemctl", """
            printf 'systemctl %s\n' "$*" >>"$TEST_LOG"
            exit "${TEST_SYSTEMCTL_EXIT:-0}"
        """)
        cls.write_executable("curl", """
            output=
            while [ "$#" -gt 0 ]; do
              case "$1" in --output) output=$2; shift 2 ;; *) shift ;; esac
            done
            printf 'curl download\n' >>"$TEST_LOG"
            if [ "${TEST_CURL_PARTIAL:-0}" = 1 ]; then
              printf 'partial, invalid key' >"$output"
              exit 22
            fi
            /usr/bin/cp "$TEST_CURL_SOURCE" "$output"
        """)
        cls.path = str(cls.fake_bin) + os.pathsep + os.environ.get("PATH", "")

    @classmethod
    def make_key(cls, suffix, faked_time=None):
        home = cls.root / f"gnupg-{suffix}"
        home.mkdir(mode=0o700)
        environment = {**os.environ, "GNUPGHOME": str(home)}
        identity = f"VectorWarp installer fixture {suffix} <{suffix}@example.invalid>"
        clock = ["--faked-system-time", faked_time] if faked_time else []
        result = run(["gpg", *clock, "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                      "--quick-generate-key", identity, "ed25519", "cert", "1d"], env=environment)
        if result.returncode:
            raise RuntimeError(result.stderr)
        listing = run(["gpg", "--batch", "--with-colons", "--list-keys", identity], env=environment)
        fingerprint = next(line.split(":")[9] for line in listing.stdout.splitlines()
                           if line.startswith("fpr:"))
        result = run(["gpg", *clock, "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                      "--quick-add-key", fingerprint, "ed25519", "sign", "1d"], env=environment)
        if result.returncode:
            raise RuntimeError(result.stderr)
        public = cls.keys / f"public-{suffix}.asc"
        secret = cls.keys / f"secret-{suffix}.asc"
        public.write_text(run(["gpg", "--batch", "--armor", "--export", fingerprint],
                              env=environment).stdout)
        secret.write_text(run(["gpg", "--batch", "--armor", "--export-secret-subkeys", fingerprint],
                              env=environment).stdout)
        public.chmod(0o644)
        secret.chmod(0o644)
        run(["gpgconf", "--homedir", str(home), "--kill", "all"], env=environment)
        return public, secret, fingerprint

    @classmethod
    def write_executable(cls, name, body):
        file = cls.fake_bin / name
        file.write_text("#!/bin/sh\nset -eu\n" + textwrap.dedent(body))
        file.chmod(0o755)

    def setUp(self):
        descriptor, path = tempfile.mkstemp(prefix="vectorwarp-installer-log-", dir="/tmp")
        os.close(descriptor)
        self.log = Path(path)
        self.log.chmod(0o666)
        self.addCleanup(self.log.unlink, missing_ok=True)

    def invoke(self, *arguments, environment=None, nonroot=False, simulated_root=False):
        env = {**os.environ, "PATH": self.path, "TEST_LOG": str(self.log),
               "TEST_CURL_SOURCE": str(self.public), **(environment or {})}
        command = ["bash", str(self.installer), *arguments]
        if simulated_root and os.geteuid() != 0:
            sudo = shutil.which("sudo")
            if not sudo:
                raise RuntimeError("root-path simulation requires sudo when tests run as a non-root user")
            assignments = [f"{name}={value}" for name, value in env.items()
                           if name.startswith("TEST_") or name == "PATH"]
            command = [sudo, "--non-interactive", "/usr/bin/env", *assignments, *command]
        demote = None
        if nonroot and os.geteuid() == 0:
            def demote():
                os.setgroups([])
                os.setgid(65534)
                os.setuid(65534)
        return run(command, env=env, preexec_fn=demote)

    def preflight(self, key, fingerprint=None, *extra):
        return self.invoke("--preflight", "--key-file", str(key), "--fingerprint",
                           fingerprint or self.fingerprint, *extra)

    def test_real_key_wrong_multiple_and_secret_inputs(self):
        passed = self.preflight(self.public)
        self.assertEqual(passed.returncode, 0, passed.stderr)
        self.assertIn("preflight passed", passed.stdout)
        for key, fingerprint, message in (
                (self.public, "0" * 40, "fingerprint"),
                (self.multiple, self.fingerprint, "single pinned"),
                (self.secret, self.fingerprint, "secret key material"),
                (self.expired, self.expired_fingerprint, "expired, revoked")):
            with self.subTest(message=message):
                result = self.preflight(key, fingerprint)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
        self.assertEqual(self.log.read_text(), "")

    def test_local_key_must_be_bounded_regular_file(self):
        for key in (self.empty, self.oversized, self.symlink):
            with self.subTest(key=key.name):
                result = self.preflight(key)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("key", result.stderr)
        self.assertEqual(self.log.read_text(), "")

    def test_key_status_parser_rejects_expired_and_revoked_identities(self):
        fp = "A" * 40
        valid = (f"pub:-:255:22:KEY:1700000000:{FUTURE}::-:::cSC:\n"
                 f"fpr:::::::::{fp}:\n"
                 f"sub:-:255:22:SUB:1700000000:{FUTURE}:::::s:\n"
                 f"fpr:::::::::{'B' * 40}:\n")
        cases = (
            ("expired primary", valid.replace(f":{FUTURE}::-", ":1::-", 1),
             "primary key is expired, revoked"),
            ("revoked primary", valid.replace("pub:-:", "pub:r:", 1),
             "primary key is expired, revoked"),
            ("disabled primary", valid.replace("pub:-:", "pub:d:", 1),
             "primary key is expired, revoked"),
            ("revoked subkey", valid.replace("sub:-:", "sub:r:", 1),
             "no unexpired, non-revoked signing subkey"),
        )
        for label, records, expected_message in cases:
            with self.subTest(label=label):
                command = """
                    source "$1"
                    gpg() { printf '%s\n' "$FAKE_GPG_RECORDS"; }
                    date() { printf '1800000000\n'; }
                    validate_public_key ignored "$2"
                """
                result = run(["bash", "-c", command, "key-parser", str(self.installer), fp],
                             env={**os.environ, "FAKE_GPG_RECORDS": records})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected_message, result.stderr)

    def test_repository_url_rejects_ambiguous_or_credentialed_forms(self):
        command = 'source "$1"; validate_repository_url "$2"'
        accepted = ("https://example.invalid", "https://localhost:8443/repository",
                    "https://127.0.0.1/path")
        rejected = ("http://example.invalid", "https://", "https://user:pass@example.invalid",
                    "https://example.invalid?q=x", "https://example.invalid/#fragment",
                    "https://bad host.invalid", "https://example.invalid\\path",
                    "https://example.invalid:0", "https://example.invalid:65536",
                    "https://bad_host.invalid")
        for url in accepted:
            with self.subTest(url=url):
                result = run(["bash", "-c", command, "url-test", str(self.installer), url])
                self.assertEqual(result.returncode, 0, result.stderr)
        for url in rejected:
            with self.subTest(url=url):
                result = run(["bash", "-c", command, "url-test", str(self.installer), url])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("repository URL", result.stderr)

    def test_nonroot_preflight_dry_run_and_real_install_refusal(self):
        common = ("--key-file", str(self.public), "--fingerprint", self.fingerprint)
        preflight = self.invoke("--preflight", *common, nonroot=True)
        self.assertEqual(preflight.returncode, 0, preflight.stderr)
        dry_run = self.invoke("--dry-run", "--start-web", *common, nonroot=True)
        self.assertEqual(dry_run.returncode, 0, dry_run.stderr)
        self.assertIn("systemctl enable --now vectorwarp-api.service", dry_run.stdout)
        self.assertNotIn("vectorwarp-processor.service", "\n".join(
            line for line in dry_run.stdout.splitlines() if line.startswith("+")))
        refused = self.invoke(*common, nonroot=True)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("installation requires root", refused.stderr)
        self.assertEqual(self.log.read_text(), "")

    def test_default_install_is_retryable_and_never_starts_a_service(self):
        arguments = ("--key-file", str(self.public), "--fingerprint", self.fingerprint)
        first = self.invoke(*arguments, simulated_root=True)
        second = self.invoke(*arguments, simulated_root=True)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("installer did not start radar", second.stdout)
        self.assertIn("existing service state was not verified", second.stdout)
        log = self.log.read_text()
        self.assertEqual(log.count("apt-get update"), 2)
        self.assertEqual(log.count("apt-get install vectorwarp"), 2)
        self.assertNotIn("systemctl", log)
        self.assertNotIn("vectorwarp-processor", log)

    def test_package_manager_failures_retain_a_clear_retry_boundary(self):
        arguments = ("--key-file", str(self.public), "--fingerprint", self.fingerprint)
        update = self.invoke(*arguments, environment={"TEST_APT_UPDATE_EXIT": "23"}, simulated_root=True)
        self.assertNotEqual(update.returncode, 0)
        self.assertIn("configuration was retained for a safe retry", update.stderr)
        self.assertNotIn("apt-get install", self.log.read_text())
        self.log.write_text("")
        install = self.invoke(*arguments, environment={"TEST_APT_INSTALL_EXIT": "24"}, simulated_root=True)
        self.assertNotEqual(install.returncode, 0)
        self.assertIn("configuration was retained for a safe retry", install.stderr)
        self.assertIn("apt-get update", self.log.read_text())
        self.assertIn("apt-get install vectorwarp", self.log.read_text())
        self.assertNotIn("systemctl", self.log.read_text())

    def test_start_web_is_explicit_and_limited_to_api(self):
        result = self.invoke("--start-web", "--key-file", str(self.public),
                             "--fingerprint", self.fingerprint, simulated_root=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("installer did not start radar", result.stdout)
        self.assertIn("existing radar service state was not verified", result.stdout)
        log = self.log.read_text()
        self.assertIn("systemctl enable --now vectorwarp-api.service", log)
        self.assertNotIn("vectorwarp-processor", log)
        self.log.write_text("")
        failed = self.invoke("--start-web", "--key-file", str(self.public),
                             "--fingerprint", self.fingerprint,
                             environment={"TEST_SYSTEMCTL_EXIT": "26"}, simulated_root=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("installer did not start radar", failed.stderr)
        self.assertIn("existing service state was not verified", failed.stderr)
        self.assertIn("systemctl enable --now vectorwarp-api.service", self.log.read_text())

    def test_partial_and_oversized_downloads_fail_before_system_changes(self):
        partial = self.invoke("--preflight", "--fingerprint", self.fingerprint,
                              environment={"TEST_CURL_PARTIAL": "1"})
        self.assertNotEqual(partial.returncode, 0)
        self.assertIn("no system configuration was changed", partial.stderr)
        self.assertNotIn("install-system", self.log.read_text())
        self.log.write_text("")
        oversized = self.invoke("--preflight", "--fingerprint", self.fingerprint,
                                environment={"TEST_CURL_SOURCE": str(self.oversized)})
        self.assertNotEqual(oversized.returncode, 0)
        self.assertIn("between 1 byte and 1 MiB", oversized.stderr)
        self.assertNotIn("install-system", self.log.read_text())

    def test_system_file_failure_stops_before_package_manager(self):
        result = self.invoke("--key-file", str(self.public), "--fingerprint", self.fingerprint,
                             environment={"TEST_INSTALL_EXIT": "25"}, simulated_root=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no package manager was run", result.stderr)
        self.assertNotIn("apt-get", self.log.read_text())

    def test_missing_required_command_is_explicit(self):
        minimal = self.root / "minimal-bin"
        minimal.mkdir(exist_ok=True)
        for name in ("uname", "dpkg", "date", "gpg", "install", "mktemp", "stat"):
            destination = minimal / name
            if not destination.exists():
                destination.symlink_to(shutil.which(name))
        result = run(["/bin/bash", str(self.installer), "--preflight", "--key-file", str(self.public),
                      "--fingerprint", self.fingerprint],
                     env={**os.environ, "PATH": str(minimal), "TEST_LOG": str(self.log)})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing command: apt-get", result.stderr)
        (minimal / "apt-get").symlink_to(self.fake_bin / "apt-get")
        result = run(["/bin/bash", str(self.installer), "--preflight", "--start-web",
                      "--key-file", str(self.public), "--fingerprint", self.fingerprint],
                     env={**os.environ, "PATH": str(minimal), "TEST_LOG": str(self.log)})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing command: systemctl", result.stderr)
        self.assertEqual(self.log.read_text(), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
