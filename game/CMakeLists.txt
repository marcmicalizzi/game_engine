# Layer L5: game. Per-game code, as hot-reloadable game modules (cmake/EngineGameModule.cmake,
# docs/subsystems/hotload.md). Nothing in the engine links a game module; a host loads it.
add_subdirectory(sandbox_npc)
add_subdirectory(walk_poc)
