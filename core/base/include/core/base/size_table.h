#pragma once

// Size table entries (docs/plan/11-performance-principles.md section 11.2, ADR-0019).
//
// Every hot type records its expected size and alignment in its module's tests/size_table.cpp:
//
//     ENGINE_EXPECT_SIZE(16, 8, FlatMap<u32, u32>);
//
// The type comes last so template arguments containing commas need no extra parentheses.
// Changing a hot type's size means updating the entry and justifying the change in the commit.
//
// Do not put types that wrap standard-library containers in the size table: their size varies
// with the standard library's debug settings. Engine containers are layout-stable by design.

#define ENGINE_EXPECT_SIZE(ExpectedSize, ExpectedAlign, ...)                                       \
  static_assert(sizeof(__VA_ARGS__) == (ExpectedSize),                                             \
                "size table: " #__VA_ARGS__ " is not " #ExpectedSize                               \
                " bytes; update the size table and justify the change");                           \
  static_assert(alignof(__VA_ARGS__) == (ExpectedAlign),                                           \
                "size table: " #__VA_ARGS__ " alignment is not " #ExpectedAlign)
