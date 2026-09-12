# ADR-0013: C++20 as the only core language

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/08-toolchain.md §8.1

## Context

Rust was considered for compiler-enforced memory safety in agent-written code. The project owner's language is C++, the graphics and physics ecosystem is C++, and C++ is as close to the hardware as the project needs.

## Decision

The engine core is C++20 (C++23 features where MSVC and Clang both support them) and there is no second systems language. Memory safety for agent-written code is carried by: sanitizer builds in CI, warnings as errors, banned-pattern lint, spans and views at module boundaries, generated serialization, fuzzing of parsers and protocol handlers, and tracking sanitizer findings per thousand lines per phase. Inline assembly is not used; intrinsics and ISPC cover SIMD, and any asm block must come with a benchmark proving the compiler could not get there. Exceptions are not used in engine code; errors are returned.

## Consequences

One toolchain, one corpus for agents to draw on, direct use of every vendor SDK. The safety burden is procedural and measured rather than compiler-enforced.

## Revisit when

Not planned.
