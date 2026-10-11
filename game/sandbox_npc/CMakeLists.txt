# The sandbox NPC game module: E7's gameplay feature in C++, hot-reloadable (game/sandbox_npc,
# docs/experiments/e7-luau-spike.md "The C++ hot-reload arm"). Built only with the hotload
# capability; the bench that measures it lives with the capability (foundation/hotload/bench).
engine_game_module(NAME sandbox_npc
  SOURCES sandbox_npc.cpp routines.cpp
  SCHEMAS sandbox_npc.schema)

# The bench loads this module by path. The edge points down, from the game to the capability's
# bench, so the capability knows nothing about any game.
if(TARGET engine_game_sandbox_npc AND TARGET engine_hotload_bench)
  add_dependencies(engine_hotload_bench engine_game_sandbox_npc)
  target_compile_definitions(engine_hotload_bench PRIVATE
    ENGINE_SANDBOX_NPC_PATH="$<TARGET_FILE:engine_game_sandbox_npc>"
    ENGINE_SANDBOX_NPC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()
