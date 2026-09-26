#!/usr/bin/env python3
"""Create one immutable TimeHandler environment for a historical replay run.

This utility is intentionally not a clock authority. It samples real UTC once at
launch (unless --real-reference is supplied) and writes the exact same immutable
configuration that every business service must receive.
"""

from __future__ import annotations

import argparse
import math
import os
import pathlib
import tempfile
from datetime import datetime, timezone

UTC_FORMAT = "%Y-%m-%dT%H:%M:%SZ"


def parse_utc(value: str) -> str:
    if len(value) != 20 or not value.endswith("Z"):
        raise argparse.ArgumentTypeError("UTC reference must use YYYY-MM-DDTHH:MM:SSZ")
    try:
        parsed = datetime.strptime(value, UTC_FORMAT)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "UTC reference must use a valid YYYY-MM-DDTHH:MM:SSZ timestamp"
        ) from exc
    return parsed.strftime(UTC_FORMAT)


def parse_speed(value: str) -> float:
    try:
        speed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("speed must be a finite number greater than zero") from exc
    if not math.isfinite(speed) or speed <= 0.0:
        raise argparse.ArgumentTypeError("speed must be a finite number greater than zero")
    return speed


def canonical_speed(value: float) -> str:
    return format(value, ".17g")


def current_real_reference() -> str:
    # Factory parsing is intentionally strict to whole seconds. One sample is taken
    # here and then propagated unchanged to every container in the run.
    return datetime.now(timezone.utc).replace(microsecond=0).strftime(UTC_FORMAT)


def render(speed: float, real_reference: str, simulated_reference: str) -> str:
    return "\n".join(
        [
            "# Generated once for one historical replay run. Do not regenerate per service.",
            f"ALGOTRADING_TIME_SPEED={canonical_speed(speed)}",
            f"ALGOTRADING_TIME_REAL_REFERENCE_UTC={real_reference}",
            f"ALGOTRADING_TIME_SIMULATED_REFERENCE_UTC={simulated_reference}",
            "",
        ]
    )


def atomic_write(path: pathlib.Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent), text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate the immutable shared TimeHandler env for one historical replay run."
    )
    parser.add_argument("--speed", required=True, type=parse_speed)
    parser.add_argument(
        "--simulated-reference",
        required=True,
        type=parse_utc,
        help="business time corresponding to the real reference, YYYY-MM-DDTHH:MM:SSZ",
    )
    parser.add_argument(
        "--real-reference",
        type=parse_utc,
        help="optional deterministic real reference; default samples UTC once now",
    )
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    real_reference = args.real_reference or current_real_reference()
    content = render(args.speed, real_reference, args.simulated_reference)
    atomic_write(args.output, content)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
