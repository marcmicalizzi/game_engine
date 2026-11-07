// A terrain's holes (terrain_holes.h; docs/subsystems/terrain.md, "Holes"; roadmap W87).
#include <core/hash/hash.h>
#include <domain/scene_gen/terrain_holes.h>

#include <bit>
#include <cmath>

namespace engine::scene_gen {

namespace {

u64 mix(u64 h, f64 v) noexcept { return hash_combine(h, std::bit_cast<u64>(v)); }

}  // namespace

bool TerrainHoles::build(std::span<const scene::TerrainHole> holes, std::string* error) {
  regions_.clear();
  hash_ = 0;
  const auto fail = [&](const scene::TerrainHole& hole, u32 index, const char* why) {
    if (error != nullptr) {
      *error = "terrain hole " + std::to_string(index) +
               (hole.name.empty() ? std::string() : " (\"" + hole.name + "\")") + " " + why;
    }
    regions_.clear();
    hash_ = 0;
    return false;
  };
  u64 h = hash_bytes("engine.terrain.holes/1", 22);
  for (u32 k = 0; k < holes.size(); ++k) {
    const scene::TerrainHole& hole = holes[k];
    const bool polygon = !hole.polygon.empty();
    const bool circle = hole.radius_m > 0.0f;
    if (polygon && circle) return fail(hole, k, "names both a polygon and a radius; name one");
    if (!polygon && !circle)
      return fail(hole, k, "names neither a polygon of three points or more nor a radius above 0");
    if (polygon && hole.polygon.size() < 3)
      return fail(hole, k, "has a polygon of fewer than three points");
    Region r;
    if (circle) {
      if (!std::isfinite(hole.center.x) || !std::isfinite(hole.center.z) ||
          !std::isfinite(hole.radius_m))
        return fail(hole, k, "has a centre or radius that is not finite");
      r.circle = true;
      r.cx = hole.center.x * 1000.0;
      r.cz = hole.center.z * 1000.0;
      const f64 rm = static_cast<f64>(hole.radius_m) * 1000.0;
      r.r2 = rm * rm;
      r.x0 = r.cx - rm;
      r.x1 = r.cx + rm;
      r.z0 = r.cz - rm;
      r.z1 = r.cz + rm;
      h = mix(mix(mix(hash_combine(h, 1u), r.cx), r.cz), rm);
    } else {
      h = hash_combine(h, 2u);
      r.x0 = r.z0 = 1.0e300;
      r.x1 = r.z1 = -1.0e300;
      for (const WorldPos& p : hole.polygon) {
        if (!std::isfinite(p.x) || !std::isfinite(p.z))
          return fail(hole, k, "has a polygon point that is not finite");
        const f64 x = p.x * 1000.0;
        const f64 z = p.z * 1000.0;
        r.xs.push_back(x);
        r.zs.push_back(z);
        r.x0 = x < r.x0 ? x : r.x0;
        r.x1 = x > r.x1 ? x : r.x1;
        r.z0 = z < r.z0 ? z : r.z0;
        r.z1 = z > r.z1 ? z : r.z1;
        h = mix(mix(h, x), z);
      }
    }
    regions_.push_back(std::move(r));
  }
  hash_ = regions_.empty() ? 0 : h;
  return true;
}

// Strictly inside: a point on a circle or on a polygon's edge is outside, so a lattice point on a
// round hole's rim is ground. The polygon is the even-odd crossing rule, a half-open test on z so a
// vertex is counted once.
bool TerrainHoles::inside(const Region& r, f64 x, f64 z) noexcept {
  if (x <= r.x0 || x >= r.x1 || z <= r.z0 || z >= r.z1) return false;
  if (r.circle) {
    const f64 dx = x - r.cx;
    const f64 dz = z - r.cz;
    return dx * dx + dz * dz < r.r2;
  }
  bool in = false;
  const u32 n = r.xs.size();
  for (u32 a = 0, b = n - 1; a < n; b = a++) {
    const f64 za = r.zs[a];
    const f64 zb = r.zs[b];
    if ((za > z) == (zb > z)) continue;
    const f64 xa = r.xs[a];
    const f64 xb = r.xs[b];
    // The edge's x at z, compared without a division: (x - xa) (zb - za) against (xb - xa) (z -
    // za), its sign flipped with the edge's direction.
    const f64 lhs = (x - xa) * (zb - za);
    const f64 rhs = (xb - xa) * (z - za);
    if (zb > za ? lhs < rhs : lhs > rhs) in = !in;
  }
  return in;
}

bool TerrainHoles::contains_mm(f64 x_mm, f64 z_mm) const noexcept {
  for (const Region& r : regions_) {
    if (inside(r, x_mm, z_mm)) return true;
  }
  return false;
}

bool TerrainHoles::cell_mm(f64 x0, f64 z0, f64 x1, f64 z1) const noexcept {
  for (const Region& r : regions_) {
    if (x0 <= r.x0 || x1 >= r.x1 || z0 <= r.z0 || z1 >= r.z1) continue;
    if (inside(r, x0, z0) && inside(r, x1, z0) && inside(r, x0, z1) && inside(r, x1, z1))
      return true;
  }
  return false;
}

bool TerrainHoles::cell(i64 spacing_mm, i64 i, i64 j) const noexcept {
  const f64 s = static_cast<f64>(spacing_mm);
  return cell_mm(static_cast<f64>(i) * s, static_cast<f64>(j) * s, static_cast<f64>(i + 1) * s,
                 static_cast<f64>(j + 1) * s);
}

bool TerrainHoles::touches_mm(f64 x0, f64 z0, f64 x1, f64 z1) const noexcept {
  for (const Region& r : regions_) {
    if (x1 > r.x0 && x0 < r.x1 && z1 > r.z0 && z0 < r.z1) return true;
  }
  return false;
}

u64 TerrainHoles::window_hash(i64 spacing_mm, i64 i0, i64 j0, u32 nx, u32 nz, u32* cells) const {
  if (cells != nullptr) *cells = 0;
  if (regions_.empty()) return 0;
  const f64 s = static_cast<f64>(spacing_mm);
  if (!touches_mm(static_cast<f64>(i0) * s, static_cast<f64>(j0) * s, static_cast<f64>(i0 + nx) * s,
                  static_cast<f64>(j0 + nz) * s))
    return 0;
  u64 h = 0;
  u32 count = 0;
  for (u32 j = 0; j < nz; ++j) {
    for (u32 i = 0; i < nx; ++i) {
      if (!cell(spacing_mm, i0 + i, j0 + j)) continue;
      h = hash_combine(h, (u64{j} << 32) | i);
      ++count;
    }
  }
  if (cells != nullptr) *cells = count;
  return count == 0 ? 0 : hash_combine(h, hash_);
}

}  // namespace engine::scene_gen
