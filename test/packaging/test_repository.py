"""Repository integrity tests; never install packages or publish a repository.

VECTORWARP_REPOSITORY_INTEGRATION=1 also builds tiny format fixtures and signs
them with a temporary test-only key. Run that mode in an isolated test runner.
"""
import argparse
from copy import deepcopy
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("repository", ROOT / "script/build-package-repository.py")
repository = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(repository)


class HomepageTests(unittest.TestCase):
    def test_timing_claims_keep_their_scope(self):
        page = repository.repository_homepage()
        for required in ('200 ms processing deadline', '147.5 ms on CPU',
                         '138.7 ms on GPU', '238.8 ms on CPU', '162.3 ms on GPU',
                         '32% less processing time', '76.1 ms on CPU', '62.0 ms on GPU',
                         '19% less processing time', '256 delay bins', '±800 Hz',
                         'Regular blah2 cannot safely process this configuration',
                         'Ryzen AI Max+ 395', 'eight CPU cores', '2.4 MS/s',
                         '27 steady frames', 'RTX 4050 Laptop', 'four CPU cores',
                         'same IQ and settings', 'two repeats',
                         'sample-clock-paced DSP replay', 'LIVE_CAPACITY_20260910.md'):
            self.assertIn(required, page)

        # The front page selects examples; the linked report must keep the
        # full comparison, including slower configurations and test boundaries.
        report = (ROOT / 'docs/LIVE_CAPACITY_20260910.md').read_text()
        for required in ('427.68', '27/27', 'not endurance tests',
                         'No live\nupstream executable was run',
                         'full output equivalence is\nnot claimed'):
            self.assertIn(required, report)

    def test_page_has_accessible_layout_and_current_repository(self):
        page = repository.repository_homepage()
        for required in ('lang="en"', 'name="viewport"', '<caption>',
                         'scope="col"', 'scope="row"', 'overflow-x:auto',
                         'tabindex="0"', 'blah2-VectorWarp#install-on-linux',
                         'https://github.com/30hours/blah2'):
            self.assertIn(required, page)

    def test_quickstart_links_and_release_status(self):
        # Wrapping and headings are editorial choices. Keep the measured scope,
        # actual upstream comparison, and release boundaries under test.
        readme = ' '.join((ROOT / 'README.md').read_text().split())
        self.assertIn('first signed APT/DNF release is being prepared', readme)
        self.assertIn('replaying the same recorded signal at its original rate', readme)
        self.assertIn('same CPU budget', readme)
        self.assertIn('2–8-channel network input', readme)
        self.assertIn('source builds with the receiver', readme)
        for claim in ('Regular blah2 CPU', 'VectorWarp CPU', 'VectorWarp GPU',
                      'clutter FFT/filtering', 'Periodic accuracy checks',
                      'docs/GPU_BENCHMARK_20260911.md',
                      'upstream processor cannot safely represent'):
            self.assertIn(claim, readme)
        # Numeric findings belong to the linked report rather than an old
        # README headline; editorial changes must not resurrect obsolete runs.
        report = (ROOT / 'docs/GPU_BENCHMARK_20260911.md').read_text()
        self.assertIn('c821bee3f0d27cf20c8447f3d908ef722905a4de', report)
        self.assertIn('1e-4', report)
        self.assertTrue((ROOT / 'html/favicon/vectorwarp-vw.svg').is_file())
        for name in ('README.md', 'docs/INSTALL.md', 'docs/SETUP.md', 'packaging/README.md'):
            document = ROOT / name
            for target in re.findall(r'\]\(([^)]+)\)', document.read_text()):
                if '://' in target or target.startswith('mailto:'):
                    continue
                relative, _, anchor = target.partition('#')
                destination = document.parent / relative if relative else document
                with self.subTest(document=name, target=target):
                    self.assertTrue(destination.is_file(), f'missing link: {target}')
                    if anchor:
                        headings = re.findall(r'^#+\s+(.+)$', destination.read_text(), re.MULTILINE)
                        anchors = {re.sub(r'[^\w\- ]', '', heading.lower()).replace(' ', '-')
                                   for heading in headings}
                        self.assertIn(anchor, anchors)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp-manifest-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / "vectorwarp_1.2.3-1_ubuntu24.04_amd64.deb"
        self.package.write_bytes(b"manifest validation fixture, not a DEB")
        self.entry = dict(name="vectorwarp", version="1.2.3", release="1", format="deb",
                          distro="ubuntu", distro_version="24.04", codename="noble", arch="amd64",
                          filename=self.package.name, size=self.package.stat().st_size,
                          sha256=repository.sha256(self.package), backend="kraken", gpu="auto",
                          node_version="24.21.0")
        self.manifest = self.root / "manifest.json"

    def load(self, entries=None, document=None):
        self.manifest.write_text(json.dumps(document if document is not None else
                                           {"schema": 1, "version": "1.2.3",
                                            "source_commit": "a" * 40,
                                            "packages": entries or [self.entry]}))
        return repository.load_manifest(self.manifest, self.root)[1]

    def test_valid_entry(self):
        self.assertEqual(self.load(), [self.entry])

    def test_resolute_target_is_valid(self):
        package = self.root / "vectorwarp_1.2.3-1_ubuntu26.04_amd64.deb"
        package.write_bytes(b"resolute manifest fixture, not a DEB")
        entry = {**self.entry, "distro_version": "26.04", "codename": "resolute",
                 "filename": package.name, "size": package.stat().st_size,
                 "sha256": repository.sha256(package)}
        self.assertEqual(self.load([entry]), [entry])

    def test_debian_trixie_target_is_valid(self):
        package = self.root / "vectorwarp_1.2.3-1_debian13_amd64.deb"
        package.write_bytes(b"trixie manifest fixture, not a DEB")
        entry = {**self.entry, "distro": "debian", "distro_version": "13", "codename": "trixie",
                 "filename": package.name, "size": package.stat().st_size,
                 "sha256": repository.sha256(package)}
        self.assertEqual(self.load([entry]), [entry])

    def test_bad_document_shapes(self):
        for document in ([], None, 1, "text", {"schema": 2, "packages": []},
                         {"schema": 1, "packages": []}, {"schema": 1, "packages": [None]}):
            with self.subTest(document=document):
                self.manifest.write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    repository.load_manifest(self.manifest, self.root)

    def test_manifest_requires_source_and_single_version(self):
        for document in ({"schema": 1, "version": "1.2.3", "packages": [self.entry]},
                         {"schema": 1, "version": "1.2.3", "source_commit": "short",
                          "packages": [self.entry]},
                         {"schema": 1, "version": "9.9.9", "source_commit": "a" * 40,
                          "packages": [self.entry]}):
            with self.subTest(document=document), self.assertRaises(ValueError):
                self.load(document=document)

    def test_release_matrix_requires_every_target_once(self):
        entries = []
        for format, distro, version, arch in repository.RELEASE_TARGETS:
            entries.append({"format": format, "distro": distro, "distro_version": version,
                            "arch": arch})
        repository.verify_release_matrix(entries)
        with self.assertRaisesRegex(ValueError, "matrix"):
            repository.verify_release_matrix(entries[:-1])
        with self.assertRaisesRegex(ValueError, "matrix"):
            repository.verify_release_matrix(entries + [entries[0]])

    def test_path_traversal_and_duplicate_names(self):
        for name in ("../a.deb", "/tmp/a.deb", "a/b.deb", "-a.deb", "a\nb.deb"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.load([{**self.entry, "filename": name}])
        with self.assertRaisesRegex(ValueError, "unique"):
            self.load([self.entry, self.entry])

    def test_wrong_metadata_types(self):
        for field in ("format", "distro", "distro_version", "arch", "version", "name", "release"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.load([{**self.entry, field: []}])

    def test_target_and_version_mismatch(self):
        for update in ({"arch": "riscv64"}, {"codename": "jammy"}, {"version": "1.2.3-rc1"},
                       {"name": "another-package"}, {"release": "2"}, {"distro_version": "26.04"}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.load([{**self.entry, **update}])

    def test_filename_and_build_profile_are_immutable(self):
        for update in ({"filename": "renamed.deb"}, {"backend": "all"}, {"gpu": "off"},
                       {"node_version": "25.0.0"}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.load([{**self.entry, **update}])

    def test_checksum_and_size_tampering(self):
        for update in ({"sha256": "0" * 64}, {"size": 1}, {"size": True}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.load([{**self.entry, **update}])

    def test_symlink_package(self):
        other = self.root / "other"
        self.package.rename(other)
        self.package.symlink_to(other)
        with self.assertRaisesRegex(ValueError, "regular package"):
            self.load()

    def test_size_budget(self):
        with patch.object(repository, "MAX_BYTES", 1), self.assertRaisesRegex(ValueError, "budget"):
            self.load()

    def test_rpm_digest_only_is_not_signature(self):
        reports = ((0, b"SHA256 digest: OK\n", False),
                   (0, b"Header V4 RSA/SHA256 Signature, key ID 1234: OK\n", True),
                   (0, b"Header V4 RSA/SHA256 Signature, key ID 1234: NOKEY\n", False),
                   (1, b"Header V4 RSA/SHA256 Signature, key ID 1234: OK\n", False))
        for code, report, expected in reports:
            result = subprocess.CompletedProcess([], code, stdout=report, stderr=b"")
            with patch.object(repository.subprocess, "run", return_value=result):
                self.assertEqual(repository.verified_rpm(Path("fixture"), Path("db")), expected)


@unittest.skipUnless(os.environ.get("VECTORWARP_REPOSITORY_INTEGRATION") == "1",
                     "set VECTORWARP_REPOSITORY_INTEGRATION=1 in an isolated runner")
class SignedRepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("gpg", "gpgv", "gpgconf", "dpkg-deb", "apt-ftparchive", "rpmbuild",
                     "rpm", "rpmsign", "createrepo_c", "apt-get"):
            if not shutil.which(tool):
                raise RuntimeError(f"Integration tests require {tool}; no silent skips")
        cls.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp-signed-repository-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        # Model the production custody plan: a certification-only primary stays
        # offline while CI imports an export containing only its signing subkey.
        offline_home = cls.root / "offline-gnupg"
        offline_home.mkdir(mode=0o700)
        cls.addClassCleanup(lambda: subprocess.run(
            ["gpgconf", "--homedir", str(offline_home), "--kill", "gpg-agent"],
            capture_output=True, timeout=10))
        with patch.dict(os.environ, {"GNUPGHOME": str(offline_home)}):
            repository.run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                            "--quick-generate-key", "VectorWarp ephemeral test <test@example.invalid>",
                            "rsa2048", "cert", "1d"])
            cls.public = cls.root / "public.asc"
            cls.public.write_bytes(repository.run(["gpg", "--batch", "--armor", "--export"]))
            cls.fingerprint = repository.public_fingerprint(cls.public)
            repository.run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                            "--quick-add-key", cls.fingerprint, "rsa2048", "sign", "1d"])
            cls.public.write_bytes(repository.run(["gpg", "--batch", "--armor", "--export",
                                                   cls.fingerprint]))
            subkeys = repository.run(["gpg", "--batch", "--armor", "--export-secret-subkeys",
                                      cls.fingerprint])
        cls.keyhome = cls.root / "ci-gnupg"
        cls.keyhome.mkdir(mode=0o700)
        cls.environment = patch.dict(os.environ, {"GNUPGHOME": str(cls.keyhome)})
        cls.environment.start()
        cls.addClassCleanup(cls.environment.stop)
        cls.addClassCleanup(lambda: subprocess.run(
            ["gpgconf", "--homedir", str(cls.keyhome), "--kill", "gpg-agent"],
            capture_output=True, timeout=10))
        subprocess.run(["gpg", "--batch", "--import"], input=subkeys, check=True,
                       capture_output=True, timeout=180)
        records = repository.run(["gpg", "--batch", "--with-colons", "--list-secret-keys"]).decode().splitlines()
        primary_records = [record.split(":") for record in records if record.startswith("sec:")]
        subkey_records = [record.split(":") for record in records if record.startswith("ssb:")]
        if (len(primary_records) != 1 or primary_records[0][14] != "#" or
                len(subkey_records) != 1 or subkey_records[0][11] != "s" or
                subkey_records[0][14] != "+"):
            raise RuntimeError("CI fixture must contain only a usable signing subkey, not the primary secret")
        cls.packages = cls.root / "packages"
        cls.packages.mkdir()
        cls.entries = []
        for codename, distro, version, arch in (("jammy", "ubuntu", "22.04", "amd64"),
                                                ("noble", "ubuntu", "24.04", "arm64"),
                                                ("resolute", "ubuntu", "26.04", "amd64"),
                                                ("trixie", "debian", "13", "amd64")):
            stage = cls.root / f"deb-{codename}"
            (stage / "DEBIAN").mkdir(parents=True)
            (stage / "DEBIAN/control").write_text(
                f"Package: vectorwarp\nVersion: 1.2.3-1\nArchitecture: {arch}\n"
                "Maintainer: Test <test@example.invalid>\nDescription: Repository test only\n")
            (stage / "usr/share/vectorwarp").mkdir(parents=True)
            (stage / "usr/share/vectorwarp/test.txt").write_text("Not an application package.\n")
            distro_label = "debian13" if distro == "debian" else f"ubuntu{version}"
            package = cls.packages / f"vectorwarp_1.2.3-1_{distro_label}_{arch}.deb"
            repository.run(["dpkg-deb", "--build", "--root-owner-group", str(stage), str(package)])
            cls.entries.append(cls.entry(package, "deb", distro, version, arch, codename))
        top = cls.root / "rpmbuild"
        top.mkdir()
        rpm_arch = {"x86_64": "x86_64", "aarch64": "aarch64", "arm64": "aarch64"}.get(platform.machine())
        if not rpm_arch:
            raise RuntimeError("RPM integration fixtures require a supported release architecture")
        spec = cls.root / "fixture.spec"
        spec.write_text("Name: vectorwarp\nVersion: 1.2.3\nRelease: 1.fc44\nSummary: Repository test only\n"
                        f"License: MIT\nBuildArch: {rpm_arch}\n%description\nNot an application package.\n"
                        "%install\nmkdir -p %{buildroot}/usr/share/vectorwarp\n"
                        "printf 'test only\\n' > %{buildroot}/usr/share/vectorwarp/test.txt\n"
                        "%files\n/usr/share/vectorwarp/test.txt\n")
        repository.run(["rpmbuild", "--define", f"_topdir {top}", "-bb", str(spec)])
        rpm = next((top / "RPMS").rglob("*.rpm"))
        package = cls.packages / rpm.name
        shutil.copyfile(rpm, package)
        cls.entries.append(cls.entry(package, "rpm", "fedora", "44", rpm_arch))
        cls.manifest = cls.root / "manifest.json"
        cls.manifest.write_text(json.dumps({"schema": 1, "version": "1.2.3",
                                           "source_commit": "a" * 40,
                                           "packages": cls.entries}))

    @staticmethod
    def entry(file, format, distro, version, arch, codename=None):
        result = dict(name="vectorwarp", version="1.2.3", release="1.fc44" if format == "rpm" else "1",
                      format=format, distro=distro, distro_version=version, arch=arch,
                      filename=file.name, sha256=repository.sha256(file), size=file.stat().st_size,
                      backend="kraken", gpu="auto", node_version="24.21.0")
        if codename:
            result["codename"] = codename
        return result

    def args(self, output):
        return argparse.Namespace(packages=str(self.packages), manifest=str(self.manifest),
                                  output=str(self.root / output), public_key=str(self.public),
                                  fingerprint=self.fingerprint, signing_key=self.fingerprint,
                                  installer_template=str(ROOT / "script/install-release.sh"),
                                  expected_version="1.2.3", expected_source_commit="a" * 40,
                                  require_release_matrix=False)

    def test_signed_site_and_refresh_preserve_packages(self):
        args = self.args("signed-site")
        original_hashes = {file.name: repository.sha256(file) for file in self.packages.iterdir()}
        document = repository.build(args)
        site = Path(args.output)
        self.assertEqual(len(document["packages"]), 5)
        for codename in ("jammy", "noble", "resolute", "trixie"):
            self.assertIn("Valid-Until:", (site / f"apt/dists/{codename}/Release").read_text())
            self.assertTrue((site / f"apt/dists/{codename}/InRelease").is_file())
        self.assertIn(self.fingerprint, (site / "install.sh").read_text())
        self.assertNotIn("@SIGNING_FINGERPRINT@", (site / "install.sh").read_text())
        # Let the real package manager verify/download indexes, but confine all
        # state/cache writes to this disposable fixture. Never install packages.
        apt_state = self.root / "apt-test"
        (apt_state / "lists/partial").mkdir(parents=True)
        (apt_state / "cache/archives/partial").mkdir(parents=True)
        (apt_state / "status").touch()
        sources = apt_state / "sources.list"
        sources.write_text("".join(
            f"deb [arch=amd64 signed-by={site}/keys/vectorwarp.gpg] file://{site}/apt {codename} main\n"
            for codename in ("jammy", "noble", "resolute", "trixie")))
        repository.run(["apt-get", "-o", f"Dir::Etc::sourcelist={sources}",
                        "-o", "Dir::Etc::sourceparts=-", "-o", f"Dir::State={apt_state}",
                        "-o", f"Dir::State::status={apt_state}/status", "-o", f"Dir::Cache={apt_state}/cache",
                        "-o", "APT::Sandbox::User=root", "-o", "APT::Architecture=amd64",
                        "-o", "Acquire::Languages=none", "-o", "APT::Update::Error-Mode=any", "update"])
        for file in self.packages.iterdir():
            self.assertEqual(repository.sha256(file), original_hashes[file.name], "Input package mutated")
        refresh_packages = self.root / "refresh-packages"
        refresh_packages.mkdir()
        for entry in document["packages"]:
            published = site / entry["repository_path"]
            self.assertEqual(repository.sha256(published), entry["sha256"])
            shutil.copyfile(published, refresh_packages / entry["filename"])
        refresh = self.args("refreshed-site")
        refresh.packages = str(refresh_packages)
        refresh.manifest = str(site / "repository-manifest.json")
        actual_run = repository.run

        def forbid_resigning(command, **kwargs):
            self.assertNotEqual(command[0], "rpmsign", "Refresh must not re-sign an immutable RPM")
            return actual_run(command, **kwargs)

        with patch.object(repository, "run", side_effect=forbid_resigning):
            refreshed = repository.build(refresh)
        self.assertEqual(document["packages"], refreshed["packages"])
        with self.assertRaisesRegex(ValueError, "must not exist"):
            repository.build(args)

    def test_bad_public_key_and_secret_file_fail_closed(self):
        args = self.args("wrong-key-site")
        args.fingerprint = args.signing_key = "0" * 40
        with self.assertRaisesRegex(ValueError, "fingerprint"):
            repository.build(args)
        secret = self.root / "secret-test-key.asc"
        secret.write_bytes(repository.run(["gpg", "--batch", "--armor", "--export-secret-keys",
                                          self.fingerprint]))
        args = self.args("secret-site")
        args.public_key = str(secret)
        with self.assertRaisesRegex(ValueError, "public|secret"):
            repository.build(args)
        self.assertFalse(Path(args.output).exists())

    def test_release_selection_and_full_matrix_fail_closed(self):
        args = self.args("wrong-version-site")
        args.expected_version = "9.9.9"
        with self.assertRaisesRegex(ValueError, "version"):
            repository.build(args)
        args = self.args("wrong-source-site")
        args.expected_source_commit = "b" * 40
        with self.assertRaisesRegex(ValueError, "source commit"):
            repository.build(args)
        args = self.args("partial-matrix-site")
        args.require_release_matrix = True
        with self.assertRaisesRegex(ValueError, "matrix"):
            repository.build(args)

    def test_real_metadata_mismatch_and_tampering(self):
        entry = deepcopy(self.entries[0])
        entry["version"] = "9.9.9"
        with self.assertRaisesRegex(ValueError, "metadata"):
            repository.verify_metadata(entry, self.packages / entry["filename"])
        args = self.args("tampered-site")
        bad = self.root / "tampered-manifest.json"
        bad.write_text(json.dumps({"schema": 1, "version": "1.2.3", "source_commit": "a" * 40,
                                   "packages": [{**self.entries[0], "sha256": "0" * 64}]}))
        args.manifest = str(bad)
        with self.assertRaisesRegex(ValueError, "checksum"):
            repository.build(args)
        self.assertFalse(Path(args.output).exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
