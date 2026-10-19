test('unit_runtime',
    executable('test_unit_runtime',
        files('decision_engine_test.cpp', 'trading_engine_test.cpp',
              'replay_runtime_test.cpp', 'manual_trading_test.cpp'),
        dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest', timeout: 120)
