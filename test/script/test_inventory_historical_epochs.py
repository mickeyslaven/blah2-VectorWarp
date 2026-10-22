import importlib.util
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "inventory-historical-epochs.py"
SPEC = importlib.util.spec_from_file_location("historical", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_inventory_uses_sidecar_frequency_and_interpolated_cpi_truth():
    event = {"id": "event", "config_epoch": "epoch", "receiver_epoch": "rx",
             "state": "finalized", "outcome": "no_radar_return", "hex": "abc",
             "predicted": {"delay_km": 1, "doppler_hz": 2},
             "truth_samples": [{"timestamp_ms": 1000, "position_timestamp_s": 1,
                                "delay_km": 1, "doppler_hz": 2},
                               {"timestamp_ms": 2000, "position_timestamp_s": 2,
                                "delay_km": 3, "doppler_hz": 4}],
             "capture_windows": [{"state": "acknowledged", "file": "/r.mchq",
                                  "window_start": 1000, "window_end": 2000, "revision": 1}]}
    sidecar = {"path": "/r.mchq", "eventIds": ["event"], "frequency": 527000000,
               "sampleRate": 2400000, "raw_exists": True, "channelGains": [2.7]}
    result = MODULE.inventory({"eligibility_events": [event]}, [sidecar])
    row = result["rows"][0]
    assert row["frequency_hz"] == 527000000
    assert row["replay_trajectory"][0]["timestamp_ms"] == 1250
    assert row["replay_trajectory"][1]["delay_km"] == 2.5
    assert result["epochs"][0]["available"] == 1
