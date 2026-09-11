"""Optional Fedora44 real-binding resolver smoke. No package installation.

Uses a private empty RPM database to exercise the actual missing-package closure,
without removing installed host/container packages. Requires native libdnf5/rpm.
"""
import importlib.util
import pathlib
import tempfile
import types

import libdnf5
import rpm

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('receiver_dnf', ROOT / 'script/vectorwarp-receiver-dnf.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
adapter.platform()

with tempfile.TemporaryDirectory(prefix='vectorwarp-rpm-resolver-') as directory:
    rpm.TransactionSet(directory).initDB()
    session = adapter.FedoraSession.__new__(adapter.FedoraSession)
    session.lib = libdnf5
    session.rpm = types.SimpleNamespace(TransactionSet=lambda: rpm.TransactionSet(directory))
    session.host = {'id': 'fedora', 'version': '44'}
    session.base = libdnf5.base.Base()
    session.base.load_config()
    config = session.base.get_config()
    for name, value in [('plugins', False), ('install_weak_deps', False), ('assumeno', True),
                        ('pkg_gpgcheck', True), ('installroot', directory), ('use_host_config', True)]:
        getattr(config, f'get_{name}_option')().set(value)
    session.base.get_vars().set('releasever', '44')
    session.base.setup()
    assert session.base.lock_system_repo()
    try:
        plan = session.resolve('HackRF')
        assert len(plan['changes']) > 1
        assert any(item['name'] == 'hackrf' for item in plan['changes'])
        assert all(item['origin'] == 'Fedora' and len(item['sha256']) == 64 for item in plan['changes'])
        print('PASS native DNF5 missing HackRF closure:', len(plan['changes']), 'packages;',
              sum(item['size'] for item in plan['changes']), 'download bytes; no installation')
    finally:
        session.close()
