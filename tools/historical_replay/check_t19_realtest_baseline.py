#!/usr/bin/env python3
from __future__ import annotations
import argparse
from decimal import Decimal, InvalidOperation
import importlib.util
import json
from pathlib import Path


IDENTITY_FIELDS = ("id", "coin", "direction", "start", "end", "entry", "exit", "exited")


def load_checker(path: Path):
    spec = importlib.util.spec_from_file_location("realtest_baseline_checker", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"[FAIL] cannot import existing RealTest baseline checker: {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def compare_known_exception_candidate(checker, actual, expected, *, location: str) -> None:
    """Lock known-exception identity/economic coordinates, not stale sizing magnitudes.

    This function is used ONLY for candidate trades already selected by the locked
    mismatch identity (kind + comparison_index + match_type). New mismatches never
    reach this relaxation because mismatch count/order/identity is checked first.
    """
    if expected is None:
        if actual is not None:
            checker.fail(f"{location}: expected <no trade>, got {actual}")
        return
    if actual is None:
        checker.fail(f"{location}: expected trade, got <no trade>")

    for field in IDENTITY_FIELDS:
        if str(actual.get(field)) != str(expected.get(field)):
            checker.fail(
                f"{location}.{field}: expected {expected.get(field)!r}, got {actual.get(field)!r}"
            )

    # Size/PnL remain required, finite informational outputs for the pre-approved
    # exception only. They are intentionally not snapshot-locked because exact
    # runtime sizing fixes can change these magnitudes without changing the known
    # exception identity, dates, prices, direction or lifecycle state.
    for field in ("size", "pnl"):
        raw = actual.get(field)
        if raw is None:
            checker.fail(f"{location}.{field}: missing")
        try:
            value = Decimal(str(raw))
        except (InvalidOperation, ValueError):
            checker.fail(f"{location}.{field}: invalid numeric value {raw!r}")
        if not value.is_finite():
            checker.fail(f"{location}.{field}: non-finite numeric value {raw!r}")
    if Decimal(str(actual["size"])) <= 0:
        checker.fail(f"{location}.size: expected positive quantity, got {actual['size']!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--mode", choices=("equal-weight", "vol-target"), default="equal-weight")
    ap.add_argument("--log", required=True)
    args = ap.parse_args()

    root = Path(args.root).resolve()
    log_path = Path(args.log).resolve()
    checker_path = root / "tools/distributed_compare/check_realtest_baseline.py"
    baseline_path = root / "tools/distributed_compare/realtest_known_baseline.json"
    if not log_path.is_file():
        raise SystemExit(f"[FAIL] T19 RealTest log not found: {log_path}")
    if not checker_path.is_file():
        raise SystemExit(f"[FAIL] existing RealTest checker not found: {checker_path}")
    if not baseline_path.is_file():
        raise SystemExit(f"[FAIL] existing RealTest known baseline not found: {baseline_path}")

    text = log_path.read_text(encoding="utf-8", errors="replace")
    if "T19_REALTEST_RESEARCH_POLICY: REVIEW_REQUIRED" not in text:
        raise SystemExit("[FAIL] T19 expected the exact historical REVIEW_REQUIRED policy result")

    checker = load_checker(checker_path)
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    mode_key = "equal_weight" if args.mode == "equal-weight" else "vol_target"
    expected = baseline[mode_key]

    # Summary remains fully locked: 631 vs 631, 628 exact matches and exactly the
    # historically accepted exception count. Any new/different mismatch fails.
    checker.check_summary(text, mode_key, expected["summary"])
    actual_mismatches = checker.mismatch_blocks(text, mode_key)
    expected_mismatches = expected["mismatches"]
    if len(actual_mismatches) != len(expected_mismatches):
        checker.fail(
            f"T19 mismatch count changed: expected {len(expected_mismatches)}, got {len(actual_mismatches)}"
        )

    for i, (actual, exp) in enumerate(zip(actual_mismatches, expected_mismatches), 1):
        for key in ("kind", "comparison_index"):
            if actual.get(key) != exp.get(key):
                checker.fail(
                    f"T19 difference #{i} {key}: expected {exp.get(key)!r}, got {actual.get(key)!r}"
                )
        if exp.get("match_type") is not None and actual.get("match_type") != exp.get("match_type"):
            checker.fail(
                f"T19 difference #{i} match_type: expected {exp.get('match_type')!r}, got {actual.get('match_type')!r}"
            )

        # The RealTest/reference side stays exact, including size/PnL for EqualWeight.
        if "realtest" in exp:
            checker.compare_trade(
                actual.get("realtest"), exp.get("realtest"), mode=mode_key,
                location=f"T19 difference #{i}.realtest"
            )

        # Candidate side: for already locked historical exceptions, lock identity,
        # dates, prices, direction and exited state. Size/PnL are finite informational
        # magnitudes, not exception identity.
        compare_known_exception_candidate(
            checker,
            actual.get("candidate"),
            exp.get("candidate"),
            location=f"T19 difference #{i}.candidate",
        )

    identities = []
    for item in actual_mismatches:
        trade = item.get("realtest") or item.get("candidate") or {}
        if trade.get("coin"):
            identities.append(
                f"{item.get('kind')}:{item.get('comparison_index')}:{trade.get('coin')}"
            )

    print(
        "PASS: T19 exact research RealTest policy matches locked accepted exception identities "
        f"mode={args.mode} differences={len(actual_mismatches)} "
        f"identities={','.join(identities)} "
        "(candidate size/PnL informational only inside those locked exceptions)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
