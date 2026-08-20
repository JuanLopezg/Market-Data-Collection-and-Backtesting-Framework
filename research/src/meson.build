sources = [
    'backtest_helpers.cpp',
    'realtest.cpp',
    'backtest_html_report.cpp',
    'backtest_metrics.cpp',
    'backtesting_metrics_main_html_reports.cpp'
]

executable(
    'algotrading_research',
    sources,
    include_directories: include_directories('.'),
    dependencies: [
        libalgolib_dep,
        global_deps['boost_dep'],
        global_deps['log4cpp_dep'],
        global_deps['nlohmann_json_dep'],
        global_deps['json_schema_validator_dep'],
        global_deps['sqlite3_dep'],
        global_deps['curl_dep'],
        global_deps['fmt_dep'],
        global_deps['arrow_dep'],
        global_deps['parquet_dep']
    ]
)
