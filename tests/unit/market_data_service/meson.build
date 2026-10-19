# Replace only the external HTTP transport; this also runs in sandboxes that deny
# loopback sockets. Keep these wrappers out of the Docker NATS publisher target.
market_data_curl_wrappers = [
    'curl_easy_init', 'curl_easy_cleanup', 'curl_easy_setopt',
    'curl_easy_perform', 'curl_easy_getinfo',
    'curl_multi_init', 'curl_multi_cleanup', 'curl_multi_setopt',
    'curl_multi_add_handle', 'curl_multi_remove_handle', 'curl_multi_perform',
    'curl_multi_info_read', 'curl_multi_poll'
]
market_data_curl_link_args = []
foreach function_name : market_data_curl_wrappers
    market_data_curl_link_args += '-Wl,--wrap=' + function_name
endforeach

test('unit_market_data_service',
    executable('test_unit_market_data_service',
        files('config_test.cpp', 'market_store_test.cpp', 'funding_store_test.cpp',
              'binance_client_test.cpp', 'ingestion_test.cpp', 'fake_binance_http.cpp'),
        dependencies: [test_support_dep, market_data_core_dep],
        link_args: market_data_curl_link_args),
    suite: ['unit'], env: test_env, protocol: 'gtest')

test('unit_market_data_service_publisher',
    executable('test_unit_market_data_service_publisher',
        files('update_publisher_test.cpp'),
        dependencies: [test_support_dep, market_data_core_dep]),
    suite: ['service'], env: test_env, protocol: 'gtest',
    depends: service_depends, timeout: 300)
