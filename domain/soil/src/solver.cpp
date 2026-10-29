#include "detail.h"

#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <domain/rocks/rocks.h>
#include <domain/soil/soil.h>

#include <array>

namespace engine::soil {
// Embedded stones and plates are part of this solver's output/cache contract. Do not
// silently change them when a dependency moves: review captures/pins and bump soil's
// version together before accepting a new rock solver.
static_assert(rocks::k_rocks_version == 8,
              "Review soil output and bump k_soil_version before changing its rock dependency");
namespace {
constexpr f64 k_pi = 3.14159265358979323846;
f32 random(u64 seed, u32 i) { return static_cast<f32>(mix64(seed + i) >> 40) / 16777216.0f; }
f32 sine(f64 a) { return static_cast<f32>(det::sin(a)); }
f32 cosine(f64 a) { return static_cast<f32>(det::cos(a)); }
struct Builder {
  assets::MeshData mesh;
  std::array<Vector<u32>, 6> indices;
  void triangle(Vec3 a, Vec3 b, Vec3 c, u32 material) {
    Vec3 n = cross(b - a, c - a);
    if (dot(n, n) < 1e-16f) return;
    n = normalize(n);
    const u32 first = static_cast<u32>(mesh.positions.size());
    for (Vec3 p : {a, b, c}) {
      mesh.positions.push_back(p);
      mesh.normals.push_back(n);
      // Dominant-axis metres, bounded by the recipe's local extent.
      Vec2 uv = std::abs(n.y) > 0.7f   ? Vec2{p.x, p.z}
                : std::abs(n.z) > 0.7f ? Vec2{p.x, p.y}
                                       : Vec2{p.z, p.y};
      mesh.uvs.push_back(uv);
    }
    indices[material].push_back(first);
    indices[material].push_back(first + 1);
    indices[material].push_back(first + 2);
  }
  void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, u32 material, bool reverse = false) {
    if (reverse) {
      triangle(a, b, c, material);
      triangle(a, c, d, material);
    } else {
      triangle(a, c, b, material);
      triangle(a, d, c, material);
    }
  }
  void finish() {
    constexpr Vec4 k_colours[] = {{0.055f, 0.035f, 0.018f, 1}, {0.14f, 0.075f, 0.035f, 1},
                                  {0.32f, 0.19f, 0.09f, 1},    {0.44f, 0.32f, 0.19f, 1},
                                  {0.16f, 0.09f, 0.04f, 1},    {0.32f, 0.30f, 0.27f, 1}};
    for (u32 m = 0; m < 6; ++m) {
      assets::Material material;
      material.base_color = k_colours[m];
      material.roughness = 0.95f;
      mesh.materials.push_back(material);
      if (indices[m].empty()) continue;
      mesh.primitives.push_back({static_cast<u32>(mesh.indices.size()),
                                 static_cast<u32>(indices[m].size()), static_cast<i32>(m)});
      for (u32 index : indices[m])
        mesh.indices.push_back(index);
    }
  }
};
u32 layer(const SoilFile& f, f32 depth) {
  return depth < f.litter_m                 ? 0u
         : depth < f.litter_m + f.topsoil_m ? 1u
         : depth < f.depth_m * 0.75f        ? 2u
                                            : 3u;
}
void tube(Builder& b, const Vector<Vec3>& points, f32 radius, u32 sides) {
  if (points.size() < 2) return;
  Vector<Vec3> rings;
  rings.reserve(points.size() * sides);
  for (u32 j = 0; j < points.size(); ++j) {
    const Vec3 direction =
        normalize(points[std::min<u32>(j + 1, points.size() - 1)] - points[j ? j - 1 : 0]);
    const Vec3 axis =
        normalize(cross(direction, std::abs(direction.y) < 0.85f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
    const Vec3 other = cross(direction, axis);
    const f32 t = static_cast<f32>(j) / static_cast<f32>(points.size() - 1);
    const f32 r = radius * (1 - 0.94f * t);
    for (u32 i = 0; i < sides; ++i) {
      const f64 angle = 2 * k_pi * i / sides;
      rings.push_back(points[j] + (axis * cosine(angle) + other * sine(angle)) * r);
    }
  }
  for (u32 j = 0; j + 1 < points.size(); ++j)
    for (u32 i = 0; i < sides; ++i) {
      const u32 next = (i + 1) % sides;
      b.quad(rings[j * sides + i], rings[(j + 1) * sides + i], rings[(j + 1) * sides + next],
             rings[j * sides + next], 4);
    }
  // Cap both ends: root tips are broken ends, not uncapped tubes.
  for (u32 i = 0; i < sides; ++i) {
    b.triangle(points.front(), rings[(i + 1) % sides], rings[i], 4);
    const u32 last = (points.size() - 1) * sides;
    b.triangle(points.back(), rings[last + i], rings[last + (i + 1) % sides], 4);
  }
}
void bank(Builder& b, const SoilFile& f, f32 cell, f32 origin_z, f32 facing) {
  const u32 n = std::max(4u, static_cast<u32>(std::ceil(f.width_m / cell)));
  Vector<f32> rows{0, f.litter_m, f.litter_m + f.topsoil_m, f.depth_m * 0.75f, f.depth_m + 0.04f};
  for (f32 y = cell; y < f.depth_m; y += cell)
    rows.push_back(y);
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  // A closed extrusion. The top, lower face, back and ends share exact positions.
  const auto section = [&](f32 x) {
    Vector<Vec3> p;
    const f32 fade = std::max(0.05f, detail::taper(f, x)), back = detail::back(f);
    const auto point = [&](f32 z, f32 depth) {
      const f32 world_z = origin_z + facing * z;
      return Vec3{x, ground_height(f.ground, x, world_z) - depth * fade, world_z};
    };
    p.push_back(point(-back, 0));
    for (f32 depth : rows) {
      const f32 v = std::min(depth / f.depth_m, 1.0f);
      // A lower recession with a continuous intact upper lip. The restrained corrugation
      // stands for soil aggregates and small rills, not a simulated erosion rate.
      const f32 notch = detail::recess(f) * sine(k_pi * static_cast<f64>(v));
      const f32 relief = 0.025f * sine(static_cast<f64>(x) * 7.0 + static_cast<f64>(f.seed % 31)) *
                         sine(k_pi * static_cast<f64>(v)) * fade;
      p.push_back(point(-notch + relief, depth));
    }
    p.push_back(point(-back, rows.back()));
    return p;
  };
  Vector<Vec3> previous = section(-f.width_m * 0.5f);
  const auto cap = [&](const Vector<Vec3>& p, bool reverse) {
    const f32 x = p.front().x, z = origin_z - facing * (detail::back(f) - 0.1f);
    const Vec3 centre{x, ground_height(f.ground, x, z) - (f.depth_m + 0.04f) * 0.025f, z};
    for (u32 k = 0; k < p.size(); ++k) {
      const Vec3 a = p[k], c = p[(k + 1) % p.size()];
      if (reverse)
        b.triangle(centre, c, a, 3);
      else
        b.triangle(centre, a, c, 3);
    }
  };
  cap(previous, facing > 0);
  for (u32 i = 1; i <= n; ++i) {
    const f32 x = -f.width_m * 0.5f + f.width_m * static_cast<f32>(i) / static_cast<f32>(n);
    auto current = section(x);
    for (u32 k = 0; k < current.size(); ++k) {
      const u32 next = (k + 1) % static_cast<u32>(current.size());
      const u32 material = k == 0                 ? 0u
                           : k <= rows.size() - 1 ? layer(f, (rows[k - 1] + rows[k]) * 0.5f)
                                                  : 3u;
      b.quad(previous[k], current[k], current[next], previous[next], material, facing < 0);
    }
    previous = std::move(current);
  }
  cap(previous, facing < 0);
}
bool rock(Builder& b, const SoilFile& f, Vec3 position, Vec3 size, u64 seed, u32 budget, bool soil,
          std::string& error) {
  rocks::RockFile recipe;
  recipe.boulder = rocks::Boulder{};
  auto& p = *recipe.boulder;
  p.seed = seed;
  p.size_x = size.x;
  p.size_y = size.y;
  p.size_z = size.z;
  p.buried = 0;
  p.triangle_budget = budget;
  p.surface.strength = 0;
  p.geology.weathering = 0.8f;
  assets::MeshData mesh;
  if (!rocks::solve_rock(recipe, mesh, nullptr, &error)) return false;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    Vec3 a = mesh.positions[mesh.indices[i]] + position;
    Vec3 c = mesh.positions[mesh.indices[i + 2]] + position;
    Vec3 d = mesh.positions[mesh.indices[i + 1]] + position;
    // The lifted plate exposes mixed mineral soil rather than horizontal undisturbed horizons.
    const u32 material = soil ? ((a.z - position.z) > size.z * 0.4f ? 1u : 2u) : 5u;
    b.triangle(a, d, c, material);
  }
  (void)f;
  return true;
}
bool emit(const SoilFile& f, f32 cell, u32 sides, u32 plate_budget, Builder& b,
          std::string& error) {
  const auto h = [&](f32 x, f32 z) { return ground_height(f.ground, x, z); };
  if (f.kind == Kind::TreeThrow) {
    const f32 z = f.length_m * 0.5f, base = h(0, z);
    if (!rock(b, f, {0, base - 0.06f, z}, {f.width_m * 0.5f, f.plate_height_m * 0.5f, 0.22f},
              f.seed, plate_budget, true, error))
      return false;
    for (u32 i = 0; i < f.roots; ++i) {
      const f64 angle = 2 * k_pi * (i + 0.2 + 0.35 * static_cast<f64>(random(f.seed + 100, i))) /
                        std::max(f.roots, 1u);
      const Vec3 start{0, base + f.plate_height_m * 0.42f, z - 0.25f};
      Vector<Vec3> path;
      for (u32 j = 0; j <= 12; ++j) {
        const f32 t = static_cast<f32>(j) / 12;
        path.push_back(start + Vec3{cosine(angle) * f.width_m * 0.49f * t,
                                    sine(angle) * f.plate_height_m * 0.52f * t - 0.18f * t * t,
                                    -0.08f * sine(k_pi * static_cast<f64>(t)) - 0.3f * t * t});
      }
      tube(b, path, f.root_radius_m * (0.8f + 0.4f * random(f.seed + 200, i)), sides);
      for (u32 k = 0; k < 2; ++k) {
        const u32 joint = 5 + k * 3;
        Vector<Vec3> branch;
        const f32 sign = k ? 1.0f : -1.0f;
        for (u32 j = 0; j <= 6; ++j) {
          const f32 t = static_cast<f32>(j) / 6;
          branch.push_back(path[joint] + Vec3{sign * 0.35f * t, -0.45f * t * t, -0.15f * t});
        }
        tube(b, branch, f.root_radius_m * 0.45f, sides);
      }
    }
    Vector<Vec3> trunk;
    for (u32 j = 0; j <= 12; ++j) {
      const f32 t = static_cast<f32>(j) / 12, zz = z + 3 * t;
      trunk.push_back({0, h(0, zz) + f.mound_height_m * (1 - t) + 0.22f, zz});
    }
    tube(b, trunk, 0.24f, sides * 2);
  } else {
    if (f.kind == Kind::Gully) {
      bank(b, f, cell, -f.length_m * 0.5f, 1);
      bank(b, f, cell, f.length_m * 0.5f, -1);
    } else
      bank(b, f, cell, 0, 1);
    for (u32 i = 0; i < f.roots; ++i) {
      const f32 x = f.width_m * (random(f.seed + 300, i) * 0.8f - 0.4f);
      const f32 z = f.kind == Kind::Gully ? (i % 2 ? 1.0f : -1.0f) * f.length_m * 0.5f : 0.0f;
      const f32 facing = f.kind == Kind::Gully && i % 2 ? -1.0f : 1.0f;
      Vector<Vec3> path;
      for (u32 j = 0; j <= 12; ++j) {
        const f32 t = static_cast<f32>(j) / 12;
        const f32 xx = x + 0.16f * sine(static_cast<f64>(t * 4) + static_cast<f64>(i)) * t;
        const f32 zz = z + facing * (-0.35f + 0.65f * t);
        path.push_back({xx, h(xx, zz) - 0.04f - f.depth_m * 0.75f * t * t, zz});
      }
      tube(b, path, f.root_radius_m * (0.6f + 0.4f * random(f.seed + 400, i)), sides);
    }
  }
  for (u32 i = 0; i < f.stones; ++i) {
    const f32 x = (random(f.seed + 500, i) - 0.5f) * f.width_m * 0.7f;
    const f32 y = 0.2f + random(f.seed + 600, i) * 0.6f;
    f32 z = 0, vertical = 0;
    if (f.kind == Kind::TreeThrow) {
      z = f.length_m * 0.5f - 0.2f;
      vertical = h(x, z) + f.plate_height_m * y;
    } else {
      const f32 facing = f.kind == Kind::Gully && i % 2 ? -1.0f : 1.0f;
      z = (f.kind == Kind::Gully ? -facing * f.length_m * 0.5f : 0) -
          facing * detail::recess(f) * sine(k_pi * static_cast<f64>(y));
      vertical = h(x, z) - f.depth_m * y;
    }
    const f32 r = f.stone_radius_m * (0.8f + 0.2f * random(f.seed + 700, i));
    if (!rock(b, f, {x, vertical - r, z}, {r, r, r * 0.75f + 0.02f}, hash_combine(f.seed, i), 500,
              false, error))
      return false;
  }
  return true;
}
}  // namespace
bool solve(const SoilFile& file, assets::MeshData& out, Report& report, std::string& error) {
  out = {};
  report = {};
  if (!in_range(file, error)) return false;
  const u32 budget = triangle_budget(file);
  constexpr f32 k_cells[] = {0.08f, 0.125f, 0.2f, 0.35f, 0.6f};
  for (u32 rung = 0; rung < 5; ++rung) {
    Builder b;
    if (!emit(file, k_cells[rung], rung < 2 ? 8u : 4u, rung < 2 ? 5000u : 1200u, b, error))
      return false;
    b.finish();
    if (b.mesh.indices.size() / 3 <= budget) {
      report = {static_cast<u32>(b.mesh.indices.size() / 3), file.roots, file.stones, budget,
                k_cells[rung]};
      out = std::move(b.mesh);
      return true;
    }
  }
  error =
      "soil triangle_budget cannot hold all requested roots, stones and soil at the coarsest rung";
  return false;
}
}  // namespace engine::soil
