#!/usr/bin/env python3
"""Normalize historical OHLCV CSV into global date,symbol order without loading it all in RAM."""
from __future__ import annotations
import argparse, csv, heapq, json, pathlib, shutil, tempfile

HEADER = ["date","symbol","open","high","low","close","volume"]

def key(row):
    return (row[0], row[1])

def flush_chunk(rows, tmpdir: pathlib.Path, idx: int) -> pathlib.Path:
    rows.sort(key=key)
    path = tmpdir / f"chunk_{idx:06d}.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        w=csv.writer(f, lineterminator="\n")
        w.writerows(rows)
    return path

def iter_chunk(path: pathlib.Path):
    with path.open(newline="", encoding="utf-8") as f:
        for row in csv.reader(f):
            yield (row[0], row[1], row)

def normalize(src: pathlib.Path, dst: pathlib.Path, chunk_rows: int) -> dict:
    if src.resolve() == dst.resolve():
        raise SystemExit("input and output must be different files")
    dst.parent.mkdir(parents=True, exist_ok=True)
    chunks=[]; rows=[]; total=0
    min_date=None; max_date=None
    with src.open(newline="", encoding="utf-8-sig") as f, tempfile.TemporaryDirectory(prefix="algotrading_csvsort_") as td:
        r=csv.reader(f)
        header=next(r, None)
        if header != HEADER:
            raise SystemExit(f"unexpected CSV header: {header!r}")
        tmpdir=pathlib.Path(td)
        for line_no,row in enumerate(r, start=2):
            if not row: continue
            if len(row)!=7: raise SystemExit(f"line {line_no}: expected 7 columns")
            date,symbol=row[0],row[1]
            if len(date)!=10 or date[4]!='-' or date[7]!='-': raise SystemExit(f"line {line_no}: invalid date {date!r}")
            if not symbol: raise SystemExit(f"line {line_no}: empty symbol")
            rows.append(row); total += 1
            min_date = date if min_date is None or date < min_date else min_date
            max_date = date if max_date is None or date > max_date else max_date
            if len(rows) >= chunk_rows:
                chunks.append(flush_chunk(rows,tmpdir,len(chunks))); rows=[]
        if rows: chunks.append(flush_chunk(rows,tmpdir,len(chunks)))
        if not chunks: raise SystemExit("historical CSV contains no data rows")
        iters=[iter_chunk(p) for p in chunks]
        with dst.open("w", newline="", encoding="utf-8") as out:
            w=csv.writer(out, lineterminator="\n"); w.writerow(HEADER)
            prev=None; written=0
            for date,symbol,row in heapq.merge(*iters):
                k=(date,symbol)
                if prev == k: raise SystemExit(f"duplicate date/symbol in historical CSV: {date}/{symbol}")
                if prev is not None and k < prev: raise SystemExit("internal sort invariant violated")
                w.writerow(row); prev=k; written += 1
        if written != total: raise SystemExit("row-count mismatch while normalizing historical CSV")
    return {"rows": total, "min_date": min_date, "max_date": max_date, "output": str(dst)}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--chunk-rows", type=int, default=100000)
    args=ap.parse_args()
    if args.chunk_rows <= 0: raise SystemExit("--chunk-rows must be positive")
    meta=normalize(pathlib.Path(args.input), pathlib.Path(args.output), args.chunk_rows)
    print(json.dumps(meta, sort_keys=True))
if __name__ == "__main__": main()
