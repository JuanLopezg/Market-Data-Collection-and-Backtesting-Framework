# ---- Import global dependencies ----
nlohmann_json_dep          = global_deps['nlohmann_json_dep']
json_schema_validator_dep  = global_deps['json_schema_validator_dep']
log4cpp_dep                 = global_deps['log4cpp_dep']
boost_dep                  = global_deps['boost_dep']
fmt_dep                   = global_deps['fmt_dep']

# ---- Bring in subdirectories ----
subdir('types')
subdir('utils')
subdir('database')
subdir('data')

# ---- Include directories for all submodules ----
include_dir = include_directories(
    'types',
    'utils',
    'database',
    'data'
)

# ---- Aggregate all sources from submodules ----
lib_sources = []
lib_sources += types_sources
lib_sources += utils_sources
lib_sources += database_sources
lib_sources += data_sources

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
        boost_dep
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
        boost_dep
    ]
)
