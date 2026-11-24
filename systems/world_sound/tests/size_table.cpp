// Size table for systems/world_sound (ADR-0019). The stage holds no per-element state the frame
// walks at S111: its voices are the mixer's (domain/audio's table pins them). What a host reads
// every tick is the counters, pinned so a field added is a decision.
#include <core/base/size_table.h>
#include <systems/world_sound/world_sound.h>

using namespace engine;

ENGINE_EXPECT_SIZE(88, 8, world_sound::StageStats);
// S112: a crown as the canopy table sends it, walked every refresh (up to audio.world.max_crowns,
// 8,192: 320 KB), its world place, height, leaf area, class and needle width.
ENGINE_EXPECT_SIZE(48, 8, world_sound::Crown);  // S112-2: merged, extent_m
// What a world voice was last told, one a voice (6 + 8 + 2 by default), read by tests and hosts.
ENGINE_EXPECT_SIZE(52, 4, world_sound::WorldVoiceInfo);
ENGINE_EXPECT_SIZE(144, 8,
                   world_sound::WorldStats);  // S112-2: the ceiling, retries; S114: shelter;
// S113: the water's counts, and its mend the surf records the cull skipped (8 bytes)
ENGINE_EXPECT_SIZE(72, 8, world_sound::Stretch);
ENGINE_EXPECT_SIZE(56, 8, world_sound::Surf);
