#!/usr/bin/env python3
"""Deterministic, receiver-independent IQ scenes for physical qualification.

Only the corpus runner writes these files. Importing this module allocates no
large fixture and starts no process. Scene truth is used by the oracle only.
"""
import cmath
import hashlib
import json
import math
from pathlib import Path
import struct

C = 299792458.0
CPI = .02
LANE_RATE = 100000
CENTER = 10000000
EPOCH_FRAMES = 32
TARGET_FRAMES = 20
CONFIRMATION = "offline-physical-gpu-qualification-v1"


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def fingerprint(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def geometry(channels):
    radius = C / CENTER / 4
    return [[radius * math.cos(-2 * math.pi * channel / channels),
             radius * math.sin(-2 * math.pi * channel / channels), 0.0]
            for channel in range(channels)]


def frequencies(lanes):
    return [CENTER + int((lane - (lanes - 1) / 2) * LANE_RATE) for lane in range(lanes)]


def trajectory(lane, frame, motion, frequency):
    assert motion in ("stationary", "integer", "fractional-delay")
    doppler = (200 + 50 * lane) * (-1 if lane % 2 else 1)
    # "integer" describes the integrator's coordinate grid, not permission to
    # decouple physical delay rate from Doppler. +1 bin/CPI at these settings
    # would require -5 kHz, outside the declared +/-800 Hz ambiguity window.
    # Six-bin lane spacing also keeps target acceptance gates disjoint.
    delay = 10 + 6 * lane - LANE_RATE * CPI * doppler * frame / frequency
    return {"delayBins": delay, "dopplerHz": doppler,
            "bearingLocalDeg": 60.0 + 30.0 * (lane % 2)}


def random_complex(count, seed):
    result = []
    for _ in range(count):
        seed = (1664525 * seed + 1013904223) & 0xffffffff
        re = (seed >> 8) / 8388608.0 - 1
        seed = (1664525 * seed + 1013904223) & 0xffffffff
        im = (seed >> 8) / 8388608.0 - 1
        result.append(complex(re, im))
    return result


def interpolate(values, position):
    index = math.floor(position)
    if index < 0 or index + 1 >= len(values):
        return 0j
    fraction = position - index
    return values[index] * (1 - fraction) + values[index + 1] * fraction


def scene_recipe(variant, target=True):
    """Modes/faults/policies share identical IQ for paired comparisons."""
    return {key: variant[key] for key in ("lanes", "channels", "motion")} | {
        "target": target, "epochFrames": EPOCH_FRAMES, "targetFrames": TARGET_FRAMES,
        "laneRate": LANE_RATE, "cpi": CPI, "frequency": CENTER}


def write_scene(path, recipe):
    """Stream completed BLAH2IQ blocks; bounded memory independent of modes."""
    lanes, channels = recipe["lanes"], recipe["channels"]
    lane_rate, cpi = recipe["laneRate"], recipe["cpi"]
    decimation = 1 if lanes == 1 else 2 * lanes
    rate = lane_rate * decimation
    count = int(lane_rate * cpi)
    frames = recipe["epochFrames"]
    samples = count * frames * decimation
    signals = [random_complex(count * frames + 2, 0x6D2B79F5 + lane * 197)
               for lane in range(lanes)]
    noise = [random_complex(count * frames + 2, 0x1B873593 + channel * 419)
             for channel in range(channels)]
    lane_frequencies = frequencies(lanes)
    positions = geometry(channels) if channels > 2 else []
    sites = {"schema": "vectorwarp-recorded-geographic-sites-v1",
        "provenance": "operator-configured-at-acquisition",
        "rx": {"latitude": 0, "longitude": 0, "altitude": 0},
        "tx": {"latitude": 0, "longitude": .001, "altitude": 0},
        "illuminators": {f"corpus-{lane}": {"frequencyHz": frequency,
            "transmitter": {"latitude": 0, "longitude": .001, "altitude": 0}}
            for lane, frequency in enumerate(lane_frequencies)}}
    # Synthetic RF carries independently pinned declared sites and exact sample
    # lineage, not a fabricated hardware gain-readback/acquisition receipt.
    acquisition = {"schema": "vectorwarp-recording-acquisition-v1",
        "receiver": "Kraken" if channels > 2 else "Usrp", "channels": channels,
        "sampleRateHz": rate, "frequencyHz": CENTER, "epoch": "7",
        "firstSample": "1000", "endSample": str(1000 + samples),
        "sourcePositionAvailable": True, "frameSequenceSource": "recorder-ingress",
        "metadataAvailable": False, "fixtureProvenance": "synthetic-generator-not-hardware",
        "frames": [{"sequence": str(frame + 1), "sourceFirst": str(1000 + frame * count * decimation),
            "sourceCount": str(count * decimation), "sliceOffset": "0", "count": str(count * decimation)}
            for frame in range(frames)],
        "geographicSites": sites, "geographicSitesSha256": fingerprint(sites)}
    encoded = canonical(acquisition)
    header = bytearray(128)
    header[:8] = b"BLAH2IQ\0"
    struct.pack_into("<IIIIII", header, 8, 2, 128, channels, rate, CENTER, 1)
    struct.pack_into("<I", header, 32, len(encoded))
    header[64:128] = hashlib.sha256(encoded).hexdigest().encode()
    struct.pack_into("<QQ", header, 40, samples, 1)
    truth = []
    with path.open("wb") as output:
        output.write(header)
        output.write(encoded)
        for frame in range(frames):
            observed = [trajectory(lane, frame, recipe["motion"], frequency)
                        for lane, frequency in enumerate(lane_frequencies)]
            truth.append(observed)
            amplitude = .8
            active = recipe["target"] and frame < recipe["targetFrames"]
            block = [bytearray() for _ in range(channels)]
            for sample in range(count * decimation):
                at = frame * count + sample / decimation
                time = at / lane_rate
                # Unequal channel noise exercises non-unity whitening weights.
                values = [.01 * (channel + 1) * interpolate(stream, at)
                          for channel, stream in enumerate(noise)]
                for lane, frequency in enumerate(lane_frequencies):
                    direct = interpolate(signals[lane], at)
                    # A continuous fractional delay, not a staircase at each
                    # CPI edge, preserves the RF Doppler/range-rate relation.
                    expected = observed[lane]
                    delay = expected["delayBins"] - expected["dopplerHz"] * sample / (decimation * frequency)
                    echo = amplitude * interpolate(signals[lane], at - delay) * \
                        cmath.exp(2j * math.pi * expected["dopplerHz"] * time) if active else 0j
                    mixer = cmath.exp(2j * math.pi * (frequency - CENTER) * time)
                    if channels == 2:
                        values[0] += mixer * direct
                        values[1] += mixer * (.25 * direct + echo)
                    else:
                        angle = math.radians(expected["bearingLocalDeg"])
                        wave = -2 * math.pi * frequency / C
                        for channel, (x, y, _) in enumerate(positions):
                            direct_response = cmath.exp(1j * wave * x)
                            target_response = cmath.exp(1j * wave *
                                (x * math.cos(angle) + y * math.sin(angle)))
                            values[channel] += mixer * (direct * direct_response + echo * target_response)
                for channel, value in zip(block, values):
                    channel.extend(struct.pack("<ff", value.real, value.imag))
            output.write(struct.pack("<IIQ", count * decimation, 0, frame * count * decimation))
            for channel in block:
                output.write(channel)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    lineage = {"schema": "vectorwarp-portable-iq-clip-v1", "format": "blah2iq",
        "encoding": "float32-complex-channel-major-blocks", "channels": channels,
        "sampleRate": rate, "frequency": CENTER, "samplesPerChannel": samples,
        "epoch": 7, "firstEpochSample": 1000, "endEpochSample": 1000 + samples,
        "dataBytes": path.stat().st_size, "dataSha256": digest,
        "perChannelGainDb": [20.0] * channels}
    Path(str(path) + ".json").write_bytes(canonical(lineage) + b"\n")
    return {"recipe": recipe, "sampleRate": rate, "samplesPerFrame": count * decimation,
            "laneSamplesPerFrame": count, "frequencies": lane_frequencies,
            "truth": truth, "dataSha256": digest, "lineage": lineage,
            "acquisition": acquisition, "acquisitionSha256": hashlib.sha256(encoded).hexdigest()}


def manifold_identity(channels, frequency, rate):
    identity = {"frequency_hz": frequency, "sample_rate_hz": rate,
        "channel_mapping": list(range(channels)), "requested_gain_db": [20.0] * channels,
        "actual_gain_db": [20.0] * channels, "positions_m": geometry(channels),
        "receiver_location": [0.0, 0.0, 0.0], "transmitter_location": [0.0, .001, 0.0],
        "x_axis_bearing_deg_true": 90.0, "direct_path_local_azimuth_deg": 0.0,
        "direct_path_beam": 0}
    encoded = bytearray(b"vectorwarp-manifold-identity-v3")
    for value in (frequency, rate): encoded.extend(struct.pack("<d", value))
    encoded.extend(struct.pack("<I", channels))
    for channel in range(channels): encoded.extend(struct.pack("<I", channel))
    for group in (identity["requested_gain_db"], identity["actual_gain_db"],
                  [v for position in identity["positions_m"] for v in position],
                  identity["receiver_location"], identity["transmitter_location"], [90.0]):
        for value in group: encoded.extend(struct.pack("<d", value))
    value = 1469598103934665603
    for byte in encoded:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    identity["compatibility_fingerprint"] = f"fnv1a64:{value:016x}"
    return identity


def write_manifold(path, channels, frequency, rate, beams=48, bad_order=False):
    identity = manifold_identity(channels, frequency, rate)
    responses = []
    for beam in range(beams):
        angle = 2 * math.pi * beam / beams
        row = []
        for x, y, _ in geometry(channels):
            value = cmath.exp(-2j * math.pi * frequency / C *
                             (x * math.cos(angle) + y * math.sin(angle)))
            row.append([value.real, value.imag])
        responses.append(row)
    if bad_order:
        identity["channel_mapping"][0:2] = [1, 0]
    value = {"schema": "vectorwarp-bearing-manifold-v2", "frequency_hz": frequency,
             "identity": identity, "responses": responses}
    path.write_bytes(canonical(value) + b"\n")
    return value


def configure(replay, variant, mode, recording, scene, root, api_port, ports, *, refusal=None):
    config = replay.config("Usrp", variant["channels"], recording, api_port, ports, loop=True)
    channels, lanes = variant["channels"], variant["lanes"]
    capture = config["capture"]
    capture.update(fs=scene["sampleRate"], fc=CENTER)
    capture["replay"].update(format="blah2", channel_count=channels,
        recording_sha256=scene["dataSha256"],
        reference_mode="array_eigenbeam" if channels > 2 else "dedicated",
        reference_channels=list(range(channels)) if channels > 2 else [0],
        surveillance_channels=list(range(channels)) if channels > 2 else [1])
    capture["replay"]["qualification"] = {"enabled": True, "confirmation": CONFIRMATION,
        "observer": {"directory": str(root / "observer"), "max_frames": 64,
                     "max_samples_per_channel": 4096, "max_map_cells": 8192, "max_bytes": 268435456}}
    process = config["process"]
    process["performance"].update(acceleration=mode, illuminator_lane_workers=1)
    process["data"]["cpi"] = CPI
    process["clutter"].update(enable=True, delayMin=-1, delayMax=3)
    # 2000 samples / 33 Doppler rows leaves 60 samples per correlation
    # block. The signed lag window must remain strictly inside that block.
    process["ambiguity"].update(delayMin=-4, delayMax=59, dopplerMin=-800, dopplerMax=800)
    # Feasible disjoint M=2 initiation at the deepest enabled bank. These are
    # synthetic qualification fixtures, not a migration of operator settings.
    depth = variant["window"]
    process["tracker"].update(gpu_capacity=256, delete=max(3, depth - 1))
    process["tracker"]["initiate"].update(M=2, N=max(3, 1 + depth), maxAcc=10000,
        stationary_clutter={"enable": False}, acceleration_model="hypothesis_bank")
    process["detection"].update(pfa=.001, integration_cpis=1, integration_motion_bins=0)
    if variant["window"] > 2:
        process["detection"]["track_before_detect"] = {"enable": True,
            "max_cpis": variant["window"], "delay_motion_bins": 1, "doppler_motion_bins": 1,
            "fractional_delay": variant["motion"] == "fractional-delay"}
    if channels > 2:
        config["location"] = {"rx": {"latitude": 0, "longitude": 0, "altitude": 0},
                              "tx": {"latitude": 0, "longitude": .001, "altitude": 0}}
        capture["device"]["heimdall"] = {"gain_control": "fixed", "channel_gains": [20.0] * channels}
        capture["device"]["array_geometry"] = {"shape": "regular_polygon", "mapping_confirmed": True,
            "geometry_confirmed": True, "orientation_confirmed": True, "x_axis_bearing_deg_true": 90,
            "elements": [{"daq_channel": channel, "position": position}
                         for channel, position in enumerate(geometry(channels))]}
        process["reference_synthesis"] = {"mode": "array_eigenbeam", "channels": list(range(channels)),
                                           "analysis_samples": 2000, "analysis_interval": 1}
        process["surveillance_fusion"] = {"mode": "noise_whitened_beam_bank"
            if variant["fusion"] == "noise-whitened" else "coherent_beam_bank", "beams": 12}
        process["bearing"] = {"enable": mode == "gpu-resident", "method": "coherent_beam_bank",
            "reference_anchor": "fixed_site", "scan_beams": 48, "min_consistent": 2}
        if mode == "gpu-resident":
            process["tracker"]["beam_tolerance"] = 1
    if lanes > 1:
        definitions = []
        for lane, frequency in enumerate(scene["frequencies"]):
            definition = {"enabled": True, "id": f"corpus-{lane}", "standard": "CUSTOM",
                "frequency_hz": frequency, "occupied_bandwidth_hz": 60000,
                "output_sample_rate_hz": LANE_RATE, "filter_taps_per_phase": 16,
                "input_role_mode": "manual", "reference_channel": 0,
                "surveillance_channels": list(range(channels)) if channels > 2 else [1]}
            if channels > 2:
                # Explicit paired lane roles select a dedicated reference.
                # Coherent lanes inherit the canonical recorded array topology.
                for field in ("input_role_mode", "reference_channel", "surveillance_channels"):
                    definition.pop(field)
                definition.update(transmitter_location={"latitude": 0, "longitude": .001, "altitude": 0})
                manifold = root / f"manifold-{lane}.json"
                write_manifold(manifold, channels, frequency, LANE_RATE, bad_order=refusal == "bad-manifold")
                definition["bearing_manifold_file"] = str(manifold if refusal != "missing-calibration"
                                                          else root / "missing-manifold.json")
            definitions.append(definition)
        process["multi_illuminator"] = {"enabled": True,
                                        "illuminators": definitions}
    elif channels > 2:
        manifold = root / "manifold.json"
        write_manifold(manifold, channels, CENTER, scene["sampleRate"], bad_order=refusal == "bad-manifold")
        process["bearing"]["manifold_file"] = str(manifold if refusal != "missing-calibration"
                                                  else root / "missing-manifold.json")
    return config
