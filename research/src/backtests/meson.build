backtests_inc = include_directories('.')

# Fast backtest used by research/replay.py fast (and the PAPER baseline study).
executable(
    'algotrading_research',
    ['backtesting_main.cpp', '../common/paper_baseline.cpp'] + chain_profile_sources + backtest_core_sources,
    include_directories: [
        backtests_inc,
        research_src_inc,
        research_common_inc,
    ],
    dependencies: [
        libalgolib_dep,
    ],
)

# Single-strategy studies with metrics/HTML reports and parameter-sensitivity grids.
executable(
    'algotrading_research_html',
    ['backtesting_metrics_main_html_reports.cpp'] + backtest_report_sources + sensitivity_study_sources,
    include_directories: [backtests_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_btc_ma',
    ['btc_moving_average.cpp'] + backtest_report_sources + sensitivity_study_sources,
    include_directories: [backtests_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_initial_params',
    ['initial_strategy_parameters.cpp'] + backtest_report_sources + sensitivity_study_sources,
    include_directories: [backtests_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_xhbreakout',
    ['testing_xhbreakout.cpp'] + backtest_report_sources + sensitivity_study_sources,
    include_directories: [backtests_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

# Eight scenario strategies over several datasets, with correlation reports.
executable(
    'algotrading_research_multi_strategy',
    ['multi_strategy_main.cpp', '../common/scenario_reports.cpp', '../common/plotly_chart_html.cpp', '../common/backtest_helpers.cpp', '../common/backtest_metrics.cpp'],
    include_directories: [backtests_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)

executable(
    'algotrading_research_stats',
    ['stats.cpp'] + backtest_report_sources,
    include_directories: [backtests_inc, research_common_inc],
    dependencies: [libalgolib_dep],
)
