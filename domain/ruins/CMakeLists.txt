# ruins capability (ADR-0027; docs/subsystems/ruins.md): the seeded assembler that turns a kit of
# wall members into ruined buildings as instances of the kit's meshes (plan 07 §7.6).
#
# OPTIONAL creates ENGINE_WITH_RUINS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest of
# the tree still builds and passes — the renderer then refuses a scene that names ruins, and
# engine-content refuses `ruins` and `ruins-kit`, each with a sentence. That is the proof the
# capability is additive.
#
# `schema` and `schemas` are the kit's: a kit file is `engine.scene.RuinKit` (a block kit
# `engine.scene.RuinBlockKit`), and a fragment an `engine.scene.Scene` (schemas/scene.schema).
# `jobs` is `assemble_tiles`'s and `assemble_block_tiles`'s performance pool. `src/grid.h` is the
# integer arithmetic the assembler and the block layer share, private to the module.
#
# `scene_gen` is the placement generator's (scene_generator.h; ADR-0046): the ruins register as the
# placement generator "ruins" from `src/scene_generator.cpp`, and the renderer's scene reader and the
# world ring find them by that name and link nothing of this module. WHOLE_ARCHIVE keeps the
# registrar, which nothing references.
engine_module(NAME ruins LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema schemas io log jobs time scene_gen
  SOURCES src/kit.cpp src/assembler.cpp src/block_kit.cpp src/blocks.cpp src/fragment.cpp
          src/synthetic_kit.cpp src/synthetic_blocks.cpp src/scene_generator.cpp)

engine_module_tests(NAME ruins
  SOURCES tests/ruins_tests.cpp tests/blocks_tests.cpp tests/scene_generator_tests.cpp
          tests/size_table.cpp)

# The assembler's cost per building and per thousand, on one thread and on the pool.
engine_module_bench(NAME ruins
  SOURCES bench/ruins_bench.cpp)
