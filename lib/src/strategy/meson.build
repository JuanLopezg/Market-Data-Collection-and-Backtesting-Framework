# ---- Include directories ----
strategy_inc = include_directories('.')

# ---- Subdirectories ----
subdir('strategies')

# ---- Merge include paths ----
strategy_inc = [strategy_inc, strategies_inc]

# ---- Source files (only translation units, not headers) ----
strategy_sources = files(
    'strategy_instance.h',
    'strategy.cpp'
)
