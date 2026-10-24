import gzip
import importlib.util
import json
from pathlib import Path


SCRIPT = Path(__file__).parents[2] / "script" / "reconcile-historical-adsb.py"
spec = importlib.util.spec_from_file_location("reconcile_historical_adsb", SCRIPT)
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)


def test_newest_event_time_selection_and_fallback(tmp_path):
    manifests, traces, output = tmp_path / "manifests", tmp_path / "traces", tmp_path / "out"
    manifests.mkdir(); (traces / "traces" / "01").mkdir(parents=True)
    source = {"schema": "v1", "rows": [{"hex": "abc001", "replay_trajectory": [
        {"frame": 0, "timestamp_ms": 10_000, "latitude": 0, "longitude": 0},
        {"frame": 1, "timestamp_ms": 50_000, "latitude": 9, "longitude": 9}]}]}
    src = manifests / "20261021-527a-miss-manifest.json"; src.write_text(json.dumps(source))
    stats = {"eligibility_events": [{"id": "e1", "hex": "abc001", "truth_samples": [{
        "timestamp_ms": 9_500, "position_timestamp_s": 9.5, "latitude": 40.0, "longitude": -80.0,
        "altitude_geom_ft": 1000, "doppler_hz": 12}]}]}
    stat = tmp_path / "stats.json"; stat.write_text(json.dumps(stats))
    trace = {"icao": "abc001", "timestamp": 0, "trace": [[9.8, 40.01, -80.01, 1100, 100, 0, 0, 0, None, "adsb_icao", 1200]]}
    with gzip.open(traces / "traces" / "01" / "trace_full_abc001.json", "wt") as f: json.dump(trace, f)
    mod.main(["--manifests", str(manifests), "--statistics", str(stat), "--traces", str(traces), "--output", str(output), "--max-age-s", "30"])
    candidate = json.loads(next(output.glob("*.adsb-reconciled-v2.json")).read_text())
    frames = candidate["rows"][0]["replay_trajectory"]
    assert frames[0]["adsb_reconciliation"]["selected_source"] == "adsb_lol_daily_archive"
    assert frames[0]["adsb_reconciliation"]["timing_error_ms"] == 200
    assert len(frames[0]["adsb_reconciliation"]["candidates"]) == 2
    assert "disagreement" in frames[0]["adsb_reconciliation"]
    assert frames[1]["adsb_reconciliation"]["selected_source"] == "legacy_manifest_v1_fallback"
    assert json.loads(src.read_text()) == source


if __name__ == "__main__":
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        test_newest_event_time_selection_and_fallback(Path(directory))
