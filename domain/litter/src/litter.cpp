#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/litter/litter.h>

#include <algorithm>
#include <cmath>

namespace engine::litter {
namespace {
constexpr f64 k_tau = 6.2831853071795864769;
bool fail(std::string& error, std::string_view why) {
  error = "litter: " + std::string(why);
  return false;
}
bool between(f32 v, f32 lo, f32 hi) { return std::isfinite(v) && v >= lo && v <= hi; }
f32 draw(u64 seed, u64 key) {
  return static_cast<f32>(hash_combine(seed, key) >> 40) * (1.0f / 16777216.0f);
}
Vec3 yaw(Vec3 p, f64 angle) {
  const f32 c = static_cast<f32>(det::cos(angle)), s = static_cast<f32>(det::sin(angle));
  return {c * p.x + s * p.z, p.y, -s * p.x + c * p.z};
}
void triangle(assets::MeshData& m, Vec3 a, Vec3 b, Vec3 c, Vec2 ua, Vec2 ub, Vec2 uc) {
  const Vec3 area = cross(b - a, c - a);
  const f32 magnitude = length(area);
  const Vec3 n = magnitude > 0 ? area / magnitude : Vec3::unit_y();
  const u32 first = m.positions.size();
  for (const auto v : {a, b, c}) {
    m.positions.push_back(v);
    m.normals.push_back(n);
  }
  for (const auto v : {ua, ub, uc})
    m.uvs.push_back(v);
  for (u32 j = 0; j < 3; ++j)
    m.indices.push_back(first + j);
}
void quad(assets::MeshData& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
  triangle(m, a, b, c, {0, 0}, {0, 1}, {1, 1});
  triangle(m, a, c, d, {0, 0}, {1, 1}, {1, 0});
}
// A bent sheet with a midrib and lobed edges. Bark also carries the opposite skin and edges.
void sheet(assets::MeshData& m, Kind kind, f32 len, f32 width, f32 curl, u32 segments) {
  auto point = [&](u32 row, i32 side) {
    const f32 t = static_cast<f32>(row) / static_cast<f32>(segments);
    f32 w = width * .5f;
    if (kind == Kind::leaf)
      w *= (.06f + .94f * static_cast<f32>(det::sin(static_cast<f64>(t) * 3.141592653589793))) *
           (.72f + .28f * static_cast<f32>(det::cos(static_cast<f64>(t) * k_tau * 4)));
    const f32 x = static_cast<f32>(side) * w;
    const f32 y = curl * len * ((t - .5f) * (t - .5f) + .2f * static_cast<f32>(side * side));
    return Vec3{x, y, (t - .5f) * len};
  };
  for (u32 i = 0; i < segments; ++i) {
    for (i32 side = -1; side < 1; ++side) {
      const Vec3 a = point(i, side), b = point(i + 1, side), c = point(i + 1, side + 1),
                 d = point(i, side + 1);
      const f32 u0 = static_cast<f32>(side + 1) * .5f, u1 = u0 + .5f;
      const f32 v0 = static_cast<f32>(i) / static_cast<f32>(segments);
      const f32 v1 = static_cast<f32>(i + 1) / static_cast<f32>(segments);
      triangle(m, a, b, c, {u0, v0}, {u0, v1}, {u1, v1});
      triangle(m, a, c, d, {u0, v0}, {u1, v1}, {u1, v0});
      if (kind == Kind::bark) {
        const Vec3 down{0, -.001f, 0};
        quad(m, d + down, c + down, b + down, a + down);
        if (side == -1) quad(m, a, a + down, b + down, b);
        if (side == 0) quad(m, c, c + down, d + down, d);
        if (i == 0) quad(m, d, d + down, a + down, a);
        if (i + 1 == segments) quad(m, b, b + down, c + down, c);
      }
    }
  }
}
void needle(assets::MeshData& m, f32 len, f32 width, u32 segments) {
  for (u32 i = 0; i < segments; ++i) {
    const f32 t = static_cast<f32>(i) / static_cast<f32>(segments);
    const f32 u = static_cast<f32>(i + 1) / static_cast<f32>(segments);
    const f32 a = width * (.5f - .4f * t), b = width * (.5f - .4f * u);
    const Vec3 p0{-a, .002f * t * t, (t - .5f) * len};
    const Vec3 p1{-b, .002f * u * u, (u - .5f) * len};
    const Vec3 p2{b, .002f * u * u, (u - .5f) * len};
    const Vec3 p3{a, .002f * t * t, (t - .5f) * len};
    triangle(m, p0, p1, p2, {0, t}, {0, u}, {1, u});
    triangle(m, p0, p2, p3, {0, t}, {1, u}, {1, t});
  }
}
void cone(assets::MeshData& m, f32 len, f32 width, u32 rows) {
  auto core = [&](u32 row, u32 side) {
    const f32 t = static_cast<f32>(row) / static_cast<f32>(rows);
    const f32 r = width * .3f *
                  (.2f + .8f * static_cast<f32>(det::sin(static_cast<f64>(t) * 3.141592653589793)));
    const f64 angle = static_cast<f64>(side % 8) * k_tau / 8;
    return Vec3{r * static_cast<f32>(det::cos(angle)),
                width * .5f + r * static_cast<f32>(det::sin(angle)), (t - .5f) * len};
  };
  for (u32 row = 0; row < rows; ++row)
    for (u32 side = 0; side < 8; ++side)
      quad(m, core(row, side), core(row, side + 1), core(row + 1, side + 1), core(row + 1, side));
  for (u32 side = 0; side < 8; ++side) {
    triangle(m, {0, width * .5f, -len * .5f}, core(0, side + 1), core(0, side), {.5f, .5f}, {0, 0},
             {1, 0});
    triangle(m, {0, width * .5f, len * .5f}, core(rows, side), core(rows, side + 1), {.5f, .5f},
             {0, 0}, {1, 0});
  }
  // Overlapping thin woody scales on an elongated core, unarmed like white pine.
  for (u32 row = 0; row < rows; ++row) {
    const f32 t = (static_cast<f32>(row) + .5f) / static_cast<f32>(rows);
    const f32 r = width * .5f *
                  (.2f + .8f * static_cast<f32>(det::sin(static_cast<f64>(t) * 3.141592653589793)));
    for (u32 j = 0; j < 8; ++j) {
      const f64 a = (j + .5 * (row % 2)) * k_tau / 8;
      auto p = [&](f64 angle, f32 radius, f32 z) {
        return Vec3{radius * static_cast<f32>(det::cos(angle)),
                    width * .5f + radius * static_cast<f32>(det::sin(angle)), z};
      };
      const f32 z = (t - .5f) * len, dz = len / static_cast<f32>(rows) * .65f;
      const Vec3 base = p(a, r * .65f, z - dz), left = p(a - .34, r, z),
                 tip = p(a, r * 1.08f, z + dz), right = p(a + .34, r, z);
      triangle(m, base, left, tip, {.5f, 0}, {0, .5f}, {.5f, 1});
      triangle(m, base, tip, right, {.5f, 0}, {.5f, 1}, {1, .5f});
    }
  }
}
}  // namespace
Profile profile(Kind k) {
  switch (k) {
    case Kind::leaf: return {.12f, .065f, 64, 4096};
    case Kind::needles: return {.10f, .001f, 400, 4800};
    case Kind::cone: return {.15f, .04f, 5, 4096};
    case Kind::bark: return {.09f, .035f, 20, 2048};
    case Kind::twig: return {.35f, .006f, 7, 12000};
    case Kind::tangle: return {.7f, .016f, 4, 24000};
  }
  return {};
}
bool in_range(const Recipe& p, std::string& error) {
  if (static_cast<u32>(p.kind) > static_cast<u32>(Kind::tangle)) return fail(error, "unknown kind");
  if (p.count > 4096) return fail(error, "count must be 0..4096");
  if (!between(p.radius_m, .05f, 4)) return fail(error, "radius_m must be .05..4");
  if (!between(p.depth_m, 0, .25f)) return fail(error, "depth_m must be 0...25");
  const bool wood = p.kind == Kind::twig || p.kind == Kind::tangle;
  if (p.length_m != 0 && !between(p.length_m, wood ? .1f : .01f, 2))
    return fail(error, "length_m out of range");
  if (p.width_m != 0 && !between(p.width_m, wood ? .006f : .0005f, .2f))
    return fail(error, "width_m out of range");
  if (!between(p.curl, 0, 1)) return fail(error, "curl must be 0..1");
  if (p.triangle_budget != 0 && (p.triangle_budget < 32 || p.triangle_budget > 1000000))
    return fail(error, "triangle_budget must be 0 or 32..1000000");
  return true;
}
bool read_file(std::string_view text, Recipe& out, std::string& error) {
  JsonValue value;
  const auto parsed = parse_json(text, value);
  if (!parsed.ok) return fail(error, parsed.message);
  schema::ReadContext ctx;
  if (!schema::from_json(out, value, ctx) || !ctx.ok()) {
    if (!ctx.diagnostics.empty())
      return fail(error, ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message);
    return fail(error, "invalid engine.litter.Recipe");
  }
  return in_range(out, error);
}
std::string canonical_json(const Recipe& p) {
  return write_json(schema::to_json(p), {.pretty = false});
}
u64 source_hash(const Recipe& p) {
  const auto text = canonical_json(p);
  u64 h = hash_combine(hash_bytes(text.data(), text.size()), k_litter_version);
  if (p.kind == Kind::twig || p.kind == Kind::tangle)
    h = hash_combine(h, deadwood::source_hash(wood_recipe(p)));
  return h;
}
deadwood::DeadwoodFile wood_recipe(const Recipe& p) {
  const auto d = profile(p.kind);
  deadwood::DeadwoodFile wood;
  wood.tree.species = "oak";
  wood.tree.tree.seed = p.seed;
  wood.tree.tree.height_m = std::max(.35f, p.length_m > 0 ? p.length_m : d.length_m);
  wood.tree.tree.trunk_radius_m = (p.width_m > 0 ? p.width_m : d.width_m) * .5f;
  wood.tree.tree.triangle_budget = 8000;
  wood.tree.tree.card_bake_px = 0;
  wood.tree.tree.impostor_px = 0;
  wood.seed = p.seed;
  wood.kind = p.kind == Kind::tangle ? deadwood::RemainKind::fallen : deadwood::RemainKind::log;
  wood.end_m = p.length_m > 0 ? p.length_m : d.length_m;
  wood.foliage = 0;
  wood.branches = p.kind == Kind::tangle ? 1.0f : 0.0f;
  wood.splinter_m = .005f;
  wood.triangle_budget = 100000;
  return wood;
}
bool solve(const Recipe& p, assets::MeshData& out, Report& report, std::string& error) {
  out = {};
  report = {};
  if (!in_range(p, error)) return false;
  const auto defaults = profile(p.kind);
  const f32 len = p.length_m > 0 ? p.length_m : defaults.length_m;
  const f32 width = p.width_m > 0 ? p.width_m : defaults.width_m;
  const u32 count = p.count ? p.count : defaults.count;
  const u32 budget = p.triangle_budget ? p.triangle_budget : defaults.budget;
  assets::MeshData particle;
  u32 segments = 0;
  if (p.kind == Kind::twig || p.kind == Kind::tangle) {
    flora::Motion motion;
    deadwood::Report wr;
    if (!deadwood::solve(wood_recipe(p), particle, motion, wr, error)) return false;
    // Material assignment below intentionally uses the litter wood role; positions/UVs/normals
    // are the deadwood solver's exact geometry, translated and yawed only.
  } else {
    for (u32 rung = 0; rung < 3; ++rung) {
      particle = {};
      if (p.kind == Kind::cone) {
        segments = 8 - rung * 2;
        cone(particle, len, width, segments);
      } else if (p.kind == Kind::needles) {
        segments = 3 - rung;
        needle(particle, len, width, segments);
      } else {
        segments = rung == 0 ? 12u : rung == 1 ? 6u : 3u;
        sheet(particle, p.kind, len, width, p.curl, segments);
      }
      if (static_cast<u64>(particle.indices.size() / 3) * count <= budget) break;
    }
  }
  const u64 triangles = static_cast<u64>(particle.indices.size() / 3) * count;
  if (triangles == 0 || triangles > budget)
    return fail(error, "triangle_budget cannot hold every particle");
  Vec3 lo{1e20f, 1e20f, 1e20f}, hi{-1e20f, -1e20f, -1e20f};
  for (const Vec3 v : particle.positions) {
    lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
    hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
  }
  const Vec3 anchor{(lo.x + hi.x) * .5f, lo.y, (lo.z + hi.z) * .5f};
  assets::MeshData result;
  for (u32 i = 0; i < count; ++i) {
    const u64 seed = hash_combine(p.seed, i);
    const f32 radius = p.radius_m * std::sqrt(draw(seed, 1));
    const f64 angle = static_cast<f64>(draw(seed, 2)) * k_tau,
              turn = static_cast<f64>(draw(seed, 3)) * k_tau;
    const f32 layer = p.depth_m * (1 - radius * radius / (p.radius_m * p.radius_m)) * draw(seed, 4);
    const Vec3 offset{radius * static_cast<f32>(det::cos(angle)), layer,
                      radius * static_cast<f32>(det::sin(angle))};
    const u32 base = result.positions.size();
    for (u32 v = 0; v < particle.positions.size(); ++v) {
      result.positions.push_back(yaw(particle.positions[v] - anchor, turn) + offset);
      result.normals.push_back(yaw(particle.normals[v], turn));
      result.uvs.push_back(particle.uvs[v]);
    }
    for (u32 index : particle.indices)
      result.indices.push_back(base + index);
  }
  assets::Material material;
  material.base_color =
      p.kind == Kind::needles ? Vec4{.22f, .12f, .045f, 1} : Vec4{.18f, .085f, .028f, 1};
  material.metallic = 0;
  material.roughness = .9f;
  material.double_sided = p.kind == Kind::leaf || p.kind == Kind::needles || p.kind == Kind::cone;
  result.materials.push_back(material);
  result.primitives.push_back({0, result.indices.size(), 0, -1});
  report = {count, static_cast<u32>(triangles), budget, segments};
  out = std::move(result);
  return true;
}
}  // namespace engine::litter
