# Local standby of the deployed chain (see standby/README.md). The plan runs the same
# profile code as the comparison baseline (research/src/common/chain_profile.*).
executable(
    'algotrading_standby_plan',
    ['src/standby_plan_main.cpp'] + chain_profile_sources,
    include_directories: [research_common_inc],
    dependencies: [libalgolib_dep],
)
