// Size table for core/jobs (ADR-0019).
#include <core/base/size_table.h>
#include <core/jobs/job_system.h>

using namespace engine;
using namespace engine::jobs;

ENGINE_EXPECT_SIZE(24, 8, Job);       // fn, data, counter
ENGINE_EXPECT_SIZE(64, 64, Counter);  // one cache line, so waiters never share with neighbours
ENGINE_EXPECT_SIZE(8, 2, WorkerInfo);
