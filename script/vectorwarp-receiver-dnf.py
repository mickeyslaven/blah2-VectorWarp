#!/usr/bin/python3 -I
"""Fedora DNF5 add-only receiver setup. No shell, plugins or key import."""
import base64
import contextlib
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import sys
from urllib.parse import urlsplit, parse_qs

spec = importlib.util.spec_from_file_location('receiver_package_common',
    pathlib.Path(__file__).with_name('vectorwarp-receiver-apt.py'))
common = importlib.util.module_from_spec(spec)
spec.loader.exec_module(common)
require, Refused, read_regular = common.require, common.Refused, common.read_regular
ROOTS = {'HackRF': 'hackrf', 'Usrp': 'uhd'}


def platform():
    values = {}
    for line in pathlib.Path('/etc/os-release').read_text().splitlines():
        key, _, value = line.partition('='); values[key] = value.strip('"')
    require(values.get('ID') == 'fedora' and values.get('VERSION_ID') == '44',
            'UNSUPPORTED_PLATFORM', 'This native DNF5 adapter currently qualifies Fedora 44 only.')
    return {'id': 'fedora', 'version': values['VERSION_ID']}


def verify_repo(repo, host, arch):
    config = repo.get_config()
    url = urlsplit(config.get_metalink_option().get_value())
    expected = {'fedora': f"fedora-{host['version']}", 'updates': f"updates-released-f{host['version']}"}
    require(repo.get_id() in expected and url.scheme == 'https' and
            url.netloc == 'mirrors.fedoraproject.org' and url.path == '/metalink' and
            parse_qs(url.query) == {'repo': [expected[repo.get_id()]], 'arch': [arch]} and
            not config.get_baseurl_option().get_value() and config.get_mirrorlist_option().empty() and
            config.get_sslverify_option().get_value(), 'UNTRUSTED_REPOSITORY',
            'Only native Fedora release/updates repositories with verified HTTPS metalinks are qualified.')
    keys = list(config.get_gpgkey_option().get_value())
    require(keys == [f"file:///etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-{host['version']}-{arch}"],
            'UNTRUSTED_REPOSITORY', 'Only the installed native Fedora release key is qualified.')
    filename = pathlib.Path(keys[0][7:])
    for target in (filename, filename.resolve(strict=True)):
        for entry in (target, *target.parents):
            info = entry.stat()
            require(info.st_uid == 0 and not info.st_mode & 0o022,
                    'UNTRUSTED_REPOSITORY', 'Fedora key files must be administrator-owned.')
    # The key is already a distribution file; never import a missing key here.
    config.get_pkg_gpgcheck_option().set(True)
    config.get_skip_if_unavailable_option().set(False)
    config.get_timeout_option().set(5)


class FedoraSession:
    def __init__(self, libdnf5, rpm, host):
        self.lib, self.rpm, self.host = libdnf5, rpm, host
        self.base = libdnf5.base.Base()
        require(hasattr(self.base, 'lock_system_repo'), 'DNF_LOCK_API_UNAVAILABLE',
                'Update native libdnf5/python3-libdnf5 locally to a version providing system-repository locking.')
        self.base.load_config()
        config = self.base.get_config()
        for name, value in [('plugins', False), ('install_weak_deps', False), ('assumeyes', False),
                            ('assumeno', True), ('pkg_gpgcheck', True), ('skip_broken', False),
                            ('skip_unavailable', False), ('tsflags', []), ('installroot', '/')]:
            getattr(config, f'get_{name}_option')().set(value)
        self.base.setup()
        require(self.base.lock_system_repo(), 'PACKAGE_MANAGER_BUSY', 'Another native package transaction is active.')
        self.locked = True

    def close(self):
        self.base.unlock_system_repo()

    def status(self):
        rpmdb = self.rpm.TransactionSet()
        rpmdb.openDB()
        cookie = rpmdb.dbCookie()
        require(isinstance(cookie, str) and cookie, 'PACKAGE_DATABASE_UNHEALTHY', 'The RPM database revision is unavailable.')
        return hashlib.sha256(cookie.encode()).hexdigest()

    def resolve(self, receiver_type, roots=None):
        lib = self.lib
        require(receiver_type in ROOTS, 'UNSUPPORTED_RECEIVER', 'Only UHD and HackRF are qualified.')
        sack = self.base.get_repo_sack()
        sack.create_repos_from_system_configuration()
        arch = self.base.get_vars().get_value('arch')
        for repo in lib.repo.RepoQuery(self.base):
            if repo.get_id() not in ('fedora', 'updates'):
                repo.get_config().get_enabled_option().set(False)
            elif repo.get_config().get_enabled_option().get_value():
                verify_repo(repo, self.host, arch)
        sack.load_repos()
        query = lib.rpm.PackageQuery(self.base); query.filter_name([ROOTS[receiver_type]])
        query.filter_arch([arch, 'noarch'])
        installed = [pkg for pkg in query if pkg.is_installed()]
        available = [pkg for pkg in query if not pkg.is_installed()]
        if roots is None:
            if installed:
                chosen = installed
            else:
                query.filter_available(); query.filter_latest_evr()
                chosen = list(query)
            require(chosen, 'PACKAGE_UNAVAILABLE', 'The native Fedora repository does not provide this receiver package.')
            chosen = sorted(chosen, key=lambda pkg: pkg.get_full_nevra())[0]
            roots = [{'name': ROOTS[receiver_type], 'version': chosen.get_evr()}]
        require(len(roots) == 1 and set(roots[0]) == {'name', 'version'} and roots[0]['name'] == ROOTS[receiver_type],
                'INVALID_TRANSACTION', 'Receiver package is outside the fixed allowlist.')
        if receiver_type == 'Usrp':
            version = re.match(r'(?:[0-9]+:)?([0-9]+)\.([0-9]+)', roots[0]['version'])
            require(version and (int(version[1]), int(version[2])) >= (4, 8),
                    'UHD_VERSION_UNSUPPORTED', 'The backend requires UHD 4.8 or newer.')
        require(not installed or all(pkg.get_evr() == roots[0]['version'] for pkg in installed),
                'INCOMPATIBLE_INSTALLED_VERSION', 'An existing receiver package would be replaced; review locally.')
        goal = lib.base.Goal(self.base)
        if not installed:
            matches = [pkg for pkg in available if pkg.get_evr() == roots[0]['version']]
            require(matches, 'PACKAGE_UNAVAILABLE', 'The exact reviewed receiver package version is unavailable.')
            goal.add_rpm_install(sorted(matches, key=lambda pkg: pkg.get_full_nevra())[0].get_full_nevra())
        self.transaction = goal.resolve()
        require(self.transaction.get_problems() == lib.base.GoalProblem_NO_PROBLEM and
                not self.transaction.get_transaction_groups() and not self.transaction.get_transaction_environments(), 'UNSUPPORTED_TRANSACTION',
                'Dependency resolution failed or includes non-package changes.')
        already = lib.rpm.PackageQuery(self.base); already.filter_installed()
        names = {pkg.get_name() for pkg in already}
        changes = []
        for item in self.transaction.get_transaction_packages():
            pkg = item.get_package()
            require(item.get_action() == lib.transaction.TransactionItemAction_INSTALL and pkg.get_name() not in names,
                    'UNSUPPORTED_TRANSACTION', 'The transaction changes existing packages; only missing dependencies may be installed.')
            verify_repo(pkg.get_repo(), self.host, arch)
            checksum = pkg.get_checksum()
            require(checksum.get_type() == lib.rpm.Checksum.Type_SHA256 and
                    re.fullmatch('[a-fA-F0-9]{64}', checksum.get_checksum()),
                    'MISSING_ARCHIVE_HASH', 'Every package needs its native repository SHA-256 digest.')
            changes.append({'name': pkg.get_name(), 'version': pkg.get_evr(), 'architecture': pkg.get_arch(),
                'sha256': checksum.get_checksum().lower(), 'size': pkg.get_download_size(),
                'origin': 'Fedora', 'archive': pkg.get_repo_id(), 'site': 'mirrors.fedoraproject.org'})
        require(len(changes) <= 64 and sum(pkg['size'] for pkg in changes) <= 512 * 1024 * 1024,
                'TRANSACTION_TOO_LARGE', 'Review transactions above 64 packages or 512 MiB locally.')
        return {'platform': 'fedora:44', 'packages': roots,
                'changes': sorted(changes, key=lambda item: (item['name'], item['architecture'])),
                'statusSha256': self.status()}

    def download_and_verify(self, transaction):
        self.transaction.download()
        signature = self.lib.rpm.RpmSignature(self.base)
        for item in self.transaction.get_transaction_packages():
            pkg = item.get_package()
            require(signature.check_package_signature(pkg) == self.lib.rpm.RpmSignature.CheckResult_OK,
                    'PACKAGE_SIGNATURE_FAILED', 'A package signature is invalid or its native key is missing. No keys are imported automatically.')
            expected = next(value for value in transaction['changes'] if
                value['name'] == pkg.get_name() and value['architecture'] == pkg.get_arch())
            require(archive_digest(pkg.get_package_path()) == expected['sha256'],
                    'PACKAGE_HASH_MISMATCH', 'A downloaded archive differs from the approved digest.')

    def commit(self, transaction):
        require(self.transaction.run() == self.lib.base.Transaction.TransactionRunResult_SUCCESS,
                'PACKAGE_INSTALL_FAILED', 'The reviewed RPM transaction did not complete; inspect native package history locally.')
        # A fresh RPM view, not the pre-install libsolv cache, verifies every item.
        rpmdb = self.rpm.TransactionSet()
        for item in transaction['changes']:
            found = False
            for header in rpmdb.dbMatch('name', item['name']):
                epoch = int(header['epoch'] or 0)
                evr = (f'{epoch}:' if epoch else '') + f"{header['version']}-{header['release']}"
                found = found or (evr == item['version'] and header['arch'] == item['architecture'])
            require(found, 'PACKAGE_POSTCONDITION_FAILED', 'Installation changed the system but its complete postcondition is not verified.')


def archive_digest(filename):
    descriptor = os.open(filename, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as handle:
        info = os.fstat(handle.fileno())
        require(common.stat.S_ISREG(info.st_mode) and info.st_size <= 512 * 1024 * 1024,
                'INVALID_METADATA', 'Expected a bounded regular RPM archive.')
        digest = hashlib.sha256()
        total = 0
        for block in iter(lambda: handle.read(1024 * 1024), b''):
            total += len(block)
            require(total <= 512 * 1024 * 1024, 'INVALID_METADATA', 'RPM archive exceeds its size bound.')
            digest.update(block)
        return digest.hexdigest()


def transact(operation, request, session):
    try:
        action = request.get('action')
        transaction = session.resolve(action['receiverType'] if action else request.get('receiverType'),
                                      action.get('packages') if action else None)
        ready = not transaction['changes']
        if operation == 'plan':
            return {'ok': True, 'ready': ready, 'transaction': transaction}
        require(action and action.get('manager') == 'dnf', 'INVALID_TRANSACTION', 'An approved native DNF action is required.')
        if ready:
            return {'ok': True, 'ready': True, 'transaction': transaction, 'exitCode': 0, 'timedOut': False}
        require(action.get('transaction') == transaction, 'PACKAGE_TRANSACTION_CHANGED',
                'Package state, dependencies or archive digests changed. Review a new installation plan.')
        if operation == 'inspect':
            return {'ok': True, 'ready': False, 'transaction': transaction}
        require(operation == 'execute', 'INVALID_REQUEST', 'Unknown native package operation.')
        session.download_and_verify(transaction)
        require(session.status() == transaction['statusSha256'], 'PACKAGE_TRANSACTION_CHANGED', 'The RPM database changed during download.')
        require(request.get('configRevision') and hashlib.sha256(read_regular(request['configPath'], 262144)).hexdigest() == request['configRevision'],
                'CONFIG_CHANGED', 'Receiver settings changed before installation; no package was installed.')
        session.commit(transaction)
        return {'ok': True, 'ready': True, 'exitCode': 0, 'timedOut': False, 'transaction': transaction}
    finally:
        session.close()


def main():
    require(os.geteuid() == 0 and len(sys.argv) == 3 and sys.argv[1] in ('plan', 'inspect', 'execute') and len(sys.argv[2]) <= 90000,
            'INVALID_REQUEST', 'Use the locally authorized receiver broker for bounded package transactions.')
    host = platform()
    try:
        import libdnf5
        import rpm
    except ImportError:
        raise Refused('DNF_ADAPTER_UNAVAILABLE', 'Install matching native python3-libdnf5 and python3-rpm packages locally, then recheck.')
    request = json.loads(base64.b64decode(sys.argv[2], validate=True))
    with contextlib.redirect_stdout(sys.stderr):
        result = transact(sys.argv[1], request, FedoraSession(libdnf5, rpm, host))
    print(json.dumps(result))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(json.dumps({'ok': False, 'code': getattr(error, 'code', 'PACKAGE_TRANSACTION_FAILED'),
                          'message': str(error) if isinstance(error, Refused) else 'Native DNF transaction could not be verified; inspect the local package manager.'}))
        sys.exit(1)
