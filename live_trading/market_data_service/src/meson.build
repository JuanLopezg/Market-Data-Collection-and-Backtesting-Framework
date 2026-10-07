# Build the market-data-service executable and its local components.
sources = [
    'market_data_service_main.cpp',
    'config.cpp',
    'market_store.cpp',
    'binance_client.cpp',
    'ingestion.cpp',
    'update_publisher.cpp'
]

executable(
    'algotrading_market_data_service',
    sources,
    include_directories: include_directories('.'),
    dependencies: [
        libalgolib_dep,
        global_deps['log4cpp_dep'],
        global_deps['nlohmann_json_dep'],
        global_deps['sqlite3_dep'],
        global_deps['curl_dep'],
        global_deps['fmt_dep']
    ]
)
