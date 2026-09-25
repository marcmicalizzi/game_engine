# npc capability (ADR-0027, ADR-0045; docs/subsystems/npc.md): scheduled residents. A document
# record whose daily routine — a table of rows, data — runs through the engine's timing wheel at the
# tier the observer set gives it, with a closed form of where it is at any time that the summarizer
# and every materialization read; the places it goes between; and the generator that writes a
# seeded, partitioned layer of both.
#
# OPTIONAL creates ENGINE_WITH_NPC (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the tunables, the schema types and
# the two mappings — from being dropped by the linker because nothing references them.

# The capability graph (docs/plan/08-toolchain.md section 8.5). Its hooks and systems write the
# entity store's components, so it links `domain/ecs`, which is itself optional: with ECS off, this
# capability is switched off too, with a line saying so, instead of failing the configure.
engine_capability_requires(npc ecs)

# The capability's own record and component types and the two mappings (ADR-0007). They import
# `world.schema` for `Transform`, so the generated module links the engine's `schemas`.
engine_schema_library(NAME npc_schemas SCHEMAS schemas/npc.schema CAPABILITY npc DEPS schemas)

# `doc` and `io` for the place index and the generator, which read and write a document; `jobs` for
# the generator's worker split. The routine itself (routine.h) needs none of them.
engine_module(NAME npc LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash log ids jobs time tunables json io sim ecs doc schema schemas
       npc_schemas
  SOURCES src/routine.cpp src/npc.cpp src/generator.cpp)

engine_module_tests(NAME npc
  SOURCES tests/routine_tests.cpp tests/npc_tests.cpp tests/generator_tests.cpp
          tests/size_table.cpp
  DEPS doc)

# The routine's cost where it is paid: a tick with 10^5 residents held, a summary over them, and the
# generator (plan 11 §11.8; docs/experiments/e38-scheduled-npcs.md).
engine_module_bench(NAME npc
  SOURCES bench/npc_bench.cpp)
