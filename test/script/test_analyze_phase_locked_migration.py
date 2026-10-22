import cmath
import importlib.util
import math
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "analyze-phase-locked-migration.py"
SPEC = importlib.util.spec_from_file_location("phase_locked", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def config():
    return {"capture": {"fc": 563e6, "device": {"array_geometry": {
        "x_axis_bearing_deg_true": 0, "elements": [
            {"daq_channel": 0, "position": [0, 0]}, {"daq_channel": 1, "position": [1, 0]}]}}},
        "location": {"rx": {"latitude": 0, "longitude": 0},
                     "tx": {"latitude": 0, "longitude": 1}},
        "process": {"data": {"cpi": .5}, "surveillance_fusion": {"beams": 1,
                    "direct_path_null_db": 0}}}


def frame(phase, target_column):
    channels = []
    for _channel in range(2):
        data = []
        for row in range(5):
            values = []
            for column in range(7):
                value = 30 if (row == 3 and column == target_column) else 1
                if row == 3 and column == target_column:
                    value *= cmath.exp(1j * phase)
                values.append([value.real if isinstance(value, complex) else value,
                               value.imag if isinstance(value, complex) else 0])
            data.append(values)
        channels.append(data)
    return {"doppler_hz": [-40, -20, 0, 20, 40], "delay_km": list(range(7)),
            "channels": channels, "reference_weights": [[1, 0], [1, 0]],
            "direct_path_reference": [math.cos(phase), math.sin(phase)]}


def job():
    return {"job_id": "j", "event_id": "e", "recording": "r", "family_trials": 1,
            "control_quantile": .99,
            "trajectory": [{"delay_km": 2, "doppler_hz": 20},
                           {"delay_km": 3, "doppler_hz": 20},
                           {"delay_km": 4, "doppler_hz": 20}]}


def test_phase_locked_migration_recovers_moving_complex_target_and_emits_vector():
    cutouts = [frame(phase, column) for phase, column in zip((0, .9, 1.8), (2, 3, 4))]
    result = MODULE.analyze(cutouts, config(), job(), phase_source="direct",
                            phase_gates=(.9,), guard=0, min_doppler=1)
    method = result["methods"]["phase_locked_migration"]
    assert result["phase_lock"]["source"] == "direct_path_reference"
    assert method["phase_coherence"] > .999
    assert method["raw_target_score"] > method["control_p99"]
    assert result["score_vectors"]["phase_locked_migration"]["target"]["evidence_log10"] == method["evidence_log10"]
    # Both phase orientations are charged, even though only one is coherent.
    assert method["parameter_trial_count"] == 2


def test_phase_gate_rejects_incoherent_pilot():
    # Successive increments are 0, pi/2, pi and -pi/2, whose unit phasors
    # cancel instead of describing one stable phase progression.
    pilots = [complex(1, 0), complex(1, 0), complex(0, 1),
              complex(0, -1), complex(-1, 0)]
    assert MODULE.phase_increment_coherence(pilots) < .01


def test_reference_weight_proxy_is_explicit_when_no_direct_pilot_exists():
    cutouts = [frame(0, 2), frame(0, 3), frame(0, 4)]
    for item in cutouts:
        del item["direct_path_reference"]
    pilots, source = MODULE.pilot_series(cutouts)
    assert source == "reference_weight_proxy"
    assert pilots == [1 + 0j] * 3
