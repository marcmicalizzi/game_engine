# base (core)

**Purpose.** The smallest layer everything else stands on: compiler and platform detection, fixed-width type aliases, assertions, and the size-table macro. No dependencies.

**Owned data.** None at runtime.

**Invariants.**
- Exactly one `ENGINE_COMPILER_*` and one `ENGINE_PLATFORM_*` macro is 1; all are defined to 0 or 1 so `#if` is always well-formed.
- `ENGINE_DEBUG` is 1 exactly when `NDEBUG` is not defined.
- `ENGINE_ASSERT` does not evaluate its expression in release builds; `ENGINE_VERIFY` always does.
- `assert_fail` never returns.

**Public API.**
- `core/base/macros.h`: `ENGINE_COMPILER_*`, `ENGINE_PLATFORM_*`, `ENGINE_DEBUG`, `ENGINE_FORCE_INLINE`, `ENGINE_NO_INLINE`, `ENGINE_RESTRICT`, `ENGINE_NO_UNIQUE_ADDRESS`, `ENGINE_DEBUG_BREAK`, `ENGINE_NON_COPYABLE`, stringize and concat helpers.
- `core/base/types.h`: `u8..u64`, `i8..i64`, `usize`, `isize`, `f32`, `f64` in `namespace engine`.
- `core/base/assert.h`: `ENGINE_ASSERT(expr, msg)`, `ENGINE_VERIFY(expr, msg)`, `ENGINE_UNREACHABLE(msg)`, `engine::assert_fail`.
- `core/base/size_table.h`: `ENGINE_EXPECT_SIZE(size, align, Type...)`.

**Depends on.** Nothing.

**Testing.** `tools/dev.ps1 test -Filter base`. Tests cover macro consistency, assertion evaluation semantics, and empty-member folding.

**Performance notes.** Header-only apart from `assert_fail`; nothing here is on a hot path.
