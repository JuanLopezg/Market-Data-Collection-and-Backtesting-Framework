test('support_smoke',
    executable('test_support_smoke', 'support_smoke_test.cpp', dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest', timeout: 180)

test('unit_utils',
    executable('test_unit_utils', 'utils_behaviour_test.cpp', dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest')
