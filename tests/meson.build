# Behaviour tests for lib/ and the C++ services that run on the VPS (tests/README.md).
#   meson test -C build --suite unit      fast, in-process
#   meson test -C build --suite service   components against Docker NATS/PostgreSQL
#   meson test -C build --suite chain     the whole trading chain over several days

gtest_dep = dependency('gtest_main')
gmock_dep = dependency('gmock')

test_support = static_library(
    'test_support',
    'support/test_support.cpp',
    dependencies: [gtest_dep]
)
test_support_dep = declare_dependency(
    include_directories: include_directories('support'),
    link_with: test_support,
    dependencies: [libalgolib_dep, gtest_dep, gmock_dep]
)

# Every test gets the source root and the built service executables.
service_executables = {
    'MARKET_DATA_SERVICE': market_data_service_exe,
    'HISTORICAL_MARKET_DATA_SERVICE': historical_market_data_service_exe,
    'STRATEGY_SERVICE': strategy_service_exe,
    'PORTFOLIO_RISK_SERVICE': portfolio_risk_service_exe,
    'ORDER_PLANNER_SERVICE': order_planner_service_exe,
    'EXECUTION_STATE_SERVICE': execution_state_service_exe,
    'EXCHANGE_GATEWAY': exchange_gateway_exe,
    'SIMULATED_EXCHANGE_SERVICE': simulated_exchange_service_exe,
    'BINANCE_SIMULATOR': binance_simulator_exe,
}
test_env = environment()
test_env.set('ALGOTRADING_SOURCE_ROOT', meson.project_source_root())
test_env.set('ALGOTRADING_LOG_QUIET', '1')
service_depends = []
foreach name, exe : service_executables
    test_env.set('ALGOTRADING_BIN_' + name, exe.full_path())
    service_depends += exe
endforeach

# One folder per work package, each with its own meson.build.
subdir('unit')
subdir('chain')
