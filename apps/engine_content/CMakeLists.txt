# engine-content: the content build on the command line (docs/plan/07-content-pipeline.md §7.3).
# Imports a glTF file, validates it, clusters it per material on the job system's performance
# pool, and writes the .clusters container the renderer loads; `build-all` does that for a
# manifest of meshes, `texture` builds one image into a block-compressed `.tex` (and `build` does it
# for every image a mesh samples), and `info` and `stats` read one back. `limit-dump` is the
# limit-surface conformance exchange with the authoring side, `normal-cases` its footpoint-normal
# edge cases one by one, `tissue` reads, validates and reports tissue definitions
# (docs/subsystems/tissue.md), `ruins` assembles ruined buildings (docs/subsystems/ruins.md), and
# `city` makes Island City's plan, buildings, proxy fragments and E18's yield
# (docs/subsystems/city.md).
#
# `tissue` needs the tissue capability, which needs physics, and both are optional (ADR-0027). An
# app is not a capability and cannot declare `engine_capability_requires`, so it does what
# engine-view does for `--animate`: link the module when this configuration has it, define
# ENGINE_CONTENT_TISSUE so the commands compile, and otherwise build an engine-content that refuses
# `tissue` with a sentence. `msvc-minimal` is the proof that the second half works.
set(_content_deps base containers math hash time log platform jobs io json geometry assets atlas
    texture content_build)
set(_content_tests tests/content_tests.cpp tests/determinism_tests.cpp tests/limit_dump_tests.cpp
    tests/normal_cases_tests.cpp tests/texture_tests.cpp)
if(TARGET engine::tissue)
  list(APPEND _content_deps tissue schemas)
  list(APPEND _content_tests tests/tissue_tests.cpp)
endif()
# `ruins` and `ruins-kit` the same way, with the ruins capability (docs/subsystems/ruins.md). The
# golden hash of a fixed seed lives in tests/determinism_tests.cpp beside the content build's rows,
# and is compiled only where the command exists.
if(TARGET engine::ruins)
  list(APPEND _content_deps ruins schema schemas)
  list(APPEND _content_tests tests/ruins_tests.cpp)
endif()
# `terrain` the same way, with the terrain capability (docs/subsystems/terrain.md): one tile of a
# scene's dune field as a JSON line. It reads the scene's terrain through the scene schema.
if(TARGET engine::terrain)
  list(APPEND _content_deps terrain terrain_schemas schema schemas)
  list(APPEND _content_tests tests/terrain_tests.cpp)
endif()
# `city` the same way, with the city capability (docs/subsystems/city.md): the plan, one lot's
# building, the proxy fragment and E18's yield. Its golden hashes live in tests/determinism_tests.cpp
# beside the ruins', and are compiled only where the command exists.
if(TARGET engine::city)
  list(APPEND _content_deps city schema schemas)
  list(APPEND _content_tests tests/city_tests.cpp)
endif()
# The audio capability's derived step, the `.clip` (docs/subsystems/audio.md, "Built clips"), the
# same way: `build` of a WAV, FLAC or MP3, a manifest's "clips" and `info` of a `.clip` exist when
# this configuration has audio. content_build holds the step's table, not the capability, so it is
# this app that links audio and hands the step over (clip_commands.cpp).
# The npc generator's command; its test runs either way, and without the capability checks the
# refusal.
list(APPEND _content_tests tests/npc_tests.cpp)
if(TARGET engine::npc)
  list(APPEND _content_deps npc)
endif()
if(TARGET engine::audio)
  list(APPEND _content_deps audio)
  list(APPEND _content_tests tests/clip_tests.cpp)
endif()
list(REMOVE_DUPLICATES _content_deps)

engine_app(NAME engine_content OUTPUT engine-content
  SOURCES main.cpp tissue_commands.cpp ruins_commands.cpp city_commands.cpp limit_dump.cpp
          normal_cases.cpp clip_commands.cpp terrain_commands.cpp npc_commands.cpp
  DEPS ${_content_deps}
  E2E_TESTS ${_content_tests})

if(TARGET engine::tissue)
  target_compile_definitions(engine_engine_content PRIVATE ENGINE_CONTENT_TISSUE=1)
  if(TARGET engine_engine_content_tests)
    target_compile_definitions(engine_engine_content_tests PRIVATE ENGINE_CONTENT_TISSUE=1)
  endif()
endif()
if(TARGET engine::ruins)
  target_compile_definitions(engine_engine_content PRIVATE ENGINE_CONTENT_RUINS=1)
  if(TARGET engine_engine_content_tests)
    target_compile_definitions(engine_engine_content_tests PRIVATE ENGINE_CONTENT_RUINS=1)
  endif()
endif()
if(TARGET engine::terrain)
  target_compile_definitions(engine_engine_content PRIVATE ENGINE_CONTENT_TERRAIN=1)
  if(TARGET engine_engine_content_tests)
    target_compile_definitions(engine_engine_content_tests PRIVATE ENGINE_CONTENT_TERRAIN=1
      ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  endif()
endif()
if(TARGET engine::city)
  target_compile_definitions(engine_engine_content PRIVATE ENGINE_CONTENT_CITY=1)
  if(TARGET engine_engine_content_tests)
    target_compile_definitions(engine_engine_content_tests PRIVATE ENGINE_CONTENT_CITY=1)
  endif()
endif()
if(TARGET engine::audio)
  target_compile_definitions(engine_engine_content PRIVATE ENGINE_CONTENT_AUDIO=1)
  if(TARGET engine_engine_content_tests)
    target_compile_definitions(engine_engine_content_tests PRIVATE ENGINE_CONTENT_AUDIO=1)
    # The clip test reads a built `.clip` back, and builds one itself to compare.
    target_link_libraries(engine_engine_content_tests PRIVATE engine::audio)
  endif()
endif()
# The end-to-end tests write their own glTF fixtures, with PNGs that foundation/image encodes, so
# nothing binary lives in the tree; the texture test reads a built `.tex` back.
if(TARGET engine_engine_content_tests)
  target_link_libraries(engine_engine_content_tests PRIVATE engine::image engine::texture)
endif()
# engine-view's mesh end-to-end test drives this app, which sits beside engine-view in bin/, to
# write the container it then renders.
if(TARGET engine_engine_view_tests)
  add_dependencies(engine_engine_view_tests engine_engine_content)
endif()
if(TARGET engine::npc)
  target_compile_definitions(engine_engine_content PRIVATE ENGINE_CONTENT_NPC=1)
  if(TARGET engine_engine_content_tests)
    target_compile_definitions(engine_engine_content_tests PRIVATE ENGINE_CONTENT_NPC=1)
  endif()
endif()
