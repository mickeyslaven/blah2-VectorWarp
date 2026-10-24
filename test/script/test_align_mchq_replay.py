import json
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "script" / "align-mchq-replay.py"


def test_replay_removes_per_channel_gain_bias(tmp_path):
    source = tmp_path / "gain.mchq"
    sidecar = tmp_path / "gain.mchq.json"
    output = tmp_path / "gain.blah2iq"
    gains = [0.0, 6.0205999]
    fixed = struct.pack(">8I", 0x4D434851, 2, 4, 2, 0, 0, 0, 0)
    channel_metadata = b"".join(
        struct.pack("<ff", 527_000_000.0, gain) for gain in gains)
    raw = bytes([108, 118, 148, 138, 108, 118, 148, 138,
                  88, 108, 168, 148, 88, 108, 168, 148])
    source.write_bytes(fixed + channel_metadata + raw)
    sidecar.write_text(json.dumps({"channels": 2, "sampleRate": 8,
        "frequency": 527_000_000, "firstEpochSample": 0,
        "channelGains": gains, "epoch": 2}))

    subprocess.run([sys.executable, str(SCRIPT), "--input", str(source),
        "--metadata", str(sidecar), "--output", str(output),
        "--frame-samples", "4"], check=True, capture_output=True, text=True)
    payload = output.read_bytes()
    count, _, _ = struct.unpack_from("<IIQ", payload, 64)
    values = np.frombuffer(payload, dtype="<f4", offset=80).reshape(2, count, 2)
    np.testing.assert_allclose(values[0], values[1], rtol=0, atol=1e-5)


def test_replay_rejects_gain_metadata_mismatch(tmp_path):
    source = tmp_path / "bad.mchq"
    sidecar = tmp_path / "bad.mchq.json"
    output = tmp_path / "bad.blah2iq"
    source.write_bytes(struct.pack(">8I", 0x4D434851, 2, 2, 2, 0, 0, 0, 0) +
        struct.pack("<ffff", 527_000_000.0, 0.0, 527_000_000.0, 6.0) +
        bytes([100, 100, 140, 140] * 2))
    sidecar.write_text(json.dumps({"channels": 2, "sampleRate": 4,
        "frequency": 527_000_000, "firstEpochSample": 0,
        "channelGains": [0.0, 5.0], "epoch": 2}))
    result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source),
        "--metadata", str(sidecar), "--output", str(output),
        "--frame-samples", "2"], capture_output=True, text=True)
    assert result.returncode != 0
    assert "RF metadata differs" in result.stderr
