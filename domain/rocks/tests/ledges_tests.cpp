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
  // Of 31 bed tops 13 crop out at the rule's members and stretches: outcrops, not every bed's
  // stripe. The first agent's bound, put back after it was loosened mid-way to 4.
  CHECK(s.report.units >= 10);
  CHECK(s.report.units < 31);
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
  MESSAGE(low << " vertices under 0.3 m, " << full << " over 0.8 m");
  CHECK(low > 50);  // the ends are there
  // And so are the full steps. (50 before a block was a body: a joint's flank no longer starts at
  // the tread's height but under its worn rim, and a crept block has moved down the slope, so
  // fewer vertices stand at a full step; 28 do.)
  CHECK(full > 20);
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
        if (std::fabs((x_axis ? p.x : p.z) - at) < 2e-5 && p.y > ground_m(slope, p.x, p.z) + 0.01)
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
    // (Each border over 10 before a block was a body: a joint's worn rim and a crept block move a
    // tread's vertices off the border's plane, the same from either tile, so the two borders are
    // counted together.)
    CHECK(ax.size() + az.size() > (stripes ? 20u : 0u));
    CHECK(ax.size() > 0u);
    CHECK(az.size() > 0u);
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
    layer.riser = 1.0f;  // the ledges alone: a crag's risers lean as a worn edge does
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
  // Corners are worn in plan too, half as far again as the edges: metres of worn corner a metre
  // of front, in the rocks' own order.
  const auto corners = [&](RockType type) {
    LedgeLayer layer = layer_of(beds());
    layer.type = type;
    const Solved s = solve(layer, h, 0, 0);
    return s.report.corner_length_m / s.report.front_length_m;
  };
  const f64 sandstone_corners = corners(RockType::Sandstone),
            slate_corners = corners(RockType::Slate);
  MESSAGE("worn corners, a share of the front: sandstone " << sandstone_corners << ", slate "
                                                           << slate_corners);
  CHECK(sandstone_corners > 0.05);
  CHECK(sandstone_corners > 2.0 * slate_corners);
  // The budget: 56 triangles a metre of front (36 before the wear, 48 before a block was a body:
  // the clefts' flanks and their worn rims and the undercut row under each front cost about 8).
  CHECK(sandstone_tris <= 56.0);
  CHECK(slate_tris <= 56.0);
  CHECK(limestone_tris <= 56.0);
}

TEST_CASE("ledges: a block is a body, rounded all round, split by clefts, undercut and crept") {
  const Hill h;
  struct Body {
    f64 rim = 0.0;       // metres of worn rim a metre of joint
    f64 flank = 0.0;     // the clefts' mean depth: flank area over joint length
    f64 joints = 0.0;    // metres of joint a metre of front
    f64 deepest = 0.0;   // the deepest cleft
    f64 undercut = 0.0;  // undercut underside, square metres a metre of front
    f64 down = 0.0;      // every down-facing underside, the same
    u32 slipped = 0;
    f64 per_metre = 0.0;  // triangles a metre of front
    f64 notch = 0.0;      // the flanks' notches round the undercut (G174), square metres
    f64 fin = 0.0;        // the fins left past it
  };
  const auto body = [&](RockType type, const scene::Formation& f) {
    LedgeLayer layer = layer_of(f);
    layer.type = type;
    const Solved s = solve(layer, h, 0, 0);
    gfx::FormationParams field;
    std::string error;
    REQUIRE(gfx::pack_formation(*layer.formation, field, &error));
    const gfx::FormationFrame frame = gfx::formation_frame(field);
    const P3 strike{-frame.dip[2], 0.0, frame.dip[0]};
    Body b;
    f64 flank_area = 0.0, down_area = 0.0;
    for (u32 t = 0; t + 2 < s.mesh.indices.size(); t += 3) {
      const P3 a = world(s, s.mesh.indices[t]), q = world(s, s.mesh.indices[t + 1]),
               c = world(s, s.mesh.indices[t + 2]);
      const P3 u{q.x - a.x, q.y - a.y, q.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
      const P3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
      const f64 twice = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
      if (!(twice > 0.0)) continue;
      // A flank stands in a joint: its normal along the strike, within the joints' 20 degrees.
      if (std::fabs(n.x * strike.x + n.z * strike.z) / twice > 0.9) flank_area += 0.5 * twice;
      if (n.y / twice < -0.5) down_area += 0.5 * twice;
    }
    b.rim = s.report.joint_worn_m2 / s.report.joint_length_m;
    b.flank = flank_area / s.report.joint_length_m;
    b.joints = s.report.joint_length_m / s.report.front_length_m;
    b.deepest = s.report.cleft_depth_m;
    b.undercut = s.report.undercut_m2 / s.report.front_length_m;
    b.down = down_area / s.report.front_length_m;
    b.slipped = s.report.slipped;
    b.per_metre = static_cast<f64>(s.report.triangles) / s.report.front_length_m;
    b.notch = s.report.notch_m2;
    b.fin = s.report.fin_m2;
    MESSAGE(static_cast<int>(type)
            << ": rim " << b.rim << " m, clefts " << b.flank << " m deep (deepest " << b.deepest
            << "), " << b.joints << " m of joint a metre of front, undercut " << b.undercut
            << " m2 a metre (all undersides " << b.down << "), " << b.slipped << " crept pieces, "
            << b.per_metre << " triangles a metre of front; flanks notched round the undercut "
            << b.notch << " m2, fins left " << b.fin << " m2");
    return b;
  };
  const Body sandstone = body(RockType::Sandstone, beds());
  const Body slate = body(RockType::Slate, beds());
  const Body limestone = body(RockType::Limestone, beds());
  // Rounded all round: a joint's rim is worn as the lip is, in the rocks' own order and by about
  // their amounts (sandstone's 12 cm of wear against slate's 2.4).
  CHECK(sandstone.rim > 0.04);
  CHECK(sandstone.rim > 2.5 * slate.rim);
  CHECK(limestone.rim > sandstone.rim);
  // Every joint is a cleft with depth: a flank on each side, as deep on the mean as a third of a
  // metre, the deepest the unit's step.
  CHECK(sandstone.joints > 0.25);
  CHECK(sandstone.flank > 0.2);
  CHECK(sandstone.deepest > 0.8);
  // The beds under these units are shale and mudstone: the feet are undercut.
  CHECK(sandstone.undercut > 0.005);
  CHECK(sandstone.down > sandstone.undercut);
  // G174: a flank over an undercut foot follows it, so it does not stand past the undercut as a
  // fin. On the beds above the steps are about a metre and their notches too low to follow; on a
  // thick sandstone over shale (clefts to 1.9 m) the deep and tall notches are taken out of the
  // flanks (0.56 m2 of the tile's, 29 % of all the notches' side area, measured), and what is
  // left is the shallow (under 0.15 m) and low (under 0.3 m) ones the triangle budget leaves.
  // With the notch never followed, as the base's flanks were, `notch` is 0.
  scene::Formation thick = beds();
  thick.beds[0] = bed("shale", 2.0f, 0.1f);
  thick.beds[1] = bed("sandstone", 2.4f, 0.9f);
  const Body tall = body(RockType::Sandstone, thick);
  CHECK(tall.notch > 0.25);
  // One standing block in four has crept out and leans.
  CHECK(sandstone.slipped > 10u);
  // Where every bed of the sequence stands there is no weaker bed under a unit: no undercut.
  scene::Formation hard = beds();
  for (scene::FormationBed& b : hard.beds)
    b.resistance = 0.9f;
  CHECK(body(RockType::Sandstone, hard).undercut == 0.0);
}

TEST_CASE("ledges: a crag has mass, its risers the crag's rock") {
  const Hill h;
  const LedgeLayer layer = layer_of(beds());
  const Solved s = solve(layer, h, 0, 0);
  const Solved far_form = solve(layer, h, 0, 0, true);
  LedgeLayer bare = layer;
  bare.riser = 1.0f;
  const Solved without = solve(bare, h, 0, 0);
  MESSAGE("risers: " << s.report.riser_m2 << " m2 in " << s.report.riser_triangles << " triangles ("
                     << static_cast<f64>(s.report.riser_triangles) / s.report.riser_m2
                     << " a square metre); far " << far_form.report.riser_m2 << " m2 in "
                     << far_form.report.riser_triangles << "; ledges alone "
                     << without.report.triangles << " triangles");
  // The ground between a crag's ledges is its rock: tens of square metres of a 64 m tile, and
  // nothing else in the mesh moves. Not in the far form: a skin over the ground z-fights with the
  // terrain's coarser levels at its distances.
  CHECK(s.report.riser_m2 > 50.0);
  CHECK(far_form.report.riser_triangles == 0u);
  CHECK(s.report.triangles == without.report.triangles + s.report.riser_triangles);
  CHECK(without.report.riser_triangles == 0u);
  // On the ground: a riser's triangles face up and lie on the hillside within the lattice's
  // own error and the lift.
  u32 off = 0, checked = 0;
  for (u32 t = 0; t + 2 < s.mesh.indices.size(); t += 3) {
    const P3 a = world(s, s.mesh.indices[t]);
    const P3 n = normal_of(s, t);
    // A riser's normal is the hillside's: up the slope's 37 degrees.
    if (!(n.y > 0.7 && n.y < 0.85 && n.x < -0.5)) continue;
    const f64 over = a.y - ground_m(h, a.x, a.z);
    if (over > 0.0 && over < 0.2) {
      ++checked;
    } else if (over < -0.2 || over > 0.4) {
      ++off;
    }
  }
  CHECK(checked > 200u);
  CHECK(off * 50u < checked);
  // None on ground the rule leaves under soil: the gentle hill has no ledge and no riser.
  Hill gentle;
  gentle.rise = 0.364;
  gentle.wave = 0.0;
  CHECK(solve(layer, gentle, 0, 0).mesh.indices.empty());
  // Out of range is refused.
  LedgePatch patch = s.patch;
  patch.riser = 0.0f;
  std::string error;
  CHECK_FALSE(ledge_patch_in_range(patch, error));
  CHECK(error.find("riser") != std::string::npos);
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
  // G200 re-pinned the near mesh (a row now recesses along its bed edge's own outward normal, 6.5
  // degrees off the fall line on this hillside, not along the fall line) and both sources
  // (`k_ledge_version` 4); the far form's mesh holds: it has no rows, and a plumb front.
  CHECK(mesh_hash(near_form.mesh) == 3260580774600535639ull);
  CHECK(ledge_source_hash(near_form.patch) == 17570146212005944688ull);
  CHECK(far_form.report.triangles > 0u);
  CHECK(mesh_hash(far_form.mesh) == 12540669320333577569ull);
  CHECK(ledge_source_hash(far_form.patch) == 6100617242444932633ull);
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
  // 128 m square, four tiles: a crag stands in each at the rule's members and stretches, and each
  // has a near and a far form (the first agent's exact counts, put back after they were loosened
  // to ranges mid-way). A tile no crag stands in would have no mesh, and one whose steps are all
  // under the far form's least no far form.
  CHECK(expanded.things == 4);
  Vector<scene_gen::PlacementMesh> resident;
  REQUIRE(desc->meshes(whole.state, context, resident, &error));
  CHECK(resident.size() == 8);
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
  for (u32 t = soil.first_index; t < soil.first_index + soil.index_count; t += 3) {
    up += normal_of(s, t).y > 0.9 ? 1u : 0u;
  }
  // (but for a sliver in a thousand, whose normal is its rounding's)
  const u32 tread_count = soil.index_count / 3;
  MESSAGE((tread_count - up) << " of " << tread_count << " tread triangles lean past 25 degrees");
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

// ---- steeply dipping beds (G200; rocks.md, "Beds at any attitude") ------------------------------

namespace {

// The steep-dip hillside: rising along +x (downhill is -x) from 35 degrees at x = 0 to 48 at
// x = 128, then a band at 60 degrees to x = 176 rounding over to a level crest by x = 208, with
// noses and hollows 1.5 m high and 40 m apart across z (lower than `Hill`'s, so no nose's flank
// passes the cliffs' 50 degrees and breaks the foot of the band into pieces). The ledges take the
// 35-48 degree ground, the cliffs the band.
constexpr f64 k_steep_base = 0.7;       // tan 35
constexpr f64 k_steep_grow = 0.003208;  // the slope's tangent gained a metre up: tan 48 at 128 m
constexpr f64 k_steep_band = 1.732;     // tan 60
f64 steep_m(f64 x, f64 z) {
  const f64 at128 = k_steep_base * 128.0 + 0.5 * k_steep_grow * 128.0 * 128.0;
  const f64 at176 = at128 + k_steep_band * 48.0;
  f64 h = 0.0;
  if (x <= 128.0)
    h = k_steep_base * x + 0.5 * k_steep_grow * x * x;
  else if (x <= 176.0)
    h = at128 + k_steep_band * (x - 128.0);
  else if (x >= 208.0)
    h = at176 + k_steep_band * 16.0;
  else
    h = at176 + k_steep_band * ((x - 176.0) - (x - 176.0) * (x - 176.0) / 64.0);
  return h + 1.5 * det::sin(z * (k_turn / 40.0));
}
f64 steep_hill(const void*, i64 x_mm, i64 z_mm) noexcept {
  return steep_m(static_cast<f64>(x_mm) / 1000.0, static_cast<f64>(z_mm) / 1000.0);
}

// The forest's formation (its v41 scene's `formation`: a gneiss sequence dipping 57 degrees, a
// macro sequence of members from a 141 m paragneiss to a 6 m graphitic schist), its folds off so
// the attitude is the one asked for.
scene::Formation forest_formation(f32 dip_deg, f32 azimuth_deg) {
  scene::Formation f;
  f.origin = WorldPos{0.0, 0.0, 0.0};
  f.dip_deg = dip_deg;
  f.azimuth_deg = azimuth_deg;
  f.soft_m = 0.7f;
  const auto add = [](Vector<scene::FormationBed>& to, const char* name, f32 t, f32 r) {
    to.push_back(bed(name, t, r));
  };
  add(f.beds, "felsic gneiss", 4.3f, 0.8f);
  add(f.beds, "biotite seam", 0.25f, 0.55f);
  add(f.beds, "banded gneiss", 2.1f, 0.7f);
  add(f.beds, "felsic gneiss", 7.8f, 0.8f);
  add(f.beds, "mafic seam", 0.6f, 0.6f);
  add(f.beds, "banded gneiss", 1.2f, 0.7f);
  add(f.beds, "quartz-rich", 3.4f, 0.85f);
  add(f.beds, "biotite gneiss", 0.9f, 0.55f);
  add(f.macro_beds, "orthogneiss", 96.0f, 0.8f);
  add(f.macro_beds, "amphibolite", 14.0f, 0.75f);
  add(f.macro_beds, "pelitic schist", 38.0f, 0.35f);
  add(f.macro_beds, "marble", 23.0f, 0.55f);
  add(f.macro_beds, "paragneiss", 141.0f, 0.7f);
  add(f.macro_beds, "calc-silicate", 9.0f, 0.65f);
  add(f.macro_beds, "quartzite", 31.0f, 0.9f);
  add(f.macro_beds, "graphitic schist", 6.0f, 0.3f);
  return f;
}

// What a steep-dip case made, for the table.
struct DipCase {
  u32 triangles = 0;
  f64 front_m = 0.0;
  u32 units = 0;
  f64 tallest = 0.0;
  f64 cover = 0.0;        // the share of the window's plan under rock or a tread
  f64 soil_steep = 0.0;   // the share of tread area that is soil on a plane past 30 degrees
  f64 tread_slope = 0.0;  // the area-weighted mean slope of the treads, degrees
  f64 face_down = 0.0;    // of the steep faces' area: facing downhill, uphill, along the contour
  f64 face_up = 0.0;
  f64 face_side = 0.0;
  f64 overhang = 0.0;    // of all rock area, facing down by more than 17 degrees
  f64 rule_share = 0.0;  // the share of the window's lattice points where the rule bares a bed
  // The same three ways for every rock triangle out of its bedding plane steeper than 45.6
  // degrees (risers on the steepest ground included), as the first run of this table counted.
  f64 wide_down = 0.0;
  f64 wide_up = 0.0;
  f64 wide_side = 0.0;
};

DipCase measure_steep(const LedgeLayer& layer, const scene_gen::Ground& ground, i64 x0_mm,
                      i64 x1_mm) {
  DipCase out;
  f64 plan = 0.0, tread = 0.0, tread_soil_steep = 0.0, tread_slope = 0.0, faces = 0.0, rock = 0.0;
  f64 down = 0.0, up = 0.0, side = 0.0, overhang = 0.0;
  // The beds' normal (the formation's folds are off here): leaning toward the dip direction.
  const f64 dip = static_cast<f64>(layer.formation->dip_deg) * (3.141592653589793 / 180.0);
  const f64 azimuth = static_cast<f64>(layer.formation->azimuth_deg) * (3.141592653589793 / 180.0);
  const f64 bed_n[3] = {std::sin(dip) * std::cos(azimuth), std::cos(dip),
                        std::sin(dip) * std::sin(azimuth)};
  std::string error;
  for (i64 x_mm = x0_mm; x_mm < x1_mm; x_mm += k_tile_mm)
    for (i64 z_mm = 0; z_mm < 2 * k_tile_mm; z_mm += k_tile_mm) {
      Solved s;
      REQUIRE_MESSAGE(
          sample_ledge_patch(layer, ground, x_mm, z_mm, k_tile_mm, false, s.patch, error), error);
      REQUIRE_MESSAGE(solve_ledges(s.patch, s.mesh, &s.report, error), error);
      out.triangles += s.report.triangles;
      out.front_m += s.report.front_length_m;
      out.units += s.report.units;
      out.tallest = std::max(out.tallest, s.report.tallest_m);
      for (const assets::Primitive& part : s.mesh.primitives) {
        const bool soil = s.mesh.materials[static_cast<u32>(part.material)].name == "soil";
        for (u32 t = part.first_index; t < part.first_index + part.index_count; t += 3) {
          const P3 a = world(s, s.mesh.indices[t]), b = world(s, s.mesh.indices[t + 1]),
                   c = world(s, s.mesh.indices[t + 2]);
          const P3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
          const P3 n2{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
          const f64 area = 0.5 * std::sqrt(n2.x * n2.x + n2.y * n2.y + n2.z * n2.z);
          if (!(area > 0.0)) continue;
          const P3 n = normal_of(s, t);
          if (n.y > 0.0) plan += area * n.y;
          if (soil) {
            tread += area;
            const f64 slope_deg = std::acos(std::clamp(n.y, -1.0, 1.0)) * 180.0 / 3.141592653589793;
            tread_slope += area * slope_deg;
            if (slope_deg > 30.0) tread_soil_steep += area;
            continue;
          }
          rock += area;
          if (n.y < -0.3) overhang += area;
          // A tread in its bedding plane (rock where it is steeper than the soil slope) is not a
          // face: the faces are the fronts, the rows and the flanks.
          if (n.x * bed_n[0] + n.y * bed_n[1] + n.z * bed_n[2] > 0.996) continue;
          // As G142's table first counted them (steeper than 45.6 degrees), for the
          // before-and-after.
          if (std::fabs(n.y) < 0.7) {
            const f64 h = std::sqrt(n.x * n.x + n.z * n.z);
            (-n.x / h > 0.5 ? out.wide_down : (-n.x / h < -0.5 ? out.wide_up : out.wide_side)) +=
                area;
          }
          // A face: steeper than 60 degrees, so no riser (a skin on the 35-48 degree ground)
          // counts.
          if (std::fabs(n.y) < 0.5) {
            faces += area;
            const f64 h = std::sqrt(n.x * n.x + n.z * n.z);
            const f64 along_fall = -n.x / h;  // downhill is -x
            if (along_fall > 0.5)
              down += area;
            else if (along_fall < -0.5)
              up += area;
            else
              side += area;
          }
        }
      }
    }
  const f64 window = static_cast<f64>(x1_mm - x0_mm) / 1000.0 * 128.0;
  out.cover = plan / window;
  out.soil_steep = tread > 0.0 ? tread_soil_steep / tread : 0.0;
  out.tread_slope = tread > 0.0 ? tread_slope / tread : 0.0;
  out.face_down = faces > 0.0 ? down / faces : 0.0;
  out.face_up = faces > 0.0 ? up / faces : 0.0;
  out.face_side = faces > 0.0 ? side / faces : 0.0;
  out.overhang = rock > 0.0 ? overhang / rock : 0.0;
  const f64 wide = out.wide_down + out.wide_up + out.wide_side;
  if (wide > 0.0) {
    out.wide_down /= wide;
    out.wide_up /= wide;
    out.wide_side /= wide;
  }
  // The rule alone, without the members and the stretches: where a resistant bed shows at all.
  ExposureThresholds rule;
  REQUIRE(prepare_exposure(layer.rule, rule, error));
  rule.seed = layer.seed;
  gfx::FormationParams field;
  REQUIRE(gfx::pack_formation(*layer.formation, field, &error));
  u32 bare = 0, points = 0;
  for (i64 x_mm = x0_mm + 1000; x_mm < x1_mm; x_mm += 2000)
    for (i64 z_mm = 1000; z_mm < 2 * k_tile_mm; z_mm += 2000) {
      bare += exposure_at(rule, field, ground, x_mm, z_mm).exposed ? 1u : 0u;
      ++points;
    }
  out.rule_share = static_cast<f64>(bare) / static_cast<f64>(points);
  return out;
}

}  // namespace

// The steep-dip table (G200, rocks.md "Beds at any attitude"): the forest's formation at four
// dips, each three ways to the slope, on the 35-50 degree ground of the steep hillside, and the
// cliffs layer on its 50-55 degree band. Printed for the page; what must hold is asserted.
TEST_CASE("ledges: steeply dipping beds, by their attitude to the slope") {
  scene_gen::Ground ground;
  ground.surface_fn = &steep_hill;
  struct Way {
    const char* name;
    f32 azimuth;
  };
  // Downhill is -x: the beds dip with the slope toward 180 degrees, into it toward 0, and their
  // strike runs up the slope when they dip toward +z.
  const Way ways[3] = {
      {"with the slope", 180.0f}, {"into the slope", 0.0f}, {"strike up the slope", 90.0f}};
  for (const f32 dip : {20.0f, 40.0f, 57.0f, 75.0f})
    for (const Way& way : ways) {
      LedgeLayer layer;
      layer.name = "steep";
      layer.seed = 11;
      layer.type = RockType::Gneiss;
      layer.formation = forest_formation(dip, way.azimuth);
      layer.region_min[0] = 0.0;
      layer.region_min[1] = 0.0;
      layer.region_max[0] = 128.0;
      layer.region_max[1] = 128.0;
      layer.cliff_slope_deg = 50.0f;
      const DipCase c = measure_steep(layer, ground, 0, 2 * k_tile_mm);
      // The cliffs layer on the band past 50 degrees.
      CliffLayer cliffs;
      cliffs.name = "steep";
      cliffs.seed = 7;
      cliffs.region_min[0] = 0.0;
      cliffs.region_min[1] = 0.0;
      cliffs.region_max[0] = 256.0;
      cliffs.region_max[1] = 256.0;
      cliffs.min_slope_deg = 50.0f;
      cliffs.min_length_m = 30.0f;
      cliffs.min_height_m = 8.0f;
      cliffs.face.type = RockType::Gneiss;
      cliffs.face.triangle_budget = 20000;
      cliffs.formation = layer.formation;
      Vector<CliffFace> faces;
      std::string error;
      u32 skipped = 0;
      REQUIRE_MESSAGE(trace_cliff_block(cliffs, ground, 0, 0, faces, &error, &skipped), error);
      CHECK(skipped == 0);
      u32 face_triangles = 0, benched = 0, membered = 0, carved = 0;
      f64 face_length = 0.0;
      for (const CliffFace& face : faces) {
        assets::MeshData mesh;
        CliffReport report;
        REQUIRE_MESSAGE(solve_cliff(face.cliff, mesh, &report, &error), error);
        face_triangles += report.triangles;
        face_length += face.lowest ? face.length : 0.0;
        benched += face.cliff.benches ? 1u : 0u;
        membered += face.cliff.members ? 1u : 0u;
        // A face's members (G200) move its front: the same face without them is not the same mesh.
        if (face.cliff.members && membered == 1) {
          Cliff plain = face.cliff;
          plain.members = false;
          assets::MeshData without;
          REQUIRE_MESSAGE(solve_cliff(plain, without, nullptr, &error), error);
          carved += mesh_hash(without) != mesh_hash(mesh) ? 1u : 0u;
        }
      }
      // The faces carve their members exactly where the beds dip past 20 degrees; under that the
      // benches step them (G172) and the faces are what they were.
      const u32 face_count = static_cast<u32>(faces.size());
      CHECK(face_count > 0u);
      CHECK(membered == (dip > 20.0f ? face_count : 0u));
      if (dip > 20.0f) CHECK(carved == 1u);
      if (dip <= 20.0f) CHECK(benched == face_count);
      MESSAGE("dip " << dip << ", " << std::string(way.name) << ": ledges " << c.units << " units, "
                     << c.front_m << " m of front, " << c.triangles << " triangles ("
                     << (c.front_m > 0.0 ? static_cast<f64>(c.triangles) / c.front_m : 0.0)
                     << " a metre), tallest " << c.tallest << " m, rock or tread over "
                     << c.cover * 100.0 << " % of the plan (the rule bares " << c.rule_share * 100.0
                     << " %); treads " << c.tread_slope << " degrees on the mean, "
                     << c.soil_steep * 100.0 << " % of their area turf past 30 degrees; faces "
                     << c.face_down * 100.0 << " % downhill, " << c.face_up * 100.0 << " % uphill, "
                     << c.face_side * 100.0
                     << " % along the contour (past 45.6 degrees: " << c.wide_down * 100.0 << ", "
                     << c.wide_up * 100.0 << ", " << c.wide_side * 100.0 << "); "
                     << c.overhang * 100.0 << " % of the rock overhangs. Cliffs: " << faces.size()
                     << " faces (" << benched << " benched), " << face_length << " m of foot, "
                     << face_triangles << " triangles");
      CHECK(c.triangles > 0);
      // No soil on a tread steeper than the soil slope: a dip slope's slab is rock (G142 put turf
      // on every tread, 100 % of it on planes past 30 degrees at every dip of 40 or more).
      CHECK(c.soil_steep == 0.0);
      // A front faces where its bed edge does (G142 recessed every row along the fall line, so at
      // 57 degrees with the strike up the slope 65 % of the faces steeper than 60 degrees faced
      // downhill and 30 % along the contour, the rows sliding sideways along their own fronts).
      if (dip >= 57.0f && way.azimuth == 90.0f) CHECK(c.face_side > 0.8);
      // Under 20 degrees of apparent dip a front is plumb: the shallow cases hardly overhang; past
      // it the slab edges and toppled beds do.
      if (dip <= 20.0f) CHECK(c.overhang < 0.01);
      if (dip == 57.0f && way.azimuth != 90.0f) CHECK(c.overhang > 0.02);
    }
}

// The steep-dip patch and face pinned to the bit (G200): the forest's formation at its own 57
// degrees and 128 degrees, folds and all, on the steep hillside. Taken on MSVC (msvc-debug,
// x86-64-v3); the containers reproduce them on Clang and GCC.
TEST_CASE("ledges: a steep-dip patch and face are pinned to the bit") {
  scene_gen::Ground ground;
  ground.surface_fn = &steep_hill;
  scene::Formation f = forest_formation(57.0f, 128.0f);
  f.fold_amplitude_m = 34.0f;
  f.fold_wavelength_m = 470.0f;
  f.fold2_amplitude_m = 210.0f;
  f.fold2_wavelength_m = 3300.0f;
  LedgeLayer layer = layer_of(f);
  layer.type = RockType::Gneiss;
  layer.cliff_slope_deg = 50.0f;
  std::string error;
  Solved s;
  REQUIRE_MESSAGE(sample_ledge_patch(layer, ground, k_tile_mm, 0, k_tile_mm, false, s.patch, error),
                  error);
  REQUIRE_MESSAGE(solve_ledges(s.patch, s.mesh, &s.report, error), error);
  CliffLayer cliffs;
  cliffs.name = "steep";
  cliffs.seed = 7;
  cliffs.region_min[0] = 0.0;
  cliffs.region_min[1] = 0.0;
  cliffs.region_max[0] = 256.0;
  cliffs.region_max[1] = 256.0;
  cliffs.face.type = RockType::Gneiss;
  cliffs.face.triangle_budget = 20000;
  cliffs.formation = f;
  Vector<CliffFace> faces;
  REQUIRE_MESSAGE(trace_cliff_block(cliffs, ground, 0, 0, faces, &error), error);
  REQUIRE(!faces.empty());
  REQUIRE(faces[0].cliff.members);
  assets::MeshData face;
  REQUIRE_MESSAGE(solve_cliff(faces[0].cliff, face, nullptr, &error), error);
  MESSAGE("steep pins: ledges " << mesh_hash(s.mesh) << " (" << s.report.triangles
                                << " triangles), face " << mesh_hash(face) << " ("
                                << face.indices.size() / 3 << " triangles)");
  CHECK(s.report.triangles > 0u);
  CHECK(mesh_hash(s.mesh) == 11016132626080586711ull);
  CHECK(mesh_hash(face) == 11392922738312473375ull);
}

// Printed, not asserted (rocks.md, "A massif inside a budget"): the steep hillside's four ledge
// tiles under the forest's own formation, near and far, as the generator solves them at `open`.
TEST_CASE("ledges: what a steep-dip tile costs") {
  scene_gen::Ground ground;
  ground.surface_fn = &steep_hill;
  scene::Formation f = forest_formation(57.0f, 128.0f);
  f.fold_amplitude_m = 34.0f;
  f.fold_wavelength_m = 470.0f;
  f.fold2_amplitude_m = 210.0f;
  f.fold2_wavelength_m = 3300.0f;
  LedgeLayer layer = layer_of(f);
  layer.type = RockType::Gneiss;
  layer.cliff_slope_deg = 50.0f;
  std::string error;
  for (const bool far : {false, true}) {
    u32 triangles = 0;
    f64 front = 0.0, ms = 0.0;
    usize bytes = 0;
    for (const i64 x_mm : {i64{0}, k_tile_mm})
      for (const i64 z_mm : {i64{0}, k_tile_mm}) {
        LedgePatch patch;
        const auto start = std::chrono::steady_clock::now();
        REQUIRE(sample_ledge_patch(layer, ground, x_mm, z_mm, k_tile_mm, far, patch, error));
        assets::MeshData mesh;
        LedgeReport report;
        REQUIRE(solve_ledges(patch, mesh, &report, error));
        ms += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                  .count();
        triangles += report.triangles;
        front += report.front_length_m;
        bytes += mesh.positions.size() * (sizeof(Vec3) * 2 + sizeof(Vec2)) +
                 mesh.indices.size() * sizeof(u32);
      }
    MESSAGE(std::string(far ? "far" : "near")
            << " form, four 64 m tiles of 35-48 degree ground at the forest's formation: "
            << triangles << " triangles, " << front << " m of front ("
            << (front > 0.0 ? static_cast<f64>(triangles) / front : 0.0) << " a metre), " << bytes
            << " bytes of source mesh, " << ms << " ms to sample and solve");
    CHECK(triangles > 0u);
  }
}

// A massif's far form (G200; rocks.md, "A massif inside a budget"): past `near_rings` a face is
// drawn as its far form, the same face at `far_budget` with no fallen blocks; inside them, and in a
// whole read, as written. No far form is the default, and a layer that asks for none is unchanged.
TEST_CASE("cliffs generator: a massif's faces past the near rings are their far form") {
  test::TempDir scratch("cliffs-far-form");
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement(k_cliffs_generator);
  REQUIRE(desc != nullptr);
  scene_gen::Context context;
  context.ground.surface_fn = &steep_hill;
  context.tile_size = 64.0f;
  const std::string dir = scratch.path();
  context.dir = dir;
  context.ddc = dir;
  CliffLayer layer;
  layer.name = "massif";
  layer.seed = 7;
  layer.region_min[0] = 0.0;
  layer.region_min[1] = 0.0;
  layer.region_max[0] = 256.0;
  layer.region_max[1] = 256.0;
  layer.face.type = RockType::Gneiss;
  layer.face.triangle_budget = 250000;  // the default, a tier's share of it
  layer.face.talus_blocks = 6;
  layer.face.talus = 0.1f;
  layer.formation = forest_formation(57.0f, 128.0f);
  std::string error;
  Opened plain;
  plain.desc = desc;
  REQUIRE_MESSAGE(desc->open(schema::to_json(layer), context, &plain.state, &error), error);
  Vector<scene_gen::PlacementMesh> as_written;
  REQUIRE(desc->meshes(plain.state, context, as_written, &error));
  layer.near_rings = 2;
  layer.far_budget = 4000;
  Opened ringed;
  ringed.desc = desc;
  REQUIRE_MESSAGE(desc->open(schema::to_json(layer), context, &ringed.state, &error), error);
  Vector<scene_gen::PlacementMesh> resident;
  REQUIRE(desc->meshes(ringed.state, context, resident, &error));
  // Every face as written, the same recipes as with no far form, then a far form each.
  REQUIRE(!as_written.empty());
  REQUIRE(resident.size() > as_written.size());
  for (u32 i = 0; i < as_written.size(); ++i)
    CHECK(resident[i].path == as_written[i].path);
  u32 near_triangles = 0, far_triangles = 0, faces = 0;
  f64 near_ms = 0.0, far_ms = 0.0;
  usize near_bytes = 0, far_bytes = 0;
  for (i32 tz = -1; tz < 5; ++tz)
    for (i32 tx = -1; tx < 5; ++tx) {
      context.ring = 1;
      scene_gen::Placements near_tile, far_tile;
      REQUIRE(desc->tile(ringed.state, scene_gen::TileCoord{tx, tz}, context, near_tile, &error));
      CHECK(desc->representation(ringed.state, context) == 0);
      context.ring = 2;
      REQUIRE(desc->tile(ringed.state, scene_gen::TileCoord{tx, tz}, context, far_tile, &error));
      CHECK(desc->representation(ringed.state, context) == 1);
      CHECK(far_tile.kind == 1);
      REQUIRE(near_tile.instances.size() == far_tile.instances.size());
      for (u32 k = 0; k < near_tile.instances.size(); ++k) {
        const scene_gen::Placement& a = near_tile.instances[k];
        const scene_gen::Placement& b = far_tile.instances[k];
        CHECK(a.position.x == b.position.x);
        CHECK(a.position.z == b.position.z);
        RockFile near_file, far_file;
        std::string text;
        REQUIRE(io::read_file(resident[a.mesh].path, text) == io::Status::Ok);
        REQUIRE_MESSAGE(read_rock_file(text, near_file, error), error);
        REQUIRE(io::read_file(resident[b.mesh].path, text) == io::Status::Ok);
        REQUIRE_MESSAGE(read_rock_file(text, far_file, error), error);
        assets::MeshData near_mesh, far_mesh;
        CliffReport near_report, far_report;
        auto start = std::chrono::steady_clock::now();
        REQUIRE_MESSAGE(solve_cliff(*near_file.cliff, near_mesh, &near_report, &error), error);
        near_ms += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                       .count();
        start = std::chrono::steady_clock::now();
        REQUIRE_MESSAGE(solve_cliff(*far_file.cliff, far_mesh, &far_report, &error), error);
        far_ms += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                      .count();
        for (const assets::MeshData* m : {&near_mesh, &far_mesh})
          (m == &near_mesh ? near_bytes : far_bytes) +=
              m->positions.size() * (sizeof(Vec3) * 2 + sizeof(Vec2)) +
              m->indices.size() * sizeof(u32);
        CHECK(far_file.cliff->talus_blocks == 0u);
        CHECK(far_report.triangles <= 4000u);
        CHECK(far_report.triangles <= near_report.triangles);
        CHECK(far_file.cliff->members == near_file.cliff->members);
        near_triangles += near_report.triangles;
        far_triangles += far_report.triangles;
        ++faces;
      }
    }
  // Printed for the page (rocks.md, "A massif inside a budget"): the band is 48 m of plan across
  // and 256 m along, 12,288 m2 of plan past 50 degrees. Timings beside other load are an upper
  // bound.
  MESSAGE(faces << " faces: " << near_triangles << " triangles as written, " << far_triangles
                << " as their far forms; " << near_bytes << " and " << far_bytes
                << " bytes of source mesh; solved in " << near_ms << " and " << far_ms << " ms");
  CHECK(faces > 0u);
  CHECK(far_triangles * 3u < near_triangles);
  // A far form needs a budget under the face's.
  layer.far_budget = 300000;
  Opened refused;
  refused.desc = desc;
  CHECK_FALSE(desc->open(schema::to_json(layer), context, &refused.state, &error));
  CHECK(error.find("far_budget") != std::string::npos);
}
