// Size table for domain/events (ADR-0019, docs/plan/11-performance-principles.md §11.2).
//
// The hot type is `EventRecord`: one per event in a bank, two banks of `sim.events.capacity`
// (65,536 by default), so 64 bytes is 8 MiB of records beside the payload arenas, and a stress
// tick of 10^5 events walks 6.4 MB of them. 64 is six 8-byte fields (tick, id, cause, root, key,
// tile), source, sequence and payload offset, the type, the phase and the depth — a cache line with
// no padding at all, which is why the payload's size is not a field: it is the type's.

#include <core/base/size_table.h>
#include <domain/events/events.h>

using namespace engine;
using namespace engine::events;

ENGINE_EXPECT_SIZE(64, 8, EventRecord);
ENGINE_EXPECT_SIZE(8, 8, EventId);
ENGINE_EXPECT_SIZE(16, 8, ChainCount);
ENGINE_EXPECT_SIZE(32, 4, GuardTrips);
ENGINE_EXPECT_SIZE(32, 8, TypeCounters);
ENGINE_EXPECT_SIZE(208, 8, TickReport);
