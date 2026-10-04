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
