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
- `core/base/size_table.h`: `ENGINE_EXPECT_SIZE(size, align, Type...)` — size and alignment first and the type last, so a template argument's commas need no parentheses. A module's entries live in its `tests/size_table.cpp`, and `tools/lint.ps1` (`size-table`) holds every module to having one, compiled and with at least one entry, or a reasoned line in `tools/size-table-exemptions.txt` (AGENTS.md, "Size table").

**Depends on.** Nothing.

**Testing.** `tools/dev.ps1 test -Filter base`. Tests cover macro consistency, assertion evaluation semantics, and empty-member folding. **A compiler canary lives here too** (`tests/compiler_canary_tests.cpp`, T197), because this is the one module every build compiles and it depends on nothing: the shape of code MSVC 19.44 and 19.51 miscompile at `/O2` (an aggregate passed by value to two calls in one expression, in a function the build inlines), compiled with the build's own flags and checked against the scalar answer and a pin. Its first case fails, naming the compiler, in a build whose flags inline the function (`/Ob2`); its second reports, as a warning and not a failure until the tree carries a workaround, that the form main's `/Ob1` reaches is miscompiled too ([the miscompile](../experiments/msvc-ob2-avx2-miscompile.md)). Never make it pass by changing the test.

**Performance notes.** Header-only apart from `assert_fail`; nothing here is on a hot path.
