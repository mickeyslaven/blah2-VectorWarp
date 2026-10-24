"""Deterministic, receiver-neutral OPRS conformance scenario generator."""

from __future__ import annotations

from typing import Any


LEVELS = [-60, -54, -48, -42, -36]


def conformance_suite(profile: str, seed: int = 20261022) -> dict[str, Any]:
    source = "ref" if profile in {"REF_SURV_2CH", "REF_PLUS_ARRAY"} else "surv0"
    targets = ["surv"] if profile == "REF_SURV_2CH" else ["surv0", "surv1"]
    scenarios = []
    for background in ("pure-deterministic", "evaluation-background-a", "evaluation-background-b"):
        scenarios.append(_scenario(f"{background}-blank", profile, source, targets, background, seed, [], blank=True))
        for delay, doppler in ((5.0, -2.0), (15.0, -80.0), (45.0, 120.0)):
            for level in LEVELS:
                name = f"{background}-d{delay:g}-f{doppler:g}-l{level}"
                trajectory = [{"time_s": 0.0, "delay_km": delay, "doppler_hz": doppler,
                               "level_db_ref_rms": level, "azimuth_deg": 0.0},
                              {"time_s": 1.0, "delay_km": delay + 1.5, "doppler_hz": doppler + 8,
                               "level_db_ref_rms": level, "azimuth_deg": 330.0 if len(targets) > 1 else 0.0}]
                scenarios.append(_scenario(name, profile, source, targets, background, seed, trajectory))
    crossing = [{"time_s": 0.0, "delay_km": 12, "doppler_hz": -90, "level_db_ref_rms": -42, "azimuth_deg": 30},
                {"time_s": 1.0, "delay_km": 22, "doppler_hz": 90, "level_db_ref_rms": -42, "azimuth_deg": 210}]
    scenarios.append(_scenario("crossing-target-primary", profile, source, targets, "evaluation-background-b", seed, crossing))
    multipath = _scenario("controlled-multipath", profile, source, targets, "evaluation-background-b", seed, crossing)
    multipath["multipath"] = [{"extra_delay_km": 2.0, "relative_level_db": -8.0, "phase_deg": 75.0}]
    scenarios.append(multipath)
    return {"schema": "oprs-conformance-suite-v1", "suite_version": "0.2.0", "profile": profile,
            "seed": seed, "partition_policy": {"training": ["training-*"], "selection": ["selection-*"],
                "evaluation": ["pure-deterministic", "evaluation-background-a", "evaluation-background-b"],
                "disjoint_recording_sessions_required": True}, "scenarios": scenarios}


def _scenario(identifier: str, profile: str, source: str, targets: list[str], background: str,
              seed: int, trajectory: list[dict[str, Any]], blank: bool = False) -> dict[str, Any]:
    return {"schema": "oprs-benchmark-scenario-v1", "scenario_id": identifier,
            "evidence_layer": "pure_synthetic" if background == "pure-deterministic" else "background_injection",
            "partition": "evaluation", "capability_profile": profile, "replay_only": True,
            "background_id": background, "seed": seed, "blank_control": blank,
            "source_channel_id": source, "inject_into_channel_ids": targets, "trajectory": trajectory,
            "tags": ["blank"] if blank else ["time_varying", "both_signs_grid", "amplitude_staircase"]}
