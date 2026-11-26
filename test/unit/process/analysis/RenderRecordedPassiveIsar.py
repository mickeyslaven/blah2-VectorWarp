"""Illustrate qualified fixed-coordinate replay evidence, never aircraft imagery."""
import hashlib
import json
import math
from pathlib import Path
import sys


def render(source, destination):
    data = source.read_bytes()
    assert len(data) < 1024 * 1024
    result = json.loads(data)
    assert result['schema'] == 'vectorwarp-recorded-aperture-qualification-v1'
    assert result['contract'] == 'PASS' and result['calibration_only']
    assert not any(result[key] for key in ('aircraft_identified', 'metric_image',
                                          'target_snr_or_false_alarm_claim', 'promotion_eligible'))
    rows = result['results']
    assert [row['frames'] for row in rows] == [2, 4, 8]
    colors = ('#fbbf24', '#38bdf8', '#c4b5fd')
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="690" viewBox="0 0 1100 690">',
           '<rect width="1100" height="690" fill="#111827"/>',
           '<g font-family="sans-serif" fill="#e5e7eb">',
           '<text x="44" y="42" font-size="25">Recorded-IQ aperture contract: 2 / 4 / 8 CPI</text>',
           '<text x="44" y="70" font-size="15">Calibration only · extracted channel 0 · fixed delay 32 samples · prescribed zero translation</text>',
           '<text x="44" y="96" font-size="15" fill="#fbbf24">Not aircraft imagery. No target identification, metric cross-range, SNR gain or false-alarm claim.</text>']
    left, top, width, height = 85, 140, 910, 285
    for tick in (0, -10, -20, -30, -40):
        y = top - tick * height / 40
        svg.extend([f'<path d="M{left},{y}h{width}" stroke="#374151"/>',
                    f'<text x="{left - 40}" y="{y + 5}" font-size="13">{tick}</text>'])
    for tick in range(-60, 61, 20):
        x = left + (tick + 60) * width / 120
        svg.append(f'<text x="{x - 12}" y="450" font-size="13">{tick}</text>')
    maximum_error = 0.
    for row, color in zip(rows, colors):
        depth = row['frames']
        assert row['unique_blocks'] == depth * 121
        assert row['unprocessed_tail_samples'] == depth * 57
        assert row['missing_internal_support_samples'] == (depth - 1) * 57
        assert row['frequencies'] == list(range(-60, 61)) and len(row['amplitude']) == 121
        assert 0 <= row['pilot_coherence_minimum'] <= 1
        errors = [row['inverse_relative_error'], row['spectrum_relative_error']]
        assert all(math.isfinite(error) and 0 <= error <= 1e-9 for error in errors)
        maximum_error = max(maximum_error, *errors)
        power = []
        for re, im in row['amplitude']:
            assert math.isfinite(re) and math.isfinite(im)
            power.append(re * re + im * im)
        peak = max(power)
        assert peak > 0 and math.isfinite(peak)
        points = []
        for frequency, value in zip(row['frequencies'], power):
            db = max(-40, 10 * math.log10(max(1e-30, value / peak)))
            points.append(f'{left + (frequency + 60) * width / 120:.2f},{top - db * height / 40:.2f}')
        svg.append(f'<polyline points="{" ".join(points)}" fill="none" stroke="{color}" stroke-width="2"/>')
    svg.extend(['<text x="340" y="480" font-size="15">Residual Doppler (Hz); each trace normalized to its own peak (dB)</text>',
                '<text x="44" y="510" font-size="14">Exact raw-contiguous support; no invented samples, padding or extra exposure for discarded tails.</text>'])
    for index, (row, color) in enumerate(zip(rows, colors)):
        svg.append(f'<text x="{44 + index * 350}" y="542" fill="{color}" font-size="16">{row["frames"]} CPI / {row["unique_blocks"]} unique blocks</text>')
        svg.append(f'<text x="{44 + index * 350}" y="568" font-size="14">{row["unprocessed_tail_samples"]} tail samples; {row["missing_internal_support_samples"]} internal missing</text>')
    svg.extend([f'<text x="44" y="605" font-size="13">Independent retained-CAF inverse / nonuniform Fourier algebra error ≤ {maximum_error:.3g}. Not a physical-target oracle.</text>',
                '<text x="44" y="631" font-size="13">Physical carrier and array-manifold qualification unknown; nonzero translation is refused.</text>',
                f'<text x="44" y="662" font-size="11">Qualification SHA-256: {hashlib.sha256(data).hexdigest()}</text>', '</g></svg>'])
    with destination.open('x') as output:
        output.write('\n'.join(svg) + '\n')
    print(f'{destination}: recorded contract evidence only; maximum complex algebra error {maximum_error:.3g}')


if __name__ == '__main__':
    render(Path(sys.argv[1]), Path(sys.argv[2]))
