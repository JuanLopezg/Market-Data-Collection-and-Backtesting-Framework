project('ALGOTRADING', 'cpp',
    version: '0.1.0',
    meson_version: '>=0.63.0',
    default_options : [
        'buildtype=debugoptimized',
        'cpp_std=c++23',
        'warning_level=3'
    ]
)

add_project_arguments(
    '-DALGOTRADING_VERSION="' + meson.project_version() + '"',
    language: 'cpp'
)

# ------------------------------
# Dependencies
# ------------------------------

boost_dep = dependency('boost',
    modules : ['filesystem', 'program_options'],
    required : true
)

nlohmann_json_dep = dependency('nlohmann_json', required: true)

json_schema_validator_dep = dependency(
  'nlohmann_json_schema_validator',
  method: 'cmake',
  modules: ['nlohmann_json_schema_validator::validator'],
  required: true
)

log4cpp_dep = dependency('log4cpp', required: true)
sqlite3_dep = dependency('sqlite3', required: true)
fmt_dep = dependency('fmt', required: true)

# --- NEW: CURL dependency ---
curl_dep = dependency('libcurl', required: true)

# ------------------------------
# Export dependencies to subdirs
# ------------------------------
global_deps = {
    'boost_dep'                 : boost_dep,
    'nlohmann_json_dep'         : nlohmann_json_dep,
    'json_schema_validator_dep' : json_schema_validator_dep,
    'log4cpp_dep'                : log4cpp_dep,
    'sqlite3_dep'               : sqlite3_dep,
    'curl_dep'                  : curl_dep, 
    'fmt_dep'                   : fmt_dep, 
}

# ------------------------------
# Subdirectories
# ------------------------------
subdir('lib')
subdir('database')
subdir('signalizer')
