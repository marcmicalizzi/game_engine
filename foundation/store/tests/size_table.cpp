// Size table for foundation/store (ADR-0019).
//
// The two record types are the hot ones: a tick's persistence flush builds one EventRecord per
// event and one ProjectionRecord per dirty projection, and a tile load reads a million of the
// second. Database and EventLog themselves hold standard-library containers and are deliberately
// absent, as the header of core/base/size_table.h requires.
#include <core/base/size_table.h>
#include <foundation/store/database.h>
#include <foundation/store/event_log.h>

using namespace engine;
using namespace engine::store;

ENGINE_EXPECT_SIZE(80, 8, EventRecord);
ENGINE_EXPECT_SIZE(56, 8, ProjectionRecord);
ENGINE_EXPECT_SIZE(40, 8, SnapshotInfo);
ENGINE_EXPECT_SIZE(24, 8, Statement);
ENGINE_EXPECT_SIZE(8, 8, Transaction);
ENGINE_EXPECT_SIZE(16, 8, Migration);
ENGINE_EXPECT_SIZE(20, 4, OpenOptions);
