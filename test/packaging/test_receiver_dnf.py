"""DNF5 approval/signature/lock failure injection; never mutate packages."""
import copy
import importlib.util
import pathlib
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('receiver_dnf', ROOT / 'script/vectorwarp-receiver-dnf.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class Session:
    def __init__(self):
        self.locked, self.downloads, self.commits = True, 0, 0
        self.signature, self.postcondition = True, True
        self.transaction = {'platform': 'fedora:44', 'packages': [{'name': 'hackrf', 'version': '1.0-1.fc44'}],
            'changes': [{'name': 'hackrf', 'version': '1.0-1.fc44', 'architecture': 'x86_64',
                         'sha256': 'a' * 64, 'size': 1024, 'origin': 'Fedora', 'archive': 'updates',
                         'site': 'mirrors.fedoraproject.org'}], 'statusSha256': 'b' * 64}
        self.current = 'b' * 64
    def resolve(self, receiver_type, roots=None):
        assert self.locked and receiver_type == 'HackRF'
        return copy.deepcopy(self.transaction)
    def download_and_verify(self, transaction):
        assert self.locked
        self.downloads += 1
        adapter.require(self.signature, 'PACKAGE_SIGNATURE_FAILED', 'unsigned fixture')
    def status(self):
        assert self.locked
        return self.current
    def commit(self, transaction):
        assert self.locked
        self.commits += 1
        adapter.require(self.postcondition, 'PACKAGE_POSTCONDITION_FAILED', 'partial fixture')
    def close(self):
        self.locked = False


class DnfTest(unittest.TestCase):
    def test_shared_native_uhd_floor_is_41(self):
        for version in ['4.1.0.5-3', '4.6.0.0-2', '1:4.9.0.1-1.fc44']:
            adapter.common.require_uhd_version(version)
        for version in ['4.0.0.0', '3.15.0', 'unavailable']:
            with self.assertRaises(adapter.Refused) as caught:
                adapter.common.require_uhd_version(version)
            self.assertEqual(caught.exception.code, 'UHD_VERSION_UNSUPPORTED')

    def setUp(self):
        self.session = Session()
        transaction = copy.deepcopy(self.session.transaction)
        self.action = {'manager': 'dnf', 'receiverType': 'HackRF', 'packages': transaction['packages'], 'transaction': transaction}
        self.request = {'action': self.action, 'configPath': '/fixture',
                        'configRevision': adapter.hashlib.sha256(b'config').hexdigest()}

    def execute(self):
        with mock.patch.object(adapter, 'read_regular', return_value=b'config'):
            return adapter.transact('execute', self.request, self.session)

    def refused(self, code, call=None):
        with self.assertRaises(adapter.Refused) as caught:
            (call or self.execute)()
        self.assertEqual(caught.exception.code, code)
        self.assertFalse(self.session.locked)

    def test_complete_approved_transaction_holds_lock_through_postcondition(self):
        self.assertTrue(self.execute()['ready'])
        self.assertEqual((self.session.downloads, self.session.commits), (1, 1))
        self.assertFalse(self.session.locked)

    def test_plan_and_inspect_never_download_or_install(self):
        result = adapter.transact('plan', {'receiverType': 'HackRF'}, self.session)
        self.assertEqual(result['transaction'], self.action['transaction'])
        self.session.locked = True
        self.assertFalse(adapter.transact('inspect', self.request, self.session)['ready'])
        self.assertEqual((self.session.downloads, self.session.commits), (0, 0))

    def test_full_closure_hash_and_database_drift_refuse(self):
        for mutate in [lambda: self.session.transaction.update(statusSha256='c' * 64),
                       lambda: self.session.transaction['changes'][0].update(sha256='d' * 64),
                       lambda: self.session.transaction['changes'].append({'name': 'extra'})]:
            self.setUp(); mutate()
            self.refused('PACKAGE_TRANSACTION_CHANGED')
            self.assertEqual(self.session.downloads, 0)

    def test_database_drift_while_downloading_refuses_before_install(self):
        self.session.current = 'd' * 64
        self.refused('PACKAGE_TRANSACTION_CHANGED')
        self.assertEqual(self.session.commits, 0)

    def test_config_revision_is_the_approved_revision(self):
        self.request['configRevision'] = 'e' * 64
        self.refused('CONFIG_CHANGED')
        self.assertEqual(self.session.commits, 0)

    def test_bad_signature_never_imports_keys_or_installs(self):
        self.session.signature = False
        self.refused('PACKAGE_SIGNATURE_FAILED')
        self.assertEqual(self.session.commits, 0)

    def test_partial_install_has_no_success_receipt(self):
        self.session.postcondition = False
        self.refused('PACKAGE_POSTCONDITION_FAILED')
        self.assertEqual(self.session.commits, 1)

    def test_installed_software_is_reused(self):
        self.session.transaction['changes'] = []
        self.assertTrue(self.execute()['ready'])
        self.assertEqual((self.session.downloads, self.session.commits), (0, 0))

    def test_native_repository_endpoint_gate(self):
        class Option:
            def __init__(self, value): self.value = value
            def get_value(self): return self.value
            def empty(self): return self.value is None
        for url in ['http://mirrors.fedoraproject.org/metalink?repo=fedora-44&arch=x86_64',
                    'https://untrusted.example/metalink?repo=fedora-44&arch=x86_64',
                    'https://mirrors.fedoraproject.org/metalink?repo=updates-testing-f44&arch=x86_64',
                    'https://mirrors.fedoraproject.org/metalink?repo=fedora-44&arch=aarch64']:
            config = types.SimpleNamespace(get_metalink_option=lambda: Option(url))
            repo = types.SimpleNamespace(get_config=lambda: config, get_id=lambda: 'fedora')
            with self.assertRaises(adapter.Refused) as caught:
                adapter.verify_repo(repo, {'version': '44'}, 'x86_64')
            self.assertEqual(caught.exception.code, 'UNTRUSTED_REPOSITORY')

    def test_rpm_cookie_must_be_opened_and_nonempty(self):
        database = mock.Mock()
        database.dbCookie.side_effect = lambda: 'cookie' if database.openDB.called else None
        value = adapter.FedoraSession.__new__(adapter.FedoraSession)
        value.rpm = types.SimpleNamespace(TransactionSet=lambda: database)
        self.assertEqual(value.status(), adapter.hashlib.sha256(b'cookie').hexdigest())
        database.dbCookie.side_effect = lambda: None
        with self.assertRaises(adapter.Refused): value.status()

    def test_fresh_rpm_postcondition_rejects_missing_or_wrong_versions(self):
        value = adapter.FedoraSession.__new__(adapter.FedoraSession)
        value.lib = types.SimpleNamespace(base=types.SimpleNamespace(Transaction=types.SimpleNamespace(TransactionRunResult_SUCCESS=0)))
        value.transaction = types.SimpleNamespace(run=lambda: 0)
        header = {'epoch': 0, 'version': '1.0', 'release': '1.fc44', 'arch': 'x86_64'}
        value.rpm = types.SimpleNamespace(TransactionSet=lambda: types.SimpleNamespace(dbMatch=lambda *args: [header]))
        value.commit(self.action['transaction'])
        header['version'] = '0.9'
        with self.assertRaises(adapter.Refused) as caught: value.commit(self.action['transaction'])
        self.assertEqual(caught.exception.code, 'PACKAGE_POSTCONDITION_FAILED')


if __name__ == '__main__':
    unittest.main()
