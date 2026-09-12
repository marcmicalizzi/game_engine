#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/base/types.h>

#include <doctest/doctest.h>

#include <type_traits>

using namespace engine;

TEST_CASE("base: fixed-width aliases have the expected widths") {
  static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);
  static_assert(sizeof(i8) == 1 && sizeof(i16) == 2 && sizeof(i32) == 4 && sizeof(i64) == 8);
  static_assert(std::is_unsigned_v<usize> && std::is_signed_v<isize>);
  CHECK(true);
}

TEST_CASE("base: exactly one compiler and one platform macro is set") {
  static_assert(ENGINE_COMPILER_MSVC + ENGINE_COMPILER_CLANG + ENGINE_COMPILER_GCC == 1);
  static_assert(ENGINE_PLATFORM_WINDOWS + ENGINE_PLATFORM_LINUX == 1);
  CHECK(true);
}

TEST_CASE("base: ENGINE_DEBUG tracks NDEBUG") {
#if defined(NDEBUG)
  static_assert(ENGINE_DEBUG == 0);
#else
  static_assert(ENGINE_DEBUG == 1);
#endif
  CHECK(true);
}

TEST_CASE("base: ENGINE_VERIFY and ENGINE_ASSERT pass on true and compile in expressions") {
  int evaluations = 0;
  ENGINE_VERIFY(++evaluations == 1, "verify evaluates its expression exactly once");
  CHECK(evaluations == 1);

  // ENGINE_ASSERT must not evaluate its expression in release builds.
  ENGINE_ASSERT(++evaluations > 0, "assert expression");
  CHECK(evaluations == (ENGINE_DEBUG ? 2 : 1));
}

TEST_CASE("base: ENGINE_NO_UNIQUE_ADDRESS folds empty members") {
  struct Empty {};
  struct Holder {
    void* p;
    ENGINE_NO_UNIQUE_ADDRESS Empty e;
  };
  static_assert(sizeof(Holder) == sizeof(void*));
  CHECK(true);
}
