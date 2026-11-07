// Size table for domain/channels (ADR-0019). The capability has no hot type: the network is
// decided once when a scene is read and its pieces are scene instances. The one record kept per
// point of every line is pinned here, since a region's lines hold tens of thousands of them.
#include <core/base/size_table.h>
#include <domain/channels/channels.h>

using namespace engine;

ENGINE_EXPECT_SIZE(40, 8, channels::ChannelPoint);
