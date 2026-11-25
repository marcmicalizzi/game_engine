# world_sound capability (ADR-0027; ADR-0079, proposed): the sound stage a host creates for a game
# — the clips a scene's `sound` block declares, the mixer, the output or the null backend, the
# listener, and a game's voices counted by the stage (docs/subsystems/world_sound.md; roadmap S111,
# the first stage of docs/subsystems/audio.md "Sound for the world").
#
# OPTIONAL creates ENGINE_WITH_WORLD_SOUND (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes; a host without it answers a game's sound calls
# `Unsupported`. That is the proof that the capability is additive.

# The capability graph (docs/plan/08-toolchain.md section 8.5). `audio` is itself an optional
# capability and this one links it, so the edge is declared: with that capability switched off,
# this capability is switched off too, instead of failing a configure that is perfectly legitimate.
engine_capability_requires(world_sound audio)

# `schemas` for the scene's `engine.scene.Sound` (the block is the scene's, so a build without this
# capability reads the same file); `schema` and `json` to read it straight from the text.
# `gfx` for the wind's header alone (domain/gfx/wind.h: the frame's wind as every reader has it,
# S112): the model is not a capability, and nothing of the device is used. `jobs` and `time` for the
# refresh off the frame's thread (S122): a refresh computed on a host's Efficiency pool, and timed.
engine_module(NAME world_sound LAYER systems OPTIONAL
  DEPS base containers math hash ids log io json schema tunables audio audio_schemas schemas gfx
       jobs time
  SOURCES src/world_sound.cpp src/world_field.cpp src/water_field.cpp src/path.cpp)

# NO_DEVICE: no case opens a GPU device, and none opens an audio device: every case runs the null
# backend and judges by its samples (ADR-0079 decision 6).
engine_module_tests(NAME world_sound NO_DEVICE
  SOURCES tests/world_sound_tests.cpp tests/world_field_tests.cpp tests/world_path_tests.cpp
          tests/world_water_tests.cpp tests/world_refresh_tests.cpp tests/size_table.cpp
  DEPS memory jobs)

# The stage's tick with N voices sounding under the null backend: the controlling thread's
# commands and the audio thread's block together (docs/subsystems/world_sound.md, "Performance notes").
engine_module_bench(NAME world_sound
  SOURCES bench/world_sound_bench.cpp)
