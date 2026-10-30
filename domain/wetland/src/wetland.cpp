#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/flora/companion.h>
#include <domain/wetland/wetland.h>

#include <algorithm>
#include <cmath>

namespace engine::wetland {
namespace {
using S = geometry::FloraSpecies;
using P = flora::KitPoint;
constexpr Profile k_profiles[] = {
    {S::sedge, .7f, .7f, .005f, .0015f, 48, .13f},
    {S::tussock, .7f, .7f, .005f, .0015f, 64, .24f},
    {S::rush, .75f, 0, 0, .0015f, 40, .13f},
    {S::reed, 2.4f, .30f, .03f, .005f, 8, .32f},
    {S::cattail, 1.8f, 1.4f, .018f, .005f, 8, .26f},
    {S::arrowhead, .4f, .22f, .12f, .003f, 8, .12f},
    {S::water_lily, 0, .25f, .25f, .003f, 6, .35f},
    {S::pond_lily, 0, .28f, .20f, .004f, 6, .35f},
};
constexpr f64 k_pi = 3.14159265358979323846;
P operator+(P a, P b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
P operator-(P a, P b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
P operator*(P a, f64 b) { return {a.x * b, a.y * b, a.z * b}; }
P cross(P a, P b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
f64 length(P a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
P norm(P a) {
  const auto n = length(a);
  return n > 0 ? a * (1 / n) : P{0, 1, 0};
}
f64 random(u64 seed, u64 slot) {
  return static_cast<f64>(hash_combine(seed, slot) >> 11) / 9007199254740992.0;
}
Vec3 vec(P p) { return {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)}; }
bool floating(S s) { return s == S::water_lily || s == S::pond_lily; }
struct Writer {
  assets::MeshData& mesh;
  flora::Motion& motion;
  Vector<u8> owners;
  u32 part = 0;
  u8 material = 1;
  bool rigid = false;
  u32 vertex(P p, P normal, f64 u, f64 v, f64 weight) {
    const u32 index = mesh.positions.size();
    mesh.positions.push_back(vec(p));
    mesh.normals.push_back(vec(norm(normal)));
    mesh.uvs.push_back({static_cast<f32>(u), static_cast<f32>(v)});
    motion.vertex_part.push_back(part);
    motion.vertex_weight.push_back(rigid ? 0 : static_cast<f32>(std::clamp(weight, 0.0, 1.0)));
    return index;
  }
  void triangle(u32 a, u32 b, u32 c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
    owners.push_back(material);
  }
  void tube(P a, P b, f64 radius, u32 sides, f64 w0 = 0, f64 w1 = 1) {
    flora::KitWriter kit{mesh, motion, part, 0};
    const P points[] = {a, b};
    const f64 radii[] = {radius, radius * .7};
    const f64 weights[] = {rigid ? 0 : w0, rigid ? 0 : w1};
    const auto before = mesh.indices.size();
    const u32 first_vertex = mesh.positions.size();
    flora::kit_polyline_tube(kit, points, radii, weights, sides);
    for (u32 v = first_vertex; v < mesh.uvs.size(); ++v) {
      mesh.uvs[v].x /= static_cast<f32>(2 * k_pi * radius);
      mesh.uvs[v].y /= static_cast<f32>(length(b - a) + radii[1]);
    }
    for (u32 i = before; i < mesh.indices.size(); i += 3)
      owners.push_back(material);
  }
  // A folded ribbon, preserving the taper shoulder and endpoint on every rung.
  void blade(P base, P heading, f64 len, f64 width, f64 lean, f64 droop, u32 rows, f64 station = 0,
             f64 tip_weight = 1) {
    const P side{-heading.z, 0, heading.x};
    const u32 first = mesh.positions.size();
    for (u32 r = 0; r < rows; ++r) {
      const f64 t = .65 * static_cast<f64>(r) / (rows - 1);
      const P centre = base + heading * (len * lean * t * t) + P{0, len * (t - droop * t * t), 0};
      const P tangent = heading * (2 * lean * t) + P{0, 1 - 2 * droop * t, 0};
      const P n = norm(cross(tangent, side));
      const f64 w = station + (tip_weight - station) * t;
      vertex(centre - side * (width * .5), n, 0, t, w);
      vertex(centre + n * (width * .16), n, .5, t, w);
      vertex(centre + side * (width * .5), n, 1, t, w);
    }
    const P tip = base + heading * (len * lean) + P{0, len * (1 - droop), 0};
    const u32 end = vertex(tip, {0, 1, 0}, .5, 1, tip_weight);
    for (u32 r = 0; r + 1 < rows; ++r) {
      const u32 a = first + 3 * r, b = a + 3;
      triangle(a, b, a + 1);
      triangle(a + 1, b, b + 1);
      triangle(a + 1, b + 1, a + 2);
      triangle(a + 2, b + 1, b + 2);
    }
    const u32 a = end - 3;
    triangle(a, end, a + 1);
    triangle(a + 1, end, a + 2);
  }
  void leaf(P centre, P forward, f64 len, f64 width, bool arrow, u32 sides, f64 weight) {
    const P side{forward.z, 0, -forward.x};
    const P up = arrow ? P{forward.x * .45, .8930285549745876, forward.z * .45} : forward;
    const P normal = norm(cross(side, up));
    const u32 root = vertex(centre, normal, .5, .5, weight);
    if (arrow) {
      constexpr f64 outline[][2] = {{0, 1},    {-.28, .12}, {-.5, -.4},
                                    {0, -.12}, {.5, -.4},   {.28, .12}};
      for (const auto& q : outline)
        vertex(centre + side * (q[0] * width) + up * (q[1] * len / 1.4), normal, q[0] + .5,
               (q[1] + .4) / 1.4, weight);
      for (u32 i = 0; i < 6; ++i)
        triangle(root, root + 1 + i, root + 1 + (i + 1) % 6);
    } else {
      // An open fan leaves a real basal sinus; it is not a painted solid disc.
      for (u32 i = 0; i <= sides; ++i) {
        const f64 angle = .16 + (2 * k_pi - .32) * i / sides;
        const f64 x = det::sin(angle), z = det::cos(angle);
        vertex(centre + side * (x * width * .5) + up * (z * len * .5), normal, x * .5 + .5,
               z * .5 + .5, weight);
      }
      for (u32 i = 0; i < sides; ++i)
        triangle(root, root + 2 + i, root + 1 + i);
    }
  }
  void finish() {
    Vector<u32> indices;
    for (u32 mat = 0; mat < 2; ++mat) {
      const u32 first = indices.size();
      for (u32 t = 0; t < owners.size(); ++t)
        if (owners[t] == mat)
          for (u32 j = 0; j < 3; ++j)
            indices.push_back(mesh.indices[t * 3 + j]);
      if (indices.size() > first)
        mesh.primitives.push_back({first, indices.size() - first, static_cast<i32>(mat), -1});
    }
    mesh.indices = std::move(indices);
  }
};
void generate(const Plant& f, const Profile& p, u32 detail, assets::MeshData& mesh,
              flora::Motion& motion, Report& report) {
  mesh = {};
  motion = {};
  report.live_shoots = 0;
  report.dead_shoots = 0;
  assets::Material dead, live;
  dead.name = "dead";
  dead.base_color = {.32f, .22f, .105f, 1};
  live.name = "live";
  live.base_color = {.075f, .22f, .045f, 1};
  dead.metallic = live.metallic = 0;
  dead.roughness = live.roughness = .7f;
  dead.double_sided = live.double_sided = true;
  mesh.materials.push_back(dead);
  mesh.materials.push_back(live);
  Writer w{mesh, motion, {}, 0, 1, floating(p.species)};
  const u32 count = f.shoots ? f.shoots : p.shoots;
  const u32 sides = detail == 0 ? 6u : detail == 1 ? 4u : 3u;
  const u32 rows = detail == 0 ? 6u : detail == 1 ? 4u : 2u;
  const f64 crown = static_cast<f64>(p.species == S::tussock ? f.tussock_height_m : 0.0f);
  for (u32 i = 0; i < count; ++i) {
    const u64 seed = hash_combine(f.seed, i);
    const f64 angle = 2 * k_pi * random(seed, 0);
    const f64 rad = static_cast<f64>(p.crown_radius_m * f.spread) * std::sqrt(random(seed, 1));
    const P heading{det::cos(angle), 0, det::sin(angle)};
    P base = heading * rad;
    base.y = crown > 0 ? crown * (1 - rad / (1.1 * static_cast<f64>(p.crown_radius_m) *
                                             static_cast<f64>(f.spread))) -
                             .01
                       : 0;
    const bool dead_shoot = random(seed, 2) < static_cast<f64>(f.dead_share);
    const bool broken = dead_shoot && random(seed, 3) < static_cast<f64>(f.broken_share);
    const f64 shortening = broken ? .35 + .45 * random(seed, 4) : 1;
    const f64 h =
        static_cast<f64>(p.height_m * f.stature) * (.8 + .2 * random(seed, 5)) * shortening;
    const f64 leaf_scale = .85 + .15 * random(seed, 6);
    if (dead_shoot)
      ++report.dead_shoots;
    else
      ++report.live_shoots;
    flora::KitWriter kit{mesh, motion};
    const f64 part_length =
        floating(p.species) ? static_cast<f64>(f.water_depth_m) : std::max(h, .05);
    w.part = flora::kit_begin_part(kit, base, {0, 1, 0}, part_length, geometry::k_motion_no_parent,
                                   0, 0, 0);
    w.material = dead_shoot ? 0 : 1;
    if (p.species == S::sedge || p.species == S::tussock) {
      for (u32 j = 0; j < 3; ++j) {
        const f64 a = angle + 2 * k_pi * j / 3;
        w.blade(base, {det::cos(a), 0, det::sin(a)}, h,
                static_cast<f64>(p.leaf_width_m) * leaf_scale, .25 + .25 * random(seed, 7 + j),
                dead_shoot ? .48 : .26, rows);
      }
      // Three-sided culm; dense terminal spike and subordinate lateral spike.
      w.tube(base, base + P{0, h, 0}, static_cast<f64>(p.stem_radius_m), 3);
      w.material = 0;
      w.tube(base + P{0, h * .85, 0}, base + P{0, h, 0}, .002, 3, .85, 1);
    } else if (p.species == S::rush) {
      const P tip = base + heading * (h * .12) + P{0, h, 0};
      w.tube(base, tip, static_cast<f64>(p.stem_radius_m), sides);
      if (!broken) {
        w.material = 0;
        for (u32 j = 0; j < 4; ++j) {
          const f64 a = angle + j * 2 * k_pi / 4;
          const P start = base + (tip - base) * .78;
          w.tube(start, start + P{det::cos(a) * .03, .04, det::sin(a) * .03}, .0008, 3, .78, .84);
        }
      }
    } else if (p.species == S::reed || p.species == S::cattail) {
      const P tip = base + heading * (h * .07) + P{0, h, 0};
      w.tube(base, tip, static_cast<f64>(p.stem_radius_m), sides);
      const u32 leaves = p.species == S::reed ? 7u : 5u;
      for (u32 j = 0; j < leaves; ++j) {
        const f64 a = angle + j * (p.species == S::reed ? k_pi : 2.399963229728653);
        const f64 station = p.species == S::reed ? .18 + .65 * j / leaves : 0;
        const P leaf_base = base + (tip - base) * station;
        const f64 l = static_cast<f64>(p.leaf_length_m) * leaf_scale *
                      static_cast<f64>(p.species == S::cattail ? f.stature : 1.0f) * shortening;
        w.blade(leaf_base, {det::cos(a), 0, det::sin(a)}, l,
                static_cast<f64>(p.leaf_width_m) * leaf_scale, p.species == S::reed ? .85 : .28,
                dead_shoot ? .65 : .32, rows, station, p.species == S::reed ? station : 1);
      }
      if (!broken) {
        w.material = 0;
        if (p.species == S::cattail) {
          w.tube(base + (tip - base) * .82, base + (tip - base) * .97, .018, sides, .82, .97);
        } else {
          // A branched plume, not one enlarged solid cone.
          for (u32 j = 0; j < 12; ++j) {
            const f64 t = static_cast<f64>(j) / 12;
            const f64 a = angle + j * 2.399963229728653;
            const P start = tip + P{0, -.3 + .3 * t, 0};
            const f64 reach = .065 * (1 - t) + .006;
            const P end = start + P{det::cos(a) * reach, .08 * (1 - t), det::sin(a) * reach};
            w.tube(start, end, .0015, 3, .85 + .15 * t, 1);
            flora::KitWriter card{mesh, motion, w.part, .9};
            const auto before = mesh.indices.size();
            flora::kit_leaf_card(card, end, {det::cos(a), .4, det::sin(a)},
                                 {-det::sin(a), 0, det::cos(a)}, .035, .012, .1);
            for (u32 k = before; k < mesh.indices.size(); k += 3)
              w.owners.push_back(0);
          }
        }
      }
    } else if (p.species == S::arrowhead) {
      const P tip = base + heading * (h * .25) + P{0, h, 0};
      w.tube(base, tip, static_cast<f64>(p.stem_radius_m), sides);
      w.leaf(tip, heading, static_cast<f64>(p.leaf_length_m) * leaf_scale,
             static_cast<f64>(p.leaf_width_m) * leaf_scale, true, 0, 1);
    } else {
      const P tip{base.x, static_cast<f64>(f.water_depth_m) + .002, base.z};
      w.tube({0, 0, 0}, tip, static_cast<f64>(p.stem_radius_m), sides);
      w.leaf(tip, heading, static_cast<f64>(p.leaf_length_m) * leaf_scale,
             static_cast<f64>(p.leaf_width_m) * leaf_scale, false,
             detail == 0   ? 24u
             : detail == 1 ? 16u
                           : 10u,
             0);
    }
  }
  if (crown > 0) {
    flora::KitWriter kit{mesh, motion};
    w.part = flora::kit_begin_part(kit, {0, 0, 0}, {0, 1, 0}, crown, geometry::k_motion_no_parent,
                                   0, 0, 0);
    kit.weight = 0;
    const auto before = mesh.indices.size();
    flora::kit_dome(kit, {0, -.01, 0}, {0, 1, 0}, {1, 0, 0},
                    static_cast<f64>(p.crown_radius_m * f.spread) * 1.1, crown, 1, sides * 4, rows);
    for (u32 i = before; i < mesh.indices.size(); i += 3)
      w.owners.push_back(0);
  }
  w.finish();
  report.triangles = mesh.indices.size() / 3;
  report.parts = motion.parts.size();
  report.detail = detail;
}
}  // namespace
std::span<const Profile> profiles() noexcept { return k_profiles; }
const Profile* profile(std::string_view name) noexcept {
  for (const auto& p : k_profiles)
    if (geometry::flora_species_info(p.species).name == name) return &p;
  return nullptr;
}
bool in_range(const Plant& f, std::string& error) {
  const char* bad = nullptr;
  if (f.collider)
    bad = "collider (wetland plants do not collide)";
  else if (!profile(f.species))
    bad = "species";
  else if (f.triangle_budget && (f.triangle_budget < 64 || f.triangle_budget > 100000))
    bad = "triangle_budget";
  else if (f.shoots > 256)
    bad = "shoots";
  else if (!(f.dead_share >= 0 && f.dead_share <= 1))
    bad = "dead_share";
  else if (!(f.broken_share >= 0 && f.broken_share <= 1))
    bad = "broken_share";
  else if (!(f.spread >= .5f && f.spread <= 2))
    bad = "spread";
  else if (!(f.stature >= .75f && f.stature <= 1.25f))
    bad = "stature";
  else if (!(f.tussock_height_m >= 0 && f.tussock_height_m <= .5f))
    bad = "tussock_height_m";
  else if (!(f.water_depth_m >= .05f && f.water_depth_m <= 2))
    bad = "water_depth_m";
  if (bad) {
    error = std::string("engine.wetland.Plant.") + bad + " is out of range (wetland.schema)";
    return false;
  }
  return true;
}
bool solve(const Plant& f, assets::MeshData& mesh, flora::Motion& motion, Report& report,
           std::string& error) {
  mesh = {};
  motion = {};
  report = {};
  if (!in_range(f, error)) return false;
  const auto& p = *profile(f.species);
  report.budget = f.triangle_budget ? f.triangle_budget
                                    : geometry::flora_species_info(p.species).triangle_budget;
  for (u32 detail = 0; detail < 3; ++detail) {
    generate(f, p, detail, mesh, motion, report);
    // Reserve six triangles for the container's orthogonal far cards.
    if (report.triangles + 6 <= report.budget) return true;
  }
  error = "wetland triangle_budget cannot hold " + std::to_string(report.triangles + 6) +
          " triangles at the detail floor";
  mesh = {};
  motion = {};
  return false;
}
bool read_file(std::string_view text, Plant& out, std::string& error) {
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok) {
    error = parsed.message;
    return false;
  }
  Plant f;
  schema::ReadContext ctx;
  if (!schema::from_json(f, json, ctx) || !ctx.ok()) {
    for (const auto& d : ctx.diagnostics)
      error += d.path + ": " + d.message + "; ";
    return false;
  }
  if (!in_range(f, error)) return false;
  out = std::move(f);
  return true;
}
std::string canonical_json(const Plant& f) {
  JsonWriteOptions opts;
  opts.pretty = false;
  return write_json(schema::to_json(f), opts);
}
u64 source_hash(const Plant& f) {
  const auto text = canonical_json(f);
  return hash_combine(hash_bytes(text.data(), text.size()), k_wetland_version);
}
}  // namespace engine::wetland
