# walk_poc: the fixture game module that proves engine-view's game runner (game/walk_poc/README.md,
# docs/subsystems/apps.md "--game: running a game module"; roadmap P41). Built only with the hotload
# capability, like every game module; nothing in the engine links it, engine-view loads it by path
# (`--game build/<preset>/bin/game/walk_poc.dll`) and tools/package-game.ps1 ships it.
engine_game_module(NAME walk_poc
  SOURCES walk_poc.cpp
  SCHEMAS walk_poc.schema)

# engine-view's end-to-end cases run it, and two test builds of it the runner must handle: one that
# declares an action no map has (refused when it loads) and one that fails from game_tick (the
# session ends with the error). The edges point down, from the game to the app's tests, as
# sandbox_npc's to the hotload bench do, so the engine knows nothing about any game.
if(TARGET engine_game_walk_poc AND TARGET engine_engine_view_tests)
  set(_tests "${CMAKE_CURRENT_BINARY_DIR}/test_builds")
  engine_game_module(NAME walk_poc_bad_action OUTPUT_DIRECTORY "${_tests}"
    SOURCES walk_poc.cpp SCHEMAS walk_poc.schema)
  target_compile_definitions(engine_game_walk_poc_bad_action PRIVATE WALK_POC_TEST=1)
  engine_game_module(NAME walk_poc_failing OUTPUT_DIRECTORY "${_tests}"
    SOURCES walk_poc.cpp SCHEMAS walk_poc.schema)
  target_compile_definitions(engine_game_walk_poc_failing PRIVATE WALK_POC_TEST=2)
  add_dependencies(engine_engine_view_tests engine_game_walk_poc engine_game_walk_poc_bad_action
                   engine_game_walk_poc_failing)
  target_compile_definitions(engine_engine_view_tests PRIVATE
    WALK_POC_PATH="$<TARGET_FILE:engine_game_walk_poc>"
    WALK_POC_BAD_ACTION_PATH="$<TARGET_FILE:engine_game_walk_poc_bad_action>"
    WALK_POC_FAILING_PATH="$<TARGET_FILE:engine_game_walk_poc_failing>"
    WALK_POC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  # The hot-reload case compiles a second build of the fixture, a field added, into its own
  # scratch directory with the exported command, as foundation/hotload's tests do.
  engine_game_module_build_command(engine_engine_view_tests)
endif()
# engine-view itself depends on the fixture being built, so `--game build/<preset>/bin/game/
# walk_poc.dll` works after building the app alone.
if(TARGET engine_game_walk_poc AND TARGET engine_engine_view)
  add_dependencies(engine_engine_view engine_game_walk_poc)
endif()
