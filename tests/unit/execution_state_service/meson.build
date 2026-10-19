# Execution-state service internals (tests/specs/execution_state_service.md).
test('unit_execution_state_service',
    executable('test_unit_execution_state_service',
               files('equity_history_test.cpp', 'position_stops_test.cpp',
                     'manual_requests_test.cpp', 'binance_prices_test.cpp'),
               dependencies: [test_support_dep, execution_state_core_dep]),
    suite: ['unit'], env: test_env, protocol: 'gtest')
