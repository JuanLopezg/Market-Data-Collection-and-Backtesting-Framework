#!/usr/bin/env python3
"""Quick integrity check for the sensitivity report tool folder."""
from __future__ import annotations
import json
from pathlib import Path

root = Path(__file__).resolve().parent
required = [
    "sensitivity_report_main.py",
    "sensitivity_report_config.json",
    "requirements_sensitivity_report.txt",
    "README_sensitivity_report.md",
]
missing = [name for name in required if not (root / name).is_file()]
if missing:
    raise SystemExit("ERROR: missing files: " + ", ".join(missing))

script = (root / "sensitivity_report_main.py").read_text(encoding="utf-8")
if not script.startswith("#!/usr/bin/env python3") or "def main()" not in script:
    raise SystemExit("ERROR: sensitivity_report_main.py does not look like the Python report script.")

try:
    config = json.loads((root / "sensitivity_report_config.json").read_text(encoding="utf-8"))
except json.JSONDecodeError as exc:
    raise SystemExit(f"ERROR: sensitivity_report_config.json is not valid JSON: {exc}") from exc
if not isinstance(config, dict):
    raise SystemExit("ERROR: config root must be a JSON object.")

requirements = (root / "requirements_sensitivity_report.txt").read_text(encoding="utf-8").strip().splitlines()
if not all(any(line.startswith(package) for line in requirements) for package in ("pandas", "numpy", "matplotlib")):
    raise SystemExit("ERROR: requirements file is missing pandas, numpy, or matplotlib.")

print("OK: sensitivity_report_tool_v3 is complete and internally consistent.")
