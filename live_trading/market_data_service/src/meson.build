# Build the market-data-service executable and its local components.
market_data_dependencies = [
    libalgolib_dep,
    global_deps['log4cpp_dep'],
    global_deps['nlohmann_json_dep'],
    global_deps['sqlite3_dep'],
    global_deps['curl_dep'],
    global_deps['fmt_dep']
]

# Everything except main(), shared by the executable and tests/.
market_data_core = static_library(
    'market_data_core',
    ['config.cpp', 'market_store.cpp', 'binance_client.cpp', 'ingestion.cpp', 'update_publisher.cpp'],
    dependencies: market_data_dependencies
)
market_data_core_dep = declare_dependency(
    include_directories: include_directories('.'),
    link_with: market_data_core,
    dependencies: market_data_dependencies
)

market_data_service_exe = executable(
    'algotrading_market_data_service',
    'market_data_service_main.cpp',
    dependencies: market_data_core_dep
)
