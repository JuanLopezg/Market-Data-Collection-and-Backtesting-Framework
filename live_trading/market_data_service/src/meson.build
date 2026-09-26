sources = [
    'market_data_service_main.cpp',
    'market_data_config.cpp',
    'market_data_store.cpp',
    'binance_market_data_client.cpp',
    'market_data_ingestor.cpp',
    'market_data_update_publisher.cpp'
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
