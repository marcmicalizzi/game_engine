# audio_system capability (ADR-0027): the ECS half of audio — registers the AudioEmitter and
# AudioListener components with a world and turns their changes into mixer commands once per tick.
# docs/subsystems/audio_system.md.
#
# A module of its own, and not part of domain/audio, because it is the bridge between an ECS-free
# module and the world: <flecs.h> belongs to domain/ecs, systems/ and game/ (ADR-0028 seam 5).
#
# OPTIONAL creates ENGINE_WITH_AUDIO_SYSTEM (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest of
# the tree still builds and passes. WHOLE_ARCHIVE keeps the tunable's registration from being
# dropped by the linker because nothing references it.

# The capability graph (docs/plan/08-toolchain.md section 8.5). `audio` and `ecs` are themselves
# optional capabilities and this one links both, so the edges are declared: with either switched
# off, this capability is switched off too — with a line on the configure's status output and a row
# in modules.json's disabled_capabilities — instead of failing a configure that is perfectly
# legitimate. The reverse is not true: domain/audio mixes with no world at all, so ECS off leaves
# the mixer in and this out (the *-no-ecs presets are that configuration).
engine_capability_requires(audio_system audio ecs)

engine_module(NAME audio_system LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math log tunables ids sim ecs audio audio_schemas
  SOURCES src/audio_system.cpp)

engine_module_tests(NAME audio_system
  SOURCES tests/audio_system_tests.cpp tests/size_table.cpp)

# The emitter tick at 10^3 and 10^4 emitters, still and moving (docs/subsystems/audio_system.md,
# "Performance notes").
engine_module_bench(NAME audio_system
  SOURCES bench/audio_system_bench.cpp)
