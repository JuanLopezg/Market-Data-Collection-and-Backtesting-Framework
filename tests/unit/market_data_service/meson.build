test('unit_market_data_service',
    executable('test_unit_market_data_service',
        files('config_test.cpp', 'market_store_test.cpp', 'funding_store_test.cpp',
              'binance_client_test.cpp', 'ingestion_test.cpp'),
        dependencies: [test_support_dep, market_data_core_dep, dependency('threads')]),
    suite: ['unit'], env: test_env, protocol: 'gtest')

test('unit_market_data_service_publisher',
    executable('test_unit_market_data_service_publisher',
        files('update_publisher_test.cpp'),
        dependencies: [test_support_dep, market_data_core_dep]),
    suite: ['service'], env: test_env, protocol: 'gtest',
    depends: service_depends, timeout: 300)
