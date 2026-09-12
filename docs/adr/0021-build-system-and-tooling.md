# ADR-0021: CMake + Ninja, `engine_module()` manifests, PowerShell and C++ tooling, no Python

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/08-toolchain.md §8.5, docs/plan/02-architecture.md §2.5, AGENTS.md

## Context

The build must work identically for humans, agents, and CI on Windows and Linux with the fewest possible toolchain prerequisites. The development machine has Visual Studio 2026 (bundled CMake 4.3, Ninja, MSVC 14.5x, clang-format, clang-tidy) and PowerShell 7, and no Python installation. Layering must be enforced mechanically, and agents need the module graph in a machine-readable form.

## Decision

CMake (3.28+) with Ninja and `CMakePresets.json` presets for MSVC, clang-cl, and Linux Clang/GCC, including sanitizer presets. Each module is declared once in its `CMakeLists.txt` through `engine_module(NAME LAYER DEPS ...)`; the function refuses dependencies on higher layers or undeclared modules at configure time and `engine_finalize_modules()` writes `build/<preset>/modules.json`. Tests use doctest via FetchContent and register with CTest through `engine_module_tests()`. All repository tooling is PowerShell 7 (`tools/dev.ps1`, `tools/lint.ps1`) or small C++ tools built by CMake; Python is not a dependency. The banned-pattern lint runs as a CTest test. `tools/dev.ps1` locates Visual Studio through vswhere and prefers its bundled CMake and Ninja so nothing beyond Visual Studio and PowerShell 7 needs to be on PATH. vcpkg manifest mode is adopted when the first non-header-only third-party dependency arrives.

## Consequences

One command builds, tests, and lints on every platform. The module graph is data. Bazel/Buck2 remain rejected for now.

## Revisit when

Build time dominates agent iteration, or a Linux-only contributor cannot run the tooling.
