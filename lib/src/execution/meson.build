execution_inc = include_directories('.')

# Execution owns orders/fills; planning is pure target/current/pending -> commands.
execution_sources = files(
    'execution_order.h',
    'order_manager.h',
    'fill.h',
    'execution_engine.cpp',
    'planning/target_resolution.h',
    'planning/quantity_order_builder.h',
    'planning/quantity_order_planner.cpp',
    'planning/notional_order_planner.cpp',
    'planning/planning_state.cpp'
)
