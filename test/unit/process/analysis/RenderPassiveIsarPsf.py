"""Render only oracle-verified synthetic evidence; no inferred aircraft image."""
import hashlib
import json
import math
from pathlib import Path
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


if __name__ == '__main__':
    render(Path(sys.argv[1]), Path(sys.argv[2]))
