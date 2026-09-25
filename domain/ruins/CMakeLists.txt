# ruins capability (ADR-0027; docs/subsystems/ruins.md): the seeded assembler that turns a kit of
# wall members into ruined buildings as instances of the kit's meshes (plan 07 §7.6).
#
# OPTIONAL creates ENGINE_WITH_RUINS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest of
# the tree still builds and passes — the renderer then refuses a scene that names ruins, and
# engine-content refuses `ruins` and `ruins-kit`, each with a sentence. That is the proof the
# capability is additive.
#
# `schema` and `schemas` are the kit's: a kit file is `engine.scene.RuinKit`, and a fragment an
# `engine.scene.Scene` (schemas/scene.schema). `jobs` is `assemble_tiles`'s performance pool.
engine_module(NAME ruins LAYER domain OPTIONAL
  DEPS base containers math hash json schema schemas io log jobs
  SOURCES src/kit.cpp src/assembler.cpp src/fragment.cpp src/synthetic_kit.cpp)

engine_module_tests(NAME ruins
  SOURCES tests/ruins_tests.cpp tests/size_table.cpp)

# The assembler's cost per building and per thousand, on one thread and on the pool.
engine_module_bench(NAME ruins
  SOURCES bench/ruins_bench.cpp)
