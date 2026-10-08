#!/usr/bin/env python3
"""Generate local repository navigation from the current Git-visible working tree."""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
from urllib.parse import quote


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = {"lib", "live_trading", "research", "dashboard", "validation", "deploy", "config", "docs", "tools"}
ROOT_FILES = {"AGENTS.md", "README.md", "CURRENT_STATE.md", "meson.build", "meson_options", ".gitignore", "handoff.sh"}
EXCLUDED_PREFIXES = (
    "docs/venue/", "tools/historical_replay/", "deploy/historical_replay/run/",
    "research/src/legacy/runtime/", "lib/src/strategy/on_hold/",
)
EXCLUDED_DIRECTORIES = {
    ".git", ".ai", ".venv", "venv", "build", "builddir", "storage", "node_modules",
    "dist", ".runtime_bundle", "__pycache__", ".pytest_cache", ".cache", "logs", "on_hold",
}
LANGUAGES = {
    ".cpp": "C++", ".cc": "C++", ".h": "C++", ".hpp": "C++",
    ".py": "Python", ".go": "Go", ".ts": "TypeScript", ".tsx": "TypeScript",
    ".js": "JavaScript", ".jsx": "JavaScript", ".sh": "Shell",
    ".md": "Markdown", ".txt": "Text", ".json": "JSON", ".yaml": "YAML", ".yml": "YAML",
    ".sql": "SQL", ".html": "HTML", ".css": "CSS", ".toml": "TOML",
    ".conf": "Configuration", ".service": "Systemd", ".timer": "Systemd",
}
SPECIAL_FILES = {"meson.build": "Meson", "meson_options": "Meson", ".gitignore": "Git",
                 "Dockerfile": "Docker", "go.mod": "Go module", "go.sum": "Go module"}
GUIDES = (
    "AGENTS.md", "docs/codex/CONTEXT_FULL.txt", "docs/codex/ARCHITECTURE_FULL.md",
    "docs/README.md", "docs/ROADMAP.md", "CURRENT_STATE.md", "lib/README.md", "live_trading/README.md",
    "research/REPLAY.md", "dashboard/README.md", "validation/README.md", "validation/LOCAL_SERVICE_CAMPAIGN.md",
    "deploy/live/README.md", "deploy/historical_replay/README.md", "docs/codex/AI_INDEX.md",
)
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)
MESON_TARGET = re.compile(r"\b(executable|library|shared_library|static_library)\s*\(\s*['\"]([\w.-]+)['\"]")


def eligible(relative: str) -> bool:
    path = PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        return False
    if path.parts[0] not in SOURCE_ROOTS and relative not in ROOT_FILES:
        return False
    if any(part in EXCLUDED_DIRECTORIES for part in path.parts):
        return False
    if relative.startswith(EXCLUDED_PREFIXES):
        return False
    name = path.name.lower()
    # Also reject these when accidentally tracked or force-added to Git.
    if name.startswith(".env") or ".env" in name or name.endswith((".pem", ".key", ".p12", ".pfx")):
        return False
    if re.search(r"(?:^|[_.-])(credentials?|secrets?|tokens?|passwords?|id_rsa|id_ed25519)(?:[_.-]|$)", name):
        return False
    return path.suffix in LANGUAGES or path.name in SPECIAL_FILES


def visible_files(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    candidates = sorted({os.fsdecode(p) for p in result.stdout.split(b"\0") if p})
    # --exclude-standard applies to untracked files; exclude ignored tracked files too.
    ignored = subprocess.run(
        ["git", "check-ignore", "--no-index", "-z", "--stdin"], cwd=root,
        input=b"".join(os.fsencode(p) + b"\0" for p in candidates),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if ignored.returncode not in (0, 1):
        raise RuntimeError("Git ignore evaluation failed")
    ignored_paths = {os.fsdecode(p) for p in ignored.stdout.split(b"\0") if p}
    included = []
    for relative in candidates:
        if relative in ignored_paths or not eligible(relative):
            continue
        path = root / relative
        # Never read links/junctions, including links to another repository file.
        if path.is_symlink() or any(parent.is_symlink() for parent in path.parents if parent != root):
            continue
        if path.is_file() and path.resolve().is_relative_to(root):
            included.append(relative)
    return included


def component(relative: str) -> str:
    parts = PurePosixPath(relative).parts
    if len(parts) >= 3 and parts[:2] == ("lib", "src"):
        return "/".join(parts[:3])
    if len(parts) >= 4 and parts[:2] == ("research", "src"):
        return "/".join(parts[:3])
    if len(parts) >= 3 and parts[0] in {"live_trading", "deploy", "dashboard"}:
        return "/".join(parts[:2])
    return parts[0] if len(parts) > 1 else "repository"


def build_index(root: Path) -> dict:
    root = root.resolve()
    files = []
    contents = {}
    by_name = defaultdict(list)
    for relative in visible_files(root):
        raw = (root / relative).read_bytes().replace(b"\r\n", b"\n")
        if b"\0" in raw:
            continue
        language = SPECIAL_FILES.get(PurePosixPath(relative).name, LANGUAGES.get(PurePosixPath(relative).suffix))
        files.append({"path": relative, "component": component(relative), "language": language,
                      "sha256": hashlib.sha256(raw).hexdigest(), "includes": [], "include_candidates": [],
                      "included_by": []})
        if language in {"C++", "Meson"}:
            contents[relative] = raw.decode("utf-8", errors="replace")
        by_name[PurePosixPath(relative).name].append(relative)

    entries = {entry["path"]: entry for entry in files}
    targets = []
    for entry in files:
        relative = entry["path"]
        text = contents.get(relative, "")
        if entry["language"] == "C++":
            confirmed = set()
            ambiguous = set()
            for include in INCLUDE.findall(text):
                local = (root / PurePosixPath(relative).parent / include).resolve()
                local_relative = local.relative_to(root).as_posix() if local.is_relative_to(root) else None
                if local_relative in entries:
                    confirmed.add(local_relative)
                    continue
                # Basenames alone are unsafe when two domains have the same header.
                candidates = [p for p in by_name[PurePosixPath(include).name]
                              if p == include or p.endswith("/" + include)]
                if len(candidates) == 1:
                    confirmed.add(candidates[0])
                elif len(candidates) > 1:
                    ambiguous.update(candidates)
            entry["includes"] = sorted(confirmed)
            entry["include_candidates"] = sorted(ambiguous)
            for dependency in confirmed:
                entries[dependency]["included_by"].append(relative)
        elif entry["language"] == "Meson":
            # Preserve line numbers, but ignore commented declarations.
            uncommented = re.sub(r"(?m)^\s*#.*$", "", text)
            for match in MESON_TARGET.finditer(uncommented):
                targets.append({"name": match[2], "kind": match[1], "path": relative,
                                "line": uncommented[:match.start()].count("\n") + 1})
    for entry in files:
        entry["included_by"].sort()
    digest = hashlib.sha256()
    for entry in files:
        digest.update(f"{entry['path']}\0{entry['sha256']}\n".encode())
    return {"schema_version": 1, "source_fingerprint": digest.hexdigest(),
            "guides": [path for path in GUIDES if path in entries],
            "build_targets": sorted(targets, key=lambda t: (t["path"], t["line"])), "files": files}


def link(path: str) -> str:
    return f"[{path}](../{quote(path, safe='/')})"


def render(index: dict) -> dict[str, str]:
    groups = defaultdict(list)
    for entry in index["files"]:
        groups[entry["component"]].append(entry)
    readme = ["# Generated repository navigation", "",
              "Generated by `tools/generate_ai_index.py` from the current working tree. Do not edit these files.",
              "Source and current validation output are authoritative. Read the maintained guides below first.", "",
              "Refresh: `python3 tools/generate_ai_index.py`", "",
              "Check freshness: `python3 tools/generate_ai_index.py --check`", "",
              "Run these commands from the repository root under WSL. No build or network access is needed.", "",
              f"Source fingerprint: `{index['source_fingerprint']}`", "",
              "[Component/file map](components.md) groups source and guides by directory. "
              "[JSON index](index.json) adds normalized file hashes, literal Meson targets and C++ include links.", "",
              "## Maintained guides", ""]
    readme.extend(f"- {link(path)}" for path in index["guides"])
    readme.extend(["", "## Components", "", "| Directory | Indexed files |", "| --- | ---: |"])
    readme.extend(f"| `{name}` | {len(entries)} |" for name, entries in sorted(groups.items()))
    readme.extend(["", "## Scope and limits", "",
                   "Includes existing tracked and non-ignored untracked source, build definitions, configuration "
                   "and maintained guides. Deleted paths, credentials/environment files, symlinks, generated data, "
                   "dependency caches, frozen venue documents and historical diagnostic tools are excluded.", "",
                   "File bodies are not copied. C++ include links use same-directory resolution or unique suffix "
                   "matches; ambiguous matches stay in `include_candidates`. This is a navigation aid, not a "
                   "compiler dependency graph, call graph or validation claim. Meson targets include only literal "
                   "declarations; use Meson introspection for configured targets. Hashes normalize CRLF to LF.", "",
                   "This directory is local and ignored by Git. Maintained architecture, readiness and priorities "
                   "remain in the original guides; no trading or deployment settings come from this index.", ""])
    components = ["# Component/file map", "", "Generated navigation; read [.ai/README.md](README.md) first.", ""]
    for name, entries in sorted(groups.items()):
        components.extend([f"## {name}", ""])
        components.extend(f"- {link(entry['path'])} ({entry['language']})" for entry in entries)
        components.append("")
    return {"README.md": "\n".join(readme), "components.md": "\n".join(components),
            "index.json": json.dumps(index, indent=2, ensure_ascii=True) + "\n"}


def generate(root: Path, check: bool = False) -> bool:
    root = root.resolve()
    output = root / ".ai"
    if output.is_symlink() or output.resolve() != root / ".ai" or (output.exists() and not output.is_dir()):
        raise RuntimeError(".ai must be a real repository-local directory")
    expected = render(build_index(root))
    for name in expected:
        path = output / name
        if path.is_symlink() or (path.exists() and not path.is_file()):
            raise RuntimeError("Generated output paths must be regular files")
    stale = [name for name, text in expected.items()
             if not (output / name).is_file() or (output / name).read_bytes() != text.encode()]
    if check:
        print("AI-INDEX: stale/missing: " + ", ".join(stale) if stale else "AI-INDEX: PASS: current")
        return not stale
    output.mkdir(exist_ok=True)
    for name in stale:
        with tempfile.NamedTemporaryFile(dir=output, prefix=".index-", delete=False) as temporary:
            temporary.write(expected[name].encode())
            temporary_path = Path(temporary.name)
        try:
            temporary_path.replace(output / name)
        finally:
            temporary_path.unlink(missing_ok=True)
    print(f"AI-INDEX: generated {len(expected)} files; updated {len(stale)}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if generated navigation is missing or stale")
    arguments = parser.parse_args()
    try:
        return 0 if generate(ROOT, arguments.check) else 1
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        # Do not print subprocess output or source contents.
        print(f"AI-INDEX: ERROR: {type(error).__name__}; generation failed")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
