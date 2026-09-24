// Size table for systems/audio_system (ADR-0019). The two components are what a world holds per
// emitter and per listener, and the private state rides beside every emitter: all three are read
// by the emitter system every tick.
#include <core/base/size_table.h>
#include <systems/audio_system/audio_system.h>

using namespace engine;
using namespace engine::audio;

// The object: clip, position, orientation, level, pitch, pan, distance model and distances, cone,
// spread, cue, bus, priority, mapping and three flags.
ENGINE_EXPECT_SIZE(96, 8, AudioEmitter);
ENGINE_EXPECT_SIZE(36, 4, AudioListener);
// The emitter as last sent — the diff every change is judged against — and its voice.
ENGINE_EXPECT_SIZE(112, 8, EmitterVoice);
