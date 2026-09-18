// Size table for domain/sim (ADR-0019, docs/plan/11-performance-principles.md §11.2).
//
// The hot type here is `TimingWheel::Slot`: the simulation is expected to hold 10^5-10^6 live
// timers ([05 §5.6](../../../docs/plan/05-simulation.md#56-what-npc-scale-is-realistic)), so a
// byte on it is a megabyte at scale. 56 bytes is due time, interval, insertion sequence, the
// 16-byte payload, the two intrusive list links, the generation, the system id, the level and
// the flags — nothing spare. Growing it means saying in the commit what the extra bytes buy.
//
// `TierChange` is the other one: tier assignment emits one per entity that crossed a band, and
// a crowd crossing together makes that a real array.

#include <core/base/size_table.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/tiers.h>
#include <domain/sim/timing_wheel.h>

using namespace engine;
using namespace engine::sim;

// The timing wheel.
ENGINE_EXPECT_SIZE(56, 8, TimingWheel::Slot);
ENGINE_EXPECT_SIZE(8, 4, TimerHandle);
ENGINE_EXPECT_SIZE(16, 8, TimerPayload);
ENGINE_EXPECT_SIZE(56, 8, TimerEvent);
ENGINE_EXPECT_SIZE(16, 8, EventSink);
ENGINE_EXPECT_SIZE(40, 8, SummarizeInterval);
ENGINE_EXPECT_SIZE(32, 8, Summarizer);
ENGINE_EXPECT_SIZE(24, 8, TimingWheelConfig);
ENGINE_EXPECT_SIZE(56, 8, FastForwardResult);

// Tier assignment.
ENGINE_EXPECT_SIZE(8, 4, TierChange);
ENGINE_EXPECT_SIZE(20, 4, TierStats);
ENGINE_EXPECT_SIZE(52, 4, TierParams);
ENGINE_EXPECT_SIZE(48, 8, TierInput);

// The scheduler's registration tables.
ENGINE_EXPECT_SIZE(1, 1, TickPhase);
ENGINE_EXPECT_SIZE(32, 8, ComponentMask);
ENGINE_EXPECT_SIZE(8, 8, ResourceMask);
ENGINE_EXPECT_SIZE(8, 4, Batch);
ENGINE_EXPECT_SIZE(40, 8, SystemContext);
// 136 and not 120: the two `ResourceMask`s that let a system declare data outside the ECS. A
// descriptor is one row per system in a table read when the schedule is built and never in a tick,
// so sixteen bytes there buy an ordering the schedule could not otherwise express.
ENGINE_EXPECT_SIZE(136, 8, SystemDesc);
ENGINE_EXPECT_SIZE(4, 2, ScheduleEntry);
ENGINE_EXPECT_SIZE(40, 8, EntityRecord);
ENGINE_EXPECT_SIZE(56, 8, MaterializationHooks);
ENGINE_EXPECT_SIZE(16, 8, TileState);
ENGINE_EXPECT_SIZE(24, 8, TileStore);
