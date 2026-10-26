"""Read verified native snapshots; never estimate covariance or steering weights.

The sibling native verifier runs the production canonical decoder. This reader
then binds each verified file to admitted IQ/RF identity and selects bounded
truth cells. Full snapshots remain as durable artifacts for arbitrary nuisance
and multi-CPI/TBD inspection.
"""
import hashlib
import math
import os
from pathlib import Path
import struct

LIMIT = 256 << 20
C_KM_S = 299792.458


def sample_clock(capture, metadata, sample_rate):
    if isinstance(sample_rate, bool) or not isinstance(sample_rate, (int, float)) or \
       not math.isfinite(sample_rate) or sample_rate <= 0:
        raise ValueError("native sample clock requires a positive sample rate")
    epoch, first, end = (metadata.get(k) for k in ("epoch", "firstEpochSample", "endEpochSample"))
    if not all(isinstance(v, int) and not isinstance(v, bool) for v in (epoch, first, end)) or \
       epoch <= 0 or first < 0 or end <= first:
        raise ValueError("native export requires admitted recording sample provenance")
    anchors, uncertainties = [], []
    for cpi in capture["processor_cpis"]:
        left, right, timestamp = (cpi.get(k) for k in
            ("first_epoch_sample", "end_epoch_sample", "timestamp_ms"))
        if cpi.get("epoch") != epoch or not all(isinstance(v, int) and not isinstance(v, bool) for v in (left, right)) or \
           not first <= left < right <= end or not isinstance(timestamp, (int, float)) or \
           isinstance(timestamp, bool) or not math.isfinite(timestamp):
            raise ValueError("native export requires timestamped admitted CPI sample ranges")
        anchors.append(((left + right) / 2, float(timestamp)))
        uncertainty = cpi.get("event_time_uncertainty_s", 0)
        if not isinstance(uncertainty, (int, float)) or isinstance(uncertainty, bool) or \
           not math.isfinite(uncertainty) or not 0 <= uncertainty <= 5:
            raise ValueError("native sample-clock uncertainty is invalid")
        uncertainties.append((anchors[-1], 0 if cpi.get("exact_event_time") is True else uncertainty * 1000))
    anchors.sort()
    if len(anchors) < 3 or any(b[0] <= a[0] or b[1] <= a[1] for a, b in zip(anchors, anchors[1:])):
        raise ValueError("native export requires three ordered sample-clock anchors")
    uncertainties.sort()
    origin, tolerance = uncertainties[0]
    for anchor, uncertainty in uncertainties[1:]:
        expected_ms = (anchor[0] - origin[0]) * 1000 / sample_rate
        if abs((anchor[1] - origin[1]) - expected_ms) > 2 + tolerance + uncertainty:
            raise ValueError("native sample-clock anchors disagree with admitted sample rate and uncertainty")
    # Admission may contain provisional host timing. Use its explicit sample
    # anchors (and retain its timing policy), never replay wall-clock time.
    def at(sample):
        if not anchors[0][0] <= sample <= anchors[-1][0]:
            raise ValueError("native truth CUT is outside the admitted sample-clock interval")
        for left, right in zip(anchors, anchors[1:]):
            if left[0] <= sample <= right[0]:
                return left[1] + (sample - left[0]) / (right[0] - left[0]) * (right[1] - left[1])
        return anchors[-1][1]
    return anchors, at


def snapshot_request(capture, metadata, config, source_hash, rf_hash, prefix):
    rate = config["capture"]["fs"]
    samples = int(rate * config["process"]["data"]["cpi"])
    if samples <= 0:
        raise ValueError("native replay CPI sample count is empty")
    anchors, clock = sample_clock(capture, metadata, rate)
    first = metadata["firstEpochSample"]
    # A bounded representative window for each eligible pass. Selection uses
    # only admitted sample geometry; it does not inspect detections or beams.
    low = max(1, math.ceil((anchors[0][0] - first) / samples + .5))
    high = min((metadata["endEpochSample"] - first) // samples,
               math.floor((anchors[-1][0] - first) / samples + .5))
    if high - low + 1 < 3:
        raise ValueError("eligible pass has fewer than three replay CPIs inside admitted truth coverage")
    count = min(8, high - low + 1)
    start = (low + high - count + 1) // 2
    finish = start + count - 1
    for frame in (start, finish):
        clock(first + (frame - .5) * samples)
    return {"prefix": str(prefix), "first_frame": start, "last_frame": finish,
            "max_frames": count, "max_bytes": LIMIT, "max_total_bytes": 2 << 30,
            "source_epoch": metadata["epoch"], "first_epoch_sample": first,
            "end_epoch_sample": metadata["endEpochSample"],
            "source_sha256": source_hash, "rf_sha256": rf_hash,
            "actual_channel_gains_db": metadata["channelGains"]}


class Snapshot:
    def __init__(self, receipt, request, config):
        path = Path(receipt.get("path", ""))
        if receipt.get("native_contract_verified") is not True or not path.is_file() or \
           path.is_symlink() or not 2048 <= path.stat().st_size <= LIMIT:
            raise ValueError("native snapshot missing, unverified, or outside bound")
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "rb") as source:
            self.data = source.read(LIMIT + 1)
        if not 2048 <= len(self.data) <= LIMIT:
            raise ValueError("native snapshot changed beyond its byte bound")
        sha = hashlib.sha256(self.data).hexdigest()
        if sha != receipt.get("sha256"):
            raise ValueError("native snapshot changed after native contract verification")
        self.receipt = {**receipt, "bytes": len(self.data)}
        u32, u64, f64 = self.u32, self.u64, self.f64
        if (u32(0), u32(4), u32(8), u32(12), u64(16)) != (0x52505331, 1, 2048, 20, len(self.data)):
            raise ValueError("native snapshot envelope differs")
        check = bytearray(self.data); check[256:288] = bytes(32)
        if hashlib.sha256(check).digest() != self.data[256:288]:
            raise ValueError("native snapshot envelope checksum differs")
        self.blocks = {}
        at = 2048
        for kind in range(1, 21):
            d = 512 + (kind - 1) * 24
            size = u64(d + 16)
            if u32(d) != kind or u32(d + 4) or u64(d + 8) != at or size > len(self.data) - at:
                raise ValueError("native snapshot block coverage differs")
            self.blocks[kind] = (at, size); at += size
        if at != len(self.data):
            raise ValueError("native snapshot trailing bytes")
        self.rows, self.cols, self.channels, self.samples = [u32(x) for x in (76, 80, 84, 88)]
        self.beams, self.depth, self.passes = u32(288), u32(296), u32(316)
        self.frame = u64(32)
        if not (0 < self.rows and 0 < self.cols and 0 < self.channels <= 8) or \
           self.rows * self.cols * self.channels * 8 > len(self.data):
            raise ValueError("native snapshot dimensions invalid")
        if u64(24) != 1 or u64(40) != self.frame or u64(48) != self.frame or \
           not request["first_frame"] <= self.frame <= request["last_frame"] or \
           self.data[192:224].hex() != request["source_sha256"] or \
           self.data[224:256].hex() != request["rf_sha256"] or \
           f64(320) != config["capture"]["fc"] or f64(328) != config["capture"]["fs"] or \
           f64(336) != config["process"]["detection"]["pfa"] or \
           self.channels != config["capture"]["device"]["channel_count"] or \
           self.samples != int(config["capture"]["fs"] * config["process"]["data"]["cpi"]) or \
           [f64(400 + 8 * c) for c in range(self.channels)] != request["actual_channel_gains_db"] or \
           u64(464) != request["max_bytes"] or u32(472) != request["max_frames"]:
            raise ValueError("native snapshot input/frame/RF/gain identity differs")
        metadata = self.blocks[5][0]
        self.doppler = [f64(metadata + 384 + 8 * r) for r in range(self.rows)]
        self.delay_samples = [self.i32(metadata + 384 + 8 * self.rows + 4 * c) for c in range(self.cols)]
        self.delay = [d * C_KM_S / f64(328) for d in self.delay_samples]
        config_start, config_size = self.blocks[20]
        native_config = self.data[config_start:config_start + config_size]
        axes = b"".join(struct.pack("<i", d) for d in self.delay_samples) + \
               b"".join(struct.pack("<d", d) for d in self.doppler)
        if hashlib.sha256(native_config + axes).digest() != self.data[96:128]:
            raise ValueError("native configuration/axis digest differs")
        mode = {"noncoherent": 0, "coherent_beam_bank": 1, "noise_whitened_beam_bank": 2}
        if self.u32(config_start + 20) != mode[config["process"]["surveillance_fusion"]["mode"]]:
            raise ValueError("native fusion mode differs from requested production arm")
        raw_start, raw_size = self.blocks[14]
        if hashlib.sha256(self.data[raw_start:raw_start + raw_size]).digest() != self.data[160:192]:
            raise ValueError("native raw IQ checksum differs")

    def u32(self, at): return struct.unpack_from("<I", self.data, at)[0]
    def i32(self, at): return struct.unpack_from("<i", self.data, at)[0]
    def u64(self, at): return struct.unpack_from("<Q", self.data, at)[0]
    def f64(self, at): return struct.unpack_from("<d", self.data, at)[0]

    def vector(self, kind, width, count, start=0):
        offset, size = self.blocks[kind]
        if start < 0 or count < 0 or (start + count) * width > size:
            raise ValueError("native block vector is incomplete")
        values = struct.unpack_from("<" + ("d" if width == 8 else "f") * count,
                                    self.data, offset + start * width)
        # Native temporal warmup NaNs are represented explicitly by null.
        if any(math.isinf(v) for v in values):
            raise ValueError("native block contains infinite values")
        return [v if math.isfinite(v) else None for v in values]

    def cell(self, row, col):
        cells, index = self.rows * self.cols, row * self.cols + col
        physical = [self.vector(13, 4, 2, 2 * (c * cells + col * self.rows +
                    (row + self.rows // 2 + 1) % self.rows)) for c in range(self.channels)]
        powers = [self.vector(10, 8, 1, b * cells + index)[0] for b in range(max(1, self.beams))]
        depth = self.depth if self.depth > 2 else 1
        history = [self.vector(6, 8, 1, slot * cells + index)[0] for slot in range(depth)]
        temporal = [self.vector(9, 8, 1, p * cells + index)[0] for p in range(self.passes)] \
                   if self.depth > 2 else []
        stats = [self.vector(11, 8, 5, (p * cells + index) * 5) for p in range(self.passes)]
        angle_start, angle_size = self.blocks[7]
        angles = [self.u32(angle_start + (slot * cells + index) * 4) for slot in range(depth)] \
                 if angle_size else []
        return {"physical_channels": physical, "beam_powers": powers,
                "native_power_history": history, "native_angle_history": angles,
                "native_temporal_banks": temporal, "native_cfar_statistics": stats,
                "native_decision_flags": self.u32(self.blocks[17][0] + index * 4)}

    def export(self, timestamp, truth, radius=(2, 2)):
        if not min(self.delay) <= truth["delay_km"] <= max(self.delay) or \
           not min(self.doppler) <= truth["doppler_hz"] <= max(self.doppler):
            raise ValueError("truth-selected native CUT is outside the exported radar geometry")
        row = min(range(self.rows), key=lambda i: abs(self.doppler[i] - truth["doppler_hz"]))
        col = min(range(self.cols), key=lambda i: abs(self.delay[i] - truth["delay_km"]))
        nuisance = []
        for r in range(max(0, row - radius[1]), min(self.rows, row + radius[1] + 1)):
            for c in range(max(0, col - radius[0]), min(self.cols, col + radius[0] + 1)):
                if (r, c) != (row, col):
                    nuisance.append({"row": r, "column": c, "delay_km": self.delay[c],
                                     "doppler_hz": self.doppler[r], **self.cell(r, c)})
        covariance = [self.vector(2, 8, 2, 2 * (left * 8 + right))
                      for left in range(8) for right in range(8)]
        weights = [self.vector(3, 4, 2, 2 * i) for i in range(self.beams * self.channels)]
        config_start = self.blocks[20][0]
        active_covariance = [[covariance[left * self.channels + right]
            for right in range(self.channels)] for left in range(self.channels)]
        reference_channels = [self.u32(config_start + 168 + 4 * index) for index in range(self.u32(92))]
        reference = [[0.0, 0.0] for _ in range(self.channels)]
        for index, channel in enumerate(reference_channels):
            reference[channel] = self.vector(1, 8, 2, 8 + 2 * index)
        return {"timestamp_ms": timestamp, "native_signal_timestamp_ms": self.u64(56),
                "logical_frame": self.frame, "truth": truth,
                "row": row, "column": col, "target_cut": self.cell(row, col),
                "nuisance_cells": nuisance, "native_snapshot": self.receipt,
                "nuisance_window": {"delay_radius_bins": radius[0], "doppler_radius_bins": radius[1],
                    "scope": "bounded local diagnostic; complete CUT/training grid remains in native snapshot"},
                "native_config_sha256": self.data[96:128].hex(),
                "native_module_sha256": self.data[128:160].hex(),
                "native_device_sha256": self.data[480:512].hex(),
                "raw_iq_sha256": self.data[160:192].hex(),
                "source_sha256": self.data[192:224].hex(), "rf_sha256": self.data[224:256].hex(),
                "frequency_hz": self.f64(320), "sample_rate_hz": self.f64(328),
                "actual_channel_gains_db": [self.f64(400 + 8 * index) for index in range(self.channels)],
                "channel_mapping": [self.u32(config_start + 200 + 4 * index) for index in range(self.channels)],
                "covariance_whitening": {"state": "native-resident-settled-snapshot",
                    "covariance_valid": bool(self.u32(72)), "covariance_storage_complex": covariance,
                    "covariance_active_stride": self.channels,
                    "covariance_matrix": active_covariance,
                    "covariance_analysis_samples": self.u32(config_start + 120),
                    "reference_weights": reference,
                    "actual_fusion_weights": weights,
                    "reference_checkpoint_f64": self.vector(1, 8, 19, 8)},
                "native_temporal_state": {"integration_cpis": self.depth,
                    "next_write_counter": self.u32(self.blocks[5][0] + 79 * 4),
                    "warmup_nulls": True, "artifact_contains_complete_banks": True}}
