// Size table for domain/doc (ADR-0019). Types holding std::string are not pinned because the
// standard library's debug settings change its size.
#include <core/base/size_table.h>
#include <domain/doc/document.h>
#include <domain/doc/partition.h>

using namespace engine;
using namespace engine::doc;

ENGINE_EXPECT_SIZE(72, 8, ResolvedObject);
// One tile of a layer's grid: a key in the index's maps and in every dirty-tile set.
ENGINE_EXPECT_SIZE(8, 4, TileCoord);
