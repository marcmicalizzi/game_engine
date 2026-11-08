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
#include <cmath>
#include <cstring>
#include <limits>
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

// Welded by exact position, closed, two-manifold, outward and `parts` components: rocks'
// check (domain/rocks/tests/rock_checks.h), which a capability's test may not include.
void check_closed(const assets::MeshData& mesh, u32 parts) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  REQUIRE(mesh.indices.size() % 3 == 0);
  const u32 n = mesh.positions.size();
  std::vector<u32> order(n), weld(n);
  for (u32 i = 0; i < n; ++i)
    order[i] = i;
  auto less = [&](u32 a, u32 b) {
    const Vec3 pa = mesh.positions[a], pb = mesh.positions[b];
    if (pa.x != pb.x) return pa.x < pb.x;
    if (pa.y != pb.y) return pa.y < pb.y;
    return pa.z < pb.z;
  };
  std::sort(order.begin(), order.end(), less);
  u32 unique = 0;
  for (u32 i = 0; i < n; ++i)
    weld[order[i]] = (i > 0 && !less(order[i - 1], order[i])) ? weld[order[i - 1]] : unique++;
  std::vector<u64> directed;
  u32 degenerate = 0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const u32 w[3] = {weld[mesh.indices[i]], weld[mesh.indices[i + 1]], weld[mesh.indices[i + 2]]};
    if (w[0] == w[1] || w[1] == w[2] || w[0] == w[2]) ++degenerate;
    for (u32 e = 0; e < 3; ++e)
      directed.push_back(static_cast<u64>(w[e]) << 32 | w[(e + 1) % 3]);
  }
  CHECK(degenerate == 0);
  std::sort(directed.begin(), directed.end());
  u32 repeated = 0, unmatched = 0;
  for (u32 i = 0; i < directed.size(); ++i) {
    if (i > 0 && directed[i] == directed[i - 1]) ++repeated;
    const u64 reverse = (directed[i] & 0xffffffffull) << 32 | directed[i] >> 32;
    if (!std::binary_search(directed.begin(), directed.end(), reverse)) ++unmatched;
  }
  CHECK_MESSAGE(repeated == 0, repeated << " edges used twice in one direction");
  CHECK_MESSAGE(unmatched == 0, unmatched << " edges with no opposite: a hole");
  std::vector<u32> parent(unique);
  for (u32 i = 0; i < unique; ++i)
    parent[i] = i;
  auto find = [&](u32 x) {
    while (parent[x] != x)
      x = parent[x] = parent[parent[x]];
    return x;
  };
  for (const u64 e : directed)
    parent[find(static_cast<u32>(e >> 32))] = find(static_cast<u32>(e & 0xffffffffull));
  u32 components = 0;
  for (u32 i = 0; i < unique; ++i)
    components += find(i) == i ? 1u : 0u;
  CHECK(components == parts);
  f64 volume = 0;
  u32 against = 0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]], b = mesh.positions[mesh.indices[i + 1]],
               c = mesh.positions[mesh.indices[i + 2]];
    volume += static_cast<f64>(dot(a, cross(b, c))) / 6.0;
    const Vec3 vn = mesh.normals[mesh.indices[i]] + mesh.normals[mesh.indices[i + 1]] +
                    mesh.normals[mesh.indices[i + 2]];
    if (dot(cross(b - a, c - a), vn) <= 0.0f) ++against;
  }
  CHECK(volume > 0.0);
  CHECK_MESSAGE(against == 0, against << " faces wound against their normals");
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
  for (const ChannelPoint& pt : link.line)
    CHECK(std::abs(pt.at.x - 128) < .01);
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
  CHECK(serial.length_m > 400);
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
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 3, nullptr, n, error));
  Vector<PlacedPiece> pieces;
  REQUIRE(place_pieces(p, n, ground(gentle), 32, pieces, error));
  u32 beds = 0, bars = 0, banks = 0;
  for (const PlacedPiece& piece : pieces) {
    assets::MeshData mesh;
    REQUIRE_MESSAGE(solve_piece(piece.recipe, mesh, error), error);
    check_closed(mesh, piece.recipe.slabs.size());
    beds += piece.recipe.part == ChannelPart::Bed;
    bars += piece.recipe.part == ChannelPart::Bar;
    banks += piece.recipe.part == ChannelPart::Bank;
  }
  CHECK(beds > 0);
  CHECK(bars > 0);
  // No bank piece since W89: the banks are the faces of the cut the terrain's `cuts` make.
  CHECK(banks == 0);
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
  c.ground = ground(gentle);
  c.tile_size = 64;
  Opened opened;
  REQUIRE(opened.desc != nullptr);
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  std::string error;
  REQUIRE_MESSAGE(opened.desc->open(schema::to_json(p), c, &opened.state, &error), error);
  scene_gen::Placements whole, forward, backward;
  REQUIRE(opened.desc->expand(opened.state, c, whole, &error));
  REQUIRE_FALSE(whole.instances.empty());
  for (i32 z = 0; z < 8; ++z)
    for (i32 x = 0; x < 4; ++x)
      REQUIRE(opened.desc->tile(opened.state, {x, z}, c, forward, &error));
  for (i32 z = 7; z >= 0; --z)
    for (i32 x = 3; x >= 0; --x) {
      if (opened.desc->occupies(opened.state, {x, z})) {
        const u32 before = backward.instances.size();
        REQUIRE(opened.desc->tile(opened.state, {x, z}, c, backward, &error));
        CHECK(backward.instances.size() > before);
      }
    }
  auto by_id = [](const auto& a, const auto& b) { return a.id < b.id; };
  std::sort(forward.instances.begin(), forward.instances.end(), by_id);
  std::sort(backward.instances.begin(), backward.instances.end(), by_id);
  REQUIRE(forward.instances.size() == whole.instances.size());
  REQUIRE(backward.instances.size() == whole.instances.size());
  for (u32 i = 0; i < whole.instances.size(); ++i) {
    CHECK(whole.instances[i].id == i + 1);
    CHECK(forward.instances[i].id == whole.instances[i].id);
    CHECK(backward.instances[i].mesh == whole.instances[i].mesh);
    CHECK(backward.instances[i].position == whole.instances[i].position);
    CHECK(io::exists(whole.meshes[whole.instances[i].mesh].path));
  }
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
  p.bed.tile_m = 0.0f;
  CHECK_FALSE(layer_in_range(p, error));
  p = layer();
  p.head_threshold_m2 = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(layer_in_range(p, error));
  // A recipe with a top below its bottom, or a short array.
  ChannelPiece piece;
  piece.surface.color = std::array<f32, 3>{.5f, .5f, .5f};
  ChannelSlab slab;
  slab.x = {0, 1, 0, 1};
  slab.z = {0, 0, 1, 1};
  slab.top = {1, 1, 1, 1};
  slab.bottom = {0, 0, 0, 0};
  piece.slabs.push_back(slab);
  CHECK(piece_in_range(piece, error));
  piece.slabs[0].bottom[2] = 2;
  CHECK_FALSE(piece_in_range(piece, error));
  piece.slabs[0].bottom[2] = 0;
  piece.slabs[0].top.pop_back();
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

// **The cuts** (channels.md, "The cut"; W89): one per link with the TerrainCut shape, a trapezoid
// of the hydraulic geometry's area under the ground at the line, its bed never rising downstream,
// and the water's surface in the cut lying between that bed and the top of the bank.
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
    CHECK(o.as_object().size() == 5);
    scene::TerrainCut cut;
    schema::ReadContext reader;
    REQUIRE(schema::from_json(cut, o, reader));
    REQUIRE(reader.ok());
    CHECK(cut.bank_slope == p.bank_slope);
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
      // At least a thalweg under the top, and never more than two (the cap over a sill) but for a
      // step's pool (W90) and a tributary's grade to its trunk's bed (W90).
      CHECK(d >= thalweg - 2e-3);
      const f64 grade = link.trunk != ~0u ? top - n.links[link.trunk].bed[0] : 0.0;
      CHECK(d <= std::max(2 * thalweg + static_cast<f64>(link.step_depth[i]), grade) + 2e-3);
      if (i == 0 && 4 * mean / (m * w) < 1)
        CHECK(d * (w - d / m) == doctest::Approx(w * mean).epsilon(0.01));
      // The water's surface stands in the cut: over the bed, under the top of the bank.
      if (i >= surface.size()) continue;
      f64 y = 0;
      REQUIRE(surface[i][1].get_f64(y));
      CHECK(y >= bed + .25 * thalweg - 2e-3);  // at least half the stage's depth (0.5)
      CHECK(y < top);
    }
  }
  CHECK(blocks >= 1);
  // Written twice, the same text.
  CHECK(write_json(terrain_cuts_json(p, n)) == write_json(cuts));
}

// The bed the pieces lay is the cut's flat bed, narrower than the bankfull width (W89).
TEST_CASE("channels: the bed lies inside the cut's banks") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 3, nullptr, n, error));
  Vector<PlacedPiece> pieces;
  REQUIRE(place_pieces(p, n, ground(gentle), 32, pieces, error));
  f64 widest_bed = 0, widest_channel = 0;
  for (const ChannelLink& link : n.links)
    for (const ChannelPoint& pt : link.line)
      widest_channel = std::max(widest_channel, static_cast<f64>(pt.width_m));
  for (const PlacedPiece& piece : pieces) {
    if (piece.recipe.part != ChannelPart::Bed) continue;
    for (const ChannelSlab& slab : piece.recipe.slabs) {
      // Across a row: the first and last columns' points.
      for (u32 r = 0; r < slab.rows; ++r) {
        const u32 a = r * slab.columns, b = a + slab.columns - 1;
        const f64 dx = static_cast<f64>(slab.x[b]) - static_cast<f64>(slab.x[a]);
        const f64 dz = static_cast<f64>(slab.z[b]) - static_cast<f64>(slab.z[a]);
        widest_bed = std::max(widest_bed, std::sqrt(dx * dx + dz * dz));
      }
    }
  }
  CHECK(widest_bed > 0);
  CHECK(widest_bed < widest_channel);
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
  // The defaults are the bounds the grounding page pins (f32, so each is read as the layer holds it).
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

TEST_CASE("channels: a step-pool's steps by their spacing and height, boulders on them") {
  const ChannelLayer p = layer();
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(steep), 0, nullptr, n, error));
  REQUIRE(n.links.size() == 1);
  const ChannelLink& link = n.links[0];
  REQUIRE(link.step_at.size() >= 4);
  REQUIRE(link.step_height.size() == link.step_at.size());
  Vector<f64> s(link.line.size(), 0.0);
  for (u32 i = 1; i < link.line.size(); ++i)
    s[i] = s[i - 1] + std::hypot(link.line[i].at.x - link.line[i - 1].at.x,
                                 link.line[i].at.z - link.line[i - 1].at.z);
  for (u32 q = 0; q + 1 < link.step_at.size(); ++q) {
    const u32 i = link.step_at[q], j = link.step_at[q + 1];
    const f64 w = static_cast<f64>(link.line[i].width_m);
    const f64 spacing = s[j] - s[i];
    // 1 to 4 widths (the step-pool literature), to the line's step either way.
    CHECK(spacing >= w - static_cast<f64>(p.step_m) - 1e-9);
    CHECK(spacing <= 4 * w + static_cast<f64>(p.step_m) + 1e-9);
    // H / L / S between 1 and 2 (Abrahams et al. 1995), at the layer's 1.5, over the line's 12 %.
    const f64 ratio = static_cast<f64>(link.step_height[q]) / spacing / .12;
    CHECK(ratio > 1.0);
    CHECK(ratio < 2.0);
  }
  // The pools lower the bed, never raise it; a step is at most its height deep.
  for (u32 i = 0; i < link.line.size(); ++i) {
    CHECK(link.step_depth[i] >= 0.0f);
    CHECK(link.step_depth[i] <= 4.0f);
  }
  // Pieces: the bed and the steps' boulders, no bars on a cascade.
  Vector<PlacedPiece> pieces;
  REQUIRE(place_pieces(p, n, ground(steep), 32, pieces, error));
  u32 bars = 0, boulders = 0;
  for (const PlacedPiece& piece : pieces) {
    bars += piece.recipe.part == ChannelPart::Bar;
    if (piece.recipe.part == ChannelPart::Boulder) {
      boulders += piece.recipe.slabs.size();
      assets::MeshData mesh;
      REQUIRE_MESSAGE(solve_piece(piece.recipe, mesh, error), error);
      check_closed(mesh, piece.recipe.slabs.size());
    }
  }
  CHECK(bars == 0);
  CHECK(boulders >= link.step_at.size());
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
  Vector<PlacedPiece> pieces;
  REQUIRE(place_pieces(p, n, ground(face), 32, pieces, error));
  CHECK(pieces.empty());
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
  // The line stops at the shoreline: its last point within a millimetre of z = 100, its ground at
  // the level, and no point below it.
  CHECK(link.line.back().at.z == doctest::Approx(100).epsilon(1e-4));
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
  CHECK(std::fabs(w_up - static_cast<f64>(link.line[widths.size() / 2].width_m)) <= 1e-3);
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
    CHECK(z >= 100 - 1e-3);
  }
  // Pieces: none under the lake.
  Vector<PlacedPiece> pieces;
  REQUIRE(place_pieces(p, wet, ground(notch), 32, pieces, error));
  REQUIRE_FALSE(pieces.empty());
  for (const PlacedPiece& piece : pieces)
    for (const ChannelSlab& slab : piece.recipe.slabs)
      for (f32 z : slab.z)
        CHECK(piece.origin.z + static_cast<f64>(z) >= 100 - 2.0);
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