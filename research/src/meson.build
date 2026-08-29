sources = [
    'backtest_helpers.cpp',
    'realtest.cpp',
    'backtesting_main.cpp'
]

executable(
    'algotrading_research',
    sources,
    include_directories: include_directories('.'),
    dependencies: [
        libalgolib_dep
    ]
)

# Local deterministic live-style integration test. No exchange API/network is used.
executable(
    'algotrading_fake_live',
    ['fake_live_main.cpp'],
    include_directories: include_directories('.'),
    dependencies: [
        libalgolib_dep
    ]
)
