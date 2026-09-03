#!/usr/bin/env python3
import argparse
import csv
import shlex
from datetime import date, datetime, timedelta
from pathlib import Path


def parse_date(value: str) -> date:
    return datetime.strptime(value, "%Y-%m-%d").date()


def load_dates(path: Path) -> set[date]:
    if path.suffix.lower() != ".csv":
        raise RuntimeError("STEP 30A historical source must be CSV")

    result: set[date] = set()
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames or "date" not in reader.fieldnames:
            raise RuntimeError("CSV must contain a 'date' column")
        for row in reader:
            raw = (row.get("date") or "").strip()
            if not raw:
                continue
            result.add(parse_date(raw))
    return result


def require_contiguous(dates: set[date], start: date, final_open: date) -> None:
    missing: list[str] = []
    current = start
    while current <= final_open:
        if current not in dates:
            missing.append(current.isoformat())
            if len(missing) >= 8:
                break
        current += timedelta(days=1)
    if missing:
        suffix = " ..." if len(missing) >= 8 else ""
        raise RuntimeError(
            "Historical source has missing whole-market dates in requested replay range: "
            + ", ".join(missing) + suffix
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Resolve a contiguous CSV historical replay window with warmup and final execution open."
    )
    parser.add_argument("--historical-data", required=True)
    parser.add_argument("--start-date", help="First measured decision close (YYYY-MM-DD). Default: latest feasible window")
    parser.add_argument("--days", type=int, default=5, help="Measured decision days")
    parser.add_argument("--warmup-days", type=int, default=30, help="Decision days before measured window")
    args = parser.parse_args()

    if args.days <= 0:
        raise RuntimeError("--days must be positive")
    if args.warmup_days < 0:
        raise RuntimeError("--warmup-days cannot be negative")

    path = Path(args.historical_data).resolve()
    if not path.is_file():
        raise RuntimeError(f"Historical source does not exist: {path}")

    dates = load_dates(path)
    if not dates:
        raise RuntimeError("Historical source contains no dates")

    source_first = min(dates)
    source_last = max(dates)

    if args.start_date:
        measure_start = parse_date(args.start_date)
    else:
        # Need measured 'days' closes plus one next-day execution open.
        measure_start = source_last - timedelta(days=args.days)

    measure_end = measure_start + timedelta(days=args.days - 1)
    replay_start = measure_start - timedelta(days=args.warmup_days)
    final_open = measure_end + timedelta(days=1)

    if replay_start < source_first:
        raise RuntimeError(
            f"Not enough warmup history: replay would start {replay_start}, source starts {source_first}"
        )
    if final_open > source_last:
        raise RuntimeError(
            f"Missing final T+1 open: need {final_open}, source ends {source_last}"
        )

    require_contiguous(dates, replay_start, final_open)

    expected_cycles = (measure_end - replay_start).days + 1
    print(f"HISTORICAL_DATA={shlex.quote(str(path))}")
    print(f"SOURCE_FIRST_DATE={source_first.isoformat()}")
    print(f"SOURCE_LAST_DATE={source_last.isoformat()}")
    print(f"REPLAY_START_DATE={replay_start.isoformat()}")
    print(f"MEASURE_START_DATE={measure_start.isoformat()}")
    print(f"MEASURE_END_DATE={measure_end.isoformat()}")
    print(f"REPLAY_END_DATE={measure_end.isoformat()}")
    print(f"FINAL_OPEN_DATE={final_open.isoformat()}")
    print(f"EXPECTED_CYCLES={expected_cycles}")
    print(f"MEASURE_DAYS={args.days}")
    print(f"WARMUP_DAYS={args.warmup_days}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"HISTORICAL_WINDOW_ERROR={exc}", flush=True)
        raise SystemExit(2)
