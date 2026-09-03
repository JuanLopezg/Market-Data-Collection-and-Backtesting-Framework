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
subdir('contracts')
subdir('transport')
subdir('market')
subdir('position')
subdir('account')
subdir('analytics')
subdir('backtest')
subdir('signal')
subdir('portfolio')
subdir('risk')
subdir('sizing')
subdir('rebalance')
subdir('execution')
subdir('exchange')
subdir('runtime')
subdir('persistence')
subdir('recovery')
subdir('testing')
subdir('strategy')
subdir('ranker')
subdir('indicator')
subdir('universe')
subdir('filter')


# ---- Include directories for all submodules ----
include_dir = include_directories(
    'common_types',
    'utils',
    'data_types',
    'contracts',
    'transport',
    'market',
    'position',
    'account',
    'analytics',
    'backtest',
    'signal',
    'portfolio',
    'risk',
    'sizing',
    'rebalance',
    'execution',
    'exchange',
    'runtime',
    'persistence',
    'recovery',
    'testing',
    'strategy',
    'strategy/strategies',
    'ranker',
    'indicator',
    'universe',
    'filter'
)

# ---- Aggregate all sources from submodules ----
lib_sources = []
lib_sources += common_types_sources
lib_sources += utils_sources
lib_sources += data_types_sources
lib_sources += contracts_sources
lib_sources += transport_sources
lib_sources += market_sources
lib_sources += position_sources
lib_sources += account_sources
lib_sources += analytics_sources
lib_sources += backtest_sources
lib_sources += signal_sources
lib_sources += portfolio_sources
lib_sources += risk_sources
lib_sources += sizing_sources
lib_sources += rebalance_sources
lib_sources += execution_sources
lib_sources += exchange_sources
lib_sources += runtime_sources
lib_sources += persistence_sources
lib_sources += recovery_sources
lib_sources += testing_sources
lib_sources += strategy_sources
lib_sources += ranker_sources
lib_sources += indicator_sources
lib_sources += universe_sources
lib_sources += filter_sources



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
    ] + persistence_deps + transport_deps
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
    ] + persistence_deps + transport_deps
)
