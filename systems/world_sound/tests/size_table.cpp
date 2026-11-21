// Size table for systems/world_sound (ADR-0019). The stage holds no per-element state the frame
// walks at S111: its voices are the mixer's (domain/audio's table pins them). What a host reads
// every tick is the counters, pinned so a field added is a decision.
#include <core/base/size_table.h>
#include <systems/world_sound/world_sound.h>

using namespace engine;

ENGINE_EXPECT_SIZE(88, 8, world_sound::StageStats);
