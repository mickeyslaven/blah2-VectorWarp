import importlib.util
import math
import hashlib
import hmac
import json
from pathlib import Path
import tempfile


PATH = Path(__file__).parents[2] / "script" / "run-integration-sensitivity-shadow.py"
SPEC = importlib.util.spec_from_file_location("integration_shadow", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_two_channel_plan_has_leave_one_out_arms_and_preserves_production():
    config = {"capture": {"device": {"channel_count": 2}},
              "process": {"surveillance_fusion": {"mode": "coherent_beam_bank"}}}
    plan = MODULE.arm_plan(config, ["production", "physical_channels", "noncoherent"])
    assert ("production", "coherent_beam_bank", None) in plan
    assert ("physical_ch0", "noncoherent", 0) in plan
    assert ("physical_ch1", "noncoherent", 1) in plan
    assert ("noncoherent", "noncoherent", None) in plan


def test_score_uses_frame_event_time_and_separates_target_from_nuisance():
    truth = [{"timestamp_ms": 1000, "delay_km": 10, "doppler_hz": 20},
             {"timestamp_ms": 2000, "delay_km": 12, "doppler_hz": 24}]
    arm = {"detections": [
        {"timestamp": 1250, "delay": [10.5, 40], "doppler": [21, -100], "snr": [4, 20]},
        {"timestamp": 1750, "delay": [40], "doppler": [-100], "snr": [30]}],
        "tracks": [
        {"timestamp": 1250, "data": [{"id": "target", "delay": 10.5 / (MODULE.C_KM_S / 2_400_000),
                                       "doppler": 21},
                                      {"id": "nuisance", "delay": 100, "doppler": -100}]},
        {"timestamp": 1750, "data": []}]}
    score = MODULE.score_arm(arm, truth, 2_400_000, .5)
    assert score["target_detection_cpis"] == 1
    assert score["target_detection_fraction"] == .5
    assert score["evaluated_detection_seconds"] == 1
    assert score["target_detection_longest_run"] == 1
    assert score["target_track_cpis"] == 1
    assert score["target_track_fraction"] == .5
    assert score["target_track_ids"] == ["target"]
    assert score["all_detection_cells"] == 3
    assert score["nuisance_detection_cells"] == 2
    assert score["nuisance_published_track_ids"] == 1


def test_admission_fails_closed_for_incomplete_truth():
    try:
        MODULE.valid_captures({"state": "admitted", "integrity": {"valid": True},
                              "captures": {"x": {"valid": True,
                              "admission_state": "admitted", "processor_cpis": [{}, {}]}}})
    except ValueError as error:
        assert "three-CPI" in str(error)
    else:
        raise AssertionError("incomplete truth was accepted")


def test_shadow_rejects_missing_provisional_provenance():
    for exact in (False, None):
        try:
            MODULE.valid_captures({"state": "admitted", "integrity": {"valid": True},
                "captures": {"x": {"valid": True, "admission_state": "admitted",
                    "processor_cpis": [{"exact_event_time": exact}] * 3}}})
        except ValueError:
            pass
        else:
            raise AssertionError("missing provisional timing provenance was accepted")


def test_job_binding_and_production_baseline_are_required():
    identity = {"recording": "/tmp/x", "recording_sha256": "a" * 64,
                "native_sidecar_sha256": "d" * 64,
                "admission_sha256": "b" * 64, "config_sha256": "c" * 64,
                "receipt_key_id": "e" * 64, "producer_host": "live-node", "revision": 1,
                "config_epoch": "c", "receiver_epoch": "r"}
    job = {"schema": "vectorwarp-integration-shadow-job-v1",
           "job_id": MODULE.canonical_digest(identity), "identity": identity,
           "experiment": {"same_iq": True, "shadow_only": True,
                           "require_production_arm": True,
                           "receipt_key_id": "e" * 64,
                           "physical_channel_reference_policy": "leave-one-out",
                           "fusion_modes": ["production"]}, "captures": []}
    job["job_binding_sha256"] = MODULE.canonical_digest(job)
    MODULE.validate_job(job)
    forged = dict(job); forged["identity"] = dict(identity, receiver_epoch="other")
    try:
        MODULE.validate_job(forged)
    except ValueError as error:
        assert "binding" in str(error)
    else:
        raise AssertionError("forged job was accepted")


def test_run_binds_config_admission_and_native_metadata(provisional=False):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        receipt_key = b"K" * 32
        receipt_key_id = hashlib.sha256(receipt_key).hexdigest()
        recording = root / "clip.mchq"; recording.write_bytes(b"immutable-iq")
        recording_sha = hashlib.sha256(recording.read_bytes()).hexdigest()
        config = {"capture": {"fc": 527000000, "fs": 2400000,
                              "device": {"channel_count": 2}},
                  "process": {"data": {"cpi": .1},
                              "detection": {"pfa": 1e-6},
                              "ambiguity": {"delayMin": 0, "delayMax": 10,
                                            "dopplerMin": -10, "dopplerMax": 10},
                              "surveillance_fusion": {"mode": "noncoherent",
                                                       "beams": 1}}}
        config_path = root / "config.json"; config_path.write_text(json.dumps(config))
        native = {"schema": "vectorwarp-kraken-iq-clip-v1", "frequency": 527000000,
                  "sampleRate": 2400000, "channels": 2, "channelGains": [2.7, 1.4]}
        native_path = Path(f"{recording}.json"); native_path.write_text(json.dumps(native))
        native_sha = hashlib.sha256(native_path.read_bytes()).hexdigest()
        truth = [{"timestamp_ms": 1000, "delay_km": 10, "doppler_hz": 20},
                 {"timestamp_ms": 1100, "delay_km": 10.1, "doppler_hz": 20},
                 {"timestamp_ms": 1200, "delay_km": 10.2, "doppler_hz": 20}]
        cpis = [{"exact_event_time": True}] * 3
        if provisional:
            native.update(epoch=1, firstEpochSample=0, endEpochSample=30)
            native_path.write_text(json.dumps(native))
            native_sha = hashlib.sha256(native_path.read_bytes()).hexdigest()
            for row in truth:
                row.update(delay_rate_km_s=1, doppler_rate_hz_s=7)
            cpis = [{"exact_event_time": False,
                "event_time_source": "kraken_heimdall_host_arrival_estimate",
                "event_time_uncertainty_s": 1,
                "event_time_uncertainty_model": "bounded-configured-nuisance-radius",
                "epoch": 1, "first_epoch_sample": index * 10, "end_epoch_sample": (index + 1) * 10,
                "max_raw_observation_offset_s": 0, "truth_bracket_span_ms": 0}
                for index in range(3)]
        admission = {"schema": "vectorwarp-capture-admission-v1", "state": "admitted",
                     "revision": 7, "config_epoch": "c", "receiver_epoch": "r",
                     "integrity": {"valid": True, "data_sha256": recording_sha,
                                   "sidecar_sha256": native_sha},
                     "captures": {"one": {"valid": True, "admission_state": "admitted",
                                            "processor_cpis": cpis,
                                            "truth_samples": truth}}}
        admission_path = Path(f"{recording}.admission.json")
        admission_path.write_text(json.dumps(admission))
        native_identity = {"frequency_hz": 527000000, "sample_rate_hz": 2400000,
                           "channels": 2, "actual_channel_gains_db": [2.7, 1.4]}
        identity = {"recording": str(recording), "recording_sha256": recording_sha,
                    "native_sidecar_sha256": native_sha,
                    "admission_sha256": MODULE.canonical_digest(admission),
                    "config_sha256": MODULE.canonical_digest(config),
                    "receipt_key_id": receipt_key_id, "producer_host": "live-node", "revision": 7,
                    "config_epoch": "c", "receiver_epoch": "r",
                    "native_identity": native_identity,
                    "native_identity_sha256": MODULE.canonical_digest(native_identity)}
        job = {"schema": "vectorwarp-integration-shadow-job-v1",
               "timing_policy": "provisional" if provisional else "exact", "decision_grade": not provisional,
               "job_id": MODULE.canonical_digest(identity), "identity": identity,
               "recording": str(recording), "admission_sidecar": str(admission_path),
               "experiment": {"same_iq": True, "shadow_only": True,
                              "require_production_arm": True,
                              "receipt_key_id": receipt_key_id,
                              "physical_channel_reference_policy": "leave-one-out",
                              "fusion_modes": ["production"], "dwell_seconds": [.1],
                              "max_wall_seconds_per_clip": 30}, "captures": []}
        job["job_binding_sha256"] = MODULE.canonical_digest(job)
        original_harness, original_run = MODULE.REPLAY.load_harness, MODULE.REPLAY.run_arm
        MODULE.REPLAY.load_harness = lambda _: object()
        MODULE.REPLAY.run_arm = lambda *args, **kwargs: {"detections": [
            {"timestamp": 1000, "delay": [], "doppler": [], "snr": []}], "tracks": []}
        try:
            result = MODULE.run(job, config_path, root / "processor", root / "harness",
                                acceleration="cpu", receipt_key=receipt_key)
        finally:
            MODULE.REPLAY.load_harness, MODULE.REPLAY.run_arm = original_harness, original_run
        assert result["summary"]["selection_performed"] is False
        assert result["timing_policy"] == ("provisional" if provisional else "exact")
        assert result["decision_grade"] is not provisional
        assert result["production_promotion_allowed"] is False
        assert result["input"]["applied_channel_gains_db"] == [2.7, 1.4]
        assert result["input"]["native_identity_sha256"] == identity["native_identity_sha256"]
        assert result["result_binding_sha256"] == MODULE.canonical_digest(
            {key: value for key, value in result.items()
             if key not in ("result_binding_sha256", "result_authentication")})
        authenticated = {key: value for key, value in result.items()
                         if key != "result_authentication"}
        assert result["result_authentication"]["tag"] == hmac.new(
            receipt_key, MODULE.canonical(authenticated).encode(), hashlib.sha256).hexdigest()


def test_provisional_score_expands_by_measured_motion_only():
    truth = [{"timestamp_ms": at, "delay_km": 10, "doppler_hz": 20,
              "delay_rate_km_s": .1, "doppler_rate_hz_s": 7} for at in (1000, 1500, 2000)]
    arm = {"detections": [{"timestamp": 1500, "delay": [10], "doppler": [32], "snr": [5]}],
           "tracks": []}
    assert MODULE.score_arm(arm, truth, 2400000, .5)["target_detection_cpis"] == 0
    assert MODULE.score_arm(arm, truth, 2400000, .5, 1)["target_detection_cpis"] == 1


def test_integration_diagnostic_reports_motion_and_phase_budget():
    config = {"capture": {"fc": 527e6, "device": {"array_geometry": {
        "x_axis_bearing_deg_true": 0, "elements": [
            {"daq_channel": 0, "position": [0, 0]},
            {"daq_channel": 1, "position": [.1, 0]}]} }},
        "location": {"rx": {"latitude": 0, "longitude": 0},
                     "tx": {"latitude": 0, "longitude": 1}},
        "process": {"data": {"cpi": .1}, "surveillance_fusion": {
            "beams": 1, "direct_path_null_db": 0}}}
    cutouts = []
    truth = []
    for index, column in enumerate((1, 2, 3)):
        phase = index * .7
        channels = []
        for _ in range(2):
            rows = []
            for row in range(3):
                values = []
                for candidate in range(5):
                    amplitude = 10 if row == 2 and candidate == column else 1
                    value = amplitude * complex(math.cos(phase), math.sin(phase))
                    values.append([value.real, value.imag])
                rows.append(values)
            channels.append(rows)
        timestamp = 1000 + index * 100
        cutouts.append({"timestamp": timestamp, "doppler_hz": [-10, 0, 10],
                        "delay_km": [0, 1, 2, 3, 4], "channels": channels,
                        "reference_weights": [[1, 0], [1, 0]],
                        "direct_path_reference": [math.cos(phase), math.sin(phase)]})
        truth.append({"timestamp_ms": timestamp, "delay_km": column,
                      "doppler_hz": 10})
    result = MODULE.integration_diagnostic(cutouts, config, truth)
    assert result["frames"] == 3
    assert result["phase_source"] == "direct_path_reference"
    assert result["phase_increment_coherence"] > .999
    assert result["coherent_phasor_concentration"] > .999
    assert result["trajectory_over_fixed_db"] > 4


if __name__ == "__main__":
    test_two_channel_plan_has_leave_one_out_arms_and_preserves_production()
    test_score_uses_frame_event_time_and_separates_target_from_nuisance()
    test_admission_fails_closed_for_incomplete_truth()
    test_shadow_rejects_missing_provisional_provenance()
    test_job_binding_and_production_baseline_are_required()
    test_run_binds_config_admission_and_native_metadata()
    test_run_binds_config_admission_and_native_metadata(provisional=True)
    test_provisional_score_expands_by_measured_motion_only()
    test_integration_diagnostic_reports_motion_and_phase_budget()
    print("PASS integration sensitivity shadow regression tests")
