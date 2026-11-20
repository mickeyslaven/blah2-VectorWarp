"""Bounded authenticated clutter replay evidence; no radar/filter implementation."""
import copy
import hashlib
import math
from pathlib import Path
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(value):
    return isinstance(value, str) and len(value) == 64 and set(value) <= set("0123456789abcdef")


def descriptor(config, source):
    replay = config["capture"]["replay"]
    qualification = replay.get("qualification", {})
    plan = qualification.get("clutter_checkpoint", {})
    require(replay.get("state") is True and replay.get("loop") is False and
            replay.get("format") in ("blah2", "mchq") and qualification.get("enabled") is True and
            qualification.get("confirmation") == "offline-physical-gpu-qualification-v1" and
            "fault" not in qualification and config["process"]["performance"]["acceleration"] == "gpu" and
            not config["process"].get("multi_illuminator", {}).get("enabled") and
            config["process"]["clutter"]["enable"] is True,
            "Clutter checkpoint requires fault-free single-lane portable native replay")
    require(plan.get("source_sha") == source and len(source) == 40 and
            plan.get("mode") in ("export", "fixed", "refit") and type(plan.get("injected")) is bool and
            (plan["mode"] != "export" or not plan["injected"]), "Invalid checkpoint operation/source")
    path = Path(plan["path"])
    require(path.is_absolute() and not path.is_symlink(), "Checkpoint path must be absolute and unlinked")
    require((plan["mode"] == "export" and not plan.get("sha256")) or
            (plan["mode"] != "export" and digest(plan.get("sha256"))), "Missing checkpoint SHA-256 pin")
    samples = int(config["capture"]["fs"] * config["process"]["data"]["cpi"])
    fields = ("warm_frame", "epoch", "first_sample", "warm_first_sample", "warm_end_sample",
              "injection_frame", "injection_first_sample")
    require(all(type(plan.get(key)) is int and 0 <= plan[key] < 2**64 for key in fields) and
            1 <= samples <= 4096 and 1 <= plan["warm_frame"] < 64 and
            plan["first_sample"] + 64*samples < 2**64 and
            plan["warm_first_sample"] == plan["first_sample"] + (plan["warm_frame"]-1)*samples and
            plan["warm_end_sample"] == plan["warm_first_sample"] + samples and
            plan["injection_frame"] == plan["warm_frame"] + 1 and
            plan["injection_first_sample"] == plan["warm_end_sample"], "Invalid exact warm/injection boundary")
    echo = replay.get("inject_echo", {})
    require(echo.get("state") is True and type(echo.get("doppler_hz")) in (int, float) and
            echo["doppler_hz"] != 0, "Checkpoint requires signed echo with muted warm-prefix history")
    return plan, samples


def retained_checkpoint(plan):
    path = Path(plan["path"])
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= 1 << 20,
            "Missing/bounded checkpoint file")
    body = path.read_bytes()
    sha = hashlib.sha256(body).hexdigest()
    require(plan["mode"] == "export" or sha == plan["sha256"], "Checkpoint SHA-256 mismatch")
    return {"sha256": sha, "bytesHex": body.hex()}


def checkpoint(run, plan, samples):
    retained = run.get("clutterCheckpoint", {})
    require(digest(retained.get("sha256")) and isinstance(retained.get("bytesHex"), str) and
            len(retained["bytesHex"]) <= 2 << 20, "Missing retained checkpoint bytes")
    body = bytes.fromhex(retained["bytesHex"])
    require(len(body) >= 194 and body[:10] == b"VWCLUTTER1" and
            hashlib.sha256(body).hexdigest() == retained["sha256"] == plan["sha256"],
            "Retained checkpoint digest/schema differs")
    identity, prefix = body[10:74].decode(), body[74:138].decode()
    warm, epoch, first, begin, end, count, channels, taps, delay = struct.unpack_from("<5Q3Ii", body, 138)
    require(digest(identity) and digest(prefix) and (warm, epoch, first, begin, end, count) ==
            (plan["warm_frame"], plan["epoch"], plan["first_sample"], plan["warm_first_sample"],
             plan["warm_end_sample"], samples) and 1 <= channels <= 8 and 1 <= taps <= samples and
            len(body) == 194 + channels*taps*24, "Checkpoint lineage/topology differs")
    require(all(math.isfinite(value) for value in struct.unpack_from("<" + "d"*(channels*taps*2), body, 194)) and
            all(math.isfinite(value) for value in struct.unpack_from("<" + "f"*(channels*taps*2), body, 194+channels*taps*16)),
            "Checkpoint contains nonfinite coefficients")
    return {"sha256": retained["sha256"], "identity": identity, "prefix": prefix,
            "channels": channels, "taps": taps, "delay": delay}


def semantic(config):
    value = copy.deepcopy(config)
    replay = value["capture"]["replay"]
    for key in ("qualification", "artifacts", "file"):
        replay.pop(key, None)
    value.get("network", {}).pop("ports", None)
    value.get("save", {}).pop("path", None)
    return value


def proof(text):
    require(isinstance(text, str) and len(text) < 512, "Missing actual worker checkpoint proof")
    fields = text.split()
    require(len(fields) == 10, "Malformed checkpoint proof")
    result = tuple(int(value) for value in fields[:5]) + tuple(fields[5:7]) + tuple(int(x) for x in fields[7:])
    require(result[4] in (0, 1) and all(value >= 0 for value in (*result[:4], *result[7:])) and
            (not result[4] or result[7:] == (0, 0, 0)), "Frozen checkpoint performed fitting work")
    return result


def validate_pair(control, injected, pair, source):
    left, samples = descriptor(control["config"], source)
    right, other_samples = descriptor(injected["config"], source)
    require(left["mode"] == right["mode"] in ("fixed", "refit") and not left["injected"] and right["injected"] and
            samples == other_samples and semantic(control["config"]) == semantic(injected["config"]) and
            control.get("calibrations", {}) == injected.get("calibrations", {}) and
            control["device"] == injected["device"] and control["deviceClass"] in ("fp32", "fp64") and
            all(left[key] == right[key] for key in left if key not in ("path", "injected")) and
            left["sha256"] == pair.get("checkpointSha256") and pair["firstSample"] >= left["injection_first_sample"],
            "Paired checkpoint/config/device/injection boundary differs")
    retained = checkpoint(control, left, samples)
    require(retained == checkpoint(injected, right, samples), "Paired immutable coefficients differ")
    warm_rows = []
    for run, declared in ((control, left), (injected, right)):
        rows = [row for row in run["observer"] if row["stage"] == "clutter"]
        require(rows and len(rows) <= 64, "Missing bounded clutter prefix")
        events = [event for event in run["events"] if event.get("boundary") == "clutter-checkpoint"]
        require(len(events) == len(rows), "Missing/duplicate worker checkpoint receipts")
        worker = set()
        for index, (row, event) in enumerate(zip(rows, events), 1):
            first = declared["first_sample"] + (index-1)*samples
            fixed = declared["mode"] == "fixed" and index > declared["warm_frame"]
            actual = proof(row.get("checkpoint_proof"))
            require(actual == proof(event.get("clutterCheckpoint")) and actual[:5] ==
                    (index, declared["epoch"], first, first+samples, int(fixed)) and
                    (row["captureEpoch"], row["captureFirst"], row["captureEnd"]) == actual[1:4] and
                    row["filter_policy"] == ("fixed-weights" if fixed else "refit-weights") and
                    row.get("clutter_backend") == "vulkan" and row.get("gpu_executed") is True and
                    row.get("cpu_executed") is False and event.get("route") == "portable" and event.get("lane") == 0 and
                    tuple(event.get(key) for key in ("frame", "captureEpoch", "captureFirstEpochSample", "captureEndEpochSample")) == actual[:4] and
                    event.get("workerPid") in run["artifactReceipt"]["workers"], "Checkpoint execution/lineage/backend drift")
            worker.add(event["workerPid"])
            require(len(row["channel_ids"]) == retained["channels"] and
                    row["delay_min"] == retained["delay"] and
                    row["delay_max_exclusive"]-row["delay_min"] == retained["taps"], "Checkpoint channel/delay geometry drift")
            require(len(row.get("reference_iq", [])) == samples and all(
                    len(channel) == samples for key in ("unfiltered_surveillance", "filtered_surveillance") for channel in row[key]) and
                    (fixed or (actual[7:9] == (1, 1) and 2 <= actual[9] <= 16 and actual[9] % 2 == 0)),
                    "Checkpoint input shape or actual fitting work differs")
            if index >= declared["warm_frame"]:
                require(actual[5:7] == (retained["sha256"], retained["prefix"]), "Missing/stale warm checkpoint proof")
            else:
                require(actual[5:7] == ("-", "-"), "Checkpoint was frozen before the declared CPI")
        require(len(worker) == 1 and len(rows) > declared["warm_frame"], "Checkpoint worker restarted or injection never began")
        prefix = [row for row in run["observer"] if row["captureEnd"] <= declared["warm_end_sample"]]
        require({row["stage"] for row in prefix} == {"input", "clutter", "products", "detector"},
                "Missing full warm input/frontend/reference/product prefix")
        warm_rows.append(prefix)
    require(warm_rows[0] == warm_rows[1], "Paired raw/frontend/reference warm histories differ")
    return {"checkpointSha256": retained["sha256"], "warmPrefixSha256": retained["prefix"],
            "warmFrame": left["warm_frame"], "injectionFirstSample": left["injection_first_sample"],
            "qualificationScope": "bounded-clutter-replay-v1", "maximumSamplesPerChannel": 4096,
            "maximumCpis": 64, "arithmetic": "host-fp64-q0-plus-post-cast-fp32-q1",
            "interpretation": "end-to-end-frozen-clutter-control"}
