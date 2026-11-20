#!/usr/bin/env python3
"""Bounded offline HP-array IQ diagnostics for calibrated MCHQ recordings.

This observational tool never contacts a receiver or uses ADS-B.  It labels a
matrix as noise evidence only when the caller supplies an independently chosen
noise segment; arbitrary selected IQ is reported separately as observed IQ.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np

MAGIC, MCHT_MAGIC = 0x4D434851, 0x4D434854
MAX_PACKET_SAMPLES, DEFAULT_MAX_SAMPLES = 65536, 1_000_000


def db(value): return 10 * math.log10(max(float(value), 1e-30))


def read_exact(stream, size):
    value = stream.read(size)
    if len(value) != size: raise ValueError("truncated MCHQ recording")
    return value


def iter_mchq(path, digest=None):
    """Portable MCHQ reader; rejects packets larger than recorder contract."""
    with path.open("rb") as stream:
        while True:
            encoded = stream.read(32)
            if not encoded: return
            if len(encoded) != 32: raise ValueError("truncated MCHQ header")
            magic, channels, samples, phase, noise, counter, group, retune = struct.unpack(">8I", encoded)
            if magic not in (MAGIC, MCHT_MAGIC) or not 1 <= channels <= 8 or not 1 <= samples <= MAX_PACKET_SAMPLES:
                raise ValueError("malformed or unbounded MCHQ header")
            metadata_bytes = read_exact(stream, channels * 8)
            metadata = struct.unpack("<" + "f" * (channels * 2), metadata_bytes)
            if not all(math.isfinite(value) for value in metadata):
                raise ValueError("non-finite MCHQ channel metadata")
            timing_bytes, timing = b"", None
            if magic == MCHT_MAGIC:
                timing_bytes = read_exact(stream, 96)
                version, size, flags, reference, *fields = struct.unpack(">4I10Q", timing_bytes)
                if (version, size) != (1, 96) or reference >= channels:
                    raise ValueError("unsupported Heimdall timing extension")
                timing = dict(zip(("epoch", "first_sample", "transfer_sequence", "monotonic_ns", "tai_ns",
                                   "realtime_ns", "clock_read_span_ns", "system_max_error_ns", "buffer_sequence", "anchor_sample"), fields))
                timing.update(flags=flags, reference_channel=reference)
                if (flags & ~7 or not timing["epoch"] or not 0 < timing["realtime_ns"] < 2 ** 63 or
                        timing["first_sample"] + samples >= 2 ** 64 or
                        timing["anchor_sample"] < timing["first_sample"] + samples):
                    raise ValueError("invalid Heimdall timing extension")
            payload = read_exact(stream, channels * samples * 2)
            if digest is not None:
                for block in (encoded, metadata_bytes, timing_bytes, payload): digest.update(block)
            raw = np.frombuffer(payload, dtype=np.uint8).reshape(channels, samples, 2)
            yield ({"channels": channels, "samples": samples, "phase": phase, "noise": noise,
                    "frequency_change_counter": counter, "group": group, "retune": retune,
                    "frequencies": list(metadata[::2]), "gains": list(metadata[1::2]), "timing": timing}, raw)


def timing_contiguous(previous, current):
    a, b = previous["timing"], current["timing"]
    if a is None or b is None: return a is b
    return (a["epoch"] == b["epoch"] and a["reference_channel"] == b["reference_channel"] and
            a["first_sample"] + previous["samples"] == b["first_sample"] and
            a["transfer_sequence"] + 1 == b["transfer_sequence"] and
            a["buffer_sequence"] + 1 == b["buffer_sequence"] and
            (not (a["flags"] & b["flags"] & 1) or b["monotonic_ns"] > a["monotonic_ns"]))


def synchronized(header):
    return (header["phase"] & 0xff == 4 and not (header["phase"] & 0xe00) and
            header["noise"] == 0 and header["retune"] == 0)


def parse_range(value, name):
    try: start, count = (int(part) for part in value.split(":", 1))
    except (ValueError, AttributeError): raise argparse.ArgumentTypeError(f"{name} must be START:COUNT") from None
    if start < 0 or count < 1: raise argparse.ArgumentTypeError(f"{name} needs non-negative START and positive COUNT")
    return start, count


def overlap(start, count, requested):
    if requested is None: return None
    left, right = max(start, requested[0]), min(start + count, requested[0] + requested[1])
    return None if right <= left else (left - start, right - left)


class Moments:
    def __init__(self, channels):
        self.count = 0; self.total = np.zeros(channels, complex); self.power = np.zeros(channels)
        self.mean = np.zeros(channels, complex)
        self.centered_outer = np.zeros((channels, channels), complex)
    def add(self, values):
        if not values.shape[1]: return
        count = values.shape[1]; combined_count = self.count + count
        batch_mean = values.mean(axis=1); centered = values - batch_mean[:, None]
        delta = batch_mean - self.mean
        # Merge centered moments without subtracting two nearly equal raw powers.
        # Raw-moment cancellation can invent variance in long constant recordings.
        self.centered_outer += centered @ centered.conj().T + np.outer(delta, delta.conj()) * (self.count * count / combined_count)
        self.mean += delta * (count / combined_count)
        self.count = combined_count; self.total += values.sum(axis=1)
        self.power += (np.abs(values) ** 2).sum(axis=1)
    def covariance(self):
        if not self.count: return None
        return self.centered_outer / self.count


class PhaseMoments:
    def __init__(self, channels, reference):
        self.count, self.reference, self.total = 0, reference, np.zeros(channels, complex)
        self.channel_counts = np.zeros(channels, dtype=np.int64)
        self.moments = Moments(channels)
    def add(self, values):
        self.count += values.shape[1]; self.moments.add(values)
        ref = values[self.reference]; valid_ref = np.abs(ref) > 1e-12
        if not valid_ref.any(): return
        for channel in range(values.shape[0]):
            product = values[channel, valid_ref] * ref[valid_ref].conj(); valid = np.abs(product) > 1e-12
            self.total[channel] += np.sum(product[valid] / np.abs(product[valid]))
            self.channel_counts[channel] += int(valid.sum())


def normalized(covariance):
    if covariance is None: return None
    diagonal = np.maximum(covariance.diagonal().real, 0)
    if np.any(diagonal <= 1e-30): return None
    return covariance / np.sqrt(np.outer(diagonal, diagonal))


def serial(matrix):
    return [[[float(v.real), float(v.imag)] for v in row] for row in matrix]


def range_document(requested, observed):
    return {"requested_start_sample": requested[0], "requested_sample_count": requested[1],
            "observed_start_sample": requested[0] if observed else None, "observed_sample_count": observed,
            "complete": observed == requested[1], "sample_index_basis": "zero-based per-channel offset in this file"}


def analyze(path, analysis_stride=1, contiguous_samples=None, sample_start=0, sample_count=DEFAULT_MAX_SAMPLES,
            channel_order=None, noise_range=None, direct_anchor_range=None, reference_channel=0,
            sample_rate_hz=None):
    """Compute exact moments from bounded ranges, retaining no recording samples."""
    del analysis_stride, contiguous_samples  # Compatibility with v1 callers.
    path = Path(path)
    if sample_start < 0 or sample_count is None or sample_count < 1: raise ValueError("analysis range must be bounded")
    if sample_rate_hz is not None and (not math.isfinite(sample_rate_hz) or sample_rate_hz <= 0):
        raise ValueError("sample rate must be finite and positive")
    for name, selected in (("noise", noise_range), ("direct-anchor", direct_anchor_range)):
        if selected is not None and (selected[0] < 0 or selected[1] < 1): raise ValueError(f"{name} range must be bounded")
    initial_stat, digest = path.stat(), hashlib.sha256()
    analysis_range, first, total_samples, packets, warnings = (sample_start, sample_count), None, 0, 0, set()
    analysis = noise = phase = None; expected = previous = None
    coherent, continuity = True, True
    for header, raw in iter_mchq(path, digest):
        if first is None:
            first = header; channels = header["channels"]
            if not 0 <= reference_channel < channels: raise ValueError("reference channel is outside recorded channel order")
            if channel_order is None: channel_order = [f"channel_{i}" for i in range(channels)]
            if len(channel_order) != channels or len(set(channel_order)) != channels or any(not str(value).strip() for value in channel_order): raise ValueError("channel order must name every channel exactly once")
            analysis = Moments(channels); noise = Moments(channels) if noise_range else None
            phase = PhaseMoments(channels, reference_channel) if direct_anchor_range else None
            expected = {key: header[key] for key in ("channels", "frequencies", "gains", "frequency_change_counter", "group", "phase", "noise", "retune")}
        elif header["channels"] != first["channels"]: raise ValueError("channel count changed inside recording")
        if any(header[key] != expected[key] for key in expected): warnings.add("receiver_metadata_changed_inside_recording")
        coherent = coherent and synchronized(header)
        if previous is not None and not timing_contiguous(previous, header): continuity = False
        previous = header
        values = ((raw[:, :, 0].astype(float) - 127.5) + 1j * (raw[:, :, 1].astype(float) - 127.5)) / 127.5
        for selected_range, moments in ((analysis_range, analysis), (noise_range, noise)):
            selected = overlap(total_samples, values.shape[1], selected_range)
            if selected and moments: offset, count = selected; moments.add(values[:, offset:offset + count])
        selected = overlap(total_samples, values.shape[1], direct_anchor_range)
        if selected and phase: offset, count = selected; phase.add(values[:, offset:offset + count])
        total_samples += values.shape[1]; packets += 1
    final_stat = path.stat()
    if any(getattr(initial_stat, key) != getattr(final_stat, key) for key in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
        raise ValueError("recording changed during analysis")
    if first is None: raise ValueError("empty MCHQ recording")
    if not analysis.count: raise ValueError("requested analysis sample range is outside recording")
    evidence_reason = ("receiver_metadata_changed_inside_recording" if warnings else
                       "recording_contains_unsynchronized_iq" if not coherent else
                       "recording_timing_discontinuity" if not continuity else None)
    covariance = analysis.covariance(); eigenvalues, vectors = np.linalg.eigh(covariance); eigenvalues = np.maximum(eigenvalues.real, 0)
    trace, principal = max(float(eigenvalues.sum()), 1e-30), vectors[:, -1]
    observed_correlation = normalized(covariance)
    channel_metrics = []
    for channel in range(first["channels"]):
        power = analysis.power[channel] / analysis.count
        channel_metrics.append({"channel_index": channel, "channel_id": channel_order[channel], "mean_power_linear": float(power),
            "mean_power_dbfs": db(power), "dc_magnitude_fraction": float(abs(analysis.total[channel] / analysis.count)),
            "gain_db_reported_by_recording": first["gains"][channel], "principal_weight_magnitude": float(abs(principal[channel])),
            "principal_weight_phase_rad": float(np.angle(principal[channel]))})
    observed = {"available": observed_correlation is not None, "reason": None if observed_correlation is not None else "zero_or_invalid_centered_channel_power",
                "selection": range_document(analysis_range, analysis.count), "matrix_representation": "[real,imaginary] pairs",
                "matrix": serial(observed_correlation) if observed_correlation is not None else None}
    noise_result = {"available": False, "reason": "noise_segment_not_supplied", "selection": None, "normalized_complex_correlation_matrix": None}
    array_result = {"available": False, "reason": "noise_segment_not_supplied"}
    if noise_range:
        noise_correlation = normalized(noise.covariance()) if noise.count else None; valid = noise.count == noise_range[1] and noise_correlation is not None and evidence_reason is None
        noise_result = {"available": valid, "reason": None if valid else (evidence_reason or ("noise_segment_outside_recording" if not noise.count else "noise_segment_incomplete_or_zero_power")),
            "selection": range_document(noise_range, noise.count), "selection_provenance": "caller_asserted_noise_segment; not independently verified",
            "normalized_complex_correlation_matrix": serial(noise_correlation) if valid else None}
        array_result = {"available": False, "reason": noise_result["reason"]}
        if valid:
            total_noise = float(np.real(noise_correlation.sum()))
            if total_noise > 1e-12:
                n, effective = first["channels"], first["channels"] ** 2 / total_noise
                array_result = {"available": True, "reason": None, "weighting": "unit weights in recorded channel basis; normalized per-channel noise; no phase rotation applied",
                    "interpretation": "conditional model, not measured achieved array gain",
                    "signal_assumption": "equal unit signal response and phase in every noise-normalized channel",
                    "formula": "N^2 / real(1^H C_noise 1)",
                    "ideal_independent_noise_gain_linear": n, "effective_gain_linear": effective, "effective_gain_db": db(effective),
                    "coherence_loss_relative_to_independent_db": db(n / effective), "coherent_efficiency_relative_to_independent": effective / n}
            else: array_result = {"available": False, "reason": "invalid_noise_correlation_sum"}
    phase_result = {"available": False, "reason": "direct_anchor_or_reference_segment_not_supplied", "reference_channel_index": reference_channel, "per_channel": []}
    if direct_anchor_range:
        valid = (phase.count == direct_anchor_range[1] and phase.count > 0 and
                 normalized(phase.moments.covariance()) is not None and evidence_reason is None)
        phase_result = {"available": valid, "reason": None if valid else (evidence_reason or "direct_anchor_segment_outside_incomplete_or_zero_power"),
            "selection": range_document(direct_anchor_range, phase.count), "reference_channel_index": reference_channel,
            "selection_provenance": "caller_asserted_direct_anchor; source purity not verified",
            "phase_basis": "instantaneous relative phase of ADC-centered IQ; sample mean is not removed",
            "per_channel": []}
        if valid:
            for channel, count in enumerate(phase.channel_counts):
                if not count:
                    phase_result["per_channel"].append({"channel_index": channel, "channel_id": channel_order[channel],
                        "available": False, "reason": "no_nonzero_relative_phase_samples"})
                    continue
                value = phase.total[channel] / count
                resultant = min(1., max(0., float(abs(value))))
                phase_result["per_channel"].append({"channel_index": channel, "channel_id": channel_order[channel], "available": True,
                    "reason": None, "relative_phase_sample_count": int(count), "mean_relative_phase_rad": float(np.angle(value)) if resultant else None,
                    "mean_resultant_length": resultant,
                    "circular_standard_deviation_rad": math.sqrt(max(0., -2 * math.log(resultant))) if resultant else None,
                    "mean_phasor_power_fraction": resultant ** 2})
    return {"schema": "vectorwarp-hp-array-iq-diagnostic-v2", "offline_only": True, "inference_inputs": "recorded IQ only; ADS-B is not read or accepted",
        "processing": {"basis": "ADC-domain diagnostics; not production-equivalent",
            "production_preprocessing_applied": False,
            "difference_from_production": "HeimdallFrame::decode_payload removes each packet/channel mean and attenuates channels to minimum tuner gain; this tool uses ADC midpoint subtraction and range-wide covariance centering"},
        "input": {"path": str(path), "sha256": digest.hexdigest(), "bytes": final_stat.st_size, "format": "mchq",
                  "sample_rate_hz": sample_rate_hz,
                  "sample_rate_provenance": "caller_supplied" if sample_rate_hz is not None else "not_encoded_in_mchq",
                  "channel_order": channel_order, "analysis_range": range_document(analysis_range, analysis.count),
                  "iq_normalization": "(unsigned I,Q - 127.5) / 127.5; no tuner-gain equalization; power reference is unit complex magnitude"},
        "recording": {"channels": first["channels"], "packets": packets, "samples_per_channel": total_samples,
            "metadata_stable": not warnings, "metadata_warnings": sorted(warnings), "all_packets_synchronized": coherent,
            "sample_continuity": ("verified_from_mcht" if first["timing"] is not None else "not_encoded_in_mchq") if continuity else "discontinuous",
            "first_packet_metadata": first}, "channel_power": channel_metrics,
        "observed_centered_iq_correlation": observed, "noise_correlation": noise_result, "equal_weight_array_gain": array_result, "direct_anchor_phase": phase_result,
        # Legacy summary retained for existing callers.
        "channels": first["channels"], "array": {"principal_coherent_fraction": float(eigenvalues[-1] / trace), "zero_lag_correlation_magnitude": np.abs(observed_correlation).tolist() if observed_correlation is not None else None},
        "limitations": ["No antenna gain is inferred from recorded IQ.", "No phase stability is claimed without a supplied direct-anchor/reference segment.", "The noise matrix is unavailable unless the caller supplies an independently selected noise segment.",
            "Array gain assumes a unit steering vector after per-channel noise normalization; signal alignment and amplitudes are not measured.",
            "Circular phase standard deviation is not general phase RMS; mean-phasor power is not finite-array coherent efficiency.",
            "Caller-supplied channel labels and sample rate are not authenticated against the capture sidecar."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("--recording", action="append", required=True, help="LABEL=PATH; may be repeated"); parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--start-sample", type=int, default=0); parser.add_argument("--max-samples", type=int, default=DEFAULT_MAX_SAMPLES); parser.add_argument("--channel-order", help="comma-separated recorded channel IDs, in wire order")
    parser.add_argument("--noise-range", type=lambda value: parse_range(value, "--noise-range")); parser.add_argument("--direct-anchor-range", type=lambda value: parse_range(value, "--direct-anchor-range")); parser.add_argument("--reference-channel", type=int, default=0)
    parser.add_argument("--sample-rate-hz", type=float, help="sample rate from the recording sidecar or capture provenance; MCHQ does not encode it")
    args = parser.parse_args()
    if args.start_sample < 0 or args.max_samples < 1: parser.error("--start-sample must be non-negative and --max-samples positive")
    order, recordings = (args.channel_order.split(",") if args.channel_order else None), []
    for item in args.recording:
        if "=" not in item: parser.error("--recording must be LABEL=PATH")
        label, value = item.split("=", 1); result = analyze(Path(value), sample_start=args.start_sample, sample_count=args.max_samples, channel_order=order, noise_range=args.noise_range, direct_anchor_range=args.direct_anchor_range, reference_channel=args.reference_channel, sample_rate_hz=args.sample_rate_hz); result["label"] = label; recordings.append(result)
    document = {"schema": "vectorwarp-hp-array-iq-diagnostic-v2", "recordings": recordings}; encoded = json.dumps(document, indent=2, allow_nan=False); args.output.write_text(encoded + "\n"); print(encoded)

if __name__ == "__main__": main()
