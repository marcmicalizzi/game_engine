// Size table for domain/tileset (ADR-0019): the four types laid over bytes on disk, the archive's
// header and its table-of-contents entry (archive.h) and the height and mask tiles' record headers
// (records.h). A change to any of them is a change to every level's file, so it moves
// `k_archive_version` or the record's own version with it.
#include "../src/archive.h"
#include "../src/records.h"

#include <core/base/size_table.h>

using namespace engine;

ENGINE_EXPECT_SIZE(64, 8, tileset::archive::Header);
ENGINE_EXPECT_SIZE(24, 8, tileset::archive::Entry);
ENGINE_EXPECT_SIZE(32, 8, tileset::TileHeader);
ENGINE_EXPECT_SIZE(32, 8, tileset::MaskHeader);
