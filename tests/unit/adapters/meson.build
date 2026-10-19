test('unit_adapters',
    executable('test_unit_adapters',
        files('message_exchange_test.cpp', 'simulated_exchange_test.cpp',
              'canonical_adapter_test.cpp'),
        dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest')

test('service_adapters_backend',
    executable('test_unit_adapters_backend', files('backend_gateway_service_test.cpp'),
        dependencies: test_support_dep),
    suite: ['service'], timeout: 300, env: test_env, protocol: 'gtest')
