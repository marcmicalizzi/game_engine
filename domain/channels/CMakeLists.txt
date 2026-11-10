# channels capability (ADR-0027; docs/subsystems/channels.md, roadmap W86): the drainage of a
# region's ground routed on a lattice, the channels its contributing area and slope begin, and the
# beds, bars and banks that dress the trough the ground already holds — the placement generator
# "channels" and the mesh generator "channel" it writes recipes for. The network is also what
# `engine-content channels` writes the water's channel description from (water_channels_json).
#
# OPTIONAL creates ENGINE_WITH_CHANNELS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types and its tests, and a scene naming the
# generator "channels" is refused with the registry's sentence. WHOLE_ARCHIVE keeps the two static
# registrations from being dropped by the linker because nothing references them.
#
# Pure CPU arithmetic in the domain layer, beside the deposits generator it is modelled on: the
# ground is a `scene_gen::Ground` it is handed, the meshes are `assets::MeshData` cluster-built by
# `geometry`, their looks a material pack's roles. No GPU, no ECS, no renderer.
engine_schema_library(NAME channels_schemas SCHEMAS schemas/channels.schema CAPABILITY channels)

engine_module(NAME channels LAYER domain OPTIONAL WHOLE_ARCHIVE
  GENERATOR placement:channels mesh:channel
  DEPS base containers math hash json schema log io jobs assets geometry materials texture scene_gen
       channels_schemas
  SOURCES src/network.cpp src/pieces.cpp src/generator.cpp src/measure.cpp)

engine_module_tests(NAME channels
  SOURCES tests/channels_tests.cpp tests/size_table.cpp)