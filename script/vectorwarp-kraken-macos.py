#!/usr/bin/env python3
"""Foreground owner for a separately installed local Heimdall receiver.

The control connection is loopback only. Readiness confirms configuration, not
physical coherence; the processor independently rejects uncalibrated IQ frames.
"""
import argparse
import json
import math
import os
from pathlib import Path
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time


def integer(value, label, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'{label} must be an integer from {low} through {high}')
    return value


def trusted_file(value, executable=False):
    path = Path(value).resolve(strict=True)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o022:
        raise ValueError(f'{path.name} must be a regular file not writable by group or world')
    if executable and not os.access(path, os.X_OK):
        raise ValueError(f'{path.name} is not executable')
    return path


def read_profile(config, root, node):
    source = "const r=require(process.argv[1]).readConfig(process.argv[2]);if(r.readError)throw Error(r.readError);process.stdout.write(JSON.stringify(r.config.capture));"
    capture = json.loads(subprocess.check_output([
        node, '-e', source, str(root / 'api/config-store.js'), str(config)], timeout=5))
    device = capture.get('device', {})
    if device.get('type') != 'Kraken' or capture.get('replay', {}).get('state') is not False:
        raise ValueError('Local Heimdall requires a saved live Kraken profile')
    endpoint = device.get('heimdall', {})
    if endpoint.get('host') != '127.0.0.1':
        raise ValueError('Local Heimdall requires the 127.0.0.1 loopback endpoint')
    integer(capture.get('fs'), 'Kraken sample rate', 2400000, 2400000)
    frequency = integer(capture.get('fc'), 'Kraken frequency', 1, 4294967295)
    channels = integer(device.get('channel_count'), 'Kraken channels', 2, 8)
    iq = integer(endpoint.get('port'), 'IQ port', 1, 65535)
    control = integer(endpoint.get('control_port', 8092), 'Control port', 1, 65535)
    gain = endpoint.get('gain', 'keep')
    if gain != 'keep' and (type(gain) not in (int, float) or not math.isfinite(gain)
                           or gain != -1 and not 0 <= gain <= 50
                           or round(gain * 10) != gain * 10):
        raise ValueError('Kraken gain must be keep, -1 (automatic), or 0–50 dB in 0.1 dB steps')
    return dict(frequency=frequency, channels=channels, iq=iq, control=control, gain=gain)


def publish(path, document):
    descriptor, temporary = tempfile.mkstemp(prefix='.kraken-ready-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            json.dump(document, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def matching_status(live, profile):
    settings = live.get('settings', {})
    if not isinstance(settings, dict):
        raise ValueError('Heimdall reported invalid settings telemetry')
    if (settings.get('center_freq') != profile['frequency'] or
            settings.get('sample_rate') != 2400000 or
            live.get('num_channels') != profile['channels'] or
            live.get('operating_mode') != 'coherent' or live.get('wideband_enabled') is True):
        raise ValueError('Heimdall telemetry does not match the saved coherent Kraken profile')
    if profile['gain'] != 'keep' and settings.get('gain') != profile['gain']:
        raise ValueError('Heimdall gain readback does not match the saved Kraken profile')
    if live.get('reconfiguring') is True or live.get('recovering') is True:
        return None
    calibration = live.get('calibration_state', 'PENDING')
    if not isinstance(calibration, str) or len(calibration) > 64:
        raise ValueError('Heimdall reported an invalid calibration state')
    if calibration.upper() in ('ERROR', 'FAILED'):
        raise ValueError('Heimdall reported a calibration failure')
    return calibration


def telemetry(port):
    with socket.create_connection(('127.0.0.1', port), timeout=1) as connection:
        data = bytearray()
        deadline = time.monotonic() + 2
        while len(data) <= 65536 and time.monotonic() < deadline:
            chunk = connection.recv(4096)
            if not chunk:
                break
            data.extend(chunk)
            while b'\n' in data:
                line, _, remaining = data.partition(b'\n')
                data = bytearray(remaining)
                document = json.loads(line)
                if isinstance(document, dict) and 'settings' in document:
                    return document
    raise OSError('No bounded Heimdall status message received')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['run-local-v1'])
    for name in ('config', 'bind-host', 'state-dir', 'ready-file', 'instance-token'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--iq-port', type=int, required=True)
    parser.add_argument('--control-port', type=int, required=True)
    args = parser.parse_args(argv)
    ready = Path(args.ready_file).absolute()
    document = dict(schema=1, instanceToken=args.instance_token, pid=os.getpid(),
                    dataPort=args.iq_port, controlPort=args.control_port,
                    bindHost=args.bind_host, calibration='PENDING')
    child = None
    stopping = False

    def stop(_sig, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        if args.bind_host != '127.0.0.1':
            raise ValueError('Only numeric IPv4 loopback binding is supported')
        if not args.instance_token or len(args.instance_token) > 128 or not all(c in '0123456789abcdefABCDEF' for c in args.instance_token):
            raise ValueError('A bounded instance token is required')
        root = Path(os.environ.get('VECTORWARP_MACOS_ROOT', Path(__file__).resolve().parent.parent)).resolve()
        node = os.environ.get('VECTORWARP_MACOS_NODE', 'node')
        profile = read_profile(Path(args.config).resolve(strict=True), root, node)
        if (profile['iq'], profile['control']) != (args.iq_port, args.control_port):
            raise ValueError('Controller ports do not match the saved configuration')
        executable = trusted_file(os.environ.get('VECTORWARP_MACOS_HEIMDALL_EXECUTABLE', root / 'bin/heimdall'), True)
        html = trusted_file(os.environ.get('VECTORWARP_MACOS_HEIMDALL_HTML', root / 'share/heimdall/index.html'))
        state = Path(args.state_dir).absolute()
        state.mkdir(mode=0o700, parents=True, exist_ok=True)
        for directory in (state, ready.parent):
            info = directory.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o022:
                raise ValueError('Controller state must be a private, current-user-owned directory')
        ports = [args.iq_port, args.control_port]
        ports += [integer(int(os.environ.get(name, default)), name, 1, 65535)
                  for name, default in [('VECTORWARP_MACOS_HEIMDALL_WEB_PORT', 8070),
                                        ('VECTORWARP_MACOS_HEIMDALL_RTL_PORT', 1234)]]
        if len(set(ports)) != 4:
            raise ValueError('Heimdall data, control, web and RTL ports must differ')
        # Preflight every endpoint before opening USB. Native listeners also
        # fail on a bind race; no existing receiver/controller is taken over.
        reservations = []
        try:
            for port in ports:
                sock = socket.socket()
                reservations.append(sock)
                # Match the native listeners: a previous owned connection in
                # TIME_WAIT must not block crash recovery. Listening still
                # rejects an active server or another concurrent reservation.
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind(('127.0.0.1', port))
                sock.listen(1)
        finally:
            for sock in reservations:
                sock.close()
        env = dict(os.environ, HEIMDALL_BIND_HOST='127.0.0.1',
                   HEIMDALL_DATA_PORT=str(ports[0]), HEIMDALL_CONTROL_PORT=str(ports[1]),
                   HEIMDALL_WEB_PORT=str(ports[2]), HEIMDALL_RTL_PORT=str(ports[3]),
                   HEIMDALL_HTML_PATH=str(html), HEIMDALL_CENTER_FREQ_HZ=str(profile['frequency']),
                   HEIMDALL_INSTANCE_TOKEN=args.instance_token, HEIMDALL_PARENT_PID=str(os.getpid()))
        env.pop('HEIMDALL_GAIN_TENTHS', None)
        # The fork applies this free-form override at read_async(), after its
        # initial settings/readback. Managed capture must retain the saved profile.
        env.pop('LIBRTLSDR_OPT', None)
        if profile['gain'] != 'keep':
            env['HEIMDALL_GAIN_TENTHS'] = str(-1 if profile['gain'] == -1 else round(profile['gain'] * 10))
        child = subprocess.Popen([str(executable), '-n', str(profile['channels'])], cwd=state, env=env)
        deadline = time.monotonic() + 60
        while not stopping:
            if child.poll() is not None:
                raise RuntimeError(f'Heimdall exited during initialization (status {child.returncode}); see logs/kraken.log')
            try:
                live = telemetry(args.control_port)
                if live.get('instance_token') != args.instance_token:
                    raise ValueError('Heimdall telemetry belongs to a different controller instance')
                calibration = matching_status(live, profile)
            except (OSError, json.JSONDecodeError):
                calibration = None
            if calibration is not None:
                document.update(status='ready', calibration=calibration, nativePid=child.pid)
                publish(ready, document)
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('Heimdall initialization timed out; see logs/kraken.log')
            time.sleep(.1)
        while not stopping and child.poll() is None:
            time.sleep(.2)
        if not stopping:
            raise RuntimeError(f'Heimdall exited (status {child.returncode}); see logs/kraken.log')
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        document.update(status='error', error=str(error).replace('\n', ' ')[:240])
        try:
            publish(ready, document)
        except OSError:
            pass
        print(f'vectorwarp-kraken: {error}', file=sys.stderr)
        return 1
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=3)


if __name__ == '__main__':
    sys.exit(main())
