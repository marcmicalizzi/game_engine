// Size table for core/log (ADR-0019). Fields are built on the caller's stack for every enabled
// record, so their size is the per-field cost of logging.
#include <core/base/size_table.h>
#include <core/log/log.h>

using namespace engine;

ENGINE_EXPECT_SIZE(40, 8, log::Field);
ENGINE_EXPECT_SIZE(80, 8, log::Record);
ENGINE_EXPECT_SIZE(24, 8, log::Category);
ENGINE_EXPECT_SIZE(16, 8, log::Sink);
