"""Real CLI round-trip over independently synthesized, sealed IQ fixtures."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable, fixture = map(Path, sys.argv[1:])
    environment = {**os.environ, 'OMP_NUM_THREADS': '1', 'OPENBLAS_NUM_THREADS': '1',
                   'VECTORWARP_BENCH_RANGE_WORKERS': '0'}
    with tempfile.TemporaryDirectory(prefix='passive-isar-cli-') as temporary:
        evidence = Path(temporary) / 'evidence'
        subprocess.run([str(fixture), '--evidence-dir', str(evidence)],
                       env=environment, check=True, capture_output=True, timeout=20)
        arguments = [str(executable), '--channels', '0,1', '--anchor', '0',
                     '--projection', '1', '--rates', '0,-.0005,.0005', '--delay', '0',
                     '--residual-hz', ','.join(str(i / 4) for i in range(-16, 17))]
        frames = [str(evidence / f'0-{i}.vwrps') for i in range(1, 9)]
        result = subprocess.run(arguments + frames, env=environment,
                                capture_output=True, check=True, timeout=20)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        assert len(records) == 9 and records[-1]['record'] == 'complete'
        complete = records[-1]
        assert complete['offline_only'] and not complete['metric_image'] and not complete['null_calibrated']
        assert complete['unique_blocks'] == '73' and complete['evaluated_diagnostic_cells'] == '99'
        assert sum(block['owned'] for frame in records[:-1] for block in frame['blocks']) == 73
        assert records[0]['first_sample'] == '9007199254741001'
        digest = hashlib.sha256(executable.read_bytes()).hexdigest()
        for frame in records[:-1]:
            assert frame['executable_sha256'] == digest
            assert len(frame['source_block_sha256_in_BlockKind_order']) == 20
            assert len(frame['channels']) == 2
            assert frame['effective_occupied_bandwidth_hz'] is None
            for channel in frame['channels']:
                assert len(channel['caf_doppler_major']) == 17 * 49
                assert len(channel['range_slow_time_uncropped']) == 17 * 49
                assert [rate['delay_rate_bins_per_second'] for rate in channel['rates']] == [0, -.0005, .0005]
                assert all(len(rate['kernels_by_block']) == 17 for rate in channel['rates'])
        oracle = next(json.loads(line) for line in (evidence / 'synthetic-psf.jsonl').read_text().splitlines()
                      if json.loads(line)['scatterers'] == 1 and json.loads(line)['frames'] == 8)
        for actual, reference in zip(complete['spectra'][0]['amplitude_rectangular_sum_over_sqrt_unique_blocks'], oracle['bins']):
            expected = complex(reference[3], reference[4])
            assert abs(complex(*actual) - expected) <= 3e-6 * max(1, abs(expected))
        for changed, paths in ((arguments, frames[:1] * 2),
                               (arguments[:6] + ['99'] + arguments[7:], frames),
                               (arguments[:8] + ['.0005'] + arguments[9:], frames),
                               (arguments[:12] + ['-30,0,30'], frames)):
            refused = subprocess.run(changed + paths, env=environment,
                                     capture_output=True, timeout=20)
            assert refused.returncode and not refused.stdout, 'Invalid request emitted a complete/partial aperture'
    print('Passive ISAR real CLI export/oracle/refusal contract passed')


if __name__ == '__main__':
    main()
