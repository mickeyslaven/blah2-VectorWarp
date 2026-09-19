#!/usr/bin/env python3
"""Small real Mach-O/signature fixtures for the local finalization boundary."""
import importlib.util
import json
from pathlib import Path
import platform
import shutil
import subprocess
import unittest
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('standalone_signer', ROOT / 'script/sign-macos-standalone.py')
signer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(signer)
PACKAGE_SPEC = importlib.util.spec_from_file_location('standalone_package_test', ROOT / 'test/macos/standalone_package_test.py')
fixtures = importlib.util.module_from_spec(PACKAGE_SPEC)
PACKAGE_SPEC.loader.exec_module(fixtures)


@unittest.skipUnless(platform.system() == 'Darwin', 'macOS codesign/clang required')
class SigningTest(unittest.TestCase):
    compile = fixtures.StandalonePackageTest.compile
    runtime = fixtures.StandalonePackageTest.runtime

    def setUp(self):
        fixtures.StandalonePackageTest.setUp(self)
        self.app = None

    def tearDown(self):
        fixtures.StandalonePackageTest.tearDown(self)

    def source_runtime(self, arch):
        runtime, _, _ = self.runtime(arch, 'a' * 40)
        binary = runtime / 'bin/fixture'
        for relative in signer.EXECUTABLES:
            target = runtime / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(binary, target)
            signer.run('/usr/bin/codesign', '--force', '--sign', '-', target)
        (runtime / 'python/lib').symlink_to('../lib')
        binary.unlink()
        core = runtime / 'bin/libblah2-capture-core.dylib'
        source_core = next(p for p in (runtime / 'lib').glob('*.dylib')
                           if all(link.startswith(signer.package.SYSTEM) for link in signer.package.commands(p)[0]))
        shutil.copy2(source_core, core)
        kit = runtime / 'receiver-source/rspduo'
        kit.mkdir(parents=True)
        (kit / 'example.c').write_text('/* fixture source */\n')
        (kit / 'kit.json').write_text(json.dumps({'kit_id': 'b' * 64, 'cohort': 'c' * 64,
            'sources': {'example.c': signer.package.digest(kit / 'example.c')},
            'core_sha256': signer.package.digest(core)}))
        metadata = signer.read_json(runtime / 'standalone.json')
        metadata['native_files'] = signer.package.native_audit(runtime, arch, '15.0')
        metadata['files'] = signer.package.files_manifest(runtime)
        signer.write_json(runtime / 'standalone.json', metadata)
        return runtime

    def assembled(self):
        arm = self.source_runtime('arm64')
        intel = self.source_runtime('x86_64')
        notices = self.base / 'notices'; notices.mkdir()
        (notices / 'LICENSE').write_text('fixture notice\n')
        signer.write_json(notices / 'notices.json', {'schema': 1, 'source_id': 'a' * 40,
            'runtime_manifest_sha256': {a: signer.package.digest(r / 'standalone.json') for a, r in [('arm64', arm), ('x86_64', intel)]},
            'files': {'LICENSE': signer.package.digest(notices / 'LICENSE')}})
        signer.package.assemble(SimpleNamespace(arm64=arm, x86_64=intel,
                                notices_dir=notices, version='1.2.3', output=self.base / 'assembled'))
        return self.base / 'assembled/VectorWarp.app'

    def test_ad_hoc_signs_actual_native_leaves_and_preserves_original_receipts(self):
        app = self.assembled()
        original = {a: signer.package.digest(app / 'Contents/Resources/runtime' / a / 'standalone.json') for a in signer.package.ARCHES}
        original_notice = signer.package.digest(app / 'Contents/Resources/ThirdPartyNotices/notices.json')
        output = self.base / 'signed'
        signer.finalize(SimpleNamespace(app=app, output=output, ad_hoc=True,
            application_identity=None, installer_identity=None, team_id=None))
        self.assertEqual({a: signer.package.digest(app / 'Contents/Resources/runtime' / a / 'standalone.json') for a in signer.package.ARCHES}, original)
        receipt = signer.read_json(output / 'receipts/signing-transform.json')
        self.assertEqual(receipt['input_runtime_manifest_sha256'], original)
        self.assertEqual(receipt['input_notice_sha256'], original_notice)
        self.assertEqual(receipt['status'], 'ad-hoc-development-only')
        self.assertFalse(receipt['notarized'])
        self.assertFalse(receipt['hardened_runtime_enabled'])
        self.assertEqual(receipt['applied_entitlement_sha256'], {})
        self.assertTrue((output / 'VectorWarp-local-ad-hoc.pkg').is_file())
        signed = output / 'VectorWarp.app'
        for arch in signer.package.ARCHES:
            runtime = signed / 'Contents/Resources/runtime' / arch
            manifest = signer.package.audit_runtime(runtime, arch)
            self.assertEqual(manifest['signing_transform']['original_manifest_sha256'], original[arch])
            signer.validate_kit(runtime)
            for executable in signer.EXECUTABLES:
                path = runtime / executable
                signer.verify_code(path, 'ad-hoc', None, True)
                native = {'arm64': 'arm64', 'aarch64': 'arm64',
                          'x86_64': 'x86_64', 'amd64': 'x86_64'}.get(platform.machine().lower())
                if arch == native:
                    result = subprocess.run([str(path)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0,
                                     f'{arch}/{executable} could not load its private dylibs: {result.stderr}')
        signer.run('/usr/bin/codesign', '--verify', '--deep', '--strict', signed)
        with self.assertRaisesRegex(ValueError, 'output already exists'):
            signer.finalize(SimpleNamespace(app=app, output=output, ad_hoc=True,
                application_identity=None, installer_identity=None, team_id=None))

    def test_bad_notice_binding_rejected_before_output(self):
        app = self.assembled()
        notice = app / 'Contents/Resources/ThirdPartyNotices/notices.json'
        data = signer.read_json(notice)
        data['runtime_manifest_sha256']['arm64'] = '0' * 64
        signer.write_json(notice, data)
        output = self.base / 'rejected'
        with self.assertRaisesRegex(ValueError, 'notices runtime manifest mismatch'):
            signer.finalize(SimpleNamespace(app=app, output=output, ad_hoc=True,
                application_identity=None, installer_identity=None, team_id=None))
        self.assertFalse(output.exists())

    def test_installer_report_requires_trusted_matching_leaf(self):
        good = '''Package "fixture.pkg":\n   Status: signed by a certificate trusted by macOS\n   Certificate Chain:\n    1. Developer ID Installer: Example (ABCDE12345)\n    2. Developer ID Certification Authority\n'''
        signer.verify_installer_signature_report(good, 'ABCDE12345')
        macos26 = '''Package "fixture.pkg":\n   Status: signed by a developer certificate issued by Apple for distribution\n   Signed with a trusted timestamp on: 2026-09-19 14:29:56 +0000\n   Certificate Chain:\n    1. Developer ID Installer: Example (ABCDE12345)\n       SHA256 fingerprint: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n    2. Developer ID Certification Authority\n    3. Apple Root CA\n'''
        signer.verify_installer_signature_report(macos26, 'ABCDE12345')
        for bad in (good.replace('ABCDE12345', 'WRONG12345'),
                    good.replace('Example (ABCDE12345)', 'ABCDE12345 Example (WRONG12345)'),
                    good.replace('Developer ID Installer:', 'Developer ID Application:'),
                    good.replace('signed by a certificate trusted by macOS', 'signed by an untrusted certificate'),
                    good.replace('signed by a certificate trusted by macOS', 'unsigned'),
                    macos26.replace('signed by a developer certificate issued by Apple for distribution',
                                    'signed by a developer certificate issued by Apple for development'),
                    good.replace('1. Developer ID Installer:', '2. Developer ID Installer:'),
                    macos26.replace('Example (ABCDE12345)', 'ABCDE12345 Example (WRONG12345)'),
                    macos26.replace('Developer ID Installer:', 'Developer ID Application:'),
                    macos26.replace('signed by a developer certificate issued by Apple for distribution', 'signed by an untrusted certificate')):
            with self.assertRaises(ValueError):
                signer.verify_installer_signature_report(bad, 'ABCDE12345')


if __name__ == '__main__':
    unittest.main()
