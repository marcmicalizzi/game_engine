// Size table for systems/audio_system (ADR-0019). The two components are what a world holds per
// emitter and per listener, and the private state rides beside every emitter: all three are read
// by the emitter system every tick.
#include <core/base/size_table.h>
#include <systems/audio_system/audio_system.h>

using namespace engine;
using namespace engine::audio;

// The object: clip, position, orientation, level, pitch, pan, distance model and distances, cone,
// spread, cue, bus, priority, mapping and three flags. 112 and not 96, and the listener 48 and not
// 36 at 8-byte alignment (2026-10-06): their positions are `WorldPos`, three f64 (ADR-0053) —
// twelve bytes each for a source that is where it is 10,000 km out, and four of padding the
// emitter's flags no longer fill.
ENGINE_EXPECT_SIZE(112, 8, AudioEmitter);
ENGINE_EXPECT_SIZE(48, 8, AudioListener);
// The emitter as last sent — the diff every change is judged against — its voice, and the loop's
// clock (16 bytes: the frames it had played, and the tick it had played them by), which is what a
// loop coming back within reach resumes from. 144 and not 128 since the emitter it holds grew by 16
// (its f64 position): two cache lines and a quarter, read once per emitter per tick.
ENGINE_EXPECT_SIZE(144, 8, EmitterVoice);
