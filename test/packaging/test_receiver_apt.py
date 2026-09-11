"""Offline APT closure/lock failure injection. Never install real packages."""
import importlib.util
import pathlib
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('receiver_apt', ROOT / 'script/vectorwarp-receiver-apt.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class Version:
    def __init__(self, version='1.0'):
        self.version = version
        self.architecture, self.sha256, self.size = 'amd64', 'a' * 64, 1024
        self.origins = [types.SimpleNamespace(trusted=True, origin='Ubuntu',
            site='archive.ubuntu.com', archive='noble')]


class Package:
    def __init__(self, name, version='1.0', installed=False):
        self.name, self.candidate = name, Version(version)
        self.versions = [self.candidate]
        self.installed = self.candidate if installed else None
        self.marked_install, self.marked_delete = not installed, False

    @property
    def is_installed(self):
        return self.installed is not None

    def mark_install(self, **kwargs):
        assert kwargs == {'auto_fix': False, 'auto_inst': True}


class AptTest(unittest.TestCase):
    def setUp(self):
        self.locked, self.commits, self.after = False, [], True
        self.packages = {'hackrf': Package('hackrf'), 'libhackrf0': Package('libhackrf0')}
        test = self

        class Cache(dict):
            broken_count, delete_count, dpkg_journal_dirty = 0, 0, False
            def get_changes(self):
                return [value for value in self.values() if value.marked_install or value.marked_delete]
            def commit(self, **kwargs):
                test.assertTrue(test.locked)
                test.commits.append(kwargs)
                if test.after:
                    for package in self.values():
                        package.installed, package.marked_install = package.candidate, False
                return True
            def open(self, progress):
                test.assertTrue(test.locked)
        self.cache = Cache(self.packages)

        class Lock:
            def __enter__(self):
                test.assertFalse(test.locked)
                test.locked = True
            def __exit__(self, *_):
                test.locked = False
        self.config = {}
        self.apt_pkg = types.SimpleNamespace(SystemLock=Lock,
            init_config=lambda: None, init_system=lambda: None,
            config=types.SimpleNamespace(set=self.config.__setitem__))
        def construct():
            self.assertTrue(self.locked, 'cache must be opened under the package-manager lock')
            return self.cache
        self.apt = types.SimpleNamespace(Cache=construct)
        self.status = 'b' * 64
        self.action = None

    def run_transaction(self, operation, **extra):
        request = {'receiverType': 'HackRF'} if self.action is None else {'action': self.action}
        request.update(extra)
        return adapter.transact(operation, request, self.apt, self.apt_pkg,
            host={'id': 'ubuntu', 'version': '24.04'}, status_digest=lambda: self.status,
            check_sources=lambda: None)

    def enroll(self):
        transaction = self.run_transaction('plan')['transaction']
        self.action = {'manager': 'apt', 'receiverType': 'HackRF',
            'packages': transaction['packages'], 'transaction': transaction}
        return transaction

    def refused(self, code, call):
        with self.assertRaises(adapter.Refused) as caught:
            call()
        self.assertEqual(caught.exception.code, code)
        self.assertFalse(self.locked)

    def test_complete_signed_closure_and_locked_add_only_commit(self):
        transaction = self.enroll()
        self.assertEqual([item['name'] for item in transaction['changes']], ['hackrf', 'libhackrf0'])
        self.assertFalse(self.run_transaction('inspect')['ready'])
        with mock.patch.object(adapter, 'read_regular', return_value=b'config'):
            result = self.run_transaction('execute', configPath='/fixture',
                configRevision=adapter.hashlib.sha256(b'config').hexdigest())
        self.assertTrue(result['ready'])
        self.assertEqual(self.commits, [{'allow_unauthenticated': False}])
        self.assertEqual(self.config['APT::Get::AllowUnauthenticated'], 'false')
        self.assertTrue(self.run_transaction('inspect')['ready'])

    def test_database_dependency_and_hash_drift(self):
        self.enroll()
        self.status = 'c' * 64
        self.refused('PACKAGE_TRANSACTION_CHANGED', lambda: self.run_transaction('execute'))
        self.status = 'b' * 64
        self.cache['libhackrf0'].candidate.sha256 = 'd' * 64
        self.refused('PACKAGE_TRANSACTION_CHANGED', lambda: self.run_transaction('execute'))
        self.cache['libhackrf0'].candidate.sha256 = 'a' * 64
        self.cache['extra'] = Package('extra')
        self.refused('PACKAGE_TRANSACTION_CHANGED', lambda: self.run_transaction('execute'))
        self.assertEqual(self.commits, [])

    def test_configuration_drift_refuses_before_commit(self):
        self.enroll()
        with mock.patch.object(adapter, 'read_regular', return_value=b'changed'):
            self.refused('CONFIG_CHANGED', lambda: self.run_transaction('execute',
                configPath='/fixture', configRevision='a' * 64))
        self.assertEqual(self.commits, [])

    def test_incomplete_postcondition_is_never_success(self):
        self.enroll(); self.after = False
        with mock.patch.object(adapter, 'read_regular', return_value=b'config'):
            self.refused('PACKAGE_POSTCONDITION_FAILED', lambda: self.run_transaction('execute',
                configPath='/fixture', configRevision=adapter.hashlib.sha256(b'config').hexdigest()))
        self.assertEqual(len(self.commits), 1)

    def test_no_existing_package_changes_or_removals(self):
        self.cache['libhackrf0'].installed = Version('0.9')
        self.refused('UNSUPPORTED_TRANSACTION', lambda: self.run_transaction('plan'))
        self.cache['libhackrf0'].installed = None
        self.cache['libhackrf0'].marked_delete = True
        self.refused('UNSUPPORTED_TRANSACTION', lambda: self.run_transaction('plan'))

    def test_signed_native_origins_only(self):
        origin = self.cache['libhackrf0'].candidate.origins[0]
        for attribute, bad in [('trusted', False), ('origin', 'ThirdParty'), ('site', 'ppa.launchpad.net')]:
            old = getattr(origin, attribute)
            setattr(origin, attribute, bad)
            self.refused('UNTRUSTED_PACKAGE', lambda: self.run_transaction('plan'))
            setattr(origin, attribute, old)
        self.cache['libhackrf0'].candidate.sha256 = ''
        self.refused('MISSING_ARCHIVE_HASH', lambda: self.run_transaction('plan'))

    def test_broken_and_interrupted_database_refuses(self):
        self.cache.broken_count = 1
        self.refused('PACKAGE_DATABASE_UNHEALTHY', lambda: self.run_transaction('plan'))
        self.cache.broken_count = 0; self.cache.dpkg_journal_dirty = True
        self.refused('PACKAGE_DATABASE_UNHEALTHY', lambda: self.run_transaction('plan'))

    def test_old_uhd_and_arbitrary_roots_refuse(self):
        self.cache['uhd-host'] = Package('uhd-host', '4.6.0')
        with self.assertRaises(adapter.Refused) as caught:
            adapter.resolve(self.cache, 'Usrp', {'id': 'ubuntu', 'version': '24.04'})
        self.assertEqual(caught.exception.code, 'UHD_VERSION_UNSUPPORTED')
        self.enroll(); self.action['packages'] = [{'name': 'bash', 'version': '1.0'}]
        self.refused('INVALID_TRANSACTION', lambda: self.run_transaction('inspect'))

    def test_healthy_installed_dependencies_are_reused(self):
        for package in self.cache.values():
            package.installed, package.marked_install = package.candidate, False
        self.assertTrue(self.run_transaction('plan')['ready'])
        self.assertEqual(self.commits, [])


if __name__ == '__main__':
    unittest.main()
