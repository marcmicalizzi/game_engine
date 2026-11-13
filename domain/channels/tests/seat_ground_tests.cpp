// W116 (docs/subsystems/channels.md, "A line across a slope"): the rule held on the tile set's own
// edited ground. The cuts the plan writes are compiled by `tileset::TerrainEdits`, the function the
// tile set's build applies to every lattice point (a test-only dependency: this file is built only
// where the tileset capability is), and the section they describe is read across every point of a
// stream along a hillside's contour, against the ground before the cut.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/channels.h>
#include <domain/tileset/cuts.h>

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace engine;
using namespace engine::channels;

namespace {
// The same hillside as channels_tests.cpp's `contour`: falling across the line toward z = 0 at the
// tangent the context holds, a bench a metre wide at its toe at z = 128 m, 1.2 % down +x.
f64 contour(const void* context, i64 x, i64 z) noexcept {
  const f64 t = *static_cast<const f64*>(context);
  const f64 a = static_cast<f64>(z) / 1000.0 - 128;
  const f64 across = a >= 0 ? t * a : (a >= -1 ? -.03 * a : .03 - t * (-a - 1));
  return 40 - .012 * static_cast<f64>(x) / 1000.0 + across;
}
constexpr f64 k_cross_slopes[3] = {.17632698070846498, .36397023426620234, .57735026918962573};
constexpr i64 k_cell_mm = 250;  // the finest level a forest's set is built at
}  // namespace

// What the edits say the section is (`surface_mm`, the lowest upper envelope of every cut) is read
// across each point of the cut, from the line out to the further of its bank top and its flood bed
// on each side, at nine places a side: none stands over the ground there by more than 2 cm (a
// place where the section stands over the ground is one the cut, which only lowers, leaves in the
// air). And the water: at each point of its description the edited ground at the handed width's
// two edges stands at or over its surface less 2 cm (the width never passes a bank), and at the
// ribbon's two edges a quarter past them, over it less 3 cm (the fit's own tolerance). On the base
// (f228814b) the section stood over the hillside on every case, by up to a metre at 30 degrees.
TEST_CASE("channels: on the edited ground nothing of a cross-slope's section stands in the air") {
  for (u32 c = 0; c < 3; ++c) {
    for (u64 seed : {1ull, 2ull}) {
      ChannelLayer p;
      p.name = "test";
      p.region_min = {0, 0};
      p.region_max = {256, 256};
      p.spacing_m = 1;
      p.head_threshold_m2 = 60;
      p.width_at_km2_m = 12;
      p.depth_at_km2_m = 1.0f;
      p.seed = seed;
      scene_gen::Ground g;
      g.surface_fn = &contour;
      g.context = &k_cross_slopes[c];
      ChannelNetwork n;
      std::string error;
      REQUIRE(plan_channels(p, g, seed, nullptr, n, error));
      const JsonValue cuts = terrain_cuts_json(p, n);
      std::vector<scene::TerrainEdit> edits;
      for (const JsonValue& o : cuts.as_array()) {
        scene::TerrainEdit e;
        schema::ReadContext reader;
        REQUIRE(schema::from_json(e, o, reader));
        REQUIRE(reader.ok());
        edits.push_back(std::move(e));
      }
      tileset::TerrainEdits compiled;
      REQUIRE(compiled.build({edits.data(), edits.size()}, &error));
      const auto ground_at = [&](f64 x, f64 z) {
        return contour(g.context, std::llround(x * 1000.0), std::llround(z * 1000.0));
      };
      u64 read = 0, over = 0;
      f64 most = 0;
      for (const scene::TerrainEdit& e : edits) {
        const u32 m = static_cast<u32>(e.points.size());
        for (u32 i = 1; i + 1 < m; ++i) {
          const f64 w = static_cast<f64>(e.width_m[i]);
          if (w < 1.0) continue;
          const f64 tx = e.points[i + 1].x - e.points[i - 1].x;
          const f64 tz = e.points[i + 1].z - e.points[i - 1].z;
          const f64 tl = std::sqrt(tx * tx + tz * tz);
          if (!(tl > 0)) continue;
          const f64 lx = -tz / tl, lz = tx / tl;
          for (const f64 side : {1.0, -1.0}) {
            f64 reach = .5 * w;
            const auto& flood = side > 0 ? e.flood_left_m : e.flood_right_m;
            if (i < flood.size()) reach = std::max(reach, static_cast<f64>(flood[i]));
            for (u32 k = 0; k <= 8; ++k) {
              // Short of the edge by a centimetre: the faces rise from there.
              const f64 d = (reach - .01) * static_cast<f64>(k) / 8;
              const f64 x = e.points[i].x + side * lx * d, z = e.points[i].z + side * lz * d;
              f64 s_mm = 0;
              if (!compiled.surface_mm(x * 1000.0, z * 1000.0, k_cell_mm, s_mm)) continue;
              ++read;
              const f64 above = s_mm / 1000.0 - ground_at(x, z);
              over += above > k_seat_bound_m;
              most = std::max(most, above);
            }
          }
        }
      }
      const JsonValue water = water_channels_json(p, n, 0, .5f, true, &g);
      u64 edges = 0, outside = 0, ribbon_over = 0;
      f64 worst = 0, ribbon_worst = 0;
      for (const JsonValue& block : water.as_array()) {
        const auto& points = block.find("points")->as_array();
        const auto& widths = block.find("width_m")->as_array();
        // The cut of the same link (no water body here, so no point is trimmed off the ribbon).
        const scene::TerrainEdit* cut = nullptr;
        for (const scene::TerrainEdit& e : edits)
          if (e.name == block.find("name")->as_string()) cut = &e;
        REQUIRE(cut != nullptr);
        REQUIRE(cut->points.size() == points.size());
        for (u32 i = 1; i + 1 < static_cast<u32>(points.size()); ++i) {
          f64 x = 0, y = 0, z = 0, w = 0, ax = 0, az = 0, bx = 0, bz = 0;
          REQUIRE(points[i].as_array()[0].get_f64(x));
          REQUIRE(points[i].as_array()[1].get_f64(y));
          REQUIRE(points[i].as_array()[2].get_f64(z));
          REQUIRE(widths[i].get_f64(w));
          REQUIRE(points[i - 1].as_array()[0].get_f64(ax));
          REQUIRE(points[i - 1].as_array()[2].get_f64(az));
          REQUIRE(points[i + 1].as_array()[0].get_f64(bx));
          REQUIRE(points[i + 1].as_array()[2].get_f64(bz));
          const f64 tl = std::hypot(bx - ax, bz - az);
          if (!(tl > 0)) continue;
          const f64 lx = -(bz - az) / tl, lz = (bx - ax) / tl;
          for (const f64 side : {1.0, -1.0}) {
            for (const f64 share : {.5, k_ribbon_half_widths}) {
              const f64 ex = x + side * lx * share * w, ez = z + side * lz * share * w;
              const f64 g_mm = ground_at(ex, ez) * 1000.0;
              f64 e_mm = g_mm;
              compiled.edited_mm(ex * 1000.0, ez * 1000.0, k_cell_mm, g_mm, e_mm);
              const f64 above = y - e_mm / 1000.0;
              ++edges;
              if (share == .5) {
                // The handed width's edge passes a bank where it lies past the section's bank top
                // and the ground there is under the water: the water would stand over a hillside.
                // Inside the section it lies on a bank's face, as bankfull water's edge does.
                if (.5 * w <= .5 * static_cast<f64>(cut->width_m[i]) + .01) continue;
                outside += above > k_seat_bound_m;
                worst = std::max(worst, above);
              } else {
                // The fit's own tolerance (3 cm), and the millimetre its bisection stops within.
                ribbon_over += above > .031;
                ribbon_worst = std::max(ribbon_worst, above);
              }
            }
          }
        }
      }
      MESSAGE("cross-slope " << k_cross_slopes[c] << " seed " << seed << ": " << read
                             << " places of the section, over the ground at " << over << " (most "
                             << most << " m); water edges " << edges << ", the handed width past a "
                             << "bank at " << outside << " (most " << worst << " m), the ribbon's "
                             << "edge over the edited ground at " << ribbon_over << " (most "
                             << ribbon_worst << " m)");
      REQUIRE(read > 2000);
      CHECK(over == 0);
      CHECK(most <= k_seat_bound_m);
      REQUIRE(edges > 500);
      CHECK(outside == 0);
      CHECK(ribbon_over == 0);
    }
  }
}
