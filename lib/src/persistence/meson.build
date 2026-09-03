# ---- Include directory for this folder ----
persistence_inc = include_directories('.')

postgres_dep = dependency('libpq', required: true)
persistence_deps = [postgres_dep]

# ---- Source files ----
persistence_sources = files(
    'sqlite_state_store.cpp',
    'postgres_state_store.cpp'
)
