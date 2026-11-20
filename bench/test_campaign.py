"""Exercise fixed-range campaign evidence with mocked processes; no IQ/GPU workload."""
import argparse
import contextlib
import csv
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import campaign


ROOT = Path(__file__).resolve().parents[1]
HOSTS = {
    'strix': (8, ['4098:5510:0'], True),
    'nvidia': (4, ['4318:10401:0'], False),
    'pavilion': (4, ['32902:22811:0', '4098:27039:1'], False),
}


class CampaignTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.bundle = self.root / 'bundle'
        self.before = self.root / 'before'
        for bundle in (self.bundle, self.before):
            (bundle / 'bin').mkdir(parents=True)
            for engine in ('upstream', 'fast'):
                (bundle / f'bin/bench-{engine}').write_bytes(f'fake {bundle.name} {engine}'.encode())
        (self.bundle / 'profile-example.json').write_bytes((ROOT / 'bench/profile-example.json').read_bytes())
        self.recording = self.root / 'input.mchq'
        self.recording.write_bytes(b'not IQ: subprocess.run is mocked')
        self.digest = hashlib.sha256(self.recording.read_bytes()).hexdigest()
        self.calls = []
        self.fault = None

    def args(self, name='equal-range', host='nvidia'):
        cpu, devices, extended = HOSTS[host]
        parent = self.root / name
        parent.mkdir(exist_ok=True)
        return argparse.Namespace(campaign=name, bundle=self.bundle, recording=self.recording,
            sha256=self.digest, output=parent / host, cpu_budget=cpu, devices=devices,
            extended=extended, before_bundle=self.before if name == 'efficiency' else None)

    def process(self, argv, **kwargs):
        self.calls.append((argv, kwargs))
        binary = Path(argv[0])
        if binary.name == 'testClutterAcceleration':
            return argparse.Namespace(returncode=1 if self.fault == 'preflight' else 0)
        if argv[1] == '--inspect-geometry':
            case = Path(argv[2]).name.removesuffix('.profile.json')
            if case.startswith('unsupported-') and self.fault != 'missing-rejection':
                return argparse.Namespace(returncode=1, stdout='', stderr='Delay limits exceed the correlation block')
            if self.fault == 'geometry':
                return argparse.Namespace(returncode=3, stdout='', stderr='unexpected geometry error')
            supported = binary.name == 'bench-fast' or case in (
                'pair-200ms-800hz', 'pair-200ms-2400hz', 'pair-100ms-800hz')
            return argparse.Namespace(returncode=0, stdout=json.dumps({'supported': supported}), stderr='')
        self.assertEqual(kwargs['timeout'], 120)
        self.assertEqual(argv[6], '20')
        prefix, golden = Path(argv[3]), Path(argv[8])
        if self.fault == 'process':
            return argparse.Namespace(returncode=7)
        if argv[7] == 'write':
            self.assertFalse(golden.exists())
            golden.write_bytes(b'fake map')
        else:
            self.assertEqual(argv[7], 'compare')
            self.assertEqual(golden.read_bytes(), b'fake map')
        second = prefix.name.endswith('-r2')
        gpu = argv[5] == 'auto'
        frames = [dict(phase='steady' if i >= 8 else 'warmup',
                       pipeline_ms=i + 1 + 20 * second, backend='vulkan' if gpu else 'cpu',
                       clutter_backend='vulkan_fft' if gpu else 'cpu',
                       **dict.fromkeys(campaign.ERRORS, 1e-5),
                       **{stage: number + 1 for number, stage in enumerate(campaign.STAGES)})
                  for i in range(20)]
        if self.fault == 'oracle' and gpu:
            frames[8]['clutter_backend'] = 'vulkan_fft_cpu_oracle'
        with Path(str(prefix) + '.frames.csv').open('w') as stream:
            writer = csv.DictWriter(stream, fieldnames=frames[0])
            writer.writeheader()
            writer.writerows(frames)
        config = json.loads(Path(argv[2]).read_text())
        summary = dict(sample_clock_paced=True, delay_bins=256, frames=20, steady_frames=12,
                       steady_dsp_mean_ms=14.5 + 20 * second, steady_dsp_p95_ms=19.45 + 20 * second,
                       requested_cpi_ms=config['cpi'] * 1000, steady_dsp_deadline_misses=0)
        Path(str(prefix) + '.summary.json').write_text(json.dumps(summary))
        Path(str(prefix) + '.outputs.jsonl').write_text('{"detections": [], "tracks": []}\n')
        return argparse.Namespace(returncode=0)

    def run_campaign(self, args):
        with patch.object(campaign.subprocess, 'run', side_effect=self.process), contextlib.redirect_stdout(io.StringIO()):
            campaign.run(args)
        return json.loads((args.output / 'summary.json').read_text())

    def summarize(self, name):
        with contextlib.redirect_stdout(io.StringIO()):
            campaign.summarize(self.root / name, campaign.CAMPAIGNS[name])
        return json.loads((self.root / name / 'comparison.json').read_text())

    def test_both_complete_host_matrices_preserve_schema_and_controls(self):
        for name, spec in campaign.CAMPAIGNS.items():
            retained = json.loads((ROOT / f'docs/benchmarks/20260911-{name}/comparison.json').read_text())
            for host in HOSTS:
                with self.subTest(campaign=name, host=host):
                    self.calls = []
                    args = self.args(name, host)
                    runs = self.run_campaign(args)
                    self.assertEqual(len(runs), retained['receipts'][host]['measurement_runs'])
                    contract = json.loads((args.output / 'contract.json').read_text())
                    expected = dict(retained['receipts'][host]['contract'], input_sha256=self.digest)
                    self.assertEqual(contract, expected)
                    preflights = [(cmd[1], kw['env']['BLAH2_GPU_MEMORY_PATH']) for cmd, kw in self.calls
                                  if Path(cmd[0]).name == 'testClutterAcceleration']
                    self.assertEqual(preflights, [(d, p) for d in args.devices for p in ('auto', 'staged')]
                                     if name == 'efficiency' else [])
                    cases = campaign.CASES + (campaign.EXTENDED_CASES if args.extended else [])
                    for case, profile, cpi, span in cases:
                        config = json.loads((args.output / f'{case}.profile.json').read_text())
                        self.assertEqual((config['sample_rate'], config['frequency'], config['channels']), (2400000, 527000000, 5))
                        self.assertEqual((config['delay_min'], config['delay_max'], config['clutter_min'], config['clutter_max']), (-10, 245, -10, 200))
                        self.assertEqual((config['cpi'], config['doppler_min'], config['doppler_max']), (cpi, -span, span))
                        workers = 1 if profile == 'pair' else 4
                        self.assertEqual((config['benchmark_workers'], config['benchmark_fft_threads']), (workers, args.cpu_budget // workers))
                        first = [(r['variant'], r['device']) for r in runs if r['case'] == case and r['repeat'] == 1]
                        second = [(r['variant'], r['device']) for r in runs if r['case'] == case and r['repeat'] == 2]
                        self.assertEqual(first, list(reversed(second)))
                    for item in runs:
                        command = json.loads((args.output / (item['label'] + '.command.json')).read_text())
                        self.assertEqual(set(command), {'argv', 'binarySha256', 'inputSha256', 'environment'})
                        binary = Path(command['argv'][0])
                        self.assertEqual(command['binarySha256'], hashlib.sha256(binary.read_bytes()).hexdigest())
                        self.assertEqual(command['inputSha256'], self.digest)
                        before = item['variant'].startswith('vectorwarp-before-')
                        self.assertEqual(binary.parent.parent, self.before if before else self.bundle)
                        if before:
                            self.assertIn(item['case'], campaign.BEFORE_CASES)
                        for key in ('BLAH2_BENCH_PACE', 'BLAH2_GPU_DIAGNOSTICS', 'OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS'):
                            self.assertEqual(command['environment'][key], '1')
                    excluded = json.loads((args.output / 'exclusions.json').read_text())
                    self.assertEqual({e['engine'] for e in excluded if e['case'].startswith('unsupported-')}, {'upstream', 'fast'})
                    self.assertFalse(any(r['case'].startswith('unsupported-') for r in runs))
            actual = self.summarize(name)
            self.assertEqual(list(actual), list(retained))
            self.assertEqual(len(actual['rows']), len(retained['rows']))
            identity = ('host', 'case', 'variant', 'device', 'cpi_ms', 'doppler_half_span_hz',
                        'delay_bins', 'max_excess_path_km', 'frames')
            self.assertEqual([[row[key] for key in identity] for row in actual['rows']],
                             [[row[key] for key in identity] for row in retained['rows']])
            for row in actual['rows']:
                self.assertEqual(list(row), list(retained['rows'][0]))
                self.assertEqual(row['frames'], 24)
                self.assertEqual(row['mean_ms'], 24.5)
                self.assertAlmostEqual(row['p95_ms'], 38.85)
                self.assertEqual(row['max_ms'], 40)
                self.assertEqual(row['repeat_means_ms'], [14.5, 34.5])
                self.assertEqual(row['deadline_misses'], 0)
                self.assertEqual(row['cpu_oracle_frames'], 0)
            with (self.root / name / 'comparison.csv').open() as stream:
                header = next(csv.reader(stream))
            with (ROOT / f'docs/benchmarks/20260911-{name}/comparison.csv').open() as stream:
                self.assertEqual(header, next(csv.reader(stream)))

    def test_refusals_stop_without_claiming_success(self):
        for fault, name, exception, message in (
                ('preflight', 'efficiency', SystemExit, 'preflight failed'),
                ('geometry', 'equal-range', RuntimeError, 'Unexpected geometry'),
                ('missing-rejection', 'equal-range', SystemExit, 'rejection disappeared'),
                ('oracle', 'equal-range', RuntimeError, 'periodic CPU oracle'),
                ('process', 'equal-range', SystemExit, '7')):
            with self.subTest(fault=fault):
                args = self.args(name)
                args.output = self.root / fault
                self.fault = fault
                self.calls = []
                with self.assertRaisesRegex(exception, message):
                    self.run_campaign(args)
                self.assertFalse((args.output / 'comparison.json').exists())
                if fault == 'preflight':
                    self.assertEqual(len(self.calls), 1)
                if fault == 'process':
                    self.assertEqual(json.loads((args.output / 'summary.json').read_text())[0]['returncode'], 7)

    def test_recording_hash_mismatch_never_starts_process(self):
        args = self.args()
        args.sha256 = '0' * 64
        with self.assertRaisesRegex(SystemExit, 'checksum mismatch'):
            self.run_campaign(args)
        self.assertEqual(self.calls, [])

    def test_summary_rejects_changed_contracts_before_writing(self):
        args = self.args('efficiency')
        self.run_campaign(args)
        path = args.output / 'contract.json'
        original = json.loads(path.read_text())
        for key, value in (('delay_max', 244), ('sample_rate_hz', 1), ('periodic_cpu_oracles', True),
                           ('fork_source_commit', 'wrong'), ('source_freeze_sha256', 'wrong'),
                           ('before_source_commit', 'wrong')):
            with self.subTest(key=key):
                path.write_text(json.dumps(dict(original, **{key: value})))
                with self.assertRaises(ValueError):
                    self.summarize('efficiency')
                self.assertFalse((args.output.parent / 'comparison.json').exists())

    def test_summary_checks_before_cpu_outputs_in_both_orders(self):
        args = self.args('efficiency')
        self.run_campaign(args)
        for repeat in (1, 2):
            path = args.output / f'pair-200ms-800hz-vectorwarp-before-cpu-auto-r{repeat}.outputs.jsonl'
            original = path.read_bytes()
            path.write_bytes(b'changed detection or track')
            with self.assertRaisesRegex(ValueError, 'CPU output changed'):
                self.summarize('efficiency')
            path.write_bytes(original)

    def test_summary_rejects_incomplete_runs_and_bad_frame_evidence(self):
        args = self.args()
        runs = self.run_campaign(args)
        summary_path = args.output / 'summary.json'
        summary_path.write_text(json.dumps(runs[:-1]))
        with self.assertRaisesRegex(ValueError, 'expected 20 runs'):
            self.summarize('equal-range')
        summary_path.write_text(json.dumps(runs))
        frame_path = args.output / (runs[0]['label'] + '.frames.csv')
        with frame_path.open() as stream:
            frames = list(csv.DictReader(stream))
        for key, value, message in (('map_rms_relative', 'nan', 'Complex-map'),
                                    ('map_peak_relative', '0.001', 'Complex-map'),
                                    ('phase', 'warmup', 'Steady frame'),
                                    ('pipeline_ms', '999', 'Mean timing')):
            with self.subTest(key=key):
                changed = [dict(row) for row in frames]
                changed[8][key] = value
                with frame_path.open('w') as stream:
                    writer = csv.DictWriter(stream, fieldnames=frames[0])
                    writer.writeheader()
                    writer.writerows(changed)
                with self.assertRaisesRegex(ValueError, message):
                    self.summarize('equal-range')
                self.assertFalse((args.output.parent / 'comparison.json').exists())

    def test_cli_refuses_wrong_campaign_source_before_output(self):
        args = self.args()
        base = ['run', 'efficiency', '--bundle', str(self.bundle), '--recording', str(self.recording),
                '--sha256', self.digest, '--output', str(args.output), '--cpu-budget', '4', '--devices', 'auto']
        for options in ([], ['--source-commit', 'wrong', '--source-sha256', 'wrong'],
                        ['--cpu-budget', '2']):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                campaign.main(base + options)
            self.assertEqual(caught.exception.code, 2)
            self.assertFalse(args.output.exists())

    def test_canonical_profile_identity_for_retained_evidence(self):
        self.assertEqual(hashlib.sha256((ROOT / 'bench/profile-example.json').read_bytes()).hexdigest(),
                         'a01d5b789cb17b0c8a500019954c4c546df53968801cf18a1396fb8ed317434e')


if __name__ == '__main__':
    unittest.main()
