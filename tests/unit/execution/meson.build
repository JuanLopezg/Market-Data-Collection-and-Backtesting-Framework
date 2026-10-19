test('unit_execution',
    executable('test_unit_execution',
        files(
            'order_manager_test.cpp',
            'execution_fills_test.cpp',
            'execution_commands_test.cpp',
            'execution_payload_test.cpp',
            'execution_conditional_test.cpp',
        ),
        dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest')
