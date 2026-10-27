import importlib.util
import json
import struct
from pathlib import Path

import numpy as np


SCRIPT = Path(__file__).parents[2] / "script" / "build-external-reference-replay.py"
SPEC = importlib.util.spec_from_file_location("external_replay", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
QUALIFIER_SCRIPT = SCRIPT.with_name("qualify-external-reference.py")
MODULE.QUALIFIER = MODULE.load_qualifier(QUALIFIER_SCRIPT)


def test_build_preserves_five_surveillance_channels_and_aligns_reference(tmp_path):
    hp_rate, owl_rate, seconds = 2400, 2000, 2
    rng = np.random.default_rng(5)
    owl = (rng.normal(size=owl_rate * seconds + 100) +
           1j * rng.normal(size=owl_rate * seconds + 100)).astype(np.complex64)
    slope, intercept, cfo = owl_rate / hp_rate, 20.0, 13.0
    base = MODULE.QUALIFIER.linear_resample(owl, intercept, slope, hp_rate * seconds)
    time = np.arange(base.size) / hp_rate
    pilot = base * np.exp(2j * np.pi * cfo * time)
    hp = np.stack([pilot * (1 + channel / 10) for channel in range(5)]).astype(np.complex64)
    qualification = {"best": {"affine_owl_samples_per_hp_sample": slope,
                               "affine_intercept_owl_samples": intercept,
                               "median_cfo_hz": cfo, "kraken_channel": 0}}
    blocks, receipts = MODULE.build(hp, owl, qualification, hp_rate, owl_rate, .5)
    assert len(blocks) == 4
    assert all(block.shape == (6, 1200) for block in blocks)
    assert np.allclose(blocks[0][1:], hp[:, :1200])
    assert min(row["coherence"] for row in receipts) > .99

    output = tmp_path / "six.blah2iq"
    samples = MODULE.write_blah2iq(output, 6, hp_rate, 551_000_000, blocks)
    data = output.read_bytes()
    assert data[:8] == b"BLAH2IQ\0"
    assert struct.unpack_from("<IIIIII", data, 8)[2:5] == (6, hp_rate, 551_000_000)
    assert samples == hp_rate * seconds
