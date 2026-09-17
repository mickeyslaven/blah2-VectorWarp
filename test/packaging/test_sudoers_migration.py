"""The obsolete grant is removed without replacing unrelated sudoers content."""
import importlib.util
import os
import pathlib
import stat
import tempfile
import unittest

SOURCE = pathlib.Path(__file__).resolve().parents[2] / 'script/vectorwarp-sudoers-migrate.py'
spec = importlib.util.spec_from_file_location('vectorwarp_sudoers_migrate', SOURCE)
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class SudoersMigrationTest(unittest.TestCase):
    def test_exact_grant_only_and_custom_content(self):
        old = b'# site comment\n' + migration.OLD_DEFAULT + migration.OLD_GRANT
        self.assertEqual(migration.retired_content(old), b'# site comment\n')
        self.assertIsNone(migration.retired_content(b'# current file\n'))
        with self.assertRaisesRegex(ValueError, 'unrecognized VectorWarp API'):
            migration.retired_content(migration.OLD_GRANT.rstrip(b'\n') + b' extra\n')
        with self.assertRaisesRegex(ValueError, 'additional VectorWarp API'):
            migration.retired_content(old + b'vectorwarp-api ALL=(root) NOPASSWD: /bin/true\n')

    @unittest.skipUnless(os.geteuid() == 0, 'trusted root-owned directory required')
    def test_atomic_upgrade_preserves_unrelated_rules(self):
        with tempfile.TemporaryDirectory(dir='/root') as directory:
            file = pathlib.Path(directory) / 'vectorwarp'
            unrelated = b'other-user ALL=(root) NOPASSWD: /usr/bin/true\n'
            file.write_bytes(migration.OLD_DEFAULT + migration.OLD_GRANT + unrelated)
            file.chmod(0o440)
            seen = []
            self.assertEqual(migration.migrate(file, verify=lambda candidate: seen.append(candidate.read_bytes())), 'retired')
            self.assertEqual(seen, [unrelated])
            self.assertEqual(file.read_bytes(), unrelated)
            self.assertEqual(stat.S_IMODE(file.stat().st_mode), 0o440)
            self.assertEqual(migration.migrate(file, verify=lambda _: self.fail('already migrated')), 'unchanged')

    @unittest.skipUnless(os.geteuid() == 0, 'trusted root-owned directory required')
    def test_symlink_and_custom_rule_are_untouched(self):
        with tempfile.TemporaryDirectory(dir='/root') as directory:
            file = pathlib.Path(directory) / 'vectorwarp'
            custom = migration.OLD_GRANT + b'vectorwarp-api ALL=(root) NOPASSWD: /bin/true\n'
            file.write_bytes(custom)
            file.chmod(0o440)
            with self.assertRaisesRegex(ValueError, 'additional VectorWarp API'):
                migration.migrate(file, verify=lambda _: None)
            self.assertEqual(file.read_bytes(), custom)
            link = pathlib.Path(directory) / 'linked'
            link.symlink_to(file)
            with self.assertRaises(OSError):
                migration.migrate(link, verify=lambda _: None)
            self.assertEqual(file.read_bytes(), custom)


if __name__ == '__main__':
    unittest.main()
