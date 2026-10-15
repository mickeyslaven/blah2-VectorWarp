#!/usr/bin/env python3
"""Exercise configuration effects with the real CPU processor and synthetic IQ.

All sockets bind loopback. No SDK capture or remote Kraken is used. API/DOM
matrices cover persistence/validation; this matrix checks observable outputs.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('replay', ROOT / 'test/recording/processor_replay_test.py')
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)


def set_field(config, dotted, value):
    parts = dotted.split('.')
    for part in parts[:-1]:
        config = config.setdefault(part, {})
    config[parts[-1]] = value


def write_input(filename, fmt, fs, fc, cpi, channels=2):
    if fmt in ('auto', 'blah2'):
        replay.write_blah2iq(filename, channels, fs, fc, frames=4, frame_samples=int(fs*cpi))
        return
    samples = int(fs*cpi)*4
    # Deterministic broadband signal; matching metadata and complete channel pairs.
    state = 173
    values = []
    for _ in range(2048):
        state = (1664525*state + 1013904223) & 0xffffffff
        values.append(((state >> 24) - 128) / 128)
    with filename.open('wb') as out:
        if fmt == 'mchq':
            block_samples = int(fs*cpi)
            assert block_samples <= 65536
            for offset in range(0, samples, block_samples):
                count = block_samples
                out.write(struct.pack('>8I', 0x4d434851, channels, count, 4, 0, 0, 0, 0))
                out.write(b''.join(struct.pack('<ff', fc, 15.) for _ in range(channels)))
                for ch in range(channels):
                    out.write(bytes(int(values[(i+ch*17) % len(values)]*100+127.5)
                                    for i in range(offset*2, (offset+count)*2)))
        elif fmt == 'usrp-blocks':
            for offset in range(0, samples, 1024):
                count = min(1024, samples-offset)
                for ch in range(channels):
                    out.write(b''.join(struct.pack('<ff', values[(i*2+ch*17) % 2048], values[(i*2+1+ch*17) % 2048])
                                       for i in range(offset, offset+count)))
        else:
            encoding, scale = ('<h', 32767) if fmt == 's16-interleaved' else ('<b', 127)
            for i in range(samples):
                for ch in range(channels):
                    for component in range(2):
                        out.write(struct.pack(encoding, int(values[(i*2+component+ch*17) % 2048]*scale)))


def run_case(binary, name, changes, receiver='Usrp', channels=2, gpu_capable=False):
    with tempfile.TemporaryDirectory(prefix='vectorwarp-config-runtime-') as temporary:
        work = Path(temporary)
        output_dir = work / 'output with spaces'
        output_dir.mkdir()
        recording = work / 'synthetic.iq'
        sinks, status = replay.SinkGroup(), replay.StatusServer()
        sinks.payloads.update({key: bytearray() for key in ('detection', 'track', 'iqdata')})
        process = None
        try:
            config = replay.config(receiver, channels, recording, status.port, sinks.ports())
            config['save']['path'] = str(output_dir) + '/'
            for key, value in changes.items():
                set_field(config, key, value)
            fs, fc = config['capture']['fs'], config['capture']['fc']
            cpi = config['process']['data']['cpi']
            fmt = config['capture']['replay']['format']
            write_input(recording, fmt, fs, fc, cpi, channels)
            config_path = work / 'settings.json'
            config_path.write_text(json.dumps(config))
            sinks.start()
            status.start()
            # File output avoids pipe backpressure hiding a processing failure.
            with (work / 'processor.log').open('w+') as log:
                process = subprocess.Popen([str(binary), '--config', str(config_path)], cwd=work,
                                           stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                completed = replay.wait_for(lambda: process.poll() is not None or
                    any(item.get('state') in ('complete', 'error') for item in status.snapshot()), time.monotonic()+30)
                if not completed or not any(item.get('state') == 'complete' for item in status.snapshot()):
                    log.seek(0)
                    raise AssertionError(f'{name}: did not complete: {status.snapshot()[-2:]} {log.read()[-3500:]}')
                assert not replay.stop_process(process), f'{name}: shutdown needed SIGKILL'
                assert process.returncode == 0, f'{name}: exit {process.returncode}'
                log.seek(0)
                logs = log.read()
            assert 'Setting up device' not in logs, 'Replay attempted live receiver initialization'
            replay.assert_processor_outputs(sinks)
            map_data = replay.first_json(sinks.payloads['map'], 'map')
            timing = replay.first_json(sinks.payloads['timing'], 'timing')
            ambiguity = config['process']['ambiguity']
            assert map_data['nCols'] == ambiguity['delayMax']-ambiguity['delayMin']+1
            assert len(map_data['delay']) == map_data['nCols']
            # Display serialization retains two decimal places.
            assert math.isclose(map_data['delay'][0], ambiguity['delayMin']*299792458/fs/1000, abs_tol=.01)
            assert map_data['doppler'][0] >= ambiguity['dopplerMin']-1
            assert map_data['doppler'][-1] <= ambiguity['dopplerMax']+1
            acceleration = timing['acceleration']
            requested = config['process']['performance']['acceleration']
            assert acceleration['requested'] == requested
            if not gpu_capable:
                assert acceleration['active'] == 'cpu'
                if requested != 'cpu': assert acceleration['state'] == 'fallback' and acceleration['reason']
            elif requested == 'cpu':
                assert (acceleration['active'], acceleration['state']) == ('cpu', 'selected')
            else:
                assert (acceleration['active'], acceleration['state']) in {
                    ('cpu', 'checking'), ('cpu', 'fallback'), ('vulkan', 'ready')}, acceleration
                if acceleration['active'] == 'cpu': assert acceleration['reason']
            clutter = timing['clutterAcceleration']
            if not gpu_capable:
                assert clutter['cpuExecuted'] == config['process']['clutter']['enable']
                assert not clutter['gpuExecuted']
            elif config['process']['clutter']['enable']:
                if clutter.get('state') == 'checking':
                    assert requested != 'cpu' and clutter['active'] == 'cpu' and clutter['reason'], clutter
                    assert clutter['cpuExecuted'] and clutter['gpuExecuted'], (name, clutter)
                else:
                    assert bool(clutter['cpuExecuted']) != bool(clutter['gpuExecuted']), (name, acceleration, clutter)
                    assert clutter['active'] == ('vulkan' if clutter['gpuExecuted'] else 'cpu'), clutter
            else: assert not clutter['cpuExecuted'] and not clutter['gpuExecuted']
            enabled = config['process']['detection']['enable']
            assert bool(sinks.bytes['detection']) == enabled
            assert bool(sinks.bytes['track']) == (enabled and config['process']['tracker']['enable'])
            for key in ('detection', 'track', 'iqdata'):
                if sinks.bytes[key]:
                    replay.assert_finite(replay.first_json(sinks.payloads[key], key), key)
            for extension, expected in [('map', config['save']['map']),
                                        ('detection', config['save']['detection'] and enabled)]:
                files = list(output_dir.glob('*.'+extension))
                assert bool(files) == expected, f'{name}: wrong saved {extension} files: {files}'
                if files:
                    assert files[0].stat().st_size > 0
                    replay.assert_finite(replay.first_json(files[0].read_bytes(), extension))
            return {'case': name, 'ok': True, 'format': fmt, 'settings': list(changes),
                    'mapShape': [map_data['nRows'], map_data['nCols']], 'acceleration': acceleration,
                    'clutterAcceleration': clutter}
        finally:
            if process is not None:
                replay.stop_process(process)
            sinks.close()
            status.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--gpu-capable', action='store_true',
                        help='allow qualified GPU execution or bounded first-frame qualification telemetry')
    args = parser.parse_args()
    binary = args.binary.resolve()
    cases = [
        ('detection-disabled-recording-requested', {'process.detection.enable': False, 'save.detection': True}),
        ('detector-tracker-save', {'process.detection.pfa': .05, 'process.detection.nGuard': 1,
            'process.detection.nTrain': 4, 'process.detection.nCentroid': 2,
            'process.detection.minDelay': 2, 'process.detection.minDoppler': 50,
            'process.tracker.enable': True, 'process.tracker.initiate.M': 2,
            'process.tracker.initiate.N': 3, 'process.tracker.initiate.maxAcc': 2,
            'process.tracker.delete': 3, 'save.map': True, 'save.detection': True}),
        ('geometry-buffer-clutter-threads', {'capture.fs': 4000000, 'capture.fc': 100000000,
            'process.data.cpi': .04, 'process.data.buffer': 3,
            'process.ambiguity.delayMin': -8, 'process.ambiguity.delayMax': 47,
            'process.ambiguity.dopplerMin': -800, 'process.ambiguity.dopplerMax': 800,
            'process.clutter.enable': True, 'process.clutter.delayMin': -2,
            'process.clutter.delayMax': 12, 'process.performance.fft_threads': 2}),
        ('auto-cpu-fallback', {'process.performance.acceleration': 'auto',
            'process.performance.surveillance_workers': 0, 'process.performance.fft_threads': 0}),
        ('requested-gpu-cpu-fallback', {'process.performance.acceleration': 'gpu', 'process.clutter.enable': True}),
    ]
    if args.gpu_capable:
        cases[3] = ('auto-backend-selection', cases[3][1])
        cases[4] = ('requested-gpu-telemetry', cases[4][1])
    results = [run_case(binary, name, changes, gpu_capable=args.gpu_capable) for name, changes in cases]
    for fmt in ('blah2', 's16-interleaved', 's8-interleaved', 'usrp-blocks', 'mchq'):
        changes = {'capture.replay.format': fmt}
        if fmt == 'usrp-blocks':
            changes.update({'capture.replay.legacy_block_samples': 1024, 'process.data.cpi': .0256})
        if fmt == 'mchq':
            changes['capture.fs'] = 2400000
        results.append(run_case(binary, 'replay-'+fmt, changes, gpu_capable=args.gpu_capable))
    # Local recording exercises built-in array code without any Kraken connection.
    results.append(run_case(binary, 'array-reference-workers', {
        'capture.device.surveillance_channels': [0, 1, 2],
        'process.performance.surveillance_workers': 2,
        'process.reference_synthesis.mode': 'array_eigenbeam',
        'process.reference_synthesis.channels': [0, 1, 2],
        'process.reference_synthesis.analysis_samples': 1024,
        'process.reference_synthesis.analysis_interval': 2,
        'process.reference_synthesis.power_iterations': 4,
        'process.reference_synthesis.covariance_smoothing': .4,
        'process.reference_synthesis.diagonal_loading': .01}, receiver='Kraken', channels=3, gpu_capable=args.gpu_capable))
    print(json.dumps({'acceptance': 'synthetic configuration runtime', 'count': len(results), 'cases': results}))


if __name__ == '__main__':
    main()
