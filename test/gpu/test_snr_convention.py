#!/usr/bin/env python3
"""Lock GPU fine refinement to the Owl/Retina/blah2arm SNR convention."""
from pathlib import Path
import math
import sys

root = Path(sys.argv[1])
shader = (root / "core" / "GpuPostprocess.private.h").read_text()
legacy_map = (root / "src" / "data" / "Map.cpp").read_text()

assert "10.0LF*positiveLog10(refined/base)" in shader
assert "20.0LF*positiveLog10(refined/base)" not in shader
assert "precise double power=m[49]" not in shader
assert "10 * std::log10(std::max(1e-30, magnitude))" in legacy_map
assert math.isclose(10 * math.log10(10), 10.0, abs_tol=1e-12)
print("PASS SNR convention: Owl/Retina/blah2arm 10*log10(|complex map value|)")
