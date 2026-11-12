// The footbridge solver (docs/subsystems/bridges.md): the form rule at each form's smallest and
// largest crossing, every member seated, nothing in the channel below bankfull but a pier or a
// stone, the freeboard, closed outward-wound solids, the generator's tiles against its whole read,
// the approaches and the footprint, and the bytes pinned.

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/bridges/bridges.h>
#include <domain/materials/materials.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>

using namespace engine;
using namespace engine::bridges;

namespace {

// A stream `width` wide between banks `depth` over its bed, the bed a V from bank to bank, the
// ground beyond the banks rising `rise` per thousand away from the stream (a valley's side) on the
// far side and level on the near one; the water at half the depth.
Crossing stream(i32 width, i32 depth, u32 d84 = 0, i32 rise = 0, i64 x = 0, i64 z = 0) {
  Crossing c;
  c.route = 0;
  c.line = 1;
  c.station = 40;
  c.x_mm = x;
  c.z_mm = z;
  c.dir_x = 6000;
  c.dir_z = 8000;
  c.width_mm = static_cast<u32>(width);
  c.bed_mm = 1000;
  c.water_mm = 1000 + depth / 2;
  c.d84_mm = d84;
  c.tread_mm = 600;
  c.profile_step_mm = 250;
  const i32 reach = width / 2 + 6000;
  const i32 n = reach / 250 * 2 + 1;
  for (i32 i = 0; i < n; ++i) {
    const i64 px = static_cast<i64>(i - n / 2) * 250;
    const i64 ax = px < 0 ? -px : px;
    i64 y = 0;
    if (2 * ax < width)
      y = static_cast<i64>(depth) * (2 * ax) / width;
    else
      y = depth + (px > 0 ? static_cast<i64>(rise) * (ax - width / 2) / 1000 : 0);
    c.profile_mm.push_back(c.bed_mm + y);
  }
  return c;
}

BridgePlan solved(const Crossing& c) {
  BridgePlan p;
  std::string why;
  REQUIRE_MESSAGE(plan_bridge(c, crossing_seed(7, c), p, why), why);
  return p;
}

bool overlaps(i32 a0, i32 a1, i32 b0, i32 b1) { return a0 < b1 && b0 < a1; }

// What a member carries on: its top, or a hewn log's flat.
i32 top_of(const Member& m) { return m.hewn > 0 ? m.hewn : m.y1; }

// Every member rests on the member it names, or on (in) the ground; a member that spans (a slab, a
// log, a stringer) has a support under each of its ends.
void check_seated(const Crossing& c, const BridgePlan& p) {
  for (u32 i = 0; i < p.members.size(); ++i) {
    const Member& m = p.members[i];
    CAPTURE(i);
    CAPTURE(static_cast<u32>(m.kind));
    if (m.support < 0) {
      i64 lowest = profile_at(c, m.x0);
      for (i32 x = m.x0; x <= m.x1; x += 50)
        lowest = std::min(lowest, profile_at(c, x));
      CHECK(c.bed_mm + m.y0 <= lowest);
    } else {
      REQUIRE(static_cast<u32>(m.support) < i);
      const Member& s = p.members[static_cast<u32>(m.support)];
      CHECK(top_of(s) == m.y0);
      CHECK(overlaps(m.x0, m.x1, s.x0, s.x1));
      CHECK(overlaps(m.z0, m.z1, s.z0, s.z1));
    }
    if (m.kind == MemberKind::slab || m.kind == MemberKind::log || m.kind == MemberKind::stringer) {
      const i32 quarter = (m.x1 - m.x0) / 4;
      bool low = false, high = false;
      for (const Member& s : p.members) {
        if (top_of(s) != m.y0 || !overlaps(m.z0, m.z1, s.z0, s.z1)) continue;
        low = low || overlaps(m.x0, m.x0 + quarter, s.x0, s.x1);
        high = high || overlaps(m.x1 - quarter, m.x1, s.x0, s.x1);
      }
      CHECK(low);
      CHECK(high);
    }
  }
}

// Nothing in the channel below the bankfull level but a pier or a stepping stone, and no abutment
// or sill inside the bankfull width.
void check_channel(const Crossing& c, const BridgePlan& p) {
  const i32 half = static_cast<i32>(c.width_mm) / 2;
  for (const Member& m : p.members) {
    if (m.kind == MemberKind::pier || m.kind == MemberKind::stone) continue;
    CAPTURE(static_cast<u32>(m.kind));
    if (m.kind == MemberKind::abutment || m.kind == MemberKind::sill) {
      CHECK((m.x1 <= -half || m.x0 >= half));
    } else if (overlaps(m.x0, m.x1, -half, half)) {
      CHECK(m.y0 >= p.bankfull);
    }
  }
}

u64 plan_hash(const BridgePlan& p) {
  u64 h = hash_combine(static_cast<u64>(p.form), static_cast<u64>(p.deck));
  for (const Member& m : p.members) {
    const i64 v[9] = {m.x0, m.x1, m.y0, m.y1, m.z0, m.z1, m.support, m.hewn, m.seed};
    h = hash_combine(h, hash_bytes(v, sizeof(v)));
  }
  return h;
}

u64 mesh_hash(const MeshPart (&parts)[k_materials]) {
  u64 h = 0;
  for (const MeshPart& part : parts) {
    h = hash_combine(h, hash_bytes(part.positions.data(), part.positions.size() * sizeof(Vec3)));
    h = hash_combine(h, hash_bytes(part.normals.data(), part.normals.size() * sizeof(Vec3)));
    h = hash_combine(h, hash_bytes(part.uvs.data(), part.uvs.size() * sizeof(Vec2)));
    h = hash_combine(h, hash_bytes(part.indices.data(), part.indices.size() * sizeof(u32)));
  }
  return h;
}

Bridge recipe(const Crossing& c) {
  Bridge b;
  b.crossing = c;
  b.seed = crossing_seed(7, c);
  return b;
}

}  // namespace

TEST_CASE("bridges: the form rule at the smallest and the largest crossing of each form") {
  // Stepping stones: up to 500 mm deep and 4 m wide.
  CHECK(choose_form(stream(300, 300)) == BridgeForm::stones);
  CHECK(choose_form(stream(4000, 500)) == BridgeForm::stones);
  CHECK(choose_form(stream(4000, 501)) != BridgeForm::stones);
  CHECK(choose_form(stream(4002, 400)) != BridgeForm::stones);
  // A clapper where the bed is boulders, up to 8 m.
  CHECK(choose_form(stream(4002, 400, 256)) == BridgeForm::clapper);
  CHECK(choose_form(stream(1500, 800, 256)) == BridgeForm::clapper);
  CHECK(choose_form(stream(8000, 900, 400)) == BridgeForm::clapper);
  CHECK(choose_form(stream(8002, 900, 400)) == BridgeForm::stringers);
  CHECK(choose_form(stream(4002, 400, 255)) == BridgeForm::log);
  // A foot log to a 5 m span (the bankfull width and the setbacks), stringers to 9 m, a ford past.
  CHECK(choose_form(stream(600, 700)) == BridgeForm::log);
  CHECK(choose_form(stream(4200, 700)) == BridgeForm::log);
  CHECK(choose_form(stream(4202, 700)) == BridgeForm::stringers);
  CHECK(choose_form(stream(8200, 900)) == BridgeForm::stringers);
  CHECK(choose_form(stream(8202, 900)) == BridgeForm::none);
  BridgePlan none = solved(stream(12000, 1200));
  CHECK(none.form == BridgeForm::none);
  CHECK(none.members.empty());
}

TEST_CASE(
    "bridges: every member is seated, the channel is clear and the deck keeps its freeboard") {
  // Each form at its smallest and its largest crossing, on level banks and with the far bank
  // rising 30 % from its edge (one seat higher than the other).
  const Crossing cases[] = {
      stream(300, 300),       stream(4000, 500),      stream(4000, 500, 0, 300),
      stream(4002, 400, 256), stream(8000, 900, 400), stream(8000, 900, 400, 300),
      stream(600, 700),       stream(4200, 700),      stream(4200, 700, 0, 300),
      stream(4202, 700),      stream(8200, 900),      stream(8200, 1500, 0, 300),
  };
  for (const Crossing& c : cases) {
    const BridgePlan p = solved(c);
    CAPTURE(c.width_mm);
    CAPTURE(static_cast<u32>(p.form));
    REQUIRE(!p.members.empty());
    check_seated(c, p);
    check_channel(c, p);
    const i32 depth = bankfull_depth_mm(c);
    if (p.form == BridgeForm::log || p.form == BridgeForm::stringers) {
      // The underside over bankfull by the bankfull depth, at least 300 mm.
      CHECK(p.soffit >= p.bankfull + std::max(k_min_freeboard_mm, depth));
      CHECK(p.span >= static_cast<i32>(c.width_mm));
      CHECK(p.handrail == (p.form == BridgeForm::stringers && p.deck > k_rail_fall_mm));
    } else if (p.form == BridgeForm::clapper) {
      CHECK(p.soffit >= p.bankfull);
      CHECK(p.span >= static_cast<i32>(c.width_mm));
      for (const Member& m : p.members)
        if (m.kind == MemberKind::slab)
          CHECK(m.x1 - m.x0 <= k_clapper_max_slab_mm + k_clapper_pier_mm);
    } else {
      // A stone's top over the water, and the stones at most a stride apart.
      CHECK(p.deck == p.water + k_stone_rise_mm);
      for (u32 i = 1; i < p.members.size(); ++i)
        CHECK(p.members[i].x0 - p.members[i - 1].x1 <= k_stone_pitch_mm);
    }
  }
}

TEST_CASE("bridges: every member is a closed solid wound outward") {
  const Crossing cases[] = {stream(3000, 400), stream(6000, 800, 400), stream(3000, 800),
                            stream(7000, 900)};
  for (const Crossing& c : cases) {
    MeshPart parts[k_materials];
    BridgePlan plan;
    BridgeReport report;
    std::string error;
    REQUIRE_MESSAGE(solve_bridge(recipe(c), parts, plan, report, error), error);
    CAPTURE(static_cast<u32>(plan.form));
    f64 volume = 0, boxes = 0;
    for (const Member& m : plan.members)
      boxes += static_cast<f64>(m.x1 - m.x0) * (m.y1 - m.y0) * (m.z1 - m.z0) * 1e-9;
    for (const MeshPart& part : parts) {
      // Every directed edge between two positions is matched by its reverse: no hole, no
      // flipped face (a face wound the wrong way doubles its edges' direction).
      std::map<std::pair<std::array<f32, 3>, std::array<f32, 3>>, i32> edges;
      for (u32 t = 0; t < part.indices.size(); t += 3) {
        for (u32 e = 0; e < 3; ++e) {
          const Vec3 a = part.positions[part.indices[t + e]];
          const Vec3 b = part.positions[part.indices[t + (e + 1) % 3]];
          const std::array<f32, 3> ka{a.x, a.y, a.z}, kb{b.x, b.y, b.z};
          if (ka == kb) continue;
          if (ka < kb)
            ++edges[{ka, kb}];
          else
            --edges[{kb, ka}];
        }
        const Vec3 a = part.positions[part.indices[t]], b = part.positions[part.indices[t + 1]],
                   d = part.positions[part.indices[t + 2]];
        const auto w = [](f32 v) { return static_cast<f64>(v); };
        const f64 ax = w(a.x), ay = w(a.y), az = w(a.z), bx = w(b.x), by = w(b.y), bz = w(b.z);
        const f64 dx = w(d.x), dy = w(d.y), dz = w(d.z);
        volume +=
            (ax * (by * dz - bz * dy) - ay * (bx * dz - bz * dx) + az * (bx * dy - by * dx)) / 6.0;
      }
      u32 open = 0;
      for (const auto& [key, count] : edges)
        open += count != 0 ? 1u : 0u;
      CHECK(open == 0);
    }
    // Outward: the signed volume is positive and no more than the members' boxes (a rough stone
    // and a log fill less than theirs, a reversed solid would count negative).
    CHECK(volume > 0.5 * boxes);
    CHECK(volume <= boxes * 1.0001);
    CHECK(report.triangles == report.material_triangles[k_stone] +
                                  report.material_triangles[k_plank] +
                                  report.material_triangles[k_log]);
  }
}

TEST_CASE("bridges: the container is one mesh of three materials with a far form") {
  const Crossing c = stream(7000, 900);
  geometry::ClusterFileData data;
  BridgeReport report;
  std::string error;
  REQUIRE_MESSAGE(build_container(recipe(c), {}, {}, 0, data, report, error), error);
  CHECK(data.materials.size() == k_materials);
  CHECK(data.cluster_material.size() == data.mesh.mesh.clusters.size());
  bool worn[k_materials] = {};
  for (const u32 m : data.cluster_material) {
    REQUIRE(m < k_materials);
    worn[m] = true;
  }
  CHECK(worn[k_stone]);  // the abutments under the sills
  CHECK(worn[k_plank]);  // the deck and the rails
  CHECK(worn[k_log]);    // the stringers and the sills
  CHECK(report.root_triangles > 0);
  CHECK(report.root_triangles < report.triangles);
  CHECK(data.generator == "bridge");
  // A ford has nothing to build.
  geometry::ClusterFileData none;
  CHECK_FALSE(build_container(recipe(stream(12000, 1200)), {}, {}, 0, none, report, error));
}

TEST_CASE("bridges: each material names its role's set through the tile contract") {
  // Stand-in packs parsed from text (no pack on disk): the stone takes the layer's rock and falls
  // back to the generated granite, the sawn and the round timber their own roles, and a timber
  // role the pack lacks draws its constant (bridges.md, "The mesh").
  const auto set = [](const char* role, const char* file) {
    return std::string("\"") + role + R"(":{"tile_m":1,"normal_convention":"opengl","maps":{)" +
           R"("base_color":{"file":")" + file + R"(.png"},"normal":{"file":")" + file +
           R"(n.png"},"roughness":{"file":")" + file + R"(r.png"},"metallic":{"file":")" + file +
           R"(m.png"},"height":{"file":")" + file + R"(h.png"}}})";
  };
  const std::string head =
      R"({"format":"engine.materials.pack/1","name":"stand-in","version":"1","roles":{)";
  materials::Pack full, granite_only;
  std::string error;
  REQUIRE_MESSAGE(
      materials::parse_pack(head + set("rock/basalt", "b") + "," + set("rock/granite", "g") + "," +
                                set("wood/plank", "p") + "," + set("wood/log", "l") + "}}",
                            "C:/stand-in", full, error),
      error);
  REQUIRE(materials::parse_pack(head + set("rock/granite", "g") + "}}", "C:/stand-in", granite_only,
                                error));
  const auto image = [](const geometry::ClusterFileData& d, u8 m) -> std::string {
    const i32 k = d.materials[m].base_color_image;
    return k >= 0 ? d.image_paths[static_cast<u32>(k)] : std::string();
  };
  Bridge b = recipe(stream(7000, 900));  // stringers: stone, planks and logs
  b.stone_role = "rock/basalt";
  geometry::ClusterFileData data;
  BridgeReport report;
  REQUIRE_MESSAGE(
      build_container(b, assets::MeshGeneratorInputs{&full}, {}, 0, data, report, error), error);
  CHECK(image(data, k_stone) == "C:/stand-in/b.png");
  CHECK(image(data, k_plank) == "C:/stand-in/p.png");
  CHECK(image(data, k_log) == "C:/stand-in/l.png");
  // The rock the pack lacks falls back to the granite; the timber it lacks keeps its colour.
  REQUIRE(
      build_container(b, assets::MeshGeneratorInputs{&granite_only}, {}, 0, data, report, error));
  CHECK(image(data, k_stone) == "C:/stand-in/g.png");
  CHECK(image(data, k_plank).empty());
  CHECK(image(data, k_log).empty());
  CHECK(data.materials[k_plank].base_color.x != data.materials[k_log].base_color.x);
  // Stepping stones wear no timber, and name no timber image.
  REQUIRE(build_container(recipe(stream(2000, 300)), assets::MeshGeneratorInputs{&full}, {}, 0,
                          data, report, error));
  CHECK(image(data, k_stone) == "C:/stand-in/g.png");
  CHECK(image(data, k_plank).empty());
  CHECK(image(data, k_log).empty());
}

TEST_CASE(
    "bridges: the generator's tiles are its whole read, and a bridge does not depend on its tile") {
  Layer layer;
  layer.seed = 11;
  layer.crossings.push_back(stream(3000, 800, 0, 0, 5000, 7000));
  layer.crossings.push_back(stream(6000, 900, 400, 0, -40000, 3000));
  layer.crossings.push_back(stream(12000, 1200, 0, 0, 70000, -9000));  // a ford: nothing
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement("bridges");
  REQUIRE(desc != nullptr);
  CHECK(desc->tile_meshes);
  scene_gen::Context context;
  context.tile_size = 32.0f;
  context.ddc = "unused-ddc";
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(desc->open(schema::to_json(layer), context, &state, &error), error);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(state, context, whole, &error));
  CHECK(whole.instances.size() == 2);
  CHECK(whole.meshes.size() == 2);
  u32 found = 0;
  for (i32 tz = -2; tz <= 1; ++tz)
    for (i32 tx = -2; tx <= 3; ++tx) {
      scene_gen::Placements one;
      REQUIRE(desc->tile(state, {tx, tz}, context, one, &error));
      CHECK(desc->occupies(state, {tx, tz}) == (!one.instances.empty() || !one.shapes.empty()));
      for (u32 k = 0; k < one.instances.size(); ++k) {
        const scene_gen::Placement& p = one.instances[k];
        bool same = false;
        for (u32 w = 0; w < whole.instances.size(); ++w)
          if (whole.instances[w].id == p.id) {
            same = whole.meshes[whole.instances[w].mesh].recipe == one.meshes[p.mesh].recipe &&
                   whole.instances[w].position == p.position;
          }
        CHECK(same);
        ++found;
      }
    }
  CHECK(found == 2);
  desc->close(state);
  // The same crossing moved by 100 m is the same bridge, moved: its members do not read the tile.
  const Crossing a = stream(3000, 800, 0, 0, 5000, 7000);
  const Crossing b = stream(3000, 800, 0, 0, 105000, 7000);
  CHECK(plan_hash(solved(a)) == plan_hash(solved(b)));
}

TEST_CASE("bridges: the approaches bring the tread up to the deck, and the footprint is its plan") {
  // A foot log over a 3 m stream: its deck stands over both banks.
  const Crossing c = stream(3000, 800, 0, 0, 2000, -3000);
  const BridgePlan p = solved(c);
  REQUIRE(p.form == BridgeForm::log);
  const Approach a = approach_of(c, p, 150);
  CHECK(a.step_mm == 0);
  CHECK(a.length_mm[0] > 0);
  CHECK(a.length_mm[1] > 0);
  // At 15 %, the fill's top meets the ground where the approach ends and stands over it 100 mm
  // before: the run is the deck's height over the bank over 0.15, to the 100 mm.
  const i64 deck = c.bed_mm + p.deck;
  const i64 len = a.length_mm[0];
  CHECK(deck - 150 * len / 1000 <= profile_at(c, p.x_start - len));
  CHECK(deck - 150 * (len - 100) / 1000 > profile_at(c, p.x_start - (len - 100)));
  CHECK(len >= 1000);
  Layer layer;
  layer.crossings.push_back(c);
  layer.seed = 7;
  const JsonValue edits = approach_edits_json(layer, 150);
  REQUIRE(edits.as_array().size() == 2);
  const JsonValue& first = edits.as_array()[0];
  CHECK(first.find("mode")->as_string() == "Fill");
  // Its top at the deck's end is the deck: no step onto the bridge.
  CHECK(first.find("points")->as_array()[0].as_array()[1].as_float() ==
        doctest::Approx(static_cast<f64>(c.bed_mm + p.deck) / 1000.0).epsilon(1e-12));
  scene_gen::FootprintPoint corners[4];
  const scene_gen::Footprint shape = bridge_footprint(c, p, corners);
  CHECK(shape.covers_mm(c.x_mm, c.z_mm));
  i64 wx = 0, wz = 0;
  world_mm(c, p.x_end - 10, 0, wx, wz);
  CHECK(shape.covers_mm(wx, wz));
  world_mm(c, p.x_end + 20, 0, wx, wz);
  CHECK_FALSE(shape.covers_mm(wx, wz));
  // Stepping stones have no approach.
  const Crossing brook = stream(2000, 300);
  CHECK(approach_of(brook, solved(brook), 150).length_mm[0] == 0);
}

TEST_CASE(
    "bridges: a tile hands the plan of every bridge that reaches it, as the whole read does") {
  // Two bridges, one across a tile's corner so its plan reaches four tiles: a point of any tile is
  // under a deck by the tile's own shapes exactly when it is by the whole read's (scene_gen.md,
  // "The shapes a tile occupies"), so the sward keeps off a streamed tile's deck as off the whole
  // read's.
  Layer layer;
  layer.seed = 5;
  layer.crossings.push_back(stream(6000, 900, 0, 0, 32000, 32000));  // on four tiles' corner
  layer.crossings.push_back(stream(3000, 800, 0, 0, 5000, 7000));
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement("bridges");
  REQUIRE(desc != nullptr);
  REQUIRE(desc->plan_ground != nullptr);
  scene_gen::Context context;
  context.tile_size = 32.0f;
  context.ddc = "unused-ddc";
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(desc->open(schema::to_json(layer), context, &state, &error), error);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(state, context, whole, &error));
  REQUIRE(whole.shapes.size() == 2);
  scene_gen::Exclusion all;
  REQUIRE(all.add_footprints(whole.shapes.span(), &error));
  u32 covered = 0, points = 0;
  for (i32 tz = -1; tz <= 1; ++tz)
    for (i32 tx = -1; tx <= 1; ++tx) {
      scene_gen::Placements one;
      REQUIRE(desc->tile(state, {tx, tz}, context, one, &error));
      CHECK(desc->occupies(state, {tx, tz}) == (!one.instances.empty() || !one.shapes.empty()));
      scene_gen::Exclusion here;
      REQUIRE(here.add_footprints(one.shapes.span(), &error));
      // Every 250 mm of the tile within 12 m of either crossing.
      for (i64 z = tz * 32000; z < (tz + 1) * 32000; z += 250)
        for (i64 x = tx * 32000; x < (tx + 1) * 32000; x += 250) {
          bool near = false;
          for (const Crossing& c : layer.crossings)
            near = near || (std::max(x > c.x_mm ? x - c.x_mm : c.x_mm - x,
                                     z > c.z_mm ? z - c.z_mm : c.z_mm - z) < 12000);
          if (!near) continue;
          const scene_gen::Ground none;
          const bool whole_says = all.excluded_mm(none, x, z, scene_gen::k_exclude_footprints);
          CHECK(here.excluded_mm(none, x, z, scene_gen::k_exclude_footprints) == whole_says);
          covered += whole_says ? 1u : 0u;
          ++points;
        }
    }
  CHECK(covered > 100);  // the decks are there to be kept off
  CHECK(points > covered);
  // The hook hands each approach as a `Fill` edit of the ground, one per deck end over the ground.
  scene_gen::GroundPlan plan;
  REQUIRE_MESSAGE(desc->plan_ground(state, context, plan, &error), error);
  CHECK(plan.holes.empty());
  for (const scene::TerrainEdit& edit : plan.edits)
    CHECK(edit.mode == scene::TerrainEditMode::Fill);
  desc->close(state);
  // A grade out of range is refused when the entry is opened.
  layer.approach_grade_permille = 0;
  CHECK_FALSE(desc->open(schema::to_json(layer), context, &state, &error));
  CHECK(error.find("approach_grade_permille") != std::string::npos);
}

TEST_CASE("bridges: the same bytes on every toolchain, pinned") {
  // Pinned on MSVC (x64, msvc-debug) 2026-11-11; GCC and Clang at v3 and GCC at v2 reproduce them.
  const Crossing cases[] = {stream(3000, 400), stream(6000, 800, 400, 300), stream(3000, 800),
                            stream(7000, 900, 0, 300)};
  const u64 plan_pins[] = {0x1599dbc7f6e3f185ull, 0x9ca998a88eceb423ull, 0xfdce0357b88c500eull,
                           0xa2e065a587840fe5ull};
  const u64 mesh_pins[] = {0xf63ee472b509b0bdull, 0x5a94b3a18ac30237ull, 0xe53b4303fd18cbb4ull,
                           0x2c4e80eb15304fafull};
  for (u32 i = 0; i < 4; ++i) {
    MeshPart parts[k_materials];
    BridgePlan plan;
    BridgeReport report;
    std::string error;
    REQUIRE(solve_bridge(recipe(cases[i]), parts, plan, report, error));
    char line[96];
    std::snprintf(line, sizeof(line), "case %u: plan 0x%016llxull mesh 0x%016llxull", i,
                  static_cast<unsigned long long>(plan_hash(plan)),
                  static_cast<unsigned long long>(mesh_hash(parts)));
    INFO(line);
    CHECK(plan_hash(plan) == plan_pins[i]);
    CHECK(mesh_hash(parts) == mesh_pins[i]);
  }
}
