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
  // A bar's skin is laid below full asymmetry alone (W93: at full asymmetry the bar is the
  // ground's own and nothing is laid on it, which the last lines of this case hold): half here, so
  // the skins are among the pieces checked.
  p.section_asymmetry = 0.5f;
  ChannelNetwork n;
  std::string error;
  REQUIRE(plan_channels(p, ground(gentle), 3, nullptr, n, error));
  Vector<PlacedPiece> pieces;
  REQUIRE(place_pieces(p, n, ground(gentle), 32, pieces, error));
  {
    ChannelLayer full = p;
    full.section_asymmetry = 1;
    ChannelNetwork shaped;
    REQUIRE(plan_channels(full, ground(gentle), 3, nullptr, shaped, error));
    Vector<PlacedPiece> laid;
    REQUIRE(place_pieces(full, shaped, ground(gentle), 32, laid, error));
    u32 bed_pieces = 0;
    for (const PlacedPiece& piece : laid) {
      CHECK(piece.recipe.part != ChannelPart::Bar);
      bed_pieces += piece.recipe.part == ChannelPart::Bed;
    }
    CHECK(bed_pieces > 0);
  }
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

// **The cuts** (channels.md, "The cut"; W89): one per link with the TerrainCut shape, a trapezoid
// of the hydraulic geometry's area under the ground at the line, its bed never rising downstream,
// and the water's surface in the cut lying between that bed and the top of the bank.
TEST_CASE("channels: the cuts have the TerrainCut shape and the water lies inside them") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  // W89's section, which W93 keeps for a layer that asks for it: the trapezoid at every point, no
  // rounding, and the five fields the cut had (W93's section has its own case below).
  p.section_asymmetry = 0;
  p.bank_round = 0;
  p.flood_bed_widths = 1;
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
  // Pieces: the bed and the steps' boulders.
  Vector<PlacedPiece> pieces;
  REQUIRE(place_pieces(p, n, ground(step_pool), 32, pieces, error));
  u32 boulders = 0;
  for (const PlacedPiece& piece : pieces) {
    if (piece.recipe.part == ChannelPart::Boulder) {
      boulders += piece.recipe.slabs.size();
      assets::MeshData mesh;
      REQUIRE_MESSAGE(solve_piece(piece.recipe, mesh, error), error);
      check_closed(mesh, piece.recipe.slabs.size());
    }
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
f64 cut_depth_at(const scene::TerrainCut& cut, u32 i, f64 s) {
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
      scene::TerrainCut cut;
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
    scene::TerrainCut cut;
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
      const f64 widening =
          .5 * static_cast<f64>(p.riffle_widening) * static_cast<f64>(link.swell[i]);
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
      scene::TerrainCut cut;
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
