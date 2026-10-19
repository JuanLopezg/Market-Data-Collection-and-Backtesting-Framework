test('unit_transport',
    executable('test_unit_transport',
        files('message_json_test.cpp', 'message_subjects_test.cpp'),
        dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest')

test('unit_transport_jetstream',
    executable('test_unit_transport_jetstream',
        files('jetstream_bus_test.cpp'),
        dependencies: test_support_dep),
    suite: ['service'], env: test_env, protocol: 'gtest',
    depends: service_depends, timeout: 300)
