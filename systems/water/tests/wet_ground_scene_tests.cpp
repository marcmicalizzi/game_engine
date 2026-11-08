// Wet ground's scene half (roadmap S99; docs/subsystems/water.md, "Channels"): a channel's bank
// records as the renderer's reader writes them — the fringe of the bank's material from the pinned
// table, the spray band of the stretch's regime — and the banks' tree, which must find at every
// point what testing every stretch finds. No device.

#include <core/math/math.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/wet_ground.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

// The saturation of every stretch tested one by one, with the traversal's own formula: the tree's
// answer must be this.
f32 every_stretch(const Vector<gfx::WaterShore>& leaves, Vec3 local) {
  f32 best = 0.0f;
  for (const gfx::WaterShore& leaf : leaves) {
    Vector<gfx::WaterShore> one;
    one.push_back(leaf);
    best = std::max(best, gfx::bank_tree_saturation(one, local, 0));
  }
  return best;
}

}  // namespace

TEST_CASE("water banks: a channel's stretches carry its bank's fringe and its regime's spray") {
  const test::TempDir tmp("water_banks_records");
  const std::string path = tmp.path() + "/scene.json";
  {
    std::ofstream out(path, std::ios::binary);
    // The brook of channel_scene_tests: the first stretch subcritical (Froude 0.93 at both ends),
    // the second ends at Froude 1.73 (Manning, n 0.04, 0.35 m deep at a slope of 0.067), and a
    // third that falls 3 m over 2 m.
    out << R"({
  "format": "engine.scene.v1",
  "name": "banks-test",
  "terrain": {"size": 65, "extent": 32, "seed": 4, "dune_height": 0},
  "water_channels": [{"name": "brook",
    "points": [[-12, 2.0, 4], [0, 1.8, 4], [12, 1.0, 4], [14, -2.0, 4]],
    "width_m": [3, 3, 2.5, 2.5], "depth_m": [0.5, 0.5, 0.35, 0.35],
    "slope": [0.017, 0.017, 0.067, 0.067], "seed": 3}]
}
)";
  }
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  const SceneWater* channel = nullptr;
  u32 instance = ~0u;
  for (u32 i = 0; i < desc.instances.size(); ++i) {
    const u32 m = desc.instances[i].mesh;
    if (m < desc.mesh_info.size() && desc.mesh_info[m].water.has_value() &&
        desc.mesh_info[m].water->channel) {
      channel = &*desc.mesh_info[m].water;
      instance = i;
    }
  }
  REQUIRE(channel != nullptr);
  REQUIRE(channel->banks.size() == 3u);
  const f32 fringe = gfx::capillary_fringe_m(gfx::k_bank_default);
  CHECK(fringe == doctest::Approx(0.428));  // fine sand, Lane and Washburn via Lohman
  CHECK(channel->banks[0].wet_m == fringe);
  CHECK(channel->banks[1].wet_m == fringe + gfx::k_spray_rapid_m);
  CHECK(channel->banks[2].wet_m == doctest::Approx(fringe + std::min(gfx::k_spray_fall_share * 3.0f,
                                                                     gfx::k_spray_fall_max_m)));
  // Local to where the instance stands: its origin plus its offset.
  const SceneInstance& at = desc.instances[instance];
  const f64 x0 = at.origin.x + static_cast<f64>(at.transform.position.x);
  const f64 y0 = at.origin.y + static_cast<f64>(at.transform.position.y);
  const f64 z0 = at.origin.z + static_cast<f64>(at.transform.position.z);
  CHECK(static_cast<f64>(channel->banks[0].stretch_a.x) == doctest::Approx(-12.0 - x0));
  CHECK(static_cast<f64>(channel->banks[0].stretch_a.y) == doctest::Approx(4.0 - z0));
  CHECK(static_cast<f64>(channel->banks[0].stretch_a.z) == doctest::Approx(2.0 - y0));
  CHECK(channel->banks[0].stretch_a.w == 1.5f);
  CHECK(channel->banks[1].stretch_b.w == 1.25f);
  for (const gfx::WaterShore& bank : channel->banks) {
    CHECK(bank.material == 0xffffffffu);  // never a body's record
    CHECK(bank.run_up_m == 0.0f);         // no swash
  }
}

TEST_CASE("water banks: the fringe table is Lane and Washburn's, by the pack's role") {
  CHECK(gfx::capillary_fringe_m(gfx::BankMaterial::Gravel) == 0.025f);
  CHECK(gfx::capillary_fringe_m(gfx::BankMaterial::CoarseSand) == 0.135f);
  CHECK(gfx::capillary_fringe_m(gfx::BankMaterial::Sand) == 0.246f);
  CHECK(gfx::capillary_fringe_m(gfx::BankMaterial::FineSand) == 0.428f);
  CHECK(gfx::capillary_fringe_m(gfx::BankMaterial::Silt) == 1.055f);
  CHECK(gfx::bank_material_of_role("gravel") == gfx::BankMaterial::Gravel);
  CHECK(gfx::bank_material_of_role("sand") == gfx::BankMaterial::Sand);
  CHECK(gfx::bank_material_of_role("soil") == gfx::k_bank_default);
  CHECK(gfx::bank_material_of_role("silt") == gfx::BankMaterial::Silt);
  // Finer grains hold water higher.
  for (u8 m = 1; m < 5; ++m)
    CHECK(gfx::k_capillary_fringe_m[m] > gfx::k_capillary_fringe_m[m - 1]);
}

TEST_CASE("water banks: the tree finds what testing every stretch finds, and little far away") {
  // A meandering channel of 3,000 stretches 3 m long falling 1 m in 100, and a second crossing it.
  Vector<gfx::WaterShore> leaves;
  const auto add = [&](Vec3 a, Vec3 b, f32 half, f32 fringe) {
    gfx::WaterShore r;
    r.wet_m = fringe;
    r.material = 0xffffffffu;
    r.stretch_a = Vec4{a.x, a.z, a.y, half};
    r.stretch_b = Vec4{b.x, b.z, b.y, half};
    const f32 grow = half + 25.0f * fringe + 1.0f;
    r.lo = Vec2{std::min(a.x, b.x) - grow, std::min(a.z, b.z) - grow};
    r.hi = Vec2{std::max(a.x, b.x) + grow, std::max(a.z, b.z) + grow};
    leaves.push_back(r);
  };
  Vec3 p{0.0f, 50.0f, 0.0f};
  for (u32 i = 0; i < 3000; ++i) {
    const f32 heading = 0.6f * std::sin(0.013f * static_cast<f32>(i));
    const Vec3 q{p.x + 3.0f * std::cos(heading), p.y - 0.03f, p.z + 3.0f * std::sin(heading)};
    add(p, q, 1.5f, i % 7 == 0 ? 0.678f : 0.428f);
    p = q;
  }
  for (u32 i = 0; i < 200; ++i) {
    const Vec3 a{4000.0f, 20.0f, -300.0f + 3.0f * static_cast<f32>(i)};
    add(a, Vec3{a.x, a.y - 0.02f, a.z + 3.0f}, 1.0f, 0.246f);
  }
  Vector<gfx::WaterShore> buffer;
  gfx::WaterShore body;  // a body's record first, as the GPU scene writes them
  buffer.push_back(body);
  Vector<gfx::WaterShore> sorted = leaves;
  gfx::build_bank_tree(sorted, 7u, 3u, buffer);
  CHECK(buffer.size() == 1u + 2u * leaves.size() - 1u);
  u32 mismatches = 0, wet = 0, far_visits = 0, far_points = 0;
  u32 state = 12345u;
  const auto next = [&]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<f32>(state >> 8) / 16777216.0f;
  };
  for (u32 k = 0; k < 4000; ++k) {
    // Half the points near the channel's line (heights about its surface), half anywhere.
    const u32 at = static_cast<u32>(next() * 3199.0f);
    const gfx::WaterShore& s = leaves[at];
    Vec3 local;
    if (k % 2 == 0) {
      local = Vec3{s.stretch_a.x + (next() - 0.5f) * 12.0f, s.stretch_a.z + (next() - 0.3f) * 1.5f,
                   s.stretch_a.y + (next() - 0.5f) * 12.0f};
    } else {
      local = Vec3{(next() - 0.25f) * 9000.0f, (next() - 0.5f) * 200.0f, (next() - 0.5f) * 3000.0f};
    }
    u32 visits = 0;
    const f32 tree = gfx::bank_tree_saturation(buffer, local, 1u, &visits);
    const f32 brute = every_stretch(leaves, local);
    if (tree != brute) ++mismatches;
    if (brute > 0.0f) ++wet;
    if (k % 2 == 1 && brute == 0.0f) {
      far_visits += visits;
      ++far_points;
    }
  }
  MESSAGE("banks' tree: " << wet << " wet points of 4000; far points visit "
                          << (far_points ? far_visits / far_points : 0) << " records on average");
  CHECK(mismatches == 0u);
  CHECK(wet > 500u);
  REQUIRE(far_points > 0u);
  CHECK(far_visits / far_points < 64u);  // of 6,399 records
}
