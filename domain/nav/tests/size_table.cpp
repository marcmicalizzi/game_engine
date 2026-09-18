// Size table for domain/nav (ADR-0019). The pinned types are the ones that exist per tile
// region, per border portal, per agent, or per path point — the things a world has tens of
// thousands of — so their footprint is the module's footprint.
#include <core/base/size_table.h>
#include <domain/nav/nav_mesh.h>
#include <domain/nav/rebuild_queue.h>
#include <domain/nav/region_graph.h>
#include <domain/nav/tile.h>

using namespace engine;

// A tile coordinate is two integers and is passed by value everywhere.
ENGINE_EXPECT_SIZE(8, 4, nav::TileCoord);

// Handles are SlotMap handles and nothing else: passing one is passing eight bytes.
ENGINE_EXPECT_SIZE(8, 4, nav::OffMeshLinkId);
ENGINE_EXPECT_SIZE(8, 4, nav::AgentId);

// One per query. The filter is copied into a backend filter per call, so it stays small enough
// to sit on a caller's stack beside the corridor buffer.
ENGINE_EXPECT_SIZE(28, 4, nav::PathFilter);
ENGINE_EXPECT_SIZE(24, 8, nav::NavPoint);
ENGINE_EXPECT_SIZE(12, 4, nav::PathResult);
ENGINE_EXPECT_SIZE(32, 4, nav::RaycastHit);

// Two per tile region and per border portal — the coarse tier's whole resident cost. At four
// regions a tile and ten portals, a 10,000-tile world spends about 2.6 MB on this table.
ENGINE_EXPECT_SIZE(44, 4, nav::TileRegionInfo);
ENGINE_EXPECT_SIZE(24, 4, nav::TilePortal);

// One per region graph node, which is one per walkable component of a tile.
ENGINE_EXPECT_SIZE(52, 4, nav::RegionNode);

// One per off-mesh link, of which a destroyed city block has hundreds.
ENGINE_EXPECT_SIZE(32, 4, nav::OffMeshLink);

// One per agent, on the caller's side of the crowd.
ENGINE_EXPECT_SIZE(48, 8, nav::AgentDesc);
ENGINE_EXPECT_SIZE(56, 8, nav::AgentState);
