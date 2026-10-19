# Real-venue code of the exchange gateway (tests/specs/exchange_gateway.md).
# Fake HTTP transport only: no request ever reaches a venue.
test('unit_exchange_gateway',
    executable('test_unit_exchange_gateway',
               files('symbol_map_test.cpp', 'kraken_protocol_test.cpp',
                     'live_venue_adapter_test.cpp', 'venue_journal_test.cpp'),
               dependencies: [test_support_dep, exchange_gateway_venue_dep]),
    suite: ['unit'], env: test_env, protocol: 'gtest')
