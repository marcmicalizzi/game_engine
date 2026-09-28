# scene_collision capability (ADR-0027; docs/subsystems/scene_collision.md): static collision
# bodies from the scene round a walker — the ground as a heightfield a tile from the scene's ground
# provider, the placements as a compound a tile of each piece's LOD-cut proxy — kept by a world
# consumer as the walker's tile ring moves, for a `physics::CharacterBody` to walk on.
#
# OPTIONAL creates ENGINE_WITH_SCENE_COLLISION (ON by default): switching it off, or configuring
# with ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes — engine-view then walks by following the ground
# (docs/subsystems/apps.md, "Walking"). That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's tunables from being dropped by the linker because nothing
# references them.

# The capability graph (docs/plan/08-toolchain.md section 8.5). It links physics (the bodies) and
# the world (the ring and its consumer table), both optional capabilities, so the edges are
# declared: with either switched off — the minimal build, or ENGINE_WITH_PHYSICS=OFF — this one is
# off too, with a line on the configure's output, instead of failing the configure.
engine_capability_requires(scene_collision physics world)

# `renderer` for the scene as loaded (its meshes' DAGs, its instances, its streamed placement
# entries), `scene_gen` for the ground provider's interface and the placement generators' registry,
# `geometry` for the DAG a proxy is cut from. No generator is a dependency: a host links the ones it
# carries (ADR-0046).
engine_module(NAME scene_collision LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash log time tunables schemas geometry gfx scene_gen renderer
       physics world sim
  SOURCES src/scene_collision.cpp)

set(_collision_tests tests/scene_collision_tests.cpp tests/size_table.cpp)
if(TARGET engine::terrain)
  # The ground refresh under a time-lapse, on the committed erg's dunes and the renderer's own
  # cadence rule: the terrain reaches the collision through the registry, so the test links it as
  # a host does.
  list(APPEND _collision_tests tests/time_lapse_tests.cpp)
endif()
engine_module_tests(NAME scene_collision SOURCES ${_collision_tests})
if(TARGET engine_scene_collision_tests)
  target_compile_definitions(engine_scene_collision_tests PRIVATE
    ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(TARGET engine::terrain)
    target_link_libraries(engine_scene_collision_tests PRIVATE engine::terrain)
  endif()
endif()

# A tile's activation (a heightfield and a compound of 100 pieces), and a character step among them.
engine_module_bench(NAME scene_collision
  SOURCES bench/scene_collision_bench.cpp)
