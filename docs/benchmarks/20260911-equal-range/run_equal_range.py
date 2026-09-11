#!/usr/bin/env python3
"""Paired, paced real-IQ tests with one immutable excess-path window."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--bundle', type=Path, required=True)
parser.add_argument('--recording', type=Path, required=True)
parser.add_argument('--sha256', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cpu-budget', type=int, required=True)
parser.add_argument('--devices', nargs='+', required=True)
parser.add_argument('--extended', action='store_true')
args = parser.parse_args()
if args.cpu_budget not in (4, 8):
    parser.error('Use the verified four- or eight-physical-core budget')
args.output.mkdir()
with args.recording.open('rb') as source:
    if hashlib.file_digest(source, 'sha256').hexdigest() != args.sha256:
        raise SystemExit('Recording checksum mismatch')
base = json.loads((args.bundle / 'profile-example.json').read_text())
base.update(sample_rate=2400000, frequency=527000000, channels=5,
            delay_min=-10, delay_max=245, clutter_min=-10, clutter_max=200)
contract = dict(sample_rate_hz=2400000, rf_hz=527000000, delay_min=-10,
                delay_max=245, delay_bins=256,
                excess_path_min_km=-10 * 299792458 / 2400000 / 1000,
                excess_path_max_km=245 * 299792458 / 2400000 / 1000,
                input_sha256=args.sha256, cpu_budget=args.cpu_budget,
                frames_per_repeat=20, startup_frames_per_repeat=8, repeats=2,
                periodic_cpu_oracles=False,
                source_freeze_sha256='2239b08347ef1b2466e7f9a10a269ed397939cc3bcea2aa5acc29d68b32fc9bc',
                fork_source_commit='8fa12290c203d85d9b48f57a5a5c0a01a66f7551',
                paced=True, comparison='complex maps; known SNR correction is not bit-identical',
                future_work='Exact wider-Doppler CAF; no shortened delay windows in this campaign')
(args.output / 'contract.json').write_text(json.dumps(contract, indent=2))
cases = [('pair-200ms-800hz', 'pair', .2, 800),
         ('pair-200ms-2400hz', 'pair', .2, 2400),
         ('pair-200ms-4800hz', 'pair', .2, 4800),
         ('array-200ms-800hz', 'array', .2, 800)]
if args.extended:
    cases += [('pair-100ms-800hz', 'pair', .1, 800),
              ('pair-1s-4800hz', 'pair', 1., 4800),
              ('array-1s-2400hz', 'array', 1., 2400)]
env_base = dict(os.environ, LD_LIBRARY_PATH=str(args.bundle / 'bin'),
                OPENBLAS_NUM_THREADS='1', OMP_NUM_THREADS='1',
                BLAH2_BENCH_PACE='1', BLAH2_GPU_MEMORY_PATH='auto',
                BLAH2_GPU_DIAGNOSTICS='1')
results, exclusions = [], []
for name, profile, cpi, span in cases + [('unsupported-200ms-40000hz', 'pair', .2, 40000)]:
    config = dict(base, cpi=cpi, doppler_min=-span, doppler_max=span,
                  benchmark_workers=1 if profile == 'pair' else 4,
                  benchmark_fft_threads=args.cpu_budget if profile == 'pair' else args.cpu_budget // 4)
    config_path = args.output / (name + '.profile.json')
    config_path.write_text(json.dumps(config, indent=2))
    eligible = {}
    for engine in ('upstream', 'fast'):
        executable = args.bundle / ('bin/bench-' + engine)
        inspected = subprocess.run([str(executable), '--inspect-geometry', str(config_path), profile],
                    env=env_base, capture_output=True, text=True, check=False, timeout=10)
        if inspected.returncode:
            if not (name.startswith('unsupported-') and inspected.returncode == 1 and
                    'Delay limits exceed the correlation block' in inspected.stderr):
                raise RuntimeError(f'Unexpected geometry inspection failure: {inspected.stderr}')
            geometry = dict(supported=False, rejection=inspected.stderr.strip(),
                            returncode=inspected.returncode)
        else:
            geometry = json.loads(inspected.stdout)
        (args.output / f'{name}-{engine}.geometry.json').write_text(json.dumps(geometry, indent=2))
        eligible[engine] = geometry['supported']
        if not geometry['supported']:
            exclusions.append(dict(case=name, engine=engine, geometry=geometry))
            (args.output / 'exclusions.json').write_text(json.dumps(exclusions, indent=2))
    if not eligible['fast']:
        if not name.startswith('unsupported-'):
            raise SystemExit(f'Unexpected unsupported fork geometry: {name}')
        continue
    if name.startswith('unsupported-'):
        raise SystemExit('Expected full-window wide-Doppler rejection disappeared')
    variants = [('regular-blah2', 'upstream', 'cpu', 'auto')] if eligible['upstream'] else []
    variants += [('vectorwarp-cpu', 'fast', 'cpu', 'auto')]
    variants += [('vectorwarp-auto', 'fast', 'auto', device) for device in args.devices]
    golden = args.output / (name + '.maps')
    for repeat in range(2):
        for variant, engine, mode, device in (variants if repeat == 0 else list(reversed(variants))):
            label = f'{name}-{variant}-{device.replace(":", "-")}-r{repeat+1}'
            prefix = args.output / label
            executable = args.bundle / ('bin/bench-' + engine)
            command = [str(executable), str(args.recording), str(config_path), str(prefix),
                       profile, mode, '20', 'compare' if golden.exists() else 'write', str(golden)]
            env = dict(env_base, BLAH2_GPU_DEVICE=device)
            with executable.open('rb') as binary:
                binary_sha = hashlib.file_digest(binary, 'sha256').hexdigest()
            (args.output / (label + '.command.json')).write_text(json.dumps(dict(argv=command,
                binarySha256=binary_sha, inputSha256=args.sha256,
                environment={key: value for key, value in env.items()
                    if key.startswith(('BLAH2_', 'OPENBLAS_', 'OMP_'))}), indent=2))
            started = time.monotonic()
            with (args.output / (label + '.log')).open('x') as log:
                run = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
            record = dict(label=label, case=name, variant=variant, device=device,
                          repeat=repeat+1, returncode=run.returncode, elapsedSeconds=time.monotonic()-started)
            summary_path = Path(str(prefix) + '.summary.json')
            if run.returncode == 0 and summary_path.exists():
                record.update(json.loads(summary_path.read_text()))
                with Path(str(prefix) + '.frames.csv').open() as frames:
                    steady = [row for row in csv.DictReader(frames) if row['phase'] == 'steady']
                if variant == 'vectorwarp-auto' and any('cpu_oracle' in row['clutter_backend'] for row in steady):
                    raise RuntimeError('Unexpected periodic CPU oracle in qualified steady processing')
                record['stages'] = {key: statistics.mean(float(row[key]) for row in steady) for key in
                    ['extract_ms', 'reference_ms', 'spectrum_ms', 'clutter_ms', 'ambiguity_ms',
                     'fusion_ms', 'detection_ms', 'tracker_ms', 'json_ms']}
                record['steady_actual_backend_counts'] = {
                    stage: {value: sum(row[stage] == value for row in steady)
                            for value in sorted(set(row[stage] for row in steady))}
                    for stage in ['backend', 'clutter_backend']}
            results.append(record)
            (args.output / 'summary.json').write_text(json.dumps(results, indent=2))
            print(json.dumps({key: record.get(key) for key in
                ['label', 'returncode', 'steady_dsp_mean_ms', 'steady_dsp_p95_ms',
                 'steady_dsp_deadline_misses', 'steady_actual_backend_counts']}), flush=True)
            if run.returncode:
                raise SystemExit(run.returncode)
