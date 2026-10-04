# Frozen research runtime.
# Historical core: 89eb80cb440d44c31a762c402074871d56610190

legacy_runtime_inc = include_directories(
    'lib/src/common_types',
    'lib/src/utils',
    'lib/src/data_types',
    'lib/src/backtest',
    'lib/src/strategy',
    'lib/src/strategy/strategies',
    'lib/src/strategy/on_hold',
    'lib/src/ranker',
    'lib/src/indicator',
    'lib/src/universe',
    'lib/src/filter',
)

legacy_runtime_sources = files(
    'lib/src/common_types/logger.cpp',
    'lib/src/utils/json_utils.cpp',
    'lib/src/utils/time_utils.cpp',
    'lib/src/utils/database_utils.cpp',
    'lib/src/utils/csv_utils.cpp',
    'lib/src/data_types/data_types.cpp',
    'lib/src/backtest/backtest.cpp',
    'lib/src/backtest/backtest_store.cpp',
    'lib/src/backtest/backtest_context.cpp',
    'lib/src/strategy/strategy.cpp',
    'lib/src/ranker/ranker.cpp',
    'lib/src/ranker/indicator_ranker.cpp',
    'lib/src/indicator/indicator_spec.cpp',
    'lib/src/indicator/indicator_engine.cpp',
    'lib/src/indicator/indicator_calculators.cpp',
    'lib/src/universe/universe_selector.cpp',
    'lib/src/universe/liquidity_universe.cpp',
    'lib/src/filter/market_filter.cpp',
    'lib/src/filter/benchmark_above_sma_filter.cpp',
)

legacy_core_deps = [
    global_deps['boost_dep'],
    global_deps['nlohmann_json_dep'],
    global_deps['json_schema_validator_dep'],
    global_deps['log4cpp_dep'],
    global_deps['sqlite3_dep'],
    global_deps['fmt_dep'],
]

legacy_algolib = static_library(
    'algolib_legacy',
    legacy_runtime_sources,
    include_directories: legacy_runtime_inc,
    dependencies: legacy_core_deps,
)

legacy_algolib_dep = declare_dependency(
    include_directories: legacy_runtime_inc,
    link_with: legacy_algolib,
    dependencies: legacy_core_deps,
)

legacy_research_common_inc = include_directories('research_common')

legacy_research_support_sources = files(
    'research_common/backtest_helpers.cpp',
    'research_common/backtest_metrics.cpp',
    'research_common/backtest_html_report.cpp',
    'research_common/realtest.cpp',
)

legacy_research_extra_deps = [
    global_deps['boost_dep'],
    global_deps['log4cpp_dep'],
    global_deps['nlohmann_json_dep'],
    global_deps['json_schema_validator_dep'],
    global_deps['sqlite3_dep'],
    global_deps['curl_dep'],
    global_deps['fmt_dep'],
    global_deps['arrow_dep'],
    global_deps['parquet_dep'],
]

legacy_research_support = static_library(
    'research_legacy_support',
    legacy_research_support_sources,
    include_directories: [
        legacy_research_common_inc,
        legacy_runtime_inc,
    ],
    dependencies: [legacy_algolib_dep] + legacy_research_extra_deps,
)

legacy_research_dep = declare_dependency(
    include_directories: [
        legacy_research_common_inc,
        legacy_runtime_inc,
    ],
    link_with: legacy_research_support,
    dependencies: [legacy_algolib_dep] + legacy_research_extra_deps,
)
