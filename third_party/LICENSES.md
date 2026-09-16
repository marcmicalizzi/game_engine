# Third-party components

Every dependency of the engine is listed here with its license. Policy: permissive licenses only (MIT, BSD, zlib, Apache-2.0, BSL-1.0, public domain); LGPL only as a dynamically linked, user-replaceable library; no GPL/AGPL/SSPL; proprietary SDKs only as optional developer-supplied plugins outside this repository. See docs/plan/08-toolchain.md section 8.8 and ADR-0014.

| Component | Version | License | Used by | How obtained |
|---|---|---|---|---|
| doctest | v2.4.11 | MIT | tests only | CMake FetchContent at configure time |
| Tracy | v0.14.1 | BSD-3-Clause | core/profiling (only when ENGINE_TRACY is on) | CMake FetchContent at configure time |
| Vulkan-Headers | vulkan-sdk-1.4.357.0 | Apache-2.0 OR MIT | domain/gfx | CMake FetchContent at configure time |
| volk | vulkan-sdk-1.4.357.0 | MIT | domain/gfx (Vulkan meta-loader) | CMake FetchContent at configure time |
| Vulkan Memory Allocator | v3.4.0 | MIT | domain/gfx | CMake FetchContent at configure time |
| Slang | v2026.17.1 | Apache-2.0 WITH LLVM-exception | shader compiler (build time only; prebuilt binaries) | CMake FetchContent of the release archive at configure time |
| meshoptimizer | v1.2 | MIT | domain/geometry (meshlets, bounds, cluster LOD) | CMake FetchContent at configure time |
| SDL3 | release-3.4.16 | zlib | foundation/window (windowing, input, Vulkan surfaces) | CMake FetchContent at configure time; static, video and events only |
