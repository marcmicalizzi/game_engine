// Ledges (G142; rocks.md, "Exposed bedrock (G142)"): the resistant beds of a formation standing
// out of a slope. The pieces on a synthetic hillside with noses and hollows: where they stand and
// where they do not, tops in the bedding planes, triangles on what is seen, nothing proud of the
// ground behind, ends that fall to nothing, a tile's border, the far form, the bytes, and the
// generator's whole read against its tiles.
#include <core/json/json.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/gfx/formation.h>
#include <domain/rocks/cliffs.h>
#include <domain/rocks/exposure.h>
#include <domain/rocks/ledges.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::rocks;

namespace {

constexpr f64 k_turn = 6.283185307179586;
constexpr i64 k_tile_mm = 64000;

// A hillside rising along +x at `rise` (0.75: 36.9 degrees) with noses and hollows 40 m apart
// across z, 3 m high: a nose at z = 10 (mod 40), a hollow at z = 30. `det::sin`, so the ground is
// the same on every toolchain and the pins below are too.
struct Hill {
  f64 rise = 0.75;
  f64 wave = 3.0;
};
f64 hill(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const auto& h = *static_cast<const Hill*>(context);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  return h.rise * x + h.wave * det::sin(z * (k_turn / 40.0));
}
scene_gen::Ground ground_of(const Hill& h) {
  scene_gen::Ground g;
  g.surface_fn = &hill;
  g.context = &h;
  return g;
}

scene::FormationBed bed(const char* name, f32 thickness_m, f32 resistance) {
  scene::FormationBed b;
  b.name = name;
  b.thickness_m = thickness_m;
  b.resistance = resistance;
  return b;
}
// Beds dipping 12 degrees toward 30 degrees from +x: two sandstones, and a sandstone under a
// limestone that stand as one unit of two rows.
scene::Formation beds() {
  scene::Formation f;
  f.origin = WorldPos{5.0, 2.0, -3.0};
  f.dip_deg = 12.0f;
  f.azimuth_deg = 30.0f;
  f.beds.push_back(bed("shale", 1.5f, 0.1f));
  f.beds.push_back(bed("sandstone", 1.2f, 0.9f));
  f.beds.push_back(bed("mudstone", 0.8f, 0.2f));
  f.beds.push_back(bed("flaggy sandstone", 0.5f, 0.7f));
  f.beds.push_back(bed("limestone", 0.4f, 0.95f));
  return f;
}
LedgeLayer layer_of(const scene::Formation& f) {
  LedgeLayer layer;
  layer.name = "test";
  layer.seed = 11;
  layer.formation = f;
  layer.region_min[0] = 0.0;
  layer.region_min[1] = 0.0;
  layer.region_max[0] = 128.0;
  layer.region_max[1] = 128.0;
  return layer;
}

struct Solved {
  LedgePatch patch;
  assets::MeshData mesh;
  LedgeReport report;
};
Solved solve(const LedgeLayer& layer, const Hill& h, i64 x_mm, i64 z_mm, bool far = false) {
  Solved s;
  std::string error;
  REQUIRE_MESSAGE(
      sample_ledge_patch(layer, ground_of(h), x_mm, z_mm, k_tile_mm, far, s.patch, error), error);
  REQUIRE_MESSAGE(solve_ledges(s.patch, s.mesh, &s.report, error), error);
  return s;
}
// A vertex's place in the world, metres.
struct P3 {
  f64 x, y, z;
};
P3 world(const Solved& s, u32 vertex) {
  const Vec3 p = s.mesh.positions[vertex];
  return {static_cast<f64>(s.patch.x_mm) / 1000.0 + static_cast<f64>(p.x),
          static_cast<f64>(s.patch.y_mm) / 1000.0 + static_cast<f64>(p.y),
          static_cast<f64>(s.patch.z_mm) / 1000.0 + static_cast<f64>(p.z)};
}
f64 ground_m(const Hill& h, f64 x, f64 z) {
  return h.rise * x + h.wave * det::sin(z * (k_turn / 40.0));
}
// A triangle's unit normal.
P3 normal_of(const Solved& s, u32 t) {
  const P3 a = world(s, s.mesh.indices[t]), b = world(s, s.mesh.indices[t + 1]),
           c = world(s, s.mesh.indices[t + 2]);
  const P3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
  P3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
  const f64 l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  return {n.x / l, n.y / l, n.z / l};
}
u64 mesh_hash(const assets::MeshData& mesh) {
  u64 h = 0xcbf29ce484222325ull;
  const auto fold = [&](u32 v) {
    h ^= v;
    h *= 0x100000001b3ull;
  };
  for (const Vec3& p : mesh.positions)
    for (const f32 v : {p.x, p.y, p.z})
      fold(std::bit_cast<u32>(v));
  for (const Vec3& p : mesh.normals)
    for (const f32 v : {p.x, p.y, p.z})
      fold(std::bit_cast<u32>(v));
  for (const Vec2& p : mesh.uvs)
    for (const f32 v : {p.x, p.y})
      fold(std::bit_cast<u32>(v));
  for (const u32 i : mesh.indices)
    fold(i);
  return h;
}

struct Opened {
  const scene_gen::PlacementGeneratorDesc* desc = nullptr;
  void* state = nullptr;
  ~Opened() {
    if (desc != nullptr && state != nullptr) desc->close(state);
  }
};

}  // namespace

TEST_CASE("ledges: they stand on a resistant bed past the soil slope, and nowhere else") {
  const Hill slope;
  const LedgeLayer layer = layer_of(beds());
  const Solved s = solve(layer, slope, 0, 0);
  REQUIRE(s.report.triangles > 500);
  CHECK(s.mesh.indices.size() == 3 * s.report.triangles);
  CHECK(s.report.tallest_m > 0.8);
  CHECK(s.report.units >= 4);  // of 31 bed tops: outcrops, not every bed's stripe
  MESSAGE("near form: " << s.report.triangles << " triangles, " << s.report.front_length_m
                        << " m of front, "
                        << static_cast<f64>(s.report.triangles) / s.report.front_length_m
                        << " triangles a metre, tallest " << s.report.tallest_m << " m, "
                        << s.report.units << " bed tops");

  gfx::FormationParams field;
  std::string error;
  REQUIRE(gfx::pack_formation(*layer.formation, field, &error));
  const gfx::FormationFrame frame = gfx::formation_frame(field);
  const f64 period = static_cast<f64>(field.beds.period_mm) / 1000.0;
  // The beds' boundaries within a period, and whether the bed under each is resistant.
  u32 treads = 0, at_tops = 0;
  f64 worst = 0.0;
  for (u32 t = 0; t < s.mesh.indices.size(); t += 3) {
    const P3 n = normal_of(s, t);
    const f64 with_bed = n.x * frame.normal[0] + n.y * frame.normal[1] + n.z * frame.normal[2];
    if (with_bed < 0.9999) continue;  // a front
    ++treads;
    for (u32 k = 0; k < 3; ++k) {
      const P3 p = world(s, s.mesh.indices[t + k]);
      const f64 c = gfx::formation_coordinate(frame, p.x, p.y, p.z).coordinate;
      // In a bedding plane: within a millimetre and a half of a bed's top.
      const f64 in_period = c - period * std::floor(c / period);
      f64 nearest = 1e9;
      for (u32 b = 0; b < field.beds.count; ++b) {
        const f64 top = static_cast<f64>(field.beds.beds[b].start_m) +
                        static_cast<f64>(field.beds.beds[b].thickness_m);
        nearest = std::min(nearest, std::fabs(in_period - top));
        nearest = std::min(nearest, std::fabs(in_period + period - top));
      }
      // Or a parting inside a resistant bed: a row's edge, in a plane of the same bedding.
      const bool parting = gfx::formation_bed(field.beds, c + 0.02).resistance >= 0.5f &&
                           gfx::formation_bed(field.beds, c - 0.02).resistance >= 0.5f;
      if (nearest < 0.0015) ++at_tops;
      if (!parting) worst = std::max(worst, nearest);
      // And the bed under that plane is a resistant one.
      CHECK(gfx::formation_bed(field.beds, c - 0.02).resistance >= 0.5f);
    }  // Never in a hollow: z within 2 m of 30 (mod 40) holds its soil at this slope.
    const P3 a = world(s, s.mesh.indices[t]);
    const f64 across = a.z - 40.0 * std::floor(a.z / 40.0);
    CHECK((across < 28.0 || across > 32.0));
  }
  CHECK(treads > 200);
  CHECK(at_tops > 600);  // most of them are the units' own tops
  CHECK(worst < 0.0015);
  MESSAGE("tops against the formation's planes: worst " << worst * 1000.0 << " mm over " << treads
                                                        << " tread triangles");

  // The same hillside with every bed weak: nothing. And the same beds on 20 degrees: nothing.
  scene::Formation weak = beds();
  for (scene::FormationBed& b : weak.beds)
    b.resistance = 0.3f;
  CHECK(solve(layer_of(weak), slope, 0, 0).mesh.indices.empty());
  Hill gentle;
  gentle.rise = 0.364;
  gentle.wave = 0.0;
  CHECK(solve(layer, gentle, 0, 0).mesh.indices.empty());
  // And ground left to the cliffs generator (the whole hillside is past 36 degrees): nothing.
  LedgeLayer under_cliffs = layer;
  under_cliffs.cliff_slope_deg = 36.0f;
  CHECK(solve(under_cliffs, slope, 0, 0).mesh.indices.empty());
  // A build of nothing is refused, so no recipe of nothing is ever written.
  geometry::ClusterFileData file;
  CHECK_FALSE(build_ledge_container(solve(layer, gentle, 0, 0).patch, {}, 0, file, error, nullptr));
  CHECK(error.find("no ledges") != std::string::npos);
}

TEST_CASE(
    "ledges: the triangles are on what is seen, and nothing stands proud of the ground behind") {
  const Hill slope;
  const Solved s = solve(layer_of(beds()), slope, 0, 0);
  REQUIRE(s.report.triangles > 0);
  // The solver's own count, on the lattice's ground, and ours on the hillside's function.
  u32 above = 0;
  for (u32 t = 0; t < s.mesh.indices.size(); t += 3) {
    const P3 a = world(s, s.mesh.indices[t]), b = world(s, s.mesh.indices[t + 1]),
             c = world(s, s.mesh.indices[t + 2]);
    const f64 cx = (a.x + b.x + c.x) / 3.0, cy = (a.y + b.y + c.y) / 3.0,
              cz = (a.z + b.z + c.z) / 3.0;
    if (cy > ground_m(slope, cx, cz) - 0.02) ++above;
  }
  const f64 share = static_cast<f64>(above) / static_cast<f64>(s.report.triangles);
  const f64 reported =
      static_cast<f64>(s.report.above_ground) / static_cast<f64>(s.report.triangles);
  MESSAGE("triangles above the ground: " << share * 100.0 << " % (the solver counts "
                                         << reported * 100.0 << " %; the v29 box had 13.9 %)");
  CHECK(share >= 0.6);
  CHECK(reported >= 0.6);

  // The retaining-wall test: walking uphill from any vertex, the ground reaches its height (less
  // 5 cm) within 8 m. A wall's top stands metres over the ground behind it; a ledge's is the
  // bedding plane, which the ground has only just cut.
  u32 proud = 0;
  f64 tallest = 0.0;
  for (u32 v = 0; v < s.mesh.positions.size(); ++v) {
    const P3 p = world(s, v);
    tallest = std::max(tallest, p.y - ground_m(slope, p.x, p.z));
    bool met = false;
    for (f64 back = 0.0; back <= 8.0 && !met; back += 0.25)
      met = ground_m(slope, p.x + back, p.z) >= p.y - 0.05;
    if (!met) ++proud;
  }
  CHECK(proud == 0);
  // And no vertex is more than the tallest step above the ground under it.
  CHECK(tallest <= static_cast<f64>(s.patch.max_step_m) + 0.05);
  CHECK(tallest > 0.8);
}

TEST_CASE("ledges: a front's height follows the exposure and falls to nothing where it ends") {
  const Hill slope;
  const LedgeLayer layer = layer_of(beds());
  const Solved s = solve(layer, slope, 0, 0);
  ExposureThresholds rule;
  std::string error;
  REQUIRE(prepare_exposure(layer.rule, rule, error));
  rule.seed = s.patch.seed;
  gfx::FormationParams field;
  REQUIRE(gfx::pack_formation(*layer.formation, field, &error));
  const scene_gen::Ground ground = ground_of(slope);
  // The thickest unit, plumb: the sandstone, 1.2 m across beds dipping 12 degrees.
  const f64 cap = 1.2 / std::cos(12.0 * k_turn / 360.0);
  u32 low = 0, full = 0;
  for (u32 v = 0; v < s.mesh.positions.size(); ++v) {
    const P3 p = world(s, v);
    const f64 over = p.y - ground_m(slope, p.x, p.z);
    if (over <= 0.02) continue;
    // The most that stands at the lattice points round it: the exposure fraction under the
    // soil's cover there.
    f64 stands = 0.0;
    for (i64 dz = -1000; dz <= 1000; dz += 1000)
      for (i64 dx = -1000; dx <= 1000; dx += 1000) {
        const i64 x_mm = scene_gen::nearest_mm(p.x) + dx, z_mm = scene_gen::nearest_mm(p.z) + dz;
        const Exposure e = exposure_at(rule, field, ground, x_mm, z_mm);
        stands = std::max(stands, standing_share(rule, e.fraction, e.cover));
      }
    CHECK(over <= cap * stands + 0.06);
    if (over < 0.3) ++low;
    if (over > 0.8) ++full;
  }
  CHECK(low > 50);   // the ends are there
  CHECK(full > 50);  // and so are the full steps
}

TEST_CASE("ledges: a ledge that crosses a tile's border meets itself to the millimetre") {
  const Hill slope;
  // Twice: the rule's own outcrops, and every bed's stripe (no stretches, no members), which
  // puts many more ledges across a border. A front's lip is worn back off the border's plane
  // (the same from either tile), so the vertices left on it are the treads' backs and the joints.
  for (const bool stripes : {false, true}) {
    CAPTURE(stripes);
    LedgeLayer layer = layer_of(beds());
    if (stripes) {
      layer.rule.stretch_m = 0.0f;
      layer.rule.member_m = 0.0f;
    }
    const Solved a = solve(layer, slope, 0, 0);
    const Solved along_x = solve(layer, slope, k_tile_mm, 0);
    const Solved along_z = solve(layer, slope, 0, -k_tile_mm);
    // The vertices on a border that stand over a centimetre above the ground. A ledge may also end
    // on a border, pinched to a point at the ground's own height, which the next tile has no
    // triangle for and no need of.
    const auto border = [&](const Solved& s, bool x_axis, f64 at) {
      std::vector<P3> out;
      for (u32 v = 0; v < s.mesh.positions.size(); ++v) {
        const P3 p = world(s, v);
        if (std::fabs((x_axis ? p.x : p.z) - at) < 1e-4 && p.y > ground_m(slope, p.x, p.z) + 0.01)
          out.push_back(p);
      }
      return out;
    };
    const auto matched = [](const std::vector<P3>& from, const std::vector<P3>& to) {
      u32 missing = 0;
      for (const P3& p : from) {
        bool found = false;
        for (const P3& q : to)
          found = found || (std::fabs(p.x - q.x) < 1e-3 && std::fabs(p.y - q.y) < 1e-3 &&
                            std::fabs(p.z - q.z) < 1e-3);
        if (!found) ++missing;
      }
      return missing;
    };
    const std::vector<P3> ax = border(a, true, 64.0), bx = border(along_x, true, 64.0);
    const std::vector<P3> az = border(a, false, 0.0), bz = border(along_z, false, 0.0);
    MESSAGE("border vertices: " << ax.size() << " and " << bx.size() << " across x = 64, "
                                << az.size() << " and " << bz.size() << " across z = 0");
    CHECK(ax.size() > (stripes ? 10u : 0u));
    CHECK(az.size() > (stripes ? 10u : 0u));
    CHECK(matched(ax, bx) == 0);
    CHECK(matched(bx, ax) == 0);
    CHECK(matched(az, bz) == 0);
    CHECK(matched(bz, az) == 0);
  }
}

TEST_CASE("ledges: a block's edges are worn round by its rock's rounding, inside the budget") {
  const Hill h;
  // The worn edges: triangles between a tread's plane and a front's, leaning 25 to 65 degrees.
  // Their area over the front's length is the mean width of the wear.
  const auto worn_width = [&](RockType type, f64& per_metre) {
    LedgeLayer layer = layer_of(beds());
    layer.type = type;
    const Solved s = solve(layer, h, 0, 0);
    f64 area = 0.0;
    for (u32 t = 0; t + 2 < s.mesh.indices.size(); t += 3) {
      const P3 a = world(s, s.mesh.indices[t]), b = world(s, s.mesh.indices[t + 1]),
               c = world(s, s.mesh.indices[t + 2]);
      const P3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
      const P3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
      const f64 twice = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
      if (!(twice > 0.0)) continue;
      const f64 up = n.y / twice;
      if (up > 0.42 && up < 0.9) area += 0.5 * twice;
    }
    per_metre = static_cast<f64>(s.report.triangles) / s.report.front_length_m;
    return area / s.report.front_length_m;
  };
  f64 sandstone_tris = 0.0, slate_tris = 0.0, limestone_tris = 0.0;
  const f64 sandstone = worn_width(RockType::Sandstone, sandstone_tris);
  const f64 slate = worn_width(RockType::Slate, slate_tris);
  const f64 limestone = worn_width(RockType::Limestone, limestone_tris);
  MESSAGE("worn edge, metres of width a metre of front: sandstone "
          << sandstone << " (rounding " << cliff_type_rounding(RockType::Sandstone) << "), slate "
          << slate << " (" << cliff_type_rounding(RockType::Slate) << "), limestone " << limestone
          << " (" << cliff_type_rounding(RockType::Limestone) << "); triangles a metre of front "
          << sandstone_tris << ", " << slate_tris << ", " << limestone_tris);
  // In the rocks' own order, and by about their amounts: slate's 0.08 against sandstone's 0.4.
  CHECK(limestone > sandstone);
  CHECK(sandstone > 3.0 * slate);
  CHECK(sandstone > 0.08);
  // The budget: 48 triangles a metre of front (36 before the wear).
  CHECK(sandstone_tris <= 48.0);
  CHECK(slate_tris <= 48.0);
  CHECK(limestone_tris <= 48.0);
}

TEST_CASE("ledges: the far form is the same steps on a coarser lattice") {
  const Hill slope;
  const LedgeLayer layer = layer_of(beds());
  const Solved near_form = solve(layer, slope, 0, 0);
  const Solved far_form = solve(layer, slope, 0, 0, true);
  REQUIRE(far_form.report.triangles > 0);
  CHECK(far_form.patch.cell_mm == 4000);
  CHECK(far_form.patch.apron == 1);
  CHECK(far_form.report.triangles * 6 < near_form.report.triangles);
  MESSAGE("far form: " << far_form.report.triangles << " triangles, "
                       << far_form.report.front_length_m << " m of front, "
                       << static_cast<f64>(far_form.report.triangles) /
                              far_form.report.front_length_m
                       << " triangles a metre (near " << near_form.report.triangles << ")");
  // Its steps are where the near form's are, to the far lattice's 4 m: every far vertex has a
  // near vertex in its 4 m square or one beside it. (It cannot see a hollow narrower than its
  // cell, and bridges one: the price of a coarse lattice, and why it is the far form.)
  std::vector<u8> marked(18 * 18, 0);
  const auto square = [](f32 v) { return static_cast<u32>(std::clamp(v / 4.0f, 0.0f, 15.0f)) + 1; };
  for (const Vec3& p : near_form.mesh.positions)
    marked[square(p.z) * 18 + square(p.x)] = 1;
  u32 stray = 0;
  for (const Vec3& p : far_form.mesh.positions) {
    bool beside = false;
    for (u32 dz = 0; dz < 3; ++dz)
      for (u32 dx = 0; dx < 3; ++dx)
        beside = beside || marked[(square(p.z) + dz - 1) * 18 + square(p.x) + dx - 1] != 0;
    if (!beside) ++stray;
  }
  CHECK(stray == 0);  // The far form's fronts carry no joint blocks: one face a step, so far fewer
                      // front triangles
  // a metre.
  CHECK(static_cast<f64>(far_form.report.front_triangles) / far_form.report.front_length_m <
        static_cast<f64>(near_form.report.front_triangles) / near_form.report.front_length_m);
}

// Taken on MSVC (msvc-debug, x86-64-v3); Clang 18 and GCC 13 reproduce them in the containers at
// v3 and GCC at v2.
TEST_CASE("ledges: a patch is pinned to the bit") {
  const Hill slope;
  scene::Formation folded = beds();
  folded.fold_amplitude_m = 2.5f;
  folded.fold_wavelength_m = 150.0f;
  const LedgeLayer layer = layer_of(folded);
  const Solved near_form = solve(layer, slope, -k_tile_mm, k_tile_mm);
  const Solved far_form = solve(layer, slope, -k_tile_mm, k_tile_mm, true);
  MESSAGE("pins: near mesh " << mesh_hash(near_form.mesh) << " source "
                             << ledge_source_hash(near_form.patch) << ", far mesh "
                             << mesh_hash(far_form.mesh) << " source "
                             << ledge_source_hash(far_form.patch));
  CHECK(near_form.report.triangles > 0u);
  CHECK(mesh_hash(near_form.mesh) == 7169356939382602585ull);
  CHECK(ledge_source_hash(near_form.patch) == 10834469400257094798ull);
  CHECK(far_form.report.triangles > 0u);
  CHECK(mesh_hash(far_form.mesh) == 12540669320333577569ull);
  CHECK(ledge_source_hash(far_form.patch) == 737779768996180544ull);
}

TEST_CASE("ledges generator: tiles in any order are the whole read, a form a ring") {
  test::TempDir scratch("ledges-generator");
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement(k_ledges_generator);
  REQUIRE(desc != nullptr);
  const Hill slope;
  scene_gen::Context context;
  context.ground = ground_of(slope);
  context.tile_size = 64.0f;
  const std::string dir = scratch.path();
  context.dir = dir;
  context.ddc = dir;
  std::string error;
  const LedgeLayer layer = layer_of(beds());

  Opened whole;
  whole.desc = desc;
  REQUIRE_MESSAGE(desc->open(schema::to_json(layer), context, &whole.state, &error), error);
  scene_gen::Placements expanded;
  REQUIRE(desc->expand(whole.state, context, expanded, &error));
  // 128 m square, four tiles: a tile no crag stands in has no mesh, and one whose steps are all
  // under the far form's least has no far form.
  CHECK(expanded.things >= 2);
  CHECK(expanded.things <= 4);
  Vector<scene_gen::PlacementMesh> resident;
  REQUIRE(desc->meshes(whole.state, context, resident, &error));
  CHECK(resident.size() > expanded.things);
  CHECK(resident.size() <= 2 * expanded.things);
  MESSAGE(expanded.things << " of 4 tiles have ledges, " << resident.size() << " meshes");
  for (const scene_gen::PlacementMesh& mesh : resident) {
    CHECK(io::exists(mesh.path));
    CHECK(mesh.path.find("/ledges/ledge-") != std::string::npos);
  }

  // Ring 0 is the near form, tile by tile in reverse: the whole read.
  context.ring = 0;
  std::vector<std::string> from_tiles, from_whole;
  for (i32 tz = 3; tz-- > -1;)
    for (i32 tx = 3; tx-- > -1;) {
      scene_gen::Placements part;
      REQUIRE(desc->tile(whole.state, scene_gen::TileCoord{tx, tz}, context, part, &error));
      CHECK(desc->occupies(whole.state, scene_gen::TileCoord{tx, tz}) == !part.instances.empty());
      CHECK(part.kind == 0);
      for (const scene_gen::Placement& p : part.instances)
        from_tiles.push_back(resident[p.mesh].path + "@" + std::to_string(p.position.x) + "," +
                             std::to_string(p.position.y) + "," + std::to_string(p.position.z));
    }
  for (const scene_gen::Placement& p : expanded.instances)
    from_whole.push_back(expanded.meshes[p.mesh].path + "@" + std::to_string(p.position.x) + "," +
                         std::to_string(p.position.y) + "," + std::to_string(p.position.z));
  std::sort(from_tiles.begin(), from_tiles.end());
  std::sort(from_whole.begin(), from_whole.end());
  CHECK(from_tiles == from_whole);

  // Ring 1 and 2 draw the far form (another mesh of the same tile), ring 3 nothing.
  scene_gen::Placements near_tile, far_tile, none;
  REQUIRE(desc->tile(whole.state, scene_gen::TileCoord{0, 0}, context, near_tile, &error));
  CHECK(desc->representation(whole.state, context) == 0);
  context.ring = 2;
  REQUIRE(desc->tile(whole.state, scene_gen::TileCoord{0, 0}, context, far_tile, &error));
  CHECK(desc->representation(whole.state, context) == 1);
  context.ring = 3;
  REQUIRE(desc->tile(whole.state, scene_gen::TileCoord{0, 0}, context, none, &error));
  CHECK(desc->representation(whole.state, context) == 2);
  REQUIRE(near_tile.instances.size() == 1);
  REQUIRE(far_tile.instances.size() == 1);
  CHECK(far_tile.kind == 1);
  CHECK(near_tile.instances[0].mesh != far_tile.instances[0].mesh);
  CHECK(near_tile.instances[0].position.x == far_tile.instances[0].position.x);
  CHECK(none.instances.empty());

  // A recipe is a mesh the registry's generator builds: the far one, read back from its file.
  std::string text;
  REQUIRE(io::read_file(resident[far_tile.instances[0].mesh].path, text) == io::Status::Ok);
  JsonValue json;
  REQUIRE(parse_json(text, json).ok);
  LedgePatch patch;
  schema::ReadContext reader;
  REQUIRE(schema::from_json(patch, json, reader));
  CHECK(patch.far);
  geometry::ClusterFileData file;
  REQUIRE_MESSAGE(build_ledge_container(patch, {}, 0, file, error, nullptr), error);
  CHECK(file.generator == "ledge");
  CHECK(file.source_hash == ledge_source_hash(patch));
  CHECK(!file.mesh.mesh.clusters.empty());
  CHECK(file.cluster_material.size() == file.mesh.mesh.clusters.size());

  // A layer with no formation is refused with a sentence.
  LedgeLayer bare = layer;
  bare.formation.reset();
  Opened refused;
  refused.desc = desc;
  CHECK_FALSE(desc->open(schema::to_json(bare), context, &refused.state, &error));
  CHECK(error.find("formation") != std::string::npos);
}

// Printed, not asserted: a timing beside other load is an upper bound. The release numbers and
// the machine they were taken on are in docs/experiments/exposed-bedrock.md.
TEST_CASE("ledges: what a tile costs") {
  const Hill slope;
  const LedgeLayer layer = layer_of(beds());
  const scene_gen::Ground ground = ground_of(slope);
  const auto ms_since = [](std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - from).count();
  };
  std::string error;
  for (const bool far : {false, true}) {
    LedgePatch patch;
    auto start = std::chrono::steady_clock::now();
    REQUIRE(sample_ledge_patch(layer, ground, 0, 0, k_tile_mm, far, patch, error));
    const f64 sample_ms = ms_since(start);
    assets::MeshData mesh;
    LedgeReport report;
    start = std::chrono::steady_clock::now();
    REQUIRE(solve_ledges(patch, mesh, &report, error));
    const f64 solve_ms = ms_since(start);
    geometry::ClusterFileData file;
    start = std::chrono::steady_clock::now();
    REQUIRE_MESSAGE(build_ledge_container(patch, {}, 0, file, error, nullptr), error);
    const f64 build_ms = ms_since(start);
    const usize vertex_bytes = mesh.positions.size() * (sizeof(Vec3) * 2 + sizeof(Vec2)) +
                               mesh.indices.size() * sizeof(u32);
    MESSAGE(std::string(far ? "far" : "near")
            << " form, a 64 m tile: sample " << sample_ms << " ms (" << patch.heights_mm.size()
            << " heights), solve " << solve_ms << " ms, container " << build_ms << " ms; "
            << report.triangles << " triangles, " << mesh.positions.size() << " vertices, "
            << vertex_bytes << " bytes of source mesh, " << file.mesh.mesh.clusters.size()
            << " clusters; " << report.front_length_m << " m of front, "
            << static_cast<f64>(report.triangles) / report.front_length_m << " triangles a metre");
    CHECK(report.triangles > 0);
  }
}

// ---- outcrops, not stripes (rocks.md, "Outcrops, not stripes") ---------------------------------

namespace {

// The beds that crop out along a transect up the slope at z: the fronts (steep triangles) the
// plane z = `z` cuts, counted as one bed where their crossings are within 0.8 m of each other
// along x (one bed's rows stand over each other; two beds are over a metre apart up this slope).
u32 beds_on_transect(const Solved& s, f64 z) {
  std::vector<f64> xs;
  for (u32 t = 0; t + 2 < s.mesh.indices.size(); t += 3) {
    if (std::fabs(normal_of(s, t).y) > 0.5) continue;
    const P3 a = world(s, s.mesh.indices[t]), b = world(s, s.mesh.indices[t + 1]),
             c = world(s, s.mesh.indices[t + 2]);
    if (std::min({a.z, b.z, c.z}) > z || std::max({a.z, b.z, c.z}) < z) continue;
    xs.push_back((a.x + b.x + c.x) / 3.0);
  }
  std::sort(xs.begin(), xs.end());
  u32 beds = 0;
  for (u32 i = 0; i < xs.size(); ++i)
    beds += i == 0 || xs[i] - xs[i - 1] > 0.8 ? 1u : 0u;
  return beds;
}

}  // namespace

TEST_CASE("ledges: rock crops out in patches across the beds, not as every bed's stripe") {
  const Hill h;
  // Every resistant bed along its whole trace: no patches, no stretches, no members.
  LedgeLayer stripes = layer_of(beds());
  stripes.rule.patch_m = 0.0f;
  stripes.rule.stretch_m = 0.0f;
  stripes.rule.member_m = 0.0f;
  // The soil's patches alone, as the solver stood before: broken along a bed, not across the beds.
  LedgeLayer patches = layer_of(beds());
  patches.rule.stretch_m = 0.0f;
  patches.rule.member_m = 0.0f;
  const LedgeLayer layer = layer_of(beds());  // the rule's own
  CHECK(layer.rule.stretch_m == 32.0f);
  CHECK(layer.rule.member_m == 12.0f);
  f64 sum[2] = {}, squares[2] = {};
  u32 transects = 0, none_at_all = 0;
  f64 front[3] = {};
  for (const i64 x_mm : {i64{0}, i64{64000}}) {
    const Solved all = solve(stripes, h, x_mm, 0);
    const Solved before = solve(patches, h, x_mm, 0);
    const Solved now = solve(layer, h, x_mm, 0);
    front[0] += all.report.front_length_m;
    front[1] += before.report.front_length_m;
    front[2] += now.report.front_length_m;
    for (f64 z = 2.0; z < 64.0; z += 4.0) {
      const u32 every = beds_on_transect(all, z);
      if (every == 0) continue;  // a hollow that keeps its soil
      const f64 share[2] = {static_cast<f64>(beds_on_transect(before, z)) / every,
                            static_cast<f64>(beds_on_transect(now, z)) / every};
      for (u32 k = 0; k < 2; ++k) {
        sum[k] += share[k];
        squares[k] += share[k] * share[k];
      }
      none_at_all += share[1] == 0.0 ? 1u : 0u;
      ++transects;
    }
  }
  f64 mean[2], deviation[2];
  for (u32 k = 0; k < 2; ++k) {
    mean[k] = sum[k] / transects;
    deviation[k] = std::sqrt(std::max(0.0, squares[k] / transects - mean[k] * mean[k]));
  }
  MESSAGE("the share of the resistant beds that crop out along a transect up the slope, over "
          << transects << " transects: " << mean[0] * 100.0 << " % (deviation "
          << deviation[0] * 100.0 << ") with the soil's patches alone, " << mean[1] * 100.0
          << " % (deviation " << deviation[1] * 100.0 << ") with members and stretches; "
          << none_at_all << " transects with no rock at all; front " << front[0]
          << " m with every bed, " << front[1] << " m with the patches, " << front[2] << " m now");
  // Fewer than half the beds on a transect, and not the same share on every transect.
  CHECK(mean[1] < 0.5);
  CHECK(mean[1] > 0.05);
  CHECK(mean[1] < 0.75 * mean[0]);
  CHECK(deviation[1] > 0.08);
  CHECK(front[2] < 0.5 * front[1]);
  // The factors are the world's: 0 to 1, the same from anywhere, and off when the rule says so.
  ExposureThresholds rule;
  std::string error;
  REQUIRE(prepare_exposure(layer.rule, rule, error));
  rule.seed = layer.seed;
  gfx::FormationParams field;
  REQUIRE(gfx::pack_formation(beds(), field, &error));
  u32 members = 0, open = 0;
  for (i64 k = 0; k < 400; ++k) {
    const f64 m = member_factor(rule, field, static_cast<f64>(k) * 0.37);
    CHECK((m == 0.0 || m == 1.0));
    members += m > 0.0 ? 1u : 0u;
    const f64 s = stretch_factor(rule, 7, k * 3170, k * 1930);
    CHECK(s >= 0.0);
    CHECK(s <= 1.0);
    CHECK(s == stretch_factor(rule, 7, k * 3170, k * 1930));
    open += s > 0.0 ? 1u : 0u;
  }
  MESSAGE(members << " of 400 bed coordinates in a cliff-forming member, " << open
                  << " of 400 points of a bed top's trace open");
  CHECK(members > 100);
  CHECK(members < 300);
  CHECK(open > 60);
  CHECK(open < 260);
  ExposureThresholds off;
  REQUIRE(prepare_exposure(stripes.rule, off, error));
  CHECK(member_factor(off, field, 3.3) == 1.0);
  CHECK(stretch_factor(off, 7, 1000, 2000) == 1.0);
  // With the formation's own macro sequence a member is the macro bed, by its resistance.
  scene::Formation with_members = beds();
  with_members.macro_beds.push_back(bed("cliff former", 10.0f, 0.9f));
  with_members.macro_beds.push_back(bed("slope former", 20.0f, 0.1f));
  REQUIRE(gfx::pack_formation(with_members, field, &error));
  CHECK(member_factor(rule, field, 5.0) > 0.5);
  CHECK(member_factor(rule, field, 20.0) == 0.0);
  LedgeLayer bad_rule = layer_of(beds());
  bad_rule.rule.stretch_m = 3.0f;
  CHECK_FALSE(prepare_exposure(bad_rule.rule, off, error));
  CHECK(error.find("stretch_m") != std::string::npos);
}

TEST_CASE("ledges: a tread carries soil to its lip, and a bed's rock is its formation's") {
  const Hill h;
  // Two resistant beds the formation tells apart: a warm sandstone and a pale, smoother limestone.
  scene::Formation f = beds();
  f.beds[1].tint = Vec3{1.0f, 0.8f, 0.6f};
  f.beds[4].tint = Vec3{0.9f, 0.95f, 1.0f};
  f.beds[4].roughness = -0.2f;
  LedgeLayer layer = layer_of(f);
  layer.rule.stretch_m = 0.0f;  // every bed's stripe, so both units are in the tile
  layer.rule.member_m = 0.0f;
  const Solved s = solve(layer, h, 0, 0);
  // One primitive a material: the soil first, then a rock a unit.
  REQUIRE(s.mesh.materials.size() == 3);
  REQUIRE(s.mesh.primitives.size() == 3);
  CHECK(s.mesh.materials[0].name == "soil");
  CHECK(s.mesh.materials[0].base_color.y > s.mesh.materials[0].base_color.x);  // a turf, not a rock
  const assets::Material plain = [&] {
    const Solved untinted = solve(layer_of(beds()), h, 0, 0);
    return untinted.mesh.materials[untinted.mesh.materials.size() - 1];
  }();
  const assets::Material& sandstone = s.mesh.materials[1];
  const assets::Material& limestone = s.mesh.materials[2];
  CHECK(sandstone.base_color.z == doctest::Approx(plain.base_color.z * 0.6f));
  CHECK(limestone.base_color.z == doctest::Approx(plain.base_color.z * 1.0f));
  CHECK(limestone.roughness == doctest::Approx(plain.roughness - 0.2f));
  CHECK(sandstone.base_color.z < limestone.base_color.z);
  // The soil's primitive is the treads: every triangle of it faces up, in a bedding plane; the
  // rock's are the fronts, the worn edges, the shelves and the joints.
  const assets::Primitive& soil = s.mesh.primitives[0];
  REQUIRE(soil.material == 0);
  u32 up = 0;
  for (u32 t = soil.first_index; t < soil.first_index + soil.index_count; t += 3)
    up += normal_of(s, t).y > 0.9 ? 1u : 0u;
  // (but for a sliver in a thousand, whose normal is its rounding's)
  CHECK((soil.index_count / 3 - up) * 1000 <= soil.index_count / 3);
  u32 steep = 0, rock = 0;
  for (u32 t = soil.first_index + soil.index_count; t + 2 < s.mesh.indices.size(); t += 3) {
    ++rock;
    steep += std::fabs(normal_of(s, t).y) < 0.9 ? 1u : 0u;
  }
  CHECK(steep > rock / 2);
  // The container keeps them apart: a cluster names one material, and both rocks and the soil
  // are there; a tread's own colour can be given.
  geometry::ClusterFileData file;
  std::string error;
  REQUIRE_MESSAGE(build_ledge_container(s.patch, {}, 0, file, error, nullptr), error);
  REQUIRE(file.materials.size() == 3);
  CHECK(file.cluster_material.size() == file.mesh.mesh.clusters.size());
  bool seen[3] = {};
  for (const u32 m : file.cluster_material) {
    REQUIRE(m < 3);
    seen[m] = true;
  }
  CHECK((seen[0] && seen[1] && seen[2]));
  CHECK(file.materials[1].base_color.z == doctest::Approx(sandstone.base_color.z));
  layer.tread_tint = std::array<f32, 3>{0.2f, 0.1f, 0.05f};
  const Solved bare = solve(layer, h, 0, 0);
  CHECK(bare.mesh.materials[0].base_color.x == doctest::Approx(0.2f));
  layer.tread_tint = std::array<f32, 3>{2.0f, 0.1f, 0.05f};
  LedgePatch refused;
  CHECK_FALSE(sample_ledge_patch(layer, ground_of(h), 0, 0, k_tile_mm, false, refused, error));
  CHECK(error.find("tread_tint") != std::string::npos);
}

TEST_CASE("ledges: a fallen block is handed over with its identity, its place and its size") {
  const Hill h;
  LedgeLayer layer = layer_of(beds());
  layer.rule.stretch_m = 0.0f;  // every bed's stripe: many blocks, and gaps across the border
  layer.rule.member_m = 0.0f;
  const Solved a = solve(layer, h, 0, 0);
  const Solved b = solve(layer, h, k_tile_mm, 0);
  Vector<LedgeGap> gaps_a, gaps_b, again;
  std::string error;
  REQUIRE_MESSAGE(ledge_gaps(a.patch, gaps_a, error), error);
  REQUIRE_MESSAGE(ledge_gaps(b.patch, gaps_b, error), error);
  REQUIRE(ledge_gaps(a.patch, again, error));
  REQUIRE(gaps_a.size() > 20);
  REQUIRE(again.size() == gaps_a.size());
  f64 area = 0.0;
  for (u32 i = 0; i < gaps_a.size(); ++i) {
    const LedgeGap& g = gaps_a[i];
    if (i > 0) CHECK(g.block > gaps_a[i - 1].block);  // in order, each once
    CHECK(g.block == again[i].block);
    CHECK(g.x == again[i].x);
    CHECK(g.area_m2 > 0.0);
    // Between its joints: a unit's thickness over the joint ratio, 0.5 to 8 m, moved by 0.3.
    CHECK(g.length_m > 0.15);
    CHECK(g.length_m < 13.0);
    CHECK(g.height_m >= static_cast<f64>(layer.min_step_m));
    CHECK(g.height_m <= static_cast<f64>(layer.max_step_m));
    // Inside the patch, on the ground.
    CHECK(g.x >= 0.0);
    CHECK(g.x <= 64.0);
    CHECK(g.z >= 0.0);
    CHECK(g.z <= 64.0);
    const f64 wx = static_cast<f64>(a.patch.x_mm) / 1000.0 + g.x;
    const f64 wz = static_cast<f64>(a.patch.z_mm) / 1000.0 + g.z;
    CHECK(static_cast<f64>(a.patch.y_mm) / 1000.0 + g.y ==
          doctest::Approx(ground_m(h, wx, wz)).epsilon(0.02));
    area += g.area_m2;
  }
  // A gap that crosses the border x = 64 is in both tiles, the same block of the same length.
  u32 shared = 0;
  for (const LedgeGap& g : gaps_a)
    for (const LedgeGap& o : gaps_b)
      if (g.block == o.block) {
        ++shared;
        CHECK(g.length_m == o.length_m);
      }
  MESSAGE(gaps_a.size() << " fallen blocks in the tile (" << area << " m2 of tread), "
                        << gaps_b.size() << " in the next, " << shared
                        << " across the border; front " << a.report.front_length_m << " m");
  CHECK(shared >= 1);
  // The far form has no blocks.
  const Solved far_form = solve(layer, h, 0, 0, true);
  Vector<LedgeGap> none;
  REQUIRE(ledge_gaps(far_form.patch, none, error));
  CHECK(none.empty());
}
