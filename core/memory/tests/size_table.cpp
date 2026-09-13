// Size table for core/memory (ADR-0019).
#include <core/base/size_table.h>
#include <core/memory/allocator.h>
#include <core/memory/arena.h>
#include <core/memory/memory.h>

using namespace engine;

ENGINE_EXPECT_SIZE(2, 2, mem::TagId);
ENGINE_EXPECT_SIZE(1, 1, mem::DefaultAlloc);  // empty; folds away under ENGINE_NO_UNIQUE_ADDRESS
ENGINE_EXPECT_SIZE(8, 8, mem::ArenaAlloc);
ENGINE_EXPECT_SIZE(40, 8, mem::Arena);  // first, current, chunk_bytes, bytes_reserved, chunk_count
ENGINE_EXPECT_SIZE(16, 8, mem::Arena::Mark);
