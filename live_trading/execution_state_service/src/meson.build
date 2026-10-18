# Build the execution-state-service executable and its local components.
executable(
    'algotrading_execution_state_service',
    [
        'execution_state_service_main.cpp',
        'equity_history.cpp',
        'manual_requests.cpp',
        'binance_prices.cpp',
        'position_stops.cpp',
    ],
    include_directories: include_directories('.'),
    dependencies: [
        libalgolib_dep,
        global_deps['nlohmann_json_dep'],
        global_deps['curl_dep']
    ]
)
