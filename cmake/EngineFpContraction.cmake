# Floating-point contraction is off for the whole tree (ADR-0035, docs/subsystems/geometry.md
# "The same bytes from every toolchain").
#
# A compiler may **contract** `a*b + c` into one fused multiply-add: one rounding where the source
# wrote two. GCC does it by default across whole inlined expressions (-ffp-contract=fast, which it
# keeps for C++ even in ISO mode), Clang within one source expression (-ffp-contract=on, and at -O0
# too: the backend still fuses what the front end marked), and both only where the instruction
# exists — x86-64-v3, not v2 (cmake/EngineCpuBaseline.cmake). MSVC's /fp:precise, the default and
# what this tree uses, never contracts, at either baseline. Left alone, the same source is three
# arithmetics: MSVC and every v2 build, GCC at v3, Clang at v3.
#
# **Why that matters, and why it is decided here rather than per module.** The content build's
# output is a derived-data cache entry addressed by a hash of its *input* (docs/plan/07-content-
# pipeline.md §7.3): an entry one machine writes, another reads — a shared cache, the package-tests
# zip, a container copied beside its source — and the key names no compiler. Measured 2026-09-24
# (docs/experiments/content-build-determinism.md): from the same glTF bytes MSVC, GCC 13 at v2 and
# GCC 14 at v2 build byte-identical containers, while GCC 13 and Clang 18 at v3 each build a DAG of
# their own (Suzanne 88, 87 and 87 clusters; the Corset 450, 443 and 447), all of it inside
# meshoptimizer's clusterizer, sphere fit and simplifier. Turning contraction off **for the content
# build's own translation units was measured and is not enough**: core/math is header-only, a
# function like `dot(Vec3, Vec3)` that the compiler does not inline — every call at -O0, a large one
# at -O2 — is a weak definition in every object that uses it, and the linker keeps whichever it met
# first. In linux-clang-debug's engine-content that was main.cpp's copy, fused, so the import's
# normals came out an ulp apart and three of eleven containers differed in their attributes while
# gltf.cpp itself was compiled without contraction. The only rule with no such hole is that no
# translation unit in a binary that can run the content build contracts — which is every binary.
#
# **What it costs**, measured the same day, and only on GCC and Clang at v3: MSVC, the primary
# build, changes not at all, because it never contracted, and a v2 build could not. Under GCC 13 at
# v3 the content build is about 12% slower (the six Tripo landmarks 59.0 s against 52.7 s, where
# MSVC takes 56.6 s), and the float-heavy runtime kernels lose what fusion gave them: up to 61% on
# core/math's quaternion-to-matrix and inverse rows, 10–31% on anim's skeleton kernels, about 10%
# on the limit surface, 4% on animation's tick (the ADR and the experiment have the table). Most of
# them are still faster than MSVC's build of the same code, which has never fused anything.
# A kernel that wants a fused multiply-add back says so in its source (std::fma, or an FMA
# intrinsic under ENGINE_CPU_BASELINE_V3): deterministic on every compiler that builds it, faster
# on MSVC too, and visible to its reader, instead of a decision each compiler makes differently.
#
# Before every add_subdirectory() and FetchContent, like the CPU baseline, so the engine, its
# tests, its host tools and every third-party library built here (meshoptimizer and xatlas above
# all) carry it. Jolt already asked for it (CROSS_PLATFORM_DETERMINISTIC, cmake/EnginePhysics.cmake).
# ENGINE_FP_CONTRACTION=0 tells source that cares (anim's tests compare two spellings of a product
# with == when nothing fuses).

include_guard(GLOBAL)

# Per language and per compiler, as generator expressions, because C is enabled later (by SDL,
# SQLite and flecs) and may be a different compiler from C++: nothing for cl.exe (/fp:precise
# never contracts), the GNU spelling for GCC and Clang, and the same behind /clang: for clang-cl
# (the clangcl-debug preset). A Clang C compiler is assumed to have the C++ one's driver, which
# holds for every preset in CMakePresets.json.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(_engine_clang_no_contract "/clang:-ffp-contract=off")
else()
  set(_engine_clang_no_contract "-ffp-contract=off")
endif()
add_compile_options(
  "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-ffp-contract=off>"
  "$<$<COMPILE_LANG_AND_ID:C,GNU>:-ffp-contract=off>"
  "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:${_engine_clang_no_contract}>"
  "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:${_engine_clang_no_contract}>")
add_compile_definitions(ENGINE_FP_CONTRACTION=0)
message(STATUS "engine floating-point contraction: off")
