#!/usr/bin/env bash
set -euo pipefail

COMMIT="89eb80cb440d44c31a762c402074871d56610190"
RUNTIME="research/src/legacy/runtime"

if [[ ! -d .git || ! -d "$RUNTIME/lib/src" ]]; then
    echo "ERROR: ejecuta esto desde la raíz de algoTrading después de install_frozen_legacy_runtime.sh"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "============================================================"
echo "FIX LEGACY RUNTIME STRATEGIES"
echo "============================================================"

# The first installer selected lib/src/strategy, but Meson shows that the
# historical runtime expects the strategies include directory explicitly.
# Re-extract the exact strategy subtree from the frozen commit.
if git ls-tree -r --name-only "$COMMIT" lib/src/strategy/strategies | grep -q .; then
    echo "[1/3] Extrayendo lib/src/strategy/strategies desde el commit histórico..."
    git archive "$COMMIT" -- lib/src/strategy/strategies | tar -x -C "$TMP"
    rm -rf "$RUNTIME/lib/src/strategy/strategies"
    mkdir -p "$RUNTIME/lib/src/strategy"
    mv "$TMP/lib/src/strategy/strategies" "$RUNTIME/lib/src/strategy/"
else
    echo "ERROR: el commit histórico no contiene lib/src/strategy/strategies"
    echo
    echo "Contenido de lib/src/strategy en ese commit:"
    git ls-tree -r --name-only "$COMMIT" lib/src/strategy | sed -n '1,120p'
    exit 1
fi

# Nested historical Meson files are documentation only; the frozen runtime
# is wired by one generated Meson file.
find "$RUNTIME/lib/src" -name meson.build -type f -delete

echo "[2/3] Regenerando source/include lists del runtime a partir del snapshot real..."

python3 - <<'PY'
from pathlib import Path
import re

runtime = Path("research/src/legacy/runtime")
meson_path = runtime / "meson.build"

old = meson_path.read_text()

# Keep the dependency declarations and library/dependency wiring, but replace
# include/source blocks with lists derived from the actual frozen tree.
cpps = sorted(
    p.relative_to(runtime).as_posix()
    for p in (runtime / "lib/src").rglob("*.cpp")
)

header_dirs = sorted({
    p.parent.relative_to(runtime).as_posix()
    for pattern in ("*.h", "*.hpp", "*.hh", "*.hxx")
    for p in (runtime / "lib/src").rglob(pattern)
})

if not cpps:
    raise SystemExit("ERROR: no hay .cpp en el runtime legacy")

if "lib/src/strategy/strategies" not in header_dirs:
    raise SystemExit("ERROR: strategies existe pero no contiene headers detectables")

def meson_list(name, items, kind):
    if kind == "include":
        lines = [f"{name} = include_directories("]
    else:
        lines = [f"{name} = files("]
    lines += [f"    '{x}'," for x in items]
    lines.append(")")
    return "\n".join(lines)

inc_block = meson_list("legacy_runtime_inc", header_dirs, "include")
src_block = meson_list("legacy_runtime_sources", cpps, "files")

old = re.sub(
    r"legacy_runtime_inc = include_directories\(\n.*?\n\)",
    inc_block,
    old,
    flags=re.S,
)
old = re.sub(
    r"legacy_runtime_sources = files\(\n.*?\n\)",
    src_block,
    old,
    flags=re.S,
)

meson_path.write_text(old)

print(f"include dirs: {len(header_dirs)}")
for d in header_dirs:
    print("  INC", d)

print(f"runtime cpp: {len(cpps)}")
for s in cpps:
    print("  SRC", s)
PY

echo
echo "[3/3] Verificando estrategias esperadas..."

find "$RUNTIME/lib/src/strategy/strategies" -maxdepth 1 -type f -printf '%f\n' | sort | sed -n '1,120p'

echo
echo "============================================================"
echo "FIX APLICADO"
echo "============================================================"
echo
echo "Ahora ejecuta:"
echo
echo "  rm -rf build-research-check"
echo "  meson setup build-research-check"
echo "  meson compile -C build-research-check"
echo
echo "No se ha compilado nada automáticamente."
