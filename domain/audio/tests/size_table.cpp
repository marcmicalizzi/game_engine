// Size table for domain/audio (ADR-0019). The hot types are the ones the audio thread walks every
// block — the voice, the spatial block beside it, the command it reads off the ring — and the ones
// the controlling thread keeps per voice.
#include <core/base/size_table.h>
#include <domain/audio/audio.h>

using namespace engine;
using namespace engine::audio;

// The audio thread's voice: everything the voice loop reads per block, one record. 48 bytes; the
// default pool of 64 is 3 KB.
ENGINE_EXPECT_SIZE(48, 8, VoiceState);
// The controlling thread's record of a slot.
ENGINE_EXPECT_SIZE(16, 8, VoiceSlot);
// The object: position, orientation, distance model, cone, spread, 2D flag and mapping. Read once
// per voice per block by the source model.
ENGINE_EXPECT_SIZE(56, 4, SourceSpatial);
// What the source model hands the decode stage.
ENGINE_EXPECT_SIZE(20, 4, SpatialParams);
// The listener, reduced once to a basis on the controlling thread.
ENGINE_EXPECT_SIZE(48, 4, ListenerBasis);
// Two cache lines: a Play carries the whole object (commands.h, "Why 128 bytes"). The Play grew
// from 80 to 88 bytes with the stream it names (a u32 and its padding), inside the same two lines.
ENGINE_EXPECT_SIZE(56, 4, SourcePayload);
ENGINE_EXPECT_SIZE(88, 8, PlayPayload);
ENGINE_EXPECT_SIZE(88, 8, CommandPayload);
ENGINE_EXPECT_SIZE(128, 64, Command);
// What a streamed voice and its fill share (stream.h): one line the fill writes (`filled`, `end`)
// and one the voice writes (`consumed`, the underrun count, `base`), so neither side's stores
// invalidate the line the other is reading from.
ENGINE_EXPECT_SIZE(128, 64, StreamRing);
ENGINE_EXPECT_SIZE(8, 4, VoiceEvent);
ENGINE_EXPECT_SIZE(8, 4, VoiceHandle);
ENGINE_EXPECT_SIZE(4, 4, ClipHandle);
// The master's limiter, walked every frame while it limits: its state is the four ring buffers'
// headers (the delay line, the hold's two columns, the box filter's deficits — all sized once, in
// the constructor) and the running scalars. Two cache lines and a bit; the rings themselves are
// 240 frames of the layout, and 241 entries each.
ENGINE_EXPECT_SIZE(136, 8, Limiter);
