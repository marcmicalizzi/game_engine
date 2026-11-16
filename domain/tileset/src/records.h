#pragma once

// The two records a level's archive holds (archive.h), here rather than in the files that read and
// write them so that the size table (tests/size_table.cpp) can pin them with the archive's own
// header and entry: they are laid over bytes on disk.

#include <core/base/types.h>

namespace engine::tileset {

// A tile's record in its level's archive (archive.h), byte for byte what its own file held before
// the archive (k_tileset_version 1): this header, then (cells + 3)^2 little-endian i32 millimetres,
// rows of x in order of z, from the point one before the tile's first to one after its last. The
// engine's targets are x86-64, little-endian, so the integers are written as they lie. Its version
// is the record's own, 1, and moves only when a record's layout does: the archive moved the set's
// version and left every tile's bytes, and so every level's hash, as they were.
inline constexpr u32 k_tile_record_version = 1;
struct TileHeader {
  char magic[4] = {'E', 'T', 'I', 'L'};
  u32 version = k_tile_record_version;
  u32 cells = 0;
  u32 reserved = 0;
  i64 spacing_mm = 0;
  i32 tx = 0;
  i32 tz = 0;
};
static_assert(sizeof(TileHeader) == 32);

// A mask tile's record in the level's archive (archive.h), byte for byte what its own `.mask` file
// held before the archive (k_mask_version 1), followed by the words the GPU reads (masks.h, "A
// tile's record"). Its version is the record's own, and moves only when a record's layout does.
inline constexpr u32 k_mask_record_version = 1;
struct MaskHeader {
  char magic[4] = {'E', 'M', 'S', 'K'};
  u32 version = k_mask_record_version;
  u32 cells = 0;
  u32 words = 0;
  i64 spacing_mm = 0;
  i32 tx = 0;
  i32 tz = 0;
};
static_assert(sizeof(MaskHeader) == 32);

}  // namespace engine::tileset
