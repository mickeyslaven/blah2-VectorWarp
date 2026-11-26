"""Check recorded export algebra/support, not target truth or imaging quality."""
import cmath
import hashlib
import json
import math
from pathlib import Path
import sys


def qualify(plan_path, directory):
    plan_bytes = plan_path.read_bytes()
    plan = json.loads(plan_bytes)
    assert plan['depths'] == [2, 4, 8] and plan['delay_rates_bins_per_second'] == [0]
    assert not plan['promotion_eligible'] and not plan['metric_image']
    results = []
    for depth in plan['depths']:
        path = directory / f'aperture-{depth}.jsonl'
        assert path.stat().st_size < 128 * 1024 * 1024
        digest = hashlib.sha256()
        profiles, times, gauges, parents = [], [], [], []
        first = end = None
        complete = None
        cpi_count = 0
        max_inverse_error = 0.
        pilot_minimum = 1.
        with path.open('rb') as source:
            for line in source:
                assert len(line) < 16 * 1024 * 1024
                digest.update(line)
                record = json.loads(line)
                if record['record'] == 'complete':
                    assert complete is None
                    complete = record
                    continue
                assert complete is None and record['record'] == 'frame'
                assert record['origin'] == 'authenticated_derived_iq_not_native_snapshot'
                assert record['derived_plan_sha256'] == plan['input_plan_sha256']
                assert record['extraction_receipt_sha256'] == plan['input_receipt_sha256']
                assert record['source_config_wire_hex'] is None and record['carrier_hz'] is None
                assert len(record['channels']) == 1 and record['channels'][0]['physical_channel'] == '0'
                start = int(record['first_sample'])
                if first is None:
                    first = start
                else:
                    assert start == end
                end = int(record['end_sample'])
                assert end - start == 240000 and int(record['processed_end']) == start + 239943
                assert record['unprocessed_tail_samples'] == '57'
                assert int(record['missing_support_before_samples']) == (57 if cpi_count else 0)
                assert int(record['sequence']) == cpi_count + 1
                channel = record['channels'][0]
                delays, frequencies = channel['delay_samples'], channel['doppler_hz']
                rows, columns = len(frequencies), len(delays)
                assert rows == 121 and columns == 257 and frequencies[rows // 2] == 0
                column = delays.index(plan['prescribed_delay_samples'])
                caf = [complex(*z) for z in channel['caf_doppler_major']]
                slow = [complex(*z) for z in channel['range_slow_time_uncropped']]
                assert len(caf) == len(slow) == rows * columns
                # Independent inverse of the retained pre-temporal CAF, at
                # every used block of the prescribed coordinate.
                local = []
                for block in range(rows):
                    expected = sum(caf[f * columns + column] * cmath.exp(
                        2j * math.pi * (f - rows // 2) * block / rows) for f in range(rows)) / rows
                    actual = slow[block * columns + column]
                    max_inverse_error = max(max_inverse_error, abs(actual - expected) / max(1, abs(expected)))
                    local.append(actual)
                gauge = complex(*record['gauge_correction_not_applied_to_export'])
                correlation = complex(*record['pilot_correlation'])
                assert abs(gauge - correlation.conjugate() / abs(correlation)) < 1e-12
                assert record['pilot_coherence'] >= record['minimum_pilot_coherence']
                pilot_minimum = min(pilot_minimum, record['pilot_coherence'])
                assert len(record['blocks']) == rows and all(b['owned'] for b in record['blocks'])
                for b, block in enumerate(record['blocks']):
                    assert int(block['first']) == start + b * 1983 and int(block['end']) == start + (b + 1) * 1983
                    profiles.append(local[b])
                    times.append((start - first + b * 1983 + 991) / 2400000)
                    gauges.append(gauge)
                parents.append(record['snapshot_sha256'])
                cpi_count += 1
        assert cpi_count == depth and complete is not None
        assert complete['offline_only'] and not complete['null_calibrated'] and not complete['metric_image']
        assert int(complete['unique_blocks']) == depth * 121
        assert int(complete['missing_support_samples']) == (depth - 1) * 57
        assert len(complete['spectra']) == 1
        spectrum = complete['spectra'][0]
        assert spectrum['translation_doppler_hz'] == 0 and spectrum['delay_at_first_midpoint'] == 32
        assert spectrum['residual_doppler_hz'] == list(range(-60, 61))
        assert len(spectrum['amplitude_rectangular_sum_over_sqrt_unique_blocks']) == 121
        assert int(spectrum['complex_mac_bound']) <= plan['per_aperture_complex_mac_bound']
        oracle_error = 0.
        for frequency, actual in zip(spectrum['residual_doppler_hz'], spectrum['amplitude_rectangular_sum_over_sqrt_unique_blocks']):
            expected = sum(z * gauge * cmath.exp(-2j * math.pi * frequency * time)
                           for z, gauge, time in zip(profiles, gauges, times)) / math.sqrt(len(profiles))
            oracle_error = max(oracle_error, abs(complex(*actual) - expected) / max(1, abs(expected)))
        assert max_inverse_error <= plan['maximum_complex_relative_oracle_error']
        assert oracle_error <= plan['maximum_complex_relative_oracle_error']
        results.append(dict(frames=depth, unique_blocks=len(profiles), raw_samples=depth * 240000,
                            processed_samples=depth * 239943, unprocessed_tail_samples=depth * 57,
                            missing_internal_support_samples=(depth - 1) * 57,
                            pilot_coherence_minimum=pilot_minimum, inverse_relative_error=max_inverse_error,
                            spectrum_relative_error=oracle_error, export_sha256=digest.hexdigest(),
                            parent_snapshot_sha256=list(dict.fromkeys(parents)),
                            frequencies=spectrum['residual_doppler_hz'],
                            amplitude=spectrum['amplitude_rectangular_sum_over_sqrt_unique_blocks']))
    result = dict(schema='vectorwarp-recorded-aperture-qualification-v1', contract='PASS',
                  qualification_plan_sha256=hashlib.sha256(plan_bytes).hexdigest(),
                  calibration_only=True, aircraft_identified=False, metric_image=False,
                  target_snr_or_false_alarm_claim=False, promotion_eligible=False, results=results)
    with (directory / 'qualification.json').open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'results'}))
    for row in results:
        print(json.dumps({key: value for key, value in row.items() if key not in ('frequencies', 'amplitude')}))


if __name__ == '__main__':
    qualify(Path(sys.argv[1]), Path(sys.argv[2]))
