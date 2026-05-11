# ---- Import global dependencies ----
nlohmann_json_dep          = global_deps['nlohmann_json_dep']
json_schema_validator_dep  = global_deps['json_schema_validator_dep']
log4cpp_dep                 = global_deps['log4cpp_dep']
boost_dep                  = global_deps['boost_dep']
fmt_dep                   = global_deps['fmt_dep']
sqlite3_dep                = global_deps['sqlite3_dep']

# ---- Bring in subdirectories ----
subdir('common_types')
subdir('utils')
subdir('data_types')
subdir('backtest')
subdir('strategy')
subdir('ranker')
subdir('indicator')
subdir('universe')

# ---- Include directories for all submodules ----
include_dir = include_directories(
    'common_types',
    'utils',
    'data_types',
    'backtest',
    'strategy',
    'strategy/strategies',
    'ranker',
    'indicator',
    'universe'
)

# ---- Aggregate all sources from submodules ----
lib_sources = []
lib_sources += common_types_sources
lib_sources += utils_sources
lib_sources += data_types_sources
lib_sources += backtest_sources
lib_sources += strategy_sources
lib_sources += ranker_sources
lib_sources += indicator_sources
lib_sources += universe_sources



# ---- Create the library ----
libalgolib = library(
    'algolib',
    lib_sources,
    include_directories: include_dir,
    dependencies: [
        log4cpp_dep,
        fmt_dep,
        nlohmann_json_dep,
        json_schema_validator_dep,
        boost_dep,
        sqlite3_dep
    ]
)

# ---- Export dependency for other parts of project ----
libalgolib_dep = declare_dependency(
    include_directories: include_dir,
    link_with: libalgolib,
    dependencies: [
        log4cpp_dep,
        fmt_dep,
        nlohmann_json_dep,
        json_schema_validator_dep,
        boost_dep,
        sqlite3_dep
    ]
)
