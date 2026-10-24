#!/usr/bin/env python3
"""Offline-only, same-IQ validation of detection steering manifolds.

The evaluator operates on serialized complex post-CAF channel vectors.  It is
deliberately not imported by the processor and the CLI refuses to run unless
``--enable-offline-experiment`` is supplied.  Truth labels identify whole
positive/miss passes for scoring only; they never select a direction, weight,
cell, or nuisance hypothesis.
"""

import argparse
import cmath
import json
import math
from pathlib import Path


EPS = 1e-12


def cdot(left, right):
    return sum(a.conjugate() * b for a, b in zip(left, right))


def norm(vector):
    return math.sqrt(max(cdot(vector, vector).real, 0.0))


def normalize(vector):
    scale = norm(vector)
    if scale <= EPS:
        raise ValueError("zero complex vector")
    return [value / scale for value in vector]


def matvec(matrix, vector):
    return [sum(value * item for value, item in zip(row, vector)) for row in matrix]


def inverse(matrix):
    size = len(matrix)
    work = [list(row) for row in matrix]
    result = [[complex(row == column) for column in range(size)] for row in range(size)]
    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(work[row][column]))
        if abs(work[pivot][column]) <= EPS:
            raise ValueError("singular matrix")
        work[column], work[pivot] = work[pivot], work[column]
        result[column], result[pivot] = result[pivot], result[column]
        divisor = work[column][column]
        work[column] = [value / divisor for value in work[column]]
        result[column] = [value / divisor for value in result[column]]
        for row in range(size):
            if row == column:
                continue
            factor = work[row][column]
            work[row] = [a - factor * b for a, b in zip(work[row], work[column])]
            result[row] = [a - factor * b for a, b in zip(result[row], result[column])]
    return result


def covariance(samples, loading=0.02):
    if not samples:
        raise ValueError("noise samples are required")
    channels = len(samples[0])
    if channels < 2 or any(len(row) != channels for row in samples):
        raise ValueError("inconsistent channel vectors")
    output = [[0j] * channels for _ in range(channels)]
    for sample in samples:
        for row in range(channels):
            for column in range(channels):
                output[row][column] += sample[row] * sample[column].conjugate()
    for row in range(channels):
        for column in range(channels):
            output[row][column] /= len(samples)
    mean_power = sum(output[index][index].real for index in range(channels)) / channels
    for index in range(channels):
        output[index][index] += max(mean_power * loading, EPS)
    return output


def conventional_score(observation, steering):
    candidate = normalize(steering)
    return abs(cdot(candidate, observation)) ** 2


def whitened_score(observation, steering, precision):
    filtered = matvec(precision, steering)
    denominator = max(cdot(steering, filtered).real, EPS)
    return abs(cdot(filtered, observation)) ** 2 / denominator


def lcmv_weights(precision, steering, nulls):
    """Minimum-variance weights with unit target response and exact nulls."""
    constraints = [steering] + list(nulls)
    columns = [matvec(precision, vector) for vector in constraints]
    gram = [[cdot(left, right) for right in columns] for left in constraints]
    solution = matvec(inverse(gram), [1 + 0j] + [0j] * len(nulls))
    return [sum(columns[column][row] * solution[column]
                for column in range(len(columns)))
            for row in range(len(steering))]


def constrained_score(observation, steering, precision, nulls):
    try:
        weights = lcmv_weights(precision, steering, nulls)
    except ValueError:
        return None
    return abs(cdot(weights, observation)) ** 2 / max(cdot(weights, matvec(
        inverse(precision), weights)).real, EPS)


def align_sample(sample, reference):
    phase = cmath.phase(cdot(reference, sample))
    return [value * cmath.exp(-1j * phase) / max(norm(sample), EPS) for value in sample]


def gain_signature(gains_db):
    return tuple(round(float(value), 3) for value in gains_db)


def fit_measured_manifold(rows, min_passes=2):
    """Fit only from positive, whole-pass-disjoint calibration rows."""
    grouped = {}
    for row in rows:
        if row.get("label") != "positive":
            continue
        key = (str(row["direction_id"]), str(row.get("frequency_id", "wideband")),
               gain_signature(row["channel_gains_db"]))
        grouped.setdefault(key, []).append(row)
    manifold, support = {}, {}
    for key, items in grouped.items():
        passes = sorted({str(item["pass_id"]) for item in items})
        if len(passes) < min_passes:
            continue
        reference = normalize(items[0]["post_caf"])
        aligned = [align_sample(item["post_caf"], reference) for item in items]
        mean = normalize([sum(row[index] for row in aligned) for index in range(len(reference))])
        residuals = [math.acos(min(1.0, abs(cdot(mean, normalize(item["post_caf"])))))
                     for item in items]
        manifold[key] = mean
        support[key] = {"passes": passes, "samples": len(items),
                        "angular_uncertainty_deg": math.degrees(max(residuals, default=0.0))}
    return {"vectors": manifold, "support": support, "training_pass_ids": sorted({
        str(row["pass_id"]) for row in rows}), "min_passes": min_passes}


def select_measured(model, direction, frequency, gains):
    key = (str(direction), str(frequency), gain_signature(gains))
    vector = model["vectors"].get(key)
    return vector, model["support"].get(key)


def _bank_max(observation, bank, score):
    candidates = [(score(observation, vector), direction) for direction, vector in bank.items()]
    candidates = [(value, direction) for value, direction in candidates if value is not None]
    if not candidates:
        return {"score": 0.0, "direction_id": None, "ranked_directions": []}
    candidates.sort(reverse=True)
    return {"score": candidates[0][0], "direction_id": candidates[0][1],
            "ranked_directions": [direction for _, direction in candidates]}


def score_post_caf(observation, ideal_bank, measured_model, frequency_id,
                   gains_db, noise_covariance, direct_vector=None,
                   reflection_vector=None):
    """Score every direction on one identical CAF vector for every arm."""
    precision = inverse(noise_covariance)
    coherent = {"coherent_sum": [1 + 0j] * len(observation)}
    measured, mask = {}, {}
    for direction, ideal in ideal_bank.items():
        vector, support = select_measured(measured_model, direction, frequency_id, gains_db)
        # Never interpolate/extrapolate an unobserved sector or gain setting.
        measured[direction] = vector if vector is not None else ideal
        mask[direction] = {"supported": vector is not None, "fallback": "ideal",
                           "support": support}
    methods = {
        "ideal": _bank_max(observation, ideal_bank, conventional_score),
        "coherent": _bank_max(observation, coherent, conventional_score),
        "noise_whitened": _bank_max(observation, ideal_bank,
                                     lambda x, a: whitened_score(x, a, precision)),
        "measured": _bank_max(observation, measured, conventional_score),
    }
    nulls = [] if direct_vector is None else [direct_vector]
    methods["mvdr_lcmv"] = _bank_max(
        observation, measured,
        lambda x, a: constrained_score(x, a, precision, nulls))
    reflection_nulls = list(nulls)
    if reflection_vector is not None:
        reflection_nulls.append(reflection_vector)
    methods["direct_reflection_lcmv"] = _bank_max(
        observation, measured,
        lambda x, a: constrained_score(x, a, precision, reflection_nulls))
    methods["measured"]["support_mask"] = mask
    methods["measured"]["selected_supported"] = mask[
        methods["measured"]["direction_id"]]["supported"]
    return methods


def score_frequency_resolved(bins, frequency_manifold, directions):
    """Experimental pre-CAF arm; coherently combines explicitly resolved bins."""
    scored = []
    for direction in directions:
        coherent = 0j
        used = []
        for frequency, observation in sorted(bins.items()):
            key = (str(direction), str(frequency))
            if key not in frequency_manifold:
                continue
            coherent += cdot(normalize(frequency_manifold[key]), observation)
            used.append(str(frequency))
        if used:
            scored.append((abs(coherent) ** 2 / len(used), str(direction), used))
    if not scored:
        return {"status": "unsupported", "score": 0.0, "direction_id": None,
                "reason": "no fully explicit frequency-resolved support"}
    scored.sort(reverse=True)
    return {"status": "experimental", "score": scored[0][0],
            "direction_id": scored[0][1], "frequency_bins_used": scored[0][2]}


def empirical_record(target, controls, nuisance_trials, family_trials):
    ordered = sorted(controls, reverse=True)
    exceed = sum(value >= target for value in controls)
    raw_p = (1 + exceed) / (len(controls) + 1)
    # Charge the direction/bin bank here; charge the full replay search only
    # after every raw p-value is available (see full_search_familywise).
    corrected = min(1.0, raw_p * max(1, nuisance_trials))
    p99 = ordered[max(0, math.floor(0.01 * len(ordered)))] if ordered else 0.0
    rank = 1 + exceed
    return {"target_raw_score": target, "control_raw_scores": list(controls),
            "raw_rank": rank, "raw_margin_over_control_p99_db": 10 * math.log10(
                max(target, EPS) / max(p99, EPS)), "empirical_survival_p": raw_p,
            "nuisance_trials": nuisance_trials, "family_trials": family_trials,
            "trial_corrected_p": corrected}


def full_search_familywise(results):
    """Attach Bonferroni p-values for all eligible post-CAF replay scores.

    The correction starts from raw empirical per-trial p-values, charges their
    searched bank, and then charges the larger of the declared family and the
    observed set of eligible method/pass hypotheses. The pre-CAF arm remains
    diagnostic-only and is excluded from inference and regression gates.
    """
    trials = []
    declared_family_trials = 1
    for row in results:
        for name, method in row["methods"].items():
            if name == "pre_caf_frequency_resolved":
                method["diagnostic_only"] = True
                continue
            if "empirical_survival_p" not in method:
                continue
            raw_p = method["empirical_survival_p"]
            if not 0.0 <= raw_p <= 1.0:
                raise ValueError("empirical survival p-values must be in [0, 1]")
            declared_family_trials = max(
                declared_family_trials, max(1, int(method.get("family_trials", 1))))
            trials.append(method)
    full_search_trials = max(declared_family_trials, len(trials))
    for method in trials:
        method["full_search_trial_count"] = full_search_trials
        method["full_search_familywise_p"] = min(
            1.0, method["empirical_survival_p"] *
            max(1, method["nuisance_trials"]) * full_search_trials)
    return {"full_search_trial_count": full_search_trials,
            "declared_family_trials": declared_family_trials,
            "eligible_hypotheses": len(trials)}


def evaluate_pass(record, ideal_bank, measured_model, family_trials=6):
    pass_id = str(record["pass_id"])
    if pass_id in measured_model["training_pass_ids"]:
        raise ValueError("scored pass overlaps measured-manifold training passes")
    noise = covariance(record["noise_vectors"])
    kwargs = dict(ideal_bank=ideal_bank, measured_model=measured_model,
                  frequency_id=record.get("frequency_id", "wideband"),
                  gains_db=record["channel_gains_db"], noise_covariance=noise,
                  direct_vector=record.get("direct_vector"),
                  reflection_vector=record.get("reflection_vector"))
    target = score_post_caf(record["target_post_caf"], **kwargs)
    controls = [score_post_caf(vector, **kwargs) for vector in record["control_post_caf"]]
    output = {}
    direction_trials = len(ideal_bank)
    for method in target:
        values = [item[method]["score"] for item in controls]
        output[method] = {**target[method], **empirical_record(
            target[method]["score"], values, direction_trials, family_trials)}
    if "frequency_bins" in record:
        experimental = score_frequency_resolved(
            record["frequency_bins"], record.get("frequency_manifold", {}), ideal_bank)
        frequency_controls = [score_frequency_resolved(
            bins, record.get("frequency_manifold", {}), ideal_bank)
            for bins in record.get("frequency_control_bins", [])]
        if experimental["status"] == "experimental" and frequency_controls and all(
                row["status"] == "experimental" for row in frequency_controls):
            nuisance = len(ideal_bank) * len(experimental["frequency_bins_used"])
            experimental.update(empirical_record(
                experimental["score"], [row["score"] for row in frequency_controls],
                nuisance, family_trials))
            experimental["qualification"] = "diagnostic_only_pre_caf_experimental"
        else:
            experimental["qualification"] = "diagnostic_only_without_matched_controls"
        output["pre_caf_frequency_resolved"] = experimental
    return {"pass_id": pass_id, "label": record["label"],
            "truth_role": "offline_scoring_only", "adsb_inference": False,
            "methods": output}


def median(values):
    ordered = sorted(values)
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2


def regression_gates(results, candidate="measured", baseline="ideal",
                     min_positive_passes=2, min_miss_passes=2, alpha=0.05):
    """Frozen pass-level gate: improve positives without increasing miss margin."""
    pass_ids = [row["pass_id"] for row in results]
    if len(pass_ids) != len(set(pass_ids)):
        raise ValueError("positive/miss scoring pass IDs must be disjoint")
    positives = [row for row in results if row["label"] == "positive"]
    misses = [row for row in results if row["label"] == "miss"]
    if len(positives) < min_positive_passes or len(misses) < min_miss_passes:
        return {"passed": False, "reason": "insufficient pass-disjoint positive/miss cohort",
                "positive_passes": len(positives), "miss_passes": len(misses)}
    if not 0.0 < alpha < 1.0:
        raise ValueError("familywise alpha must be between zero and one")
    familywise = full_search_familywise(results)
    def margins(rows, method):
        return [row["methods"][method]["raw_margin_over_control_p99_db"] for row in rows]
    positive_candidate, positive_baseline = margins(positives, candidate), margins(positives, baseline)
    miss_candidate, miss_baseline = margins(misses, candidate), margins(misses, baseline)
    metrics = {
        "candidate": candidate, "baseline": baseline,
        "positive_passes": len(positives), "miss_passes": len(misses),
        "positive_median_margin_db": median(positive_candidate),
        "baseline_positive_median_margin_db": median(positive_baseline),
        "miss_max_margin_db": max(miss_candidate),
        "baseline_miss_max_margin_db": max(miss_baseline),
        "familywise_alpha": alpha,
        **familywise,
        "positive_full_search_familywise_p": [
            row["methods"][candidate]["full_search_familywise_p"] for row in positives],
    }
    metrics["all_positive_familywise_significant"] = all(
        value <= alpha for value in metrics["positive_full_search_familywise_p"])
    metrics["passed"] = (metrics["positive_median_margin_db"] >=
                         metrics["baseline_positive_median_margin_db"] and
                         metrics["miss_max_margin_db"] <= metrics["baseline_miss_max_margin_db"] and
                         metrics["all_positive_familywise_significant"])
    metrics["reason"] = ("pass-level positive non-regression, miss ceiling, and full-search familywise significance satisfied"
                         if metrics["passed"] else
                         "candidate regressed positive median, increased worst miss margin, or failed full-search familywise significance")
    return metrics


def decode_vector(value):
    if not isinstance(value, list) or not value:
        raise ValueError("complex vector must be a non-empty array")
    if all(isinstance(item, list) and len(item) == 2 and
           all(isinstance(part, (int, float)) for part in item) for item in value):
        return [complex(*item) for item in value]
    if all(isinstance(item, (int, float, complex)) for item in value):
        return [complex(item) for item in value]
    raise ValueError("complex vector entries must be numbers or [real, imaginary]")


def decode_input(raw):
    """Decode only schema-declared vector fields; preserve ordinary numeric arrays."""
    data = dict(raw)
    data["ideal_manifold"] = {str(key): decode_vector(value)
                              for key, value in raw["ideal_manifold"].items()}
    data["training_rows"] = []
    for source in raw["training_rows"]:
        row = dict(source)
        row["post_caf"] = decode_vector(source["post_caf"])
        data["training_rows"].append(row)
    data["scored_passes"] = []
    for source in raw["scored_passes"]:
        row = dict(source)
        row["target_post_caf"] = decode_vector(source["target_post_caf"])
        row["control_post_caf"] = [decode_vector(value)
                                   for value in source["control_post_caf"]]
        row["noise_vectors"] = [decode_vector(value) for value in source["noise_vectors"]]
        for field in ("direct_vector", "reflection_vector"):
            if field in source:
                row[field] = decode_vector(source[field])
        if "frequency_bins" in source:
            row["frequency_bins"] = {str(key): decode_vector(value)
                                     for key, value in source["frequency_bins"].items()}
            row["frequency_control_bins"] = [
                {str(key): decode_vector(value) for key, value in bins.items()}
                for bins in source.get("frequency_control_bins", [])]
            row["frequency_manifold"] = {
                (str(direction), str(frequency)): decode_vector(vector)
                for direction, frequencies in source.get("frequency_manifold", {}).items()
                for frequency, vector in frequencies.items()}
        data["scored_passes"].append(row)
    return data


def encode(value):
    if isinstance(value, complex):
        return [value.real, value.imag]
    if isinstance(value, dict):
        return {str(key): encode(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [encode(item) for item in value]
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--enable-offline-experiment", action="store_true")
    args = parser.parse_args()
    if not args.enable_offline_experiment:
        parser.error("default-off: pass --enable-offline-experiment for offline replay")
    data = decode_input(json.loads(args.input.read_text()))
    model = fit_measured_manifold(data["training_rows"], data.get("min_passes", 2))
    ideal = {str(key): value for key, value in data["ideal_manifold"].items()}
    results = [evaluate_pass(row, ideal, model, data.get("family_trials", 6))
               for row in data["scored_passes"]]
    familywise = full_search_familywise(results)
    output = {"schema": "vectorwarp-detection-manifold-replay-v1",
              "offline_only": True, "default_enabled": False,
              "same_iq_post_caf": True,
              "pre_caf_frequency_resolved_status": "experimental",
              "adsb_inference": False, "training_pass_ids": model["training_pass_ids"],
              "results": results,
              "full_search_familywise": familywise,
              "regression_gates": regression_gates(
                  results, min_positive_passes=data.get("min_positive_passes", 2),
                  min_miss_passes=data.get("min_miss_passes", 2))}
    args.output.write_text(json.dumps(encode(output), indent=2) + "\n")


if __name__ == "__main__":
    main()
