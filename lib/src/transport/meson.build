# Message serialization and durable NATS JetStream transport.
transport_inc = include_directories('.')

nats_dep = dependency('libnats', required: true)
transport_deps = [nats_dep]

transport_sources = files(
    'message_json.cpp',
    'jetstream_bus.cpp'
)
