"""Native encoder/decoder -> Python truth export seam; synthetic CPU evidence."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).parents[2]
SPEC = importlib.util.spec_from_file_location("worker", ROOT / "script/run-integration-sensitivity-shadow.py")
WORKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WORKER)
NATIVE = WORKER.NATIVE


def rejects(call, text):
    try:
        call()
    except (ValueError, RuntimeError) as error:
        assert text in str(error), str(error)
    else:
        raise AssertionError(f"expected rejection: {text}")


def test_bridge(fixture, verifier):
    with tempfile.TemporaryDirectory(prefix="native-evidence-test-") as temporary:
        root = Path(temporary)
        subprocess.run([str(fixture), str(root)], check=True, capture_output=True)
        paths = [root / f"frame.{i}.rps" for i in (1, 2, 3)]
        checked = subprocess.run([str(verifier), *map(str, paths)], check=True,
                                 capture_output=True, text=True)
        receipts = [{"path": str(p), "sha256": h, "native_contract_verified": True}
                    for p, h in zip(paths, checked.stdout.splitlines())]
        config = {"capture": {"fc": 539000000, "fs": 128000, "device": {"channel_count": 2}},
                  "process": {"data": {"cpi": .0005}, "detection": {"pfa": .012},
                    "surveillance_fusion": {"mode": "coherent_beam_bank"}}}
        metadata = {"epoch": 9, "firstEpochSample": 0, "endEpochSample": 192,
                    "channelGains": [29.7, 29.7]}
        capture = {"processor_cpis": [{"epoch": 9, "first_epoch_sample": 64 * i,
                    "end_epoch_sample": 64 * (i + 1), "timestamp_ms": 1000 + i * .5}
                    for i in range(3)],
                   "truth_samples": [{"timestamp_ms": 1000 + i * .5, "delay_km": 0,
                                      "doppler_hz": 0} for i in range(3)]}
        request = NATIVE.snapshot_request(capture, metadata, config, "33" * 32, "44" * 32, root / "frame")
        for receipt in receipts:
            assert hashlib.sha256(Path(receipt["path"]).read_bytes()).hexdigest() == receipt["sha256"]
        export = WORKER.truth_selected_export(receipts, config, capture, metadata, request,
                     "cfar-rejected", "0.0005s-production", "ab" * 32, "cd" * 32)
        assert export["schema"] == "vectorwarp-truth-selected-shadow-export-v2"
        frames = export["multi_cpi_history"]
        assert [f["timestamp_ms"] for f in frames] == [1000, 1000.5, 1001]
        assert frames[0]["native_signal_timestamp_ms"] == 900001
        first = frames[0]
        assert first["covariance_whitening"]["covariance_storage_complex"][:2] == [[123.5, 0], [7, -2]]
        assert first["covariance_whitening"]["actual_fusion_weights"][:2] == [[1, 0], [0, -.25]]
        assert first["target_cut"]["physical_channels"] == [[8, -8], [29, -29]]
        assert first["target_cut"]["beam_powers"] == [10 + 100 * b for b in range(12)]
        assert all(pass_[4] == 1 for pass_ in first["target_cut"]["native_cfar_statistics"])
        assert first["target_cut"]["native_power_history"] == [11, None, None, None]
        assert len(first["nuisance_cells"]) == 20
        assert export["covariance_whitening"]["estimated_from_cut"] is False
        rejects(lambda: WORKER.truth_selected_export(receipts[:-1], config, capture, metadata, request,
            "x", "y", "ab" * 32, "cd" * 32), "complete requested native")
        rejects(lambda: NATIVE.Snapshot(receipts[0], {**request, "source_sha256": "55" * 32}, config),
                "identity differs")
        rejects(lambda: NATIVE.Snapshot({**receipts[0], "native_contract_verified": False}, request, config),
                "unverified")
        data = bytearray(paths[0].read_bytes()); data[-1] ^= 1; paths[0].write_bytes(data)
        rejects(lambda: NATIVE.Snapshot(receipts[0], request, config), "changed after")
        rejected = subprocess.run([str(verifier), str(paths[0])], capture_output=True)
        assert rejected.returncode != 0
        missing = copy.deepcopy(capture); del missing["processor_cpis"][0]["first_epoch_sample"]
        rejects(lambda: NATIVE.sample_clock(missing, metadata, 128000), "timestamped admitted")
        shifted = copy.deepcopy(capture); shifted["processor_cpis"][1]["epoch"] += 1
        rejects(lambda: NATIVE.sample_clock(shifted, metadata, 128000), "timestamped admitted")


def test_production_policy():
    config = {"process": {"detection": {"track_before_detect": {"enable": True, "cpis": 8}},
              "tracker": {"track_guided_shadow": {"enable": True, "promote": True}},
              "bearing": {"enable": True, "reference_anchor": "fixed_site"}}}
    original = copy.deepcopy(config)
    WORKER.REPLAY.apply_temporal_bearing_policy(config, preserve_production=True)
    assert config == original
    WORKER.REPLAY.apply_temporal_bearing_policy(config)
    assert config["process"]["detection"]["track_before_detect"]["enable"] is False
    assert config["process"]["bearing"]["enable"] is False


if __name__ == "__main__":
    test_bridge(Path(sys.argv[1]), Path(sys.argv[2]))
    test_production_policy()
    print("PASS native canonical snapshot evidence bridge (synthetic CPU fixture; no hardware qualification)")
