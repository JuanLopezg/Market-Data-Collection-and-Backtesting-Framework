# Build the exchange-gateway executable and its local components.
venue_sources = files(
    'venue/http_transport.cpp',
    'venue/kraken_futures_protocol.cpp',
    'venue/live_venue_adapter.cpp',
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

executable(
    'algotrading_exchange_gateway',
    ['exchange_gateway_main.cpp'] + venue_sources,
    dependencies: venue_dependencies
)

# Offline venue adapter tests: recorded responses only, never a network request.
executable(
    'algotrading_venue_adapter_test',
    ['venue/venue_adapter_test.cpp'] + venue_sources,
    dependencies: venue_dependencies
)
