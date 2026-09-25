# audio capability (ADR-0027): output devices and a null backend, decoded clips, a fixed voice
# pool, a bus tree and the master's clip, driven through two lock-free queues — the device and
# mixing half of plan 05 §5.11's "miniaudio for devices and mixing". docs/subsystems/audio.md.
#
# OPTIONAL creates ENGINE_WITH_AUDIO (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema library, its tests, its bench and the
# miniaudio download from the build, and the rest of the tree still builds and passes. That is the
# proof that the capability is additive. `systems/audio_system` — the ECS half — requires this
# capability and leaves with it.
engine_capability_enabled(audio _engine_audio)
if(NOT _engine_audio)
  return()
endif()

include(EngineAudio)
engine_fetch_miniaudio()

# The capability's own component and record types (ADR-0007): the AudioEmitter and AudioListener
# components systems/audio_system registers with a world, and audio.devices' result. They live
# here, not in the shared schemas/ directory, so that adding them touches no file this capability
# does not own; CAPABILITY ties them to the same switch.
engine_schema_library(NAME audio_schemas SCHEMAS schemas/audio.schema CAPABILITY audio)

# WHOLE_ARCHIVE keeps the tunables' registrations (src/audio.cpp) in a binary that links the
# module for its protocol method alone.
# `io` for streaming: a streamed clip is read by range through `io::FileHandle` (docs page,
# "Streaming").
engine_module(NAME audio LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash ids jobs log io tunables protocol audio_schemas
  SOURCES src/audio.cpp src/backend.cpp src/clip_store.cpp src/decoder.cpp src/device.cpp
          src/endpoint.cpp src/format.cpp src/limiter.cpp src/mixer.cpp src/protocol.cpp
          src/spatial.cpp src/stream.cpp)

# miniaudio is linked PRIVATE: its header, and the defines that shape its structs, reach
# src/backend.cpp and nothing that includes a public header of this module (physics.md, "Why
# nothing from Jolt is public", is the same argument).
target_link_libraries(engine_audio_impl PRIVATE engine::miniaudio)

# src/endpoint.cpp, the module's platform file, talks to the Windows endpoint enumerator through
# COM (CoInitializeEx, CoCreateInstance): ole32. miniaudio loads it at run time for itself; the
# engine's own watcher links it, as every Windows system library is linked, from the system.
if(WIN32)
  target_link_libraries(engine_audio_impl PRIVATE ole32)
endif()

engine_module_tests(NAME audio
  SOURCES tests/audio_tests.cpp tests/queue_tests.cpp tests/mixer_tests.cpp tests/clip_tests.cpp
          tests/limiter_tests.cpp tests/determinism_tests.cpp tests/realtime_tests.cpp
          tests/stream_tests.cpp
          tests/size_table.cpp
  DEPS json schema)

# The mix cost per voice per 10 ms block, by voice count and by voice kind (docs/subsystems/audio.md,
# "Performance notes").
engine_module_bench(NAME audio
  SOURCES bench/audio_bench.cpp)
