#!/usr/bin/env python3
import argparse
import json
import re
import sys
from pathlib import Path

TRADE_FIELDS = ("id", "coin", "direction", "start", "end", "entry", "exit", "size", "pnl", "exited")


def fail(message: str) -> None:
    print(f"[FAIL] RealTest known-baseline check: {message}")
    raise SystemExit(1)


def grab_int(text: str, label: str) -> int:
    m = re.search(rf"^{re.escape(label)}\s*:\s*(\d+)\s*$", text, re.MULTILINE)
    if not m:
        fail(f"missing summary field: {label}")
    return int(m.group(1))


def grab_text(text: str, label: str) -> str:
    m = re.search(rf"^{re.escape(label)}\s*:\s*(.*?)\s*$", text, re.MULTILINE)
    if not m:
        fail(f"missing field: {label}")
    return m.group(1).strip()


def section_between(block: str, start_label: str, stop_labels: tuple[str, ...]) -> str:
    start_match = re.search(rf"^{re.escape(start_label)}\s*$", block, re.MULTILINE)
    if not start_match:
        fail(f"missing section {start_label}")
    start = start_match.end()
    end = len(block)
    for label in stop_labels:
        m = re.search(rf"^{re.escape(label)}(?:\s.*)?$", block[start:], re.MULTILINE)
        if m:
            end = min(end, start + m.start())
    return block[start:end]


def parse_trade(section: str):
    if "<no trade>" in section:
        return None
    result = {}
    for field in TRADE_FIELDS:
        m = re.search(rf"^\s*{re.escape(field)}\s*:\s*(.*?)\s*$", section, re.MULTILINE)
        if m:
            result[field] = m.group(1).strip()
    return result


def compare_trade(actual, expected, *, mode: str, location: str) -> None:
    if expected is None:
        if actual is not None:
            fail(f"{location}: expected <no trade>, got {actual}")
        return
    if actual is None:
        fail(f"{location}: expected trade, got <no trade>")

    # EqualWeight's research policy validates all these economic fields.
    # VolTarget intentionally ignores size/PnL magnitude, so the baseline lock does too.
    fields = ("id", "coin", "direction", "start", "end", "entry", "exit", "exited")
    if mode == "equal_weight":
        fields += ("size", "pnl")
    for field in fields:
        if str(actual.get(field)) != str(expected.get(field)):
            fail(
                f"{location}.{field}: expected {expected.get(field)!r}, got {actual.get(field)!r}"
            )


def mismatch_blocks(text: str, mode: str):
    if mode == "equal_weight":
        header = r"^MISMATCH #(\d+)\s*$"
        tail_marker = "INTERACTIVE COMPARISON SUMMARY"
        unmatched_header = r"^UNMATCHED BACKTESTER TRADE #(\d+)\s*$"
    else:
        header = r"^VOL-TARGET MISMATCH #(\d+)\s*$"
        tail_marker = "VOL-TARGET CAMPAIGN VALIDATION SUMMARY"
        unmatched_header = r"^UNMATCHED BACKTESTER CAMPAIGN #(\d+)\s*$"

    starts = [(m.start(), "mismatch", int(m.group(1))) for m in re.finditer(header, text, re.MULTILINE)]
    starts += [(m.start(), "unmatched", int(m.group(1))) for m in re.finditer(unmatched_header, text, re.MULTILINE)]
    starts.sort()
    tail_pos = text.find(tail_marker)
    if tail_pos < 0:
        fail(f"missing {tail_marker}")

    parsed = []
    for idx, (start, kind, number) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else tail_pos
        block = text[start:end]
        if kind == "mismatch":
            index_match = re.search(r"^REAL COMPARISON INDEX:\s*(\d+)\s*$", block, re.MULTILINE)
            match_type = re.search(r"^MATCH TYPE\s*:\s*(.*?)\s*$", block, re.MULTILINE)
            if not index_match or not match_type:
                fail(f"mismatch #{number}: missing comparison index/match type")
            real_section = section_between(block, "REALTEST", ("BACKTESTER",))
            candidate_section = section_between(
                block,
                "BACKTESTER",
                ("DIFFERENCES", "VALIDATION DIFFERENCES", "INFORMATIONAL ONLY", "Press ENTER")
            )
            parsed.append({
                "kind": "missing_candidate" if parse_trade(candidate_section) is None else "matched_mismatch",
                "comparison_index": int(index_match.group(1)),
                "match_type": match_type.group(1).strip(),
                "realtest": parse_trade(real_section),
                "candidate": parse_trade(candidate_section),
            })
        else:
            candidate_section = section_between(block, "BACKTESTER", ("Press ENTER",))
            parsed.append({
                "kind": "unmatched_candidate",
                "comparison_index": number,
                "candidate": parse_trade(candidate_section),
            })
    return parsed


def check_summary(text: str, mode: str, expected: dict) -> None:
    if mode == "equal_weight":
        actual = {
            "realtest_trades_checked": grab_int(text, "RealTest trades checked"),
            "backtester_trades_total": grab_int(text, "Backtester trades total"),
            "fully_matched": grab_int(text, "Fully matched"),
            "different_or_missing": grab_int(text, "Different / missing"),
            "missing_backtester_same_day": grab_int(text, "Missing backtester same-day"),
        }
    else:
        actual = {
            "campaigns_matching": grab_int(text, "Campaigns matching date/price/direction"),
            "different_matched_campaigns": grab_int(text, "Different matched campaigns"),
            "missing_backtester_campaigns": grab_int(text, "Missing backtester campaigns"),
            "unmatched_backtester_campaigns": grab_int(text, "Unmatched backtester campaigns"),
            "price_tolerance_percent": grab_text(text, "Price tolerance").removesuffix("%"),
        }
    if actual != expected:
        fail(f"summary changed: expected {expected}, got {actual}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("equal-weight", "vol-target"), required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument(
        "--baseline",
        default=str(Path(__file__).with_name("realtest_known_baseline.json")),
    )
    args = parser.parse_args()

    log_path = Path(args.log)
    if not log_path.is_file():
        fail(f"log not found: {log_path}")
    baseline_path = Path(args.baseline)
    if not baseline_path.is_file():
        fail(f"baseline file not found: {baseline_path}")

    text = log_path.read_text(encoding="utf-8", errors="replace")
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    mode = "equal_weight" if args.mode == "equal-weight" else "vol_target"
    expected = baseline[mode]

    if "DISTRIBUTED_FAST_COMPARE: PASS" not in text:
        fail("distributed-fast prerequisite did not PASS")
    if "DISTRIBUTED_REALTEST_RESEARCH_POLICY: REVIEW_REQUIRED" not in text:
        fail("expected exact research comparator REVIEW_REQUIRED marker not found")

    check_summary(text, mode, expected["summary"])
    actual_mismatches = mismatch_blocks(text, mode)
    expected_mismatches = expected["mismatches"]
    if len(actual_mismatches) != len(expected_mismatches):
        fail(
            f"mismatch count changed: expected {len(expected_mismatches)}, got {len(actual_mismatches)}"
        )

    for i, (actual, exp) in enumerate(zip(actual_mismatches, expected_mismatches), 1):
        for key in ("kind", "comparison_index"):
            if actual.get(key) != exp.get(key):
                fail(f"difference #{i} {key}: expected {exp.get(key)!r}, got {actual.get(key)!r}")
        if exp.get("match_type") is not None and actual.get("match_type") != exp.get("match_type"):
            fail(
                f"difference #{i} match_type: expected {exp.get('match_type')!r}, got {actual.get('match_type')!r}"
            )
        if "realtest" in exp:
            compare_trade(actual.get("realtest"), exp.get("realtest"), mode=mode, location=f"difference #{i}.realtest")
        compare_trade(actual.get("candidate"), exp.get("candidate"), mode=mode, location=f"difference #{i}.candidate")

    print(
        "REALTEST_KNOWN_BASELINE: PASS "
        f"mode={args.mode} differences={len(actual_mismatches)} "
        "(exact research policy + exact accepted exception identities)"
    )


if __name__ == "__main__":
    main()
