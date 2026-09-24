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
| meshoptimizer | v1.2 | MIT | domain/geometry (meshlets, bounds, cluster LOD); domain/atlas (the proxy the content build's `--atlas repack` charts on) | CMake FetchContent at configure time |
| cgltf | v1.15 | MIT | domain/assets (glTF 2.0 and GLB parsing) | CMake FetchContent at configure time; header-only, compiled in one translation unit |
| xatlas | repository commit f700c7790aaa030e794b52ba7791a05c085faf0c (2022-07-26; the project cuts no releases) | MIT (xatlas, Jonathan Young; and the two works it carries under the same licence, thekla_atlas — Thekla, Inc. and NVIDIA's Ignacio Castaño — and Fast-BVH, Brandon Pelfrey) plus BSD-3-Clause (the OpenNL sparse solver inside it, Bruno Levy, INRIA) | domain/atlas (the content build's `--atlas repack`: charting the proxy, or a small mesh directly, and packing; build machines only, nothing the renderer links) | CMake FetchContent of the repository at configure time (`cmake/EngineAtlas.cmake`); `source/xatlas/xatlas.cpp` compiled into a static library with `XA_MULTITHREADED=0` |
| SDL3 | release-3.4.16 | zlib | foundation/window (windowing, input, Vulkan surfaces) | CMake FetchContent at configure time; static, video and events only |
| stb_image | v2.30 (repository commit 2c980bb59875b0d32144a71867fbdebb2f77cd20) | MIT OR Unlicense (public domain) | foundation/image (PNG/JPEG/TGA/BMP decoding) | CMake FetchContent of the stb repository at configure time; header compiled into foundation/image/src/decode.cpp |
| stb_image_write | v1.16 (same commit) | MIT OR Unlicense (public domain) | tests only (foundation/image's baseline JPEG fixture) | Comes with the stb checkout above; included only by foundation/image/tests/decode_tests.cpp |
| flecs | v4.1.6 | MIT | domain/ecs (the runtime entity store; capability `ENGINE_WITH_ECS`) | CMake FetchContent at configure time, static, only when the capability is on |
| SQLite | 3.53.4 (amalgamation) | public domain | foundation/store (event log, projections, snapshots; capability `ENGINE_WITH_STORE`) | CMake FetchContent of the published amalgamation archive at configure time, pinned by URL and SHA-256, compiled into `engine_sqlite3`; only when the capability is on |
| Recast/Detour | v1.6.0 | zlib | domain/nav (tile building, path queries, crowd; capability `ENGINE_WITH_NAV`) | CMake FetchContent at configure time, static, only when the capability is on; the `Recast`, `Detour` and `DetourCrowd` targets only — the demo, the tests, `DebugUtils` and `DetourTileCache` are excluded |

## Consulted, not vendored

Nothing below is fetched, linked, or compiled into anything; each is recorded because a published
work was read closely enough while writing our own implementation that the debt should be visible.

| Work | License | What was taken | Where it is used |
|---|---|---|---|
| NVIDIA FLIP (github.com/NVlabs/flip) | BSD-3-Clause | The numeric constants of the LDR-FLIP evaluator, checked against the reference: the contrast-sensitivity Gaussians per opponent channel, the color-difference exponent and its redistribution, the feature-detection width and exponent, and the D65 primaries. No source code. | `foundation/image/src/metrics.cpp`, written from the paper (Andersson et al., *FLIP: A Difference Evaluator for Alternating Images*, HPG 2020) — see `docs/subsystems/image.md` |
