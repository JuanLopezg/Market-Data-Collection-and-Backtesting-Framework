test('unit_accounting',
    executable('test_unit_accounting',
        files(
            'account_test.cpp',
            'position_ledger_test.cpp',
            'trade_recorder_test.cpp',
            'backtester_test.cpp',
        ),
        dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest')
