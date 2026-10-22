"""Small dependency-free helpers shared by OPRS tools."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
from typing import Any


STATUSES = {"PASS", "WARN", "FAIL", "INVALID", "N/A", "NOT_QUALIFIED"}
LAYERS = {"pure_synthetic", "background_injection", "held_out_real"}
PROFILES = {"REF_SURV_2CH", "SURV_ARRAY_NCH", "REF_PLUS_ARRAY", "SINGLE_CHANNEL_REPLAY"}


class ContractError(ValueError):
    """Raised when evidence violates the frozen evaluation contract."""


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def object_hash(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_bytes(value)).hexdigest()


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ContractError(f"cannot read JSON {path}: {error}") from error


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")


def finite(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def wilson(successes: int, total: int, z: float = 1.959963984540054) -> dict[str, Any]:
    if total <= 0:
        return {"estimate": None, "lower_95": None, "upper_95": None, "numerator": successes, "denominator": total}
    p = successes / total
    denominator = 1 + z * z / total
    centre = (p + z * z / (2 * total)) / denominator
    radius = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / denominator
    return {"estimate": p, "lower_95": max(0.0, centre - radius),
            "upper_95": min(1.0, centre + radius), "numerator": successes, "denominator": total}


def percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    low = int(math.floor(position))
    high = int(math.ceil(position))
    if low == high:
        return ordered[low]
    return ordered[low] * (high - position) + ordered[high] * (position - low)
