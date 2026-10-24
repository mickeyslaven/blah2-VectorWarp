import cmath
import importlib.util
import math
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "evaluate-detection-manifold-replay.py"
SPEC = importlib.util.spec_from_file_location("manifold_replay", PATH)
REPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPLAY)


IDEAL = {"north": [1, 1j, -1, -1j], "east": [1, -1, 1, -1]}
GAINS = [20, 20, 20, 20]


def phase_distorted(vector):
    errors = [cmath.exp(1j * value) for value in (0.0, .45, -.35, .7)]
    return [item * error for item, error in zip(vector, errors)]


def model(samples=2):
    target = phase_distorted(IDEAL["north"])
    rows = [{"pass_id": f"train-{index}", "label": "positive",
             "direction_id": "north", "frequency_id": "wideband",
             "channel_gains_db": GAINS,
             "post_caf": [value * cmath.exp(1j * index * .2) for value in target]}
            for index in range(samples)]
    return REPLAY.fit_measured_manifold(rows, min_passes=2)


def white_noise():
    return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]] * 3


def test_measured_manifold_gain_on_same_post_caf_iq():
    observation = phase_distorted(IDEAL["north"])
    scored = REPLAY.score_post_caf(observation, IDEAL, model(), "wideband", GAINS,
                                   REPLAY.covariance(white_noise()))
    assert scored["measured"]["score"] > scored["ideal"]["score"] * 1.10
    assert scored["measured"]["direction_id"] == "north"


def test_direct_and_reflection_constraints_preserve_target_and_null_nuisance():
    target = [1, 1j, -1, -1j]
    direct = [1, 1, 1, 1]
    reflection = [1, -1j, -1, 1j]
    precision = REPLAY.inverse(REPLAY.covariance(white_noise()))
    weights = REPLAY.lcmv_weights(precision, target, [direct, reflection])
    assert abs(REPLAY.cdot(weights, target) - 1) < 1e-8
    assert abs(REPLAY.cdot(weights, direct)) < 1e-8
    assert abs(REPLAY.cdot(weights, reflection)) < 1e-8


def test_unsupported_sector_falls_back_to_ideal_and_exposes_support_mask():
    scored = REPLAY.score_post_caf(IDEAL["east"], IDEAL, model(), "wideband", GAINS,
                                   REPLAY.covariance(white_noise()))
    mask = scored["measured"]["support_mask"]
    assert mask["east"] == {"supported": False, "fallback": "ideal", "support": None}
    assert scored["measured"]["direction_id"] == "east"
    assert scored["measured"]["selected_supported"] is False


def test_frequency_resolved_arm_detects_group_delay_and_flat_model_loses_coherence():
    frequencies = (-1.0, 0.0, 1.0)
    delay = .33
    bins, matched, flat = {}, {}, {}
    for frequency in frequencies:
        vector = [cmath.exp(1j * 2 * math.pi * frequency * delay * channel)
                  for channel in range(4)]
        bins[str(frequency)] = vector
        matched[("north", str(frequency))] = vector
        flat[("north", str(frequency))] = [1] * 4
    good = REPLAY.score_frequency_resolved(bins, matched, ["north"])
    bad = REPLAY.score_frequency_resolved(bins, flat, ["north"])
    assert good["status"] == "experimental"
    assert good["score"] > bad["score"] * 3


def test_frequency_arm_charges_direction_and_bin_nuisance_trials_with_controls():
    frequency_manifold = {("north", "a"): IDEAL["north"],
                          ("north", "b"): IDEAL["north"]}
    record = {"pass_id": "frequency-held-out", "label": "positive",
              "channel_gains_db": GAINS,
              "target_post_caf": phase_distorted(IDEAL["north"]),
              "control_post_caf": [[1, 0, 0, 0], [0, 1, 0, 0]],
              "noise_vectors": white_noise(),
              "frequency_bins": {"a": IDEAL["north"], "b": IDEAL["north"]},
              "frequency_control_bins": [{"a": [1, 0, 0, 0], "b": [0, 1, 0, 0]}],
              "frequency_manifold": frequency_manifold}
    result = REPLAY.evaluate_pass(record, IDEAL, model(), family_trials=3)["methods"][
        "pre_caf_frequency_resolved"]
    assert result["raw_rank"] == 1
    assert result["nuisance_trials"] == 4
    assert result["family_trials"] == 3
    assert result["qualification"] == "diagnostic_only_pre_caf_experimental"
    assert "full_search_familywise_p" not in result


def test_full_search_familywise_uses_raw_p_values_and_excludes_diagnostic_arm():
    def method(raw_p, nuisance=1, family=1):
        return {"empirical_survival_p": raw_p, "nuisance_trials": nuisance,
                "family_trials": family, "raw_rank": 1,
                "raw_margin_over_control_p99_db": 3.0}
    results = [
        {"pass_id": "p1", "label": "positive", "methods": {
            "measured": method(.01), "ideal": method(.01),
            "pre_caf_frequency_resolved": method(.99)}},
        {"pass_id": "m1", "label": "miss", "methods": {
            "measured": method(.01), "ideal": method(.01)}}]
    summary = REPLAY.full_search_familywise(results)
    assert summary == {"full_search_trial_count": 4,
                       "declared_family_trials": 1, "eligible_hypotheses": 4}
    assert math.isclose(results[0]["methods"]["measured"]["full_search_familywise_p"], .04)
    assert results[0]["methods"]["pre_caf_frequency_resolved"]["diagnostic_only"] is True
    assert "full_search_familywise_p" not in results[0]["methods"]["pre_caf_frequency_resolved"]


def test_per_channel_gain_signature_never_reuses_incompatible_measurement():
    changed = [20, 20, 24, 20]
    scored = REPLAY.score_post_caf(phase_distorted(IDEAL["north"]), IDEAL, model(),
                                   "wideband", changed,
                                   REPLAY.covariance(white_noise()))
    assert not any(row["supported"] for row in scored["measured"]["support_mask"].values())


def test_pass_disjoint_gate_and_positive_miss_regression_accounting():
    measured = model()
    record = {"pass_id": "held-out", "label": "positive",
              "frequency_id": "wideband", "channel_gains_db": GAINS,
              "target_post_caf": phase_distorted(IDEAL["north"]),
              "control_post_caf": [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]],
              "noise_vectors": white_noise(), "direct_vector": [1, 1, 1, 1],
              "reflection_vector": [1, -1j, -1, 1j]}
    output = REPLAY.evaluate_pass(record, IDEAL, measured, family_trials=2)
    result = output["methods"]["measured"]
    assert output["adsb_inference"] is False
    assert result["raw_rank"] == 1
    assert result["raw_margin_over_control_p99_db"] > 0
    assert result["nuisance_trials"] == 2
    assert result["trial_corrected_p"] >= result["empirical_survival_p"]
    record["pass_id"] = "train-0"
    try:
        REPLAY.evaluate_pass(record, IDEAL, measured)
    except ValueError as error:
        assert "overlaps" in str(error)
    else:
        raise AssertionError("training/scoring pass overlap was accepted")


def test_miss_uses_same_scoring_path_and_raw_margin_can_be_negative():
    record = {"pass_id": "held-out-miss", "label": "miss",
              "channel_gains_db": GAINS, "target_post_caf": [1, 0, 0, 0],
              "control_post_caf": [phase_distorted(IDEAL["north"]), IDEAL["east"]],
              "noise_vectors": white_noise()}
    output = REPLAY.evaluate_pass(record, IDEAL, model(), family_trials=1)
    assert output["label"] == "miss"
    assert output["methods"]["measured"]["raw_margin_over_control_p99_db"] < 0


def test_regression_gate_requires_disjoint_positive_and_miss_passes():
    def row(pass_id, label, measured, ideal, raw_p=.001):
        def method(margin):
            return {"raw_margin_over_control_p99_db": margin,
                    "empirical_survival_p": raw_p, "nuisance_trials": 1,
                    "family_trials": 1}
        return {"pass_id": pass_id, "label": label,
                "methods": {"measured": method(measured), "ideal": method(ideal)}}
    results = [row("p1", "positive", 4, 2), row("p2", "positive", 3, 2),
               row("m1", "miss", -2, -1), row("m2", "miss", -3, -1)]
    assert REPLAY.regression_gates(results)["passed"] is True
    results[-1]["pass_id"] = "p1"
    try:
        REPLAY.regression_gates(results)
    except ValueError as error:
        assert "disjoint" in str(error)
    else:
        raise AssertionError("overlapping positive/miss IDs were accepted")


def test_regression_gate_rejects_the_old_tautological_corrected_p_acceptance():
    def row(pass_id, label, measured, ideal):
        def method(margin):
            # The old gate accepted this because min(1, .6) <= 1 is always true.
            return {"raw_margin_over_control_p99_db": margin,
                    "empirical_survival_p": .6, "nuisance_trials": 1,
                    "family_trials": 1}
        return {"pass_id": pass_id, "label": label,
                "methods": {"measured": method(measured), "ideal": method(ideal)}}
    results = [row("p1", "positive", 4, 2), row("p2", "positive", 3, 2),
               row("m1", "miss", -2, -1), row("m2", "miss", -3, -1)]
    gate = REPLAY.regression_gates(results)
    assert gate["all_positive_familywise_significant"] is False
    assert gate["passed"] is False


def test_json_decoder_preserves_two_channel_gain_array():
    raw = {"ideal_manifold": {"n": [[1, 0], [0, 1]]},
           "training_rows": [{"pass_id": "a", "label": "positive",
                              "direction_id": "n", "channel_gains_db": [19, 21],
                              "post_caf": [[1, 0], [0, 1]]}],
           "scored_passes": [{"pass_id": "b", "label": "miss",
                              "channel_gains_db": [19, 21],
                              "target_post_caf": [[1, 0], [0, 1]],
                              "control_post_caf": [[[0, 1], [1, 0]]],
                              "noise_vectors": [[[1, 0], [0, 0]],
                                                [[0, 0], [1, 0]]]}]}
    decoded = REPLAY.decode_input(raw)
    assert decoded["training_rows"][0]["channel_gains_db"] == [19, 21]
    assert decoded["ideal_manifold"]["n"] == [1 + 0j, 1j]
