"""Render only oracle-verified synthetic evidence; no inferred aircraft image."""
import hashlib
import argparse
import html
import json
import math
from pathlib import Path
import re
import sys


def render(source, destination):
    data = source.read_bytes()
    assert len(data) < 1024 * 1024
    records = [json.loads(line) for line in data.splitlines()]
    assert {(r['scatterers'], r['frames']) for r in records} == {(s, f) for s in (1, 2) for f in (2, 4, 8)}
    assert len(records) == 6
    error = 0.
    for record in records:
        assert record['schema'] == 'vectorwarp-synthetic-aperture-psf-v1'
        assert record['synthetic_only'] and not record['metric_image']
        assert record['unique_blocks'] == 17 + (record['frames'] - 1) * 8
        assert len(record['bins']) == 33
        for frequency, re, im, oracle_re, oracle_im in record['bins']:
            assert all(math.isfinite(value) for value in (frequency, re, im, oracle_re, oracle_im))
            relative = abs(complex(re, im) - complex(oracle_re, oracle_im)) / max(1, abs(complex(oracle_re, oracle_im)))
            error = max(error, relative)
    assert error <= 3e-6, 'Cannot illustrate failed complex oracle evidence'
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="620" viewBox="0 0 1100 620">',
           '<rect width="1100" height="620" fill="#111827"/>',
           '<g font-family="sans-serif" fill="#e5e7eb">',
           '<text x="45" y="42" font-size="25">Verified synthetic aperture signatures</text>',
           '<text x="45" y="69" font-size="15">Not aircraft imagery · residual Doppler, not metres · rectangular unique-block aperture</text>']
    colors = {2: '#fbbf24', 4: '#38bdf8', 8: '#a78bfa'}
    for case in (1, 2):
        left, top, width, height = 75 + (case - 1) * 525, 135, 425, 330
        svg.append(f'<text x="{left}" y="110" font-size="19">{case} prescribed point scatterer{"s" if case == 2 else ""}</text>')
        for tick in (0, -10, -20, -30, -40):
            y = top - tick * height / 45
            svg.extend([f'<path d="M{left},{y}h{width}" stroke="#374151"/>',
                        f'<text x="{left - 35}" y="{y + 5}" font-size="12">{tick}</text>'])
        for tick in (-4, -2, 0, 2, 4):
            x = left + (tick + 4) * width / 8
            svg.append(f'<text x="{x - 6}" y="490" font-size="13">{tick}</text>')
        for record in sorted((r for r in records if r['scatterers'] == case), key=lambda r: r['frames']):
            peak = max(re * re + im * im for _, re, im, _, _ in record['bins'])
            points = []
            for frequency, re, im, _, _ in record['bins']:
                db = max(-45, 10 * math.log10(max(1e-30, (re * re + im * im) / peak)))
                points.append(f'{left + (frequency + 4) * width / 8:.2f},{top - db * height / 45:.2f}')
            svg.append(f'<polyline points="{" ".join(points)}" fill="none" stroke="{colors[record["frames"]]}" stroke-width="2.2"/>')
        svg.append(f'<text x="{left + 95}" y="518" font-size="14">Residual Doppler frequency (Hz)</text>')
    for index, frames in enumerate((2, 4, 8)):
        svg.append(f'<text x="{75 + index * 315}" y="552" fill="{colors[frames]}" font-size="15">{frames} frames / {17 + (frames - 1) * 8} unique blocks</text>')
    svg.extend([f'<text x="45" y="581" font-size="12">Each curve normalized to its own peak (dB); not an SNR-gain comparison. Maximum complex-oracle error: {error:.2g}</text>',
                f'<text x="45" y="603" font-size="11">Evidence SHA-256: {hashlib.sha256(data).hexdigest()}</text>', '</g></svg>'])
    with destination.open('x') as output:
        output.write('\n'.join(svg) + '\n')
    print(f'{destination}: synthetic only, independent complex oracle max relative error {error:.3g}')


GRID_SCHEMA = 'vectorwarp-coherent-passive-isar-grid-v1'
DELAYS = [18, 26, 28, 30, 32, 34, 36, 44, 50]
FREQUENCIES = [-3, -2, -1, 0, 1, 2, 3]
RATES = [0, -.5, .5]
DOMAIN = {'enabled': True, 'minimum_delay': 18, 'minimum_abs_doppler_hz': 2,
          'stage': 'rate-corrected-profile taps'}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def unique_object(pairs):
    value = dict(pairs)
    require(len(value) == len(pairs), 'Duplicate receipt JSON key')
    return value


def reject_constant(value):
    raise ValueError('Nonfinite JSON constant: ' + value)


def finite(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def counter(value):
    require(isinstance(value, str) and re.fullmatch(r'0|[1-9][0-9]{0,19}', value)
            and int(value) < 2**64, 'Exact unsigned decimal counter required')
    return int(value)


def digest_pin(value):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value), 'SHA-256 pin required')
    return value


def read_receipt(source, expected_hash, allow_fixture=False):
    """Validate transport, completion and cross-grid provenance before rendering."""
    digest_pin(expected_hash)
    with source.open('rb') as stream:
        data = stream.read(16 * 1024 * 1024 + 1)
    require(len(data) <= 16 * 1024 * 1024, 'Receipt exceeds 16 MiB bound')
    require(hashlib.sha256(data).hexdigest() == expected_hash, 'Receipt SHA-256 differs')
    records = [json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant) for line in data.splitlines()]
    require(records and all(isinstance(row, dict) for row in records), 'Receipt records required')
    def one(kind):
        found = [row for row in records if row.get('kind') == kind]
        require(len(found) == 1, 'Exactly one ' + kind + ' record required')
        return found[0]
    contract = one('contract')
    require(records[0] is contract and contract['scope'] == 'synthetic-development-only', 'Synthetic contract must lead receipt')
    require(type(contract['fixture_only']) is bool, 'Fixture status must be explicit')
    fixture = contract['fixture_only']
    require(not fixture or allow_fixture, 'Uncalibrated fixture requires --allow-uncalibrated-fixture')
    for key in ('binary_sha256', 'plan_sha256', 'selector_contract_sha256', 'generator_sha256'):
        digest_pin(contract[key])
    require(hashlib.sha256(contract['generator'].encode()).hexdigest() == contract['generator_sha256'], 'Generator identity differs')
    require(not any(row.get('kind') == 'acceptance_refusal' for row in records), 'Refused receipt cannot be rendered')
    finish = one('fixture_pass' if fixture else 'acceptance_pass')
    require(records[-1] is finish and finish['unexpected_refusals'] == 0, 'Successful final completion required')
    if fixture:
        require(not any(row.get('kind') in ('joint', 'positive_oracle', 'acceptance_pass') for row in records), 'Fixture cannot claim calibrated acceptance')
        calibration = None
        iq_hash = digest_pin(finish['positive_oracle_iq_sha256'])
    else:
        require(not any(row.get('kind') == 'fixture_pass' for row in records), 'Mixed fixture/acceptance receipt')
        joint, oracle = one('joint'), one('positive_oracle')
        calibration = digest_pin(joint['calibration_sha256'])
        require(oracle['calibration_sha256'] == calibration and oracle['selector18_detected'] is True,
                'Frozen positive oracle acceptance required')
        iq_hash = digest_pin(oracle['iq_sha256'])
    grids = [row for row in records if row.get('kind') == 'aperture_grid']
    require(len(grids) == 9, 'Exactly nine depth/rate grids required')
    indexed = {}
    for grid in grids:
        depth, rate_index = grid['depth'], grid['rate_index']
        require(type(depth) is int and depth in (2, 4, 8) and type(rate_index) is int and rate_index in (0, 1, 2), 'Invalid depth/rate identity')
        require((depth, rate_index) not in indexed, 'Duplicate depth/rate grid')
        indexed[depth, rate_index] = grid
        require(grid['schema'] == GRID_SCHEMA and grid['scope'] == contract['scope'] and grid['metric_image'] is False,
                'Grid schema/scope differs')
        require(grid['source_domain'] == DOMAIN and grid['source_domain']['enabled'] is True
                and grid['normalization'] == 'rectangular_sum_over_sqrt_unique_blocks',
                'Source mask or aperture normalization differs')
        require(all(finite(grid[key]) for key in ('render_power_reference', 'render_db_min', 'render_db_max'))
                and grid['render_power_reference'] == 1.0 and grid['render_db_min'] == -40 and grid['render_db_max'] == 80,
                'Fixed render scale differs')
        require(grid['delays_at_terminal_midpoint_samples'] == DELAYS and grid['residual_doppler_hz'] == FREQUENCIES
                and all(finite(value) for key in ('delays_at_terminal_midpoint_samples', 'residual_doppler_hz') for value in grid[key]),
                'Prescribed grid coordinates differ')
        require(finite(grid['rate']) and grid['rate'] == RATES[rate_index] and finite(grid['translation_doppler_hz'])
                and finite(grid['delay_drift_per_hz']), 'Invalid physical phase coordinates')
        require(grid['selector_contract_sha256'] == contract['selector_contract_sha256']
                and grid['calibration_sha256'] == calibration and grid['iq_sha256'] == iq_hash, 'Grid provenance differs')
        require(counter(grid['unique_blocks']) == depth * 33 and 0 < counter(grid['complex_mac_bound']) <= 2**28,
                'Aperture support/work bound differs')
        hop = counter(grid['hop_samples'])
        require(hop > 0 and counter(grid['bootstrap_end_sample']) - counter(grid['bootstrap_first_sample']) == hop,
                'Bootstrap interval differs')
        for key in ('source_intervals', 'producer_seals', 'producer_configuration_sha256', 'pilot_coherence', 'gauge_correction'):
            require(len(grid[key]) == depth, 'Incomplete ' + key)
        previous = None
        for interval, seal, configs, coherence, gauge in zip(*(grid[key] for key in
                ('source_intervals', 'producer_seals', 'producer_configuration_sha256', 'pilot_coherence', 'gauge_correction'))):
            first, end, sequence = (counter(interval[key]) for key in ('first', 'end', 'sequence'))
            require(end - first == hop and (previous is None or (first == previous[0] and sequence == previous[1] + 1)),
                    'Noncontiguous source intervals')
            previous = end, sequence
            digest_pin(seal)
            require(len(configs) == 3, 'Incomplete producer configuration bank')
            for pin in configs:
                digest_pin(pin)
            require(finite(coherence) and 0 < coherence <= 1, 'Invalid pilot coherence')
            require(isinstance(gauge, list) and len(gauge) == 2 and all(finite(value) for value in gauge)
                    and abs(math.hypot(*gauge) - 1) <= 1e-9, 'Invalid phase gauge')
        require(grid['first_sample'] == grid['source_intervals'][0]['first'] and grid['end_sample'] == grid['source_intervals'][-1]['end'],
                'Grid source interval differs')
        mask, pixels = grid['valid_mask'], grid['pixels_delay_major']
        require(len(mask) == len(pixels) == len(DELAYS) * len(FREQUENCIES), 'Incomplete grid/mask')
        for i, (valid, pixel) in enumerate(zip(mask, pixels)):
            require(type(valid) is bool, 'Mask must contain booleans')
            require((not valid and pixel is None) or (valid and isinstance(pixel, list) and len(pixel) == 2
                    and all(finite(value) for value in pixel)), 'Masked cells must be null; valid cells must be finite complex pairs')
            require(not valid or abs(grid['translation_doppler_hz'] + FREQUENCIES[i % len(FREQUENCIES)]) >= 2,
                    'Valid cell violates source Doppler domain')
    provenance = ('source_intervals', 'producer_seals', 'producer_configuration_sha256', 'pilot_coherence', 'gauge_correction')
    for depth in (2, 4, 8):
        reference = indexed[depth, 0]
        for rate_index in (1, 2):
            require(all(indexed[depth, rate_index][key] == reference[key] for key in
                        (*provenance, 'first_sample', 'end_sample', 'bootstrap_first_sample', 'bootstrap_end_sample',
                         'hop_samples', 'unique_blocks', 'delay_drift_per_hz')), 'Rate grids use different source provenance')
        require(all(reference[key] == indexed[8, 0][key][-depth:] for key in provenance), 'Depth grids are not suffixes of one aperture')
        require(reference['end_sample'] == indexed[8, 0]['end_sample'] and reference['bootstrap_end_sample'] == indexed[8, 0]['bootstrap_end_sample'],
                'Depth endpoint/bootstrap differs')
    require(indexed[8, 0]['first_sample'] == indexed[8, 0]['bootstrap_end_sample'], 'Bootstrap leaked into aperture support')
    return contract, indexed, fixture


def power_color(pixel):
    """One absolute scale for every depth/rate; never use a grid's maximum."""
    magnitude = math.hypot(*pixel)
    db = -40 if magnitude == 0 else max(-40, min(80, 20 * math.log10(magnitude)))
    fraction = (db + 40) / 120
    low, high = (15, 23, 42), (250, 204, 21)
    return '#' + ''.join(f'{round(a + (b - a) * fraction):02x}' for a, b in zip(low, high))


def render_receipt(source, expected_hash, destination, allow_fixture=False):
    contract, grids, fixture = read_receipt(source, expected_hash, allow_fixture)
    status = 'UNCALIBRATED FIXTURE ONLY' if fixture else 'Synthetic development acceptance only'
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1320" height="1130" viewBox="0 0 1320 1130">',
           '<title>Coherent 2/4/8 CPI passive aperture: delay samples by residual Doppler Hz</title>',
           '<metadata>' + html.escape(json.dumps({'receipt_sha256': expected_hash, 'uncalibrated_fixture': fixture,
               'contract': contract, 'grids': list(grids.values())}, allow_nan=False, sort_keys=True)) + '</metadata>',
           '<defs><pattern id="invalid" width="8" height="8" patternUnits="userSpaceOnUse"><rect width="8" height="8" fill="#334155"/><path d="M0,0L8,8M8,0L0,8" stroke="#94a3b8" stroke-width="1"/></pattern></defs>',
           '<rect width="1320" height="1130" fill="#111827"/><g font-family="sans-serif" fill="#e5e7eb">',
           '<text x="35" y="35" font-size="24">Coherent 2 / 4 / 8 CPI → passive aperture</text>',
           f'<text x="35" y="61" font-size="16" fill="#fbbf24">{status}</text>',
           '<text x="35" y="85" font-size="14">Synthetic prescribed signal; no aircraft identity, metric cross-range or ADS-B inference.</text>',
           '<text x="35" y="108" font-size="14">Fixed power scale: −40 to +80 dB relative to 1 amplitude²; rectangular sum / √unique blocks.</text>']
    for row, depth in enumerate((2, 4, 8)):
        for column, rate_index in enumerate((0, 1, 2)):
            grid = grids[depth, rate_index]
            left, top, width, height = 75 + column * 430, 165 + row * 275, 345, 175
            svg.append(f'<g data-depth="{depth}" data-rate-index="{rate_index}">')
            svg.append(f'<text x="{left}" y="{top - 18}" font-size="15">{depth} CPI · rate {grid["rate"]:+g} bins/s · {grid["unique_blocks"]} blocks</text>')
            for i, (valid, pixel) in enumerate(zip(grid['valid_mask'], grid['pixels_delay_major'])):
                delay_index, frequency_index = divmod(i, len(FREQUENCIES))
                # Uneven prescribed delay coordinates retain their true spacing.
                delay = DELAYS[delay_index]
                lower = (DELAYS[delay_index - 1] + delay) / 2 if delay_index else 14
                upper = (delay + DELAYS[delay_index + 1]) / 2 if delay_index + 1 < len(DELAYS) else 53
                x = left + (lower - 14) * width / 39
                y = top + (6 - frequency_index) * height / 7
                fill = power_color(pixel) if valid else 'url(#invalid)'
                svg.append(f'<rect data-cell="{i}" data-valid="{str(valid).lower()}" x="{x:.3f}" y="{y:.3f}" width="{(upper-lower)*width/39:.3f}" height="{height/7:.3f}" fill="{fill}"><title>delay {delay} samples; residual Doppler {FREQUENCIES[frequency_index]} Hz; {"valid" if valid else "masked / unavailable"}</title></rect>')
            for frequency in (-3, 0, 3):
                y = top + (3 - frequency + .5) * height / 7
                svg.append(f'<text x="{left-25}" y="{y+4:.3f}" font-size="12">{frequency}</text>')
            for delay in (18, 26, 32, 36, 44, 50):
                x = left + (delay - 14) * width / 39
                svg.append(f'<text x="{x-7:.3f}" y="{top+height+18}" font-size="12">{delay}</text>')
            svg.append(f'<text x="{left+25}" y="{top+height+39}" font-size="13">Delay at terminal midpoint (samples)</text>')
            svg.append(f'<text transform="translate({left-43},{top+height-4}) rotate(-90)" font-size="12">Residual Doppler (Hz)</text>')
            svg.append(f'<text x="{left}" y="{top+height+59}" font-size="12">Bulk Doppler removed: {grid["translation_doppler_hz"]:.6g} Hz</text></g>')
    for i in range(120):
        power_db = -40 + i
        svg.append(f'<rect x="{35+i*4}" y="983" width="4" height="15" fill="{power_color([10**(power_db/20), 0])}"/>')
    svg.extend(['<text x="35" y="1015" font-size="12">−40</text><text x="475" y="1015" font-size="12">+80 dB</text>',
                '<rect x="550" y="983" width="20" height="18" fill="url(#invalid)"/><text x="580" y="998" font-size="13">Masked: source-domain taps or Doppler excluded; no padding</text>'])
    for offset, (label, value) in enumerate((('Receipt SHA-256', expected_hash), ('Executable SHA-256', contract['binary_sha256']),
                                          ('Selector contract SHA-256', contract['selector_contract_sha256']))):
        svg.append(f'<text x="35" y="{1042+offset*21}" font-size="11">{label}: {html.escape(value)}</text>')
    svg.append('</g></svg>')
    with destination.open('x') as output:
        output.write('\n'.join(svg) + '\n')
    print(f'{destination}: {status}; nine grids; fixed power scale; receipt {expected_hash}')


if __name__ == '__main__':
    if len(sys.argv) == 3 and not sys.argv[1].startswith('--'):
        render(Path(sys.argv[1]), Path(sys.argv[2]))
    else:
        parser = argparse.ArgumentParser(description='Render authenticated synthetic 2/4/8 aperture grids with a fixed power scale.')
        parser.add_argument('--receipt', required=True, type=Path)
        parser.add_argument('--sha256', required=True)
        parser.add_argument('--output', required=True, type=Path)
        parser.add_argument('--allow-uncalibrated-fixture', action='store_true')
        args = parser.parse_args()
        try:
            render_receipt(args.receipt, args.sha256, args.output, args.allow_uncalibrated_fixture)
        except (ValueError, KeyError, TypeError, OSError) as error:
            parser.exit(1, f'Receipt refused: {error}\n')
