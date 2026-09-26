#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import shutil
import sys

LEGACY_FILES = [
    "lib/src/contracts/clock_state.h",
    "lib/src/contracts/clock_control.h",
    "lib/src/contracts/clock_sync_request.h",
    "lib/src/runtime/clock.h",
    "lib/src/runtime/service_clock.h",
    "lib/src/runtime/runtime_mode.h",
]
LEGACY_DIRS = [
    "live_trading/replay_controller",
    "deploy/distributed_replay",
    "tools/distributed_compare",
]
EDIT_FILES = [
    "lib/src/transport/contract_json_codec.h",
    "lib/src/transport/contract_json_codec.cpp",
    "lib/src/transport/transport_subjects.h",
    "live_trading/meson.build",
]

FORBIDDEN_PATTERNS = [
    r"\bClockState\b",
    r"\bClockControl\b",
    r"\bClockSyncRequest\b",
    r"\bServiceClockContext\b",
    r"\bSimulatedClock\b",
    r"\bCLOCK_STATE\b",
    r"\bCLOCK_CONTROL\b",
    r"\bCLOCK_SYNC_REQUEST\b",
    r"simulation\.clock\.(?:state|control|sync)",
    r"service_clock\.h",
    r"runtime_mode\.h",
    r"clock_state\.h",
    r"clock_control\.h",
    r"clock_sync_request\.h",
]

SCAN_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp", ".cxx", ".build"}
SCAN_ROOTS = ["lib", "live_trading"]


def copy_any(src: pathlib.Path, dst: pathlib.Path) -> None:
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def restore_backup(root: pathlib.Path, backup: pathlib.Path) -> None:
    for rel in EDIT_FILES + LEGACY_FILES + LEGACY_DIRS:
        current = root / rel
        saved = backup / rel
        if current.is_dir():
            shutil.rmtree(current)
        elif current.exists():
            current.unlink()
        if saved.exists():
            copy_any(saved, current)


def require(path: pathlib.Path) -> None:
    if not path.is_file():
        raise RuntimeError(f"required current file missing: {path}")


def patch_codec_h(text: str) -> str:
    for header in ("clock_control.h", "clock_state.h", "clock_sync_request.h"):
        text = re.sub(rf'^#include\s+"{re.escape(header)}"\s*\n', '', text, flags=re.M)

    decls = [
        r"std::string encode\(const ClockState& value\);\s*",
        r"ClockState decodeClockState\(const std::string& payload\);\s*",
        r"std::string encode\(const ClockControl& value\);\s*",
        r"ClockControl decodeClockControl\(const std::string& payload\);\s*",
        r"std::string encode\(const ClockSyncRequest& value\);\s*",
        r"ClockSyncRequest decodeClockSyncRequest\(const std::string& payload\);\s*",
    ]
    for pat in decls:
        text = re.sub(pat, '', text)
    return text


def patch_codec_cpp(text: str) -> str:
    start = text.find("std::string encode(const ClockState& value)")
    marker = "std::string encode(const MarketDataReleaseRequest& value)"
    end = text.find(marker)
    if start >= 0:
        if end < 0 or end <= start:
            raise RuntimeError("cannot locate MarketDataReleaseRequest codec after ClockState codec")
        text = text[:start] + text[end:]
    return text


def patch_subjects(text: str) -> str:
    for name in ("CLOCK_STATE", "CLOCK_CONTROL", "CLOCK_SYNC_REQUEST"):
        text = re.sub(
            rf'^inline constexpr const char\* {name}\s*=\s*"[^"]+";\s*\n',
            '', text, flags=re.M,
        )

    pattern = re.compile(
        r"(?:^//[^\n]*\n)*"
        r"inline std::vector<std::string> runtimeSubjects\(\)\s*\{.*?^\}\s*\n",
        flags=re.M | re.S,
    )
    replacement = (
        "// Compatibility alias for non-clock services. Runtime subjects are purely\n"
        "// trading subjects; temporal authority is local TimeHandler configuration.\n"
        "inline std::vector<std::string> runtimeSubjects()\n"
        "{\n"
        "    return tradingRuntimeSubjects();\n"
        "}\n"
    )
    new_text, n = pattern.subn(replacement, text, count=1)
    if n == 0 and "runtimeSubjects()" in text:
        raise RuntimeError("could not safely rewrite runtimeSubjects()")
    return new_text


def patch_live_meson(text: str) -> str:
    text = re.sub(r"^\s*subdir\(['\"]replay_controller['\"]\)\s*\n", "", text, flags=re.M)
    return text


def scan_forbidden(root: pathlib.Path) -> list[str]:
    hits: list[str] = []
    compiled = [(p, re.compile(p)) for p in FORBIDDEN_PATTERNS]
    for relroot in SCAN_ROOTS:
        base = root / relroot
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if not p.is_file():
                continue
            if p.name != "meson.build" and p.suffix not in SCAN_SUFFIXES:
                continue
            try:
                text = p.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for _, rx in compiled:
                m = rx.search(text)
                if m:
                    line = text.count("\n", 0, m.start()) + 1
                    hits.append(f"{p.relative_to(root)}:{line}: {m.group(0)}")
                    break
    return sorted(hits)


def main() -> int:
    ap = argparse.ArgumentParser(description="T23 safe removal of legacy shared-clock code")
    ap.add_argument("--root", default=".")
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    root = pathlib.Path(args.root).resolve()
    for rel in EDIT_FILES:
        require(root / rel)

    plan = {
        "edit_in_place": EDIT_FILES,
        "delete_files": LEGACY_FILES,
        "delete_dirs": LEGACY_DIRS,
        "policy": "preserve current files; remove only legacy shared-clock declarations/code/graph entries",
    }
    if not args.apply:
        print(json.dumps(plan, indent=2))
        return 0

    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = root / "deploy/historical_replay/run" / f"t23_current_safe_backup_{stamp}"
    backup.mkdir(parents=True, exist_ok=True)
    for rel in EDIT_FILES + LEGACY_FILES + LEGACY_DIRS:
        src = root / rel
        if src.exists():
            copy_any(src, backup / rel)

    try:
        transforms = {
            "lib/src/transport/contract_json_codec.h": patch_codec_h,
            "lib/src/transport/contract_json_codec.cpp": patch_codec_cpp,
            "lib/src/transport/transport_subjects.h": patch_subjects,
            "live_trading/meson.build": patch_live_meson,
        }
        for rel, fn in transforms.items():
            p = root / rel
            before = p.read_text(encoding="utf-8")
            after = fn(before)
            p.write_text(after, encoding="utf-8")

        for rel in LEGACY_FILES:
            p = root / rel
            if p.exists():
                p.unlink()
        for rel in LEGACY_DIRS:
            p = root / rel
            if p.exists():
                shutil.rmtree(p)

        hits = scan_forbidden(root)
        if hits:
            raise RuntimeError(
                "legacy shared-clock references remain after safe cleanup:\n  " + "\n  ".join(hits)
            )

        if "subdir('replay_controller')" in (root / "live_trading/meson.build").read_text(encoding="utf-8"):
            raise RuntimeError("replay_controller still present in live_trading/meson.build")

    except Exception as exc:
        restore_backup(root, backup)
        print(f"FAIL: {exc}", file=sys.stderr)
        print(f"ROLLBACK: restored from {backup}", file=sys.stderr)
        return 1

    report = {
        "result": "PASS",
        "backup": str(backup),
        "edited_in_place": EDIT_FILES,
        "deleted_files": LEGACY_FILES,
        "deleted_dirs": LEGACY_DIRS,
        "rollback_on_cleanup_failure": True,
    }
    out = root / "deploy/historical_replay/run/t23_cleanup_summary.json"
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    print("PASS: T23 current-source-safe legacy shared-clock cleanup applied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
