# Build the historical-market-data-service executable and its local components.
sources = [
    'historical_market_data_service_main.cpp',
    'historical_csv_source.cpp',
    '../../market_data_service/src/market_data_store.cpp'
]

executable(
    'algotrading_historical_market_data_service',
    sources,
    include_directories: include_directories('.', '../../market_data_service/src'),
    dependencies: [
        libalgolib_dep,
        global_deps['log4cpp_dep'],
        global_deps['nlohmann_json_dep'],
        global_deps['sqlite3_dep'],
        global_deps['fmt_dep']
    ]
)
