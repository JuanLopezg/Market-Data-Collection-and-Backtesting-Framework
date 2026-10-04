#!/usr/bin/env python3
from __future__ import annotations
import argparse
import json
import math
import re
from pathlib import Path

TRADE_FIELDS = ("id", "coin", "direction", "start", "end", "entry", "exit", "size", "pnl", "exited")
IDENTITY_FIELDS = ("id", "coin", "direction", "start", "end", "exited")


def parse_trade_section(block: str, label: str):
    label_match = re.search(rf"(?m)^{re.escape(label)}\s*$", block)
    if not label_match:
        return None
    start = label_match.end()
    if label == "REALTEST":
        next_match = re.search(r"(?m)^BACKTESTER\s*$", block[start:])
    else:
        next_match = re.search(r"(?m)^(?:DIFFERENCES|={10,}|INTERACTIVE COMPARISON SUMMARY)\s*$", block[start:])
    end = start + next_match.start() if next_match else len(block)
    body = block[start:end]
    if "<no trade>" in body:
        return None

    out = {}
    for key in TRADE_FIELDS:
        match = re.search(rf"(?m)^\s*{re.escape(key)}\s*:\s*(.*?)\s*$", body)
        if match:
            out[key] = match.group(1)
    return out or None


def parse_log(text: str):
    summary = {}
    patterns = {
        "realtest_trades_checked": r"RealTest trades checked\s*:\s*(\d+)",
        "backtester_trades_total": r"Backtester trades total\s*:\s*(\d+)",
        "fully_matched": r"Fully matched\s*:\s*(\d+)",
        "different_or_missing": r"Different / missing\s*:\s*(\d+)",
        "missing_backtester_same_day": r"Missing backtester same-day\s*:\s*(\d+)",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match:
            raise SystemExit(f"[FAIL] RealTest summary field missing: {key}")
        summary[key] = int(match.group(1))

    header = re.compile(r"(?m)^(MISMATCH) #(\d+)\s*$|^(UNMATCHED BACKTESTER TRADE) #(\d+)\s*$")
    matches = list(header.finditer(text))
    items = []
    summary_pos = text.find("INTERACTIVE COMPARISON SUMMARY")

    for index, match in enumerate(matches):
        block_start = match.start()
        next_start = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        if summary_pos >= 0 and summary_pos > block_start:
            next_start = min(next_start, summary_pos)
        block = text[block_start:next_start]

        if match.group(1):
            comparison = re.search(r"REAL COMPARISON INDEX:\s*(\d+)", block)
            match_type = re.search(r"MATCH TYPE\s*:\s*(\S+)", block)
            if not comparison or not match_type:
                raise SystemExit("[FAIL] malformed RealTest mismatch block")
            kind = (
                "missing_candidate"
                if match_type.group(1) == "NO_BACKTESTER_TRADE"
                else "matched_mismatch"
            )
            items.append({
                "kind": kind,
                "comparison_index": int(comparison.group(1)),
                "match_type": match_type.group(1),
                "realtest": parse_trade_section(block, "REALTEST"),
                "candidate": parse_trade_section(block, "BACKTESTER"),
            })
        else:
            items.append({
                "kind": "unmatched_candidate",
                "comparison_index": int(match.group(4)),
                "candidate": parse_trade_section(block, "BACKTESTER"),
            })

    return summary, items


def ensure_finite_trade(trade, where: str):
    if trade is None:
        return
    for key in ("entry", "exit", "size", "pnl"):
        if key not in trade:
            raise SystemExit(f"[FAIL] {where}.{key} missing")
        try:
            value = float(trade[key])
        except Exception as exc:
            raise SystemExit(f"[FAIL] {where}.{key} invalid") from exc
        if not math.isfinite(value):
            raise SystemExit(f"[FAIL] {where}.{key} non-finite")
    if float(trade["entry"]) <= 0 or float(trade["exit"]) <= 0 or float(trade["size"]) <= 0:
        raise SystemExit(f"[FAIL] {where} has non-positive price/size")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--summary")
    args = parser.parse_args()

    text = Path(args.log).read_text(encoding="utf-8", errors="replace")
    if "T19_REALTEST_RESEARCH_POLICY: REVIEW_REQUIRED" not in text:
        raise SystemExit("[FAIL] expected historical RealTest REVIEW_REQUIRED research-policy outcome")

    baseline = json.loads(Path(args.baseline).read_text(encoding="utf-8"))["equal_weight"]
    summary, items = parse_log(text)

    if summary != baseline["summary"]:
        raise SystemExit(f"[FAIL] RealTest summary changed: got={summary} expected={baseline['summary']}")

    expected = baseline["mismatches"]
    if len(items) != len(expected):
        raise SystemExit(
            f"[FAIL] RealTest mismatch identity count changed: got={len(items)} expected={len(expected)}"
        )

    for ordinal, (got, want) in enumerate(zip(items, expected), 1):
        for key in ("kind", "comparison_index"):
            if got.get(key) != want.get(key):
                raise SystemExit(
                    f"[FAIL] mismatch #{ordinal} {key}: got={got.get(key)!r} expected={want.get(key)!r}"
                )

        if want.get("match_type") is not None and got.get("match_type") != want.get("match_type"):
            raise SystemExit(f"[FAIL] mismatch #{ordinal} match_type changed")

        if "realtest" in want:
            got_realtest = got.get("realtest")
            want_realtest = want.get("realtest")
            if got_realtest is None:
                raise SystemExit(f"[FAIL] mismatch #{ordinal} lost RealTest side")
            for key, value in want_realtest.items():
                if str(got_realtest.get(key)) != str(value):
                    raise SystemExit(
                        f"[FAIL] mismatch #{ordinal} RealTest {key}: "
                        f"got={got_realtest.get(key)!r} expected={value!r}"
                    )

        got_candidate = got.get("candidate")
        want_candidate = want.get("candidate")
        if want_candidate is None:
            if got_candidate is not None:
                raise SystemExit(f"[FAIL] mismatch #{ordinal} unexpectedly gained candidate trade")
        else:
            if got_candidate is None:
                raise SystemExit(f"[FAIL] mismatch #{ordinal} candidate trade disappeared")
            for key in IDENTITY_FIELDS:
                if str(got_candidate.get(key)) != str(want_candidate.get(key)):
                    raise SystemExit(
                        f"[FAIL] mismatch #{ordinal} candidate {key}: "
                        f"got={got_candidate.get(key)!r} expected={want_candidate.get(key)!r}"
                    )
            ensure_finite_trade(got_candidate, f"mismatch #{ordinal}.candidate")

    output = {
        "result": "PASS",
        "researchPolicy": "REVIEW_REQUIRED",
        "summary": summary,
        "mismatchIdentities": [
            f"{item['kind']}:{item['comparison_index']}:"
            f"{(item.get('realtest') or item.get('candidate') or {}).get('coin', '')}"
            for item in items
        ],
        "candidateMagnitudePolicy": "IDENTITY_LOCKED_MAGNITUDES_INFORMATIONAL_FOR_PREEXISTING_EXCEPTIONS",
    }

    if args.summary:
        path = Path(args.summary)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(
        "PASS: STEP58A RealTest research-policy exceptions unchanged "
        + ",".join(output["mismatchIdentities"])
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
