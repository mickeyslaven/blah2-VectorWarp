#!/usr/bin/env python3
"""Actual Heimdall -> VectorWarp -> API integration, with USB-isolated fake IQ."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--heimdall', type=Path, required=True)
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--app-root', type=Path, default=ROOT)
    args = parser.parse_args()
    args.heimdall = args.heimdall.resolve(strict=True)
    args.include = args.include.resolve(strict=True)
    app = args.app_root.resolve(strict=True)
    work = args.output.resolve()
    work.mkdir(mode=0o700, parents=True, exist_ok=False)
    simulation = module('native_simulation', ROOT / 'test/macos/kraken/native_simulation_test.py')
    native = simulation.prepare(args, work)
    lifecycle = module('lifecycle', app / 'script/vectorwarp-macos.py')
    sockets = [socket.socket() for _ in range(12)]
    for sock in sockets:
        sock.bind(('127.0.0.1', 0))
    ports = [sock.getsockname()[1] for sock in sockets]
    for sock in sockets:
        sock.close()
    config = json.loads(subprocess.check_output([shutil.which('node'), '-e',
        'process.stdout.write(JSON.stringify(require("js-yaml").load(require("fs").readFileSync(process.argv[1],"utf8"))))',
        str(app / 'config/config-kraken.yml')], cwd=app / 'api', text=True))
    for index, name in enumerate(config['network']['ports']):
        config['network']['ports'][name] = ports[index]
    api_port = config['network']['ports']['api']
    config['network']['ip'] = '127.0.0.1'
    config['capture']['fc'] = 123500000
    config['capture']['device']['heimdall'].update(host='127.0.0.1', port=ports[8], control_port=ports[9], gain=40)
    config['capture']['device']['surveillance_channels'] = [1, 2, 3, 4]
    config['process']['reference_synthesis'].update(mode='dedicated', channels=[0])
    config['process']['performance'].update(acceleration='cpu', surveillance_workers=1, fft_threads=1)
    config['process']['data'].update(cpi=.05, buffer=1, overlap=0)
    config['process']['ambiguity'].update(delayMin=0, delayMax=16, dopplerMin=-100, dopplerMax=100)
    config['process']['clutter']['enable'] = False
    config['process']['tracker']['enable'] = False
    config['truth']['adsb']['enabled'] = False
    config['save']['path'] = str(work) + '/'
    filename = work / 'config.json'
    filename.write_text(json.dumps(config))
    state = work / 'state'
    sdk = work / 'simulated-sdk'
    sdk.mkdir(mode=0o700)
    (sdk / 'command').write_text('')
    html = work / 'index.html'
    html.write_text('<!doctype html><title>Simulated controller</title>')
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('DYLD_', 'LD_', 'HEIMDALL_', 'VECTORWARP_KRAKEN_SIM_'))}
    env.update(VECTORWARP_MACOS_ROOT=str(app), VECTORWARP_MACOS_STATE=str(state),
        VECTORWARP_MACOS_CONFIG=str(filename), VECTORWARP_MACOS_PROCESSOR=str(args.binary.resolve(strict=True)),
        VECTORWARP_MACOS_NODE=shutil.which('node'), VECTORWARP_MACOS_HEIMDALL_EXECUTABLE=str(native),
        VECTORWARP_MACOS_HEIMDALL_HTML=str(html), VECTORWARP_MACOS_HEIMDALL_WEB_PORT=str(ports[10]),
        VECTORWARP_MACOS_HEIMDALL_RTL_PORT=str(ports[11]), VECTORWARP_KRAKEN_SIM_ROOT=str(sdk),
        VECTORWARP_KRAKEN_SIM_COUNT='5', HEIMDALL_VERBOSE_LOG='1')
    launcher = app / 'script/vectorwarp-macos'
    def run(action, check=True):
        result = subprocess.run([str(launcher), action], env=env, text=True, capture_output=True, timeout=85)
        with (work / 'lifecycle.log').open('a') as stream:
            stream.write(f'{action}: {result.returncode}\n{result.stdout}{result.stderr}')
        if check:
            assert result.returncode == 0, result.stdout + result.stderr
        return result
    base = f'http://127.0.0.1:{api_port}'
    http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def request(endpoint, body=None, revision=None):
        headers = {'Origin': base, 'Content-Type': 'application/json',
            'X-VectorWarp-Intent': 'config-write-v1', 'X-VectorWarp-Receiver-Sync': 'synchronize-v1'}
        if revision:
            headers['If-Match'] = f'"{revision}"'
        req = urllib.request.Request(base + endpoint, headers=headers,
            data=json.dumps(body).encode() if body is not None else None,
            method='PUT' if body is not None else 'GET')
        try:
            with http.open(req, timeout=75 if body else 3) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            raise AssertionError(error.read().decode()) from error
    def wait(check, description, seconds=90):
        deadline = time.monotonic() + seconds
        last = None
        while time.monotonic() < deadline:
            try:
                value = check()
                if value:
                    return value
            except (OSError, ValueError, urllib.error.URLError) as error:
                last = error
            time.sleep(.25)
        raise AssertionError(f'{description}: {last}')
    def receiving():
        status = request('/api/system/status')
        return status if status.get('radar') == 'receiving' else None
    def owned():
        return [json.loads((state / f'{name}.json').read_text())['pid'] for name in ('api', 'kraken', 'processor')]
    supervisor = None
    try:
        run('start')
        first = owned()
        capabilities = request('/api/config/capabilities')
        assert not request('/api/system/status').get('setupRequired'), capabilities
        first_status = wait(receiving, 'No native five-channel radar telemetry')
        run('start')
        assert owned() == first, 'Repeated start duplicated the owned stack'
        revision = request('/api/config/capabilities')['configRevision']
        config['capture']['fc'] = 124500000
        result = request('/api/config?restart=true', config, revision)
        assert result['ok'] and result['restarting'], result
        wait(lambda: all(a != b for a, b in zip(first, owned())), 'Save & Restart did not replace the full stack')
        status = wait(receiving, 'Retuned native radar did not resume')
        assert status['configRevision'] == status['loadedRevision'] == result['revision']
        second = owned()
        ready = json.loads((state / 'kraken-ready.json').read_text())
        native_pid = ready['nativePid']
        # A killed adapter cannot leave its owned USB process running.
        os.kill(second[1], signal.SIGKILL)
        wait(lambda: lifecycle.process_identity(native_pid) is None,
             'Native controller survived the loss of its owning adapter', seconds=15)
        # Ownership must survive an externally edited receiver/replay profile.
        config['capture']['replay']['state'] = True
        filename.write_text(json.dumps(config))
        run('stop')
        assert all(lifecycle.process_identity(pid) is None for pid in second)
        assert not any((state / f'{name}.json').exists() for name in ('api', 'kraken', 'processor'))

        # Exercise the actual foreground target used by Homebrew/launchd. A
        # crashed native controller must replace and resume the entire stack.
        config['capture']['replay']['state'] = False
        filename.write_text(json.dumps(config))
        with (work / 'supervisor.log').open('w') as log:
            supervisor = subprocess.Popen([str(launcher), 'supervise'], env=env,
                                          stdout=log, stderr=subprocess.STDOUT)
        wait(receiving, 'Supervised native capture did not start')
        supervised = owned()
        native_pid = json.loads((state / 'kraken-ready.json').read_text())['nativePid']
        crash_time = time.time() * 1000
        os.kill(native_pid, signal.SIGKILL)
        wait(lambda: all(a != b for a, b in zip(supervised, owned())),
             'Supervisor did not replace the full stack after native failure', seconds=30)
        resumed = wait(receiving, 'Supervised capture did not recover after native failure')
        assert resumed['lastFrameAt'] > crash_time
        final_owned = owned()
        final_native = json.loads((state / 'kraken-ready.json').read_text())['nativePid']
        supervisor.terminate()
        supervisor.wait(timeout=35)
        assert supervisor.returncode == 0, 'Supervisor did not stop cleanly'
        supervisor = None
        assert all(lifecycle.process_identity(pid) is None for pid in final_owned + [final_native])
        assert not any((state / f'{name}.json').exists() for name in ('api', 'kraken', 'processor'))
        events = simulation.driver_events(sdk)
        assert events and not any(event['event'] in ('write_after_close', 'close_while_reading') for event in events)
        report = dict(simulatedOnly=True, hardwareTested=False, channels=5, cpu=True,
            initialFrames=first_status.get('lastFrameAt'), retunedFrames=status.get('lastFrameAt'),
            idempotentStart=True, saveRestart=True, parentDeathCleanup=True,
            supervisorRecovery=True, supervisorStopped=True, driverEvents=len(events))
        (work / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report), flush=True)
    finally:
        if supervisor is not None and supervisor.poll() is None:
            supervisor.terminate()
            try:
                supervisor.wait(timeout=35)
            except subprocess.TimeoutExpired:
                supervisor.kill()
                supervisor.wait(timeout=5)
        run('stop', check=False)


if __name__ == '__main__':
    main()
