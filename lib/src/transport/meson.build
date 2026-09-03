# ---- Include directory for this folder ----
transport_inc = include_directories('.')

nats_dep = dependency('libnats', required: true)
transport_deps = [nats_dep]

# ---- Source files ----
transport_sources = files(
    'contract_json_codec.cpp',
    'in_memory_message_bus.cpp',
    'nats_message_bus.cpp',
    'nats_jetstream_message_bus.cpp'
)
