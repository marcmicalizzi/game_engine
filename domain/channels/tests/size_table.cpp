// Size table for domain/channels (ADR-0019). The capability has no hot type: the network is
// decided once when a scene is read and its pieces are scene instances. The one record kept per
// point of every line is pinned here, since a region's lines hold tens of thousands of them.
#include <core/base/size_table.h>
#include <domain/channels/channels.h>
#include <domain/channels/falls.h>

using namespace engine;

ENGINE_EXPECT_SIZE(40, 8, channels::ChannelPoint);
// W97: the bed's sediment, one per point of every line beside its `ChannelPoint`, written to the
// plan file as its bytes.
ENGINE_EXPECT_SIZE(32, 4, channels::ChannelSediment);
// S104: the falls file's three records, written as their bytes (channels.md, "Where the water
// leaves its bed (S104)", has the layout the water reads): a fall, a column of its lip, a vertex of
// a column's path.
ENGINE_EXPECT_SIZE(88, 8, channels::Fall);
ENGINE_EXPECT_SIZE(28, 4, channels::FallColumn);
ENGINE_EXPECT_SIZE(28, 4, channels::FallVertex);
