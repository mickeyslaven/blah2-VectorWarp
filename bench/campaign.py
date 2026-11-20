#!/usr/bin/env python3
"""Reproduce/reconcile the retained fixed-range campaigns; see docs/benchmarks/README.md."""
import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time

from summarize import percentile


@dataclass(frozen=True)
class Campaign:
    source_commit: str
    source_sha256: str
    run_counts: dict
    before_commit: str = None


CAMPAIGNS = {
    'equal-range': Campaign(
        '8fa12290c203d85d9b48f57a5a5c0a01a66f7551',
        '2239b08347ef1b2466e7f9a10a269ed397939cc3bcea2aa5acc29d68b32fc9bc',
        {'strix': 34, 'nvidia': 20, 'pavilion': 28}),
    'efficiency': Campaign(
        '8ba6e1330fad9fcdbe8a5a54134a712d0959775e',
        'ccbd2ec1c379310b0df1be1857570d93dd5306a6b435c63c8e785b7c8084dab6',
        {'strix': 42, 'nvidia': 28, 'pavilion': 40},
        '2a9bfdf6d56f63e40d916a9176b40d2ddbeb459e'),
}
CASES = [('pair-200ms-800hz', 'pair', .2, 800),
         ('pair-200ms-2400hz', 'pair', .2, 2400),
         ('pair-200ms-4800hz', 'pair', .2, 4800),
         ('array-200ms-800hz', 'array', .2, 800)]
EXTENDED_CASES = [('pair-100ms-800hz', 'pair', .1, 800),
                  ('pair-1s-4800hz', 'pair', 1., 4800),
                  ('array-1s-2400hz', 'array', 1., 2400)]
BEFORE_CASES = ('pair-200ms-800hz', 'pair-200ms-2400hz')
UNSUPPORTED_CASE = ('unsupported-200ms-40000hz', 'pair', .2, 40000)
STAGES = ('extract_ms', 'reference_ms', 'spectrum_ms', 'clutter_ms', 'ambiguity_ms',
          'fusion_ms', 'detection_ms', 'tracker_ms', 'json_ms')
ERRORS = ('map_rms_relative', 'map_peak_relative', 'fusion_rms_relative', 'fusion_peak_relative')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def contract_for(args, campaign):
    contract = dict(sample_rate_hz=2400000, rf_hz=527000000, delay_min=-10,
                    delay_max=245, delay_bins=256,
                    excess_path_min_km=-10 * 299792458 / 2400000 / 1000,
                    excess_path_max_km=245 * 299792458 / 2400000 / 1000,
                    input_sha256=args.sha256, cpu_budget=args.cpu_budget,
                    frames_per_repeat=20, startup_frames_per_repeat=8, repeats=2,
                    periodic_cpu_oracles=False,
                    source_freeze_sha256=campaign.source_sha256,
                    fork_source_commit=campaign.source_commit)
    if campaign.before_commit:
        contract['before_source_commit'] = campaign.before_commit if args.before_bundle else None
    contract.update(paced=True, comparison='complex maps; known SNR correction is not bit-identical',
                    future_work='Exact wider-Doppler CAF; no shortened delay windows in this campaign')
    return contract


def run(args):
    campaign = CAMPAIGNS[args.campaign]
    args.output.mkdir()
    with args.recording.open('rb') as source:
        if hashlib.file_digest(source, 'sha256').hexdigest() != args.sha256:
            raise SystemExit('Recording checksum mismatch')
    base = json.loads((args.bundle / 'profile-example.json').read_text())
    base.update(sample_rate=2400000, frequency=527000000, channels=5,
                delay_min=-10, delay_max=245, clutter_min=-10, clutter_max=200)
    (args.output / 'contract.json').write_text(json.dumps(contract_for(args, campaign), indent=2))
    cases = CASES + (EXTENDED_CASES if args.extended else []) + [UNSUPPORTED_CASE]
    env_base = dict(os.environ, LD_LIBRARY_PATH=str(args.bundle / 'bin'),
                    OPENBLAS_NUM_THREADS='1', OMP_NUM_THREADS='1',
                    BLAH2_BENCH_PACE='1', BLAH2_GPU_MEMORY_PATH='auto',
                    BLAH2_GPU_DIAGNOSTICS='1')
    if campaign.before_commit:
        for device in args.devices:
            for memory_path in ('auto', 'staged'):
                label = f'preflight-clutter-{device.replace(":", "-")}-{memory_path}'
                with (args.output / (label + '.log')).open('x') as log:
                    preflight = subprocess.run([str(args.bundle / 'bin/testClutterAcceleration'), device],
                        env=dict(env_base, BLAH2_GPU_MEMORY_PATH=memory_path),
                        stdout=log, stderr=subprocess.STDOUT, timeout=90)
                if preflight.returncode:
                    raise SystemExit(f'GPU numerical preflight failed: {label}')
                print(f'PASS {label}', flush=True)
    results, exclusions = [], []
    for name, profile, cpi, span in cases:
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
        if args.before_bundle and name in BEFORE_CASES:
            variants += [('vectorwarp-before-cpu', 'fast', 'cpu', 'auto')]
            variants += [('vectorwarp-before-auto', 'fast', 'auto', device) for device in args.devices]
        golden = args.output / (name + '.maps')
        for repeat in range(2):
            for variant, engine, mode, device in (variants if repeat == 0 else list(reversed(variants))):
                label = f'{name}-{variant}-{device.replace(":", "-")}-r{repeat+1}'
                prefix = args.output / label
                before = variant.startswith('vectorwarp-before-')
                executable = (args.before_bundle if before else args.bundle) / ('bin/bench-' + engine)
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
                    process = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
                record = dict(label=label, case=name, variant=variant, device=device,
                              repeat=repeat+1, returncode=process.returncode, elapsedSeconds=time.monotonic()-started)
                summary_path = Path(str(prefix) + '.summary.json')
                if process.returncode == 0 and summary_path.exists():
                    record.update(json.loads(summary_path.read_text()))
                    with Path(str(prefix) + '.frames.csv').open() as frames:
                        steady = [row for row in csv.DictReader(frames) if row['phase'] == 'steady']
                    if variant == 'vectorwarp-auto' and any('cpu_oracle' in row['clutter_backend'] for row in steady):
                        raise RuntimeError('Unexpected periodic CPU oracle in qualified steady processing')
                    record['stages'] = {key: statistics.mean(float(row[key]) for row in steady) for key in STAGES}
                    record['steady_actual_backend_counts'] = {
                        stage: {value: sum(row[stage] == value for row in steady)
                                for value in sorted(set(row[stage] for row in steady))}
                        for stage in ('backend', 'clutter_backend')}
                    if variant == 'vectorwarp-before-cpu':
                        current = args.output / f'{name}-vectorwarp-cpu-auto-r{repeat+1}.outputs.jsonl'
                        if current.exists():
                            if current.read_bytes() != Path(str(prefix) + '.outputs.jsonl').read_bytes():
                                raise RuntimeError('CPU detection/tracking output changed from the previous fork')
                results.append(record)
                (args.output / 'summary.json').write_text(json.dumps(results, indent=2))
                print(json.dumps({key: record.get(key) for key in
                    ('label', 'returncode', 'steady_dsp_mean_ms', 'steady_dsp_p95_ms',
                     'steady_dsp_deadline_misses', 'steady_actual_backend_counts')}), flush=True)
                if process.returncode:
                    raise SystemExit(process.returncode)


def summarize(root, campaign):
    """Validate full per-host evidence before writing the historical comparison schema."""
    all_rows, receipts = [], {}
    for host, expected_runs in campaign.run_counts.items():
        folder = root / host
        if not (folder / 'summary.json').exists():
            continue
        contract = json.loads((folder / 'contract.json').read_text())
        require((contract['delay_min'], contract['delay_max'], contract['delay_bins']) == (-10, 245, 256), 'Delay window changed')
        require(contract['sample_rate_hz'] == 2400000 and contract['rf_hz'] == 527000000, 'Recording geometry changed')
        require(contract['periodic_cpu_oracles'] is False, 'Periodic CPU oracles enabled')
        require(contract['fork_source_commit'] == campaign.source_commit and
                contract['source_freeze_sha256'] == campaign.source_sha256, 'Campaign source identity changed')
        runs = json.loads((folder / 'summary.json').read_text())
        require(len(runs) == expected_runs, f'{host}: expected {expected_runs} runs, got {len(runs)}')
        if campaign.before_commit:
            require(contract['before_source_commit'] == campaign.before_commit, 'Before source identity changed')
            for case in BEFORE_CASES:
                for repeat in (1, 2):
                    old = folder / f'{case}-vectorwarp-before-cpu-auto-r{repeat}.outputs.jsonl'
                    new = folder / f'{case}-vectorwarp-cpu-auto-r{repeat}.outputs.jsonl'
                    require(old.read_bytes() == new.read_bytes(), f'{host}, {case}, {repeat}: CPU output changed')
        groups = {}
        for item in runs:
            require(item['returncode'] == 0 and item['sample_clock_paced'] is True, 'Failed or unpaced run')
            require(item['delay_bins'] == 256 and item['frames'] == 20 and item['steady_frames'] == 12, 'Run frame/window count changed')
            config = json.loads((folder / (item['case'] + '.profile.json')).read_text())
            require((config['delay_min'], config['delay_max'], config['clutter_min'], config['clutter_max']) == (-10, 245, -10, 200), 'Profile window changed')
            with (folder / (item['label'] + '.frames.csv')).open() as source:
                frames = list(csv.DictReader(source))
            require(len(frames) == 20, 'Frame CSV count changed')
            steady = [frame for frame in frames if frame['phase'] == 'steady']
            require(len(steady) == 12, 'Steady frame count changed')
            if item['variant'] == 'vectorwarp-auto':
                require(all('cpu_oracle' not in frame['clutter_backend'] for frame in steady), 'Periodic CPU oracle in steady frames')
            timings = [float(frame['pipeline_ms']) for frame in steady]
            require(abs(statistics.mean(timings) - item['steady_dsp_mean_ms']) < 1e-6, 'Mean timing differs from frames')
            require(sum(value > item['requested_cpi_ms'] for value in timings) == item['steady_dsp_deadline_misses'], 'Deadline count differs from frames')
            require(all(math.isfinite(float(frame[key])) and float(frame[key]) <= 1e-4
                        for frame in frames for key in ERRORS), 'Complex-map comparison failed')
            key = (item['case'], item['variant'], item['device'])
            group = groups.setdefault(key, dict(frames=[], repeats=[], summaries=[], config=config))
            group['frames'].extend(steady)
            group['repeats'].append(item['repeat'])
            group['summaries'].append(item)
        for (case, variant, device), group in groups.items():
            require(sorted(group['repeats']) == [1, 2], 'Paired repetitions missing or duplicated')
            frames = group['frames']
            timings = [float(frame['pipeline_ms']) for frame in frames]
            cpi_ms = group['config']['cpi'] * 1000
            row = dict(host=host, case=case, variant=variant, device=device, cpi_ms=cpi_ms,
                       doppler_half_span_hz=group['config']['doppler_max'], delay_bins=256,
                       max_excess_path_km=contract['excess_path_max_km'], frames=len(frames),
                       mean_ms=statistics.mean(timings), p95_ms=percentile(timings, .95),
                       max_ms=max(timings), deadline_misses=sum(value > cpi_ms for value in timings),
                       mean_capacity=cpi_ms / statistics.mean(timings),
                       gpu_dd_frames=sum(frame['backend'] == 'vulkan' for frame in frames),
                       gpu_clutter_frames=sum(frame['clutter_backend'].startswith('vulkan_fft') for frame in frames),
                       cpu_oracle_frames=sum('cpu_oracle' in frame['clutter_backend'] for frame in frames),
                       max_complex_error=max(float(frame[key]) for frame in frames for key in ERRORS),
                       repeat_means_ms=[item['steady_dsp_mean_ms'] for item in sorted(group['summaries'], key=lambda item:item['repeat'])],
                       stages={key: statistics.mean(float(frame[key]) for frame in frames) for key in STAGES})
            all_rows.append(row)
        receipts[host] = dict(contract=contract, measurement_runs=len(runs),
                              complete_cpis=len(runs) * 20, measured_steady_cpis=len(runs) * 12,
                              processing_acceptance='all measured runs passed',
                              runner_exit_note='All timed runs and the campaign completed successfully; the unsupported full-range 40-kHz profile was rejected before processing.')
    require(bool(all_rows), 'No complete host evidence found')
    (root / 'comparison.json').write_text(json.dumps(dict(receipts=receipts, rows=all_rows), indent=2))
    fields = [key for key in all_rows[0] if key not in ('stages', 'repeat_means_ms')]
    with (root / 'comparison.csv').open('w') as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction='ignore', lineterminator='\n')
        writer.writeheader()
        writer.writerows(all_rows)
    print(json.dumps(dict(hosts=list(receipts), measurement_runs=sum(r['measurement_runs'] for r in receipts.values()),
                         matched_groups=len(all_rows)), indent=2))
    for row in all_rows:
        print(f"{row['host']:8} {row['case']:23} {row['variant']:16} {row['device']:15} "
              f"{row['mean_ms']:8.3f} ms p95={row['p95_ms']:8.3f} miss={row['deadline_misses']}/24 "
              f"GPUdd={row['gpu_dd_frames']} GPUclutter={row['gpu_clutter_frames']}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    runner = commands.add_parser('run', help='Run a campaign in a fresh external output directory')
    runner.add_argument('campaign', choices=CAMPAIGNS)
    runner.add_argument('--bundle', type=Path, required=True)
    runner.add_argument('--recording', type=Path, required=True)
    runner.add_argument('--sha256', required=True)
    runner.add_argument('--output', type=Path, required=True)
    runner.add_argument('--cpu-budget', type=int, choices=(4, 8), required=True)
    runner.add_argument('--devices', nargs='+', required=True)
    runner.add_argument('--extended', action='store_true')
    runner.add_argument('--source-commit')
    runner.add_argument('--source-sha256')
    runner.add_argument('--before-bundle', type=Path)
    summary = commands.add_parser('summarize', help='Reconcile retained per-host raw evidence')
    summary.add_argument('campaign', choices=CAMPAIGNS)
    summary.add_argument('root', type=Path)
    args = parser.parse_args(argv)
    campaign = CAMPAIGNS[args.campaign]
    if args.action == 'summarize':
        summarize(args.root, campaign)
        return
    if campaign.before_commit and (not args.source_commit or not args.source_sha256):
        runner.error('efficiency requires --source-commit and --source-sha256')
    if ((args.source_commit and args.source_commit != campaign.source_commit) or
            (args.source_sha256 and args.source_sha256 != campaign.source_sha256)):
        runner.error('Source identity must match the selected campaign')
    if args.before_bundle and not campaign.before_commit:
        runner.error('--before-bundle is only supported by the efficiency campaign')
    run(args)


if __name__ == '__main__':
    main()
