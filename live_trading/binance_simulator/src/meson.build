threads_dep = dependency('threads')

sources = [
    'binance_simulator_main.cpp',
    'historical_market_data.cpp',
    'binance_http_server.cpp'
]

executable(
    'algotrading_binance_simulator',
    sources,
    include_directories: include_directories('.'),
    dependencies: [
        global_deps['boost_dep'],
        global_deps['nlohmann_json_dep'],
        threads_dep
    ]
)
