"""Build-time RPM kit finalization; no system writes, SDKs or receiver calls."""
import importlib.util
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('finalize_kit', ROOT / 'script/finalize-rspduo-kit.py')
finalizer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(finalizer)


class FinalizeKitTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='vectorwarp-rpm-finalizer-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.current = self.root / 'opt/vectorwarp/current'
        self.kit = self.current / 'receiver-source/rspduo/kit.json'
        self.kit.parent.mkdir(parents=True)
        self.core = self.current / 'bin/libblah2-capture-core.so.1'
        self.core.parent.mkdir()
        self.core.write_bytes(b'core after RPM processing')
        helper = self.current.parent / 'libexec/vectorwarp-cohort-check'
        helper.parent.mkdir()
        shutil.copyfile(ROOT / 'tools/vectorwarp_soak_identity.py', helper)
        self.identity = runpy.run_path(str(helper))
        for name in self.identity['NATIVE_HASH_PATHS'].values():
            (self.current / name).write_bytes(b'native before RPM processing')
        self.manifest = self.current / '.vectorwarp-build'
        self.manifest.write_text('source_commit=' + 'a' * 40 + '\n' + ''.join(
            f'{key}={value}\n' for key, value in self.identity['installed_hashes'](self.current).items()))
        self.source = self.kit.parent / 'source.cpp'
        self.source.write_text('VectorWarp fixture source\n')
        self.metadata = self.root / 'opt/vectorwarp/PACKAGE-METADATA'
        self.metadata.write_text('backend=all\ntest_only=false\nlocal_build_receivers=RspDuo\n')
        self.value = dict(schema=1, receiver='RspDuo', kit_id='a' * 64, cohort='b' * 64,
                          compiler={'id': 'GNU', 'version': 'fixture'}, core_sha256='c' * 64,
                          sources={'source.cpp': self.identity['file_hash'](self.source)})
        self.kit.write_text(json.dumps(self.value))
        self.kit.chmod(0o644)

    def test_binds_final_bytes_preserving_other_fields_and_mode(self):
        finalizer.finalize(self.root)
        self.value['core_sha256'] = self.identity['file_hash'](self.core)
        self.assertEqual(json.loads(self.kit.read_text()), self.value)
        self.assertEqual(self.kit.stat().st_mode & 0o777, 0o644)
        first = self.kit.read_bytes()
        finalizer.finalize(self.root)
        self.assertEqual(self.kit.read_bytes(), first)
        self.assertEqual(list(self.kit.parent.glob('.kit-*')), [])

    def test_binds_every_post_brp_native_member_and_refuses_partial_pins(self):
        for key, name in self.identity['NATIVE_HASH_PATHS'].items():
            (self.current / name).write_bytes(('post-BRP ' + key).encode())
        with self.assertRaisesRegex(ValueError, 'installed bytes'):
            self.identity['check_declared_hashes'](self.current)
        finalizer.finalize(self.root)
        self.assertEqual(self.identity['check_declared_hashes'](self.current),
                         self.identity['installed_hashes'](self.current))
        self.assertTrue(self.manifest.read_text().startswith('source_commit=' + 'a' * 40 + '\n'))
        self.manifest.write_text('\n'.join(line for line in self.manifest.read_text().splitlines()
                                          if not line.startswith('gpu_worker_sha256=')) + '\n')
        with self.assertRaisesRegex(ValueError, 'pre-BRP native hash cohort'):
            finalizer.finalize(self.root)

    def test_changed_source_is_not_silently_rebound(self):
        original = self.kit.read_bytes()
        self.source.write_text('changed source')
        with self.assertRaisesRegex(ValueError, 'changed a local kit source'):
            finalizer.finalize(self.root)
        self.assertEqual(self.kit.read_bytes(), original)

    def test_release_directory_and_core_soname_symlinks(self):
        releases = self.current.parent / 'releases'
        releases.mkdir()
        self.current.rename(releases / 'fixture')
        self.current.symlink_to('releases/fixture', target_is_directory=True)
        self.core.rename(self.core.parent / 'libblah2-capture-core.so.1.0.0')
        self.core.symlink_to('libblah2-capture-core.so.1.0.0')
        finalizer.finalize(self.root)
        self.assertEqual(json.loads(self.kit.read_text())['core_sha256'], self.identity['file_hash'](self.core))

    def test_core_symlink_cannot_escape_buildroot(self):
        original = self.kit.read_bytes()
        self.core.unlink()
        self.core.symlink_to('/etc/hosts')
        with self.assertRaises(ValueError):
            finalizer.finalize(self.root)
        self.assertEqual(self.kit.read_bytes(), original)

    def test_missing_stable_kit_fails_but_explicit_open_test_skips(self):
        self.kit.unlink()
        with self.assertRaises(FileNotFoundError):
            finalizer.finalize(self.root)
        self.metadata.write_text('backend=open-test\ntest_only=true\n')
        (self.current / 'bin/blah2').write_bytes(b'post-BRP open-test processor')
        finalizer.finalize(self.root)
        self.assertFalse(self.kit.exists())
        self.identity['check_declared_hashes'](self.current)
        self.kit.write_text(json.dumps(self.value))
        with self.assertRaisesRegex(ValueError, 'unexpectedly contains'):
            finalizer.finalize(self.root)

    def test_invalid_profile_and_manifest_fail(self):
        self.metadata.write_text('backend=all\ntest_only=true\n')
        with self.assertRaisesRegex(ValueError, 'receiver profile'):
            finalizer.finalize(self.root)
        self.metadata.write_text('backend=all\ntest_only=false\nlocal_build_receivers=RspDuo\n')
        self.value['receiver'] = 'HackRF'
        self.kit.write_text(json.dumps(self.value))
        with self.assertRaisesRegex(ValueError, 'invalid local'):
            finalizer.finalize(self.root)

    def test_no_live_root_or_escaping_source(self):
        with self.assertRaisesRegex(ValueError, 'separate RPM build root'):
            finalizer.finalize('/')
        self.value['sources'] = {'../../outside': '0' * 64}
        self.kit.write_text(json.dumps(self.value))
        with self.assertRaisesRegex(ValueError, 'source path'):
            finalizer.finalize(self.root)

    def test_spec_preserves_distro_chain_and_packager_checks_finished_rpm(self):
        spec = (ROOT / 'packaging/rpm/vectorwarp.spec.in').read_text()
        packager = (ROOT / 'script/package-native.sh').read_text()
        self.assertIn('%global vectorwarp_saved_os_install_post %{__os_install_post}', spec)
        self.assertIn('%global __os_install_post %{vectorwarp_saved_os_install_post} '
                      '/usr/bin/python3 -I "%{SOURCE1}" "%{buildroot}"', spec)
        self.assertNotIn('%global __os_install_post %{nil}', spec)
        self.assertNotIn('strip --strip-unneeded', packager)
        self.assertIn('SOURCES/finalize-rspduo-kit.py', packager)
        self.assertIn('final RPM core hash does not match local RSPduo kit', packager)
        self.assertIn("identity['check_declared_hashes'](root)", packager)
        self.assertLess(packager.index('rpmbuild --define'),
                        packager.index('final RPM core hash does not match local RSPduo kit'))

    def test_finished_payload_uses_stable_helper_after_release_symlink_resolution(self):
        releases = self.current.parent / 'releases'
        releases.mkdir()
        self.current.rename(releases / 'fixture')
        self.current.symlink_to('releases/fixture', target_is_directory=True)
        self.assertFalse((self.current / 'libexec').exists())
        finalizer.finalize(self.root)
        packager = (ROOT / 'script/package-native.sh').read_text()
        verifier = packager.split("import json, pathlib, runpy, sys\n", 1)[1].split('\nPY', 1)[0]
        command = [sys.executable, '-c', 'import json, pathlib, runpy, sys\n' + verifier,
                   str(self.current), 'false']
        subprocess.run(command, check=True, capture_output=True)
        (self.current / 'bin/blah2').write_bytes(b'changed after RPM processing')
        self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
