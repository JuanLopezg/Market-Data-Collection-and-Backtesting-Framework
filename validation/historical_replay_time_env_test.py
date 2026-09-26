#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str], expect_ok: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True)
    if expect_ok and result.returncode != 0:
        raise AssertionError(f"command failed: {command}\nstdout={result.stdout}\nstderr={result.stderr}")
    if not expect_ok and result.returncode == 0:
        raise AssertionError(f"command unexpectedly passed: {command}\nstdout={result.stdout}")
    return result


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    generator = root / "tools/historical_replay/create_time_env.py"
    if not generator.is_file():
        raise AssertionError(f"missing generator: {generator}")

    with tempfile.TemporaryDirectory() as temporary:
        temp = pathlib.Path(temporary)
        output = temp / "time.env"
        command = [
            sys.executable,
            str(generator),
            "--speed",
            "100",
            "--real-reference",
            "2026-09-20T13:00:00Z",
            "--simulated-reference",
            "2020-01-01T00:00:00Z",
            "--output",
            str(output),
        ]
        run(command)
        content = output.read_text(encoding="utf-8")
        expected = {
            "ALGOTRADING_TIME_SPEED=100",
            "ALGOTRADING_TIME_REAL_REFERENCE_UTC=2026-09-20T13:00:00Z",
            "ALGOTRADING_TIME_SIMULATED_REFERENCE_UTC=2020-01-01T00:00:00Z",
        }
        actual = {line for line in content.splitlines() if line and not line.startswith("#")}
        if actual != expected:
            raise AssertionError(f"unexpected env contents: {actual!r}")

        # Re-running with an explicit reference is deterministic.
        output2 = temp / "time2.env"
        run(command[:-1] + [str(output2)])
        if output.read_text(encoding="utf-8") != output2.read_text(encoding="utf-8"):
            raise AssertionError("explicit-reference generation must be deterministic")

        # Bad speed/reference values must fail before producing a usable config.
        for bad_speed in ["0", "-1", "nan", "inf"]:
            bad = temp / f"bad-speed-{bad_speed}.env"
            bad_command = command.copy()
            bad_command[bad_command.index("--speed") + 1] = bad_speed
            bad_command[-1] = str(bad)
            run(bad_command, expect_ok=False)

        bad = temp / "bad-time.env"
        bad_command = command.copy()
        bad_command[bad_command.index("--simulated-reference") + 1] = "2020-02-30T00:00:00Z"
        bad_command[-1] = str(bad)
        run(bad_command, expect_ok=False)

        # Default mode samples UTC once and still writes the strict three-variable contract.
        sampled = temp / "sampled.env"
        sampled_command = [
            sys.executable,
            str(generator),
            "--speed",
            "2",
            "--simulated-reference",
            "2020-01-01T00:00:00Z",
            "--output",
            str(sampled),
        ]
        run(sampled_command)
        sampled_lines = [
            line for line in sampled.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
        if len(sampled_lines) != 3:
            raise AssertionError(f"expected exactly three config variables, got {sampled_lines!r}")
        if not any(line.startswith("ALGOTRADING_TIME_REAL_REFERENCE_UTC=") for line in sampled_lines):
            raise AssertionError("sampled config is missing the real reference")

    print("PASS: T13 historical replay shared time env generator tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
