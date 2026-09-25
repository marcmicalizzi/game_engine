# kinematics capability (ADR-0027): motion without forces. An entity with a `world::Transform`
# and a `kinematics::Velocity` moves by its velocity every tick; `Mover` records materialize into
# the two (docs/subsystems/kinematics.md).
#
# OPTIONAL creates ENGINE_WITH_KINEMATICS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the schema types and the Mover
# mapping — from being dropped by the linker because nothing references them.

# The capability graph (docs/plan/08-toolchain.md section 8.5). A capability that ticks attaches
# through `ecs::register_system`, so it links `domain/ecs`, which is itself optional: with ECS off,
# this capability is switched off too, with a line saying so, instead of failing the configure.
engine_capability_requires(kinematics ecs)

# The capability's own component and record types, and the Mover mapping (ADR-0007). They import
# `world.schema` for `Transform`, so the generated module links the engine's `schemas`.
engine_schema_library(NAME kinematics_schemas SCHEMAS schemas/kinematics.schema
  CAPABILITY kinematics DEPS schemas)

engine_module(NAME kinematics LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math log ids sim ecs schema schemas kinematics_schemas
  SOURCES src/kinematics.cpp)

# The tests materialize `Mover` records through the driver and the entity store's hook, so they
# reach the document model too.
engine_module_tests(NAME kinematics
  SOURCES tests/kinematics_tests.cpp tests/size_table.cpp
  DEPS doc)

# The integrate system over 10^3..10^5 movers, through the real tick (plan 11 §11.8).
engine_module_bench(NAME kinematics
  SOURCES bench/kinematics_bench.cpp)
