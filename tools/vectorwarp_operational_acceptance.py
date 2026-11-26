"""Bounded operational baseline/prospective acceptance; not a causal RF study.

Consumed by vectorwarp_release_regression.py's retained-evidence CLI. All raw
snapshots are hash-addressed, with identity measured separately by the existing
probe. ADS-B remains evaluation-only. Unmatched tracks are not false tracks.
"""
import re
from vectorwarp_release_regression import (count, digest, keys, number, require)
from vectorwarp_two_node_soak import REGRESSION_CHECKS, REGRESSION_RECEIPT_SCHEMA

POLICY_SCHEMA = "vectorwarp-operational-release-policy-v1"
INPUT_SCHEMA = "vectorwarp-operational-release-input-v1"
MODE = "frozen-baseline-prospective-operational-v1"
IDENTITIES = ("sourceCommit", "artifactSha256", "configSha256", "packageSha256")


def snapshot(value, identities, expected_epoch=None):
    keys(value, ("schema", "epoch", "nodes", "statistics"), "operational snapshot")
    require(value["schema"] == "vectorwarp-operational-release-snapshot-v1" and
            number(value["epoch"], 1), "invalid snapshot timestamp/schema")
    if expected_epoch is not None:
        require(value["epoch"] == expected_epoch, "snapshot timestamp differs")
    keys(value["nodes"], identities, "snapshot identities")
    for name, identity in identities.items():
        require(value["nodes"][name] == identity, "snapshot source/package/config identity differs")
    stats = value["statistics"]
    require(stats.get("schema") == "vectorwarp-network-statistics-v1" and
            number(stats.get("updated_at"), 1) and
            0 <= value["epoch"] * 1000 - stats["updated_at"] <= 30000,
            "stale or future federation snapshot")
    nodes = stats.get("nodes")
    require(isinstance(nodes, list) and len(nodes) == len(identities) and
            {node.get("id") for node in nodes} == set(identities), "missing or duplicate radar node")
    require(stats.get("evaluation", {}).get("policy_compatible") is True and
            len(stats["evaluation"].get("policy_hashes", [])) == 1 and
            digest(stats["evaluation"]["policy_hashes"][0]), "incompatible evaluation policy")
    require(stats.get("statistics_window", {}).get("alignment_pending") is False,
            "unaligned finalized-pass evidence")
    for node in nodes:
        expected = identities[node["id"]]["configSha256"]
        require(node.get("config_revision") == node.get("loaded_revision") == expected,
                "snapshot configuration differs from identity")
    return stats, {node["id"]: node for node in nodes}


def pass_metrics(stats, start, end):
    system = stats.get("system_wide_unique_passes", {})
    require(system.get("scope") == "all-nodes-all-channels-deduplicated" and
            system.get("window") == "common-retained-finalized-passes" and
            system.get("alignment_pending") is False, "wrong pass population")
    rows = system.get("passes")
    require(isinstance(rows, list) and len(rows) <= 100 and
            type(system.get("passes_complete")) is bool, "missing pass completeness")
    require(all(number(row.get("started_at"), 1) and number(row.get("finalized_at"), row["started_at"])
                and row["finalized_at"] <= end for row in rows), "invalid finalized pass interval")
    require(all(left["started_at"] >= right["started_at"] for left, right in zip(rows, rows[1:])),
            "unordered finalized population")
    # A bounded transport is sufficient only when its oldest retained row
    # precedes this window. Never silently ignore overflow inside the window.
    complete = system["passes_complete"] or bool(rows and rows[-1]["started_at"] < start)
    eligible = [row for row in rows if row["started_at"] >= start and row.get("eligible") is True]
    require(len({(row.get("identity"), row["started_at"]) for row in rows}) == len(rows),
            "duplicate system-wide pass")
    policy_hash = stats["evaluation"]["policy_hashes"][0]
    for row in eligible:
        require(type(row.get("detected")) is bool and type(row.get("tracked")) is bool and
                (not row["tracked"] or row["detected"]), "invalid measured pass outcome")
        require(row.get("provenance") and all(
            item.get("evaluation_policy_hash") == policy_hash and
            item.get("sensitivity_epoch_mixed") is False for item in row["provenance"]),
            "mixed or incompatible pass provenance")
    detected = sum(row["detected"] for row in eligible)
    tracked = sum(row["tracked"] for row in eligible)
    return {"eligible": len(eligible), "detected": detected, "tracked": tracked,
            "detection": detected / len(eligible) if eligible else None,
            "tracking": tracked / detected if detected else None, "complete": complete}


def track_windows(nodes, end):
    result, complete = {}, True
    for name, node in nodes.items():
        channels = node.get("channels")
        require(isinstance(channels, list) and channels, "missing channel population")
        for channel in channels:
            key = (name, channel.get("id"))
            require(isinstance(key[1], str) and key not in result, "duplicate/missing channel identity")
            window = channel.get("operational_track_window", {})
            require(window.get("schema") == "vectorwarp-operational-track-window-v1" and
                    number(window.get("ended_at"), 1) and
                    0 <= end - window["ended_at"] <= 30000 and
                    window.get("started_at") == window["ended_at"] - 600000 and
                    type(window.get("complete")) is bool and
                    count(window.get("retained_tracks")) and count(window.get("unmatched_tracks")) and
                    window["unmatched_tracks"] <= window["retained_tracks"], "invalid unmatched-track window")
            complete &= window["complete"]
            if window["complete"]:
                require(number(window.get("observed_since"), 1, window["started_at"]) and
                        number(window.get("observed_through"), window["ended_at"] - 3000, window["ended_at"]),
                        "track-window coverage does not support completeness")
            result[key] = window
    return result, complete


def evaluate(binding, binding_sha, policy, policy_sha, inputs, archive, now):
    keys(policy, ("schema", "status", "reviewedAtEpoch", "reviewEvidenceSha256", "minimumFreeBytes",
                  "baselineSha256", "baselineFrozenAtEpoch", "baselineIdentities",
                  "minimumEligiblePasses", "yieldTolerance", "maximumBacklogMs", "maximumSampleGapSeconds"), "policy")
    require(policy["schema"] == POLICY_SCHEMA and policy["status"] == "reviewed-before-install",
            "operational policy was not reviewed before installation")
    require(count(policy["minimumFreeBytes"]) and policy["minimumFreeBytes"] > 0 and
            count(policy["minimumEligiblePasses"]) and policy["minimumEligiblePasses"] > 0 and
            number(policy["yieldTolerance"], 0, .05) and number(policy["maximumBacklogMs"], 0, 1000) and
            number(policy["maximumSampleGapSeconds"], 1, 60), "invalid operational limits")
    archive.load(policy["reviewEvidenceSha256"], structured=False)
    baseline = archive.load(policy["baselineSha256"])
    keys(policy["baselineIdentities"], binding["nodes"], "baseline node set")
    candidates = {name: {"sourceCommit": binding["sourceCommit"],
        **{key: node[key] for key in IDENTITIES[1:]}} for name, node in binding["nodes"].items()}
    for name, identity in policy["baselineIdentities"].items():
        keys(identity, IDENTITIES, "baseline identity")
        require(isinstance(identity["sourceCommit"], str) and re.fullmatch(r"[0-9a-f]{40}", identity["sourceCommit"]) and
                all(digest(identity[key]) for key in ("artifactSha256", "configSha256")) and
                (identity["packageSha256"] is None or digest(identity["packageSha256"])) and
                identity["configSha256"] == candidates[name]["configSha256"], "invalid baseline identity/config")
    base_stats, base_nodes = snapshot(baseline, policy["baselineIdentities"])
    require(all(node.get("available") is True and node.get("processor_fresh") is True and
                node.get("radar") == "receiving" and node.get("config_match") is True and
                node.get("stale") is not True for node in base_nodes.values()), "baseline node is unavailable/stale")
    base_end = base_stats["updated_at"]
    base_metrics = pass_metrics(base_stats, base_end - 600000, base_end)
    base_tracks, base_complete = track_windows(base_nodes, base_end)
    require(base_complete and base_metrics["complete"] and
            base_metrics["eligible"] >= policy["minimumEligiblePasses"] and
            base_metrics["detected"] > 0 and base_metrics["tracked"] > 0,
            "baseline lacks complete eligible measured exposure")
    keys(inputs, ("schema", "bindingSha256", "policySha256", "evaluationStartEpoch",
                  "evaluatedThroughEpoch", "snapshots"), "inputs")
    start, through = inputs["evaluationStartEpoch"], inputs["evaluatedThroughEpoch"]
    require(inputs["schema"] == INPUT_SCHEMA and inputs["bindingSha256"] == binding_sha and
            inputs["policySha256"] == policy_sha == binding["regressionPolicySha256"] and
            number(start, 1) and number(through, start) and 0 <= now - through <= 180 and
            number(policy["reviewedAtEpoch"], 1) and number(policy["baselineFrozenAtEpoch"], 1) and
            baseline["epoch"] <= policy["baselineFrozenAtEpoch"] < start and
            policy["reviewedAtEpoch"] <= policy["baselineFrozenAtEpoch"], "unfrozen or mismatched cohort")
    require(isinstance(inputs["snapshots"], list) and 1 <= len(inputs["snapshots"]) <= 1600,
            "missing/bounded operational samples")
    receipt = {"schema": REGRESSION_RECEIPT_SCHEMA, "evaluationMode": MODE,
        "bindingSha256": binding_sha, "policySha256": policy_sha,
        "evaluationStartEpoch": start, "evaluatedThroughEpoch": through,
        "nodes": {}, "regressionDetected": False}
    healthy = {name: True for name in candidates}
    deadlines = dict(healthy)
    previous = None
    for reference in inputs["snapshots"]:
        sample = archive.load(reference)
        stats, nodes = snapshot(sample, candidates)
        epoch = sample["epoch"]
        require(epoch == start if previous is None else
                0 < epoch - previous <= policy["maximumSampleGapSeconds"], "operational sample gap/order")
        require(epoch <= through and stats["evaluation"]["policy_hashes"] == base_stats["evaluation"]["policy_hashes"],
                "future sample or changed evaluation policy")
        for name, node in nodes.items():
            timing = node.get("timing", {})
            require(isinstance(timing.get("drops"), list) and timing["drops"] and
                    all(count(value) for value in timing["drops"]) and
                    all(count(timing.get(key)) for key in ("retired_cpis", "retired_samples")), "missing capture counters")
            healthy[name] &= (node.get("available") is True and node.get("processor_fresh") is True and
                node.get("stale") is not True and node.get("radar") == "receiving" and node.get("config_match") is True and
                node.get("evidence_status") != "degraded" and node.get("tracking_status") != "degraded" and
                not any(timing["drops"]) and timing["retired_cpis"] == timing["retired_samples"] == 0)
            deadlines[name] &= all(number(timing.get(key), 0, limit) for key, limit in (
                ("cpi_ms", 500), ("maximum_completed_cycle_ms", 500),
                ("maximum_completion_interval_ms", binding["nodes"][name]["maximumCadenceMs"]),
                ("backlog_ms", policy["maximumBacklogMs"])))
        previous = epoch
    require(previous == through, "last snapshot differs from evaluated-through time")
    end = stats["updated_at"]
    metrics = pass_metrics(stats, max(start * 1000, end - 600000), end)
    tracks, complete = track_windows(nodes, end)
    require(set(tracks) == set(base_tracks), "candidate channel set changed")
    complete &= all(window["started_at"] >= start * 1000 for window in tracks.values())
    exposed = complete and metrics["complete"] and metrics["eligible"] >= policy["minimumEligiblePasses"]
    tolerance = policy["yieldTolerance"]
    statuses = {
        "detections": "inconclusive" if not exposed else "accepted" if metrics["detected"] > 0 and
            metrics["detection"] + tolerance >= base_metrics["detection"] else "rejected",
        "measurement-backed-track-continuity": "inconclusive" if not exposed else "accepted" if metrics["tracked"] > 0 and
            metrics["tracking"] + tolerance >= base_metrics["tracking"] else "rejected",
        "false-track-controls": "inconclusive" if not complete else "accepted" if all(
            window["unmatched_tracks"] <= base_tracks[key]["unmatched_tracks"] for key, window in tracks.items()) else "rejected"}
    results = {"baseline": base_metrics, "candidate": metrics, "yieldTolerance": tolerance,
        "trackProxy": "unmatched/independent-track nonincrease proxy; no false-alarm-rate claim"}
    for name, node in nodes.items():
        checks = {**statuses, "cpi-deadlines": "accepted" if deadlines[name] else "rejected",
                  "capture-health": "accepted" if healthy[name] else "rejected"}
        receipt["nodes"][name] = {"qualifications": binding["nodes"][name]["qualifications"],
            "retiredCpis": node["timing"]["retired_cpis"], "retiredSamples": node["timing"]["retired_samples"],
            "checks": {key: {"status": status, "accepted": status == "accepted",
                "evidenceSha256": archive.result({"check": key, "status": status, "node": name,
                    "inputs": inputs, "results": results})} for key, status in checks.items()}}
        receipt["regressionDetected"] |= "rejected" in checks.values()
    return receipt, results
