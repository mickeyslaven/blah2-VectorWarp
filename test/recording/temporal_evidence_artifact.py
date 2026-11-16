#!/usr/bin/env python3
"""Validate and index full-precision VWTBD01 temporal replay artifacts."""

import argparse
import json
import math
import mmap
from pathlib import Path
import struct


def _take(view, position, size, name):
    if size < 0 or position < 0 or position + size > len(view):
        raise ValueError(f"truncated {name}")
    return bytes(view[position:position + size]), position + size


def _skip(view, position, size, name):
    if size < 0 or position < 0 or position + size > len(view):
        raise ValueError(f"truncated {name}")
    return position + size


def _number(view, position, code, name):
    size = struct.calcsize(code)
    if position < 0 or position + size > len(view):
        raise ValueError(f"truncated {name}")
    return struct.unpack_from("<" + code, view, position)[0], position + size


def _string(view, position, name):
    size, position = _number(view, position, "H", name)
    block, position = _take(view, position, size, name)
    try:
        return block.decode("utf8"), position
    except UnicodeDecodeError as error:
        raise ValueError(f"invalid {name}") from error


def _finite_doubles(view, offset, count, nonnegative=False):
    for index in range(count):
        value = struct.unpack_from("<d", view, offset + index * 8)[0]
        if not math.isfinite(value) or (nonnegative and value < 0):
            return False
    return True


def _signed_byte(value):
    return value if value < 128 else value - 256


def index_artifact(path):
    """Return zero-copy array offsets after fully validating record geometry."""
    stream = Path(path).open("rb")
    data = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
    view = memoryview(data)
    try:
        magic, position = _take(view, 0, 8, "artifact header")
        if bytes(magic) != b"VWTBD01\0":
            raise ValueError("temporal artifact magic is invalid")
        version, position = _number(view, position, "I", "version")
        maximum, position = _number(view, position, "Q", "maximum bytes")
        mature_cpis, position = _number(view, position, "I", "mature CPIs")
        delay_motion_bins, position = _number(view, position, "I", "delay motion bins")
        doppler_motion_bins, position = _number(view, position, "I", "Doppler motion bins")
        capture_cpi_samples, position = _number(view, position, "Q", "capture CPI samples")
        capture_sample_step, position = _number(view, position, "I", "capture sample step")
        statistic, position = _string(view, position, "input statistic")
        covariance, position = _string(view, position, "covariance")
        if version != 1 or mature_cpis not in (4, 8) or delay_motion_bins > 2 or \
                doppler_motion_bins > 16 or len(view) > maximum or \
                not capture_cpi_samples or not capture_sample_step or \
                statistic not in ("fused", "post_clutter_bootstrap") or \
                covariance not in ("diagonal", "shrinkage"):
            raise ValueError("temporal artifact header is invalid")
        records = []
        previous_epoch = previous_sequence = None
        epoch_geometry = None
        epoch_available = 0
        while position < len(view):
            start = position
            tag, position = _number(view, position, "I", "record tag")
            size, position = _number(view, position, "Q", "record size")
            if tag != 0x314D5246 or size < 44 or start + size > len(view):
                raise ValueError("temporal artifact record header is invalid")
            epoch, position = _number(view, position, "Q", "epoch")
            sequence, position = _number(view, position, "Q", "sequence")
            capture_first, position = _number(view, position, "Q", "capture first sample")
            capture_end, position = _number(view, position, "Q", "capture end sample")
            available, position = _number(view, position, "I", "available CPIs")
            rows, position = _number(view, position, "I", "rows")
            columns, position = _number(view, position, "I", "columns")
            bank_count, position = _number(view, position, "I", "bank count")
            if not rows or not columns or bank_count != 4 or capture_end < capture_first or \
                    capture_end - capture_first + capture_sample_step != capture_cpi_samples:
                raise ValueError("temporal artifact record geometry is invalid")
            if records and capture_first <= records[-1]["capture_end_sample"]:
                raise ValueError("temporal artifact capture ranges are not chronological")
            if previous_epoch is not None and (epoch < previous_epoch or
                    (epoch == previous_epoch and sequence != previous_sequence + 1)):
                raise ValueError("temporal artifact record sequence is invalid")
            previous_epoch, previous_sequence = epoch, sequence
            expected_available = 1 if not records or epoch != records[-1]["epoch"] else \
                min(epoch_available + 1, mature_cpis)
            if available != expected_available:
                raise ValueError("temporal artifact available-CPI history is invalid")
            epoch_available = available
            cells = rows * columns
            delay_offset = position
            position = _skip(view, position, columns * 4, "delay axis")
            doppler_offset = position
            position = _skip(view, position, rows * 8, "Doppler axis")
            if not _finite_doubles(view, doppler_offset, rows):
                raise ValueError("temporal artifact Doppler axis is not finite")
            geometry = (rows, columns,
                bytes(view[delay_offset:delay_offset + columns * 4]),
                bytes(view[doppler_offset:doppler_offset + rows * 8]))
            if not records or epoch != records[-1]["epoch"]:
                epoch_geometry = geometry
            elif geometry != epoch_geometry:
                raise ValueError("temporal artifact axes changed within an epoch")
            input_power_offset = position
            position = _skip(view, position, cells * 8, "per-CPI input power")
            if not _finite_doubles(view, input_power_offset, cells, nonnegative=True):
                raise ValueError("temporal artifact input power is invalid")
            family_present, position = _number(view, position, "B", "family flag")
            if family_present not in (0, 1):
                raise ValueError("temporal artifact family flag is invalid")
            if (statistic == "post_clutter_bootstrap") != bool(family_present):
                raise ValueError("temporal artifact statistic family contract differs")
            family_offset = position
            if family_present:
                position = _skip(view, position, cells, "statistic family")
                if any(view[family_offset + index] > 2 for index in range(cells)):
                    raise ValueError("temporal artifact statistic family is invalid")
            banks = []
            expected_depths = (2, 2, 4, 8)
            for bank_index in range(bank_count):
                ready, position = _number(view, position, "B", "bank ready")
                depth, position = _number(view, position, "B", "bank depth")
                reserved, position = _number(view, position, "H", "bank reserved")
                expected_ready = available >= depth and depth <= mature_cpis
                if ready not in (0, 1) or depth != expected_depths[bank_index] or \
                        reserved != 0 or bool(ready) != expected_ready:
                    raise ValueError("temporal artifact bank header is invalid")
                sequences_offset = None
                power_offset = delay_motion_offset = doppler_motion_offset = None
                if ready:
                    sequences_offset = position
                    sequence_values = struct.unpack_from("<" + "Q" * depth, view, position)
                    position = _skip(view, position, depth * 8, "bank sequences")
                    if sequence_values != tuple(sequence - age for age in range(depth)):
                        raise ValueError("temporal artifact bank sequence is invalid")
                    power_offset = position
                    position = _skip(view, position, cells * 8, "bank power")
                    if not _finite_doubles(view, power_offset, cells, nonnegative=True):
                        raise ValueError("temporal artifact bank power is invalid")
                    delay_motion_offset = position
                    position = _skip(view, position, cells, "delay motion")
                    doppler_motion_offset = position
                    position = _skip(view, position, cells, "Doppler motion")
                    if any(abs(_signed_byte(view[delay_motion_offset + index])) > delay_motion_bins
                           for index in range(cells)) or any(
                           abs(_signed_byte(view[doppler_motion_offset + index])) > doppler_motion_bins
                           for index in range(cells)):
                        raise ValueError("temporal artifact bank motion is invalid")
                    if bank_index == 0 and any(
                            view[delay_motion_offset + index] != 0 or
                            view[doppler_motion_offset + index] != 0
                            for index in range(cells)):
                        raise ValueError("temporal artifact baseline bank has motion")
                banks.append({"ready": bool(ready), "depth": depth,
                    "sequences_offset": sequences_offset,
                    "linear_power_offset": power_offset,
                    "delay_motion_offset": delay_motion_offset,
                    "doppler_motion_offset": doppler_motion_offset})
            if position != start + size:
                raise ValueError("temporal artifact record size differs")
            records.append({"epoch": epoch, "sequence": sequence,
                "capture_first_sample": capture_first,
                "capture_end_sample": capture_end,
                "available_cpis": available, "rows": rows, "columns": columns,
                "delay_offset": delay_offset, "doppler_offset": doppler_offset,
                "input_linear_power_offset": input_power_offset,
                "statistic_family_present": bool(family_present),
                "statistic_family_offset": family_offset, "banks": banks})
        return {"schema": "VWTBD01", "version": version,
            "maximum_bytes": maximum, "mature_cpis": mature_cpis,
            "delay_motion_bins": delay_motion_bins,
            "doppler_motion_bins": doppler_motion_bins,
            "capture_cpi_samples": capture_cpi_samples,
            "capture_sample_step": capture_sample_step,
            "input_statistic": statistic, "covariance": covariance,
            "records": records, "_stream": stream, "_mmap": data}
    except Exception:
        view.release()
        data.close()
        stream.close()
        raise


def close_artifact(index):
    index["_mmap"].close()
    index["_stream"].close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    arguments = parser.parse_args()
    index = index_artifact(arguments.artifact)
    try:
        print(json.dumps({key: value for key, value in index.items()
            if not key.startswith("_") and key != "records"} |
            {"records": len(index["records"])}, separators=(",", ":")))
    finally:
        close_artifact(index)


if __name__ == "__main__":
    main()
