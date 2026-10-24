# tileset capability (ADR-0027; docs/subsystems/tileset.md): an authored terrain — the
# `engine.terrain.heightfield/1` source (a JSON header beside a raw float32 grid), the tile set
# built from it (its finest tiles with a point of apron and its far mip chain, each level the
# surface box-filtered over each point's own cell, in whole millimetres), the reader that answers a
# tile source from it, and the `tiles` ground provider that opens one from a scene's terrain entry.
#
# OPTIONAL creates ENGINE_WITH_TILESET (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests and its schema library, and the rest of the tree
# still builds and passes — a scene naming the provider "tiles" is then refused with the registry's
# sentence, and engine-content refuses `terrain-synth` and `terrain-tiles`. WHOLE_ARCHIVE keeps the
# provider's registrar, which nothing references.
#
# `scene_gen` for the registry and the tile source, `schemas` for the scene's terrain entry (its
# `tiles` block), `geometry` for the repository's derived-data root (`find_ddc_root`), `io` for the
# files. No GPU, no ECS, no renderer.
engine_schema_library(NAME tileset_schemas SCHEMAS schemas/tileset.schema CAPABILITY tileset)

engine_module(NAME tileset LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema schemas tileset_schemas io image geometry scene_gen
       tunables time profiling
  SOURCES src/tileset.cpp src/masks.cpp src/ground.cpp)

engine_module_tests(NAME tileset
  SOURCES tests/tileset_tests.cpp tests/determinism_tests.cpp tests/mask_tests.cpp)
