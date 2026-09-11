#!/usr/bin/python3 -I
"""Qualified add-only Ubuntu/Debian SDR dependency transactions.

No shell, repository updates, upgrades, removals or arbitrary package requests.
Each resolver result, native signed origin, archive hash and installed-package
database revision must match the locally approved transaction under APT's lock.
"""
import base64
import contextlib
import hashlib
import json
import os
import pathlib
import re
import stat
import sys

ROOTS = {'HackRF': 'hackrf', 'Usrp': 'uhd-host'}
NATIVE_SITES = {
    'ubuntu': {'archive.ubuntu.com', 'security.ubuntu.com', 'ports.ubuntu.com'},
    'debian': {'deb.debian.org', 'security.debian.org'},
}


class Refused(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def require(value, code, message):
    if not value:
        raise Refused(code, message)


def read_regular(filename, limit):
    descriptor = os.open(filename, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as handle:
        info = os.fstat(handle.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= limit, 'INVALID_METADATA', 'Expected bounded regular package metadata.')
        result = handle.read(limit + 1)
        require(len(result) <= limit, 'INVALID_METADATA', 'Package metadata exceeds its bound.')
        return result


def platform():
    values = {}
    for line in pathlib.Path('/etc/os-release').read_text().splitlines():
        key, _, value = line.partition('=')
        values[key] = value.strip('"')
    distribution = values.get('ID')
    # DragonOS is accepted only as an Ubuntu derivative. resolve() still checks
    # every archive against authenticated Ubuntu origins, never derivative PPAs.
    if distribution == 'dragonos' and 'ubuntu' in values.get('ID_LIKE', '').split():
        distribution = 'ubuntu'
    require(distribution in NATIVE_SITES, 'UNSUPPORTED_PLATFORM',
            'Automatic dependency installation currently qualifies native Ubuntu/Debian APT only.')
    return {'id': distribution, 'version': values.get('VERSION_ID', ''), 'distribution': values.get('ID')}


def verify_sources():
    files = [pathlib.Path('/etc/apt/sources.list')]
    directory = pathlib.Path('/etc/apt/sources.list.d')
    if directory.exists():
        files.extend(item for item in directory.iterdir() if item.suffix in ('.list', '.sources'))
    require(len(files) <= 64, 'UNSUPPORTED_SOURCES', 'Too many APT source files for bounded review.')
    for filename in files:
        if not filename.exists():
            continue
        for ancestor in (filename, *filename.parents):
            info = ancestor.stat()
            require(info.st_uid == 0 and not info.st_mode & 0o022, 'UNTRUSTED_SOURCES', 'APT sources must be administrator-owned.')
        text = read_regular(filename, 262144).decode()
        active = '\n'.join(line.split('#', 1)[0] for line in text.splitlines())
        require(not re.search(r'(?im)\b(?:trusted|allow-insecure|allow-weak|allow-downgrade-to-insecure)\s*(?:=|:)\s*(?:yes|true|1)\b', active),
                'UNTRUSTED_SOURCES', 'Unsigned or weakly trusted APT source overrides are not qualified.')


def configure(apt_pkg):
    apt_pkg.init_config()
    for key in ('APT::Get::AllowUnauthenticated', 'Acquire::AllowInsecureRepositories',
                'Acquire::AllowDowngradeToInsecureRepositories', 'Acquire::AllowWeakRepositories',
                'APT::Install-Recommends', 'APT::Install-Suggests'):
        apt_pkg.config.set(key, 'false')
    apt_pkg.config.set('Acquire::ForceHash', 'sha256')
    apt_pkg.config.set('DPkg::Lock::Timeout', '0')
    apt_pkg.init_system()


def require_uhd_version(value):
    # Actual capture source is compile-qualified against native Ubuntu UHD4.1.
    version = re.match(r'(?:[0-9]+:)?([0-9]+)\.([0-9]+)', value)
    require(version and (int(version[1]), int(version[2])) >= (4, 1),
            'UHD_VERSION_UNSUPPORTED', 'This VectorWarp backend requires UHD 4.1 or newer.')


def resolve(cache, receiver_type, host, roots=None):
    require(receiver_type in ROOTS, 'UNSUPPORTED_RECEIVER', 'Only UHD and HackRF distro dependencies are qualified.')
    require(not cache.broken_count and not cache.dpkg_journal_dirty, 'PACKAGE_DATABASE_UNHEALTHY',
            'Repair interrupted or broken packages locally before requesting receiver installation.')
    root_name = ROOTS[receiver_type]
    require(root_name in cache, 'PACKAGE_UNAVAILABLE', 'The native distribution does not provide this receiver package.')
    if roots is None:
        candidate = cache[root_name].installed or cache[root_name].candidate
        require(candidate is not None, 'PACKAGE_UNAVAILABLE', 'No receiver package candidate is available.')
        roots = [{'name': root_name, 'version': candidate.version}]
    require(len(roots) == 1 and roots[0].get('name') == root_name and set(roots[0]) == {'name', 'version'},
            'INVALID_TRANSACTION', 'The requested package is outside the fixed receiver allowlist.')
    for root in roots:
        package = cache[root['name']]
        versions = [value for value in package.versions if value.version == root['version']]
        require(len(versions) == 1, 'PACKAGE_UNAVAILABLE', 'The exact approved receiver package version is unavailable.')
        if receiver_type == 'Usrp':
            require_uhd_version(root['version'])
        package.candidate = versions[0]
        package.mark_install(auto_fix=False, auto_inst=True)
    require(not cache.broken_count and not cache.delete_count, 'UNSUPPORTED_TRANSACTION', 'The proposed dependency transaction is broken or removes packages.')
    changes = []
    for package in cache.get_changes():
        require(not package.is_installed and package.marked_install and not package.marked_delete,
                'UNSUPPORTED_TRANSACTION', 'Existing packages would change. Review those changes locally; automatic setup installs missing packages only.')
        candidate = package.candidate
        origins = [origin for origin in candidate.origins if origin.trusted and
                   origin.origin == host['id'].capitalize() and origin.site in NATIVE_SITES[host['id']]]
        require(origins and all(origin.trusted and origin.origin == host['id'].capitalize() and
                origin.site in NATIVE_SITES[host['id']] for origin in candidate.origins if origin.site),
                'UNTRUSTED_PACKAGE', 'Every package must come only from the signed native distribution repositories.')
        require(re.fullmatch('[a-fA-F0-9]{64}', candidate.sha256 or ''),
                'MISSING_ARCHIVE_HASH', 'Every package archive needs an authenticated SHA-256 digest.')
        origin = sorted(origins, key=lambda value: (value.site, value.archive))[0]
        changes.append({'name': package.name, 'version': candidate.version,
                        'architecture': candidate.architecture, 'sha256': candidate.sha256.lower(),
                        'size': candidate.size, 'origin': origin.origin, 'archive': origin.archive, 'site': origin.site})
    require(len(changes) <= 64 and sum(value['size'] for value in changes) <= 512 * 1024 * 1024,
            'TRANSACTION_TOO_LARGE', 'This transaction exceeds 64 packages or 512 MiB of downloads; review it locally.')
    return {'platform': f"{host['id']}:{host['version']}", 'packages': roots,
            'changes': sorted(changes, key=lambda value: value['name'])}


def transact(operation, request, apt, apt_pkg, host=None, status_digest=None, check_sources=verify_sources):
    host = host or platform()
    check_sources()
    configure(apt_pkg)
    status_digest = status_digest or (lambda: hashlib.sha256(read_regular('/var/lib/dpkg/status', 32 * 1024 * 1024)).hexdigest())
    # Hold the frontend lock from opening the cache through the final comparison
    # and commit. python-apt releases only the inner dpkg lock when invoking dpkg.
    with apt_pkg.SystemLock():
        cache = apt.Cache()
        action = request.get('action')
        receiver_type = action['receiverType'] if action else request.get('receiverType')
        transaction = resolve(cache, receiver_type, host, action.get('packages') if action else None)
        transaction['statusSha256'] = status_digest()
        ready = not transaction['changes']
        if operation == 'plan':
            return {'ok': True, 'ready': ready, 'transaction': transaction}
        require(action and action.get('manager') == 'apt', 'INVALID_TRANSACTION', 'An approved APT action is required.')
        if ready:
            return {'ok': True, 'ready': True, 'transaction': transaction, 'exitCode': 0, 'timedOut': False}
        require(action.get('transaction') == transaction, 'PACKAGE_TRANSACTION_CHANGED',
                'The package database, archive hashes or resolved dependencies changed. Review a new installation plan.')
        if operation == 'inspect':
            return {'ok': True, 'ready': False, 'transaction': transaction}
        require(operation == 'execute', 'INVALID_REQUEST', 'Unknown package operation.')
        expected = request.get('configRevision')
        require(expected and hashlib.sha256(read_regular(request['configPath'], 262144)).hexdigest() == expected,
                'CONFIG_CHANGED', 'Receiver settings changed before package installation; no package was installed.')
        require(cache.commit(allow_unauthenticated=False), 'PACKAGE_INSTALL_FAILED', 'The approved package transaction did not complete.')
        cache.open(None)
        require(not cache.broken_count and not cache.dpkg_journal_dirty and all(
            cache[item['name']].installed and cache[item['name']].installed.version == item['version']
            for item in transaction['changes']), 'PACKAGE_POSTCONDITION_FAILED',
            'Package installation changed the system but its complete postcondition was not verified. Inspect the package manager locally.')
        return {'ok': True, 'ready': True, 'exitCode': 0, 'timedOut': False, 'transaction': transaction}


def main():
    require(os.geteuid() == 0, 'ROOT_REQUIRED', 'The package transaction adapter must run through local receiver authorization.')
    require(len(sys.argv) == 3 and sys.argv[1] in ('plan', 'inspect', 'execute') and len(sys.argv[2]) <= 90000,
            'INVALID_REQUEST', 'Expected a bounded receiver package transaction.')
    try:
        import apt
        import apt_pkg
    except ImportError:
        raise Refused('APT_ADAPTER_UNAVAILABLE', 'Install the distribution python3-apt package locally to enable reviewed native dependency installation.')
    request = json.loads(base64.b64decode(sys.argv[2], validate=True))
    with contextlib.redirect_stdout(sys.stderr):
        result = transact(sys.argv[1], request, apt, apt_pkg)
    print(json.dumps(result))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(json.dumps({'ok': False, 'code': getattr(error, 'code', 'PACKAGE_TRANSACTION_FAILED'),
                          'message': str(error) if isinstance(error, Refused) else 'Package transaction could not be verified; inspect the local package manager.'}))
        sys.exit(1)
