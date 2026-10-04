canonical_inc = include_directories('.')

executable(
    'algotrading_canonical_replay',
    ['canonical_replay.cpp'],
    include_directories: [
        canonical_inc,
        research_src_inc,
        research_common_inc,
    ],
    dependencies: [
        libalgolib_dep,
    ],
)
