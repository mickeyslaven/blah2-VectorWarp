#!/usr/bin/env python3
"""Actual macOS launcher/API/CPU integration with synthetic IQ and no SDR access."""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import statistics
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('replay_fixture', ROOT / 'test/recording/processor_replay_test.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def eventually(check, description, seconds=25):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            value = check()
            if value:
                return value
        except (OSError, ValueError, urllib.error.URLError) as error:
            last = error
        time.sleep(.15)
    raise AssertionError(f'{description}: {last}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--app-root', type=Path, default=ROOT,
                        help='test a complete installed artifact instead of the source API and launcher')
    parser.add_argument('--standalone', action='store_true',
                        help='exercise a staged standalone runtime without Homebrew receiver actions')
    parser.add_argument('--endurance-seconds', type=int, default=0,
                        help='also test actual crash recovery, process pause/resume and bounded replay endurance')
    args = parser.parse_args()
    if args.endurance_seconds and args.endurance_seconds < 360:
        parser.error('--endurance-seconds must be at least 360 (five-minute history warmup plus steady-state samples)')
    binary = args.binary.resolve()
    app_root = args.app_root.resolve()
    if not binary.is_file():
        raise SystemExit(f'Missing native processor: {binary}')
    node = (app_root / 'bin/node').resolve() if args.standalone else Path(shutil.which('node') or '')
    if not node.is_file():
        raise SystemExit(f'Missing Node runtime: {node}')
    with tempfile.TemporaryDirectory(prefix='vectorwarp-macos-lifecycle-') as temporary:
        work = Path(temporary)
        state = work / 'state with spaces'
        config_file = work / 'config.json'
        recording = work / 'synthetic.blah2iq'
        fixture.write_blah2iq(recording, 2, 2000000, 204640000, frames=5)
        reservations = [socket.socket() for _ in range(8)]
        for listener in reservations:
            listener.bind(('127.0.0.1', 0))
        ports = dict(zip(['api', *fixture.PORT_NAMES, 'config'], [s.getsockname()[1] for s in reservations]))
        for listener in reservations:
            listener.close()
        config = fixture.config('Usrp', 2, recording, ports['api'],
                                {name: ports[name] for name in fixture.PORT_NAMES}, loop=True)
        # The native replay fixture is deliberately minimal; browser validation
        # also requires the regular application's display/site configuration.
        base = json.loads(subprocess.check_output([str(node), '-e',
            'process.stdout.write(JSON.stringify(require("js-yaml").load(require("fs").readFileSync(process.argv[1],"utf8"))))',
            str(app_root / 'config/config.yml')], cwd=app_root / 'api', text=True))
        base.update(config)
        base['network']['ports']['config'] = ports['config']
        base['truth']['adsb']['enabled'] = False
        base['save']['path'] = str(work) + '/'
        config_file.write_text(json.dumps(base))
        # Prevent the test's web-only check from opening the user's default browser.
        commands = work / 'commands'
        commands.mkdir()
        (commands / 'open').write_text('#!/bin/sh\nexit 0\n')
        (commands / 'open').chmod(0o755)
        env = dict(os.environ, VECTORWARP_MACOS_ROOT=str(app_root),
                   VECTORWARP_MACOS_STATE=str(state), VECTORWARP_MACOS_CONFIG=str(config_file),
                   VECTORWARP_MACOS_PROCESSOR=str(binary), VECTORWARP_MACOS_NODE=str(node),
                   PATH=str(commands) + os.pathsep + os.environ['PATH'])
        if args.standalone:
            env.update(DISTRIBUTION='standalone', VECTORWARP_MACOS_DISTRIBUTION='standalone')
        launcher = app_root / 'script/vectorwarp-macos'
        base_url = f'http://127.0.0.1:{ports["api"]}'
        def run(action, success=True):
            result = subprocess.run([str(launcher), action], env=env, text=True, capture_output=True, timeout=50)
            if success and result.returncode:
                raise AssertionError(f'{action}: {result.stdout}{result.stderr}')
            return result
        def request(endpoint, method='GET', body=None, revision=None):
            headers = {'Origin': base_url, 'Content-Type': 'application/json',
                       'X-VectorWarp-Intent': 'config-write-v1',
                       'X-VectorWarp-Receiver-Sync': 'synchronize-v1'}
            if revision:
                headers['If-Match'] = f'"{revision}"'
            req = urllib.request.Request(base_url + endpoint, method=method, headers=headers,
                                         data=json.dumps(body).encode() if body is not None else None)
            try:
                with urllib.request.urlopen(req, timeout=3) as response:
                    return json.load(response)
            except urllib.error.HTTPError as error:
                raise AssertionError(f'{method} {endpoint}: HTTP {error.code}: {error.read().decode()}') from error
        def receiving():
            status = request('/api/system/status')
            return status if status.get('radar') == 'receiving' else None
        def pids():
            return tuple(json.loads((state / f'{name}.json').read_text())['pid'] for name in ['api', 'processor'])
        try:
            run('open')
            assert not (state / 'processor.json').exists(), 'web-only launch started processing'
            capabilities = request('/api/config/capabilities')
            assert capabilities['restartAvailable'] is True
            native = json.loads(subprocess.check_output([str(binary), '--receiver-status'], text=True))
            expected = {item['receiver'] for item in native['receivers'] if item['compiled']}
            assert set(capabilities['compiledLiveTypes']) == expected, \
                f'Browser adapters {capabilities["compiledLiveTypes"]} must match native {sorted(expected)}'
            assert len(capabilities['compiledLiveTypes']) == len(expected), 'Compiled receiver names must be unique'
            receivers = request('/api/receivers')
            assert len(receivers['receivers']) == 4
            if args.standalone:
                assert receivers['managementAvailable'] is False
                assert receivers['management']['code'] == 'STANDALONE_COMPANION_REQUIRED'
                assert receivers['management']['actions'] == []
                kraken = next(item for item in receivers['receivers'] if item['type'] == 'Kraken')
                assert any('separately installed local Heimdall companion' in step['text']
                           for step in kraken['setupGuide'])
            elif any(Path(path).is_file() for path in ('/opt/homebrew/bin/brew', '/usr/local/bin/brew')):
                assert receivers['managementAvailable'] is True
                assert {action['id'] for action in receivers['management']['actions']} == {
                    'macos-install-uhd', 'macos-install-hackrf', 'macos-start-kraken'}
            else:
                assert receivers['management']['code'] == 'HOMEBREW_REQUIRED'
            run('start')
            first = pids()
            status = eventually(receiving, 'CPU replay did not produce fresh API telemetry')
            assert status['gpuSetup']['qualification'] == 'not-run'
            run('start')
            assert pids() == first, 'Repeated start created duplicate processes'
            run('restart')
            second = pids()
            assert all(a != b for a, b in zip(first, second))
            eventually(receiving, 'command restart did not resume replay')
            candidate = copy.deepcopy(base)
            candidate['capture']['fc'] = 204640000  # Keep recording RF metadata valid.
            candidate['process']['detection']['pfa'] = .02
            revision = request('/api/config/capabilities')['configRevision']
            reply = request('/api/config?restart=true', 'PUT', candidate, revision)
            assert reply['ok'] and reply['restarting']
            eventually(lambda: all(a != b for a, b in zip(second, pids())), 'Save & Restart did not restart both processes')
            refreshed = eventually(receiving, 'Save & Restart did not restore fresh frames')
            assert refreshed['configRevision'] == refreshed['loadedRevision'] == reply['revision']
            assert request('/api/runtime/config')['process']['detection']['pfa'] == .02
            run('stop')
            stopped = run('status').stdout
            assert 'web: stopped' in stopped and 'processor: stopped' in stopped
            assert not (state / 'api.json').exists() and not (state / 'processor.json').exists()
            if args.endurance_seconds:
                with (work / 'supervisor.log').open('w') as output:
                    supervisor = subprocess.Popen([str(launcher), 'supervise'], env=env,
                        stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                    try:
                        eventually(receiving, 'supervised replay did not start')
                        for component in (0, 1):
                            previous = pids()
                            # Only the PID recorded in this test's private state
                            # is signalled; these are actual native/API crashes.
                            os.kill(previous[component], signal.SIGKILL)
                            eventually(lambda: all(a != b for a, b in zip(previous, pids())),
                                       'supervisor did not replace both crashed-instance processes')
                            eventually(receiving, 'crash recovery did not resume frames')
                        owned = pids()
                        os.kill(owned[1], signal.SIGSTOP)
                        try:
                            eventually(lambda: request('/api/system/status')['radar'] != 'receiving',
                                       'paused processor did not become stale')
                        finally:
                            os.kill(owned[1], signal.SIGCONT)
                        eventually(receiving, 'process resume did not restore frames')
                        assert pids() == owned, 'pause/resume unexpectedly replaced processes'
                        samples, sample_times, frame_updates, last_frame = [], [], 0, None
                        started = time.monotonic()
                        deadline = started + args.endurance_seconds
                        while time.monotonic() < deadline:
                            status = request('/api/system/status')
                            assert status['radar'] == 'receiving', 'replay stalled during endurance'
                            assert pids() == owned and supervisor.poll() is None
                            frame = status['lastFrameAt']
                            frame_updates += frame != last_frame
                            last_frame = frame
                            samples.append([int(subprocess.check_output(
                                ['ps', '-p', str(pid), '-o', 'rss='], text=True).strip()) for pid in owned])
                            sample_times.append(time.monotonic() - started)
                            time.sleep(1)
                        assert frame_updates >= args.endurance_seconds * .8, 'too few fresh frames'
                        # The required five-minute detection window fills during
                        # startup. Compare steady-state medians after that full
                        # warmup; startup-to-final RSS confounds retained history
                        # with a leak, and a single endpoint confounds V8 GC.
                        steady = [sample for elapsed, sample in zip(sample_times, samples) if elapsed >= 300]
                        assert len(steady) >= 40, 'insufficient post-history-warmup memory samples'
                        initial = [statistics.median(row[i] for row in steady[:20]) for i in (0, 1)]
                        final = [statistics.median(row[i] for row in steady[-20:]) for i in (0, 1)]
                        print(json.dumps({'test': 'native supervised synthetic replay',
                            'seconds': args.endurance_seconds, 'frameUpdates': frame_updates,
                            'rssKiBInitial': samples[0], 'rssKiBFinal': samples[-1],
                            'rssKiBPeak': [max(row[i] for row in samples) for i in (0, 1)],
                            'rssKiBAt30Samples': samples[::30],
                            'memoryWarmupSeconds': 300, 'steadyStateSamples': len(steady),
                            'rssKiBSteadyInitialMedian': initial, 'rssKiBSteadyFinalMedian': final,
                            'crashRecovery': ['api', 'processor'], 'processPauseResume': True,
                            'physicalSleepWake': False}), flush=True)
                        for component in (0, 1):
                            assert final[component] - initial[component] < 65536, f'{["API", "processor"][component]} steady-state RSS grew by 64 MiB: {initial} -> {final}'
                    finally:
                        supervisor.terminate()
                        supervisor.wait(timeout=30)
                        assert supervisor.returncode == 0
                assert not (state / 'api.json').exists() and not (state / 'processor.json').exists()
            print('Actual macOS API + native synthetic replay: web-only/start/idempotence/restart/Save & Restart/stop PASS; no physical SDR used.')
        except Exception:
            for name in ['api', 'processor']:
                log = state / 'logs' / f'{name}.log'
                if log.exists():
                    print(f'{name} log tail:\n{log.read_text()[-5000:]}')
            raise
        finally:
            run('stop', success=False)


if __name__ == '__main__':
    main()
