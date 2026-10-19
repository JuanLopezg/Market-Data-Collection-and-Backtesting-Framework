# The trading chain end to end (tests/specs/chain.md). Needs Docker; several minutes.
test('chain_paper',
    executable('test_chain_paper', files('chain_harness.cpp', 'paper_chain_test.cpp'),
               dependencies: test_support_dep),
    suite: ['chain'], env: test_env, protocol: 'gtest', depends: service_depends,
    timeout: 1800, is_parallel: false)
