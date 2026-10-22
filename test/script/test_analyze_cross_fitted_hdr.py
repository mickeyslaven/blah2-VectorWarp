#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path

import numpy as np

SCRIPT = Path(__file__).parents[2] / "script" / "analyze-cross-fitted-hdr.py"
SPEC = importlib.util.spec_from_file_location("cross_fitted_hdr", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_cross_fitted_reference_never_uses_held_channel():
    rng = np.random.default_rng(4)
    spectra = rng.normal(size=(5, 8, 16)) + 1j * rng.normal(size=(5, 8, 16))
    gains = np.ones(5)
    noise = np.ones(5)
    before = MODULE.cross_fitted_references(spectra, gains, noise)
    spectra[2] *= 1e6
    after = MODULE.cross_fitted_references(spectra, gains, noise)
    # The reference for held channel 2 is invariant to arbitrary held data.
    np.testing.assert_allclose(before[2][1], after[2][1])


def test_heteroscedastic_calibration_does_not_amplify_low_gain_noise():
    rng = np.random.default_rng(7)
    base = rng.normal(size=8192) + 1j * rng.normal(size=8192)
    iq = np.stack([base, base * 10])
    _, physical, _ = MODULE.channel_calibration(iq, [0.0, 20.0])
    assert .8 < physical[1] / physical[0] < 1.2


def test_consensus_penalizes_one_good_view_and_rewards_consistency(monkeypatch):
    stack = np.array([[[10.0]], [[0.0]], [[0.0]], [[0.0]]])
    assert np.quantile(stack, .25, axis=0).item() == 0.0
    consistent = np.ones((4, 1, 1)) * 2.0
    assert np.quantile(consistent, .25, axis=0).item() == 2.0


def test_cyclostationarity_gate_requires_stable_repeated_peak():
    n = 128 * 4096
    t = np.arange(n) / MODULE.MCHQ.SAMPLE_RATE
    carrier = np.exp(2j * np.pi * 10000 * t)
    modulated = np.stack([(1 + .8 * np.cos(2 * np.pi * 1000 * t)) * carrier
                          for _ in range(5)])
    result = MODULE.cyclostationarity_scan(modulated, z_threshold=5.0)
    assert result["stable_peaks"] is True
    assert abs(result["cyclic_frequency_hz"] - 1000) < 30
    rng = np.random.default_rng(9)
    noise = rng.normal(size=(5, n)) + 1j * rng.normal(size=(5, n))
    assert MODULE.cyclostationarity_scan(noise, z_threshold=20.0)["stable_peaks"] is False


def test_manifest_trajectory_is_preferred_and_all_candidates_are_ranked(tmp_path):
    manifest = {"rows": [
        {"event_id": "x", "truth_source": "event_prediction_fallback",
         "truth_samples_count": 1, "capture": {"file": "/tmp/a.mchq"},
         "replay_trajectory": [{"delay_km": 2, "doppler_hz": 0}]},
        {"event_id": "x", "truth_source": "interpolated_position_samples",
         "truth_samples_count": 8, "capture": {"file": "/tmp/a.mchq"},
         "replay_trajectory": [{"delay_km": 3, "doppler_hz": 0}]}]}
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest))
    trajectory, source = MODULE.load_trajectory(path, event_id="x", recording="a.mchq")
    assert trajectory[0]["delay_km"] == 3
    assert source == "manifest:interpolated_position_samples"

    surface = np.ones((7, 7)); surface[3, 3] = 2; surface[1, 1] = 4
    delay = 3 * MODULE.MCHQ.C_KM_S / MODULE.MCHQ.SAMPLE_RATE
    summary, candidates = MODULE.score([surface], np.arange(-3, 4),
        [{"doppler_hz": 0, "delay_km": delay}], guard=0, controls_stride=1)
    assert len(candidates) == 49
    assert summary["target_rank_of_all_candidates"] == 2
    assert summary["validated_at_plus_3db"] is False
    assert sorted(item["rank"] for item in candidates) == list(range(1, 50))


def test_channel_gains_default_to_recording_sidecar(tmp_path):
    recording = tmp_path / "clip.mchq"
    Path(str(recording) + ".json").write_text(json.dumps(
        {"channelGains": [1, 2, 3, 4, 5]}))
    gains, source = MODULE.load_gains(recording)
    assert gains == [1, 2, 3, 4, 5]
    assert source.endswith("clip.mchq.json")
