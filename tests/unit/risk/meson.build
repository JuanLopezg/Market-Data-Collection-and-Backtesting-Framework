test('unit_risk',
    executable('test_unit_risk',
        files('sizing_covariance_test.cpp', 'rebalance_portfolio_test.cpp',
              'portfolio_risk_engine_test.cpp'),
        dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest')
