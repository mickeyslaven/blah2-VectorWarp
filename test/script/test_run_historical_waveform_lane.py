import importlib.util
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "run-historical-waveform-lane.py"
SPEC = importlib.util.spec_from_file_location("historical_lane", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_strix_fetch_reads_sidecar_and_publishes_only_verified_atomic_raw():
    command = MODULE.fetch_verified_command(
        "/root/wwat527a-lane/cache/clip.mchq",
        "http://100.126.186.65:39081/clip.mchq")
    assert "clip.mchq.json" in command
    assert 'json.load(sys.stdin)["dataSha256"]' in command
    assert 'json.load(sys.stdin)["dataBytes"]' in command
    assert 'sha256sum "$part"' in command
    assert 'mv -f "$part" "$raw"' in command
    assert command.index('sha256sum "$part"') < command.index('mv -f "$part" "$raw"')
