#!/usr/bin/env python3
"""Quantify the ideal-manifold error caused by the radius unit mistake.

This is an offline analytic comparison.  It models only plane-wave phase at the
five phase centers and deliberately does not claim a detection probability.
"""
import cmath
import json
import math
from pathlib import Path

C = 299_792_458.0
FREQUENCY_HZ = 527_000_000.0
TRUE_RADIUS_M = 0.0762
OLD_RADIUS_M = 0.193548
CH0_BEARING_DEG_TRUE = 80.0
DIRECT_BEARING_DEG_TRUE = 62.08284655418339
DIRECT_ELEVATION_DEG = 3.3


def positions(radius):
    return [
        (radius * math.sin(math.radians(CH0_BEARING_DEG_TRUE + 72 * channel)),
         radius * math.cos(math.radians(CH0_BEARING_DEG_TRUE + 72 * channel)))
        for channel in range(5)
    ]


def horizontal_direction(bearing, elevation):
    scale = math.cos(math.radians(elevation))
    return (scale * math.sin(math.radians(bearing)),
            scale * math.cos(math.radians(bearing)))


def response(radius, target_bearing, target_elevation, model_2d=False):
    target = horizontal_direction(target_bearing, 0 if model_2d else target_elevation)
    direct = horizontal_direction(DIRECT_BEARING_DEG_TRUE,
                                  0 if model_2d else DIRECT_ELEVATION_DEG)
    wave = 2 * math.pi * FREQUENCY_HZ / C
    return [cmath.exp(1j * wave * (x * (target[0] - direct[0]) +
                                    y * (target[1] - direct[1])))
            for x, y in positions(radius)]


def coherence(expected, actual):
    return abs(sum(a.conjugate() * b for a, b in zip(expected, actual))) / 5


def loss_db(value):
    return 20 * math.log10(max(value, 1e-12))


def angular_error_deg(a, b):
    return abs((a - b + 180) % 360 - 180)


def best_bank(radius, actual, target_bearing):
    # Production constructs local beams 0,30,... and reports true bearing as
    # x_axis - local. Preserve production's first-winner tie behavior.
    best_value, best_true = -1.0, None
    for local in range(0, 360, 30):
        beam_true = (CH0_BEARING_DEG_TRUE - local) % 360
        value = coherence(response(radius, beam_true, 0, model_2d=True), actual)
        if value > best_value:
            best_value, best_true = value, beam_true
    return best_value, best_true, angular_error_deg(best_true, target_bearing)


def loss_color(value):
    # 0 dB white; -12 dB or worse deep red.
    t = min(1.0, max(0.0, -value / 12.0))
    return f"rgb(255,{round(255 * (1-t))},{round(255 * (1-t))})"


def error_color(value):
    # 0 degrees white; 180 degrees deep blue.
    t = min(1.0, max(0.0, value / 180.0))
    return f"rgb({round(255 * (1-t))},{round(255 * (1-t))},255)"


def svg_panel(rows, key, x0, y0, title, subtitle, color_fn):
    width, height = 504, 220
    bearing_step, elevation_step = 5, 5
    chunks = [f'<text x="{x0}" y="{y0-37}" font-size="17" font-weight="bold">{title}</text>']
    chunks.append(f'<text x="{x0}" y="{y0-17}" font-size="12">{subtitle}</text>')
    for row in rows:
        x = x0 + row["bearing_deg"] / 360 * width
        y = y0 + (85 - row["elevation_deg"]) / 85 * height
        chunks.append(
            f'<rect x="{x:.2f}" y="{y:.2f}" width="{width*bearing_step/360+0.5:.2f}" '
            f'height="{height*elevation_step/85+0.5:.2f}" fill="{color_fn(row[key])}"/>')
    chunks += [
        f'<rect x="{x0}" y="{y0}" width="{width}" height="{height}" fill="none" stroke="#222"/>',
        f'<text x="{x0+width/2}" y="{y0+height+25}" text-anchor="middle" font-size="13">Aircraft bearing (0–360° true)</text>',
        f'<text x="{x0-38}" y="{y0+height/2}" transform="rotate(-90 {x0-38} {y0+height/2})" text-anchor="middle" font-size="13">Elevation (0–85°)</text>',
    ]
    return "\n".join(chunks)


def main():
    root = Path(__file__).resolve().parent
    rows = []
    for elevation in range(0, 86, 5):
        for bearing in range(0, 360, 5):
            actual = response(TRUE_RADIUS_M, bearing, elevation)
            corrected = response(TRUE_RADIUS_M, bearing, elevation, model_2d=True)
            old = response(OLD_RADIUS_M, bearing, elevation, model_2d=True)
            corrected_best, corrected_beam, corrected_error = best_bank(
                TRUE_RADIUS_M, actual, bearing)
            old_best, old_beam, old_error = best_bank(OLD_RADIUS_M, actual, bearing)
            rows.append({
                "bearing_deg": bearing,
                "elevation_deg": elevation,
                "corrected_radius_2d_coherence": coherence(corrected, actual),
                "corrected_radius_2d_loss_db": loss_db(coherence(corrected, actual)),
                "old_radius_2d_coherence": coherence(old, actual),
                "old_radius_2d_loss_db": loss_db(coherence(old, actual)),
                "corrected_radius_best_of_12_coherence": corrected_best,
                "corrected_radius_best_of_12_loss_db": loss_db(corrected_best),
                "corrected_radius_best_beam_deg": corrected_beam,
                "corrected_radius_best_beam_error_deg": corrected_error,
                "old_radius_best_of_12_coherence": old_best,
                "old_radius_best_of_12_loss_db": loss_db(old_best),
                "old_radius_best_beam_deg": old_beam,
                "old_radius_best_beam_error_deg": old_error,
            })
    def summarize(key):
        values = sorted(row[key] for row in rows)
        return {
            "median_db": values[len(values)//2],
            "p10_db": values[len(values)//10],
            "worst_db": values[0],
            "cells_worse_than_minus_3db": sum(v < -3 for v in values),
            "cells_worse_than_minus_6db": sum(v < -6 for v in values),
            "cell_count": len(values),
        }
    def summarize_bank(prefix):
        result = summarize(f"{prefix}_best_of_12_loss_db")
        errors = sorted(row[f"{prefix}_best_beam_error_deg"] for row in rows)
        result.update({
            "median_bearing_error_deg": errors[len(errors)//2],
            "p90_bearing_error_deg": errors[9*len(errors)//10],
            "cells_bearing_error_over_30deg": sum(value > 30 for value in errors),
        })
        return result
    output = {
        "schema": "vectorwarp.array-radius-phase-error.v1",
        "simulation_only": True,
        "live_radar_modified": False,
        "frequency_hz": FREQUENCY_HZ,
        "manufacturer_radius_m": TRUE_RADIUS_M,
        "invalidated_radius_m": OLD_RADIUS_M,
        "ch0_bearing_deg_true": CH0_BEARING_DEG_TRUE,
        "direct_path": {"bearing_deg_true": DIRECT_BEARING_DEG_TRUE,
                        "elevation_deg": DIRECT_ELEVATION_DEG},
        "model_note": "Actual response includes target elevation; both compared VectorWarp ideal manifolds are two-dimensional horizon models. The 12-beam bank uses the exact production local grid converted by true_bearing=x_axis-local and first-winner ties.",
        "summary": {
            "same_labeled_direction": {
                "corrected_radius_2d": summarize("corrected_radius_2d_loss_db"),
                "old_radius_2d": summarize("old_radius_2d_loss_db"),
            },
            "production_relevant_best_of_12_bank": {
                "corrected_radius_2d": summarize_bank("corrected_radius"),
                "old_radius_2d": summarize_bank("old_radius"),
            },
        },
        "rows": rows,
    }
    (root / "array-radius-phase-error.json").write_text(json.dumps(output, indent=2) + "\n")
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="700" viewBox="0 0 1120 700">
<rect x="0" y="0" width="1120" height="700" fill="white"/>
{svg_panel(rows, "old_radius_best_of_12_loss_db", 60, 72, "Locked radius: strongest of 12 beams", "Amplitude loss: white 0 dB, deep red ≤ -12 dB", loss_color)}
{svg_panel(rows, "corrected_radius_best_of_12_loss_db", 600, 72, "Corrected radius: strongest of 12 beams", "Amplitude loss: white 0 dB, deep red ≤ -12 dB", loss_color)}
{svg_panel(rows, "old_radius_best_beam_error_deg", 60, 412, "Locked radius: bearing assignment error", "White 0°, deep blue 180°", error_color)}
{svg_panel(rows, "corrected_radius_best_beam_error_deg", 600, 412, "Corrected radius: bearing assignment error", "White 0°, deep blue 180°", error_color)}
</svg>'''
    (root / "array-radius-phase-error.svg").write_text(svg)
    print(json.dumps(output["summary"], indent=2))


if __name__ == "__main__":
    main()
