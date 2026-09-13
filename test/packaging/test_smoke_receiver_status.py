#!/usr/bin/env python3
"""Execute the receiver-status validator embedded in package smoke checks."""

from __future__ import annotations

import json
from pathlib import Path
import os
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
SMOKE = ROOT / "script/smoke-native-package.sh"


def validator() -> str:
    text = SMOKE.read_text(encoding="utf-8")
    match = re.search(r'\| EXPECTED_RECEIVERS=.*?\$node" -e \'\n(.*?)\n\' \|\| die', text, re.DOTALL)
    if not match:
        raise AssertionError("could not locate embedded receiver-status validator")
    return match.group(1)


def report(compiled: set[str], *, rsp_loadable: bool = False) -> dict:
    return {"schema": 1, "hardwareProbed": False, "receivers": [
        {"receiver": name, "compiled": name in compiled,
         "moduleLoadable": rsp_loadable if name == "RspDuo" and name in compiled else name in compiled}
        for name in ("Kraken", "RspDuo", "Usrp", "HackRF")
    ]}


class SmokeReceiverStatusTest(unittest.TestCase):
    def check(self, payload: dict, receivers: str, expected: int) -> None:
        environment = os.environ | {"EXPECTED_RECEIVERS": receivers, "TEST_ONLY": "true" if receivers != "RspDuo,Usrp,HackRF,Kraken" else "false"}
        result = subprocess.run(["node", "-e", validator()], input=json.dumps(payload), text=True,
                                capture_output=True, env=environment, check=False)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def test_accepts_stable_with_missing_optional_sdrplay_runtime(self):
        self.check(report({"Kraken", "RspDuo", "Usrp", "HackRF"}), "RspDuo,Usrp,HackRF,Kraken", 0)

    def test_accepts_open_test_with_uncompiled_sdrplay_profile(self):
        self.check(report({"Kraken", "Usrp", "HackRF"}), "Usrp,HackRF,Kraken", 0)

    def test_rejects_wrong_compiled_flag_duplicate_extra_or_missing_profile(self):
        wrong = report({"Kraken", "Usrp", "HackRF"})
        wrong["receivers"][1]["compiled"] = True
        noncompiled_loadable = report({"Kraken", "Usrp", "HackRF"})
        noncompiled_loadable["receivers"][1]["moduleLoadable"] = True
        duplicate = report({"Kraken", "Usrp", "HackRF"})
        duplicate["receivers"][3]["receiver"] = "Kraken"
        extra = report({"Kraken", "Usrp", "HackRF"})
        extra["receivers"][3]["receiver"] = "Airspy"
        missing = report({"Kraken", "Usrp", "HackRF"})
        missing["receivers"] = missing["receivers"][:-1]
        for payload in (wrong, noncompiled_loadable, duplicate, extra, missing):
            with self.subTest(payload=payload):
                self.check(payload, "Usrp,HackRF,Kraken", 1)


if __name__ == "__main__":
    unittest.main()
