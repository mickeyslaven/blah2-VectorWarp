#!/usr/bin/env python3
"""Local SDK enumeration with nonexistent serials, no capture, and replay recovery."""
import argparse, importlib.util, json, os, re, signal, subprocess, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('replay', ROOT / 'test/recording/processor_replay_test.py')
replay = importlib.util.module_from_spec(spec); spec.loader.exec_module(replay)

def no_device(binary, receiver, env=None):
    with tempfile.TemporaryDirectory(prefix='vectorwarp-macos-sdk-') as temporary:
        root = Path(temporary); sinks = replay.SinkGroup(); status = replay.StatusServer()
        try:
            sinks.start(); status.start()
            payload = replay.config(receiver, 2, root / 'unused.blah2iq', status.port, sinks.ports())
            payload['capture']['replay']['state'] = False
            if receiver == 'Usrp':
                payload['capture']['device']['address'] = 'type=b200,serial=0000000000000000'
            elif receiver == 'RspDuo':
                payload['capture']['device']['serial'] = 'NO-RSP-HARDWARE'
            else:
                payload['capture']['device']['serial'] = ['0000000000000000', 'ffffffffffffffff']
            config = root / 'no-device.json'; config.write_text(json.dumps(payload))
            process = subprocess.Popen([str(binary), '-c', str(config)], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
            try:
                # Capture startup failures are deliberately retained as an API
                # error state until the operator stops/restarts the processor.
                # Require that observable state, then verify graceful shutdown;
                # a timeout or SIGKILL is never counted as passing.
                ready = replay.wait_for(lambda: process.poll() is not None or any(
                    item.get('input') == 'live' and item.get('state') == 'error' and item.get('error')
                    for item in status.snapshot()), time.monotonic()+10)
                if not ready or process.poll() is not None:
                    raise AssertionError(f'{receiver} failed to publish its live-input error while remaining controllable')
                process.terminate()
                output, _ = process.communicate(timeout=5)
                if process.returncode != 0:
                    raise AssertionError(f'{receiver} did not shut down cleanly after startup failure: {process.returncode}')
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
            expected = {'Usrp': r'No devices found',
                        'HackRF': r'Failed to find 2.*readable serials',
                        'RspDuo': r'NumDevs=\d+[\s\S]*No receiver found'}[receiver]
            if not re.search(expected, output, re.IGNORECASE):
                raise AssertionError(f'{receiver} did not reach the SDK no-device path: {output[-4000:]}')
            return output
        finally:
            sinks.close(); status.close()

def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--binary', type=Path, default=ROOT/'build/macos/staged-open/artifact/bin/blah2')
    parser.add_argument('--include-rspduo', action='store_true',
        help='also run RSPduo against an explicit, private SDK service')
    parser.add_argument('--sdrplay-service', type=Path,
        help='absolute sdrplay_apiService path; required with --include-rspduo')
    parser.add_argument('--local-rspduo-kit', action='store_true',
        help='build the staged source kit against the explicitly supplied manual SDK before testing')
    args = parser.parse_args(); binary = args.binary.resolve()
    if args.include_rspduo and (not args.sdrplay_service or not args.sdrplay_service.is_file()):
        raise SystemExit('--include-rspduo requires --sdrplay-service PATH from a user-installed/private SDK')
    env = dict(os.environ)
    if args.local_rspduo_kit and not args.include_rspduo:
        parser.error('--local-rspduo-kit requires --include-rspduo')
    core_links = subprocess.check_output(['otool', '-L', str(binary)], text=True)
    assert not any(name in core_links for name in ('libsdrplay', 'libuhd', 'libhackrf')), \
        'Optional receiver SDKs must not become required core dependencies'
    service = None
    if args.include_rspduo:
        service = args.sdrplay_service.resolve()
        sdk_root = service.parent.parent
        runtime_dirs = [str(sdk_root / 'bin'), str(sdk_root / 'lib')]
        if env.get('DYLD_LIBRARY_PATH'): runtime_dirs.append(env['DYLD_LIBRARY_PATH'])
        env['DYLD_LIBRARY_PATH'] = ':'.join(runtime_dirs)
        if args.local_rspduo_kit:
            root = binary.parent.parent
            env['VECTORWARP_MACOS_ROOT'] = str(root)
            builder = root / 'script/vectorwarp-build-sdrplay-macos.sh'
            env['VECTORWARP_MACOS_RSPDUO_BUILD'] = str(builder)
            # SDK and state paths are explicit operator inputs, never fetched
            # or inferred by this test. The source kit retains its SONAME links.
            for name in ('VECTORWARP_MACOS_STATE', 'BLAH2_SDRPLAY_INCLUDE_DIR', 'BLAH2_SDRPLAY_LIBRARY'):
                assert env.get(name), f'{name} must name a private test state/manual SDK path'
            subprocess.run([str(builder)], env=env, check=True, timeout=180)
            node = env.get('VECTORWARP_MACOS_NODE', 'node')
            status = json.loads(subprocess.check_output([node, '-e',
                'process.stdout.write(JSON.stringify(require(process.argv[1]).createMacRspduoBuild().status()))',
                str(root / 'api/macos-rspduo-build.js')], env=env, text=True))
            assert status['ok'] and status['state'] == 'current', status
        else:
            module_commands = subprocess.check_output(['otool', '-l',
                str(binary.parent / 'blah2-receiver-rspduo.dylib')], text=True)
            assert re.search(r'cmd LC_RPATH\s+cmdsize \d+\s+path /usr/local/lib ', module_commands), \
                'RSPduo module must find the official externally installed macOS runtime'
    report = json.loads(subprocess.check_output([str(binary), '--receiver-status'], text=True, env=env))
    available = {item['receiver']: item for item in report.get('receivers', []) if item.get('compiled') and item.get('moduleLoadable')}
    required = {'Usrp', 'HackRF'} | ({'RspDuo'} if args.include_rspduo else set())
    if not required <= available.keys(): raise SystemExit(f'compiled/loadable SDK adapters required: {available}')
    results = {}
    service_process = None
    try:
        if service:
            service_process = subprocess.Popen([str(service)], env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, start_new_session=True)
            time.sleep(5)
            if service_process.poll() is not None:
                raise AssertionError('The explicitly supplied SDRplay foreground service exited during startup')
        receivers = ('Usrp', 'HackRF') + (('RspDuo',) if args.include_rspduo else ())
        for receiver in receivers:
            results[receiver] = {'noDevice': no_device(binary, receiver, env)[-240:]}
            results[receiver]['replay'] = replay.run_case(binary, receiver, 2)
    finally:
        if service_process:
            if service_process.poll() is None:
                os.killpg(service_process.pid, signal.SIGTERM)
            try:
                service_process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(service_process.pid, signal.SIGKILL)
                service_process.communicate(timeout=5)
                raise AssertionError('The test-owned SDRplay service required SIGKILL')
    print(json.dumps({'acceptance':'macOS local SDK no-device then replay recovery','results':results}, separators=(',',':')))

if __name__ == '__main__': main()
