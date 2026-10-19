# Build the execution-state-service executable and its local components.
execution_state_dependencies = [
    libalgolib_dep,
    global_deps['nlohmann_json_dep'],
    global_deps['curl_dep']
]

# Everything except main(), shared by the executable and tests/.
execution_state_core = static_library(
    'execution_state_core',
    ['equity_history.cpp', 'manual_requests.cpp', 'binance_prices.cpp', 'position_stops.cpp'],
    dependencies: execution_state_dependencies
)
execution_state_core_dep = declare_dependency(
    include_directories: include_directories('.'),
    link_with: execution_state_core,
    dependencies: execution_state_dependencies
)

execution_state_service_exe = executable(
    'algotrading_execution_state_service',
    'execution_state_service_main.cpp',
    dependencies: execution_state_core_dep
)
