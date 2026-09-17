#!/usr/bin/python3 -I
"""Small local receiver broker. Its root-owned policy is the receiver action catalog.

The HTTP server cannot grant itself privileges. Receiver software changes need
an exact, locally authorized short-lived plan. A separate fixed restart request
uses SO_PEERCRED to bind it to the API account. No web password, shell command,
path, unit or package argument crosses this socket.
"""
import argparse
import base64
import fcntl
import hashlib
import json
import os
import pathlib
import pwd
import re
import secrets
import socket
import stat
import struct
import subprocess
import sys
import threading
import time

POLICY = '/etc/vectorwarp-management/receivers.json'
SOCKET = '/run/vectorwarp-receiver/management.sock'
LOCK = '/run/vectorwarp-receiver/operation.lock'
TYPES = ('Kraken', 'RspDuo', 'Usrp', 'HackRF')
MAX_BYTES = 65536
LIFETIME = 300
SERVICE_UNITS = {
    'Kraken': ('vectorwarp-heimdall.service', 'krakensdr-suite-v2.service', 'heimdall-v2.service'),
    'RspDuo': ('vectorwarp-sdrplay.service', 'sdrplay.service', 'sdrplay_apiService.service'),
}


class Refused(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def require(condition, code, message):
    if not condition:
        raise Refused(code, message)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def bounded_read(path, limit=MAX_BYTES):
    with open(path, 'rb') as handle:
        value = handle.read(limit + 1)
    require(len(value) <= limit, 'OVERSIZED_METADATA', 'Receiver metadata exceeds its size limit.')
    return value


def config_revision(path):
    # This directory is API-writable. Never follow a replacement symlink, open
    # a device/FIFO, or allow a special file to block the privileged broker.
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return hashlib.sha256(b'missing').hexdigest()
    with os.fdopen(descriptor, 'rb') as handle:
        info = os.fstat(handle.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= 262144,
                'INVALID_CONFIG_FILE', 'The saved configuration must be a bounded regular file.')
        raw = handle.read(262145)
        require(len(raw) <= 262144, 'INVALID_CONFIG_FILE', 'Saved configuration exceeds its limit.')
        return hashlib.sha256(raw).hexdigest()


def trusted_path(path, file=True):
    """Permit installed release symlinks, verifying every resolved ancestor."""
    original = pathlib.Path(path)
    require(original.is_absolute(), 'UNTRUSTED_PATH', 'Installed policy contains a relative path.')
    # Check both lexical and resolved parents: a writable symlink parent could
    # replace a safe target after this check.
    for target in (original, original.resolve(strict=True)):
        for part in (target, *target.parents):
            info = part.stat()
            require(info.st_uid == 0 and not info.st_mode & 0o022,
                    'UNTRUSTED_PATH', 'Receiver management requires root-owned, non-writable installed files.')
    if file:
        require(stat.S_ISREG(original.stat().st_mode), 'UNTRUSTED_PATH', 'Expected an installed regular file.')
    return original


def command(argv, timeout=5):
    """Fixed policy argv only; bounded output and process group timeout."""
    env = {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LANG': 'C', 'LC_ALL': 'C',
           'DEBIAN_FRONTEND': 'noninteractive'}
    # Output is drained continuously; a noisy child cannot allocate unlimited RAM.
    child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, cwd='/', env=env, start_new_session=True)
    chunks = bytearray()
    overflow = threading.Event()
    def drain():
        while True:
            block = child.stdout.read(4096)
            if not block:
                break
            if len(chunks) + len(block) > MAX_BYTES:
                overflow.set()
            else:
                chunks.extend(block)
    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    timed_out = False
    try:
        child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(child.pid, 15)
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, 9)
            child.wait(timeout=3)
    reader.join(timeout=1)
    if reader.is_alive():
        try:
            os.killpg(child.pid, 9)
        except ProcessLookupError:
            pass
        reader.join(timeout=1)
    require(not reader.is_alive(), 'COMMAND_OUTPUT_STUCK', 'The receiver command did not close its output.')
    require(not overflow.is_set(), 'COMMAND_OUTPUT_LIMIT', 'The receiver command exceeded its output limit.')
    return {'exitCode': child.returncode, 'timedOut': timed_out,
            'output': chunks.decode('utf-8', errors='replace')}


def parse_manifest(path):
    trusted_path(path)
    values = {}
    for line in bounded_read(path).decode().splitlines():
        key, _, value = line.partition('=')
        require(key not in values, 'INVALID_MANIFEST', 'Installed build manifest contains duplicate fields.')
        values[key] = value
    receivers = values.get('compiled_receivers', '').split(',')
    require(receivers and all(item in TYPES for item in receivers) and len(set(receivers)) == len(receivers),
            'INVALID_MANIFEST', 'Installed build manifest must declare the exact live backends.')
    return receivers


def load_policy(path=POLICY):
    trusted_path(path)
    policy = json.loads(bounded_read(path))
    require(set(policy) == {'schemaVersion', 'apiUser', 'configPath', 'artifactManifest', 'actions'} and
            policy['schemaVersion'] == 1 and isinstance(policy['actions'], list) and len(policy['actions']) <= 16,
            'INVALID_POLICY', 'Receiver management policy is invalid.')
    require(policy['apiUser'] == 'vectorwarp-api', 'INVALID_POLICY', 'The receiver API account must be vectorwarp-api.')
    require(pathlib.Path(policy['configPath']).is_absolute(), 'INVALID_POLICY', 'The config path must be absolute.')
    parse_manifest(policy['artifactManifest'])
    seen = set()
    for action in policy['actions']:
        common = {'id', 'receiverType', 'kind', 'review', 'artifactSha256'}
        require(isinstance(action, dict) and action.get('receiverType') in TYPES and
                re.fullmatch(r'[a-z][a-z0-9-]{2,63}', action.get('id', '')) and action['id'] not in seen and
                isinstance(action.get('review'), str) and 1 <= len(action['review']) <= 200 and
                re.fullmatch('[a-f0-9]{64}', action.get('artifactSha256', '')),
                'INVALID_POLICY', 'Every action needs an exact reviewed artifact and unique ID.')
        seen.add(action['id'])
        if action.get('kind') == 'start-service':
            require(set(action) == common | {'unit', 'unitSha256', 'executable', 'executableSha256'} and
                    action['unit'] in SERVICE_UNITS.get(action['receiverType'], ()) and
                    re.fullmatch('[a-f0-9]{64}', action['unitSha256']) and
                    re.fullmatch('[a-f0-9]{64}', action['executableSha256']),
                    'INVALID_POLICY', 'Only an explicitly enrolled allowlisted receiver unit may be started.')
        else:
            transaction_keys = {'transaction'} if 'transaction' in action else set()
            require(action.get('kind') == 'install-packages' and
                    set(action) == common | {'platform', 'manager', 'packages'} | transaction_keys and
                    action['receiverType'] in ('Usrp', 'HackRF') and action['manager'] in ('apt', 'dnf') and
                    isinstance(action['packages'], list) and 1 <= len(action['packages']) <= 12 and
                    re.fullmatch(r'[a-z0-9]+:[a-zA-Z0-9_.-]+', action['platform']),
                    'INVALID_POLICY', 'Only reviewed distribution UHD/HackRF package sets may be installed.')
            if transaction_keys:
                transaction = action['transaction']
                require(action['manager'] in ('apt', 'dnf') and isinstance(transaction, dict) and
                        set(transaction) == {'platform', 'packages', 'changes', 'statusSha256'} and
                        transaction['platform'] == action['platform'] and transaction['packages'] == action['packages'] and
                        isinstance(transaction['changes'], list) and len(transaction['changes']) <= 64 and
                        re.fullmatch('[a-f0-9]{64}', transaction.get('statusSha256', '')),
                        'INVALID_POLICY', 'The complete native transaction and package-database revision must be reviewed.')
            for package in action['packages']:
                require(isinstance(package, dict) and set(package) == {'name', 'version'} and
                        re.fullmatch(r'[a-z0-9][a-z0-9+.-]{0,79}', package['name']) and
                        re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+:~_-]{0,99}', package['version']),
                        'INVALID_POLICY', 'Package names and versions must be exact reviewed values.')
                allowed_package = (re.fullmatch(r'(?:uhd-host|uhd|uhd-tools|libuhd[0-9.]+)', package['name']) if action['receiverType'] == 'Usrp' else
                                   re.fullmatch(r'(?:hackrf|libhackrf[0-9]+|hackrf-libs)', package['name']))
                require(allowed_package, 'INVALID_POLICY', 'Package is outside the fixed UHD/HackRF dependency allowlist.')
                if action['receiverType'] == 'Usrp':
                    version = re.match(r'(?:[0-9]+:)?([0-9]+)\.([0-9]+)', package['version'])
                    require(version and (int(version[1]), int(version[2])) >= (4, 1), 'INVALID_POLICY', 'This backend requires reviewed UHD 4.1 or newer.')
    return policy


def package_adapter(operation, request):
    manager = request.get('action', {}).get('manager')
    if manager is None:
        values = dict(line.split('=', 1) for line in bounded_read('/etc/os-release').decode().splitlines() if '=' in line)
        manager = 'dnf' if values.get('ID', '').strip('"') == 'fedora' else 'apt'
    require(manager in ('apt', 'dnf'), 'UNSUPPORTED_PLATFORM', 'Only qualified native package managers are available.')
    executable = trusted_path(pathlib.Path(__file__).with_name(f'vectorwarp-receiver-{manager}.py'))
    encoded = base64.b64encode(json.dumps(request, separators=(',', ':')).encode()).decode()
    require(len(encoded) <= 90000, 'INVALID_TRANSACTION', 'The package plan exceeds its size limit.')
    result = command(['/usr/bin/python3', '-I', str(executable), operation, encoded],
                     timeout=180 if operation == 'execute' else 15)
    require(not result['timedOut'], 'PACKAGE_TRANSACTION_TIMEOUT',
            'Package operation timed out. Its grant is consumed; inspect the local package manager before retrying.')
    try:
        receipt = json.loads(result['output'].strip().splitlines()[-1])
    except (ValueError, IndexError):
        raise Refused('PACKAGE_TRANSACTION_FAILED', 'The package adapter did not return a valid receipt.')
    require(receipt.get('ok') and result['exitCode'] == 0,
            receipt.get('code', 'PACKAGE_TRANSACTION_FAILED'), receipt.get('message', 'The package operation failed.'))
    return {**receipt, 'manager': manager}


class Inspector:
    def __init__(self, policy, run=command):
        self.policy, self.run = policy, run

    def inspect(self, action):
        manifest = self.policy['artifactManifest']
        require(action['receiverType'] in parse_manifest(manifest), 'BACKEND_NOT_COMPILED',
                'Install a VectorWarp build containing this backend before its receiver dependencies.')
        require(hashlib.sha256(bounded_read(manifest)).hexdigest() == action['artifactSha256'],
                'ARTIFACT_CHANGED', 'The installed VectorWarp build differs from the reviewed action.')
        if action['kind'] == 'start-service':
            unit = self.run(['/usr/bin/systemctl', 'show', '--no-pager',
                             '--property=LoadState,ActiveState,FragmentPath,DropInPaths,NeedDaemonReload', '--', action['unit']])
            require(unit['exitCode'] == 0 and not unit['timedOut'], 'SERVICE_UNAVAILABLE', 'The owned receiver service could not be inspected.')
            props = dict(line.split('=', 1) for line in unit['output'].splitlines() if '=' in line)
            require(props.get('LoadState') == 'loaded' and not props.get('DropInPaths') and
                    props.get('NeedDaemonReload') == 'no', 'SERVICE_POLICY_MISMATCH',
                    'The owned receiver unit is absent, has unreviewed overrides, or systemd has not loaded its current definition.')
            unit_path = trusted_path(props.get('FragmentPath', ''))
            executable = trusted_path(action['executable'])
            require(hashlib.sha256(bounded_read(unit_path)).hexdigest() == action['unitSha256'] and
                    hashlib.sha256(bounded_read(executable, 128 * 1024 * 1024)).hexdigest() == action['executableSha256'],
                    'SERVICE_POLICY_MISMATCH', 'The receiver unit or executable changed since review.')
            require(props.get('ActiveState') in ('active', 'inactive', 'failed'), 'SERVICE_BUSY',
                    'The receiver service is changing state. Recheck after it settles.')
            return {'ready': props['ActiveState'] == 'active', 'state': props['ActiveState'],
                    'unitSha256': action['unitSha256'], 'executableSha256': action['executableSha256']}
        if action.get('transaction'):
            return package_adapter('inspect', {'action': action})
        platform = {}
        for line in bounded_read('/etc/os-release').decode().splitlines():
            key, _, value = line.partition('=')
            platform[key] = value.strip('"')
        require(f"{platform.get('ID')}:{platform.get('VERSION_ID')}" == action['platform'],
                'UNSUPPORTED_PLATFORM', 'This reviewed package set belongs to another operating system.')
        versions = []
        for item in action['packages']:
            if action['manager'] == 'apt':
                result = self.run(['/usr/bin/dpkg-query', '-W', '-f=${db:Status-Abbrev}\t${Version}', item['name']])
                version = result['output'][4:].strip() if result['exitCode'] == 0 and result['output'].startswith('ii \t') else None
                missing = result['exitCode'] == 1 and 'no packages found matching' in result['output']
            else:
                result = self.run(['/usr/bin/rpm', '-q', '--qf', '%{VERSION}-%{RELEASE}', item['name']])
                version = result['output'].strip() if result['exitCode'] == 0 else None
                missing = result['exitCode'] == 1 and f"package {item['name']} is not installed" in result['output']
            require(not result['timedOut'], 'PACKAGE_STATUS_UNKNOWN', 'Package inspection timed out.')
            require(version is not None or missing, 'PACKAGE_STATUS_UNKNOWN',
                    'The package database did not establish an installed or missing state.')
            require(version is None or version == item['version'], 'INCOMPATIBLE_INSTALLED_VERSION',
                    'A different installed package version must be reviewed locally; it will not be replaced.')
            versions.append(version)
        require(all(versions), 'INSTALL_TRANSACTION_REVIEW_REQUIRED',
                'Dependency installation is unavailable until the complete signed package transaction, including transitive dependencies, has been reviewed and locked. Install the required SDK locally, then recheck.')
        return {'ready': True, 'versions': versions}

    def execute(self, action, revision=None):
        if action['kind'] == 'start-service':
            return self.run(['/usr/bin/systemctl', 'start', '--', action['unit']], timeout=90)
        if action.get('manager') in ('apt', 'dnf') and action.get('transaction'):
            return package_adapter('execute', {'action': action,
                'configPath': self.policy['configPath'], 'configRevision': revision})
        raise Refused('INSTALL_TRANSACTION_REVIEW_REQUIRED',
                      'Package mutation is disabled until a complete reviewed transaction and package-manager lock adapter is available.')


class Broker:
    def __init__(self, policy, api_uid, inspector=None, now=time.monotonic, revision=None, policy_loader=None):
        self.policy, self.api_uid, self.now = policy, api_uid, now
        self.inspector = inspector or Inspector(policy)
        self.revision = revision or (lambda: config_revision(self.policy['configPath']))
        self.plans, self.guard, self.operation = {}, threading.Lock(), threading.Lock()
        self.policy_loader = policy_loader

    def handle(self, request, uid):
        require(uid in (0, self.api_uid), 'UNAUTHORIZED_PEER', 'This OS account cannot access receiver management.')
        if self.policy_loader:
            with self.guard:
                updated = self.policy_loader()
                if updated != self.policy:
                    require(not self.operation.locked(), 'MANAGEMENT_BUSY', 'Wait for the current receiver operation before loading a changed policy.')
                    self.policy = updated
                    self.inspector.policy = updated
                    self.plans.clear()
        require(isinstance(request, dict), 'INVALID_REQUEST', 'Expected a bounded JSON object.')
        verb = request.get('verb')
        allowed = {'discover': {'verb'}, 'plan': {'verb', 'actionId', 'configRevision'},
                   'authorize': {'verb', 'planId'}, 'describe': {'verb', 'planId'},
                   'execute': {'verb', 'planId', 'configRevision'},
                   'restart': {'verb'}, 'gpu-access': {'verb'}}
        require(verb in allowed and set(request) == allowed[verb], 'INVALID_REQUEST', 'Unknown receiver request fields.')
        if verb == 'gpu-access':
            # Metadata-only host view for the API's PrivateDevices sandbox.
            # No driver is loaded and no caller-supplied path/action is accepted.
            helper = trusted_path(pathlib.Path(__file__).resolve().with_name('vectorwarp-gpu-setup'))
            result = self.inspector.run(['/usr/bin/python3', '-I', str(helper), '--access-status', '--json'], timeout=1)
            require(result['exitCode'] == 0 and not result['timedOut'], 'GPU_ACCESS_UNAVAILABLE', 'GPU service-access check failed.')
            access = json.loads(result['output'])
            require(isinstance(access, dict) and isinstance(access.get('nodes'), list) and len(access['nodes']) <= 16,
                    'GPU_ACCESS_UNAVAILABLE', 'Invalid GPU service-access response.')
            return {'ok': True, 'serviceAccess': access}
        if verb == 'restart':
            # The HTTP path enforces save, origin and receiver checks. The broker
            # binds this fixed unit start to the unprivileged API OS account;
            # no command, path or unit comes from the socket request.
            require(uid == self.api_uid, 'UNAUTHORIZED_PEER', 'Only the VectorWarp API account may request a restart.')
            require(self.operation.acquire(blocking=False), 'MANAGEMENT_BUSY', 'Another receiver operation is running.')
            try:
                result = self.inspector.run(['/usr/bin/systemctl', '--no-block', 'start',
                                             'vectorwarp-restart.service'], timeout=5)
                require(result['exitCode'] == 0 and not result['timedOut'], 'RESTART_REQUEST_FAILED',
                        'The service manager did not accept the VectorWarp restart request. Check the receiver helper log.')
                return {'ok': True, 'status': 'accepted',
                        'message': 'Restart request accepted by the service manager; wait for fresh radar status.'}
            finally:
                self.operation.release()
        if verb == 'discover':
            entries = []
            for action in self.policy['actions']:
                try:
                    state = self.inspector.inspect(action)
                    entries.append({'id': action['id'], 'receiverType': action['receiverType'], 'kind': action['kind'],
                                    'ready': state['ready'], 'available': True, 'review': action['review']})
                except (Refused, OSError) as error:
                    entries.append({'id': action['id'], 'receiverType': action['receiverType'], 'kind': action['kind'],
                                    'available': False, 'code': getattr(error, 'code', 'INSPECTION_FAILED'),
                                    'message': str(error) if isinstance(error, Refused) else 'Installed receiver metadata is unavailable.'})
            return {'ok': True, 'actions': entries, 'authorization': 'local-one-use', 'lifetimeSeconds': LIFETIME}
        with self.guard:
            self.plans = {key: value for key, value in self.plans.items() if value['deadline'] > self.now()}
            if verb == 'plan':
                require(len(self.plans) < 64, 'TOO_MANY_PLANS', 'Too many pending plans; wait for them to expire.')
                require(request['configRevision'] == self.revision(), 'CONFIG_CHANGED', 'Saved settings changed. Review a fresh plan.')
                action = next((item for item in self.policy['actions'] if item['id'] == request['actionId']), None)
                require(action is not None, 'ACTION_NOT_REVIEWED', 'No installed reviewed action matches this request.')
                observed = self.inspector.inspect(action)
                if observed['ready']:
                    return {'ok': True, 'status': 'not-required', 'message': 'The reviewed software or service is already available.'}
                plan_id = secrets.token_hex(32)
                plan = {'action': action, 'observed': digest(observed), 'revision': request['configRevision'],
                        'deadline': self.now() + LIFETIME, 'authorized': False, 'owner': uid}
                self.plans[plan_id] = plan
                return {'ok': True, 'status': 'awaiting-local-authorization', 'planId': plan_id,
                        'actionId': action['id'], 'receiverType': action['receiverType'], 'kind': action['kind'],
                        'review': action['review'], 'lifetimeSeconds': LIFETIME,
                        **({'transaction': action['transaction']} if action.get('transaction') else {})}
            require(isinstance(request.get('planId'), str) and re.fullmatch('[a-f0-9]{64}', request['planId']),
                    'INVALID_REQUEST', 'A valid receiver plan ID is required.')
            plan = self.plans.get(request['planId'])
            require(plan is not None, 'PLAN_EXPIRED', 'Receiver plan expired or was consumed. Review a fresh plan.')
            require(uid == 0 or uid == plan['owner'], 'UNAUTHORIZED_PEER', 'The plan belongs to another API account.')
            if verb in ('describe', 'authorize'):
                require(uid == 0, 'LOCAL_AUTHORIZATION_REQUIRED', 'Authorize this exact plan from a local administrator terminal.')
                require(plan['revision'] == self.revision(), 'CONFIG_CHANGED', 'Saved settings changed. Review a fresh plan.')
                require(digest(self.inspector.inspect(plan['action'])) == plan['observed'], 'STATUS_CHANGED',
                        'Receiver state changed. Review a fresh plan.')
                if verb == 'authorize':
                    plan['authorized'] = True
                return {'ok': True, 'authorized': plan['authorized'], 'action': plan['action'],
                        'configRevision': plan['revision'], 'remainingSeconds': max(0, int(plan['deadline'] - self.now()))}
            require(plan['authorized'], 'LOCAL_AUTHORIZATION_REQUIRED', 'This plan needs one-use local administrator authorization.')
            # Consume before any asynchronous/privileged operation, including a
            # failed precondition. No retry can silently repeat a partial action.
            del self.plans[request['planId']]
            require(self.operation.acquire(blocking=False), 'MANAGEMENT_BUSY', 'Another receiver operation is running.')
        try:
            require(request['configRevision'] == plan['revision'] == self.revision(), 'CONFIG_CHANGED', 'Saved settings changed; no action was started.')
            require(digest(self.inspector.inspect(plan['action'])) == plan['observed'], 'STATUS_CHANGED', 'Receiver state changed; no action was started.')
            result = self.inspector.execute(plan['action'], revision=plan['revision'])
            try:
                after = self.inspector.inspect(plan['action'])
                complete = result['exitCode'] == 0 and not result['timedOut'] and after['ready'] and self.revision() == plan['revision']
            except (Refused, OSError):
                complete = False
            return {'ok': complete, 'status': 'complete' if complete else 'failed',
                    'actionId': plan['action']['id'], 'commandExitCode': result['exitCode'],
                    'timedOut': result['timedOut'], 'postconditionVerified': complete,
                    'message': 'Reviewed software/service postcondition verified. Recheck receiver telemetry before capture.' if complete else
                    'Receiver action did not pass its postcondition. State may have changed; inspect before creating another plan.'}
        finally:
            self.operation.release()


def exchange(request):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(10)
        client.connect(SOCKET)
        client.sendall(json.dumps(request).encode() + b'\n')
        raw = client.makefile('rb').readline(MAX_BYTES + 1)
        require(len(raw) <= MAX_BYTES, 'OVERSIZED_RESPONSE', 'Receiver response is too large.')
        return json.loads(raw)


def serve_client(connection, broker, timeout=5):
    try:
        connection.settimeout(timeout)
        _, uid, _ = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i')))
        try:
            raw = connection.makefile('rb').readline(MAX_BYTES + 1)
            require(len(raw) <= MAX_BYTES and raw.endswith(b'\n'), 'INVALID_REQUEST', 'Request is oversized or incomplete.')
            result = broker.handle(json.loads(raw), uid)
        except Refused as error:
            result = {'ok': False, 'code': error.code, 'message': str(error)}
        except (ValueError, UnicodeDecodeError):
            result = {'ok': False, 'code': 'INVALID_REQUEST', 'message': 'Request is not valid JSON.'}
        except TimeoutError:
            result = {'ok': False, 'code': 'REQUEST_TIMEOUT', 'message': 'The local request did not finish before its deadline.'}
        except Exception:
            result = {'ok': False, 'code': 'MANAGEMENT_FAILED', 'message': 'Receiver management failed. Inspect the local service log.'}
        connection.sendall(json.dumps(result).encode() + b'\n')
        # Never log tokens, serials or command output.
        print(json.dumps({'event': 'receiver-management', 'uid': uid, 'status': result.get('status'),
                          'code': result.get('code'), 'actionId': result.get('actionId')}), flush=True)
    except OSError:
        pass
    finally:
        connection.close()


def serve():
    require(os.geteuid() == 0, 'ROOT_REQUIRED', 'The receiver broker must run under its installed root service.')
    policy = load_policy()
    api_uid = pwd.getpwnam(policy['apiUser']).pw_uid
    require(api_uid != 0, 'INVALID_ACCOUNT', 'The receiver API account must be unprivileged.')
    # A single process owns the OS lease for its lifetime; another broker cannot
    # race the in-memory operation lock or reuse the socket.
    lock = open(LOCK, 'a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    require(os.environ.get('LISTEN_PID') == str(os.getpid()) and os.environ.get('LISTEN_FDS') == '1',
            'SOCKET_ACTIVATION_REQUIRED', 'Start the installed socket-activated receiver service.')
    broker = Broker(policy, api_uid, policy_loader=load_policy)
    listener = socket.socket(fileno=3)
    workers = threading.BoundedSemaphore(8)
    def client(connection):
        try:
            serve_client(connection, broker)
        finally:
            workers.release()
    while True:
        connection, _ = listener.accept()
        if not workers.acquire(blocking=False):
            connection.close()
        else:
            threading.Thread(target=client, args=(connection,), daemon=True).start()


def enrolled_service(policy, receiver_type, unit, run=command):
    """Local administrator enrollment only; never a browser-supplied unit."""
    if unit is None and receiver_type in SERVICE_UNITS:
        result = run(['/usr/bin/systemctl', 'show', '--no-pager',
                      '--property=Id,LoadState', '--', *SERVICE_UNITS[receiver_type]])
        require(result['exitCode'] == 0 and not result['timedOut'], 'SERVICE_UNAVAILABLE', 'Installed receiver services could not be inspected.')
        loaded = []
        for section in re.split(r'\n\s*\n', result['output'].strip()):
            props = dict(line.split('=', 1) for line in section.splitlines() if '=' in line)
            if props.get('LoadState') == 'loaded' and props.get('Id') in SERVICE_UNITS[receiver_type]:
                loaded.append(props['Id'])
        require(len(loaded) == 1, 'SERVICE_SELECTION_REQUIRED',
                'Expected exactly one installed receiver service. Specify its allowlisted unit name explicitly: ' + ', '.join(SERVICE_UNITS[receiver_type]))
        unit = loaded[0]
    require(unit in SERVICE_UNITS.get(receiver_type, ()), 'SERVICE_NOT_ALLOWLISTED',
            'Select an installed supported Kraken Suite or SDRplay service.')
    require(receiver_type in parse_manifest(policy['artifactManifest']), 'BACKEND_NOT_COMPILED',
            'Install a build containing this receiver before enrolling its service.')
    result = run(['/usr/bin/systemctl', 'show', '--no-pager',
                  '--property=LoadState,ActiveState,FragmentPath,DropInPaths,NeedDaemonReload,ExecStart', '--', unit])
    require(result['exitCode'] == 0 and not result['timedOut'], 'SERVICE_UNAVAILABLE', 'The installed service could not be inspected.')
    props = dict(line.split('=', 1) for line in result['output'].splitlines() if '=' in line)
    require(props.get('LoadState') == 'loaded' and not props.get('DropInPaths') and
            props.get('NeedDaemonReload') == 'no', 'SERVICE_POLICY_MISMATCH',
            'Enroll a loaded service without overrides or pending definition changes.')
    paths = re.findall(r'(?:^|[ {])path=([^ ;}]+)', props.get('ExecStart', ''))
    require(len(paths) == 1 and re.fullmatch(r'/[A-Za-z0-9_./+-]+', paths[0]),
            'SERVICE_POLICY_MISMATCH', 'The service must have one inspectable installed executable.')
    fragment, executable = trusted_path(props.get('FragmentPath', '')), trusted_path(paths[0])
    unit_raw = bounded_read(fragment)
    action = {'id': f'start-{receiver_type.lower()}', 'receiverType': receiver_type,
              'kind': 'start-service', 'unit': unit,
              'review': f'Start the explicitly enrolled existing {unit}; preserve its installed settings.',
              'artifactSha256': hashlib.sha256(bounded_read(policy['artifactManifest'])).hexdigest(),
              'unitSha256': hashlib.sha256(unit_raw).hexdigest(), 'executable': str(executable),
              'executableSha256': hashlib.sha256(bounded_read(executable, 128 * 1024 * 1024)).hexdigest()}
    return action, unit_raw.decode()


def save_enrollment(policy, action, path=POLICY):
    require(load_policy(path) == policy, 'POLICY_CHANGED', 'Receiver policy changed during review; start again.')
    updated = dict(policy, actions=[item for item in policy['actions'] if item['id'] != action['id']] + [action])
    require(len(updated['actions']) <= 16, 'INVALID_POLICY', 'Too many enrolled actions.')
    temporary = f'{path}.{secrets.token_hex(12)}.tmp'
    try:
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'w') as handle:
            json.dump(updated, handle, indent=2); handle.write('\n'); handle.flush(); os.fsync(handle.fileno())
        os.replace(temporary, path)
        descriptor = os.open(str(pathlib.Path(path).parent), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('serve', 'request-restart', 'authorize', 'enroll-service', 'enroll-packages'))
    parser.add_argument('plan', nargs='?')
    parser.add_argument('unit', nargs='?')
    args = parser.parse_args()
    if args.operation == 'serve':
        return serve()
    if args.operation == 'request-restart':
        require(args.plan is None and args.unit is None, 'INVALID_REQUEST', 'Restart accepts no arguments.')
        result = exchange({'verb': 'restart'})
        require(result.get('ok') is True and result.get('status') == 'accepted',
                result.get('code', 'RESTART_REQUEST_FAILED'),
                result.get('message', 'The restart request was not accepted.'))
        print(result['message'])
        return
    require(os.geteuid() == 0 and sys.stdin.isatty(), 'LOCAL_AUTHORIZATION_REQUIRED',
            'Use a local administrator terminal to authorize a receiver plan.')
    if args.operation == 'enroll-packages':
        policy = load_policy()
        require(args.plan in ('HackRF', 'Usrp') and args.plan in parse_manifest(policy['artifactManifest']),
                'BACKEND_NOT_COMPILED', 'Enroll dependencies only for an installed live UHD/HackRF backend.')
        result = package_adapter('plan', {'receiverType': args.plan})
        if result['ready']:
            print('The required native package is already installed. Recheck receiver software in Settings.')
            return
        transaction = result['transaction']
        action = {'id': f'install-{args.plan.lower()}', 'receiverType': args.plan, 'kind': 'install-packages',
                  'review': f"Install {len(transaction['changes'])} reviewed missing packages from signed native distribution repositories; no upgrades or removals.",
                  'artifactSha256': hashlib.sha256(bounded_read(policy['artifactManifest'])).hexdigest(),
                  'platform': transaction['platform'], 'manager': result['manager'],
                  'packages': transaction['packages'], 'transaction': transaction}
        print(json.dumps(action, indent=2))
        require(input('Enroll this exact package transaction for one-use approval in Receiver setup? Type yes: ') == 'yes',
                'CANCELLED', 'No package transaction was enrolled.')
        require(package_adapter('plan', {'receiverType': args.plan})['transaction'] == transaction,
                'PACKAGE_TRANSACTION_CHANGED', 'The package transaction changed during review; start again.')
        save_enrollment(policy, action)
        print('Enrolled. No packages were installed. Return to Receiver setup, review the package list and authorize its one-use installation.')
        return
    if args.operation == 'enroll-service':
        policy = load_policy()
        action, definition = enrolled_service(policy, args.plan, args.unit)
        print(definition)
        print(json.dumps(action, indent=2))
        require(input('Enroll this exact existing service for one-use starts from Receiver setup? Type yes: ') == 'yes',
                'CANCELLED', 'No service was enrolled.')
        checked, _ = enrolled_service(policy, args.plan, args.unit)
        require(checked == action, 'SERVICE_POLICY_MISMATCH', 'Service artifacts changed during review; start again.')
        save_enrollment(policy, action)
        print('Enrolled. No receiver service was started or reconfigured. Return to Receiver setup and Check receiver software.')
        return
    result = exchange({'verb': 'describe', 'planId': args.plan})
    require(result.get('ok'), result.get('code'), result.get('message'))
    print(json.dumps(result, indent=2))
    require(input('Authorize this exact one-use receiver action? Type yes: ') == 'yes', 'CANCELLED', 'No action was authorized.')
    result = exchange({'verb': 'authorize', 'planId': args.plan})
    require(result.get('ok'), result.get('code'), result.get('message'))
    print('Authorized once. Return to Receiver setup and choose Run authorized action before this plan expires.')


if __name__ == '__main__':
    try:
        main()
    except (Refused, OSError, ValueError) as error:
        print(f'receiver-helper: {error}', file=sys.stderr)
        sys.exit(1)
