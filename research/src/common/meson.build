research_common_inc = include_directories('.')

backtest_core_sources = files(
    'backtest_helpers.cpp',
    'realtest.cpp',
)

backtest_report_sources = files(
    'backtest_metrics.cpp',
    'backtest_html_report.cpp',
)

# The deployed chain profile (strategy/portfolio JSON) built in process.
chain_profile_sources = files('chain_profile.cpp')

# Parameter-sensitivity sweep and study driver of the single-strategy studies.
sensitivity_study_sources = files(
    'parameter_sweep.cpp',
    'sensitivity_study.cpp',
)
