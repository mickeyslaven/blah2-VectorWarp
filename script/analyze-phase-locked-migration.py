#!/usr/bin/env python3
"""Replay-only phase-locked, trajectory-migrated complex CAF integration.

This consumes the complex cutouts emitted by the coherent-beam replay.  A
direct-path/pilot phasor is used only to remove a common per-CPI phase; target
coordinates come from the supplied trajectory.  Every coherent beam, phase
orientation, residual delay/Doppler grid point and requested quality gate is
maximised identically at the target and every translated-path control.  Thus
the empirical controls, rather than an uncharged best setting, absorb those
trials.

Current v1 cutouts do not record a direct-path phasor.  The explicit
``reference_weight_proxy`` fallback is consequently labelled as a proxy, not
as a direct-path measurement.  It is suitable for a bounded diagnostic only;
an actual ``direct_path_reference`` or ``pilot_reference`` field takes
precedence when a replay provides one.
"""

import argparse
import cmath
import importlib.util
import json
import math
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "calibrated", Path(__file__).with_name("analyze-calibrated-complex-replay.py"))
CAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAL)


def parse_numbers(value):
    result = [float(item) for item in value.split(",") if item.strip()]
    if not result:
        raise ValueError("expected at least one comma-separated number")
    return result


def as_complex(value):
    if isinstance(value, (int, float)):
        return complex(value, 0)
    if isinstance(value, list) and len(value) == 2:
        return complex(value[0], value[1])
    raise ValueError("phase reference must be [real, imaginary]")


def unit(value):
    magnitude = abs(value)
    if not math.isfinite(magnitude) or magnitude <= 1e-30:
        raise ValueError("phase reference contains a zero or non-finite phasor")
    return value / magnitude


def pilot_series(cutouts, source="auto"):
    """Return normalized per-frame pilot phasors and an auditable source name."""
    keys = ("direct_path_reference", "pilot_reference", "reference_component")
    if source in ("auto", "direct"):
        for key in keys:
            if all(key in frame for frame in cutouts):
                return [unit(as_complex(frame[key])) for frame in cutouts], key
        if source == "direct":
            raise ValueError("cutouts have no direct-path/pilot reference")
    if source not in ("auto", "reference-weight-proxy"):
        raise ValueError("unknown phase source")
    # The first non-zero reference weight has a well-defined common phase for
    # a frame.  It is deliberately a fallback: the output names it as a proxy.
    values = []
    for frame in cutouts:
        weights = [as_complex(item) for item in frame.get("reference_weights", [])]
        candidate = next((item for item in weights if abs(item) > 1e-30), None)
        if candidate is None:
            raise ValueError("cutout has no usable reference-weight phase proxy")
        values.append(unit(candidate))
    return values, "reference_weight_proxy"


def phase_increment_coherence(pilots):
    if len(pilots) < 2:
        raise ValueError("at least two cutout frames are required")
    return abs(sum(current * previous.conjugate()
                   for previous, current in zip(pilots, pilots[1:])) / (len(pilots) - 1))


def nearest(axis, value):
    return min(range(len(axis)), key=lambda index: abs(axis[index] - value))


def beam_values(frame, config, row, column):
    observation = [complex(*channel[row][column]) for channel in frame["channels"]]
    reference = [complex(*item) for item in frame["reference_weights"]]
    return [sum(weight * sample for weight, sample in zip(weights, observation))
            for weights in CAL.calibrated_weights(config, reference)]


def translated_cells(cutouts, trajectory, row_offset, column_offset):
    cells = []
    for frame, truth in zip(cutouts, trajectory):
        row = nearest(frame["doppler_hz"], truth["doppler_hz"]) + row_offset
        column = nearest(frame["delay_km"], truth["delay_km"]) + column_offset
        if not (0 <= row < len(frame["doppler_hz"]) and
                0 <= column < len(frame["delay_km"])):
            return None
        cells.append((row, column))
    return cells


def coherent_score(cutouts, config, cells, pilots, phase_sign, delay_residual,
                   doppler_residual, cpi):
    """Best fixed beam score for one phase/grid hypothesis and translated path."""
    beam_count = int(config["process"]["surveillance_fusion"]["beams"])
    # The residual grid is in cutout bins, so it is applied at every CPI after
    # the physical trajectory has already supplied range/Doppler migration.
    shifted = []
    for frame, (row, column) in zip(cutouts, cells):
        row += doppler_residual
        column += delay_residual
        if not (0 <= row < len(frame["doppler_hz"]) and
                0 <= column < len(frame["delay_km"])):
            return None
        shifted.append((row, column))
    sums = [0j] * beam_count
    for frame, (row, column), pilot in zip(cutouts, shifted, pilots):
        correction = pilot if phase_sign > 0 else pilot.conjugate()
        for beam, value in enumerate(beam_values(frame, config, row, column)):
            sums[beam] += value * correction
    scores = [abs(value) ** 2 / len(cutouts) for value in sums]
    best = max(range(beam_count), key=lambda index: scores[index])
    return scores[best], best


def percentile(values, fraction):
    if not values:
        raise ValueError("empty control population")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def calibrated_record(kind, score, controls, row_offset, column_offset, leave_one_out=False):
    exceedances = sum(value >= score for value in controls)
    denominator = len(controls) + 1
    if leave_one_out:
        exceedances -= 1
        denominator = len(controls)
    survival = (1 + exceedances) / denominator
    return {"kind": kind, "doppler_offset_bins": row_offset,
            "delay_offset_bins": column_offset, "raw_score": score,
            "empirical_survival_p": survival,
            "evidence_log10": -math.log10(survival)}


def analyze(cutouts, config, job, phase_source="auto", phase_gates=(.6,),
            delay_grid=(0,), doppler_grid=(0,), phase_signs=(-1, 1), guard=1,
            min_doppler=15):
    trajectory = job["trajectory"]
    if len(cutouts) != len(trajectory):
        raise ValueError("complex cutout and trajectory frame counts differ")
    pilots, source = pilot_series(cutouts, phase_source)
    quality = phase_increment_coherence(pilots)
    gates = sorted(set(float(value) for value in phase_gates))
    if any(not 0 <= gate <= 1 for gate in gates):
        raise ValueError("phase gates must be in [0, 1]")
    active_gates = [gate for gate in gates if quality >= gate]
    if not active_gates:
        raise ValueError("phase-coherence quality gate rejected this recording")
    cpi = config["process"]["data"]["cpi"]
    # All requested variants are charged.  Gates which fail do not produce an
    # observation, but remain visible in the accounting for auditability.
    variants = [(sign, gate, delay, doppler) for sign in phase_signs
                for gate in active_gates for delay in delay_grid for doppler in doppler_grid]
    target_cells = translated_cells(cutouts, trajectory, 0, 0)
    if target_cells is None:
        raise ValueError("truth trajectory is outside the complex cutout")

    def score(cells):
        values = [coherent_score(cutouts, config, cells, pilots, sign, delay, doppler, cpi)
                  for sign, _gate, delay, doppler in variants]
        values = [item for item in values if item is not None]
        if not values:
            return None
        raw, beam = max(values, key=lambda item: item[0])
        return raw, beam

    target = score(target_cells)
    if target is None:
        raise ValueError("target cannot be scored for the requested residual grid")
    rows = range(-len(cutouts[0]["doppler_hz"]) + 1, len(cutouts[0]["doppler_hz"]))
    columns = range(-len(cutouts[0]["delay_km"]) + 1, len(cutouts[0]["delay_km"]))
    control_scores = []
    for row_offset in rows:
        for column_offset in columns:
            if abs(row_offset) <= guard and abs(column_offset) <= guard:
                continue
            cells = translated_cells(cutouts, trajectory, row_offset, column_offset)
            if cells is None or any(abs(frame["doppler_hz"][row]) < min_doppler
                                    for frame, (row, _column) in zip(cutouts, cells)):
                continue
            item = score(cells)
            if item is not None:
                control_scores.append((row_offset, column_offset, item[0], item[1]))
    raw_controls = [item[2] for item in control_scores]
    target_record = calibrated_record("target", target[0], raw_controls, 0, 0)
    controls = [calibrated_record("control", value, raw_controls, row, column, True)
                for row, column, value, _beam in control_scores]
    method = {"evidence_log10": target_record["evidence_log10"],
              "control_evidence_log10": [item["evidence_log10"] for item in controls],
              "parameter_trial_count": len(phase_signs) * len(gates) * len(delay_grid) * len(doppler_grid),
              "matched_control_count": len(controls),
              "raw_target_score": target[0],
              "control_p99": percentile(raw_controls, job["control_quantile"]),
              "phase_coherence": quality,
              "phase_source": source}
    vector = {"schema": "vectorwarp-calibrated-trajectory-score-vector-v1",
              "method_id": "phase_locked_migration", "job_id": job["job_id"],
              "event_id": job["event_id"], "recording": job["recording"],
              "trial_family_count": job["family_trials"],
              "parameter_grid_choices": method["parameter_trial_count"],
              "internal_search_trials_per_path": int(config["process"]["surveillance_fusion"]["beams"]),
              "target": target_record, "controls": controls}
    return {"schema": "vectorwarp-phase-locked-migration-v1", "methods":
            {"phase_locked_migration": method}, "score_vectors":
            {"phase_locked_migration": vector}, "aligned_control_offsets":
            [{"doppler_offset_bins": item["doppler_offset_bins"],
              "delay_offset_bins": item["delay_offset_bins"]} for item in controls],
            "phase_lock": {"source": source, "increment_coherence": quality,
                           "requested_gates": gates, "active_gates": active_gates,
                           "phase_signs": list(phase_signs)},
            "migration": {"trajectory_frames": len(trajectory), "cpi_s": cpi,
                          "delay_residual_bins": list(delay_grid),
                          "doppler_residual_bins": list(doppler_grid), "guard_bins": guard,
                          "min_doppler_hz": min_doppler},
            "target_selected_beam": target[1]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--job", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--phase-source", choices=("auto", "direct", "reference-weight-proxy"),
                        default="auto")
    parser.add_argument("--phase-gates", default="0.6")
    parser.add_argument("--delay-residual-bins", default="0")
    parser.add_argument("--doppler-residual-bins", default="0")
    parser.add_argument("--guard-bins", type=int, default=1)
    parser.add_argument("--min-doppler-hz", type=float, default=15)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    config = json.loads(args.config.read_text())
    job = json.loads(args.job.read_text())
    result = analyze(replay["arms"]["coherent_beam_bank"]["complex_cutouts"], config, job,
                     args.phase_source, parse_numbers(args.phase_gates),
                     tuple(int(value) for value in parse_numbers(args.delay_residual_bins)),
                     tuple(int(value) for value in parse_numbers(args.doppler_residual_bins)),
                     guard=args.guard_bins, min_doppler=args.min_doppler_hz)
    args.output.write_text(json.dumps(result, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
