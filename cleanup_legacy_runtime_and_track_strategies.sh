#!/usr/bin/env bash
set -euo pipefail

if [[ ! -d .git || ! -f research/src/legacy/meson.build ]]; then
    echo "ERROR: ejecuta este script desde la raíz de algoTrading."
    exit 1
fi

echo "==> 1. Eliminando el runtime legacy incompleto creado en el intento anterior..."
rm -rf research/src/legacy/runtime

echo "==> 2. Quitando strategies/ de .gitignore..."
python3 - <<'PY'
from pathlib import Path

p = Path(".gitignore")
s = p.read_text()

lines = s.splitlines()
new = []
removed = []

for line in lines:
    if line.strip() in {
        "lib/src/strategy/strategies/",
        "/lib/src/strategy/strategies/",
    }:
        removed.append(line)
        continue
    new.append(line)

p.write_text("\n".join(new) + ("\n" if s.endswith("\n") else ""))

if removed:
    print("Eliminado de .gitignore:")
    for x in removed:
        print("  ", x)
else:
    print("WARN: no encontré una línea exacta para strategies/; se usará git add -f igualmente.")
PY

echo "==> 3. Restaurando Meson legacy a una base limpia contra el runtime moderno..."
cat > research/src/legacy/meson.build <<'EOF'
legacy_inc = include_directories('.')

legacy_includes = [
    legacy_inc,
    research_src_inc,
    research_common_inc,
]

# Backtester/research principal activo.
executable(
    'algotrading_research',
    ['backtesting_main.cpp'] + backtest_core_sources,
    include_directories: legacy_includes,
    dependencies: [
        libalgolib_dep,
    ],
)

# Research históricos. Se mantienen como targets reales; los iremos
# adaptando a la API actual hasta que todos compilen.
executable(
    'algotrading_research_html',
    ['backtesting_metrics_main_html_reports.cpp']
        + backtest_core_sources
        + backtest_report_sources,
    include_directories: legacy_includes,
    dependencies: [
        libalgolib_dep,
    ],
)

executable(
    'algotrading_research_btc_ma',
    ['btc_moving_average.cpp'],
    include_directories: legacy_includes,
    dependencies: [
        libalgolib_dep,
    ],
)

executable(
    'algotrading_research_initial_params',
    ['initial_strategy_parameters.cpp'],
    include_directories: legacy_includes,
    dependencies: [
        libalgolib_dep,
    ],
)

executable(
    'algotrading_research_multi_strategy',
    ['multi_strategy_main.cpp'],
    include_directories: legacy_includes,
    dependencies: [
        libalgolib_dep,
    ],
)

executable(
    'algotrading_research_stats',
    ['stats.cpp'],
    include_directories: legacy_includes,
    dependencies: [
        libalgolib_dep,
    ],
)

executable(
    'algotrading_research_xhbreakout',
    ['testing_xhbreakout.cpp'],
    include_directories: legacy_includes,
    dependencies: [
        libalgolib_dep,
    ],
)
EOF

echo "==> 4. Añadiendo strategy/strategies al índice de Git..."
git add .gitignore
git add -f lib/src/strategy/strategies

echo
echo "============================================================"
echo "LIMPIEZA / TRACKING TERMINADOS"
echo "============================================================"
echo
echo "Estado relevante:"
git status --short -- \
    .gitignore \
    lib/src/strategy/strategies \
    research/src/legacy/meson.build \
    research/src/legacy/runtime || true

echo
echo "No se ha compilado ni hecho commit."
