# Build the exchange-gateway executable and its local components.
venue_sources = files(
    'venue/http_transport.cpp',
    'venue/kraken_futures_protocol.cpp',
    'venue/live_venue_adapter.cpp',
    'venue/symbol_map.cpp',
    'venue/venue_journal.cpp',
    'venue/venue_protocol.cpp',
)

venue_dependencies = [
    libalgolib_dep,
    global_deps['curl_dep'],
    global_deps['openssl_dep'],
    global_deps['sqlite3_dep'],
    global_deps['nlohmann_json_dep'],
    global_deps['fmt_dep'],
]

# Venue code shared by the executable, the offline adapter test and tests/.
exchange_gateway_venue = static_library(
    'exchange_gateway_venue',
    venue_sources,
    dependencies: venue_dependencies
)
exchange_gateway_venue_dep = declare_dependency(
    include_directories: include_directories('.', 'venue'),
    link_with: exchange_gateway_venue,
    dependencies: venue_dependencies
)

exchange_gateway_exe = executable(
    'algotrading_exchange_gateway',
    'exchange_gateway_main.cpp',
    dependencies: exchange_gateway_venue_dep
)

# Offline venue adapter tests: recorded responses only, never a network request.
executable(
    'algotrading_venue_adapter_test',
    'venue/venue_adapter_test.cpp',
    dependencies: exchange_gateway_venue_dep
)
