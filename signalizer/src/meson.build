sources = [
    'signalizer_main.cpp'
]

executable(
    'algotrading_signalizer',
    sources,
    include_directories: include_directories('.'),
    dependencies: [
        libalgolib_dep,
        global_deps['boost_dep'],
        global_deps['log4cpp_dep'],
        global_deps['nlohmann_json_dep'],
        global_deps['json_schema_validator_dep'],
        global_deps['sqlite3_dep'],
        global_deps['curl_dep'],
        global_deps['fmt_dep']
    ]
)
