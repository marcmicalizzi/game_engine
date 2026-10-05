// The dunes far from the origin (ADR-0053; docs/subsystems/terrain.md, "Far from the origin"): the
// ground provider's tile source and its point entry sample the field at integer millimetres, so a
// lattice point 1e8 m out is the field's own height there to the bit.
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/tile_source.h>
#include <domain/terrain/scene_ground.h>

#include <doctest/doctest.h>

#include <cmath>
#include <span>
#include <string>

using namespace engine;
using namespace engine::terrain;

// **The dunes as a tile source sample the field at the lattice's integers, anywhere.** A lattice
// point's height is the field at `i * spacing_mm`, which is exact for every index; until 2026-10-05
// the index went through the point's float32 metres and back (`to_mm(lattice.x(i))`), the same
// millimetre only while a float holds it — 16 km at any spacing, 4,000 km at 25 cm. The far points
// are the owner's 419 km, 10,000 km and 1e8 m, on the 25 cm lattice the renderer's finest tiles use
// and on the walker's 1 mm one: the window's heights are the field's own to the bit, and so is the
// point entry (`GroundProvider::height_mm`), which alone reaches past the 1 mm lattice's i32 index
// (2,147 km).
TEST_CASE("terrain: the dunes' tile source at a far lattice point is the field's own height") {
  scene::Terrain entry;
  REQUIRE(validate_terrain_entry(entry));
  // The fixed features are the entry's float arithmetic of metres; the default entry has none, so
  // what is compared is the field's sampling alone.
  REQUIRE(entry.ridges.empty());
  REQUIRE(entry.basins.empty());
  const scene_gen::GroundProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_ground(k_ground_provider);
  REQUIRE(desc != nullptr);
  scene_gen::GroundProvider ground;
  std::string error;
  REQUIRE_MESSAGE(desc->make(entry, scene_gen::Context{}, ground, &error), error);
  const DuneField* field = dune_field(ground);
  REQUIRE(field != nullptr);
  const scene_gen::TileSource tiles = ground.tiles();
  const f64 time = entry.time + 3600.0;
  const i64 time_us = static_cast<i64>(std::floor(time * 1'000'000.0 + 0.5));
  struct Far {
    i64 x_mm;
    i64 z_mm;
  };
  const Far sites[] = {{419'072'000, -419'072'000},
                       {10'000'000'000, 10'000'000'000},
                       {100'000'000'000, -100'000'000'000}};
  constexpr i64 k_i32_reach = (i64{1} << 31) - 8;
  for (const i64 spacing_mm : {i64{250}, i64{1}}) {
    for (const Far& site : sites) {
      const i64 i0 = site.x_mm / spacing_mm + 3;
      const i64 j0 = site.z_mm / spacing_mm - 5;
      const bool indexable =
          i0 > -k_i32_reach && i0 < k_i32_reach && j0 > -k_i32_reach && j0 < k_i32_reach;
      f32 window[4 * 4] = {};
      if (indexable) {
        REQUIRE(tiles.heights(time, spacing_mm, static_cast<i32>(i0), static_cast<i32>(j0), 4, 4,
                              std::span<f32>(window, 16)));
      }
      u32 off = 0;
      for (u32 j = 0; j < 4; ++j) {
        for (u32 i = 0; i < 4; ++i) {
          const i64 x = (i0 + i) * spacing_mm;
          const i64 z = (j0 + j) * spacing_mm;
          const f32 own = height_m(field->height_um(x, z, time_us, Detail::dunes));
          off += ground.height_mm(time, x, z) == own ? 0u : 1u;
          if (indexable) off += window[j * 4 + i] == own ? 0u : 1u;
        }
      }
      MESSAGE("spacing " << spacing_mm << " mm at x = " << site.x_mm << " mm: " << off
                         << " of the points off the field"
                         << std::string(
                                indexable ? "" : " (the point entry only: no i32 index reaches)"));
      CHECK(off == 0);
    }
  }
}
