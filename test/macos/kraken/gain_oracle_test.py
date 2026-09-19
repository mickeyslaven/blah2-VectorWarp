#!/usr/bin/env python3
"""Compare the fake gain ABI with the pinned driver's actual pure gain helpers.

Only extracted arithmetic helpers are compiled; neither libusb nor a real
RTL-SDR device/library is loaded. Requires a local Kraken librtlsdr source tree.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    tuner = (source / "src/tuner_r82xx.c").read_text()
    driver = (source / "src/librtlsdr.c").read_text()
    # Compile the actual source arithmetic, not a second handwritten oracle.
    arithmetic = tuner[tuner.index("static const int r82xx_lna_gain_steps[]"):
                       tuner.index("static int r82xx_get_if_gain_index")]
    getters = tuner[tuner.index("static int r82xx_get_lna_gain_from_index"):
                    tuner.index("static int r82xx_get_vga_gain_from_index")]
    oracle_c = arithmetic + getters + """
int oracle_gain(int request) {
    int lna, mixer;
    r82xx_get_rf_gain_index(request, &lna, &mixer);
    return r82xx_get_lna_gain_from_index(lna) + r82xx_get_mixer_gain_from_index(mixer);
}
"""
    match = re.search(r"static const int r82xx_gains\[\]\s*=\s*\{([^}]+)\}", driver)
    assert match
    real_table = [int(n) for n in re.findall(r"-?\d+", match.group(1))]
    with tempfile.TemporaryDirectory(prefix="vectorwarp-gain-oracle-") as temporary:
        work = Path(temporary)
        (work / "oracle.c").write_text(oracle_c)
        oracle_path = work / "oracle.dylib"
        fake_path = work / "fake.dylib"
        subprocess.run(["xcrun", "clang", "-dynamiclib", str(work / "oracle.c"), "-o", str(oracle_path)], check=True)
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-dynamiclib", "-pthread", "-I", str(source / "include"),
                        str(Path(__file__).with_name("fake_rtlsdr.cpp")), "-o", str(fake_path)], check=True)
        os.environ["VECTORWARP_KRAKEN_SIM_ROOT"] = str(work)
        fake = ctypes.CDLL(str(fake_path))
        oracle = ctypes.CDLL(str(oracle_path))
        handle = ctypes.c_void_p()
        fake.rtlsdr_open.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint]
        fake.rtlsdr_set_tuner_gain.argtypes = [ctypes.c_void_p, ctypes.c_int]
        fake.rtlsdr_get_tuner_gain.argtypes = [ctypes.c_void_p]
        fake.rtlsdr_get_tuner_gains.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        fake.rtlsdr_close.argtypes = [ctypes.c_void_p]
        assert fake.rtlsdr_open(ctypes.byref(handle), 0) == 0
        count = fake.rtlsdr_get_tuner_gains(handle, None)
        table = (ctypes.c_int * count)()
        assert fake.rtlsdr_get_tuner_gains(handle, table) == count
        assert list(table) == real_table
        for request in range(501):
            assert fake.rtlsdr_set_tuner_gain(handle, request) == 0
            expected = oracle.oracle_gain(request)
            actual = fake.rtlsdr_get_tuner_gain(handle)
            assert actual == expected, (request, actual, expected)
        assert fake.rtlsdr_close(handle) == 0
    print(json.dumps({"simulatedOnly": True, "gainRequestsChecked": 501,
                      "supportedGainValues": len(real_table), "requested400Actual": 402}))


if __name__ == "__main__":
    main()
