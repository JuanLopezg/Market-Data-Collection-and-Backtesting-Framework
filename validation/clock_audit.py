#!/usr/bin/env python3
"""STEP 35A — static audit of time sources before shared logical clock work.

This audit is intentionally non-invasive: it does not change trading semantics.  It
classifies host/monotonic time usage and fails if a direct business-time dependency
appears in the distributed trading services or runtime business core.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

SOURCE_EXTENSIONS = {".h", ".hpp", ".c", ".cc", ".cpp", ".py", ".sh"}
SKIP_PARTS = {".git", "build", "builddir", ".runtime_bundle", "logs", "__pycache__"}

RUNTIME_SERVICES = {
    "strategy_service",
    "portfolio_risk_service",
    "order_planner_service",
    "execution_state_service",
    "exchange_gateway",
    "market_data_service",
    "replay_controller",
    "simulated_exchange_service",
}

BUSINESS_LIB_AREAS = {
    "account", "backtest", "contracts", "exchange", "execution", "filter", "indicator",
    "market", "portfolio", "position", "ranker", "rebalance", "recovery", "risk", "runtime",
    "signal", "sizing", "strategy", "universe",
}

PATTERNS = {
    "wall_clock": re.compile(r"(?:std::chrono::)?system_clock::now\s*\("),
    "c_time": re.compile(r"\b(?:std::)?time\s*\(\s*(?:nullptr|NULL|0)?\s*\)"),
    "steady_clock": re.compile(r"(?:std::chrono::)?steady_clock::now\s*\("),
    "high_resolution": re.compile(r"(?:std::chrono::)?high_resolution_clock"),
    "sleep": re.compile(r"(?:std::this_thread::)?sleep_(?:for|until)\s*\("),
    "wait": re.compile(r"\.(?:wait_for|wait_until)\s*\("),
    "sql_now": re.compile(r"\bNOW\s*\(\s*\)|\bCURRENT_TIMESTAMP\b"),
    "shell_date": re.compile(r"\bdate\s+\+"),
    "shell_sleep": re.compile(r"^\s*sleep\s+[0-9.]", re.MULTILINE),
}

NOARG_TIME_UTILS = re.compile(
    r"\b(?:nowString|currentUtcTimestamp|timeUntilUtcMidnight|getCurrentUtcDate|computeNextMidnightUTC)\s*\(\s*\)"
)

@dataclass(frozen=True)
class Hit:
    path: str
    line: int
    kind: str
    text: str
    classification: str
    severity: str


def iter_source_files(root: Path) -> Iterable[Path]:
    for top in ("lib", "live_trading", "research", "tools", "validation"):
        base = root / top
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_EXTENSIONS:
                continue
            if path.name == "clock_audit.py":
                continue
            if any(part in SKIP_PARTS for part in path.parts):
                continue
            yield path


def rel(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def is_runtime_service(path: str) -> bool:
    parts = Path(path).parts
    return len(parts) >= 2 and parts[0] == "live_trading" and parts[1] in RUNTIME_SERVICES


def is_business_lib(path: str) -> bool:
    parts = Path(path).parts
    return len(parts) >= 3 and parts[0] == "lib" and parts[1] == "src" and parts[2] in BUSINESS_LIB_AREAS


def classify(path: str, kind: str, text: str) -> tuple[str, str]:
    # The Clock implementation is the one legitimate wall-clock boundary for LIVE/TESTNET.
    if path == "lib/src/runtime/clock.h":
        return "CLOCK_BOUNDARY", "OK"

    # Existing database downloader/scheduler already injects Clock. Host sleeps are network backoff.
    if path.startswith("live_trading/database/"):
        if kind in {"sleep", "wait", "steady_clock", "high_resolution"}:
            return "TECHNICAL_DATABASE_SCHEDULING", "OK"
        return "DATABASE_TIME_REVIEW", "INFO"

    # Replay controller steady_clock measures technical waiting only: barrier deadlines,
    # control-plane refresh cadence and wall pacing for REPLAY speed presentation. Economic
    # event time still comes exclusively from the shared logical ClockState.
    if path == "live_trading/replay_controller/src/replay_controller_main.cpp" and kind == "steady_clock":
        return "TECHNICAL_REPLAY_WALL_PACING", "OK"

    # ServiceClockContext may use steady_clock only to rate-limit technical re-sync
    # requests while a REPLAY follower is unsynchronized. It never defines event time.
    if path == "lib/src/runtime/service_clock.h" and kind == "steady_clock":
        return "TECHNICAL_CLOCK_RESYNC_RETRY", "OK"

    # Startup request nonce is transport identity only; produced_at comes from engine event state.
    if path == "live_trading/execution_state_service/src/execution_state_service_main.cpp" and kind == "wall_clock":
        if "nonce" in text and "time_since_epoch" in text:
            return "TECHNICAL_MESSAGE_ID_NONCE", "OK"
        return "BUSINESS_WALL_CLOCK", "FAIL"

    # Any other direct time source/sleep in a distributed runtime service is a blocker.
    if is_runtime_service(path):
        if kind in {"wall_clock", "c_time", "sleep", "high_resolution"}:
            return "BUSINESS_TIME_DEPENDENCY", "FAIL"
        if kind == "steady_clock":
            return "UNCLASSIFIED_RUNTIME_MONOTONIC", "FAIL"
        return "RUNTIME_TECHNICAL_TIME", "INFO"

    # Runtime/business library code must not source current time itself. clock.h is excepted above.
    if is_business_lib(path):
        if kind in {"wall_clock", "c_time", "sleep", "high_resolution", "steady_clock"}:
            return "BUSINESS_CORE_TIME_DEPENDENCY", "FAIL"
        return "BUSINESS_CORE_REVIEW", "INFO"

    # Generic scheduler/config/persistence timing is process/filesystem/storage metadata.
    if path == "lib/src/common_types/scheduler.h":
        if kind == "high_resolution":
            return "TECHNICAL_SCHEDULER_CLOCK_REVIEW", "WARN"
        return "TECHNICAL_SCHEDULER_WAIT", "OK"
    if path == "lib/src/common_types/config_handler.h":
        return "TECHNICAL_CONFIG_FILE_MTIME", "OK"
    if path == "lib/src/persistence/postgres_state_store.cpp" and kind == "sql_now":
        return "TECHNICAL_PERSISTENCE_UPDATED_AT", "OK"
    if path == "lib/src/utils/database_utils.cpp" and kind == "wall_clock":
        return "TECHNICAL_ARTIFACT_FILENAME", "OK"

    # No-argument helpers expose host wall time. They are not currently used by the distributed
    # business services, but must not be introduced there during the clock implementation.
    if path == "lib/src/utils/time_utils.cpp" and kind in {"wall_clock", "c_time"}:
        return "HOST_TIME_UTILITY_NOT_BUSINESS", "WARN"

    # Research, comparators and validation harnesses may measure real elapsed time or timestamp files.
    if path.startswith("research/"):
        return "RESEARCH_TOOLING_TIME", "OK"
    if path.startswith("tools/"):
        return "TOOLING_TIME", "OK"
    if path.startswith("validation/"):
        return "VALIDATION_HARNESS_TIME", "OK"

    return "UNCLASSIFIED", "FAIL"


def scan(root: Path) -> list[Hit]:
    hits: list[Hit] = []
    for path in iter_source_files(root):
        rpath = rel(root, path)
        try:
            content = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line_no, line in enumerate(content.splitlines(), 1):
            for kind, rx in PATTERNS.items():
                if rx.search(line):
                    classification, severity = classify(rpath, kind, line.strip())
                    hits.append(Hit(rpath, line_no, kind, line.strip(), classification, severity))

            # A no-arg host-time utility call inside runtime/business code is also a violation even
            # though the current-time source lives in time_utils.cpp.
            if NOARG_TIME_UTILS.search(line) and (is_runtime_service(rpath) or is_business_lib(rpath)):
                # Ignore the helper declarations/definitions themselves.
                if rpath not in {"lib/src/utils/time_utils.cpp", "lib/src/utils/time_utils.h"}:
                    hits.append(Hit(
                        rpath, line_no, "host_time_helper", line.strip(),
                        "BUSINESS_HOST_TIME_HELPER", "FAIL"
                    ))
    return hits


def find_clock_consumers(root: Path) -> list[str]:
    consumers: list[str] = []
    base = root / "live_trading"
    for service in sorted(RUNTIME_SERVICES):
        service_root = base / service
        if not service_root.exists():
            continue
        found = False
        for path in service_root.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_EXTENSIONS:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"\b(?:Clock|SystemClock|FixedClock|SimulatedClock|ServiceClockContext)\b", text):
                found = True
                break
        if found:
            consumers.append(service)
    return consumers


def exists_token(root: Path, token: str) -> bool:
    rx = re.compile(rf"\b{re.escape(token)}\b")
    for path in iter_source_files(root):
        try:
            if rx.search(path.read_text(encoding="utf-8", errors="replace")):
                return True
        except OSError:
            pass
    return False


def render_markdown(root: Path, hits: list[Hit], consumers: list[str]) -> str:
    failures = [h for h in hits if h.severity == "FAIL"]
    warnings = [h for h in hits if h.severity == "WARN"]
    ok = [h for h in hits if h.severity == "OK"]
    simulated_clock = exists_token(root, "SimulatedClock")
    clock_state = exists_token(root, "ClockState")

    lines = [
        "# STEP 35A — Clock audit report",
        "",
        "This report is a static pre-implementation audit. `PASS` means the current time sources",
        "were classified without finding a direct business wall-clock dependency. It does **not**",
        "mean the shared logical clock has been implemented or validated.",
        "",
        "## Summary",
        "",
        f"- Audit result: **{'PASS' if not failures else 'FAIL'}**",
        f"- Blocking business-time findings: **{len(failures)}**",
        f"- Review warnings: **{len(warnings)}**",
        f"- Classified OK time-source hits: **{len(ok)}**",
        f"- Distributed replay services using `Clock`: **{len(consumers)}/{len(RUNTIME_SERVICES)}**",
        f"- `SimulatedClock` present: **{'YES' if simulated_clock else 'NO'}**",
        f"- `ClockState` present: **{'YES' if clock_state else 'NO'}**",
        "",
        "## Architectural finding",
        "",
        "The common `Clock`/`SystemClock` boundary exists. Distributed services may be wired through",
        "`ServiceClockContext`, which selects the simple `SystemClock` in LIVE/TESTNET and a",
        "`SimulatedClock` follower in REPLAY. Replay-controller barrier timeouts remain technical",
        "monotonic/process time and are intentionally outside logical-time semantics.",
        "",
        "## Review warnings",
        "",
    ]
    if warnings:
        for h in warnings:
            lines.append(f"- `{h.path}:{h.line}` — `{h.classification}` — `{h.text}`")
    else:
        lines.append("- None.")

    lines += ["", "## Blocking findings", ""]
    if failures:
        for h in failures:
            lines.append(f"- `{h.path}:{h.line}` — `{h.classification}` — `{h.text}`")
    else:
        lines.append("- None.")

    lines += [
        "",
        "## Current runtime Clock consumers",
        "",
    ]
    if consumers:
        for service in consumers:
            lines.append(f"- `{service}`")
    else:
        lines.append("- None of the distributed replay services yet.")

    lines += [
        "",
        "## Next gate",
        "",
        "Do not treat this audit as clock completion. Implement the shared logical clock and then add",
        "validation for monotonic revisions, restart recovery, pause/resume, x1 vs accelerated/MAX",
        "economic equivalence, shared simulation time and exact `distributed == fast` where applicable.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--report", type=Path, help="Write a Markdown report to this path")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    root = args.root.resolve()
    clock_header = root / "lib/src/runtime/clock.h"
    if not clock_header.exists():
        print(f"[FAIL] expected Clock abstraction missing: {clock_header}", file=sys.stderr)
        return 2

    hits = scan(root)
    consumers = find_clock_consumers(root)
    failures = [h for h in hits if h.severity == "FAIL"]
    warnings = [h for h in hits if h.severity == "WARN"]

    print("============================================================")
    print("STEP 35A — CLOCK AUDIT")
    print("============================================================")
    print(f"root                         : {root}")
    print(f"time-source hits             : {len(hits)}")
    print(f"blocking business findings   : {len(failures)}")
    print(f"review warnings              : {len(warnings)}")
    print(f"runtime Clock consumers      : {len(consumers)}/{len(RUNTIME_SERVICES)}")
    print(f"SimulatedClock implemented   : {'YES' if exists_token(root, 'SimulatedClock') else 'NO'}")
    print(f"ClockState implemented       : {'YES' if exists_token(root, 'ClockState') else 'NO'}")

    if warnings:
        print("\n[WARN] review items:")
        for h in warnings:
            print(f"  {h.path}:{h.line} [{h.classification}] {h.text}")

    if failures:
        print("\n[FAIL] unapproved time dependencies:")
        for h in failures:
            print(f"  {h.path}:{h.line} [{h.classification}] {h.text}")

    if args.verbose:
        print("\n[INFO] complete classification:")
        for h in hits:
            print(f"  {h.severity:4} {h.path}:{h.line} [{h.classification}] {h.text}")

    report = render_markdown(root, hits, consumers)
    if args.report:
        report_path = args.report
        if not report_path.is_absolute():
            report_path = root / report_path
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(report, encoding="utf-8")
        print(f"\n[INFO] report written: {report_path}")

    print("\n============================================================")
    if failures:
        print("STEP 35A CLOCK AUDIT RESULT: FAIL")
        print("Do not implement the shared logical clock until findings are classified/resolved.")
        print("============================================================")
        return 1

    print("STEP 35A CLOCK AUDIT RESULT: PASS")
    print("Audit only. This PASS does not by itself close the shared-clock implementation/validation phase.")
    print("============================================================")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
