// The landform's bytes (docs/subsystems/landform.md, "The same bytes everywhere"): a fixed
// sketch's heightfield and each of its fields (the sediment's among them) hash to values pinned on
// MSVC, which GCC and Clang at x86-64-v3 and GCC at v2 reproduce in the containers, and the same
// sketch solved on one, four and sixteen workers is the serial solve byte for byte.
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
// Version 2 (W95 stage B, deposition): every hash moved, because the rivers now lay down
// sediment (the golden sketch runs at the default G = 1) and the sediment field was added.
// Version 3 (structure): every hash moved again, because the golden sketch's rock is now a
// formation of dipping, folded beds in place of two elevation bands, and the rivers' sweeps end
// at a millimetre; the resistance field was added.
constexpr u64 k_height_hash = 0x12bb0b1a52cf248full;
constexpr u64 k_area_hash = 0x9e8842ec8f2beedaull;
constexpr u64 k_receiver_hash = 0xc3d198544e7d351full;
constexpr u64 k_slope_hash = 0x595bd6bbbe4d697cull;
constexpr u64 k_erosion_hash = 0xec6dc5b13efd398bull;
constexpr u64 k_fill_hash = 0x49ed8836e914e0e7ull;
constexpr u64 k_sediment_hash = 0x519c27a7d4a20979ull;
constexpr u64 k_resistance_hash = 0x17f0c4241eae4f90ull;

}  // namespace

TEST_CASE("landform: the golden sketch solves to the pinned bytes on every toolchain") {
  const landform::Sketch sketch = golden_sketch();
  landform::Landform land;
  std::string error;
  REQUIRE_MESSAGE(landform::solve(sketch, nullptr, {}, land, error), error);
  std::printf(
      "landform golden: height %016llx area %016llx receiver %016llx slope %016llx "
      "erosion %016llx fill %016llx sediment %016llx resistance %016llx\n",
      static_cast<unsigned long long>(hash_of(land.height_mm)),
      static_cast<unsigned long long>(hash_of(land.drainage_area)),
      static_cast<unsigned long long>(hash_of(land.receiver)),
      static_cast<unsigned long long>(hash_of(land.slope)),
      static_cast<unsigned long long>(hash_of(land.erosion_rate)),
      static_cast<unsigned long long>(hash_of(land.fill)),
      static_cast<unsigned long long>(hash_of(land.sediment)),
      static_cast<unsigned long long>(hash_of(land.resistance)));
  CHECK(hash_of(land.height_mm) == k_height_hash);
  CHECK(hash_of(land.drainage_area) == k_area_hash);
  CHECK(hash_of(land.receiver) == k_receiver_hash);
  CHECK(hash_of(land.slope) == k_slope_hash);
  CHECK(hash_of(land.erosion_rate) == k_erosion_hash);
  CHECK(hash_of(land.fill) == k_fill_hash);
  CHECK(hash_of(land.sediment) == k_sediment_hash);
  CHECK(hash_of(land.resistance) == k_resistance_hash);
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
    CHECK(hash_of(land.sediment) == hash_of(serial.sediment));
    CHECK(hash_of(land.resistance) == hash_of(serial.resistance));
    CHECK(land.river_sweeps == serial.river_sweeps);
    // The budget's sums are taken in one order whatever the workers: the same bits.
    CHECK(land.budget.eroded_m3 == serial.budget.eroded_m3);
    CHECK(land.budget.deposited_m3 == serial.budget.deposited_m3);
    CHECK(land.budget.left_m3 == serial.budget.left_m3);
    CHECK(land.budget.change_m3 == serial.budget.change_m3);
  }
}
