import importlib.util
from pathlib import Path

import numpy as np


SCRIPT = (Path(__file__).parents[2] / "script" /
          "analyze-external-reference-cancellation.py")
SPEC = importlib.util.spec_from_file_location("external_cancellation", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_held_out_cancellation_prefers_matching_reference():
    rng = np.random.default_rng(31)
    reference = (rng.normal(size=65536) + 1j * rng.normal(size=65536)).astype(
        np.complex64)
    target = np.roll(reference, 3) * (.7 + .2j)
    target += .05 * (rng.normal(size=target.size) + 1j * rng.normal(size=target.size))
    unrelated = (rng.normal(size=target.size) + 1j * rng.normal(size=target.size))
    assert MODULE.frequency_selective_cancellation(target, reference) > 10
    assert MODULE.frequency_selective_cancellation(target, unrelated) < 1


def test_second_reference_adds_independent_predictive_signal():
    rng = np.random.default_rng(37)
    first = (rng.normal(size=65536) + 1j * rng.normal(size=65536)).astype(
        np.complex64)
    second = (rng.normal(size=65536) + 1j * rng.normal(size=65536)).astype(
        np.complex64)
    target = first + .6 * second
    one = MODULE.frequency_selective_cancellation(target, first)
    both = MODULE.multi_reference_cancellation(target, [first, second])
    assert both > one + 10
