#!/usr/bin/env python3
"""Bounded replay-only cross-fitted, heteroscedastic array detector.

No channel is ever used to construct the reference against which it is scored.
The reference is a robust rank-one estimate from the remaining coherent
channels, fitted on the opposite slow-time fold.  Fixed subbands and every
leave-one-channel-out view enter a lower-quartile consensus; there is no
best-view or best-subband selection.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path

import numpy as np

BASE = Path(__file__).with_name("analyze-loaded-reciprocal-mchq.py")
SPEC = importlib.util.spec_from_file_location("mchq", BASE)
MCHQ = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MCHQ)


def db_ratio(value, reference):
    return 10.0 * math.log10(max(float(value), 1e-300) /
                             max(float(reference), 1e-300))


def percentile(values, fraction=.99):
    ordered = sorted(float(value) for value in values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def load_trajectory(path, event_id=None, recording=None):
    """Load a trajectory, preferring corrected manifest rows when present."""
    document = json.loads(Path(path).read_text())
    if "rows" not in document:
        return document["trajectory"], "trajectory_file:" + document.get("schema", "unknown")
    rows = document["rows"]
    if event_id:
        rows = [row for row in rows if row.get("event_id") == event_id]
    if recording:
        name = Path(recording).name
        matching = [row for row in rows
                    if Path(row.get("capture", {}).get("file", "")).name == name]
        if matching:
            rows = matching
    if not rows:
        raise ValueError("manifest has no matching corrected trajectory")
    # Interpolated/reconciled paths precede prediction fallbacks.  This is a
    # data-quality preference, never a score-based choice.
    rows.sort(key=lambda row: ("fallback" in row.get("truth_source", ""),
                               -int(row.get("truth_samples_count", 0))))
    row = rows[0]
    return row["replay_trajectory"], "manifest:" + row.get("truth_source", "unknown")


def load_gains(recording, configured=None):
    if configured is not None:
        return configured, "command_line"
    sidecar = Path(str(recording) + ".json")
    if not sidecar.exists():
        raise ValueError("channel gains not supplied and recording sidecar is unavailable")
    document = json.loads(sidecar.read_text())
    gains = document.get("channelGains")
    if not gains:
        raise ValueError("recording sidecar lacks calibrated channelGains")
    return [float(value) for value in gains], str(sidecar)


def channel_calibration(iq, gains_db):
    """Return physical-domain noise variances and clipping diagnostics."""
    if len(gains_db) != iq.shape[0]:
        raise ValueError("one calibrated gain is required per channel")
    gains = np.power(10.0, np.asarray(gains_db, dtype=float) / 20.0)
    variances, clipped = [], []
    for channel in iq:
        # Adjacent differences suppress the coherent broadcast/direct path;
        # median complex difference power / 2 estimates observed noise power.
        delta = np.diff(channel)
        observed = float(np.median(np.abs(delta) ** 2) / (2.0 * math.log(2.0)))
        variances.append(max(observed, 1e-12))
        clipped.append(float(np.mean((np.abs(channel.real) >= .999) |
                                     (np.abs(channel.imag) >= .999))))
    physical = np.asarray(variances) / np.square(gains)
    return gains, physical, np.asarray(clipped)


def robust_rank_one_weights(training, noise_variances, clip_quantile=.9):
    """Fit a winsorized rank-one spatial mode and GLS-combine its donors."""
    # training is donors x slow-time x frequency in physical units.
    flat = training.reshape(training.shape[0], -1)
    energy = np.sum(np.abs(flat) ** 2, axis=0)
    cap = max(float(np.quantile(energy, clip_quantile)), 1e-18)
    weights = np.minimum(1.0, cap / np.maximum(energy, 1e-18))
    covariance = (flat * weights[None, :]) @ flat.conj().T / max(weights.sum(), 1.0)
    _, vectors = np.linalg.eigh(covariance)
    mode = vectors[:, -1]
    anchor = int(np.argmax(np.abs(mode)))
    mode *= np.exp(-1j * np.angle(mode[anchor]))
    precision = 1.0 / np.maximum(noise_variances, 1e-18)
    combine = mode.conj() * precision
    combine /= max(float(np.sum(np.abs(mode) ** 2 * precision)), 1e-18)
    return combine


def cross_fitted_references(spectra, gains, noise_variances):
    """Synthesize each held-out reference using opposite-fold rank-one fits."""
    physical = spectra / gains[:, None, None]
    output = []
    for held in range(spectra.shape[0]):
        donors = np.array([i for i in range(spectra.shape[0]) if i != held])
        reference = np.empty_like(spectra[held])
        reference_noise = np.empty(spectra.shape[1], dtype=float)
        for fold in (0, 1):
            evaluation = np.arange(fold, spectra.shape[1], 2)
            training = np.arange(1 - fold, spectra.shape[1], 2)
            combine = robust_rank_one_weights(
                physical[donors][:, training], noise_variances[donors])
            reference[evaluation] = np.einsum(
                "c,cbf->bf", combine, physical[donors][:, evaluation])
            reference_noise[evaluation] = float(np.sum(
                np.abs(combine) ** 2 * noise_variances[donors]))
        output.append((physical[held], reference, noise_variances[held],
                       reference_noise))
    return output


def fixed_subbands(fft_size, count, edge_fraction=.05):
    usable = np.arange(int(math.ceil(edge_fraction * fft_size)),
                       int(math.floor((1.0 - edge_fraction) * fft_size)))
    return [band for band in np.array_split(usable, count) if len(band)]


def consensus_surface(iq, fft_size, max_delay_samples, max_doppler_hz,
                      subbands, gains_db):
    blocks = iq.shape[1] // fft_size
    data = iq[:, :blocks * fft_size].reshape(
        iq.shape[0], blocks, fft_size).copy()
    data -= data.mean(axis=2, keepdims=True)
    gains, noise, clipped = channel_calibration(iq, gains_db)
    spectra = np.fft.fft(data, axis=2)
    prf = MCHQ.SAMPLE_RATE / fft_size
    doppler = np.fft.fftshift(np.fft.fftfreq(blocks, d=1.0 / prf))
    rows = np.flatnonzero(np.abs(doppler) <= max_doppler_hz)
    views = []
    view_diagnostics = []
    bands = fixed_subbands(fft_size, subbands)
    for held, (test, reference, test_noise, reference_noise) in enumerate(
            cross_fitted_references(spectra, gains, noise)):
        for band_index, band in enumerate(bands):
            product = np.zeros_like(test)
            product[:, band] = test[:, band] * np.conj(reference[:, band])
            correlations = np.fft.ifft(product, axis=1)[:, :max_delay_samples]
            ambiguity = np.fft.fftshift(np.fft.fft(correlations, axis=0), axes=0)
            power = np.abs(ambiguity[rows]) ** 2
            test_energy = float(np.mean(np.abs(test[:, band]) ** 2))
            reference_energy = float(np.mean(np.abs(reference[:, band]) ** 2))
            rn = float(np.mean(reference_noise))
            # Variance of a noisy cross product.  Gain calibration is applied
            # inside this physical-domain likelihood, not as output rescaling.
            variance = (test_noise * reference_energy + rn * test_energy +
                        test_noise * rn)
            censor_information = max(0.0, 1.0 - clipped[held])
            likelihood = censor_information * power / max(variance, 1e-18)
            background = max(float(np.median(likelihood)), 1e-18)
            views.append(np.log1p(likelihood / background))
            view_diagnostics.append({
                "held_out_channel": held, "subband": band_index,
                "noise_variance_physical": float(test_noise),
                "reference_noise_variance_physical": rn,
                "clipped_fraction": float(clipped[held]),
                "median_likelihood": background})
    stack = np.stack(views)
    # Fixed lower quartile and agreement fraction require evidence across the
    # bank.  Neither operation can select a favorable antenna or subband.
    lower_quartile = np.quantile(stack, .25, axis=0)
    agreement = np.mean(stack >= np.median(stack, axis=(1, 2))[:, None, None], axis=0)
    return doppler[rows], lower_quartile * agreement, view_diagnostics


def cyclostationarity_scan(iq, decimation=128, windows=4, z_threshold=8.0,
                           tolerance_bins=1):
    """Cheap envelope cyclic-frequency stability gate on retained IQ."""
    count = (iq.shape[1] // decimation) * decimation
    envelope = np.mean(np.abs(iq[:, :count]) ** 2, axis=0)
    envelope = envelope.reshape(-1, decimation).mean(axis=1)
    parts = np.array_split(envelope, windows)
    peaks = []
    for part in parts:
        part = part - np.mean(part)
        spectrum = np.abs(np.fft.rfft(part * np.hanning(len(part)))) ** 2
        spectrum[0] = 0.0
        median = float(np.median(spectrum[1:]))
        mad = float(np.median(np.abs(spectrum[1:] - median)))
        index = int(np.argmax(spectrum))
        z = (float(spectrum[index]) - median) / max(1.4826 * mad, 1e-18)
        peaks.append((index, z, len(part)))
    stable = False
    frequency = None
    qualifying = [(index, z, size) for index, z, size in peaks if z >= z_threshold]
    if len(qualifying) >= max(2, int(math.ceil(.75 * windows))):
        indices = [item[0] for item in qualifying]
        stable = max(indices) - min(indices) <= tolerance_bins
        if stable:
            frequency = float(np.median([
                index * (MCHQ.SAMPLE_RATE / decimation) / size
                for index, _, size in qualifying]))
    return {"method": "decimated_squared-envelope cyclic-frequency scan",
            "decimation": decimation, "windows": windows,
            "z_threshold": z_threshold,
            "peaks": [{"bin": i, "robust_z": z} for i, z, _ in peaks],
            "stable_peaks": stable, "cyclic_frequency_hz": frequency,
            "cyclic_detection_proceeded": stable}


def score(surfaces, doppler_axis, trajectory, guard, controls_stride):
    frames = min(len(surfaces), len(trajectory))
    delay_axis = np.arange(surfaces[0].shape[1]) * MCHQ.C_KM_S / MCHQ.SAMPLE_RATE
    target_rows = [MCHQ.nearest(doppler_axis, item["doppler_hz"])
                   for item in trajectory[:frames]]
    target_columns = [MCHQ.nearest(delay_axis, item["delay_km"])
                      for item in trajectory[:frames]]
    row_offsets = range(-min(target_rows), len(doppler_axis) - max(target_rows),
                        controls_stride)
    column_offsets = range(-min(target_columns), surfaces[0].shape[1] - max(target_columns),
                           controls_stride)
    offsets = [(r, c) for r in row_offsets for c in column_offsets
               if abs(r) > guard or abs(c) > guard]
    candidates = [{"kind": "target", "doppler_offset_bins": 0,
                   "delay_offset_bins": 0}]
    candidates += [{"kind": "translated_control", "doppler_offset_bins": r,
                    "delay_offset_bins": c} for r, c in offsets]
    for candidate in candidates:
        r, c = candidate["doppler_offset_bins"], candidate["delay_offset_bins"]
        values = [float(surface[row + r, column + c])
                  for surface, row, column in zip(surfaces, target_rows, target_columns)]
        candidate["score"] = float(np.mean(values))
    candidates.sort(key=lambda item: item["score"], reverse=True)
    for rank, candidate in enumerate(candidates, 1):
        candidate["rank"] = rank
    target = next(item for item in candidates if item["kind"] == "target")
    controls = [item["score"] for item in candidates if item["kind"] != "target"]
    threshold = percentile(controls)
    margin = db_ratio(target["score"], threshold)
    return {"target_score": target["score"], "control_p99": threshold,
            "margin_over_control_p99_db": margin,
            "target_rank_of_all_candidates": target["rank"],
            "controls_exceeding_target": sum(value >= target["score"] for value in controls),
            "validated_at_plus_3db": margin >= 3.0}, candidates


def analyze(recording, trajectory_path, event_id, gains_db, cpi, fft_size,
            max_delay_km, max_doppler_hz, subbands, guard, controls_stride,
            max_frames=None):
    trajectory, trajectory_source = load_trajectory(
        trajectory_path, event_id=event_id, recording=recording)
    gains_db, gains_source = load_gains(recording, gains_db)
    if max_frames is not None:
        trajectory = trajectory[:max_frames]
    cpi_samples = int(round(cpi * MCHQ.SAMPLE_RATE))
    max_delay_samples = int(math.ceil(max_delay_km * MCHQ.SAMPLE_RATE /
                                      MCHQ.C_KM_S)) + 1
    surfaces, diagnostics = [], []
    scan = None
    doppler = None
    for index, iq in enumerate(MCHQ.frames(recording, cpi_samples)):
        if index >= len(trajectory):
            break
        if scan is None:
            scan = cyclostationarity_scan(iq)
        doppler, surface, views = consensus_surface(
            iq, fft_size, max_delay_samples, max_doppler_hz, subbands, gains_db)
        surfaces.append(surface)
        diagnostics.append(views)
    if not surfaces:
        raise ValueError("recording has no complete CPI with trajectory truth")
    summary, candidates = score(surfaces, doppler, trajectory, guard, controls_stride)
    return {"schema": "vectorwarp-cross-fitted-hdr-v1", "candidate_only": True,
            "recording": str(recording), "trajectory": str(trajectory_path),
            "trajectory_source": trajectory_source, "frames": len(surfaces),
            "sample_rate_hz": MCHQ.SAMPLE_RATE, "cpi_s": cpi,
            "configuration": {"fft_size": fft_size, "subbands": subbands,
                "max_delay_km": max_delay_km, "max_doppler_hz": max_doppler_hz,
                "channel_gains_db": gains_db, "channel_gains_source": gains_source,
                "guard_bins": guard,
                "controls_stride_bins": controls_stride,
                "plus_3db_is_post_scoring_label_only": True},
            "reference": {"method": "opposite-fold winsorized rank-one leave-one-channel-out",
                "target_protection": "held-out channel excluded; donor covariance winsorized; no score-directed fitting"},
            "likelihood": {"method": "per-channel gain/noise calibrated heteroscedastic cross-product likelihood",
                "censor_aware": True, "simple_inverse_gain_output_scaling": False},
            "consensus": {"method": "fixed lower quartile times agreement fraction",
                "best_view_selection": False, "views_per_frame": len(diagnostics[0])},
            "cyclostationarity_gate": scan, "score": summary,
            "candidate_scores_ranked": candidates, "view_diagnostics": diagnostics,
            "deployment_note": "Replay-only; no RF or live configuration change."}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recording", required=True, type=Path)
    parser.add_argument("--trajectory", required=True, type=Path,
                        help="Corrected manifest (preferred) or trajectory JSON")
    parser.add_argument("--event-id")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--channel-gains-db",
                        help="Comma-separated calibrated gains; defaults to MCHQ sidecar")
    parser.add_argument("--cpi", type=float, default=.5)
    parser.add_argument("--fft-size", type=int, default=1024)
    parser.add_argument("--subbands", type=int, default=4)
    parser.add_argument("--max-delay-km", type=float, default=64)
    parser.add_argument("--max-doppler-hz", type=float, default=800)
    parser.add_argument("--guard", type=int, default=2)
    parser.add_argument("--controls-stride", type=int, default=4)
    parser.add_argument("--max-frames", type=int)
    args = parser.parse_args()
    gains = ([float(value) for value in args.channel_gains_db.split(",")]
             if args.channel_gains_db else None)
    result = analyze(args.recording, args.trajectory, args.event_id, gains,
                     args.cpi, args.fft_size, args.max_delay_km,
                     args.max_doppler_hz, args.subbands, args.guard,
                     args.controls_stride, args.max_frames)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
