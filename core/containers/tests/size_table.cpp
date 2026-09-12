// Size table for core/containers (ADR-0019). These are layout-stable regardless of the
// standard library's debug settings because the containers manage their own storage.
#include <core/base/size_table.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/flat_set.h>

#include <string>

using namespace engine;

// 8-byte pointer + two 32-bit counts; the empty comparator folds into padding.
ENGINE_EXPECT_SIZE(16, 8, FlatMap<u32, u32>);
ENGINE_EXPECT_SIZE(16, 8, FlatMap<u64, std::string>);
ENGINE_EXPECT_SIZE(16, 8, FlatMap<std::string, u32>);
ENGINE_EXPECT_SIZE(16, 8, FlatSet<u32>);
ENGINE_EXPECT_SIZE(16, 8, FlatSet<std::string>);

// A 64-bit size type costs 8 more bytes; use it only when a single map may exceed 2^32 entries.
ENGINE_EXPECT_SIZE(24, 8, FlatMap<u32, u32, std::less<>, u64>);
ENGINE_EXPECT_SIZE(24, 8, FlatSet<u32, std::less<>, u64>);

// Iterators are a pointer plus an index.
ENGINE_EXPECT_SIZE(16, 8, FlatMap<u32, u32>::iterator);
