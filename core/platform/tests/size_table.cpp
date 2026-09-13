// Size table for core/platform (ADR-0019). Topology types are cold data; sizes are pinned so
// accidental growth is noticed, not because they sit on a hot path.
#include <core/base/size_table.h>
#include <core/platform/topology.h>

using namespace engine;
using namespace engine::platform;

ENGINE_EXPECT_SIZE(32, 8, CpuSet);      // SmallVector<u64, 2>
ENGINE_EXPECT_SIZE(24, 4, LogicalCpu);
