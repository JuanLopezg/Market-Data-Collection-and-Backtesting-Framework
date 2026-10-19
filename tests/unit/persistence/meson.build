test('unit_persistence',
    executable('test_unit_persistence',
        files('sqlite_state_store_test.cpp', 'reconciler_test.cpp', 'recovery_coordinator_test.cpp'),
        dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest')

test('unit_persistence_postgres',
    executable('test_unit_persistence_postgres',
        files('postgres_state_store_test.cpp'),
        dependencies: test_support_dep),
    suite: ['service'], env: test_env, protocol: 'gtest',
    depends: service_depends, timeout: 300)
