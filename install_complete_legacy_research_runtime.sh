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

for f in \
    lib/src/strategy/strategies/xHBreakout.h \
    lib/src/strategy/strategies/xHBreakout_atr.h \
    lib/src/strategy/strategies/donchianBreakout.h \
    lib/src/strategy/on_hold/atrBreakout.h \
    lib/src/strategy/on_hold/bargainChaser.h \
    lib/src/strategy/on_hold/mrRSILong.h \
    lib/src/strategy/on_hold/mrShort.h \
    lib/src/strategy/on_hold/pureMom.h
do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: falta $f"
        exit 1
    fi
done

echo "============================================================"
echo "INSTALL COMPLETE LEGACY RESEARCH RUNTIME"
echo "core snapshot: $LEGACY_COMMIT"
echo "============================================================"

rm -rf "$RUNTIME"
mkdir -p "$RUNTIME"

echo "[1/6] Extrayendo core histórico..."
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
    research/src/realtest.cpp \
    research/src/realtest.h \
    | tar -x -C "$TMP"

mkdir -p "$RUNTIME/lib" "$RUNTIME/research_common"
mv "$TMP/lib/src" "$RUNTIME/lib/src"

for f in \
    backtest_helpers.cpp backtest_helpers.h \
    backtest_metrics.cpp backtest_metrics.h \
    backtest_html_report.cpp backtest_html_report.h \
    realtest.cpp realtest.h
do
    mv "$TMP/research/src/$f" "$RUNTIME/research_common/$f"
done

# Nested Meson files from the historical tree are not used directly.
find "$RUNTIME/lib/src" -name meson.build -type f -delete

echo "[2/6] Copiando estrategias legacy que solo existían localmente..."
mkdir -p \
    "$RUNTIME/lib/src/strategy/strategies" \
    "$RUNTIME/lib/src/strategy/on_hold"

cp lib/src/strategy/strategies/xHBreakout.h \
   "$RUNTIME/lib/src/strategy/strategies/"
cp lib/src/strategy/strategies/xHBreakout_atr.h \
   "$RUNTIME/lib/src/strategy/strategies/"
cp lib/src/strategy/strategies/donchianBreakout.h \
   "$RUNTIME/lib/src/strategy/strategies/"

cp lib/src/strategy/on_hold/atrBreakout.h \
   "$RUNTIME/lib/src/strategy/on_hold/"
cp lib/src/strategy/on_hold/bargainChaser.h \
   "$RUNTIME/lib/src/strategy/on_hold/"
cp lib/src/strategy/on_hold/mrRSILong.h \
   "$RUNTIME/lib/src/strategy/on_hold/"
cp lib/src/strategy/on_hold/mrShort.h \
   "$RUNTIME/lib/src/strategy/on_hold/"
cp lib/src/strategy/on_hold/pureMom.h \
   "$RUNTIME/lib/src/strategy/on_hold/"

echo "[3/6] Creando PureRSI compatible con la API legacy..."

cat > "$RUNTIME/lib/src/strategy/strategies/pureRSI.h" <<'EOF'
#pragma once

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "data_types.h"
#include "indicator_engine.h"
#include "indicator_spec.h"
#include "logger.h"
#include "market_filter.h"
#include "ranker.h"
#include "strategy.h"
#include "universe_selector.h"

/*
 * Legacy research-only PureRSI.
 *
 * This class intentionally targets the pre-SignalState Strategy API used by
 * the historical research executables.  The production/current PureRSI stays
 * in lib/src/strategy/strategies/pureRSI.h and is not modified.
 *
 * Entry signal : RSI > rsiEntry
 * Entry price  : next available bar OPEN
 * Exit signal  : RSI < rsiExit
 * Exit price   : next available bar OPEN
 * Sizing       : strategy_allocation * riskPerTrade_
 */
class StrategyPureRSI final : public Strategy {
public:
    StrategyPureRSI(
        unsigned int maxPosOpen,
        double qtyFraction,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition,
        unsigned int rsiLength,
        double rsiEntry,
        double rsiExit,
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    )
        : Strategy(
              "Pure_RSI",
              maxPosOpen,
              qtyFraction,
              std::move(universeSelector),
              std::move(ranker),
              commissionEntryFactor,
              commissionExitFactor,
              maxRankingPosition,
              std::move(marketFilters)
          ),
          rsiEntry_(rsiEntry),
          rsiExit_(rsiExit),
          rsiSpec_{IndicatorKind::RSI, PriceField::Close, rsiLength}
    {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();
        specs.push_back(rsiSpec_);
        return specs;
    }

protected:
    bool shouldEnter(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        const std::vector<Trade>&,
        double,
        const IndicatorEngine& indicators
    ) const override
    {
        const BarData* bar = findBar(marketData, coin, ts);
        const double rsi = indicators.value(coin, ts, rsiSpec_);

        return
            bar &&
            bar->close > 0.0 &&
            std::isfinite(rsi) &&
            rsi > rsiEntry_;
    }

    Trade buildTrade(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        double strategy_allocation,
        bool live_trading,
        const IndicatorEngine&
    ) const override
    {
        (void)live_trading;

        Timestamp entryTs = 0;
        BarData entryBar;

        if (!findNextBar(marketData, coin, ts, entryTs, entryBar)) {
            LG_ERROR(
                "PureRSI: no next bar for {} at {}, using current close",
                coin,
                ts
            );

            const BarData* currentBar = findBar(marketData, coin, ts);
            if (!currentBar || currentBar->close <= 0.0)
                return invalidTrade();

            entryTs = ts;
            entryBar = *currentBar;
            entryBar.open = currentBar->close;
        }

        const double entryPrice =
            entryBar.open > 0.0 ? entryBar.open : entryBar.close;

        if (!std::isfinite(entryPrice) || entryPrice <= 0.0)
            return invalidTrade();

        const double tradeValue = strategy_allocation * riskPerTrade_;
        const double size = tradeValue / entryPrice;

        if (!std::isfinite(size) || size <= 0.0)
            return invalidTrade();

        Trade trade;
        trade.trade_id_ = ++last_trade_id;
        trade.start_ = entryTs;
        trade.end_ = 0;
        trade.coin_ = coin;
        trade.direction_ = Direction::Long;
        trade.entry_ = entryPrice;
        trade.exit_ = 0.0;
        trade.current_price_ = entryPrice;
        trade.size_ = size;
        trade.pnl_ = 0.0;
        trade.commission_ =
            entryPrice * size * commissionEntryFactor_;
        trade.sl_ = 0.0;
        trade.slReference_ = 0.0;
        trade.isSimulated_ = false;
        trade.exited_ = false;
        trade.barsHeld = 0;
        trade.strategy_name_ = strategy_name_;
        return trade;
    }

    void onBar(
        Trade& trade,
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        bool live_trading,
        const IndicatorEngine& indicators
    ) const override
    {
        (void)live_trading;

        const BarData* bar = findBar(marketData, coin, ts);
        if (!bar || ts < trade.start_)
            return;

        trade.current_price_ = bar->close;
        trade.pnl_ =
            (trade.current_price_ - trade.entry_) * trade.size_;
        ++trade.barsHeld;

        const double rsi = indicators.value(coin, ts, rsiSpec_);
        if (!std::isfinite(rsi) || rsi >= rsiExit_)
            return;

        Timestamp exitTs = 0;
        BarData exitBar;

        if (findNextBar(marketData, coin, ts, exitTs, exitBar)) {
            trade.exit_ =
                exitBar.open > 0.0 ? exitBar.open : exitBar.close;
            trade.end_ = exitTs;
        } else {
            trade.exit_ = bar->close;
            trade.end_ = ts;
        }

        trade.exited_ = true;
        trade.current_price_ = trade.exit_;
        trade.pnl_ =
            (trade.exit_ - trade.entry_) * trade.size_;
        trade.commission_ +=
            trade.exit_ * trade.size_ * commissionExitFactor_;
    }

private:
    const BarData* findBar(
        const MarketData& marketData,
        const Coin& coin,
        Timestamp ts
    ) const
    {
        const auto tsIt = marketData.find(ts);
        if (tsIt == marketData.end())
            return nullptr;

        const auto coinIt = tsIt->second.find(coin);
        if (coinIt == tsIt->second.end())
            return nullptr;

        return &coinIt->second;
    }

    bool findNextBar(
        const MarketData& marketData,
        const Coin& coin,
        Timestamp ts,
        Timestamp& nextTs,
        BarData& nextBar
    ) const
    {
        auto it = marketData.upper_bound(ts);
        while (it != marketData.end()) {
            const auto coinIt = it->second.find(coin);
            if (coinIt != it->second.end()) {
                nextTs = it->first;
                nextBar = coinIt->second;
                return true;
            }
            ++it;
        }
        return false;
    }

    Trade invalidTrade() const
    {
        Trade trade;
        trade.strategy_name_ = strategy_name_;
        trade.isSimulated_ = true;
        trade.exited_ = true;
        return trade;
    }

private:
    double rsiEntry_ = 80.0;
    double rsiExit_ = 70.0;
    IndicatorSpec rsiSpec_;
};
EOF

cat > "$RUNTIME/lib/src/strategy/strategies/all_strategies.h" <<'EOF'
#pragma once

// Complete strategy set required by historical research mains.
// These headers compile only against the frozen legacy Strategy API.

#include "pureRSI.h"
#include "xHBreakout.h"
#include "xHBreakout_atr.h"
#include "donchianBreakout.h"

#include "atrBreakout.h"
#include "bargainChaser.h"
#include "mrRSILong.h"
#include "mrShort.h"
#include "pureMom.h"
EOF

echo "[4/6] Generando Meson del runtime legacy..."

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

    for name in re.findall(r"""['"]([^'"]+\.cpp)['"]""", text):
        rel = Path("lib/src") / module / name
        full = runtime / rel
        if not full.is_file():
            raise SystemExit(
                f"ERROR: source histórico no encontrado: {full}"
            )
        item = rel.as_posix()
        if item not in sources:
            sources.append(item)

include_dirs = [
    "lib/src/common_types",
    "lib/src/utils",
    "lib/src/data_types",
    "lib/src/backtest",
    "lib/src/strategy",
    "lib/src/strategy/strategies",
    "lib/src/strategy/on_hold",
    "lib/src/ranker",
    "lib/src/indicator",
    "lib/src/universe",
    "lib/src/filter",
]

lines = []
lines += [
    "# Frozen research runtime.",
    f"# Historical core: {commit}",
    "",
    "legacy_runtime_inc = include_directories(",
]
for d in include_dirs:
    lines.append(f"    '{d}',")
lines += [
    ")",
    "",
    "legacy_runtime_sources = files(",
]
for s in sources:
    lines.append(f"    '{s}',")
lines += [
    ")",
    "",
    "legacy_core_deps = [",
    "    global_deps['boost_dep'],",
    "    global_deps['nlohmann_json_dep'],",
    "    global_deps['json_schema_validator_dep'],",
    "    global_deps['log4cpp_dep'],",
    "    global_deps['sqlite3_dep'],",
    "    global_deps['fmt_dep'],",
    "]",
    "",
    "legacy_algolib = static_library(",
    "    'algolib_legacy',",
    "    legacy_runtime_sources,",
    "    include_directories: legacy_runtime_inc,",
    "    dependencies: legacy_core_deps,",
    ")",
    "",
    "legacy_algolib_dep = declare_dependency(",
    "    include_directories: legacy_runtime_inc,",
    "    link_with: legacy_algolib,",
    "    dependencies: legacy_core_deps,",
    ")",
    "",
    "legacy_research_common_inc = include_directories('research_common')",
    "",
    "legacy_research_support_sources = files(",
    "    'research_common/backtest_helpers.cpp',",
    "    'research_common/backtest_metrics.cpp',",
    "    'research_common/backtest_html_report.cpp',",
    "    'research_common/realtest.cpp',",
    ")",
    "",
    "legacy_research_extra_deps = [",
    "    global_deps['boost_dep'],",
    "    global_deps['log4cpp_dep'],",
    "    global_deps['nlohmann_json_dep'],",
    "    global_deps['json_schema_validator_dep'],",
    "    global_deps['sqlite3_dep'],",
    "    global_deps['curl_dep'],",
    "    global_deps['fmt_dep'],",
    "    global_deps['arrow_dep'],",
    "    global_deps['parquet_dep'],",
    "]",
    "",
    "legacy_research_support = static_library(",
    "    'research_legacy_support',",
    "    legacy_research_support_sources,",
    "    include_directories: [",
    "        legacy_research_common_inc,",
    "        legacy_runtime_inc,",
    "    ],",
    "    dependencies: [legacy_algolib_dep] + legacy_research_extra_deps,",
    ")",
    "",
    "legacy_research_dep = declare_dependency(",
    "    include_directories: [",
    "        legacy_research_common_inc,",
    "        legacy_runtime_inc,",
    "    ],",
    "    link_with: legacy_research_support,",
    "    dependencies: [legacy_algolib_dep] + legacy_research_extra_deps,",
    ")",
    "",
]

(runtime / "meson.build").write_text("\n".join(lines))

print(f"Core legacy: {len(sources)} translation units")
PY

cat > "$RUNTIME/README.md" <<EOF
# Frozen legacy research runtime

Purpose: compile the historical research executables without changing the
modern Strategy/SignalState architecture used by canonical replay/live code.

Historical core source:
\`${LEGACY_COMMIT}\`

The breakout/older strategy headers were local/ignored files in this working
tree and therefore do not exist in Git history. Copies are frozen here for
legacy research only.

\`pureRSI.h\` in this runtime is a compatibility implementation for the old
Strategy API. The production/current PureRSI is not modified.

Do not use this runtime from canonical replay, dashboard, TESTNET or LIVE.
EOF

echo "[5/6] Cableando los ejecutables..."

cat > research/src/legacy/meson.build <<'EOF'
legacy_inc = include_directories('.')

# Frozen compatibility runtime used only by historical research programs.
subdir('runtime')

# ------------------------------------------------------------------
# CURRENT research/backtester.
# Remains on the modern production library.
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
# These intentionally use the isolated legacy runtime.
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

echo "[6/6] Verificaciones estructurales..."

grep -Fq 'riskPerTrade_' "$RUNTIME/lib/src/strategy/strategy.h"
grep -Fq 'class StrategyPureRSI' \
    "$RUNTIME/lib/src/strategy/strategies/pureRSI.h"
grep -Fq 'class StrategyXHBreakout' \
    "$RUNTIME/lib/src/strategy/strategies/xHBreakout.h"
grep -Fq 'class StrategyMRRSILong' \
    "$RUNTIME/lib/src/strategy/on_hold/mrRSILong.h"

echo
echo "============================================================"
echo "RUNTIME LEGACY COMPLETO INSTALADO"
echo "============================================================"
echo "No se ha compilado nada."
echo
echo "Ahora ejecuta:"
echo
echo "  rm -rf build-research-check"
echo "  meson setup build-research-check"
echo "  meson compile -C build-research-check"
