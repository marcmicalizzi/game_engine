// The landform's bytes (docs/subsystems/landform.md, "The same bytes everywhere"): a fixed
// sketch's heightfield and each of its fields hash to values pinned on MSVC, which GCC and Clang
// at x86-64-v3 and GCC at v2 reproduce in the containers, and the same sketch solved on one, four
// and sixteen workers is the serial solve byte for byte.
#include "test_sketches.h"

#include <core/jobs/job_system.h>

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

using namespace engine;
using namespace landform_test;

namespace {

// Pinned on MSVC (msvc-debug), 2026-11-10. A change that moves one re-pins it with a
// sentence in the commit saying what moved it, and bumps `k_solver_version`.
constexpr u64 k_height_hash = 0x49c3b7110359e1c9ull;
constexpr u64 k_area_hash = 0x3482c041039819c2ull;
constexpr u64 k_receiver_hash = 0x4f7479694798fe45ull;
constexpr u64 k_slope_hash = 0xe6dad71620f6b739ull;
constexpr u64 k_erosion_hash = 0x0cc2b80c2f31fd27ull;
constexpr u64 k_fill_hash = 0xe6eca1925a3a18d7ull;

}  // namespace

TEST_CASE("landform: the golden sketch solves to the pinned bytes on every toolchain") {
  const landform::Sketch sketch = golden_sketch();
  landform::Landform land;
  std::string error;
  REQUIRE_MESSAGE(landform::solve(sketch, nullptr, {}, land, error), error);
  std::printf(
      "landform golden: height %016llx area %016llx receiver %016llx slope %016llx "
      "erosion %016llx fill %016llx\n",
      static_cast<unsigned long long>(hash_of(land.height_mm)),
      static_cast<unsigned long long>(hash_of(land.drainage_area)),
      static_cast<unsigned long long>(hash_of(land.receiver)),
      static_cast<unsigned long long>(hash_of(land.slope)),
      static_cast<unsigned long long>(hash_of(land.erosion_rate)),
      static_cast<unsigned long long>(hash_of(land.fill)));
  CHECK(hash_of(land.height_mm) == k_height_hash);
  CHECK(hash_of(land.drainage_area) == k_area_hash);
  CHECK(hash_of(land.receiver) == k_receiver_hash);
  CHECK(hash_of(land.slope) == k_slope_hash);
  CHECK(hash_of(land.erosion_rate) == k_erosion_hash);
  CHECK(hash_of(land.fill) == k_fill_hash);
}

TEST_CASE("landform: one, four and sixteen workers solve the serial solve's bytes") {
  const landform::Sketch sketch = golden_sketch();
  landform::Landform serial;
  std::string error;
  REQUIRE_MESSAGE(landform::solve(sketch, nullptr, {}, serial, error), error);
  for (const u32 workers : {1u, 4u, 16u}) {
    CAPTURE(workers);
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    config.pin_threads = false;
    jobs::JobSystem jobs(config);
    landform::SolveOptions options;
    options.jobs = &jobs;
    landform::Landform land;
    REQUIRE_MESSAGE(landform::solve(sketch, nullptr, options, land, error), error);
    CHECK(hash_of(land.height_mm) == hash_of(serial.height_mm));
    CHECK(hash_of(land.drainage_area) == hash_of(serial.drainage_area));
    CHECK(hash_of(land.receiver) == hash_of(serial.receiver));
    CHECK(hash_of(land.slope) == hash_of(serial.slope));
    CHECK(hash_of(land.erosion_rate) == hash_of(serial.erosion_rate));
    CHECK(hash_of(land.fill) == hash_of(serial.fill));
  }
}
