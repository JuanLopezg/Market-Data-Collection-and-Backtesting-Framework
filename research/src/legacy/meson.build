legacy_inc = include_directories('.')

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
# CURRENT metrics/HTML reporting (PureRSI and research-only breakout studies).
# ------------------------------------------------------------------
executable(
    'algotrading_research_html',
    ['backtesting_metrics_main_html_reports.cpp'] + backtest_report_sources,
    include_directories: [legacy_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_btc_ma',
    ['btc_moving_average.cpp'] + backtest_report_sources,
    include_directories: [legacy_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_initial_params',
    ['initial_strategy_parameters.cpp'] + backtest_report_sources,
    include_directories: [legacy_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_multi_strategy',
    ['multi_strategy_main.cpp', '../common/scenario_reports.cpp', '../common/backtest_helpers.cpp', '../common/backtest_metrics.cpp'],
    include_directories: [legacy_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_stats',
    ['stats.cpp'] + backtest_report_sources,
    include_directories: [legacy_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_xhbreakout',
    ['testing_xhbreakout.cpp'] + backtest_report_sources,
    include_directories: [legacy_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)
