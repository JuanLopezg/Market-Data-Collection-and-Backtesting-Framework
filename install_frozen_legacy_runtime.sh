#!/usr/bin/env bash
set -euo pipefail

LEGACY_COMMIT="89eb80cb440d44c31a762c402074871d56610190"
RUNTIME="research/src/legacy/runtime"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -d .git || ! -f research/src/legacy/meson.build ]]; then
    echo "ERROR: ejecuta este script desde la raíz de algoTrading."
    exit 1
fi

if ! git cat-file -e "${LEGACY_COMMIT}^{commit}" 2>/dev/null; then
    echo "ERROR: no existe el commit legacy ${LEGACY_COMMIT} en este clon."
    exit 1
fi

echo "============================================================"
echo "INSTALL LEGACY RESEARCH RUNTIME"
echo "snapshot=${LEGACY_COMMIT}"
echo "============================================================"
echo
echo "Este fix NO toca canonical ni lib/ moderno."
echo "Congela el runtime histórico dentro de research/src/legacy/runtime/."
echo

# -------------------------------------------------------------------
# 1. Materialize the exact historical runtime/support sources.
# -------------------------------------------------------------------
echo "[1/4] Extrayendo snapshot histórico..."

git archive "$LEGACY_COMMIT" -- \
    lib/src/common_types \
    lib/src/utils \
    lib/src/data_types \
    lib/src/backtest \
    lib/src/strategy \
    lib/src/ranker \
    lib/src/indicator \
    lib/src/universe \
    lib/src/filter \
    research/src/backtest_helpers.cpp \
    research/src/backtest_helpers.h \
    research/src/backtest_metrics.cpp \
    research/src/backtest_metrics.h \
    research/src/backtest_html_report.cpp \
    research/src/backtest_html_report.h \
    | tar -x -C "$TMP"

rm -rf "$RUNTIME"
mkdir -p "$RUNTIME/lib" "$RUNTIME/research_common"

mv "$TMP/lib/src" "$RUNTIME/lib/src"

for f in \
    backtest_helpers.cpp backtest_helpers.h \
    backtest_metrics.cpp backtest_metrics.h \
    backtest_html_report.cpp backtest_html_report.h
do
    mv "$TMP/research/src/$f" "$RUNTIME/research_common/$f"
done

# The old Meson files are useful to derive the exact source list but should
# not become nested build definitions in the frozen runtime.
find "$RUNTIME/lib/src" -name meson.build -type f -delete

cat > "$RUNTIME/README.md" <<EOF
# Frozen legacy research runtime

This directory is an intentional source snapshot used only by the historical
research executables in \`research/src/legacy/\`.

Source commit:

\`${LEGACY_COMMIT}\`

Reason:

The historical research mains were written against the pre-modernization
Strategy / BacktestContext API. They are compiled against this frozen runtime
instead of the current production/canonical library, so historical research
semantics are preserved while the whole repository remains buildable.

Do not use this runtime for canonical replay, dashboard, TESTNET or LIVE code.
EOF

# -------------------------------------------------------------------
# 2. Derive the exact source list from the Meson files at that commit.
# -------------------------------------------------------------------
echo "[2/4] Generando Meson del runtime congelado..."

python3 - "$LEGACY_COMMIT" "$RUNTIME" <<'PY'
from pathlib import Path
import re
import subprocess
import sys

commit = sys.argv[1]
runtime = Path(sys.argv[2])

modules = [
    "common_types",
    "utils",
    "data_types",
    "backtest",
    "strategy",
    "ranker",
    "indicator",
    "universe",
    "filter",
]

sources = []

for module in modules:
    meson_path = f"lib/src/{module}/meson.build"
    text = subprocess.check_output(
        ["git", "show", f"{commit}:{meson_path}"],
        text=True,
    )

    # Historical files use files('a.cpp', 'b.cpp', ...). We only extract
    # C++ translation units, preserving their declared order.
    for name in re.findall(r"""['"]([^'"]+\.cpp)['"]""", text):
        rel = Path("lib/src") / module / name
        full = runtime / rel
        if not full.is_file():
            raise SystemExit(
                f"ERROR: source declared by {meson_path} not found: {full}"
            )
        if str(rel) not in sources:
            sources.append(str(rel))

if not sources:
    raise SystemExit("ERROR: no legacy runtime sources discovered")

q = lambda s: "'" + s.replace("'", "\\'") + "'"

meson = []
meson.append("# Generated from historical commit " + commit)
meson.append("# Do not silently switch these sources to the modern lib.")
meson.append("")
meson.append("legacy_boost_dep = dependency(")
meson.append("    'boost',")
meson.append("    modules: ['filesystem', 'program_options'],")
meson.append("    required: true,")
meson.append(")")
meson.append("legacy_nlohmann_json_dep = dependency('nlohmann_json', required: true)")
meson.append("legacy_json_schema_validator_dep = dependency(")
meson.append("    'nlohmann_json_schema_validator',")
meson.append("    method: 'cmake',")
meson.append("    modules: ['nlohmann_json_schema_validator::validator'],")
meson.append("    required: true,")
meson.append(")")
meson.append("legacy_log4cpp_dep = dependency('log4cpp', required: true)")
meson.append("legacy_sqlite3_dep = dependency('sqlite3', required: true)")
meson.append("legacy_fmt_dep = dependency('fmt', required: true)")
meson.append("")
meson.append("legacy_external_deps = [")
meson.append("    legacy_log4cpp_dep,")
meson.append("    legacy_fmt_dep,")
meson.append("    legacy_nlohmann_json_dep,")
meson.append("    legacy_json_schema_validator_dep,")
meson.append("    legacy_boost_dep,")
meson.append("    legacy_sqlite3_dep,")
meson.append("]")
meson.append("")
meson.append("legacy_runtime_inc = include_directories(")
for d in [
    "lib/src/common_types",
    "lib/src/utils",
    "lib/src/data_types",
    "lib/src/backtest",
    "lib/src/strategy",
    "lib/src/strategy/strategies",
    "lib/src/ranker",
    "lib/src/indicator",
    "lib/src/universe",
    "lib/src/filter",
]:
    meson.append(f"    {q(d)},")
meson.append(")")
meson.append("")
meson.append("legacy_runtime_sources = files(")
for s in sources:
    meson.append(f"    {q(s)},")
meson.append(")")
meson.append("")
meson.append("legacy_algolib = static_library(")
meson.append("    'algolib_legacy',")
meson.append("    legacy_runtime_sources,")
meson.append("    include_directories: legacy_runtime_inc,")
meson.append("    dependencies: legacy_external_deps,")
meson.append(")")
meson.append("")
meson.append("legacy_algolib_dep = declare_dependency(")
meson.append("    include_directories: legacy_runtime_inc,")
meson.append("    link_with: legacy_algolib,")
meson.append("    dependencies: legacy_external_deps,")
meson.append(")")
meson.append("")
meson.append("legacy_research_common_inc = include_directories('research_common')")
meson.append("")
meson.append("legacy_research_support_sources = files(")
for s in [
    "research_common/backtest_helpers.cpp",
    "research_common/backtest_metrics.cpp",
    "research_common/backtest_html_report.cpp",
]:
    meson.append(f"    {q(s)},")
meson.append(")")
meson.append("")
meson.append("legacy_research_support = static_library(")
meson.append("    'research_legacy_support',")
meson.append("    legacy_research_support_sources,")
meson.append("    include_directories: [")
meson.append("        legacy_research_common_inc,")
meson.append("        legacy_runtime_inc,")
meson.append("    ],")
meson.append("    dependencies: [legacy_algolib_dep],")
meson.append(")")
meson.append("")
meson.append("legacy_research_dep = declare_dependency(")
meson.append("    include_directories: [")
meson.append("        legacy_research_common_inc,")
meson.append("        legacy_runtime_inc,")
meson.append("    ],")
meson.append("    link_with: legacy_research_support,")
meson.append("    dependencies: [legacy_algolib_dep],")
meson.append(")")
meson.append("")

(runtime / "meson.build").write_text("\n".join(meson))

print(f"legacy runtime C++ sources: {len(sources)}")
for src in sources:
    print("  ", src)
PY

# -------------------------------------------------------------------
# 3. Wire only the historical mains to the frozen runtime.
#    Keep algotrading_research on the current modern library.
# -------------------------------------------------------------------
echo
echo "[3/4] Actualizando research/src/legacy/meson.build..."

cat > research/src/legacy/meson.build <<'EOF'
legacy_inc = include_directories('.')

# ------------------------------------------------------------------
# Frozen pre-modernization runtime for historical research programs.
# ------------------------------------------------------------------
subdir('runtime')

# ------------------------------------------------------------------
# ACTIVE/current research backtester.
#
# This is intentionally compiled against the CURRENT library because
# research/replay.py fast and the accepted RealTest path depend on it.
# ------------------------------------------------------------------
executable(
    'algotrading_research',
    ['backtesting_main.cpp'] + backtest_core_sources,
    include_directories: [
        legacy_inc,
        research_src_inc,
        research_common_inc,
    ],
    dependencies: [
        libalgolib_dep,
    ],
)

# ------------------------------------------------------------------
# HISTORICAL research executables.
#
# These source files predate the modern Strategy/BacktestContext API.
# Compile them against the frozen runtime at commit 89eb80cb...
# ------------------------------------------------------------------
executable(
    'algotrading_research_html',
    ['backtesting_metrics_main_html_reports.cpp'],
    include_directories: [legacy_inc],
    dependencies: [legacy_research_dep],
)

executable(
    'algotrading_research_btc_ma',
    ['btc_moving_average.cpp'],
    include_directories: [legacy_inc],
    dependencies: [legacy_research_dep],
)

executable(
    'algotrading_research_initial_params',
    ['initial_strategy_parameters.cpp'],
    include_directories: [legacy_inc],
    dependencies: [legacy_research_dep],
)

executable(
    'algotrading_research_multi_strategy',
    ['multi_strategy_main.cpp'],
    include_directories: [legacy_inc],
    dependencies: [legacy_research_dep],
)

executable(
    'algotrading_research_stats',
    ['stats.cpp'],
    include_directories: [legacy_inc],
    dependencies: [legacy_research_dep],
)

executable(
    'algotrading_research_xhbreakout',
    ['testing_xhbreakout.cpp'],
    include_directories: [legacy_inc],
    dependencies: [legacy_research_dep],
)
EOF

# -------------------------------------------------------------------
# 4. Sanity checks only. Do not compile.
# -------------------------------------------------------------------
echo
echo "[4/4] Comprobaciones estructurales..."

test -f "$RUNTIME/lib/src/strategy/strategy.h"
test -f "$RUNTIME/lib/src/backtest/backtest_context.h"
test -f "$RUNTIME/research_common/backtest_metrics.cpp"
test -f "$RUNTIME/meson.build"

grep -Fq 'usesEntryOrders' "$RUNTIME/lib/src/strategy/strategy.h"
grep -Fq 'riskPerTrade_' "$RUNTIME/lib/src/strategy/strategy.h"

if grep -q "libalgolib_dep" research/src/legacy/meson.build \
   && grep -q "legacy_research_dep" research/src/legacy/meson.build; then
    echo "OK: modern y legacy quedan cableados por separado."
else
    echo "ERROR: no se detectó el cableado esperado."
    exit 1
fi

echo
echo "============================================================"
echo "LEGACY RUNTIME INSTALADO"
echo "============================================================"
echo "canonical / modern lib: SIN CAMBIOS"
echo "legacy runtime commit : $LEGACY_COMMIT"
echo
echo "Ahora ejecuta:"
echo
echo "  rm -rf build-research-check"
echo "  meson setup build-research-check"
echo "  meson compile -C build-research-check"
echo
echo "No se ha compilado nada automáticamente."
