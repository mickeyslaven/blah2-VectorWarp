#!/usr/bin/env python3
"""User-local macOS lifecycle; no privileged services or receiver management."""
import contextlib
import ctypes
import fcntl
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request

RECEIVERS = {'Kraken', 'Usrp', 'HackRF', 'RspDuo'}
HELP = '''Usage: vectorwarp-macos [open|start|stop|restart|restart-processing|start-kraken|stop-kraken|supervise|status|logs|help]
  open (default)     Start web settings only and open the browser.
  start              Start web settings and the configured processor.
  stop               Stop this instance's processor and web service.
  restart            Restart both processes, loading the saved configuration.
  restart-processing Restart only the processor for diagnosis.
  start-kraken       Start the saved local Kraken controller only.
  stop-kraken        Stop this instance's Kraken controller only.
  supervise          Foreground service target; keep the owned web and processor pair running.
  status / logs      Inspect this instance without creating or changing state.
State defaults to ~/Library/Application Support/VectorWarp. No login item or
vendor service is installed, started or stopped.'''


class BsdInfo(ctypes.Structure):
    # Apple's public sys/proc_info.h, PROC_PIDTBSDINFO. Unlike ps lstart,
    # microsecond birth time distinguishes a reused PID in the same second.
    _fields_ = [(name, ctypes.c_uint32) for name in (
        'flags', 'status', 'xstatus', 'pid', 'ppid', 'uid', 'gid', 'ruid',
        'rgid', 'svuid', 'svgid', 'reserved')]
    _fields_ += [('comm', ctypes.c_char * 16), ('name', ctypes.c_char * 32)]
    _fields_ += [(name, ctypes.c_uint32) for name in (
        'nfiles', 'pgid', 'jobc', 'tdev', 'tpgid', 'nice')]
    _fields_ += [('seconds', ctypes.c_uint64), ('microseconds', ctypes.c_uint64)]


def process_identity(pid):
    """Read birth time and exact argv for one process; never retain its environment."""
    if type(pid) is not int or pid <= 1:
        return None
    info = BsdInfo()
    libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
    count = libproc.proc_pidinfo(pid, 3, 0, ctypes.byref(info), ctypes.sizeof(info))
    if count != ctypes.sizeof(info) or info.uid != os.getuid() or info.status == 5:
        return None  # Missing, foreign-owner or zombie process.
    libc = ctypes.CDLL(None, use_errno=True)
    mib = (ctypes.c_int * 3)(1, 49, pid)  # CTL_KERN, KERN_PROCARGS2, PID
    size = ctypes.c_size_t()
    if libc.sysctl(mib, 3, None, ctypes.byref(size), None, 0) or not 4 < size.value <= 1048576:
        return None
    data = ctypes.create_string_buffer(size.value)
    if libc.sysctl(mib, 3, data, ctypes.byref(size), None, 0):
        return None
    raw = data.raw[:size.value]
    argc = int.from_bytes(raw[:4], sys.byteorder)
    if not 0 < argc <= 4096:
        return None
    position = raw.find(b'\0', 4) + 1  # Skip executable path and padding.
    while position < len(raw) and raw[position] == 0:
        position += 1
    argv = []
    for _ in range(argc):
        end = raw.find(b'\0', position)
        if end < 0:
            return None
        argv.append(os.fsdecode(raw[position:end]))
        position = end + 1
    return {'birth': [info.seconds, info.microseconds], 'command': argv}


def atomic_write(path, payload):
    descriptor, temporary = tempfile.mkstemp(prefix=f'.{path.name}.', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def bounded_private_json(path):
    """Read one current-user regular file without following links or blocking on FIFOs."""
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        metadata = os.fstat(descriptor)
        if (not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.getuid() or
                metadata.st_mode & 0o077 or metadata.st_size > 65536):
            raise RuntimeError('local Kraken readiness must be a private regular file of at most 64 KiB')
        data = bytearray()
        while len(data) <= 65536:
            chunk = os.read(descriptor, 65537 - len(data))
            if not chunk:
                break
            data.extend(chunk)
        if len(data) > 65536:
            raise RuntimeError('local Kraken readiness document exceeds 64 KiB')
        return json.loads(data)
    finally:
        os.close(descriptor)


class Lifecycle:
    def __init__(self, env=None):
        self.env = dict(os.environ if env is None else env)
        self.root = Path(self.env.get('VECTORWARP_MACOS_ROOT', Path(__file__).resolve().parent.parent)).resolve()
        self.state = Path(self.env.get('VECTORWARP_MACOS_STATE', Path.home() / 'Library/Application Support/VectorWarp')).resolve()
        self.config = Path(self.env.get('VECTORWARP_MACOS_CONFIG', self.state / 'config.yml')).resolve()
        self.processor = Path(self.env.get('VECTORWARP_MACOS_PROCESSOR', self.root / 'bin/blah2')).resolve()
        node = self.env.get('VECTORWARP_MACOS_NODE', 'node')
        self.node = str(Path(shutil.which(node) or node).resolve())
        # macOS reports a framework Python executable in KERN_PROCARGS2 even
        # when sys.executable names its opt/bin symlink. Use the kernel spelling
        # so the controller record remains an exact argv identity.
        self.python = (process_identity(os.getpid()) or {}).get('command', [sys.executable])[0]
        self.launcher = Path(__file__).resolve().with_suffix('')
        self.kraken_adapter = self.root / 'script/vectorwarp-kraken-macos.py'
        self.logs = self.state / 'logs'
        self.http = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def config_data(self, config_path=None):
        config_path = Path(config_path or self.config).resolve()
        source = "const c=require(process.argv[1]).readConfig(process.argv[2]).config;process.stdout.write(JSON.stringify(c));"
        return json.loads(subprocess.check_output([self.node, '-e', source,
            str(self.root / 'api/config-store.js'), str(config_path)], timeout=5))

    def kraken_config(self):
        candidate = self.env.get('VECTORWARP_MACOS_KRAKEN_CONFIG')
        if not candidate:
            return self.config
        path = Path(candidate).resolve()
        candidates = (self.state / 'kraken-controller/candidates').resolve()
        if path.parent != candidates or not path.is_file() or path.stat().st_uid != os.getuid() or path.stat().st_mode & 0o077:
            raise RuntimeError('local Kraken candidate configuration must be a private file in this instance state')
        return path

    def kraken_settings(self, config_path=None):
        """Return one local live Kraken endpoint, or None for every other mode."""
        config = self.config_data(config_path or self.kraken_config())
        capture = config.get('capture', {})
        device = capture.get('device', {})
        if device.get('type') != 'Kraken' or capture.get('replay', {}).get('state') is True:
            return None
        heimdall = device.get('heimdall', {})
        host, data_port = heimdall.get('host'), heimdall.get('port')
        control_port = heimdall.get('control_port', 8092)
        if host != '127.0.0.1':
            raise RuntimeError('local Kraken controller requires capture.device.heimdall.host to be 127.0.0.1')
        if (not isinstance(data_port, int) or not isinstance(control_port, int) or
                not 0 < data_port <= 65535 or not 0 < control_port <= 65535 or
                data_port == control_port):
            raise RuntimeError('local Kraken controller needs distinct valid data and control ports')
        return {'host': host, 'data_port': data_port, 'control_port': control_port}

    def kraken_argv(self, token, config_path=None):
        config_path = Path(config_path or self.kraken_config()).resolve()
        if config_path != self.config:
            candidates = (self.state / 'kraken-controller/candidates').resolve()
            if (config_path.parent != candidates or not config_path.is_file() or
                    config_path.stat().st_uid != os.getuid() or config_path.stat().st_mode & 0o077):
                raise RuntimeError('local Kraken controller configuration is not owned by this instance')
        settings = self.kraken_settings(config_path)
        if not settings or not isinstance(token, str) or not len(token) == 64 or not all(c in '0123456789abcdef' for c in token):
            raise RuntimeError('local Kraken controller is not configured')
        return [self.python, str(self.kraken_adapter), 'run-local-v1', '--config', str(config_path),
            '--bind-host', '127.0.0.1', '--iq-port', str(settings['data_port']),
            '--control-port', str(settings['control_port']), '--state-dir', str(self.state / 'kraken-controller'),
            '--ready-file', str(self.state / 'kraken-ready.json'), '--instance-token', token]

    def expected_argv(self, name):
        if name == 'api':
            return [self.node, str(self.root / 'api/server.js'), str(self.config)]
        if name == 'processor':
            return [str(self.processor), '--config', str(self.config)]
        raise RuntimeError(f'no fixed argv is available for {name}')

    def runtime_env(self):
        """Pass resolved, rather than ambient, instance paths to every child."""
        return dict(self.env, VECTORWARP_MACOS_LIFECYCLE='1', VECTORWARP_MACOS_LAUNCHER=str(self.launcher),
            VECTORWARP_MACOS_ROOT=str(self.root), VECTORWARP_MACOS_STATE=str(self.state),
            VECTORWARP_MACOS_CONFIG=str(self.config), VECTORWARP_MACOS_PROCESSOR=str(self.processor),
            VECTORWARP_MACOS_NODE=self.node, BLAH2_RECEIVER_STATUS_EXECUTABLE=str(self.processor),
            VECTORWARP_MACOS_HEIMDALL_EXECUTABLE=self.env.get('VECTORWARP_MACOS_HEIMDALL_EXECUTABLE', str(self.root / 'bin/heimdall')),
            VECTORWARP_MACOS_HEIMDALL_HTML=self.env.get('VECTORWARP_MACOS_HEIMDALL_HTML', str(self.root / 'share/heimdall/index.html')))

    def record_path(self, name):
        return self.state / f'{name}.json'

    @staticmethod
    def formula_release_root(executable, suffix):
        """Return a Cellar release root for one exact executable suffix."""
        try:
            path = Path(executable)
            if tuple(path.parts[-len(suffix):]) != suffix:
                return None
            root = path
            for _ in suffix:
                root = root.parent
            cellar, formula, _version, libexec = root.parent.parent.parent, root.parent.parent, root.parent, root
            if libexec.name == 'libexec' and formula.name == 'vectorwarp' and cellar.name == 'Cellar':
                return root
        except (OSError, IndexError):
            pass
        return None

    def expected_matches(self, name, recorded):
        if name == 'kraken':
            if not isinstance(recorded, list) or len(recorded) != 17 or not all(isinstance(value, str) for value in recorded):
                return False
            try:
                config_path = Path(recorded[4]).resolve()
                candidates = (self.state / 'kraken-controller/candidates').resolve()
                valid_config = config_path == self.config or config_path.parent == candidates
                adapter = Path(recorded[1]).resolve()
                current_adapter = self.kraken_adapter.resolve()
                old_root = self.formula_release_root(adapter, ('script', 'vectorwarp-kraken-macos.py'))
                current_root = self.formula_release_root(current_adapter, ('script', 'vectorwarp-kraken-macos.py'))
                valid_adapter = adapter == current_adapter or (old_root is not None and current_root is not None and
                    old_root.parent.parent == current_root.parent.parent)
                ports = [int(recorded[8]), int(recorded[10])]
                return (valid_config and valid_adapter and all(0 < port <= 65535 for port in ports) and ports[0] != ports[1] and
                    recorded[2:8] == ['run-local-v1', '--config', recorded[4], '--bind-host', '127.0.0.1', '--iq-port'] and
                    recorded[9] == '--control-port' and recorded[11:16] == ['--state-dir', str(self.state / 'kraken-controller'),
                        '--ready-file', str(self.state / 'kraken-ready.json'), '--instance-token'] and
                    len(recorded[16]) == 64 and all(char in '0123456789abcdef' for char in recorded[16]))
            except (OSError, ValueError, TypeError):
                return False
        expected = self.expected_argv(name)
        if not (isinstance(recorded, list) and len(recorded) == len(expected) and
                isinstance(recorded[0], str) and os.path.isabs(recorded[0])):
            return False
        if recorded == expected:
            return True
        if name == 'api':
            # Dependency updates can change Node's Cellar path.
            if recorded[2:] != expected[2:]:
                return False
            old_root = self.formula_release_root(recorded[1], ('api', 'server.js'))
            current_root = self.formula_release_root(expected[1], ('api', 'server.js'))
        else:
            if recorded[1:] != expected[1:]:
                return False
            old_root = self.formula_release_root(recorded[0], ('bin', 'blah2'))
            current_root = self.formula_release_root(expected[0], ('bin', 'blah2'))
        # This deliberately parses the immutable Cellar layout instead of
        # checking old files: Homebrew may unlink the prior release before its
        # detached, birth-validated child can be stopped.
        return old_root is not None and current_root is not None and old_root.parent.parent == current_root.parent.parent

    def live(self, name):
        try:
            path = self.record_path(name)
            if path.stat().st_size > 65536:
                return None
            value = json.loads(path.read_text())
            command = value.get('command')
            recorded_argv = value.get('argv')
            if (not self.expected_matches(name, recorded_argv) or not isinstance(command, list) or
                    command[-len(recorded_argv):] != recorded_argv):
                return None
            identity = process_identity(value.get('pid'))
            if identity and all(identity[key] == value.get(key) for key in ('birth', 'command')):
                return value
        except (OSError, ValueError, TypeError, AttributeError):
            pass
        return None

    def foreign_live(self, name):
        """A birth-validated live record from another installation; never unlink it."""
        try:
            value = bounded_private_json(self.record_path(name))
            argv, command = value.get('argv'), value.get('command')
            identity = process_identity(value.get('pid'))
            valid = lambda items: isinstance(items, list) and bool(items) and all(isinstance(item, str) and item for item in items)
            return (valid(argv) and valid(command) and identity and
                    identity.get('birth') == value.get('birth') and identity.get('command') == value.get('command') and
                    command[-len(argv):] == argv and not self.expected_matches(name, argv))
        except (OSError, ValueError, TypeError, AttributeError):
            return False

    def refuse_foreign_instances(self):
        if any(self.foreign_live(name) for name in ('api', 'processor', 'kraken')):
            raise RuntimeError('A live VectorWarp instance belongs to another installation; stop the original installation before switching.')

    def record(self, name, pid, argv):
        identity = process_identity(pid)
        if not identity or not self.expected_matches(name, argv):
            raise RuntimeError(f'{name} exited before its process identity could be recorded')
        # Native binaries use the complete argv; script fixtures may prepend
        # their interpreter. All requested arguments must remain exact.
        if identity['command'][-len(argv):] != argv:
            raise RuntimeError(f'{name} process arguments do not match this instance')
        atomic_write(self.record_path(name), json.dumps({'pid': pid, 'argv': argv, **identity}).encode())

    @contextlib.contextmanager
    def lock(self):
        self.state.mkdir(mode=0o700, parents=True, exist_ok=True)
        state_info = self.state.stat()
        if state_info.st_uid != os.getuid() or state_info.st_mode & 0o022:
            raise RuntimeError('state directory must be current-user owned and not group or world writable')
        filename = self.state / 'lifecycle.lock'
        descriptor = os.open(filename, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            metadata = os.fstat(descriptor)
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_mode & 0o022:
                raise RuntimeError('lifecycle lock must be a current-user-owned regular file not writable by group or world')
            deadline = time.monotonic() + 30
            while True:
                try:
                    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('another local lifecycle action is still running')
                    time.sleep(.1)
            # Never unlink this inode. The kernel releases flock on exit,
            # including crashes, and competing processes share the same lock.
            yield
        finally:
            os.close(descriptor)

    def prepare(self):
        if not (self.root / 'api/server.js').is_file():
            raise RuntimeError(f'API is missing under {self.root}')
        self.logs.mkdir(mode=0o700, exist_ok=True)
        (self.state / 'recordings').mkdir(mode=0o700, exist_ok=True)
        if not self.config.exists():
            self.config.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            # Parse/dump through the app's YAML library: quotes or newlines in
            # a user directory must never become YAML syntax.
            source = '''const fs=require('fs'),path=require('path');
const root=process.argv[1],state=process.argv[2];
const yaml=require(path.join(root,'api/node_modules/js-yaml'));
const c=yaml.load(fs.readFileSync(path.join(root,'config/config.yml'),'utf8'));
c.network.ip='127.0.0.1'; c.save.path=path.join(state,'recordings')+'/';
c.capture.replay.file=path.join(state,'recordings','recording.blah2iq');
process.stdout.write(yaml.dump(c));'''
            payload = subprocess.check_output([self.node, '-e', source, str(self.root), str(self.state)], timeout=5)
            atomic_write(self.config, payload)

    def endpoint(self):
        if self.env.get('VECTORWARP_MACOS_API_PORT'):
            host, port = '127.0.0.1', int(self.env['VECTORWARP_MACOS_API_PORT'])
        else:
            source = "const c=require(process.argv[1]).readConfig(process.argv[2]).config;process.stdout.write(JSON.stringify(c.network));"
            network = json.loads(subprocess.check_output([self.node, '-e', source,
                str(self.root / 'api/config-store.js'), str(self.config)], timeout=5))
            host, port = network['ip'], network['ports']['api']
            host = {'0.0.0.0': '127.0.0.1', '::': '::1'}.get(host, host)
        if type(port) is not int or not 0 < port <= 65535:
            raise RuntimeError('API port must be an integer from 1 through 65535')
        return f'http://[{host}]:{port}' if ':' in host else f'http://{host}:{port}'

    def receiver_types(self):
        timeout = 20 if self.env.get('VECTORWARP_MACOS_DISTRIBUTION') == 'standalone' else 5
        started = time.monotonic()
        try:
            report = json.loads(subprocess.check_output([str(self.processor), '--receiver-status'], timeout=timeout,
                stderr=subprocess.DEVNULL, env=self.runtime_env()))
            if report.get('schema') != 1 or report.get('hardwareProbed') is not False:
                return ''
            names = [item['receiver'] for item in report['receivers'] if item.get('compiled') is True]
            return ','.join(dict.fromkeys(name for name in names if name in RECEIVERS))
        except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
            # Fail closed while retaining only bounded diagnostic metadata.
            print(f'Receiver capability probe failed ({type(error).__name__}) after {time.monotonic() - started:.1f}s.', file=sys.stderr)
            return ''

    def spawn(self, name, env=None, argv=None):
        if self.foreign_live(name):
            raise RuntimeError('A live VectorWarp instance belongs to another installation; stop the original installation before switching.')
        argv = argv or self.expected_argv(name)
        self.record_path(name).unlink(missing_ok=True)
        with (self.logs / f'{name}.log').open('ab') as log:
            child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                start_new_session=True, close_fds=True, cwd=self.root, env=env or self.env)
        try:
            time.sleep(.1)
            if child.poll() is not None:
                raise RuntimeError(f'{name} exited during startup; inspect {self.logs / (name + ".log")}')
            self.record(name, child.pid, argv)
        except BaseException:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            raise
        return child

    def start_kraken(self):
        config_path = self.kraken_config()
        if not self.kraken_settings(config_path) or self.live('kraken'):
            return
        if not self.kraken_adapter.is_file():
            raise RuntimeError(f'local Kraken adapter is missing: {self.kraken_adapter}')
        token = secrets.token_hex(32)
        ready_file = self.state / 'kraken-ready.json'
        ready_file.unlink(missing_ok=True)
        child = self.spawn('kraken', self.runtime_env(), self.kraken_argv(token, config_path))
        settings = self.kraken_settings(config_path)
        try:
            deadline = time.monotonic() + 60
            while self.live('kraken') and time.monotonic() < deadline:
                try:
                    ready = bounded_private_json(ready_file)
                    if not isinstance(ready, dict) or ready.get('schema') != 1 or ready.get('instanceToken') != token or ready.get('pid') != child.pid:
                        raise RuntimeError('local Kraken readiness document does not match this launch')
                    if ready.get('status') == 'error':
                        raise RuntimeError(f'local Kraken controller failed: {str(ready.get("error", "unknown error"))[:240]}')
                    if (ready.get('status') == 'ready' and ready.get('bindHost') == '127.0.0.1' and
                            ready.get('dataPort') == settings['data_port'] and ready.get('controlPort') == settings['control_port'] and
                            isinstance(ready.get('calibration'), str) and len(ready['calibration']) <= 64):
                        print(f'Local Kraken controller ready (calibration: {ready["calibration"]}).')
                        return
                except FileNotFoundError:
                    pass
                except (ValueError, TypeError) as error:
                    raise RuntimeError(f'local Kraken readiness document is invalid: {error}')
                time.sleep(.1)
            raise RuntimeError('local Kraken controller did not become ready within 60 seconds; inspect logs')
        except BaseException:
            self.stop('kraken')
            ready_file.unlink(missing_ok=True)
            raise

    def stop(self, name):
        if self.foreign_live(name):
            raise RuntimeError('A live VectorWarp instance belongs to another installation; stop the original installation before switching.')
        value = self.live(name)
        if value:
            try:
                os.kill(value['pid'], signal.SIGTERM)
            except ProcessLookupError:
                pass
            # The adapter relays SIGTERM to its native child and grants that
            # child up to eight seconds. Do not race it with a launcher kill.
            deadline = time.monotonic() + (15 if name == 'kraken' else 8)
            while self.live(name) and time.monotonic() < deadline:
                time.sleep(.1)
            # Recheck birth, exact argv and expected instance before escalation.
            if self.live(name):
                try:
                    os.kill(value['pid'], signal.SIGKILL)
                except ProcessLookupError:
                    pass
            print(f'Stopped {name}.')
        if name == 'kraken' and value:
            try:
                candidate = Path(value['argv'][4]).resolve()
                if candidate.parent == (self.state / 'kraken-controller/candidates').resolve():
                    candidate.unlink(missing_ok=True)
            except (IndexError, OSError, TypeError):
                pass
        self.record_path(name).unlink(missing_ok=True)

    def start_web(self):
        if self.live('api'):
            return
        env = dict(self.runtime_env(), BLAH2_RECEIVER_TYPES=self.receiver_types(), BLAH2_CONFIG_RESTART_COMMAND='')
        rsp_builder = self.root / 'script/vectorwarp-build-sdrplay-macos.sh'
        if rsp_builder.is_file() and os.access(rsp_builder, os.X_OK):
            # The browser route receives no build path or arguments. It can
            # only invoke this co-staged, no-argument local source-kit runner.
            env['VECTORWARP_MACOS_RSPDUO_BUILD'] = str(rsp_builder)
        if self.env.get('VECTORWARP_MACOS_API_PORT'):
            env['BLAH2_SETUP_PORT'] = self.env['VECTORWARP_MACOS_API_PORT']
        child = self.spawn('api', env)
        try:
            url = self.endpoint() + '/api/system/status'
            deadline = time.monotonic() + 12
            while self.live('api') and time.monotonic() < deadline:
                try:
                    with self.http.open(url, timeout=.5) as response:
                        status = json.loads(response.read(262145))
                    if str(status.get('serverId', '')).startswith(f'{child.pid}-') and self.live('api'):
                        print('Started local web service.')
                        return
                except (OSError, ValueError):
                    pass
                time.sleep(.1)
            raise RuntimeError('local web service did not become ready; inspect logs')
        except BaseException:
            self.stop('api')
            raise

    def start_processor(self):
        if not self.live('processor'):
            if not os.access(self.processor, os.X_OK):
                raise RuntimeError(f'native processor is missing or not executable: {self.processor}')
            # Each surveillance path already runs independently. Keep the
            # small Armadillo/OpenBLAS solves from starting another pool per
            # path; honor an explicit setting from the launch environment.
            env = self.runtime_env()
            env.setdefault('OPENBLAS_NUM_THREADS', '1')
            env.setdefault('OMP_NUM_THREADS', '1')
            self.spawn('processor', env)
            print('Started native processor.')

    def run(self, action):
        if action == 'status':
            for name, label in [('api', 'web'), ('kraken', 'Kraken controller'), ('processor', 'processor')]:
                print(f'{label}: {"running" if self.live(name) else "stopped"}')
            return
        if action == 'logs':
            print(f'API log: {self.logs / "api.log"}\nKraken log: {self.logs / "kraken.log"}\nProcessor log: {self.logs / "processor.log"}')
            return
        with self.lock():
            self.refuse_foreign_instances()
            if action in ('stop', 'restart'):
                self.stop('processor')
                self.stop('kraken')
                self.stop('api')
            if action == 'stop-kraken':
                self.stop('processor')
                self.stop('kraken')
            if action == 'restart-processing':
                self.stop('processor')
            if action not in ('stop', 'stop-kraken'):
                self.prepare()
                if action != 'restart-processing':
                    self.start_web()
                if action == 'start-kraken' and not self.kraken_settings():
                    raise RuntimeError('saved configuration is not a live local Kraken profile')
                if action in ('start', 'restart', 'start-kraken'):
                    self.start_kraken()
                if action in ('start', 'restart', 'restart-processing'):
                    self.start_processor()
        if action == 'open':
            url = self.endpoint() + '/'
            print(f'VectorWarp: {url}')
            subprocess.run(['open', url], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def supervise(self, stopping):
        """Foreground launchd target: restart the owned stack after a child dies."""
        delay = 1
        try:
            while not stopping.is_set():
                try:
                    with self.lock():
                        self.refuse_foreign_instances()
                        self.prepare()
                        self.start_web()
                        self.start_kraken()
                        self.start_processor()
                    delay = 1
                    while not stopping.wait(.25):
                        kraken_ok = not self.kraken_settings() or self.live('kraken')
                        if self.live('api') and self.live('processor') and kraken_ok:
                            continue
                        # A partial stack is never left running after a crash.
                        with self.lock():
                            self.refuse_foreign_instances()
                            self.stop('processor')
                            self.stop('kraken')
                            self.stop('api')
                        break
                except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                    print(f'vectorwarp-macos supervisor: {error}', file=sys.stderr)
                stopping.wait(delay)
                delay = min(delay * 2, 15)
        finally:
            with self.lock():
                self.stop('processor')
                self.stop('kraken')
                self.stop('api')


def main():
    if len(sys.argv) > 2:
        raise RuntimeError(HELP)
    action = sys.argv[1] if len(sys.argv) == 2 else 'open'
    if action in ('help', '-h', '--help'):
        print(HELP)
        return
    if sys.platform != 'darwin':
        raise RuntimeError('this local launcher requires macOS')
    if action not in ('open', 'start', 'stop', 'restart', 'restart-processing', 'start-kraken', 'stop-kraken', 'status', 'logs', 'supervise'):
        raise RuntimeError(HELP)
    stopping = __import__('threading').Event()
    def interrupted(signum, _frame):
        stopping.set()
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    lifecycle = Lifecycle()
    lifecycle.supervise(stopping) if action == 'supervise' else lifecycle.run(action)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'vectorwarp-macos: {error}', file=sys.stderr)
        sys.exit(1)
