// Size table for core/containers (ADR-0019). These are layout-stable regardless of the
// standard library's debug settings because the containers manage their own storage.
#include <core/base/size_table.h>
#include <core/base/types.h>
#include <core/containers/fixed_vector.h>
#include <core/containers/flat_map.h>
#include <core/containers/flat_set.h>
#include <core/containers/hash_map.h>
#include <core/containers/hash_set.h>
#include <core/containers/slot_map.h>
#include <core/containers/small_vector.h>
#include <core/memory/arena.h>

#include <string>

using namespace engine;

// Flat containers: 8-byte pointer + two 32-bit counts; empty comparator and allocator fold.
ENGINE_EXPECT_SIZE(16, 8, FlatMap<u32, u32>);
ENGINE_EXPECT_SIZE(16, 8, FlatMap<u64, std::string>);
ENGINE_EXPECT_SIZE(16, 8, FlatMap<std::string, u32>);
ENGINE_EXPECT_SIZE(16, 8, FlatSet<u32>);
ENGINE_EXPECT_SIZE(16, 8, FlatSet<std::string>);
// A 64-bit size type costs 8 more bytes; use it only when a single map may exceed 2^32 entries.
ENGINE_EXPECT_SIZE(24, 8, FlatMap<u32, u32, std::less<>, u64>);
ENGINE_EXPECT_SIZE(24, 8, FlatSet<u32, std::less<>, u64>);
// An arena allocator costs one pointer.
ENGINE_EXPECT_SIZE(24, 8, FlatMap<u32, u32, std::less<>, u32, mem::ArenaAlloc>);
ENGINE_EXPECT_SIZE(16, 8, FlatMap<u32, u32>::iterator);

// Hash containers: two pointers, three 32-bit counts, one byte of shift; empties fold.
ENGINE_EXPECT_SIZE(32, 8, HashMap<u32, u32>);
ENGINE_EXPECT_SIZE(32, 8, HashMap<std::string, std::string>);
ENGINE_EXPECT_SIZE(32, 8, HashSet<u32>);
ENGINE_EXPECT_SIZE(32, 8, HashSet<std::string>);
ENGINE_EXPECT_SIZE(8, 4, containers::detail::HashBucket<u32>);

// Slot map: two pointers and five 32-bit fields.
ENGINE_EXPECT_SIZE(40, 8, SlotMap<u32>);
ENGINE_EXPECT_SIZE(40, 8, SlotMap<std::string>);
ENGINE_EXPECT_SIZE(8, 4, SlotHandle);

// Small and fixed vectors: header plus inline storage.
ENGINE_EXPECT_SIZE(16 + 8 * 4, 8, SmallVector<u32, 8>);
ENGINE_EXPECT_SIZE(16 + 4 * 8, 8, SmallVector<u64, 4>);
ENGINE_EXPECT_SIZE(4 + 8 * 4, 4, FixedVector<u32, 8>);
ENGINE_EXPECT_SIZE(8 + 4 * 8, 8, FixedVector<u64, 4>);
