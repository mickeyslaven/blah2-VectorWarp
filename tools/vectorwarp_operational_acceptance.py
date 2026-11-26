"""Bounded operational baseline/prospective acceptance; not a causal RF study.

Consumed by vectorwarp_release_regression.py's retained-evidence CLI. All raw
snapshots are hash-addressed, with identity measured separately by the existing
probe. ADS-B remains evaluation-only. Unmatched tracks are not false tracks.
"""
import re
from vectorwarp_release_regression import (count, digest, keys, number, require)
from vectorwarp_two_node_soak import (REGRESSION_CHECKS, REGRESSION_RECEIPT_SCHEMA,
                                     FUNCTIONAL_MODE, acceptance_mode)

POLICY_SCHEMA = "vectorwarp-operational-release-policy-v1"
INPUT_SCHEMA = "vectorwarp-operational-release-input-v1"
MODE = "frozen-baseline-prospective-operational-v1"
FUNCTIONAL_POLICY_SCHEMA = "vectorwarp-functional-release-policy-v1"
FUNCTIONAL_INPUT_SCHEMA = "vectorwarp-functional-release-input-v1"
IDENTITIES = ("sourceCommit", "artifactSha256", "configSha256", "packageSha256")


def snapshot(value, identities, expected_epoch=None, functional=False):
    fields = ("schema", "epoch", "nodes", "statistics") + (("nativeFrames",) if functional else ())
    keys(value, fields, "operational snapshot")
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
    require(type(stats.get("statistics_window", {}).get("alignment_pending")) is bool,
            "missing finalized-pass alignment status")
    for node in nodes:
        expected = identities[node["id"]]["configSha256"]
        require(node.get("config_revision") == node.get("loaded_revision") == expected,
                "snapshot configuration differs from identity")
    return stats, {node["id"]: node for node in nodes}


def pass_bounds(stats):
    """The API's complete flag describes its requested24h, not our600s."""
    bounds = []
    for node in stats["nodes"]:
        window = node.get("statistics_window", {})
        leaves = window.get("channels", [window])
        require(isinstance(leaves, list) and leaves and type(window.get("complete")) is bool and
                number(window.get("retained_since"), 0), "missing retained pass coverage")
        if "channels" in window:
            require(len(leaves) == len(node["channels"]) and
                    {row.get("channel") for row in leaves} == {row.get("id") for row in node["channels"]},
                    "retained pass coverage omits a channel")
        for row in leaves:
            require(number(row.get("retained_since"), 0) and number(row.get("requested_started_at")) and
                    number(row.get("ended_at"), row["retained_since"]) and
                    0 <= stats["updated_at"] - row["ended_at"] <= 30000 and
                    type(row.get("complete")) is bool and
                    row["complete"] == (row["requested_started_at"] >= row["retained_since"]),
                    "inconsistent retained pass coverage")
        require(window["complete"] == all(row["complete"] for row in leaves) and
                window["retained_since"] == max(row["retained_since"] for row in leaves),
                "aggregate pass coverage differs from channels")
        bounds.extend(leaves)
    return max(row["retained_since"] for row in bounds), min(row["ended_at"] for row in bounds)


def pass_metrics(stats, start, end, retained_since):
    system = stats.get("system_wide_unique_passes", {})
    require(system.get("scope") == "all-nodes-all-channels-deduplicated" and
            system.get("window") == "common-retained-finalized-passes" and
            type(system.get("alignment_pending")) is bool, "wrong pass population")
    rows = system.get("passes")
    require(isinstance(rows, list) and len(rows) <= 100 and
            type(system.get("passes_complete")) is bool, "missing pass completeness")
    require(all(number(row.get("started_at"), 1) and number(row.get("finalized_at"), row["started_at"])
                and row["finalized_at"] <= stats["updated_at"] for row in rows), "invalid finalized pass interval")
    require(all(left["started_at"] >= right["started_at"] for left, right in zip(rows, rows[1:])),
            "unordered finalized population")
    # A bounded transport is sufficient only when its oldest retained row
    # precedes this window. Never silently ignore overflow inside the window.
    complete = system["passes_complete"] or bool(rows and rows[-1]["started_at"] < start)
    common_start = stats["statistics_window"].get("started_at")
    complete &= (end - start >= 600000 and retained_since <= start and number(common_start, 0, start) and
                 system.get("window_started_at") == common_start and
                 not system["alignment_pending"] and not stats["statistics_window"]["alignment_pending"])
    eligible = [row for row in rows if row["started_at"] >= start and
                row["finalized_at"] <= end and row.get("eligible") is True]
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


def functional_authorization(binding, policy, archive, candidates):
    reference = binding["functionalAuthorizationSha256"]
    require(policy["functionalAuthorizationSha256"] == reference, "functional authorization differs")
    authorization = archive.load(reference)
    expected = {"schema": "vectorwarp-functional-release-authorization-v1",
        "evaluationMode": FUNCTIONAL_MODE, "sourceCommit": binding["sourceCommit"],
        "candidateRunId": binding["candidateRunId"], "candidateRunAttempt": binding["candidateRunAttempt"],
        "releaseVersion": binding["releaseVersion"], "nodes": candidates,
        "windowSeconds": 600, "passYieldNonRegression": "not-evaluated"}
    keys(authorization, (*expected, "authorizedAtEpoch", "reason"), "functional authorization")
    require(all(type(authorization[key]) is type(value) and authorization[key] == value for key, value in expected.items()) and
            number(authorization["authorizedAtEpoch"], 1, policy["reviewedAtEpoch"]) and
            isinstance(authorization["reason"], str) and 1 <= len(authorization["reason"].strip()) <= 4096,
            "missing or mismatched one-release operator authorization")


def exact(value):
    require(isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]{0,19}", value) and
            int(value) < 2**64, "invalid native exact identity")
    return int(value)


def functional_observation(sample, state, cutoff):
    """Consume retained native products, never truth/association-derived tracks."""
    keys(sample["nativeFrames"], state, "native node frames")
    for name, frame in sample["nativeFrames"].items():
        prior = state[name]
        require(isinstance(frame, dict) and number(frame.get("timestamp"), 1) and
                0 <= sample["epoch"] * 1000 - frame["timestamp"] <= 3000,
                "missing/stale/future native tracker frame")
        channel = frame.get("illuminator_id")
        node = next(node for node in sample["statistics"]["nodes"] if node["id"] == name)
        require(isinstance(channel, str) and channel in {row.get("id") for row in node["channels"]} and
                channel == prior.get("channel", channel), "native frame channel changed or unavailable")
        prior["channel"] = channel
        lineage, diagnostics = frame.get("input_lineage", {}), frame.get("trackerDiagnostics", {})
        require(isinstance(lineage, dict) and isinstance(diagnostics, dict) and
                lineage.get("available") is True and lineage.get("sampleInterval") == "half-open" and
                diagnostics.get("schema") == "vectorwarp-tracker-frame-diagnostics-v2" and
                diagnostics.get("truth_blind") is True and diagnostics.get("no_op") is False and
                count(diagnostics.get("detections")) and count(diagnostics.get("lineage_resets")) and
                diagnostics["lineage_resets"] == 0,
                "missing native lineage/truth-blind detector diagnostics")
        epoch, first, end = (exact(lineage.get(key)) for key in
            ("captureEpochExact", "firstInputSampleExact", "endInputSampleExact"))
        integration = frame.get("detector_integration", {})
        require(isinstance(integration, dict), "missing native detector integration")
        sequence, history_epoch = integration.get("sequence"), integration.get("history_epoch")
        require(first < end and count(sequence) and 0 < sequence <= 2**53 - 1 and
                count(history_epoch) and 0 < history_epoch <= 2**53 - 1 and
                count(integration.get("capture_epoch")) and integration["capture_epoch"] == epoch and
                integration.get("history_reset") is False,
                "invalid detector frame identity")
        if "last" in prior:
            old_epoch, old_end, old_sequence, old_time, old_history = prior["last"]
            require(epoch == old_epoch and first >= old_end and sequence > old_sequence and
                    history_epoch == old_history and frame["timestamp"] > old_time,
                    "native capture/history reset/repeated/unordered frame")
        prior["last"] = (epoch, end, sequence, frame["timestamp"], history_epoch)
        prior["detectedFrames"] += diagnostics["detections"] > 0 and frame["timestamp"] >= cutoff
        rows = frame.get("data")
        require(isinstance(rows, list) and len(rows) <= 10000, "missing/bounded native tracks")
        seen = set()
        for row in rows:
            require(isinstance(row, dict) and isinstance(row.get("id"), str) and row["id"].strip() and
                    row["id"] not in seen and type(row.get("measured")) is bool and
                    row.get("source") in ("detector", "prediction") and
                    row["measured"] == (row["source"] == "detector"), "invalid native track provenance")
            seen.add(row["id"])
            if not row["measured"]:
                continue
            evidence = row.get("measurement_evidence", {})
            require(isinstance(evidence, dict), "missing native measurement evidence")
            support = evidence.get("support_sequences_exact")
            require(exact(evidence.get("epoch_exact")) == history_epoch and
                    exact(evidence.get("newest_sequence_exact")) == sequence and
                    type(evidence.get("cpis")) is int and evidence["cpis"] in (1, 2, 4, 8) and
                    isinstance(support, list) and len(support) == evidence["cpis"] and
                    [exact(item) for item in support] == list(range(sequence, sequence - len(support), -1)) and
                    all(exact(item) > 0 for item in support) and diagnostics["detections"] > 0,
                    "invalid detector-backed measurement evidence")
            if row.get("state") == "ACTIVE" and frame["timestamp"] >= cutoff:
                history = prior["tracks"].setdefault(row["id"], [])
                history.append(frame["timestamp"])


def evaluate(binding, binding_sha, policy, policy_sha, inputs, archive, now):
    functional = acceptance_mode(binding) == FUNCTIONAL_MODE
    fields = ("schema", "status", "reviewedAtEpoch", "reviewEvidenceSha256", "minimumFreeBytes",
                  "baselineSha256", "baselineFrozenAtEpoch", "baselineIdentities",
                  "maximumBacklogMs", "maximumSampleGapSeconds")
    keys(policy, fields + (("functionalAuthorizationSha256",) if functional else
                          ("minimumEligiblePasses", "yieldTolerance")), "policy")
    require(policy["schema"] == (FUNCTIONAL_POLICY_SCHEMA if functional else POLICY_SCHEMA) and policy["status"] == "reviewed-before-install",
            "operational policy was not reviewed before installation")
    require(count(policy["minimumFreeBytes"]) and policy["minimumFreeBytes"] > 0 and
            (functional or count(policy["minimumEligiblePasses"]) and policy["minimumEligiblePasses"] > 0 and
             number(policy["yieldTolerance"], 0, .05)) and number(policy["maximumBacklogMs"], 0, 1000) and
            number(policy["maximumSampleGapSeconds"], 1, 60), "invalid operational limits")
    archive.load(policy["reviewEvidenceSha256"], structured=False)
    baseline = archive.load(policy["baselineSha256"])
    keys(policy["baselineIdentities"], binding["nodes"], "baseline node set")
    candidates = {name: {"sourceCommit": binding["sourceCommit"],
        **{key: node[key] for key in IDENTITIES[1:]}} for name, node in binding["nodes"].items()}
    if functional:
        require(len(candidates) == 2, "functional acceptance requires both radar nodes")
        functional_authorization(binding, policy, archive, candidates)
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
    base_metrics = None
    if not functional:
        base_since, base_end = pass_bounds(base_stats)
        base_metrics = pass_metrics(base_stats, base_end - 600000, base_end, base_since)
    base_tracks, base_complete = track_windows(base_nodes, base_stats["updated_at"])
    require(base_complete and (functional or base_metrics["complete"] and
            base_metrics["eligible"] >= policy["minimumEligiblePasses"] and
            base_metrics["detected"] > 0 and base_metrics["tracked"] > 0),
            "baseline lacks complete eligible measured exposure")
    keys(inputs, ("schema", "bindingSha256", "policySha256", "evaluationStartEpoch",
                  "evaluatedThroughEpoch", "snapshots"), "inputs")
    start, through = inputs["evaluationStartEpoch"], inputs["evaluatedThroughEpoch"]
    require(inputs["schema"] == (FUNCTIONAL_INPUT_SCHEMA if functional else INPUT_SCHEMA) and inputs["bindingSha256"] == binding_sha and
            inputs["policySha256"] == policy_sha == binding["regressionPolicySha256"] and
            number(start, 1) and number(through, start) and 0 <= now - through <= 180 and
            number(policy["reviewedAtEpoch"], 1) and number(policy["baselineFrozenAtEpoch"], 1) and
            baseline["epoch"] <= policy["baselineFrozenAtEpoch"] < start and
            policy["reviewedAtEpoch"] <= policy["baselineFrozenAtEpoch"], "unfrozen or mismatched cohort")
    require(isinstance(inputs["snapshots"], list) and 1 <= len(inputs["snapshots"]) <= 1600,
            "missing/bounded operational samples")
    receipt = {"schema": REGRESSION_RECEIPT_SCHEMA, "evaluationMode": FUNCTIONAL_MODE if functional else MODE,
        "bindingSha256": binding_sha, "policySha256": policy_sha,
        "evaluationStartEpoch": start, "evaluatedThroughEpoch": through,
        "nodes": {}, "regressionDetected": False}
    healthy = {name: True for name in candidates}
    deadlines = dict(healthy)
    native = {name: {"detectedFrames": 0, "tracks": {}} for name in candidates}
    if functional:
        receipt.update(functionalAuthorizationSha256=binding["functionalAuthorizationSha256"],
                       passYieldNonRegression="not-evaluated", windowSeconds=600)
    previous = None
    for reference in inputs["snapshots"]:
        sample = archive.load(reference)
        stats, nodes = snapshot(sample, candidates, functional=functional)
        epoch = sample["epoch"]
        require(epoch == start if previous is None else
                0 < epoch - previous <= policy["maximumSampleGapSeconds"], "operational sample gap/order")
        require(epoch <= through and stats["evaluation"]["policy_hashes"] == base_stats["evaluation"]["policy_hashes"],
                "future sample or changed evaluation policy")
        if functional:
            functional_observation(sample, native, max(start, through - 600) * 1000)
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
    metrics = None
    if not functional:
        retained_since, end = pass_bounds(stats)
        metrics = pass_metrics(stats, max(start * 1000, end - 600000), end, retained_since)
    tracks, complete = track_windows(nodes, stats["updated_at"])
    require(set(tracks) == set(base_tracks), "candidate channel set changed")
    complete &= all(window["started_at"] >= start * 1000 for window in tracks.values())
    exposed = complete and (through - start >= 600 if functional else
        metrics["complete"] and metrics["eligible"] >= policy["minimumEligiblePasses"])
    tolerance = policy.get("yieldTolerance")
    statuses = {
        "detections": "inconclusive" if functional or not exposed else "accepted" if metrics["detected"] > 0 and
            metrics["detection"] + tolerance >= base_metrics["detection"] else "rejected",
        "measurement-backed-track-continuity": "inconclusive" if functional or not exposed else "accepted" if metrics["tracked"] > 0 and
            metrics["tracking"] + tolerance >= base_metrics["tracking"] else "rejected",
        "false-track-controls": "inconclusive" if not complete else "accepted" if all(
            window["unmatched_tracks"] <= base_tracks[key]["unmatched_tracks"] for key, window in tracks.items()) else "rejected"}
    results = {"baseline": base_metrics, "candidate": metrics, "yieldTolerance": tolerance,
        "trackProxy": "unmatched/independent-track nonincrease proxy; no false-alarm-rate claim"}
    if functional:
        results.update(passYieldNonRegression="not-evaluated", native=native)
    for name, node in nodes.items():
        checks = {**statuses, "cpi-deadlines": "accepted" if deadlines[name] else "rejected",
                  "capture-health": "accepted" if healthy[name] else "rejected"}
        if functional:
            checks["detections"] = "accepted" if exposed and native[name]["detectedFrames"] >= 3 else "inconclusive"
            checks["measurement-backed-track-continuity"] = "accepted" if exposed and any(
                len(times) >= 3 and times[-1] - times[0] >= 1000 for times in native[name]["tracks"].values()) else "inconclusive"
        receipt["nodes"][name] = {"qualifications": binding["nodes"][name]["qualifications"],
            "retiredCpis": node["timing"]["retired_cpis"], "retiredSamples": node["timing"]["retired_samples"],
            "checks": {key: {"status": status, "accepted": status == "accepted",
                "evidenceSha256": archive.result({"check": key, "status": status, "node": name,
                    "inputs": inputs, "results": results})} for key, status in checks.items()}}
        receipt["regressionDetected"] |= "rejected" in checks.values()
    return receipt, results
