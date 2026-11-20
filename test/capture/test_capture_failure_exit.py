#!/usr/bin/env python3
"""Staged-bundle live/replay exit contract; synthetic module, no radio access."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("replay_fixture", ROOT / "test/recording/processor_replay_test.py")
REPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPLAY)


def main():
    binary, failing_module = (Path(value).resolve() for value in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="vectorwarp-fatal-live-") as temporary:
        root = Path(temporary)
        # Preserve the complete staged native cohort and replace only the
        # receiver in this private fixture tree with a matching SDK-free double.
        shutil.copytree(binary.parent, root / "bin", symlinks=True)
        shutil.copy2(failing_module, root / "bin/blah2-receiver-usrp.so")
        status, sinks = REPLAY.StatusServer(), REPLAY.SinkGroup()
        process = None
        try:
            status.start(); sinks.start()
            payload = REPLAY.config("Usrp", 2, root / "unused.blah2iq", status.port, sinks.ports())
            payload["capture"].update(fs=6000000, fc=527000000)
            payload["capture"]["replay"]["state"] = False
            payload["process"]["data"]["cpi"] = .002
            config = root / "config.json"
            config.write_text(json.dumps(payload))
            environment = dict(os.environ)
            environment.pop("LD_LIBRARY_PATH", None)
            started = time.monotonic()
            process = subprocess.Popen([str(root / "bin" / binary.name), "-c", str(config)],
                                       cwd=root, env=environment, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            output, _ = process.communicate(timeout=15)
            elapsed = time.monotonic() - started
            assert process.returncode == 1, (process.returncode, output[-16000:])
            assert "fixture live receive failure" in output, output[-16000:]
            assert elapsed < 10, ("Fatal live input did not terminate promptly", elapsed)
            print(f"PASS staged live receiver exception exits1 in {elapsed:.3f}s")
        finally:
            if process is not None and process.poll() is None:
                process.kill(); process.wait(timeout=3)
            sinks.close(); status.close()
    # A valid but sub-CPI recording passes metadata startup and fails inside
    # ReplayPlayer. A missing file is rejected by startup preflight instead,
    # before an inspectable replay session exists.
    REPLAY.run_case(binary, "Usrp", 2, "runtime-short", cpi=.002,
                    recording_frames=1, recording_frame_samples=1)
    print("PASS staged replay failure remains inspectable until controlled shutdown")


if __name__ == "__main__":
    main()
