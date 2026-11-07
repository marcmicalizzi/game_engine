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

TEST_CASE("channels: one channel down a V-notch, its head at the threshold, its width by the "
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
  CHECK(product(head) >= p.head_threshold_m2);
  CHECK(product(head + n.width) < p.head_threshold_m2);
  CHECK(static_cast<f64>(n.area_cells[head]) * cell >= p.min_head_area_m2);
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
  const f64 ratio = std::pow(static_cast<f64>(b.area_m2) / a.area_m2, .4);
  CHECK(static_cast<f64>(b.width_m) / a.width_m == doctest::Approx(ratio).epsilon(1e-4));
  CHECK(b.width_m == doctest::Approx(3.0 * std::pow(b.area_m2 / 1e6, .4)).epsilon(1e-4));
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

TEST_CASE("channels: the placed pieces are closed, welded and outward, and build the same "
          "container twice") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  p.bank_min_width_m = 1.0f;  // the valley's widest bed is 1.3 m
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
  CHECK(banks > 0);
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
  p.bed.tile_m = 0;
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
    CHECK(o.as_object().size() == 6);
    for (const char* key : {"name", "points", "width_m", "depth_m", "slope", "seed"})
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
