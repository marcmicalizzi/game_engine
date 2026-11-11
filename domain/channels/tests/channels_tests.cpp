// The channels capability (docs/subsystems/channels.md): the network on synthetic valleys, its
// determinism across thread counts and tile orders, the refusals, the placed meshes' topology and
// the water's description.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/channels/channels.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <vector>

using namespace engine;
using namespace engine::channels;

namespace {
// A tilted plane with a V-notch at x = 128 m, falling toward z = 0 at 5 %: the one place the
// flow gathers.
f64 notch(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  return .05 * zz + .15 * std::abs(xx - 128);
}
// A gentler valley, 0.6 % down its floor, with a low wave across the floor so the line has
// something to follow: meanders are allowed here.
f64 gentle(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 floor = 128 + 6 * std::sin(zz / 40.0);
  return .006 * zz + .04 * std::abs(xx - floor) + .2 * std::sin(xx / 13.0) * std::cos(zz / 17.0);
}
f64 nan_ground(const void*, i64, i64) noexcept { return std::numeric_limits<f64>::quiet_NaN(); }
// The steeper valleys, defined with the reaches' cases below: a step-pool reach (4.5 %) and a
// cascade (12 %), the two reaches whose boulders are what the generator places.
f64 step_pool(const void*, i64 x, i64 z) noexcept;
f64 steep(const void*, i64 x, i64 z) noexcept;

scene_gen::Ground ground(scene_gen::HeightFn fn) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  return g;
}
ChannelLayer layer(f64 depth = 512) {
  ChannelLayer p;
  p.name = "test";
  p.region_min = {0, 0};
  p.region_max = {256, depth};
  return p;
}
bool same_network(const ChannelNetwork& a, const ChannelNetwork& b) {
  if (a.ground_um != b.ground_um || a.filled_um != b.filled_um || a.receiver != b.receiver ||
      a.area_cells != b.area_cells || a.channel != b.channel || a.heads != b.heads ||
      a.links.size() != b.links.size())
    return false;
  for (u32 i = 0; i < a.links.size(); ++i) {
    const auto& x = a.links[i];
    const auto& y = b.links[i];
    if (x.cells != y.cells || x.seed != y.seed || x.line.size() != y.line.size()) return false;
    if (std::memcmp(x.line.data(), y.line.data(), x.line.size() * sizeof(ChannelPoint)) != 0)
      return false;
  }
  return true;
}

// W97: one clast's mesh is one welded surface, outward, with no degenerate triangle, whose open
// edges (where its buried triangles were left out) lie wholly under `under`.
void check_buried_open(const assets::MeshData& mesh, f64 under) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  std::vector<u64> directed;
  u32 degenerate = 0, against = 0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const u32 w[3] = {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
    const Vec3 a = mesh.positions[w[0]], b = mesh.positions[w[1]], c = mesh.positions[w[2]];
    if (!(length(cross(b - a, c - a)) > 0.0f)) ++degenerate;
    const Vec3 vn = mesh.normals[w[0]] + mesh.normals[w[1]] + mesh.normals[w[2]];
    if (dot(cross(b - a, c - a), vn) <= 0.0f) ++against;
    for (u32 e = 0; e < 3; ++e)
      directed.push_back(static_cast<u64>(w[e]) << 32 | w[(e + 1) % 3]);
  }
  CHECK(degenerate == 0);
  CHECK_MESSAGE(against == 0, against << " faces wound against their normals");
  std::sort(directed.begin(), directed.end());
  u32 repeated = 0, open_above = 0;
  for (u32 i = 0; i < directed.size(); ++i) {
    if (i > 0 && directed[i] == directed[i - 1]) ++repeated;
    const u64 reverse = (directed[i] & 0xffffffffull) << 32 | directed[i] >> 32;
    if (std::binary_search(directed.begin(), directed.end(), reverse)) continue;
    const f64 ya = static_cast<f64>(mesh.positions[directed[i] >> 32].y);
    const f64 yb = static_cast<f64>(mesh.positions[directed[i] & 0xffffffffull].y);
    if (ya > under + 1e-4 || yb > under + 1e-4) ++open_above;
  }
  CHECK_MESSAGE(repeated == 0, repeated << " edges used twice in one direction");
  CHECK_MESSAGE(open_above == 0, open_above << " open edges above the ground: a visible hole");
}

struct Opened {
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement("channels");
  void* state = nullptr;
  ~Opened() {
    if (state) desc->close(state);
  }
};
}  // namespace

TEST_CASE(
    "channels: one channel down a V-notch, its head at the threshold, its width by the "
    "exponent") {
  const ChannelLayer p = layer();
  ChannelNetwork n;
  std::string error;
  REQUIRE_MESSAGE(plan_channels(p, ground(notch), 0, nullptr, n, error), error);
  CHECK(n.width == 65);
  CHECK(n.depth == 129);
  REQUIRE(n.links.size() == 1);
  REQUIRE(n.heads.size() == 1);
  // Every channel cell is in the notch.
  for (u32 i = 0; i < n.channel.size(); ++i)
    if (n.channel[i]) CHECK(i % n.width == 32);
  // The head is the first notch cell where area times slope reaches the threshold: it does, the
  // cell above it does not.
  const f64 cell = 16.0;
  const u32 head = n.heads[0];
  auto product = [&](u32 i) {
    const u32 r = n.receiver[i];
    const f64 slope = static_cast<f64>(n.ground_um[i] - n.ground_um[r]) / 1e6 / 4.0;
    return static_cast<f64>(n.area_cells[i]) * cell * slope;
  };
  CHECK(product(head) >= static_cast<f64>(p.head_threshold_m2));
  CHECK(product(head + n.width) < static_cast<f64>(p.head_threshold_m2));
  CHECK(static_cast<f64>(n.area_cells[head]) * cell >= static_cast<f64>(p.min_head_area_m2));
  // The link runs from the head to the outlet on the region's edge, down the notch's line.
  const ChannelLink& link = n.links[0];
  CHECK(link.head);
  CHECK(link.cells.front() == head);
  CHECK(link.cells.back() / n.width == 0);
  CHECK(link.meander == doctest::Approx(0.0));  // 5 % is above the meander limit
  REQUIRE(link.line.size() > 10);
  // Down the notch's line, within a steep line's wander of it (W93: `steep_wander_widths` of the
  // width, of a metre at least; the trough test holds it to less where the notch's sides rise).
  f64 off_line = 0;
  for (const ChannelPoint& pt : link.line) {
    CHECK(std::abs(pt.at.x - 128) <
          static_cast<f64>(p.steep_wander_widths) * std::max(1.0, static_cast<f64>(pt.width_m)) +
              .01);
    off_line = std::max(off_line, std::abs(pt.at.x - 128));
  }
  MESSAGE("the notch's line: " << off_line << " m off it at the most");
  // Width grows downstream as area to the 0.4.
  const ChannelPoint& a = link.line[link.line.size() / 4];
  const ChannelPoint& b = link.line.back();
  CHECK(b.area_m2 > a.area_m2);
  CHECK(b.width_m > a.width_m);
  const f64 ratio = std::pow(static_cast<f64>(b.area_m2) / static_cast<f64>(a.area_m2), .4);
  CHECK(static_cast<f64>(b.width_m) / static_cast<f64>(a.width_m) ==
        doctest::Approx(ratio).epsilon(1e-4));
  CHECK(static_cast<f64>(b.width_m) ==
        doctest::Approx(3.0 * std::pow(static_cast<f64>(b.area_m2) / 1e6, .4)).epsilon(1e-4));
  // A threshold beyond the valley's area: no channel at all.
  ChannelLayer dry = p;
  dry.head_threshold_m2 = 1e6;
  REQUIRE(plan_channels(dry, ground(notch), 0, nullptr, n, error));
  CHECK(n.links.empty());
  CHECK(n.heads.empty());
}

TEST_CASE("channels: the same network at one job and six, and meanders where the slope allows") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  ChannelNetwork serial, parallel;
  std::string error;
  REQUIRE_MESSAGE(plan_channels(p, ground(gentle), 7, nullptr, serial, error), error);
  jobs::JobSystemConfig config;
  config.performance_workers = 6;
  config.efficiency_workers = 0;
  config.pin_threads = false;
  jobs::JobSystem js(config);
  REQUIRE(plan_channels(p, ground(gentle), 7, &js, parallel, error));
  CHECK(same_network(serial, parallel));
  REQUIRE_FALSE(serial.links.empty());
  bool bent = false;
  for (const ChannelLink& link : serial.links)
    bent = bent || link.meander > .5;
  CHECK(bent);
  // (Over 400 m until W93 low-passed the kinks out of the line: 397.9 m without them.)
  MESSAGE("the gentle valley's channels: " << serial.length_m << " m");
  CHECK(serial.length_m > 390);
  Vector<PlacedPiece> a, b;
  REQUIRE(place_pieces(p, serial, ground(gentle), 32, a, error));
  REQUIRE(place_pieces(p, parallel, ground(gentle), 32, b, error));
  REQUIRE(a.size() == b.size());
  for (u32 i = 0; i < a.size(); ++i) {
    CHECK(piece_json(a[i].recipe) == piece_json(b[i].recipe));
    CHECK(a[i].id == i + 1);
  }
  // Another world seed bends the same channels differently and routes them the same.
  ChannelNetwork other;
  REQUIRE(plan_channels(p, ground(gentle), 8, nullptr, other, error));
  CHECK(other.heads == serial.heads);
  CHECK_FALSE(same_network(other, serial));
}

TEST_CASE(
    "channels: the placed pieces are closed, welded and outward, and build the same "
    "container twice") {
  // What is placed is the boulders of a step-pool reach's steps and of a cascade's clusters
  // (W93, continued): every piece of both valleys is checked.
  const ChannelLayer p = layer();
  ChannelNetwork n;
  std::string error;
  Vector<PlacedPiece> pieces;
  for (const scene_gen::HeightFn valley : {&step_pool, &steep}) {
    REQUIRE(plan_channels(p, ground(valley), 3, nullptr, n, error));
    Vector<PlacedPiece> laid;
    REQUIRE(place_pieces(p, n, ground(valley), 32, laid, error));
    REQUIRE_FALSE(laid.empty());
    for (const PlacedPiece& piece : laid) {
      assets::MeshData mesh;
      REQUIRE_MESSAGE(solve_piece(piece.recipe, mesh, error), error);
      CHECK(piece.recipe.part == ChannelPart::Clasts);
      pieces.push_back(piece);
      // W97: each clast alone (the first 40 of a piece) is one welded, outward surface whose only
      // open edges lie under the ground beneath it, where its buried triangles were left out.
      for (u32 k = 0; k < std::min<u32>(40, clast_count(piece.recipe)); ++k) {
        ChannelPiece one = piece.recipe;
        one.clasts.assign(piece.recipe.clasts.begin() + k * k_clast_ints,
                          piece.recipe.clasts.begin() + (k + 1) * k_clast_ints);
        assets::MeshData alone;
        REQUIRE_MESSAGE(solve_piece(one, alone, error), error);
        check_buried_open(alone, static_cast<f64>(clast_of(one, 0).ground_mm) / 1000.0 - .02);
      }
    }
  }
  // The mesh generator "channel" reads the recipe's own JSON and builds the same container.
  const auto* desc = assets::MeshGeneratorRegistry::global().find("channel");
  REQUIRE(desc != nullptr);
  const std::string text = piece_json(pieces[0].recipe);
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());
  u64 hash = 0;
  REQUIRE(desc->source_hash(bytes, hash, error));
  CHECK(hash == piece_source_hash(pieces[0].recipe));
  geometry::ClusterLodOptions lod;
  geometry::ClusterFileData first, second;
  REQUIRE_MESSAGE(desc->build(bytes, {}, lod, 0, first, error), error);
  REQUIRE(build_piece_container(pieces[0].recipe, lod, 0, second, error));
  CHECK(first.source_hash == second.source_hash);
  CHECK(first.build_key == second.build_key);
  CHECK(first.mesh.mesh.clusters.size() == second.mesh.mesh.clusters.size());
  CHECK(first.mesh.mesh.vertices.size() == second.mesh.mesh.vertices.size());
  CHECK(std::memcmp(first.mesh.mesh.vertices.data(), second.mesh.mesh.vertices.data(),
                    first.mesh.mesh.vertices.size() * sizeof(Vec3)) == 0);
  CHECK(first.generator == "channel");
}

TEST_CASE("channels: whole expansion equals any tile order, with stable ids") {
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground(step_pool);  // a reach with steps: the boulders are what is placed
  c.tile_size = 64;
  Opened opened;
  REQUIRE(opened.desc != nullptr);
  ChannelLayer p = layer();
  std::string error;
  REQUIRE_MESSAGE(opened.desc->open(schema::to_json(p), c, &opened.state, &error), error);
  scene_gen::Placements whole;
  REQUIRE(opened.desc->expand(opened.state, c, whole, &error));
  REQUIRE_FALSE(whole.instances.empty());
  // **A tile's bed arrives with the tile** (W97b; scene_gen.md, "A tile's meshes"): the entry lists
  // nothing resident, says its tiles hand meshes, and writes no recipe file.
  CHECK(opened.desc->meshes == nullptr);
  CHECK(opened.desc->tile_meshes);
  // Each tile is handed alone (a host's `Placements` holds one tile's call), and what it hands is
  // gathered here as (id, placement, the recipe its placement names, the mesh's path and reach).
  struct Handed {
    u32 id;
    scene_gen::Placement at;
    std::string recipe, path;
    f32 reach;
  };
  auto activate = [&](i32 x, i32 z, std::vector<Handed>& into) {
    scene_gen::Placements one;
    REQUIRE(opened.desc->tile(opened.state, {x, z}, c, one, &error));
    CHECK(one.meshes.size() == one.instances.size());  // one piece a tile, its own mesh
    for (const scene_gen::Placement& placed : one.instances) {
      REQUIRE(placed.mesh < one.meshes.size());
      const scene_gen::PlacementMesh& m = one.meshes[placed.mesh];
      into.push_back({placed.id, placed, m.recipe, m.path, m.reach});
    }
    return one.instances.size();
  };
  std::vector<Handed> forward, backward;
  // W97: a clast near the region's edge may stand a little past it, in the tile beyond.
  for (i32 z = -1; z < 9; ++z)
    for (i32 x = -1; x < 5; ++x)
      (void)activate(x, z, forward);
  for (i32 z = 8; z >= -1; --z)
    for (i32 x = 4; x >= -1; --x)
      if (opened.desc->occupies(opened.state, {x, z})) CHECK(activate(x, z, backward) > 0);
  auto by_id = [](const Handed& a, const Handed& b) { return a.id < b.id; };
  std::sort(forward.begin(), forward.end(), by_id);
  std::sort(backward.begin(), backward.end(), by_id);
  REQUIRE(forward.size() == whole.instances.size());
  REQUIRE(backward.size() == whole.instances.size());
  for (u32 i = 0; i < whole.instances.size(); ++i) {
    const scene_gen::PlacementMesh& listed = whole.meshes[whole.instances[i].mesh];
    CHECK(whole.instances[i].id == i + 1);
    CHECK(forward[i].id == whole.instances[i].id);
    CHECK(backward[i].at.position == whole.instances[i].position);
    // What a tile hands is what the whole read lists, recipe for recipe, and the recipe is the
    // mesh generator's own bytes: nothing is read from or written to disk.
    CHECK(forward[i].recipe == listed.recipe);
    CHECK(backward[i].recipe == listed.recipe);
    CHECK(backward[i].path == listed.path);
    CHECK_FALSE(listed.recipe.empty());
    CHECK_FALSE(io::exists(listed.path));
    CHECK(listed.reach > 0.0f);
    CHECK(forward[i].reach == listed.reach);
  }
}

// **The same bytes from any start and any thread count** (W97b): two entries opened over one
// ground, one planning with no job system and one with six workers, hand every tile the same
// recipe in whichever order their tiles activate.
TEST_CASE("channels: a tile's bed is the same bytes whichever tile activates first, any jobs") {
  test::TempDir one_dir, six_dir;
  jobs::JobSystemConfig config;
  config.performance_workers = 6;
  config.efficiency_workers = 0;
  config.pin_threads = false;
  jobs::JobSystem js(config);
  scene_gen::Context serial, parallel;
  for (scene_gen::Context* c : {&serial, &parallel}) {
    c->ground = ground(gentle);  // a pool-riffle valley with bars and a flood bed
    c->tile_size = 64;
  }
  const std::string one_path = one_dir.path(), six_path = six_dir.path();
  serial.dir = one_path;
  serial.ddc = one_path;
  parallel.dir = six_path;
  parallel.ddc = six_path;
  parallel.jobs_context = &js;
  parallel.jobs_fn = [](void* context) { return static_cast<jobs::JobSystem*>(context); };
  Opened a, b;
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  std::string error;
  REQUIRE_MESSAGE(a.desc->open(schema::to_json(p), serial, &a.state, &error), error);
  REQUIRE_MESSAGE(b.desc->open(schema::to_json(p), parallel, &b.state, &error), error);
  std::vector<std::pair<u32, std::string>> first, second;
  auto take = [&](Opened& o, const scene_gen::Context& c, i32 x, i32 z, auto& into) {
    scene_gen::Placements one;
    REQUIRE(o.desc->tile(o.state, {x, z}, c, one, &error));
    for (const scene_gen::Placement& at : one.instances)
      into.emplace_back(at.id, one.meshes[at.mesh].recipe);
  };
  for (i32 z = -1; z < 9; ++z)
    for (i32 x = -1; x < 5; ++x)
      take(a, serial, x, z, first);
  for (i32 z = 8; z >= -1; --z)
    for (i32 x = 4; x >= -1; --x)
      take(b, parallel, x, z, second);
  std::sort(first.begin(), first.end());
  std::sort(second.begin(), second.end());
  REQUIRE_FALSE(first.empty());
  CHECK(first == second);
}

TEST_CASE("channels: the refusals") {
  std::string error;
  ChannelNetwork n;
  ChannelLayer p = layer();
  CHECK_FALSE(plan_channels(p, scene_gen::Ground{}, 0, nullptr, n, error));
  CHECK_FALSE(plan_channels(p, ground(nan_ground), 0, nullptr, n, error));
  CHECK(n.links.empty());
  CHECK(n.ground_um.empty());
  p.spacing_m = 4.0005f;
  CHECK_FALSE(layer_in_range(p, error));
  p = layer();
  p.region_min = {1, 0};
  CHECK_FALSE(layer_in_range(p, error));
  CHECK(error.find("region alignment") != std::string::npos);
  p = layer();
  p.meander_slope_min = .05f;
  CHECK_FALSE(layer_in_range(p, error));
  p = layer();
  p.max_samples = 100;
  CHECK_FALSE(plan_channels(p, ground(notch), 0, nullptr, n, error));
  CHECK(error.find("max_samples") != std::string::npos);
  p = layer();
  p.boulders.tile_m = 0.0f;
  CHECK_FALSE(layer_in_range(p, error));
  p = layer();
  p.head_threshold_m2 = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(layer_in_range(p, error));
  p = layer();
  p.clast_min_mm = 8;
  CHECK_FALSE(layer_in_range(p, error));
  // W97: a clast whose axes are out of order, whose direction is no unit vector, or a short array.
  ChannelPiece piece;
  piece.surface.color = std::array<f32, 3>{.5f, .5f, .5f};
  piece.clasts = {0, 0, 0, -50, 100, 70, 40, 10000, 0, 3000, 500, 7};
  CHECK(piece_in_range(piece, error));
  piece.clasts[5] = 120;
  CHECK_FALSE(piece_in_range(piece, error));
  piece.clasts[5] = 70;
  piece.clasts[8] = 5000;
  CHECK_FALSE(piece_in_range(piece, error));
  piece.clasts[8] = 0;
  piece.clasts.pop_back();
  CHECK_FALSE(piece_in_range(piece, error));
  // A moving ground, and a layer the schema refuses.
  scene_gen::Context c;
  c.ground = ground(notch);
  c.stand = ground(gentle);
  Opened moving;
  CHECK_FALSE(moving.desc->open(schema::to_json(layer()), c, &moving.state, &error));
  CHECK(error.find("static ground") != std::string::npos);
  c.stand = {};
  JsonValue bad = schema::to_json(layer());
  bad["spacing_m"] = JsonValue("four");
  Opened refused;
  CHECK_FALSE(refused.desc->open(bad, c, &refused.state, &error));
}

TEST_CASE("channels: the water's description has the WaterChannel shape") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 0, nullptr, n, error));
  const JsonValue all = water_channels_json(p, n, 0, .5f);
  REQUIRE(all.is_array());
  REQUIRE(all.as_array().size() >= 1);
  u32 big = 0;
  for (const ChannelLink& link : n.links)
    big += link.line.back().area_m2 >= 20000.0f;
  CHECK(water_channels_json(p, n, 20000, .5f).as_array().size() == big);
  for (const JsonValue& o : all.as_array()) {
    REQUIRE(o.is_object());
    // W90 added `reach`, one code a point, to W86's six.
    CHECK(o.as_object().size() == 7);
    for (const char* key : {"name", "points", "width_m", "depth_m", "slope", "seed", "reach"})
      REQUIRE_MESSAGE(o.find(key) != nullptr, key);
    CHECK(o.find("name")->is_string());
    CHECK(o.find("seed")->is_uint());
    const auto& points = o.find("points")->as_array();
    const auto& widths = o.find("width_m")->as_array();
    const auto& depths = o.find("depth_m")->as_array();
    const auto& slopes = o.find("slope")->as_array();
    REQUIRE(points.size() >= 2);
    CHECK(widths.size() == points.size());
    CHECK(depths.size() == points.size());
    CHECK(slopes.size() == points.size());
    f64 previous = std::numeric_limits<f64>::infinity();
    for (u32 i = 0; i < points.size(); ++i) {
      REQUIRE(points[i].as_array().size() == 3);
      f64 y = 0, w = 0, d = 0, s = 0;
      REQUIRE(points[i][1].get_f64(y));
      REQUIRE(widths[i].get_f64(w));
      REQUIRE(depths[i].get_f64(d));
      REQUIRE(slopes[i].get_f64(s));
      CHECK(y <= previous);  // the surface never rises downstream
      previous = y;
      CHECK(w > 0);
      CHECK(d >= 0);
      CHECK(s > 0);
    }
  }
  // Written and read back, the same text: what the subcommand writes is this function's.
  const std::string text = write_json(all);
  JsonValue back;
  REQUIRE(parse_json(text, back).ok);
  CHECK(write_json(back) == text);
}

// **The bank's reach travels with the water** (T78; scene_gen.md, "Exclusion"): a recipe whose
// banks are wider than the defaults hands that width to the placements' exclusion through the
// water's description, so a point between the default reach and the recipe's is left bare. Until
// T78 the exclusion copied the defaults as constants and grew grass there.
TEST_CASE("channels: the exclusion reaches the recipe's bank width, not the default") {
  ChannelLayer p = layer();
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(notch), 0, nullptr, n, error));
  // The same network written twice: at the recipe's defaults, and with banks a width and 2 m wide.
  const JsonValue plain = water_channels_json(p, n, 0, .5f, true);
  ChannelLayer wide_banks = p;
  wide_banks.bank_width_widths = 1.0f;
  wide_banks.bank_width_m = 2.0f;
  const JsonValue wide = water_channels_json(wide_banks, n, 0, .5f, true);
  const auto exclusion_of = [&](const JsonValue& blocks, scene_gen::Exclusion& out,
                                Vector<scene::WaterChannel>& channels) {
    for (const JsonValue& block : blocks.as_array()) {
      scene::WaterChannel channel;
      schema::ReadContext reader;
      REQUIRE(schema::from_json(channel, block, reader));
      REQUIRE(reader.ok());
      channels.push_back(std::move(channel));
    }
    REQUIRE(out.build({}, {}, {channels.data(), channels.size()}, &error));
  };
  Vector<scene::WaterChannel> plain_channels, wide_channels;
  scene_gen::Exclusion at_default, at_recipe;
  exclusion_of(plain, at_default, plain_channels);
  exclusion_of(wide, at_recipe, wide_channels);
  REQUIRE(!plain_channels.empty());
  // Across the middle of every segment of the first ribbon, halfway between the default bank edge
  // (0.75 w + 0.4 m from the line) and the recipe's (1.5 w + 2 m): outside the one, inside the
  // other.
  const scene::WaterChannel& c = plain_channels[0];
  const scene_gen::Ground flat;
  u32 between = 0;
  for (u32 i = 0; i + 1 < c.points.size(); ++i) {
    const f64 dx = c.points[i + 1].x - c.points[i].x;
    const f64 dz = c.points[i + 1].z - c.points[i].z;
    const f64 len = std::sqrt(dx * dx + dz * dz);
    if (!(len > 0.0)) continue;
    const f64 w = 0.5 * (static_cast<f64>(c.width_m[i]) + static_cast<f64>(c.width_m[i + 1]));
    const f64 d = 0.5 * ((0.75 * w + 0.4) + (1.5 * w + 2.0));
    const f64 x = 0.5 * (c.points[i].x + c.points[i + 1].x) - dz / len * d;
    const f64 z = 0.5 * (c.points[i].z + c.points[i + 1].z) + dx / len * d;
    const i64 x_mm = std::llround(x * 1000.0);
    const i64 z_mm = std::llround(z * 1000.0);
    // A bend can bring another segment's default bank over the point; only the clean ones count.
    if (at_default.excluded_mm(flat, x_mm, z_mm, scene_gen::k_exclude_channels)) continue;
    ++between;
    CHECK_MESSAGE(at_recipe.excluded_mm(flat, x_mm, z_mm, scene_gen::k_exclude_channels),
                  "segment " << i << ": " << d << " m from the line, inside the recipe's bank");
  }
  CHECK(between > 10);
  // At the defaults the description is the one W91 wrote, field for field: nothing new is written.
  for (const JsonValue& block : plain.as_array()) {
    CHECK(block.find("bank_width_widths") == nullptr);
    CHECK(block.find("bank_width_m") == nullptr);
  }
}

// **The cuts** (channels.md, "The cut", "The section"; W89, W93): one per link with the TerrainCut
// shape — a slope a bank a point, a rounded top, and a flood bed where the link has one — its
// section the trapezoid where the line is straight and deeper against the outer bank at a bend,
// and the water's surface in the cut lying between its bed and the top of the bank.
TEST_CASE("channels: the cuts have the TerrainCut shape and the water lies inside them") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 0, nullptr, n, error));
  const JsonValue cuts = terrain_cuts_json(p, n);
  const JsonValue water = water_channels_json(p, n, 0, .5f, true);
  REQUIRE(cuts.is_array());
  u32 links = 0;
  for (const ChannelLink& link : n.links)
    links += link.line.size() >= 2;
  REQUIRE(cuts.as_array().size() == links);
  // Every link drains at least 0 m2; a tributary wholly inside its trunk's banks carries none of
  // its own (W90), and a tributary's water stops at the trunk's banks: a prefix of its cut.
  u32 blocks = 0;
  for (u32 k = 0; k < links; ++k) {
    const JsonValue* block = nullptr;
    for (const JsonValue& b : water.as_array())
      if (b.find("name")->as_string() == p.name + " link " + std::to_string(k)) block = &b;
    if (block != nullptr) ++blocks;
    const JsonValue& o = cuts[k];
    // Every cut is shaped: the eight fields, and five more where the link has a flood bed.
    CHECK((o.as_object().size() == 8 || o.as_object().size() == 13));
    scene::TerrainEdit cut;
    schema::ReadContext reader;
    REQUIRE(schema::from_json(cut, o, reader));
    REQUIRE(reader.ok());
    CHECK(cut.bank_slope == p.bank_slope);
    CHECK(cut.bank_round == p.bank_round);
    REQUIRE(cut.bank_slope_left.size() == cut.points.size());
    REQUIRE(cut.bank_slope_right.size() == cut.points.size());
    CHECK((cut.flood_left_m.empty() || cut.flood_left_m.size() == cut.points.size()));
    REQUIRE(cut.points.size() >= 2);
    REQUIRE(cut.width_m.size() == cut.points.size());
    REQUIRE(cut.depth_m.size() == cut.points.size());
    const ChannelLink& link = n.links[k];
    REQUIRE(link.line.size() == cut.points.size());
    if (block == nullptr) {
      CHECK(link.keep < 2);
      continue;
    }
    const auto& surface = block->find("points")->as_array();
    REQUIRE(surface.size() == std::min<u32>(link.keep, cut.points.size()));
    for (u32 i = 0; i < cut.points.size(); ++i) {
      const ChannelPoint& pt = link.line[i];
      const f64 top = cut.points[i].y;
      const f64 bed = top - static_cast<f64>(cut.depth_m[i]);
      CHECK(std::fabs(top - pt.at.y) <= 5e-4 + 1e-9);  // the ground at the line, to the mm
      // The section's thalweg: D (W - D / m) = W d, the area the hydraulic geometry's mean depth
      // gives, at least, where the bed runs through no rise (or a V of twice the mean).
      const f64 w = static_cast<f64>(pt.width_m), mean = static_cast<f64>(pt.depth_m);
      const f64 m = static_cast<f64>(p.bank_slope);
      const f64 d = static_cast<f64>(cut.depth_m[i]);
      const f64 thalweg =
          4 * mean / (m * w) >= 1 ? 2 * mean : .5 * m * w * (1 - std::sqrt(1 - 4 * mean / (m * w)));
      // The banks: the layer's slope both sides where the section is symmetric, the outer bank
      // the steeper at a bend (positive asymmetry bends left, so the outer bank is the right).
      const f64 asym = static_cast<f64>(link.asym[i]);
      const f64 left = static_cast<f64>(cut.bank_slope_left[i]);
      const f64 right = static_cast<f64>(cut.bank_slope_right[i]);
      CHECK((left >= .02 && left <= 16 && right >= .02 && right <= 16));
      if (asym == 0) CHECK((std::fabs(left - m) <= 1e-4 && std::fabs(right - m) <= 1e-4));
      if (asym > .05) CHECK(right >= left);
      if (asym < -.05) CHECK(left >= right);
      // At least nine tenths of a thalweg under the top (a riffle's section is up to 6 % wider and
      // that much shallower), and never more than two (the cap over a sill) and a bend's pool
      // (twice the mean depth at the most) but for a step's pool (W90) and a tributary's grade to
      // its trunk's bed (W90).
      CHECK(d >= .9 * thalweg - 2e-3);
      const f64 grade = link.trunk != ~0u ? top - n.links[link.trunk].bed[0] : 0.0;
      CHECK(d <= std::max(2 * thalweg + static_cast<f64>(link.step_depth[i]), grade) + 2.2 * mean +
                     2e-3);
      if (i == 0 && 4 * mean / (m * w) < 1)
        CHECK(d * (w - d / m) == doctest::Approx(w * mean).epsilon(0.01));
      // The water's surface stands in the cut: over the bed, under the top of the bank.
      if (i >= surface.size()) continue;
      f64 y = 0;
      REQUIRE(surface[i][1].get_f64(y));
      CHECK(y >= bed + .25 * thalweg - 2e-3);  // at least half the stage's depth (0.5)
      (void)asym;
      CHECK(y < top);
    }
  }
  CHECK(blocks >= 1);
  // Written twice, the same text.
  CHECK(write_json(terrain_cuts_json(p, n)) == write_json(cuts));
}

// **The bed is the ground** (W93, continued; channels.md, "What it places"): no bed slab and no bar
// skin on any reach. Over valleys of every alluvial reach and the steep ones, every placed piece
// is boulders (the notch falls 5 %, a step-pool reach); a pool-riffle and a plane-bed valley, whose
// bed and bars W86 to W93 laid as slabs,
// place nothing at all; and no slab of any piece is wider than a clast (a bed slab was the bed's
// width across and tens of metres long).
namespace {
f64 plane_bed(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  return .022 * zz + .2 * std::abs(xx - 128);
}
}  // namespace
TEST_CASE("channels: nothing is laid on the bed but clasts: no slab or skin on any reach") {
  // W97: the bed carries its sediment's clasts now, on every reach whose sediment reaches the drawn
  // size, but still nothing that is a slab: every piece is clasts, none wider than a boulder, and
  // the steps' and clusters' boulders only where there are steps and clusters.
  const ChannelLayer p = layer();
  std::string error;
  struct Valley {
    scene_gen::HeightFn fn;
    bool boulders;
  };
  for (const Valley& v : {Valley{&gentle, false}, Valley{&notch, true}, Valley{&plane_bed, false},
                          Valley{&step_pool, true}, Valley{&steep, true}}) {
    ChannelLayer q = p;
    q.head_threshold_m2 = 300;
    ChannelNetwork n;
    REQUIRE(plan_channels(q, ground(v.fn), 3, nullptr, n, error));
    REQUIRE_FALSE(n.links.empty());
    Vector<PlacedPiece> pieces;
    ClastLog log;
    REQUIRE(place_pieces(q, n, ground(v.fn), 32, pieces, error, &log));
    u64 boulders = 0;
    for (const Clast& c : log.clasts)
      boulders += c.zone >= 4 ? 1u : 0u;
    CHECK((boulders > 0) == v.boulders);
    for (const PlacedPiece& piece : pieces) {
      CHECK(piece.recipe.part == ChannelPart::Clasts);
      for (u32 k = 0; k < clast_count(piece.recipe); ++k)
        CHECK(clast_of(piece.recipe, k).b_mm <= 1000);  // a 2 m b axis at the most
    }
  }
}

// ---- W90: reaches, steps, mouths, confluences, the planform -------------------------------------

namespace {
// A steep notch: 12 % down its floor at x = 128 m, a cascade by Montgomery & Buffington's bounds.
f64 steep(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  return .12 * zz + .4 * std::abs(xx - 128);
}
// A 35-degree face (0.70 down the floor), the owner's mountainside.
f64 face(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  return .70 * zz + 1.2 * std::abs(xx - 128);
}
// Two valleys meeting: the trunk's floor at x = 128 m, a tributary's coming in at 45 degrees from
// the east and joining it near z = 200 m; both fall 5 % toward z = 0.
f64 forked(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 trunk = std::abs(xx - 128);
  const f64 trib = zz > 200 ? std::abs(xx - (128 + (zz - 200))) / 1.4142135623730951 : 1e9;
  return .05 * zz + .15 * std::min(trunk, trib);
}
scene::WaterBody lake(f64 level, f64 z0, f64 z1) {
  scene::WaterBody b;
  b.name = "lake";
  b.level_m = level;
  for (const auto& [x, z] : {std::pair<f64, f64>{-10, z0}, {300, z0}, {300, z1}, {-10, z1}})
    b.shape.polygon.push_back(WorldPos{x, 0, z});
  return b;
}
}  // namespace

TEST_CASE("channels: reaches by the pinned bounds (Montgomery & Buffington 1997)") {
  const ChannelLayer p = layer();
  // The defaults are the bounds the grounding page pins (f32, so each is read as the layer holds
  // it).
  CHECK(p.slope_pool_riffle == .001f);
  CHECK(p.slope_plane_bed == .015f);
  CHECK(p.slope_step_pool == .03f);
  CHECK(p.slope_cascade == .065f);
  CHECK(p.slope_steep == .2f);
  auto at = [](f32 v) { return static_cast<f64>(v); };
  CHECK(reach_of(p, .0005, false) == ChannelReach::DuneRipple);
  CHECK(reach_of(p, at(p.slope_pool_riffle), false) == ChannelReach::PoolRiffle);
  CHECK(reach_of(p, .0149, false) == ChannelReach::PoolRiffle);
  CHECK(reach_of(p, at(p.slope_plane_bed), false) == ChannelReach::PlaneBed);
  CHECK(reach_of(p, at(p.slope_step_pool), false) == ChannelReach::StepPool);
  CHECK(reach_of(p, at(p.slope_cascade), false) == ChannelReach::Cascade);
  CHECK(reach_of(p, at(p.slope_steep), false) == ChannelReach::Bedrock);
  CHECK(reach_of(p, .19, true) == ChannelReach::Cascade);
  CHECK(reach_of(p, .7, true) == ChannelReach::Colluvial);
  // Down the gentle valley (0.6 %) every point is pool-riffle or plane-bed or flatter; down the
  // 12 % notch the middle of the line, a window from either end, is a cascade.
  ChannelLayer q = layer();
  q.head_threshold_m2 = 300;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(q, ground(gentle), 0, nullptr, n, error));
  for (const ChannelLink& link : n.links) {
    REQUIRE(link.reach.size() == link.line.size());
    for (u8 r : link.reach)
      CHECK(r <= static_cast<u8>(ChannelReach::PlaneBed));
  }
  REQUIRE(plan_channels(p, ground(steep), 0, nullptr, n, error));
  REQUIRE(n.links.size() == 1);
  const ChannelLink& link = n.links[0];
  CHECK(link.reach[link.reach.size() / 2] == static_cast<u8>(ChannelReach::Cascade));
}

namespace {
// A step-pool notch: 4.5 % down its floor at x = 128 m (Montgomery & Buffington's 0.03-0.065).
f64 step_pool(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  return .045 * zz + .3 * std::abs(xx - 128);
}
Vector<f64> lengths(const ChannelLink& link) {
  Vector<f64> s(link.line.size(), 0.0);
  for (u32 i = 1; i < link.line.size(); ++i)
    s[i] = s[i - 1] + std::hypot(link.line[i].at.x - link.line[i - 1].at.x,
                                 link.line[i].at.z - link.line[i - 1].at.z);
  return s;
}
}  // namespace

// W90's steps, kept on step-pool reaches alone since W91 (they were taken on the 12 % cascade,
// which since W91 carries clusters instead: the next test).
TEST_CASE("channels: a step-pool's steps by their spacing and height, boulders on them") {
  const ChannelLayer p = layer();
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(step_pool), 0, nullptr, n, error));
  REQUIRE(n.links.size() == 1);
  const ChannelLink& link = n.links[0];
  CHECK(link.reach[link.reach.size() / 2] == static_cast<u8>(ChannelReach::StepPool));
  REQUIRE(link.step_at.size() >= 4);
  REQUIRE(link.step_height.size() == link.step_at.size());
  const Vector<f64> s = lengths(link);
  for (u32 q = 0; q < link.step_at.size(); ++q)
    CHECK(link.reach[link.step_at[q]] == static_cast<u8>(ChannelReach::StepPool));
  for (u32 q = 0; q + 1 < link.step_at.size(); ++q) {
    const u32 i = link.step_at[q], j = link.step_at[q + 1];
    const f64 w = static_cast<f64>(link.line[i].width_m);
    const f64 spacing = s[j] - s[i];
    // 1 to 4 widths (the step-pool literature), to the line's step either way.
    CHECK(spacing >= w - static_cast<f64>(p.step_m) - 1e-9);
    CHECK(spacing <= 4 * w + static_cast<f64>(p.step_m) + 1e-9);
    // H / L / S between 1 and 2 (Abrahams et al. 1995), at the layer's 1.5, over the line's 4.5 %.
    const f64 ratio = static_cast<f64>(link.step_height[q]) / spacing / .045;
    CHECK(ratio > 1.0);
    CHECK(ratio < 2.0);
  }
  // The pools lower the bed, never raise it; a step is at most its height deep. The riser is a
  // slope at the boulders' angle of repose, never a wall the water falls over (W91): no stretch
  // of the bed falls at 45 degrees or more.
  u32 falls = 0;
  for (u32 i = 0; i < link.line.size(); ++i) {
    CHECK(link.step_depth[i] >= 0.0f);
    CHECK(link.step_depth[i] <= 4.0f);
    if (i + 1 < link.line.size()) falls += link.bed[i] - link.bed[i + 1] >= s[i + 1] - s[i];
  }
  CHECK(falls == 0);
  // The steps' boulders (W97: clasts of the reach's D84 to D97.7, in its tiles' pieces).
  Vector<PlacedPiece> pieces;
  ClastLog log;
  REQUIRE(place_pieces(p, n, ground(step_pool), 32, pieces, error, &log));
  u32 boulders = 0;
  for (const Clast& c : log.clasts)
    if (c.zone == 4) {
      ++boulders;
      CHECK(static_cast<f64>(2 * c.b_mm) >=
            .99e3 * std::min(2.0, static_cast<f64>(link.sediment[c.point].d84_m)));
    }
  CHECK(boulders >= link.step_at.size());
}

// **A cascade** (W91; Montgomery & Buffington 1997: disorganised, no regular steps): the 12 %
// notch gets no steps, boulder clusters at irregular spacings, no bars and no pool lowering.
TEST_CASE("channels: a cascade gets boulder clusters and no regular steps") {
  const ChannelLayer p = layer();
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(steep), 0, nullptr, n, error));
  REQUIRE(n.links.size() == 1);
  const ChannelLink& link = n.links[0];
  CHECK(link.step_at.empty());
  for (u32 i = 0; i < link.line.size(); ++i)
    CHECK(link.step_depth[i] == 0.0f);
  REQUIRE(link.cluster_at.size() >= 6);
  REQUIRE(link.cluster_size.size() == link.cluster_at.size());
  const Vector<f64> s = lengths(link);
  f64 sum = 0, sum2 = 0;
  for (u32 q = 0; q < link.cluster_at.size(); ++q) {
    CHECK(link.reach[link.cluster_at[q]] == static_cast<u8>(ChannelReach::Cascade));
    CHECK(link.cluster_size[q] >= .25f);
    CHECK(link.cluster_size[q] <= 1.5f);
    if (q == 0) continue;
    const f64 gap = s[link.cluster_at[q]] - s[link.cluster_at[q - 1]];
    CHECK(gap >= .5 * static_cast<f64>(link.line[link.cluster_at[q]].width_m) -
                     static_cast<f64>(p.step_m));
    sum += gap;
    sum2 += gap * gap;
  }
  // No rhythm: the spacings spread (an exponential's coefficient of variation is 1; a regular
  // sequence's is near 0).
  const f64 k = static_cast<f64>(link.cluster_at.size() - 1);
  const f64 mean = sum / k, spread = std::sqrt(std::max(0.0, sum2 / k - mean * mean));
  CHECK(spread / mean > .3);
  Vector<PlacedPiece> pieces;
  ClastLog log;
  REQUIRE(place_pieces(p, n, ground(steep), 32, pieces, error, &log));
  u32 others = 0, boulders = 0;
  for (const PlacedPiece& piece : pieces)
    others += piece.recipe.part != ChannelPart::Clasts;
  for (const Clast& c : log.clasts) {
    boulders += c.zone == 5 ? 1u : 0u;
    CHECK(c.zone != 4);  // no step's keystone on a cascade
  }
  CHECK(others == 0);
  CHECK(boulders >= link.cluster_at.size());
}

TEST_CASE("channels: a 35-degree face is colluvial, cut and nothing laid") {
  ChannelLayer p = layer();
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(face), 0, nullptr, n, error));
  REQUIRE_FALSE(n.links.empty());
  u32 colluvial = 0, points = 0;
  for (const ChannelLink& link : n.links)
    for (u32 i = 0; i < link.reach.size(); ++i) {
      ++points;
      colluvial += link.reach[i] == static_cast<u8>(ChannelReach::Colluvial);
    }
  CHECK(colluvial == points);
  // W97: a colluvial channel's bed is the slope's own debris (Montgomery & Buffington 1997: bed
  // material "variable"), so it carries clasts, angular ones (a few hundred metres of transport at
  // the most), and no step's or cluster's boulders.
  Vector<PlacedPiece> pieces;
  ClastLog log;
  REQUIRE(place_pieces(p, n, ground(face), 32, pieces, error, &log));
  for (const Clast& c : log.clasts) {
    CHECK(c.zone < 4);
    CHECK(c.roundness <= .5);
  }
  // The cut is still written: a gully is cut into its slope.
  CHECK(terrain_cuts_json(p, n).as_array().size() == n.links.size());
}

TEST_CASE("channels: a link ends at a lake's shoreline, its fan grading to the level") {
  const ChannelLayer p = layer();
  std::string error;
  // The notch falls 5 % toward z = 0; a lake at 5 m stands over z < 100 m.
  const Vector<scene::WaterBody> bodies{lake(5.0, -10, 100)};
  scene_gen::Exclusion water;
  REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
  ChannelNetwork dry, wet;
  REQUIRE(plan_channels(p, ground(notch), 0, nullptr, dry, error));
  REQUIRE(plan_channels(p, ground(notch), 0, nullptr, wet, error, &water));
  REQUIRE(wet.links.size() == 1);
  const ChannelLink& link = wet.links[0];
  CHECK(link.mouth);
  CHECK(wet.water.mouths == 1);
  CHECK(wet.water.points_removed > 100);
  // The line stops at the shoreline: its last point's ground at the level, and no point below it.
  // The shoreline is the level's contour, which crosses the notch's line at z = 100 m and lies 3 m
  // of z nearer for every metre off it (0.15 across over 0.05 along), so a line within its wander
  // of the notch's (W93: 0.75 m at the most here) ends within 2.25 m of z = 100.
  const f64 shore_z = 100 - 3 * static_cast<f64>(p.steep_wander_widths);
  CHECK(link.line.back().at.y == doctest::Approx(5.0).epsilon(2e-4));
  CHECK(link.line.back().at.z <= 100 + 1e-3);
  CHECK(link.line.back().at.z >= shore_z - 1e-3);
  CHECK(link.mouth_level == doctest::Approx(5.0).epsilon(1e-3));
  for (const ChannelPoint& pt : link.line)
    CHECK(pt.at.y >= 5.0 - 1e-3);
  // The fan: the bed at the shore half a thalweg under the level, and the cut wider there.
  const JsonValue cuts = terrain_cuts_json(p, wet);
  const JsonValue blocks = water_channels_json(p, wet, 0, .5f, true);
  REQUIRE(cuts.as_array().size() == 1);
  REQUIRE(blocks.as_array().size() == 1);
  const auto& widths = cuts[0].find("width_m")->as_array();
  f64 w_end = 0, w_up = 0;
  REQUIRE(widths[widths.size() - 1].get_f64(w_end));
  REQUIRE(widths[widths.size() / 2].get_f64(w_up));
  CHECK(w_end > 1.9 * static_cast<f64>(link.line.back().width_m));
  // Above the fan the cut is the hydraulic geometry's width but for the reach's own swell (W93:
  // the notch is a step-pool reach, narrower at a step's crest and wider over its pool).
  CHECK(std::fabs(w_up - static_cast<f64>(link.line[widths.size() / 2].width_m) *
                             (1 + static_cast<f64>(link.swell[widths.size() / 2]))) <= 1e-3);
  const ChannelPoint& end = link.line.back();
  const f64 thalweg = 2 * static_cast<f64>(end.depth_m) / 2;  // a bound, below
  CHECK(link.bed.back() < 5.0);
  CHECK(link.bed.back() > 5.0 - 2 * thalweg - 1e-3);
  // The water meets the level at the shore, and stands nowhere below it.
  const auto& points = blocks[0].find("points")->as_array();
  f64 y = 0;
  REQUIRE(points[points.size() - 1][1].get_f64(y));
  CHECK(y == doctest::Approx(5.0).epsilon(1e-3));
  for (const JsonValue& at : points) {
    f64 z = 0;
    REQUIRE(at[2].get_f64(z));
    CHECK(z >= shore_z - 1e-3);
  }
  // Pieces: none under the lake.
  // W97: no clast stands in the lake: every one's centre is on the shore's side of it, within a
  // clast of the line's last point.
  Vector<PlacedPiece> pieces;
  ClastLog log;
  REQUIRE(place_pieces(p, wet, ground(notch), 32, pieces, error, &log));
  REQUIRE_FALSE(pieces.empty());
  for (const Clast& c : log.clasts)
    CHECK(static_cast<f64>(c.z_mm - c.a_mm) / 1000.0 >= 100 - 2.0);
  // A lake over the whole valley: the link begins under it and is dropped.
  const Vector<scene::WaterBody> drowned{lake(1000.0, -10, 600)};
  scene_gen::Exclusion all;
  REQUIRE(all.build({}, {drowned.data(), drowned.size()}, {}, &error));
  ChannelNetwork none;
  REQUIRE(plan_channels(p, ground(notch), 0, nullptr, none, error, &all));
  CHECK(none.water.dropped == 1);
  CHECK(water_channels_json(p, none, 0, .5f, true).as_array().empty());
  CHECK(terrain_cuts_json(p, none).as_array().empty());
  // No bodies, an empty exclusion: the network is the one planned with none, byte for byte.
  scene_gen::Exclusion empty;
  REQUIRE(empty.build({}, {}, {}, &error));
  ChannelNetwork same;
  REQUIRE(plan_channels(p, ground(notch), 0, nullptr, same, error, &empty));
  CHECK(same_network(same, dry));
  CHECK(write_json(terrain_cuts_json(p, same)) == write_json(terrain_cuts_json(p, dry)));
}

TEST_CASE("channels: a tributary meets its trunk at the trunk's surface, without overlap") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(forked), 0, nullptr, n, error));
  CHECK(n.water.confluences >= 1);
  const JsonValue blocks = water_channels_json(p, n, 0, .5f, true);
  // The blocks by link name.
  auto block_of = [&](u32 k) -> const JsonValue* {
    const std::string name = p.name + " link " + std::to_string(k);
    for (const JsonValue& b : blocks.as_array())
      if (b.find("name")->as_string() == name) return &b;
    return nullptr;
  };
  u32 tributaries = 0;
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& trib = n.links[k];
    if (trib.trunk == ~0u) continue;
    ++tributaries;
    const ChannelLink& down = n.links[trib.trunk];
    const WorldPos at = down.line[0].at;
    // It ends at the junction, the trunk's bed is reached from above, never raised to.
    CHECK(trib.line.back().at.x == at.x);
    CHECK(trib.line.back().at.z == at.z);
    CHECK(trib.bed.back() <= down.bed[0] + 1e-9);
    CHECK(trib.keep < trib.line.size());
    const JsonValue* tb = block_of(k);
    const JsonValue* db = block_of(trib.trunk);
    if (tb == nullptr || db == nullptr) continue;  // inside the trunk's banks whole
    const auto& points = tb->find("points")->as_array();
    CHECK(points.size() == trib.keep);
    // No point of its water within the trunk's banks at the junction.
    for (const JsonValue& q : points) {
      f64 x = 0, z = 0;
      REQUIRE(q[0].get_f64(x));
      REQUIRE(q[2].get_f64(z));
      CHECK(std::hypot(x - at.x, z - at.z) >= .5 * static_cast<f64>(down.line[0].width_m) - 1e-6);
    }
    // Its last surface is lowered toward the trunk's at the junction and never raised: no higher
    // than the tributary's own water there, and no lower than the lower of the two.
    ChannelNetwork alone = n;
    alone.links[k].trunk = ~0u;
    const JsonValue own_blocks = water_channels_json(p, alone, 0, .5f, true);
    f64 own = 0;
    for (const JsonValue& b : own_blocks.as_array())
      if (b.find("name")->as_string() == p.name + " link " + std::to_string(k))
        REQUIRE((*b.find("points"))[points.size() - 1][1].get_f64(own));
    f64 last = 0, trunk_y = 0;
    REQUIRE(points[points.size() - 1][1].get_f64(last));
    REQUIRE((*db->find("points"))[0][1].get_f64(trunk_y));
    CHECK(last <= own + 1e-9);
    CHECK(last >= std::min(own, trunk_y) - 1e-3);
    if (own > trunk_y) CHECK(std::fabs(last - trunk_y) <= 2e-3);  // meets it at the banks
    MESSAGE("tributary " << k << ": its own surface " << own << " m, graded " << last
                         << " m, the trunk's at the junction " << trunk_y << " m");
  }
  CHECK(tributaries >= 1);
  // Written twice, one text.
  CHECK(write_json(water_channels_json(p, n, 0, .5f, true)) == write_json(blocks));
}

// **Bedrock and its knickpoint** (W91; channels.md, "Reaches"): two valleys at 40 % meeting (the
// link below the junction bedrock, the heads' links colluvial), a 3 m scarp across the region at
// z = 100 m. No step anywhere; the bed falls at 45 degrees or more only at the scarp, which is the
// network's one knickpoint below the junction; elsewhere the cut follows the ground as a chute.
namespace {
f64 scarp(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 trunk = std::abs(xx - 128);
  const f64 trib = zz > 200 ? std::abs(xx - (128 + (zz - 200))) / 1.4142135623730951 : 1e9;
  const f64 cliff = zz >= 101 ? 3.0 : (zz <= 100 ? 0.0 : 3.0 * (zz - 100));
  return .4 * zz + 1.0 * std::min(trunk, trib) + cliff;
}
}  // namespace
TEST_CASE("channels: a bedrock chute falls only at its knickpoint") {
  const ChannelLayer p = layer();
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(scarp), 0, nullptr, n, error));
  const ChannelLink* trunk = nullptr;
  for (const ChannelLink& link : n.links) {
    CHECK(link.step_at.empty());
    if (!link.head && link.line.size() >= 2 && link.line[0].at.z > 150 &&
        link.line.back().at.z < 50 && std::abs(link.line.back().at.x - 128) < 8)
      trunk = &link;
  }
  REQUIRE(trunk != nullptr);
  const ChannelLink& link = *trunk;
  CHECK(link.reach[link.reach.size() / 4] == static_cast<u8>(ChannelReach::Bedrock));
  CHECK(link.knickpoints == 1);
  CHECK(link.steep_left == 0);
  const Vector<f64> s = lengths(link);
  u32 falls = 0;
  for (u32 i = 0; i + 1 < link.line.size(); ++i) {
    if (link.bed[i] - link.bed[i + 1] < s[i + 1] - s[i]) continue;
    ++falls;
    // At the scarp and the knickpoint's window below it, nowhere else.
    CHECK_MESSAGE(std::abs(link.line[i].at.z - 100.5) < 16, "a fall at z " << link.line[i].at.z);
    CHECK(link.knick[i] == 1);
  }
  CHECK(falls >= 1);
  // The water the flowing-water capability draws: falls only there too, and a single run of them.
  const JsonValue water = water_channels_json(p, n, 0, .5f, true);
  for (const JsonValue& block : water.as_array()) {
    if (block.find("name")->as_string() !=
        p.name + " link " + std::to_string(&link - n.links.data()))
      continue;
    JsonValue one = JsonValue::array();
    one.push_back(block);
    const WaterStats stats = measure_water(one, ground(scarp));
    CHECK(stats.falls == 1);
    CHECK(stats.fall_stretches_by_reach[static_cast<u32>(ChannelReach::Bedrock)] ==
          stats.fall_stretches);
  }
}

// **The line held in the trough, the ribbon on the ground** (W91; channels.md, "Invariants"): a
// trough running 20 degrees off the lattice's axes down a plane that tilts across it, so the
// lattice's 45-degree steps and the low-pass both carry a line off its floor. Every point of the
// longest line stays within two lattice spacings of the trough's axis, and with the ground the
// ribbon is fitted to, no edge stands more than three centimetres over the ground beside it.
namespace {
constexpr f64 k_sin20 = .3420201433256687, k_cos20 = .9396926207859084;
f64 askew(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 across = (xx - 128) * k_cos20 - (zz - 256) * k_sin20;
  return .05 * zz + .08 * xx + .15 * std::abs(across);
}
}  // namespace
TEST_CASE("channels: a line across a tilted plane stays in its trough, the ribbon on the ground") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(askew), 0, nullptr, n, error));
  const ChannelLink* longest = nullptr;
  for (const ChannelLink& link : n.links)
    if (longest == nullptr || link.length_m > longest->length_m) longest = &link;
  REQUIRE(longest != nullptr);
  CHECK(longest->length_m > 200);
  for (const ChannelPoint& pt : longest->line) {
    const f64 across = (pt.at.x - 128) * k_cos20 - (pt.at.z - 256) * k_sin20;
    CHECK_MESSAGE(std::abs(across) <= 2 * static_cast<f64>(p.spacing_m),
                  "a point " << across << " m off the trough at z " << pt.at.z);
  }
  const scene_gen::Ground g = ground(askew);
  const JsonValue fitted = water_channels_json(p, n, 0, .5f, true, &g);
  const WaterStats stats = measure_water(fitted, g);
  REQUIRE(stats.edges > 0);
  CHECK(stats.edges_above_005 == 0);
  CHECK(stats.edge_max_m <= .03 + 1e-6);
  // Each block carries its fit, one code a point, and the fit only narrows.
  const JsonValue plain = water_channels_json(p, n, 0, .5f, true);
  REQUIRE(fitted.as_array().size() == plain.as_array().size());
  for (u32 k = 0; k < fitted.as_array().size(); ++k) {
    const JsonValue& b = fitted[k];
    const auto& fit = b.find("fit")->as_array();
    REQUIRE(fit.size() == b.find("points")->as_array().size());
    for (u32 i = 0; i < fit.size(); ++i) {
      f64 w = 0, w0 = 0;
      REQUIRE(b.find("width_m")->as_array()[i].get_f64(w));
      REQUIRE(plain[k].find("width_m")->as_array()[i].get_f64(w0));
      CHECK(w <= w0 + 1e-9);
    }
  }
  // The same bytes twice, and from a network planned on six jobs.
  jobs::JobSystemConfig config;
  config.performance_workers = 6;
  config.efficiency_workers = 0;
  config.pin_threads = false;
  jobs::JobSystem js(config);
  ChannelNetwork parallel;
  REQUIRE(plan_channels(p, ground(askew), 0, &js, parallel, error));
  CHECK(same_network(n, parallel));
  CHECK(write_json(water_channels_json(p, parallel, 0, .5f, true, &g)) == write_json(fitted));
  CHECK(write_json(terrain_cuts_json(p, parallel)) == write_json(terrain_cuts_json(p, n)));
  ChannelNetwork cascade_a, cascade_b;
  REQUIRE(plan_channels(p, ground(steep), 0, nullptr, cascade_a, error));
  REQUIRE(plan_channels(p, ground(steep), 0, &js, cascade_b, error));
  REQUIRE(cascade_a.links.size() == cascade_b.links.size());
  for (u32 k = 0; k < cascade_a.links.size(); ++k) {
    CHECK(cascade_a.links[k].cluster_at == cascade_b.links[k].cluster_at);
    CHECK(cascade_a.links[k].cluster_size == cascade_b.links[k].cluster_size);
    CHECK(cascade_a.links[k].knick == cascade_b.links[k].knick);
  }
}

// **A fan down a steep face** (W91 continuation): v36's link 690 fell at 0.72 m/m into the sea, and
// the fan's grade, anchored at its first point with that point's own share already applied, dropped
// its surface under the cut's bed for five points (depth 0, which the water capability refuses).
// The grade starts at the first fan point itself and never runs under the bed: the 35-degree face
// running into a lake writes a positive depth at every point, by either path the subcommand takes.
TEST_CASE("channels: a fan down a 35-degree face keeps water over its bed to the shore") {
  const ChannelLayer p = layer();
  std::string error;
  const Vector<scene::WaterBody> bodies{lake(70.0, -10, 100)};
  scene_gen::Exclusion water;
  REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
  ChannelNetwork n;
  REQUIRE(plan_channels(p, ground(face), 0, nullptr, n, error, &water));
  u32 mouths = 0;
  for (const ChannelLink& link : n.links)
    mouths += link.mouth ? 1u : 0u;
  REQUIRE(mouths >= 1);
  const scene_gen::Ground g = ground(face);
  for (const JsonValue& blocks : {water_channels_json(p, n, 0, .5f, true),
                                  water_channels_json(p, n, 0, .5f, true, &g, &water)}) {
    REQUIRE_FALSE(blocks.as_array().empty());
    u32 depths = 0, dry = 0;
    for (const JsonValue& b : blocks.as_array())
      for (const JsonValue& d : b.find("depth_m")->as_array()) {
        f64 v = 0;
        REQUIRE(d.get_f64(v));
        ++depths;
        dry += v > 0 ? 0u : 1u;
      }
    CHECK(depths > 0);
    CHECK_MESSAGE(dry == 0, dry << " of " << depths << " depths not positive");
    CHECK(measure_water(blocks, g).depths_not_positive == 0);
  }
}

// **The ribbon's end a lake covers** (W91 continuation; item 25 of the POC's list): a ribbon whose
// last points lie inside a lake's outline with their surface under its level is under the lake's
// own water, and loses exactly those points; the points inside the outline that stand over the
// level stay, and everything upstream is written as it was.
TEST_CASE("channels: a ribbon's end under a lake inside its outline is trimmed, and only that") {
  const ChannelLayer p = layer();
  std::string error;
  ChannelNetwork n;
  // Planned with no water, so the line runs on through the outline to the region's edge.
  REQUIRE(plan_channels(p, ground(notch), 0, nullptr, n, error));
  REQUIRE(n.links.size() == 1);
  // The ground meets 4 m at z = 80; the cut's water lies under 4 m a little further up.
  const f64 level = 4.0;
  const Vector<scene::WaterBody> bodies{lake(level, -10, 100)};
  scene_gen::Exclusion water;
  REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
  const JsonValue plain = water_channels_json(p, n, 0, .5f, true);
  RibbonTrim trim;
  const JsonValue trimmed = water_channels_json(p, n, 0, .5f, true, nullptr, &water, &trim);
  REQUIRE(plain.as_array().size() == 1);
  REQUIRE(trimmed.as_array().size() == 1);
  const auto& all = plain[0].find("points")->as_array();
  const auto& kept = trimmed[0].find("points")->as_array();
  auto yz = [](const JsonValue& at, f64& y, f64& z) {
    REQUIRE(at[1].get_f64(y));
    REQUIRE(at[2].get_f64(z));
  };
  // The covered run at the end, as the plain description has it.
  u32 covered = 0;
  for (u32 i = all.size(); i-- > 0;) {
    f64 y = 0, z = 0;
    yz(all[i], y, z);
    if (!(z < 100 && y < level - 1e-3)) break;
    ++covered;
  }
  REQUIRE(covered > 0);
  CHECK(kept.size() == all.size() - covered);
  CHECK(trim.points == covered);
  CHECK(trim.blocks == 1);
  CHECK(trim.dropped == 0);
  CHECK(trim.mouth_fans == 0);
  CHECK(trim.at.size() == covered);
  for (u32 i = 0; i < kept.size() && i < all.size(); ++i)
    CHECK(write_json(kept[i]) == write_json(all[i]));
  u32 inside = 0;
  for (const JsonValue& at : kept) {
    f64 y = 0, z = 0;
    yz(at, y, z);
    if (z >= 100) continue;
    ++inside;
    CHECK(y >= level - 1e-3);
  }
  // Points inside the outline whose water stands over the level are the stream's, and stay.
  CHECK(inside > 0);
}

TEST_CASE("channels: no two bends alike") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  ChannelNetwork varied, regular, again;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 7, nullptr, varied, error));
  REQUIRE(plan_channels(p, ground(gentle), 7, nullptr, again, error));
  CHECK(same_network(varied, again));
  p.meander_wavelength_variation = 0;
  p.meander_amplitude_variation = 0;
  REQUIRE(plan_channels(p, ground(gentle), 7, nullptr, regular, error));
  CHECK(varied.heads == regular.heads);
  CHECK_FALSE(same_network(varied, regular));
}

// ---- W93: the section follows the line ----------------------------------------------------------

namespace {
// A stream wide enough to have bars on the gentle valley: 12 m and 1 m at a square kilometre.
ChannelLayer stream_layer(u64 seed) {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  p.width_at_km2_m = 12;
  p.depth_at_km2_m = 1.0f;
  p.seed = seed;
  return p;
}
// The cut's depth below its top at the signed distance `s` from the line (left positive), from the
// cut's own numbers: the test's reading of `engine.scene.TerrainCut` version 2.
f64 cut_depth_at(const scene::TerrainEdit& cut, u32 i, f64 s) {
  const f64 w = static_cast<f64>(cut.width_m[i]), d = static_cast<f64>(cut.depth_m[i]);
  f64 l = static_cast<f64>(cut.bank_slope_left[i]), r = static_cast<f64>(cut.bank_slope_right[i]);
  const f64 f = (d / l + d / r) / w;
  if (f > 1) {
    l *= f;
    r *= f;
  }
  return std::max(0.0, std::min(d, std::min(l * (.5 * w - s), r * (.5 * w + s))));
}
// The sine of the line's turn about point i at the scale a bar is built at: between its heading
// two widths above the point and its heading two widths below, each heading the chord of a width
// of line. Positive where it turns toward its left ((-dz, dx)); 0 near an end.
f64 turn_at(const ChannelLink& link, const std::vector<f64>& s, u32 i) {
  const f64 w = static_cast<f64>(link.line[i].width_m);
  const u32 m = link.line.size();
  // The first point at least `d` above (negative) or below the point along the line, or m.
  const auto at = [&](f64 d) {
    u32 q = i;
    if (d < 0) {
      while (q > 0 && s[i] - s[q] < -d)
        --q;
      return s[i] - s[q] < -d ? m : q;
    }
    while (q + 1 < m && s[q] - s[i] < d)
      ++q;
    return s[q] - s[i] < d ? m : q;
  };
  const u32 a0 = at(-2.5 * w), a1 = at(-1.5 * w), b0 = at(1.5 * w), b1 = at(2.5 * w);
  if (a0 == m || a1 == m || b0 == m || b1 == m) return 0;
  const f64 ux = link.line[a1].at.x - link.line[a0].at.x;
  const f64 uz = link.line[a1].at.z - link.line[a0].at.z;
  const f64 vx = link.line[b1].at.x - link.line[b0].at.x;
  const f64 vz = link.line[b1].at.z - link.line[b0].at.z;
  return (ux * vz - uz * vx) / (std::sqrt(ux * ux + uz * uz) * std::sqrt(vx * vx + vz * vz));
}
std::vector<f64> along(const ChannelLink& link) {
  std::vector<f64> s(link.line.size(), 0.0);
  for (u32 i = 1; i < link.line.size(); ++i) {
    const f64 dx = link.line[i].at.x - link.line[i - 1].at.x;
    const f64 dz = link.line[i].at.z - link.line[i - 1].at.z;
    s[i] = s[i - 1] + std::sqrt(dx * dx + dz * dz);
  }
  return s;
}
}  // namespace

// The brief's first two tests: over twelve seeds, at every bend's apex the outer bank is the deeper
// side (95 % at least), and the pools stand 5 to 7 widths apart on average (Keller & Melhorn 1978).
TEST_CASE(
    "channels: at a bend the outer bank is the deeper side, and pools stand 5 to 7 widths apart") {
  u32 bends = 0, outer_deeper = 0, pools = 0;
  f64 spacing_sum = 0, spacing_min = 1e9, spacing_max = 0;
  u32 spacings = 0;
  for (u64 seed = 1; seed <= 12; ++seed) {
    const ChannelLayer p = stream_layer(seed);
    ChannelNetwork n;
    std::string error;
    REQUIRE(plan_channels(p, ground(gentle), seed, nullptr, n, error));
    const JsonValue cuts = terrain_cuts_json(p, n);
    u32 written = 0;
    for (u32 k = 0; k < n.links.size(); ++k) {
      const ChannelLink& link = n.links[k];
      if (link.line.size() < 2) continue;
      scene::TerrainEdit cut;
      schema::ReadContext reader;
      REQUIRE(schema::from_json(cut, cuts[written++], reader));
      REQUIRE(reader.ok());
      REQUIRE(cut.bank_slope_left.size() == link.line.size());
      REQUIRE(link.asym.size() == link.line.size());
      const std::vector<f64> s = along(link);
      // A bend: the line turns at least 30 degrees between two widths above the point and two
      // below (`turn_at`: the scale a bar is built at, so a kink a width long is not one), taken
      // at its apex, the point where it turns most.
      for (u32 i = 1; i + 1 < link.line.size(); ++i) {
        const f64 turn = turn_at(link, s, i);
        if (std::fabs(turn) < .5 || link.reach[i] != static_cast<u8>(ChannelReach::PoolRiffle))
          continue;
        if (std::fabs(turn_at(link, s, i - 1)) > std::fabs(turn) ||
            std::fabs(turn_at(link, s, i + 1)) >= std::fabs(turn))
          continue;
        if (static_cast<f64>(link.line[i].width_m) < static_cast<f64>(p.bar_min_width_m)) continue;
        ++bends;
        const f64 q = .3 * static_cast<f64>(cut.width_m[i]);
        // Turning left, the outer bank is the right.
        const f64 outer = cut_depth_at(cut, i, turn > 0 ? -q : q);
        const f64 inner = cut_depth_at(cut, i, turn > 0 ? q : -q);
        outer_deeper += outer > inner + .02;
      }
      // Pools: the apexes of the asymmetry, at least half a bend's; the spacing of each from the
      // last in the mean width between them.
      // A pool is a stretch of the line on one side of which the thalweg lies, whose asymmetry
      // reaches half a bend's, at the point where it is most.
      f64 last = -1;
      f64 width_sum = 0;
      u32 width_count = 0;
      for (u32 i = 0; i < link.line.size();) {
        const f64 a = static_cast<f64>(link.asym[i]);
        if (a == 0) {
          width_sum += static_cast<f64>(link.line[i].width_m);
          ++width_count;
          ++i;
          continue;
        }
        // One side's stretch, from where the thalweg crosses the line to where it crosses back.
        u32 e = i, apex = i;
        while (e + 1 < link.line.size() && static_cast<f64>(link.asym[e + 1]) * a > 0) {
          ++e;
          if (std::fabs(link.asym[e]) > std::fabs(link.asym[apex])) apex = e;
        }
        if (std::fabs(static_cast<f64>(link.asym[apex])) < .5) {
          // A glide: the thalweg swings to this side and no pool forms.
          for (u32 q = i; q <= e; ++q) {
            width_sum += static_cast<f64>(link.line[q].width_m);
            ++width_count;
          }
          i = e + 1;
          continue;
        }
        // A run of the top value: its middle.
        u32 flat = apex;
        while (flat < e && link.asym[flat + 1] == link.asym[apex])
          ++flat;
        apex = (apex + flat) / 2;
        for (u32 q = i; q <= e; ++q) {
          width_sum += static_cast<f64>(link.line[q].width_m);
          ++width_count;
        }
        ++pools;
        if (last >= 0) {
          const f64 widths = (s[apex] - last) / (width_sum / width_count);
          spacing_sum += widths;
          spacing_min = std::min(spacing_min, widths);
          spacing_max = std::max(spacing_max, widths);
          ++spacings;
        }
        last = s[apex];
        width_sum = 0;
        width_count = 0;
        i = e + 1;
      }
    }
  }
  MESSAGE("bends " << bends << ", outer deeper at " << outer_deeper << "; pools " << pools
                   << ", spacing mean " << (spacings ? spacing_sum / spacings : 0.0) << " widths ("
                   << spacing_min << " to " << spacing_max << ", " << spacings << " spacings)");
  REQUIRE(bends >= 40);
  CHECK(outer_deeper * 100 >= bends * 95);
  REQUIRE(spacings >= 40);
  CHECK(spacing_sum / spacings >= 5.0);
  CHECK(spacing_sum / spacings <= 7.0);
}

TEST_CASE("channels: the section keeps the hydraulic geometry's area from riffle to pool") {
  const ChannelLayer p = stream_layer(3);
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 3, nullptr, n, error));
  const JsonValue cuts = terrain_cuts_json(p, n);
  const JsonValue water = water_channels_json(p, n, 0, .5f, true);
  u32 written = 0, riffles = 0, apexes = 0, measured = 0;
  f64 widest = 0, narrowest = 9;
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& link = n.links[k];
    if (link.line.size() < 2) continue;
    const JsonValue& o = cuts[written++];
    // W89's five fields, the two slope arrays and the rounding; and a flood bed's five on a link
    // that has one.
    CHECK(o.as_object().size() == (o.find("flood_left_m") == nullptr ? 8u : 13u));
    scene::TerrainEdit cut;
    schema::ReadContext reader;
    REQUIRE(schema::from_json(cut, o, reader));
    REQUIRE(reader.ok());
    CHECK(cut.bank_round == p.bank_round);
    const JsonValue* block = nullptr;
    for (const JsonValue& b : water.as_array())
      if (b.find("name")->as_string() == p.name + " link " + std::to_string(k)) block = &b;
    for (u32 i = 0; i < link.line.size(); ++i) {
      const ChannelPoint& pt = link.line[i];
      const f64 w = static_cast<f64>(pt.width_m), mean = static_cast<f64>(pt.depth_m);
      const f64 a = std::fabs(static_cast<f64>(link.asym[i]));
      const f64 l = static_cast<f64>(cut.bank_slope_left[i]);
      const f64 r = static_cast<f64>(cut.bank_slope_right[i]);
      CHECK(l >= .02);
      CHECK(r >= .02);
      CHECK(l <= 16);
      CHECK(r <= 16);
      // The riffle is wider than the pool by `riffle_widening`, half each way about the hydraulic
      // geometry's width (no fan on this ground), where the reach has the rhythm (`swell`).
      const f64 widening = static_cast<f64>(link.swell[i]);
      CHECK(static_cast<f64>(cut.width_m[i]) == doctest::Approx(w * (1 + widening)).epsilon(1e-3));
      widest = std::max(widest, static_cast<f64>(cut.width_m[i]) / w);
      narrowest = std::min(narrowest, static_cast<f64>(cut.width_m[i]) / w);
      riffles += a < .1;
      if (a == 0) {
        CHECK(l == static_cast<f64>(p.bank_slope));
        CHECK(r == static_cast<f64>(p.bank_slope));
      } else {
        // The gentler bank is the bar's, on the inside: the left where the asymmetry is positive.
        CHECK((link.asym[i] > 0 ? l <= r : r <= l));
      }
      // Where the bed runs through no rise, no step and no grade, the section's area is the
      // hydraulic geometry's, whatever its shape: integrated across the cut's own numbers.
      const f64 top = cut.points[i].y, bed = link.bed[i];
      const f64 cw = static_cast<f64>(cut.width_m[i]);
      f64 area = 0;
      const u32 strips = 400;
      for (u32 q = 0; q < strips; ++q)
        area +=
            cut_depth_at(cut, i, (-.5 + (static_cast<f64>(q) + .5) / strips) * cw) * cw / strips;
      const f64 m = static_cast<f64>(p.bank_slope);
      const f64 thalweg =
          4 * mean / (m * w) >= 1 ? 2 * mean : .5 * m * w * (1 - std::sqrt(1 - 4 * mean / (m * w)));
      if (std::fabs((top - bed) - thalweg) <= 2e-3 && link.trunk == ~0u) {
        CHECK(area == doctest::Approx(w * mean).epsilon(0.04));
        ++measured;
        if (a >= .95) {
          // A bend's apex: a triangle, twice the mean depth at the outer bank's toe.
          CHECK(static_cast<f64>(cut.depth_m[i]) ==
                doctest::Approx(2 * mean * w / cw).epsilon(0.03));
          ++apexes;
        }
      }
      // The water stands over the riffle's bed and under the top, as it did; the pool is under it.
      if (block == nullptr || i >= block->find("points")->as_array().size()) continue;
      f64 y = 0;
      REQUIRE(block->find("points")->as_array()[i][1].get_f64(y));
      CHECK(y < top);
      CHECK(y > top - static_cast<f64>(cut.depth_m[i]));
    }
  }
  CHECK(riffles > 20);
  CHECK(measured > 150);
  CHECK(apexes >= 1);
  // A riffle at its widest over a pool at its narrowest: half `riffle_widening` (0.12) each way.
  const f64 half = .5 * static_cast<f64>(p.riffle_widening);
  CHECK(widest / narrowest > 1.08);
  CHECK(widest / narrowest <= (1 + half) / (1 - half) + 1e-3);
  // The same text twice.
  CHECK(write_json(terrain_cuts_json(p, n)) == write_json(cuts));
}

// ---- W93: the flood bed
// ---------------------------------------------------------------------------

namespace {
// A valley whose floor is `*half` metres wide each side of x = 128 m, falling 0.6 % down z and 1 %
// across to its middle, between walls at 45 degrees.
f64 floored(const void* context, i64 x, i64 z) noexcept {
  const f64 half = *static_cast<const f64*>(context);
  const f64 d = std::abs(static_cast<f64>(x) / 1000.0 - 128);
  return .006 * static_cast<f64>(z) / 1000.0 + .01 * d + std::max(0.0, d - half);
}
}  // namespace

// The brief's fourth test: the flood bed's width follows the confinement measured.
TEST_CASE("channels: the flood bed's width follows the valley's confinement") {
  struct Case {
    f64 half;
    const char* what;
  };
  for (const Case& c : {Case{16, "wide"}, Case{4.5, "narrow"}, Case{1, "entrenched"}}) {
    ChannelLayer p = stream_layer(1);
    p.meander_amplitude_coeff = 0;  // the line down the valley's middle, so each side is the same
    p.alternate_asymmetry = 0;
    scene_gen::Ground g;
    g.surface_fn = &floored;
    g.context = &c.half;
    ChannelNetwork n;
    std::string error;
    REQUIRE(plan_channels(p, g, 1, nullptr, n, error));
    const JsonValue cuts = terrain_cuts_json(p, n);
    const JsonValue water = water_channels_json(p, n, 0, .5f, true);
    u32 written = 0, checked = 0, bare = 0;
    for (u32 k = 0; k < n.links.size(); ++k) {
      const ChannelLink& link = n.links[k];
      if (link.line.size() < 2) continue;
      const JsonValue& o = cuts[written++];
      if (link.line.size() < 200 || !link.head) continue;  // the trunk
      scene::TerrainEdit cut;
      schema::ReadContext reader;
      REQUIRE(schema::from_json(cut, o, reader));
      REQUIRE(reader.ok());
      const std::vector<f64> s = along(link);
      for (u32 i = 0; i < link.line.size(); ++i) {
        const ChannelPoint& pt = link.line[i];
        const f64 w = static_cast<f64>(pt.width_m), mean = static_cast<f64>(pt.depth_m);
        // The middle of the link, where the bed is wide enough to have a flood bed and the line
        // is on the valley's middle.
        if (s[i] < .3 * s.back() || s[i] > .7 * s.back() || w < 2.5) continue;
        if (std::abs(pt.at.x - 128) > .25) continue;
        const f64 thalweg = .5 * w * (1 - std::sqrt(1 - 4 * mean / w));
        // Each side: the floor's half width and the wall's run to a thalweg over the line, the
        // floor's own 1 % taken off.
        const f64 prone = c.half + (thalweg - .01 * c.half) / 1.01;
        const f64 ratio = 2 * prone / w;
        const f64 run = 1.25 * thalweg / 1.5;
        ++checked;
        if (ratio < 1.4) {
          CHECK(link.flood_left[i] == 0);
          CHECK(link.flood_right[i] == 0);
          CHECK(cut.flood_left_m.empty());
          continue;
        }
        // (The walk stops at the furthest a bed can reach on a side and its scarp's run, 1.6 times
        // half the three widths: a ratio past that is recorded as that.)
        const f64 walked = 2 * (1.6 * 1.5 * w + run) / w;
        CHECK(static_cast<f64>(link.entrenchment[i]) ==
              doctest::Approx(std::min(ratio, walked)).epsilon(0.03));
        // As wide as the flood-prone width allows, three widths at the most.
        const f64 expected = std::min(3 * w, 2 * (prone - run));
        const f64 total =
            static_cast<f64>(link.flood_left[i]) + static_cast<f64>(link.flood_right[i]);
        CHECK(total == doctest::Approx(expected).epsilon(0.03));
        REQUIRE(cut.flood_left_m.size() == link.line.size());
        CHECK(cut.flood_left_m[i] == link.flood_left[i]);
        CHECK(cut.flood_right_m[i] == link.flood_right[i]);
        // The bed a quarter of a thalweg under the top, its scarp's top a thalweg over it.
        CHECK(static_cast<f64>(cut.flood_depth_m[i]) ==
              doctest::Approx(.25 * thalweg).epsilon(0.01));
        CHECK(static_cast<f64>(cut.scarp_m[i]) == doctest::Approx(1.25 * thalweg).epsilon(0.01));
        CHECK(cut.scarp_slope == p.scarp_slope);
        bare += total > 1.5 * w;
      }
    }
    // **The same bytes from every toolchain** (W93): the cuts' text over this ground, which calls
    // no C library function, hashes to a value taken on MSVC (2026-11-10) that GCC and Clang
    // reproduce in the containers: the section, the flood bed and the line's move decide in
    // products, sums, divisions, square roots and `core/math/deterministic.h`. (Re-pinned by
    // W93's continuation, which moved every line: the kinks low-passed out, the pool a width
    // downstream of its bend's apex, `bend_radius_widths` 3.)
    {
      const std::string text = write_json(cuts);
      const u64 hash = hash_bytes(text.data(), text.size());
      CHECK(hash == (c.half >= 16 ? 2670861390155137291ull
                     : c.half > 1 ? 6798986846940614891ull
                                  : 12194280523489796769ull));
    }
    MESSAGE(std::string(c.what) << ": " << checked << " points checked, " << bare
                                << " with a bed over 1.5 widths");
    CHECK(checked > 100);
    if (c.half >= 16) CHECK(bare == checked);
    if (c.half <= 1) CHECK(bare == 0);
    // The placements leave the bed bare: a point 0.9 of the bed's reach from the ribbon's line is
    // excluded, on either side, and the water's description is what says so.
    Vector<scene::WaterChannel> channels;
    for (const JsonValue& block : water.as_array()) {
      scene::WaterChannel channel;
      schema::ReadContext reader;
      REQUIRE(schema::from_json(channel, block, reader));
      REQUIRE(reader.ok());
      channels.push_back(std::move(channel));
    }
    scene_gen::Exclusion exclusion;
    REQUIRE(exclusion.build({}, {}, {channels.data(), channels.size()}, &error));
    const scene_gen::Ground flat;
    u32 on_bed = 0;
    for (const scene::WaterChannel& channel : channels) {
      if (channel.flood_left_m.empty()) continue;
      REQUIRE(channel.flood_left_m.size() == channel.points.size());
      for (u32 i = 0; i + 1 < channel.points.size(); i += 7) {
        const f64 dx = channel.points[i + 1].x - channel.points[i].x;
        const f64 dz = channel.points[i + 1].z - channel.points[i].z;
        const f64 len = std::sqrt(dx * dx + dz * dz);
        if (!(len > 0.0)) continue;
        for (const f64 side : {1.0, -1.0}) {
          const f64 reach =
              static_cast<f64>(side > 0 ? channel.flood_left_m[i] : channel.flood_right_m[i]);
          if (!(reach > 0)) continue;
          const f64 x = channel.points[i].x - side * dz / len * .9 * reach;
          const f64 z = channel.points[i].z + side * dx / len * .9 * reach;
          CHECK(exclusion.excluded_mm(flat, std::llround(x * 1000.0), std::llround(z * 1000.0),
                                      scene_gen::k_exclude_channels));
          ++on_bed;
        }
      }
    }
    if (c.half >= 16) CHECK(on_bed > 20);
  }
}

// ---- W93: a line across a slope
// ---------------------------------------------------------------------

namespace {
// A V whose floor swings 18 m either way every 80 m down a 1.2 % fall, between sides at 0.35: the
// low-pass cuts its bends' corners, so the line runs across the valley's side there.
f64 zigzag(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  return .012 * zz + .35 * std::abs(xx - (128 + 18 * std::sin(zz * 6.283185307179586 / 80)));
}
}  // namespace

// Stage 3's invariant: neither bank's ground (half the width across the line's heading) stands more
// than half a depth under the ground at the line, so the cut has a bank on both sides.
TEST_CASE("channels: a line across a slope is moved between its banks") {
  const ChannelLayer p = stream_layer(1);
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(zigzag), 1, nullptr, n, error));
  u32 points = 0, half_bank = 0, no_bank = 0, moved = 0;
  f64 worst = 0;
  for (const ChannelLink& link : n.links) {
    moved += link.bank_moved;
    for (u32 i = 1; i + 1 < link.line.size(); ++i) {
      const ChannelPoint& pt = link.line[i];
      const f64 w = static_cast<f64>(pt.width_m), d = static_cast<f64>(pt.depth_m);
      if (w < 1.0) continue;
      const f64 tx = link.line[i + 1].at.x - link.line[i - 1].at.x;
      const f64 tz = link.line[i + 1].at.z - link.line[i - 1].at.z;
      const f64 tl = std::sqrt(tx * tx + tz * tz);
      if (!(tl > 0)) continue;
      const auto at = [&](f64 side) {
        return zigzag(nullptr, std::llround((pt.at.x - tz / tl * side * .5 * w) * 1000.0),
                      std::llround((pt.at.z + tx / tl * side * .5 * w) * 1000.0));
      };
      const f64 under = pt.at.y - std::min(at(1), at(-1));
      ++points;
      half_bank += under > .5 * d;
      no_bank += under > 1.13 * d;  // the trapezoid's thalweg: the bank's ground under the bed
      worst = std::max(worst, under / d);
    }
  }
  MESSAGE("points " << points << ", a bank more than half a depth under the line at " << half_bank
                    << ", under the bed at " << no_bank << ", the worst " << worst
                    << " depths; moved " << moved);
  // Measured (2026-11-10): 1,969 points, 486 moved; a bank more than half a depth under the line
  // at 32 of them (100 after one round of the move) and under the bed at 12, the worst 1.23
  // depths. Held: under 2 % and under 1 %.
  REQUIRE(points > 500);
  CHECK(moved > 0);
  CHECK(half_bank * 100 <= points * 2);
  CHECK(no_bank * 100 <= points);
}

// ---- W93, continued: the steep reaches and the line's kinks -------------------------------------

namespace {
// A plane falling 35 % toward z = 0 with the faintest fold down x = 128 m, under a bowl that
// gathers the flow above z = 300 m: the routing's line down the plane is a ruled line, which is
// what a side gully on a valley's planar side was.
f64 hillside(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 bowl = zz > 300 ? .3 * std::min(1.0, (zz - 300) / 50) : 0.0;
  return .35 * zz + (.01 + bowl) * std::abs(xx - 128);
}
// The longest stretch of a polyline, metres, whose points all lie within `tolerance` of the chord
// between its two ends: how long a ruled segment the line holds.
f64 longest_straight(const std::vector<f64>& x, const std::vector<f64>& z, f64 tolerance) {
  f64 longest = 0;
  const usize n = x.size();
  for (usize a = 0; a + 1 < n; ++a) {
    usize b = a + 1;
    for (; b < n; ++b) {
      const f64 ex = x[b] - x[a], ez = z[b] - z[a], len = std::sqrt(ex * ex + ez * ez);
      bool straight = true;
      for (usize q = a + 1; q < b && straight && len > 0; ++q)
        straight = std::fabs(ex * (z[q] - z[a]) - ez * (x[q] - x[a])) / len <= tolerance;
      if (!straight) break;
    }
    const f64 ex = x[b - 1] - x[a], ez = z[b - 1] - z[a];
    longest = std::max(longest, std::sqrt(ex * ex + ez * ez));
  }
  return longest;
}
}  // namespace

// **The steep reaches** (channels.md, "The steep reaches"): on a step-pool reach the cut's width
// follows the steps, narrower at each crest and wider over each pool by 15 % of the hydraulic
// geometry's either way, and its thalweg wanders between the banks; a gully down a planar slope
// is no ruled line: neither bank's top holds a stretch longer than 20 m within 5 cm of a chord.
TEST_CASE("channels: a steep reach's width follows its steps and its banks are no ruled line") {
  // Channels a few metres wide (the default's are under the metre a section is shaped from).
  ChannelLayer p = layer();
  p.width_at_km2_m = 12;
  p.depth_at_km2_m = 1.2f;
  std::string error;
  {
    ChannelNetwork n;
    REQUIRE(plan_channels(p, ground(step_pool), 0, nullptr, n, error));
    REQUIRE(n.links.size() == 1);
    const ChannelLink& link = n.links[0];
    const Vector<f64> s = lengths(link);
    const JsonValue cuts = terrain_cuts_json(p, n);
    scene::TerrainEdit cut;
    schema::ReadContext reader;
    REQUIRE(schema::from_json(cut, cuts[0], reader));
    REQUIRE(cut.width_m.size() == link.line.size());
    f64 widest = 0, narrowest = 9, most_left = 0, most_right = 0;
    for (u32 i = 0; i < link.line.size(); ++i) {
      const f64 w = static_cast<f64>(link.line[i].width_m);
      if (s[i] < 3 * w || s.back() - s[i] < 3 * w) continue;
      if (link.reach[i] != static_cast<u8>(ChannelReach::StepPool) || link.knick[i]) continue;
      const f64 ratio = static_cast<f64>(cut.width_m[i]) / w;
      widest = std::max(widest, ratio);
      narrowest = std::min(narrowest, ratio);
      most_left = std::max(most_left, static_cast<f64>(link.asym[i]));
      most_right = std::min(most_right, static_cast<f64>(link.asym[i]));
    }
    MESSAGE("step-pool: the cut " << narrowest << " to " << widest
                                  << " of the hydraulic geometry's width, the asymmetry "
                                  << most_right << " to " << most_left);
    CHECK(widest >= 1.12);
    CHECK(widest <= 1.15 + 1e-3);
    CHECK(narrowest <= .88);
    CHECK(narrowest >= .85 - 1e-3);
    CHECK(widest / narrowest >= 1.27);  // stated: the width varies by a quarter and more
    CHECK(most_left >= .15);
    CHECK(most_right <= -.15);
    CHECK(most_left <= static_cast<f64>(p.steep_asymmetry) + 1e-3);
    // Narrower than the hydraulic geometry's at every crest away from the ends, wider between.
    u32 crests = 0, pools = 0;
    for (u32 q = 0; q + 1 < link.step_at.size(); ++q) {
      const u32 a = link.step_at[q], b = link.step_at[q + 1], mid = (a + b) / 2;
      const f64 w = static_cast<f64>(link.line[a].width_m);
      if (s[a] < 3 * w || s.back() - s[b] < 3 * w) continue;
      ++crests;
      CHECK(static_cast<f64>(cut.width_m[a]) < w);
      if (link.reach[mid] != static_cast<u8>(ChannelReach::StepPool)) continue;
      ++pools;
      CHECK(static_cast<f64>(cut.width_m[mid]) > static_cast<f64>(link.line[mid].width_m));
    }
    CHECK(crests >= 4);
    CHECK(pools >= 4);
  }
  {
    ChannelLayer q = p;
    q.head_threshold_m2 = 300;
    ChannelNetwork n;
    REQUIRE(plan_channels(q, ground(hillside), 0, nullptr, n, error));
    REQUIRE_FALSE(n.links.empty());
    const JsonValue cuts = terrain_cuts_json(q, n);
    f64 longest = 0, length = 0, most_sinuous = 1;
    for (u32 k = 0; k < n.links.size(); ++k) {
      const ChannelLink& link = n.links[k];
      if (link.line.size() < 40) continue;
      scene::TerrainEdit cut;
      schema::ReadContext reader;
      REQUIRE(schema::from_json(cut, cuts[k], reader));
      const Vector<f64> s = lengths(link);
      std::vector<f64> lx, lz, rx, rz;
      // The bank tops: half the cut's width across the line each way, away from the two ends.
      u32 first = ~0u, last = 0;
      for (u32 i = 1; i + 1 < link.line.size(); ++i) {
        if (s[i] < 8 || s.back() - s[i] < 8) continue;
        const f64 tx = link.line[i + 1].at.x - link.line[i - 1].at.x;
        const f64 tz = link.line[i + 1].at.z - link.line[i - 1].at.z;
        const f64 tl = std::sqrt(tx * tx + tz * tz);
        const f64 half = .5 * static_cast<f64>(cut.width_m[i]);
        lx.push_back(link.line[i].at.x - tz / tl * half);
        lz.push_back(link.line[i].at.z + tx / tl * half);
        rx.push_back(link.line[i].at.x + tz / tl * half);
        rz.push_back(link.line[i].at.z - tx / tl * half);
        first = std::min(first, i);
        last = i;
      }
      if (lx.size() < 20) continue;
      longest = std::max({longest, longest_straight(lx, lz, .05), longest_straight(rx, rz, .05)});
      const f64 dx = link.line[last].at.x - link.line[first].at.x;
      const f64 dz = link.line[last].at.z - link.line[first].at.z;
      most_sinuous = std::max(most_sinuous, (s[last] - s[first]) / std::sqrt(dx * dx + dz * dz));
      length += s[last] - s[first];
    }
    MESSAGE("hillside: " << length << " m of gully, the longest ruled stretch of a bank " << longest
                         << " m, the greatest sinuosity " << most_sinuous);
    REQUIRE(length > 100);
    CHECK(longest <= 20.0);  // 17.0 m measured; the whole 440 m before the wander
    CHECK(most_sinuous > 1.002);
    CHECK(most_sinuous <= 1.2);  // Rosgen's type A: 1.0 to 1.2
  }
}

// **No kinks** (channels.md, "The planform"): after the low-pass at the channel's own scale the
// line turns, between consecutive points, no more than its step over a radius of `k_radius` of
// the width (a metre's at least) — on every reach of every valley these tests plan.
TEST_CASE("channels: the line's turn between consecutive points is bounded on every reach") {
  struct Valley {
    const char* name;
    scene_gen::HeightFn fn;
    f32 threshold;
  };
  std::string error;
  f64 worst = 0;
  for (const Valley& v : {Valley{"gentle", &gentle, 300}, Valley{"notch", &notch, 1500},
                          Valley{"steep", &steep, 1500}, Valley{"step-pool", &step_pool, 1500},
                          Valley{"forked", &forked, 200}, Valley{"face", &face, 1500},
                          Valley{"hillside", &hillside, 300}}) {
    ChannelLayer p = layer();
    p.head_threshold_m2 = v.threshold;
    f64 here = 0, sharpest_turn = 0;
    u64 points = 0;
    for (u64 seed = 0; seed < 4; ++seed) {
      ChannelNetwork n;
      REQUIRE(plan_channels(p, ground(v.fn), seed, nullptr, n, error));
      for (const ChannelLink& link : n.links) {
        for (u32 i = 1; i + 1 < link.line.size(); ++i) {
          const f64 ax = link.line[i].at.x - link.line[i - 1].at.x;
          const f64 az = link.line[i].at.z - link.line[i - 1].at.z;
          const f64 bx = link.line[i + 1].at.x - link.line[i].at.x;
          const f64 bz = link.line[i + 1].at.z - link.line[i].at.z;
          const f64 la = std::sqrt(ax * ax + az * az), lb = std::sqrt(bx * bx + bz * bz);
          if (!(la > 0) || !(lb > 0)) continue;
          // The turn's sine over the mean step is the curvature; times the width, widths a radius.
          const f64 sine = std::fabs(ax * bz - az * bx) / (la * lb);
          const f64 w = std::max(1.0, static_cast<f64>(link.line[i].width_m));
          here = std::max(here, sine / (.5 * (la + lb)) * w);
          sharpest_turn = std::max(sharpest_turn, sine);
          ++points;
        }
      }
    }
    MESSAGE(std::string(v.name) << ": " << points << " points, the tightest radius " << 1 / here
                                << " widths, the sharpest turn's sine " << sharpest_turn);
    worst = std::max(worst, here);
  }
  CHECK(worst <= 1 / .5);  // no radius under half a width (of a metre at least)
}
// **The bed's material** (channels.md, "The flood bed"): the beds' outlines as the terrain's mask
// regions. None for a layer that names no material; with one, every region is a
// `TerrainMaskRegion` naming it, every point of a line a metre wide or more lies inside one, with
// the points two fifths of the bed's reach to either side, and the ground a bed's reach and 3 m
// beyond its edge lies outside them all (but where another bend's bed comes round to it).
TEST_CASE("channels: the beds' outlines are mask regions that hold the cut and its flood bed") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  p.width_at_km2_m = 12;
  p.depth_at_km2_m = 1.2f;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 0, nullptr, n, error));
  CHECK(bed_regions_json(p, n).as_array().empty());
  p.bed_material = "gravel";
  const JsonValue list = bed_regions_json(p, n);
  REQUIRE_FALSE(list.as_array().empty());
  CHECK(write_json(bed_regions_json(p, n)) == write_json(list));
  std::vector<scene::TerrainMaskRegion> regions;
  for (const JsonValue& o : list.as_array()) {
    scene::TerrainMaskRegion region;
    schema::ReadContext reader;
    REQUIRE(schema::from_json(region, o, reader));
    REQUIRE(reader.ok());
    CHECK(region.material == "gravel");
    CHECK(region.feather_m == doctest::Approx(.4f));
    CHECK(region.polygon.size() >= 4);
    CHECK(region.name.find(p.name + " link ") == 0);
    regions.push_back(std::move(region));
  }
  // Even-odd, over every region.
  const auto inside = [&](f64 x, f64 z) {
    for (const scene::TerrainMaskRegion& r : regions) {
      bool in = false;
      for (u32 a = 0, b = r.polygon.size() - 1; a < r.polygon.size(); b = a++) {
        const WorldPos& u = r.polygon[a];
        const WorldPos& v = r.polygon[b];
        if ((u.z > z) != (v.z > z) && x < (v.x - u.x) * (z - u.z) / (v.z - u.z) + u.x) in = !in;
      }
      if (in) return true;
    }
    return false;
  };
  u32 on_bed = 0, beyond = 0, beyond_out = 0, flooded = 0;
  for (const ChannelLink& link : n.links) {
    const Vector<f64> s = lengths(link);
    const u32 m = std::min<u32>(link.keep, link.line.size());
    for (u32 i = 2; i + 2 < m; i += 3) {
      const ChannelPoint& pt = link.line[i];
      const f64 w = static_cast<f64>(pt.width_m);
      if (w < static_cast<f64>(p.bar_min_width_m) || link.knick[i]) continue;
      if (s[i] < 3 * w || s[m - 1] - s[i] < 3 * w) continue;
      const f64 tx = link.line[i + 1].at.x - link.line[i - 1].at.x;
      const f64 tz = link.line[i + 1].at.z - link.line[i - 1].at.z;
      const f64 tl = std::sqrt(tx * tx + tz * tz);
      const f64 half = .5 * w * (1 + static_cast<f64>(link.swell[i]));
      const f64 left = std::max(half, static_cast<f64>(link.flood_left[i]));
      const f64 right = std::max(half, static_cast<f64>(link.flood_right[i]));
      flooded += link.flood_left[i] > 0;
      ++on_bed;
      CHECK(inside(pt.at.x, pt.at.z));
      CHECK(inside(pt.at.x - tz / tl * .4 * left, pt.at.z + tx / tl * .4 * left));
      CHECK(inside(pt.at.x + tz / tl * .4 * right, pt.at.z - tx / tl * .4 * right));
      beyond += 2;
      beyond_out += !inside(pt.at.x - tz / tl * (left + 3), pt.at.z + tx / tl * (left + 3));
      beyond_out += !inside(pt.at.x + tz / tl * (right + 3), pt.at.z - tx / tl * (right + 3));
    }
  }
  MESSAGE(regions.size() << " regions; " << on_bed << " points on a bed, " << flooded
                         << " of them with a flood bed; " << beyond_out << " of " << beyond
                         << " probes 3 m past the edge outside every region");
  CHECK(on_bed > 100);
  CHECK(flooded > 20);
  CHECK(beyond_out * 100 >= beyond * 90);
}
// ---- W93, continued: one plan -------------------------------------------------------------------

namespace {
// The step-pool valley with every channel's corridor lowered by 0.4 m: a ground the plan's own cuts
// have changed, which is what the generator stands on in a cut scene.
f64 step_pool_cut(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  (void)zz;
  return step_pool(nullptr, x, z) - (std::abs(xx - 128) < 6 ? .4 : 0.0);
}
// What the three readers of a plan make of it: the cuts' text, the water's, and the exclusion built
// from the water.
struct Readers {
  std::string cuts, water;
  Vector<scene::WaterChannel> channels;
  scene_gen::Exclusion exclusion;
  void read(const ChannelLayer& p, const ChannelPlan& plan) {
    cuts = write_json(terrain_cuts_json(p, plan.network));
    const JsonValue blocks = water_channels_json(p, plan.network, 0, .5f, true);
    water = write_json(blocks);
    for (const JsonValue& block : blocks.as_array()) {
      scene::WaterChannel channel;
      schema::ReadContext reader;
      REQUIRE(schema::from_json(channel, block, reader));
      REQUIRE(reader.ok());
      channels.push_back(std::move(channel));
    }
    std::string error;
    REQUIRE(exclusion.build({}, {}, {channels.data(), channels.size()}, &error));
  }
  // How many points of a 2 m lattice over the region the channels' corridor excludes.
  u32 excluded() const {
    const scene_gen::Ground flat;
    u32 count = 0;
    for (i64 z = 0; z <= 512000; z += 2000)
      for (i64 x = 0; x <= 256000; x += 2000)
        count += exclusion.excluded_mm(flat, x, z, scene_gen::k_exclude_channels) ? 1u : 0u;
    return count;
  }
};
}  // namespace

// **One plan** (channels.md, "One plan"): the cut, the water's description, the exclusion's
// corridor and the generator's placements are read from one object, kept as a file keyed by what it
// was made from. Planned once; a second ask and a load that names it plan nothing; a parameter
// moves all three readers together; and the generator, on ground the cuts lowered, places what the
// plan says and never plans again.
TEST_CASE("channels: one plan is read by the cut, the water, the exclusion and the generator") {
  test::TempDir scratch;
  const std::string dir = scratch.path();
  std::string plans_dir, error;
  REQUIRE_MESSAGE(derived_dir(dir, dir, plans_dir, error), error);
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground(step_pool);
  c.tile_size = 64;
  c.world_seed = 7;
  ChannelLayer p = layer();

  // Made once, and written.
  const u64 before = plans_made();
  ChannelPlan first;
  REQUIRE_MESSAGE(ensure_plan(p, c, plans_dir, first, error), error);
  CHECK(first.planned);
  CHECK(plans_made() == before + 1);
  CHECK(io::exists(plan_path(plans_dir, first.key)));
  CHECK(first.layer_hash == plan_layer_hash(p));
  // Asked again: read, not made, and the same links to the byte (the file is the plan).
  ChannelPlan again;
  REQUIRE_MESSAGE(ensure_plan(p, c, plans_dir, again, error), error);
  CHECK_FALSE(again.planned);
  CHECK(plans_made() == before + 1);
  CHECK(again.key == first.key);
  CHECK(again.network.ground_um.empty());  // the lattice is not what a reader of a plan asks
  ChannelPlan bare = first;                // the same without the lattice: what the file holds
  bare.network.ground_um = {};
  bare.network.filled_um = {};
  bare.network.receiver = {};
  bare.network.area_cells = {};
  bare.network.channel = {};
  CHECK(plan_bytes(again) == plan_bytes(bare));
  Readers a, a_again;
  a.read(p, first);
  a_again.read(p, again);
  CHECK(a.cuts == a_again.cuts);
  CHECK(a.water == a_again.water);
  REQUIRE(a.excluded() > 100);
  CHECK(a.excluded() == a_again.excluded());

  // A network with a confluence, whose water orders its junctions by contributing area: the plan
  // read from its file writes the same water as the plan just made (the file holds no lattice).
  {
    scene_gen::Context f = c;
    f.ground = ground(forked);
    ChannelLayer q = layer();
    q.head_threshold_m2 = 200;
    ChannelPlan made, cached;
    REQUIRE_MESSAGE(ensure_plan(q, f, plans_dir, made, error), error);
    REQUIRE_MESSAGE(ensure_plan(q, f, plans_dir, cached, error), error);
    CHECK(made.planned);
    CHECK_FALSE(cached.planned);
    u32 tributaries = 0;
    for (const ChannelLink& link : made.network.links)
      tributaries += link.trunk != ~0u ? 1u : 0u;
    CHECK(tributaries > 0);
    Readers fresh, from_file;
    fresh.read(q, made);
    from_file.read(q, cached);
    CHECK(fresh.water == from_file.water);
    CHECK(fresh.cuts == from_file.cuts);
  }
  const u64 made_so_far = plans_made();

  // A parameter of the layer moves the key and all three readers.
  ChannelLayer wider = p;
  wider.width_at_km2_m = 6.0f;
  ChannelPlan second;
  REQUIRE_MESSAGE(ensure_plan(wider, c, plans_dir, second, error), error);
  CHECK(second.planned);
  CHECK(plans_made() == made_so_far + 1);
  CHECK(second.key != first.key);
  Readers b;
  b.read(wider, second);
  CHECK(b.cuts != a.cuts);
  CHECK(b.water != a.water);
  CHECK(b.excluded() > a.excluded());
  // And so does the world's seed, and the ground.
  scene_gen::Context other = c;
  other.world_seed = 8;
  ChannelPlan third;
  REQUIRE_MESSAGE(ensure_plan(p, other, plans_dir, third, error), error);
  CHECK(third.key != first.key);
  other = c;
  other.ground = ground(step_pool_cut);
  REQUIRE_MESSAGE(ensure_plan(p, other, plans_dir, third, error), error);
  CHECK(third.key != first.key);

  // **A load on the cut ground plans nothing**: the entry names its plan, the ground is the one the
  // cuts lowered, and what is placed stands at the plan's own stations.
  const u64 loads = plans_made();
  ChannelLayer named = p;
  named.plan = plan_key_text(first.key);
  scene_gen::Context cut = c;
  cut.ground = ground(step_pool_cut);
  ChannelPlan read;
  REQUIRE_MESSAGE(ensure_plan(named, cut, plans_dir, read, error), error);
  CHECK_FALSE(read.planned);
  CHECK(plan_bytes(read) == plan_bytes(again));
  Opened opened;
  REQUIRE(opened.desc != nullptr);
  REQUIRE_MESSAGE(opened.desc->open(schema::to_json(named), cut, &opened.state, &error), error);
  CHECK(plans_made() == loads);
  scene_gen::Placements whole;
  REQUIRE(opened.desc->expand(opened.state, cut, whole, &error));
  Vector<PlacedPiece> pieces;
  REQUIRE(place_pieces(p, first.network, ground(step_pool_cut), 64, pieces, error));
  REQUIRE_FALSE(pieces.empty());
  REQUIRE(whole.instances.size() == pieces.size());
  for (u32 i = 0; i < pieces.size(); ++i)
    CHECK(whole.instances[i].position == pieces[i].origin);

  // The refusals: a key that is not one, a plan that is not there, and a plan made from another
  // layer or seed than the entry's.
  ChannelPlan refused;
  ChannelLayer bad = named;
  bad.plan = "not-a-key";
  CHECK_FALSE(ensure_plan(bad, cut, plans_dir, refused, error));
  CHECK(error.find("plan") != std::string::npos);
  bad.plan = "0123456789abcdef";
  CHECK_FALSE(ensure_plan(bad, cut, plans_dir, refused, error));
  CHECK(error.find("is not in") != std::string::npos);
  bad = wider;
  bad.plan = named.plan;
  CHECK_FALSE(ensure_plan(bad, cut, plans_dir, refused, error));
  CHECK(error.find("another layer") != std::string::npos);
  other = cut;
  other.world_seed = 9;
  CHECK_FALSE(ensure_plan(named, other, plans_dir, refused, error));
  CHECK(plans_made() == loads);
  // A file cut short, or changed, is refused and never read as a plan.
  std::string bytes = plan_bytes(first);
  CHECK(read_plan(bytes, refused, error));
  CHECK_FALSE(read_plan(std::string_view(bytes).substr(0, bytes.size() / 2), refused, error));
  bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 1);
  CHECK_FALSE(read_plan(bytes, refused, error));
}

// ---- W97: bed sediment -------------------------------------------------------------------------

namespace {
f64 phi(f64 x) { return .5 * std::erfc(-x / std::sqrt(2.0)); }
// The four valleys whose reaches run from pool-riffle to cascade, and the clasts each lays.
struct Laid {
  ChannelNetwork n;
  Vector<PlacedPiece> pieces;
  ClastLog log;
};
Laid lay_valley(const ChannelLayer& p, scene_gen::HeightFn fn, f32 tile = 32) {
  Laid out;
  std::string error;
  REQUIRE_MESSAGE(plan_channels(p, ground(fn), 3, nullptr, out.n, error), error);
  REQUIRE_MESSAGE(place_pieces(p, out.n, ground(fn), tile, out.pieces, error, &out.log), error);
  return out;
}
f64 vertical_half(const Clast& c) {
  const f64 B = static_cast<f64>(c.b_mm) / 1000, C = static_cast<f64>(c.c_mm) / 1000;
  const f64 l = std::sqrt(1 + c.dip * c.dip);
  const f64 by = c.dip / l, cy = 1 / l;
  return std::sqrt(B * by * B * by + C * cy * C * cy);
}
}  // namespace

// **The sediment is in the plan** (W97; channels.md, "Bed sediment"): every point of every link
// carries it; the median coarsens from pool-riffle to cascade (Montgomery & Buffington 1997's
// gravel, cobble-boulder and boulder beds); D84 and D16 are the median times and over 2^sigma; the
// d90 stays under the reach's relative roughness; roundness only grows downstream; and the plan's
// file carries it to the byte.
TEST_CASE("channels: the bed's sediment by reach, in the plan") {
  const ChannelLayer p = layer();
  f64 median[7] = {}, count[7] = {};
  for (const scene_gen::HeightFn fn : {&gentle, &plane_bed, &step_pool, &steep}) {
    ChannelNetwork n;
    std::string error;
    REQUIRE(plan_channels(p, ground(fn), 3, nullptr, n, error));
    for (const ChannelLink& link : n.links) {
      REQUIRE(link.sediment.size() == link.line.size());
      for (u32 i = 0; i < link.line.size(); ++i) {
        const ChannelSediment& s = link.sediment[i];
        const f64 d50 = static_cast<f64>(s.d50_m), sigma = static_cast<f64>(s.sorting);
        CHECK(d50 >= .0005);
        CHECK(static_cast<f64>(s.d84_m) == doctest::Approx(d50 * std::exp2(sigma)).epsilon(1e-5));
        CHECK(static_cast<f64>(s.d16_m) == doctest::Approx(d50 / std::exp2(sigma)).epsilon(1e-5));
        const u8 r = link.reach[i];
        const f64 rr = r <= 1 ? .3 : r <= 3 ? .8 : 1.0;
        CHECK(d50 * std::exp2(1.2816 * sigma) <=
              rr * static_cast<f64>(link.line[i].depth_m) * (1 + 1e-5) + 1e-9);
        CHECK(s.roundness >= .05f);
        CHECK(s.roundness <= .95f);
        if (i > 0) CHECK(s.roundness >= link.sediment[i - 1].roundness);
        CHECK(s.cover >= 0.0f);
        CHECK(s.cover <= 1.0f);
        median[r] += d50;
        count[r] += 1;
      }
    }
  }
  const auto mean = [&](ChannelReach r) {
    const u32 k = static_cast<u32>(r);
    return count[k] > 0 ? median[k] / count[k] : 0.0;
  };
  MESSAGE("mean D50 by reach, mm: pool-riffle "
          << 1e3 * mean(ChannelReach::PoolRiffle) << ", plane-bed "
          << 1e3 * mean(ChannelReach::PlaneBed) << ", step-pool "
          << 1e3 * mean(ChannelReach::StepPool) << ", cascade "
          << 1e3 * mean(ChannelReach::Cascade));
  REQUIRE(count[static_cast<u32>(ChannelReach::PoolRiffle)] > 0);
  REQUIRE(count[static_cast<u32>(ChannelReach::Cascade)] > 0);
  CHECK(mean(ChannelReach::PoolRiffle) < mean(ChannelReach::StepPool));
  CHECK(mean(ChannelReach::StepPool) < mean(ChannelReach::Cascade));
}

// **Counts by size follow the stated distribution** (W97; channels.md, "The clasts"): over four
// seeds of the step-pool valley, the bed's surface under clasts between the drawn size and D84, and
// above D84, each against what the lognormal says at each clast's own station, as a share of the
// bed's area. A pebble count samples by area (Wolman 1954), so the comparison is by area.
TEST_CASE("channels: the clasts' sizes follow the sediment's distribution over several seeds") {
  for (u64 seed = 1; seed <= 4; ++seed) {
    ChannelLayer p = layer();
    p.seed = seed;
    const Laid laid = lay_valley(p, &step_pool);
    const f64 bed = laid.log.area_m2[0];
    REQUIRE(bed > 100);
    f64 below = 0, above = 0, expect_below = 0, expect_above = 0;
    u64 clasts = 0;
    const f64 psi_min = std::log2(static_cast<f64>(p.clast_min_mm) / 1000);
    // What each station's cells expect, weighted by the share of the bed's area its clasts stand
    // on: summed over the clasts as the station's expectation divided by its expected count, so a
    // station with more bed counts for more.
    for (const Clast& c : laid.log.clasts) {
      if (c.zone != 0) continue;
      ++clasts;
      const ChannelSediment& s = laid.n.links[c.link].sediment[c.point];
      const f64 area = .25 * 3.141592653589793 * 4e-6 * static_cast<f64>(c.a_mm * c.b_mm);
      (static_cast<f64>(2 * c.b_mm) / 1000 >= static_cast<f64>(s.d84_m) ? above : below) += area;
      const f64 sigma = static_cast<f64>(s.sorting), psi50 = std::log2(static_cast<f64>(s.d50_m));
      const f64 z = (psi_min - psi50) / sigma;
      const f64 drawn = 1 - phi(z);
      if (drawn <= 0) continue;
      // This clast's share of what its station draws: the expectation of each band is the
      // station's drawn cover times the band's share of it.
      expect_above += area * (1 - phi(1.0)) / drawn;
      expect_below += area * std::max(0.0, phi(1.0) - phi(z)) / drawn;
    }
    REQUIRE(clasts > 500);
    const f64 total = below + above;
    MESSAGE("seed " << seed << ": " << clasts << " bed clasts over " << bed << " m2, cover "
                    << total / bed << "; above D84 " << above / total << " (expected "
                    << expect_above / (expect_above + expect_below) << ")");
    // The bands' shares of the drawn cover, within 0.05 of the lognormal's.
    CHECK(std::fabs(above / total - expect_above / (expect_above + expect_below)) <= .05);
  }
}

// **A bar sorts head to tail** (Rice & Church 2010): on the gentle valley's bends, with clasts
// drawn from 16 mm so a gravel bar has them, the bar's clasts at its head are coarser than at its
// tail, over four seeds.
TEST_CASE("channels: a bar's clasts fine from its head to its tail") {
  u32 seeds_coarser = 0;
  for (u64 seed = 1; seed <= 4; ++seed) {
    ChannelLayer p = layer();
    p.seed = seed;
    p.clast_min_mm = 16;
    const Laid laid = lay_valley(p, &gentle);
    std::vector<f64> head, tail;
    for (const Clast& c : laid.log.clasts) {
      if (c.zone != 1) continue;
      if (c.bar < .3f) head.push_back(static_cast<f64>(c.b_mm));
      if (c.bar > .7f) tail.push_back(static_cast<f64>(c.b_mm));
    }
    REQUIRE(head.size() > 20);
    REQUIRE(tail.size() > 20);
    // The mean, not the median: most drawn clasts are near the drawn size, so the medians of a
    // millimetre-rounded b tie.
    f64 h = 0, t = 0;
    for (const f64 b : head)
      h += b / static_cast<f64>(head.size());
    for (const f64 b : tail)
      t += b / static_cast<f64>(tail.size());
    MESSAGE("seed " << seed << ": bar head mean b " << 2 * h << " mm (" << head.size() << "), tail "
                    << 2 * t << " mm (" << tail.size() << ")");
    seeds_coarser += h > t ? 1u : 0u;
  }
  CHECK(seeds_coarser == 4);
}

// **Partly buried, none floating** (W97; channels.md, "The clasts"): every clast's lowest point is
// under the lowest ground beneath it; its top stands over the ground at its centre by no more than
// 0.9 of its height; and almost none is buried whole (a clast on a bank's foot lowered to the
// downhill ground's level may be).
TEST_CASE("channels: a clast is partly buried and none floats") {
  for (const scene_gen::HeightFn fn : {&gentle, &step_pool, &steep}) {
    const ChannelLayer p = layer();
    const Laid laid = lay_valley(p, fn);
    REQUIRE_FALSE(laid.log.clasts.empty());
    u64 whole = 0, exposed_over = 0, floating = 0;
    f64 exposure = 0;
    for (const Clast& c : laid.log.clasts) {
      const f64 hv = vertical_half(c);
      const f64 y = static_cast<f64>(c.y_mm) / 1000;
      const f64 g = ground(fn).surface_mm(c.x_mm, c.z_mm);
      floating += y - hv > static_cast<f64>(c.ground_mm) / 1000 - .0095 ? 1u : 0u;
      const f64 top = y + hv - g;
      exposed_over += top > .9 * 2 * hv + .002 ? 1u : 0u;
      whole += top <= 0 ? 1u : 0u;
      exposure += std::max(0.0, top) / (2 * static_cast<f64>(c.b_mm) / 1000);
    }
    const u64 k = laid.log.clasts.size();
    MESSAGE(k << " clasts: mean protrusion " << exposure / static_cast<f64>(k)
              << " of b; buried whole " << whole);
    CHECK(floating == 0);
    CHECK(exposed_over == 0);
    CHECK(whole * 20 <= k);
  }
}

// **Tile-stable** (W97): the clasts are the same set whatever the tile size and from whatever
// start, each in exactly one tile's piece (the one its centre stands in), one piece a tile; and
// `open` builds no mesh.
TEST_CASE("channels: the clasts are tile-stable, one merged piece a tile, and open builds none") {
  const ChannelLayer p = layer();
  const Laid a = lay_valley(p, &step_pool, 32), b = lay_valley(p, &step_pool, 64);
  REQUIRE(a.log.clasts.size() == b.log.clasts.size());
  const auto key = [](const Clast& c) {
    return std::tuple(c.link, c.point, c.seed, c.x_mm, c.y_mm, c.z_mm, c.a_mm, c.b_mm, c.c_mm);
  };
  std::vector<decltype(key(a.log.clasts[0]))> ka, kb;
  for (const Clast& c : a.log.clasts)
    ka.push_back(key(c));
  for (const Clast& c : b.log.clasts)
    kb.push_back(key(c));
  std::sort(ka.begin(), ka.end());
  std::sort(kb.begin(), kb.end());
  CHECK(ka == kb);
  for (const Laid* l : {&a, &b}) {
    std::vector<std::pair<i32, i32>> tiles;
    u64 held = 0;
    for (const PlacedPiece& piece : l->pieces) {
      tiles.push_back({piece.tile.x, piece.tile.z});
      held += clast_count(piece.recipe);
    }
    CHECK(held == l->log.clasts.size());
    std::sort(tiles.begin(), tiles.end());
    CHECK(std::adjacent_find(tiles.begin(), tiles.end()) == tiles.end());
  }
  // `open` writes the recipes and builds nothing.
  test::TempDir scratch;
  scene_gen::Context c;
  c.dir = scratch.path();
  c.ddc = scratch.path();
  c.ground = ground(step_pool);
  c.tile_size = 64;
  const u64 solved = pieces_solved();
  Opened opened;
  std::string error;
  REQUIRE_MESSAGE(opened.desc->open(schema::to_json(p), c, &opened.state, &error), error);
  CHECK(pieces_solved() == solved);
}

// **The same bytes everywhere** (W97): the recipes of the step-pool valley and the mesh of its
// first piece, pinned on MSVC; Clang and GCC reproduce them in the containers.
TEST_CASE("channels: the clasts' recipes and mesh are pinned") {
  const ChannelLayer p = layer();
  const Laid laid = lay_valley(p, &step_pool, 64);
  REQUIRE_FALSE(laid.pieces.empty());
  u64 recipes = 0;
  for (const PlacedPiece& piece : laid.pieces)
    recipes = hash_combine(recipes, piece_source_hash(piece.recipe));
  assets::MeshData mesh;
  std::string error;
  REQUIRE(solve_piece(laid.pieces[0].recipe, mesh, error));
  const u64 positions = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  const u64 indices = hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32));
  MESSAGE("recipes " << recipes << ", first piece: " << mesh.indices.size() / 3
                     << " triangles, positions " << positions << ", indices " << indices);
  // W97b re-pinned the recipes (k_channels_version 9; the surface carries no default colour) and
  // the indices (the triangles grouped by lithology); the positions are W97's to the bit.
  CHECK(recipes == 7718558231201880594ull);
  CHECK(positions == 14068701233020217500ull);
  CHECK(indices == 4152819545259226263ull);
}

// **The stones are of several rocks, as constants** (W97b, G165; channels.md, "What a stone is
// made of"): a tile's container names no image (a tile's mesh draws constants only, ADR-0061),
// holds a material a lithology and tone with a dry rock's albedo, gives every cluster one of them,
// and over two valleys the lithologies' shares of the triangles follow their shares of the stones.
TEST_CASE("channels: the stones are of four rocks in their shares, as material constants") {
  const ChannelLayer p = layer();
  f64 by_rock[4] = {}, total = 0;
  for (const scene_gen::HeightFn fn : {&step_pool, &plane_bed}) {
    const Laid laid = lay_valley(p, fn, 64);
    REQUIRE_FALSE(laid.pieces.empty());
    for (const PlacedPiece& piece : laid.pieces) {
      assets::MeshData mesh;
      std::string error;
      REQUIRE(solve_piece(piece.recipe, mesh, error));
      REQUIRE(mesh.materials.size() == 8);
      for (const assets::Primitive& prim : mesh.primitives) {
        by_rock[prim.material / 2] += prim.index_count / 3;
        total += prim.index_count / 3;
      }
    }
    geometry::ClusterFileData data;
    std::string error;
    REQUIRE_MESSAGE(build_piece_container(laid.pieces[0].recipe, {}, 0, data, error), error);
    CHECK(data.image_paths.empty());
    REQUIRE(data.materials.size() == 8);
    REQUIRE(data.cluster_material.size() == data.mesh.mesh.clusters.size());
    for (const u32 m : data.cluster_material)
      CHECK(m < 8);
    for (const geometry::ClusterFileMaterial& m : data.materials) {
      const f32 mean = (m.base_color.x + m.base_color.y + m.base_color.z) / 3.0f;
      CHECK(mean > 0.08f);  // the darkest tone of basalt's 0.12
      CHECK(mean < 0.32f);  // the lightest of the pale rock's 0.25
    }
  }
  const f64 share[4] = {.45, .25, .15, .15};
  for (u32 r = 0; r < 4; ++r) {
    MESSAGE("rock " << r << ": " << by_rock[r] / total << " of the triangles");
    CHECK(std::abs(by_rock[r] / total - share[r]) < .05);
  }
}

// **What a tile of bed costs** (W97; measured, printed; the numbers channels.md, "What a tile
// costs", quotes): the gentle valley drained at W93's scratch scene's hydraulic geometry (14 m and
// 1.2 m at a square kilometre), so its trunk is a stream metres wide. Laying every clast of the
// region (what `open` does beside the plan), and the busiest 64 m tile's mesh and container (what
// its activation builds): clasts and triangles a square metre at the finest and coarsest forms.
TEST_CASE("channels: what a tile of bed costs (measured, printed)") {
  ChannelLayer p = layer();
  p.width_at_km2_m = 14;
  p.depth_at_km2_m = 1.2f;
  p.head_threshold_m2 = 300;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 3, nullptr, n, error));
  Vector<PlacedPiece> pieces;
  ClastLog log;
  const auto laid_at = std::chrono::steady_clock::now();
  REQUIRE(place_pieces(p, n, ground(gentle), 64, pieces, error, &log));
  const f64 lay_ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - laid_at).count();
  REQUIRE_FALSE(pieces.empty());
  f64 area = 0;
  for (const f64 a : log.area_m2)
    area += a;
  u32 busiest = 0;
  for (u32 i = 1; i < pieces.size(); ++i)
    if (clast_count(pieces[i].recipe) > clast_count(pieces[busiest].recipe)) busiest = i;
  const PlacedPiece& piece = pieces[busiest];
  geometry::ClusterFileData data;
  const auto built = std::chrono::steady_clock::now();
  REQUIRE_MESSAGE(build_piece_container(piece.recipe, {}, 0, data, error), error);
  const f64 build_ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - built).count();
  u64 root = 0;
  for (u32 k = 0; k < data.mesh.lod.size(); ++k)
    if (data.mesh.lod[k].parent_error == geometry::k_lod_terminal_error)
      root += data.mesh.mesh.clusters[k].triangle_count;
  // The tile's share of the bed's area, by its share of the clasts (the cells are not kept a
  // tile): an estimate, said so on the page.
  const f64 share =
      static_cast<f64>(clast_count(piece.recipe)) / static_cast<f64>(log.clasts.size());
  const f64 tile_area = area * share;
  const ChannelSediment& trunk = n.links.back().sediment[n.links.back().line.size() / 2];
  MESSAGE("region: ", log.clasts.size(), " clasts over ", area, " m2 of bed (",
          static_cast<f64>(log.clasts.size()) / area, " a m2) in ", pieces.size(),
          " tiles, laid in ", lay_ms, " ms; a trunk point's D50 ",
          1e3 * static_cast<f64>(trunk.d50_m), " mm, D84 ", 1e3 * static_cast<f64>(trunk.d84_m),
          " mm, cover ", trunk.cover, "; busiest tile ", clast_count(piece.recipe),
          " clasts over about ", tile_area, " m2, finest ", data.mesh.leaf_triangle_count,
          " triangles (", static_cast<f64>(data.mesh.leaf_triangle_count) / tile_area,
          " a m2), coarsest ", root, " (", static_cast<f64>(root) / tile_area, " a m2), built in ",
          build_ms, " ms");
  CHECK(data.mesh.leaf_triangle_count > 0);
  CHECK(root < data.mesh.leaf_triangle_count);
}
