#!/usr/bin/env python3
"""Keep two UTC days of diagnostics and trim oversized files without extra archives."""

import argparse
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
import re
import os
import stat
import sys
import fcntl


DEFAULT_ROOT = Path("/var/log/algotrading/services")
SERVICE = re.compile(r"algotrading\.[A-Za-z0-9_.-]+")
DAILY_FILE = re.compile(r"\d{4}-\d{2}-\d{2}\.log")
MAX_FILE_BYTES = 50 * 1024 * 1024
TRIM_MARKER = b"[LOG-TRIM: older diagnostic entries removed to enforce the file size limit]\n"


def validate_root(root: Path) -> None:
    if root.name != "services" or root.parent.name != "algotrading":
        raise ValueError("Expected a dedicated algotrading/services diagnostic directory")
    if root.is_symlink() or root.resolve() != root.absolute():
        raise ValueError("Diagnostic directory must not contain symlinks")


def diagnostic_files(root: Path):
    validate_root(root)
    if not root.exists():
        return
    for directory in root.iterdir():
        if directory.is_symlink() or not directory.is_dir() or not SERVICE.fullmatch(directory.name):
            continue
        for path in directory.iterdir():
            if path.is_symlink() or not path.is_file() or not DAILY_FILE.fullmatch(path.name):
                continue
            try:
                day = date.fromisoformat(path.stem)
            except ValueError:
                continue
            yield path, day


def clean(root: Path, today: date) -> int:
    cutoff = today - timedelta(days=1)
    removed = 0
    for path, day in diagnostic_files(root):
        if day < cutoff:
            path.unlink()
            removed += 1
    return removed


def trim_handle(handle, limit_bytes: int) -> None:
    """Keep complete recent lines and leave half the budget available for new logs."""
    info = os.fstat(handle.fileno())
    keep_bytes = limit_bytes // 2 - len(TRIM_MARKER)
    handle.seek(max(0, info.st_size - keep_bytes))
    tail = handle.read(keep_bytes)
    newline = tail.find(b"\n")
    tail = tail[newline + 1:] if newline >= 0 else b""
    handle.seek(0)
    handle.write(TRIM_MARKER + tail)
    handle.truncate()


def trim(root: Path, limit_bytes: int = MAX_FILE_BYTES) -> int:
    """Trim only diagnostic files; the receiver uses the same per-file lock."""
    if limit_bytes < 1024:
        raise ValueError("Diagnostic file limit must be at least 1024 bytes")
    trimmed = 0
    for path, _ in diagnostic_files(root):
        if path.stat().st_size < limit_bytes:
            continue
        descriptor = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "r+b", buffering=0) as handle:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
            info = os.fstat(handle.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size < limit_bytes:
                continue
            trim_handle(handle, limit_bytes)
            os.fsync(handle.fileno())
            trimmed += 1
    return trimmed


def append_record(root: Path, record: bytes, limit_bytes: int = MAX_FILE_BYTES) -> None:
    """Write rsyslog's already-redacted line, enforcing the daily file size first."""
    if limit_bytes < 1024:
        raise ValueError("Diagnostic file limit must be at least 1024 bytes")
    fields = record.split(b" ", 2)
    if len(fields) != 3 or not fields[1].startswith(b"service="):
        raise ValueError("Invalid diagnostic record header")
    day = date.fromisoformat(fields[0][:10].decode("ascii"))
    service = fields[1][len(b"service="):].decode("ascii")
    if not SERVICE.fullmatch(service) or b"\n" in record.rstrip(b"\n"):
        raise ValueError("Invalid diagnostic service or multiline record")
    # Validate the dedicated root even if it has no files yet.
    validate_root(root)
    root.mkdir(parents=True, exist_ok=True, mode=0o750)
    directory = root / service
    directory.mkdir(exist_ok=True, mode=0o750)
    if directory.is_symlink():
        raise ValueError("Diagnostic service directory must not be a symlink")
    path = directory / f"{day}.log"
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o640)
    with os.fdopen(descriptor, "r+b", buffering=0) as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        if not stat.S_ISREG(os.fstat(handle.fileno()).st_mode):
            raise ValueError("Diagnostic output must be a regular file")
        if len(record) > limit_bytes // 2:
            # A single huge diagnostic must not defeat the cap; keep its header
            # and mark truncation rather than placing partial UTF-8 on disk.
            record = fields[0] + b" " + fields[1] + b" [LOG-TRIM: oversized diagnostic record omitted]\n"
        if os.fstat(handle.fileno()).st_size + len(record) > limit_bytes:
            trim_handle(handle, limit_bytes)
        handle.seek(0, os.SEEK_END)
        handle.write(record)


def receive(root: Path, limit_bytes: int) -> int:
    """omprog confirmation protocol; one receiver owns append/trim ordering."""
    if limit_bytes < 1024:
        raise ValueError("Diagnostic file limit must be at least 1024 bytes")
    clean(root, datetime.now(timezone.utc).date())
    trim(root, limit_bytes)
    print("OK", flush=True)
    last_day = None
    for record in sys.stdin.buffer:
        try:
            day = date.fromisoformat(record[:10].decode("ascii"))
            if day != last_day:
                clean(root, day)
                last_day = day
            append_record(root, record, limit_bytes)
        except (OSError, ValueError):
            # Do not echo incoming diagnostics or filesystem exceptions with data.
            print("DAILY-LOG-WRITER: diagnostic write failed", file=sys.stderr, flush=True)
            print("FAIL", flush=True)
        else:
            print("OK", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--trim", action="store_true", help="Trim oversized diagnostic files")
    modes.add_argument("--receive", action="store_true", help="Receive redacted records from rsyslog")
    parser.add_argument("--max-bytes", type=int, default=MAX_FILE_BYTES)
    arguments = parser.parse_args()
    try:
        if arguments.receive:
            return receive(arguments.root, arguments.max_bytes)
        if arguments.trim:
            trimmed = trim(arguments.root, arguments.max_bytes)
            print(f"DAILY-LOG-CLEANUP: trimmed {trimmed} oversized diagnostic files")
            return 0
        removed = clean(arguments.root, datetime.now(timezone.utc).date())
        trimmed = trim(arguments.root, arguments.max_bytes)
    except (OSError, ValueError):
        print("DAILY-LOG-CLEANUP: ERROR: diagnostic directory or removal failed")
        return 1
    print(f"DAILY-LOG-CLEANUP: removed {removed} expired files; trimmed {trimmed} oversized files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
