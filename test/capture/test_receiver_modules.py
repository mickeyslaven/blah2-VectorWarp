#!/usr/bin/env python3
"""Relocated module faults; no vendor libraries, devices or services are used."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    binary, core, fixtures = map(Path, sys.argv[1:])
    fixtures = fixtures.resolve()
    cases = 0
    with tempfile.TemporaryDirectory(prefix="vectorwarp-receiver-modules-") as temporary:
        root = Path(temporary)
        def run(name, mode="load", module=None, expected=42):
            nonlocal cases
            directory = root / name
            directory.mkdir()
            executable = directory / "testReceiverModule"
            shutil.copy2(binary, executable)
            # Preserve the SONAME, but intentionally omit all vendor SDKs.
            shutil.copy2(core, directory / "libblah2-capture-core.so.1")
            if module == "malformed":
                (directory / "blah2-receiver-usrp.so").write_bytes(b"not an ELF module")
            elif module:
                shutil.copy2(fixtures / (module + ".so"), directory / "blah2-receiver-usrp.so")
            # Loading a selected USRP must not eagerly load the other adapter.
            shutil.copy2(fixtures / "hackrf.so", directory / "blah2-receiver-hackrf.so")
            event_file = directory / "events"
            env = dict(os.environ, BLAH2_RECEIVER_TEST_EVENTS=str(event_file))
            env.pop("LD_LIBRARY_PATH", None)
            result = subprocess.run([str(executable), mode], cwd=directory,
                                    env=env, text=True, capture_output=True, timeout=10)
            assert result.returncode == expected, (name, result.returncode, result.stdout, result.stderr)
            events = event_file.read_text().splitlines() if event_file.exists() else []
            if mode != "status" and module != "hackrf":
                assert not any(event.startswith("HackRF:") for event in events), (name, events)
            cases += 1
            return result.stdout, events

        text, events = run("valid", module="valid", expected=0)
        assert events == ["Usrp:" + step for step in (
            "loaded", "created", "started", "processed", "stopped", "destroyed", "unloaded")], events
        assert "shared IQ interface passed" in text
        text, events = run("missing")
        assert "could not load" in text and "No other receiver was selected" in text and not events
        text, events = run("malformed", module="malformed")
        assert "could not load" in text and not events
        text, events = run("no-entry", module="libreceiver-fixture-runtime")
        assert "no supported factory entry point" in text and not events
        text, events = run("bad-abi", module="badAbi")
        assert "ABI/cohort mismatch" in text and events == ["Usrp:loaded", "Usrp:unloaded"]
        text, events = run("bad-cohort", module="badCohort")
        assert "ABI/cohort mismatch" in text and events == ["Usrp:loaded", "Usrp:unloaded"]
        text, events = run("wrong-receiver", module="hackrf")
        assert "ABI/cohort mismatch" in text  # Refuse even a valid sibling adapter.
        text, events = run("factory-error", module="createError")
        assert "fixture creation refused" in text and events == ["Usrp:loaded", "Usrp:unloaded"]
        text, events = run("runtime-missing", module="missingRuntime")
        assert "receiver-fixture-runtime" in text and "could not load" in text and not events
        text, events = run("unknown", mode="unknown", module="valid")
        assert "Unknown receiver type" in text and not events
        text, events = run("invalid-config", mode="invalid-config", module="valid")
        assert "Invalid two-channel" in text and not events
        text, events = run("status", mode="status", module="valid", expected=0)
        status = json.loads(text)
        assert status["schema"] == 1 and status["hardwareProbed"] is False
        receivers = {item["receiver"]: item for item in status["receivers"]}
        assert set(receivers) == {"Kraken", "Usrp", "HackRF", "RspDuo"}
        assert receivers["Kraken"] == dict(receiver="Kraken", compiled=True, builtIn=True,
                                          moduleLoadable=True, error="")
        assert receivers["Usrp"]["moduleLoadable"] and receivers["HackRF"]["moduleLoadable"]
        assert not receivers["RspDuo"]["moduleLoadable"] and receivers["RspDuo"]["compiled"]
        assert all(len(item["error"]) <= 240 and all(32 <= ord(c) <= 126 for c in item["error"])
                   for item in status["receivers"])
        assert all(event.endswith((":loaded", ":unloaded")) for event in events), events
    print(f"Receiver module isolation: {cases} relocated ABI/runtime/lifetime/read-only cases passed; no hardware opened.")


if __name__ == "__main__":
    main()
