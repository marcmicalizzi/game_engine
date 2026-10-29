#include "kit.h"
#include "maturity.h"
#include "species.h"

#include <domain/flora/roots.h>

#include <algorithm>
#include <limits>

namespace engine::flora {
namespace {
using namespace detail;
bool fail(std::string& error, const char* message) {
  error = std::string("roots: ") + message;
  return false;
}
bool between(f64 v, f64 low, f64 high) { return std::isfinite(v) && v >= low && v <= high; }
bool between(f32 v, f64 low, f64 high) { return between(static_cast<f64>(v), low, high); }
bool known(std::string_view name) {
  return is_tree_species(name) || name == "birch" || name == "willow" || name == "broadleaf" ||
         name == "conifer" || name == "orchard" || name == "ancient";
}
// Relative authored tendencies, not measured species constants (tree-surface-roots.md).
f64 tendency(std::string_view name) {
  if (name == "beech" || name == "cedar" || name == "willow") return 1.2;
  if (name == "pine" || name == "walnut" || name == "oak") return .8;
  return 1;
}
f64 ground_at(const Roots& p, f64 x, f64 z) {
  if (!p.ground) return 0;
  const auto& g = *p.ground;
  const f64 u = (x - static_cast<f64>(g.min_x_m)) / static_cast<f64>(g.spacing_m),
            v = (z - static_cast<f64>(g.min_z_m)) / static_cast<f64>(g.spacing_m);
  if (!(u >= 0 && v >= 0 && u <= g.columns - 1 && v <= g.rows - 1))
    return std::numeric_limits<f64>::quiet_NaN();
  const u32 ix = std::min(static_cast<u32>(u), g.columns - 2);
  const u32 iz = std::min(static_cast<u32>(v), g.rows - 2);
  const f64 a = lerp(static_cast<f64>(g.heights_m[iz * g.columns + ix]),
                     static_cast<f64>(g.heights_m[iz * g.columns + ix + 1]), u - ix);
  const f64 b = lerp(static_cast<f64>(g.heights_m[(iz + 1) * g.columns + ix]),
                     static_cast<f64>(g.heights_m[(iz + 1) * g.columns + ix + 1]), u - ix);
  return lerp(a, b, v - iz);
}
f64 reach_of(const Roots& p) {
  return static_cast<f64>(p.max_radius_m > 0 ? std::min(p.max_radius_m, p.crown_radius_m)
                                             : p.crown_radius_m);
}
bool room(const Roots& p, D3 at, f64 radius) {
  const f64 reach = reach_of(p);
  if (std::sqrt(at.x * at.x + at.z * at.z) + radius > reach) return false;
  if (p.bounds) {
    const auto& b = *p.bounds;
    if (at.x - radius < static_cast<f64>(b.min_x_m) ||
        at.x + radius > static_cast<f64>(b.max_x_m) ||
        at.z - radius < static_cast<f64>(b.min_z_m) || at.z + radius > static_cast<f64>(b.max_z_m))
      return false;
  }
  for (const auto& e : p.exclusions) {
    const f64 dx = at.x - static_cast<f64>(e.x_m), dz = at.z - static_cast<f64>(e.z_m),
              r = radius + static_cast<f64>(e.radius_m);
    if (dx * dx + dz * dz < r * r) return false;
  }
  return true;
}
struct Station {
  D3 point, side;
  f64 radius, vertical, floor, arc;
};
struct Root {
  Vector<Station> nodes;
  bool secondary = false;
};
// The canonical path resolution is independent of the mesh budget and LOD.
Root grow(const Roots& p, D3 start, f64 heading, f64 reach, f64 radius, bool secondary,
          u64 stream) {
  Root root;
  root.secondary = secondary;
  Rng rng = stream_of(p.seed, stream);
  const f64 bend = rng.symmetric() * .7;
  const f64 phase = rng.unit() * 2 * k_pi;
  const f64 step = p.ground ? std::min(.22, static_cast<f64>(p.ground->spacing_m) * .5) : .22;
  const u32 count = std::max(4u, static_cast<u32>(std::ceil(reach / step)));
  D3 at = start;
  f64 arc = 0;
  for (u32 i = 0; i <= count; ++i) {
    const f64 t = static_cast<f64>(i) / count;
    const f64 angle = heading + bend * smoothstep01(t) + .18 * det::sin(t * 7 + phase);
    const D3 direction{det::cos(angle), 0, det::sin(angle)};
    if (i != 0) {
      at = at + direction * (reach / count);
      arc += reach / count;
    }
    const D3 side{-direction.z, 0, direction.x};
    const f64 r = radius * (.04 + .96 * (1 - t) * (1 - t));
    const f64 basal = secondary ? 0 : (1 - t) * (1 - t) * (1 - t) * (1 - t);
    const f64 vertical = r * (1 + .8 * tendency(p.species) * basal);
    if (!room(p, at, r)) break;
    bool crossed = false;
    if (!root.nodes.empty()) {
      const auto& previous = root.nodes.back();
      const D3 delta = at - previous.point;
      const f64 length2 = delta.x * delta.x + delta.z * delta.z;
      for (const auto& e : p.exclusions) {
        const f64 along = length2 > 0
                              ? clamp01(((static_cast<f64>(e.x_m) - previous.point.x) * delta.x +
                                         (static_cast<f64>(e.z_m) - previous.point.z) * delta.z) /
                                        length2)
                              : 0;
        const f64 dx = previous.point.x + along * delta.x - static_cast<f64>(e.x_m);
        const f64 dz = previous.point.z + along * delta.z - static_cast<f64>(e.z_m);
        const f64 clearance = static_cast<f64>(e.radius_m) + std::max(r, previous.radius);
        if (dx * dx + dz * dz < clearance * clearance) crossed = true;
      }
    }
    if (crossed) break;
    f64 floor = ground_at(p, at.x, at.z);
    for (const f64 across : {-1.0, -.5, .5, 1.0}) {
      const f64 h = ground_at(p, at.x + side.x * r * across, at.z + side.z * r * across);
      if (!std::isfinite(h)) {
        floor = h;
        break;
      }
      floor = std::min(floor, h);
    }
    if (!std::isfinite(floor)) break;
    if (!root.nodes.empty() && std::abs(floor - root.nodes.back().floor) > 1.25 * (reach / count))
      break;
    root.nodes.push_back({at, side, r, vertical, floor, arc});
  }
  if (root.nodes.size() < 4) {
    root.nodes.clear();
    return root;
  }
  const f64 end = root.nodes.back().arc;
  for (u32 i = 0; i < root.nodes.size(); ++i) {
    auto& n = root.nodes[i];
    const f64 t = n.arc / end;
    const f64 tail = smoothstep01((t - .72) / .28);
    f64 floor = n.floor;
    if (i > 0 && i + 1 < root.nodes.size())
      floor =
          std::min(floor, (root.nodes[i - 1].floor + 2 * floor + root.nodes[i + 1].floor) * .25);
    const f64 waves = lerp(1.0, .2 + .9 * det::sin(t * 10 + phase), smoothstep01(t / .3));
    const f64 visible =
        n.radius * static_cast<f64>(p.exposure) * tendency(p.species) * 1.35 * waves;
    n.point.y = floor - n.vertical + visible * (1 - tail) - .025 * tail;
    if (!secondary) {
      const f64 collar = 1 - smoothstep01(t / .35);
      n.point.y += radius * .65 * collar;
      // Reinforcement above the axis: preserve the lower envelope and buried tail.
      const f64 span = std::min(end * .6, 3 * static_cast<f64>(p.trunk_radius_m));
      const f64 q = 1 - clamp01(n.arc / span);
      const f64 ridge = 2 * static_cast<f64>(p.trunk_radius_m) * q * q * q;
      n.vertical += ridge * .5;
      n.point.y += ridge * .5;
    }
    if (secondary && i == 0) n.point.y = start.y;
  }
  return root;
}
void triangles(Emitter& emit, Vector<u32>* coarse, u32 a, u32 b, u32 c, D3 normal) {
  const Vec3 cross_n = cross(emit.mesh.positions[b] - emit.mesh.positions[a],
                             emit.mesh.positions[c] - emit.mesh.positions[a]);
  if (dot(from_f32(cross_n), normal) < 0) std::swap(b, c);
  if (coarse) {
    coarse->push_back(a);
    coarse->push_back(b);
    coarse->push_back(c);
  } else
    emit.triangle(a, b, c);
}
void emit_root(const Root& root, u32 sides, Emitter& emit, RootReport& report) {
  const u32 first = emit.mesh.positions.size();
  for (u32 i = 0; i < root.nodes.size(); ++i) {
    const auto& n = root.nodes[i];
    for (u32 j = 0; j <= sides; ++j) {
      const f64 angle = static_cast<f64>(j % sides) * 2 * k_pi / sides;
      const f64 c = det::cos(angle), s = det::sin(angle);
      const auto& before = root.nodes[i > 0 ? i - 1 : i];
      const auto& after = root.nodes[i + 1 < root.nodes.size() ? i + 1 : i];
      // Differentiate the actual fitted surface, including slope and taper.
      const D3 along = after.point - before.point +
                       (after.side * after.radius - before.side * before.radius) * c +
                       k_up * ((after.vertical - before.vertical) * s);
      const D3 around = n.side * (-n.radius * s) + k_up * (n.vertical * c);
      const D3 normal = cross(along, around);
      emit.vertex(n.point + n.side * (n.radius * c) + k_up * (n.vertical * s), normal,
                  static_cast<f64>(j) / sides * 2 * k_pi * n.radius, n.arc);
    }
  }
  const auto& front = root.nodes.front();
  const auto& back = root.nodes.back();
  const D3 front_dir = normalized(front.point - root.nodes[1].point);
  const D3 back_dir = normalized(back.point - root.nodes[root.nodes.size() - 2].point);
  const u32 cap0 = emit.vertex(front.point, front_dir, 0, 0);
  const u32 cap1 = emit.vertex(back.point, back_dir, 0, back.arc);
  const u32 fine_first = emit.mesh.indices.size(), coarse_first = report.coarse_indices.size();
  for (u32 pass = 0; pass < 2; ++pass) {
    Vector<u32>* indices = pass == 0 ? nullptr : &report.coarse_indices;
    const u32 stride = pass == 0 ? 1 : sides / 4;
    for (u32 i = 0; i + 1 < root.nodes.size(); ++i) {
      for (u32 j = 0; j < sides; j += stride) {
        const u32 a = first + i * (sides + 1) + j, b = a + stride;
        const u32 c = a + sides + 1, d = c + stride;
        const f64 angle = (j + stride * .5) * 2 * k_pi / sides;
        const D3 normal = root.nodes[i].side * det::cos(angle) + k_up * det::sin(angle);
        triangles(emit, indices, a, b, c, normal);
        triangles(emit, indices, b, d, c, normal);
      }
    }
    const u32 last = first + (root.nodes.size() - 1) * (sides + 1);
    for (u32 j = 0; j < sides; j += stride) {
      triangles(emit, indices, cap0, first + j, first + j + stride, front_dir);
      triangles(emit, indices, cap1, last + j, last + j + stride, back_dir);
    }
  }
  for (u32 i = fine_first; i < emit.mesh.indices.size(); i += 3)
    report.fine_element.push_back(report.elements);
  for (u32 i = coarse_first; i < report.coarse_indices.size(); i += 3)
    report.coarse_element.push_back(report.elements);
  ++report.elements;
  report.buried_tips.push_back(to_f32(back.point + k_up * back.vertical));
  for (u32 i = 0; i + 1 < root.nodes.size(); ++i) {
    const auto& a = root.nodes[i];
    const auto& b = root.nodes[i + 1];
    if (std::max(a.point.y + a.vertical - a.floor, b.point.y + b.vertical - b.floor) > .001)
      report.exposed.push_back(
          {to_f32(a.point), to_f32(b.point), static_cast<f32>(std::max(a.radius, b.radius))});
    report.lod_error =
        std::max(report.lod_error, static_cast<f32>(.3 * std::max(a.radius, a.vertical)));
  }
}
bool collar_face_fits(const Roots& p, D3 a, D3 b, D3 c) {
  for (const auto& e : p.exclusions) {
    const D3 centre = from_f32({e.x_m, 0, e.z_m});
    const D3 vertices[]{a, b, c};
    f64 signs[3]{};
    for (u32 i = 0; i < 3; ++i) {
      const D3 from = vertices[i], to = vertices[(i + 1) % 3];
      const f64 dx = to.x - from.x, dz = to.z - from.z;
      const f64 length2 = dx * dx + dz * dz;
      const f64 t = length2 > 0
                        ? clamp01(((centre.x - from.x) * dx + (centre.z - from.z) * dz) / length2)
                        : 0;
      const f64 ex = from.x + t * dx - centre.x, ez = from.z + t * dz - centre.z;
      if (ex * ex + ez * ez < static_cast<f64>(e.radius_m) * static_cast<f64>(e.radius_m))
        return false;
      signs[i] = dx * (centre.z - from.z) - dz * (centre.x - from.x);
    }
    // Nondegenerate projected triangles cannot bridge over an exclusion.
    const f64 area = (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
    if (std::abs(area) > 1e-12 && ((signs[0] >= 0 && signs[1] >= 0 && signs[2] >= 0) ||
                                   (signs[0] <= 0 && signs[1] <= 0 && signs[2] <= 0)))
      return false;
  }
  return true;
}
struct Collar {
  Vector<D3> points, normals;
  Vector<Vec2> uv;
  u32 sides = 0, rows = 0;
};
Collar make_collar(const Roots& p, const Vector<Root>& roots) {
  Collar out;
  if (!p.stem) return out;
  const auto& stem = *p.stem;
  out.sides = stem.sides;
  out.rows = stem.rows;
  const f64 r = static_cast<f64>(p.trunk_radius_m);
  for (u32 row = 0; row < stem.rows; ++row) {
    D3 centre{};
    for (u32 j = 0; j < stem.sides; ++j) {
      const auto& a = stem.samples[row * (stem.sides + 1) + j];
      centre = centre + from_f32({a.x, a.y, a.z}) * (1.0 / stem.sides);
    }
    const f64 t = static_cast<f64>(row) / (stem.rows - 1);
    const f64 low = (1 - t) * (1 - t) * (1 - t);
    for (u32 j = 0; j <= stem.sides; ++j) {
      const auto& a = stem.samples[row * (stem.sides + 1) + j];
      const D3 radial =
          normalized(D3{static_cast<f64>(a.x) - centre.x, 0, static_cast<f64>(a.z) - centre.z});
      f64 lobe = 0;
      for (const Root& root : roots) {
        if (root.secondary) continue;
        const auto& n = root.nodes[2];
        const D3 direction = normalized(D3{n.point.x, 0, n.point.z});
        const f64 d = std::max(0.0, dot(radial, direction));
        const f64 d2 = d * d, d4 = d2 * d2;
        lobe = std::max(lobe, d4 * d4);
      }
      // Slightly embed the rim; flare and its derivative vanish at the top.
      D3 point = from_f32({a.x, a.y, a.z}) + radial * (r * (.12 + .9 * lobe) * low - .002 * r);
      const f64 floor = ground_at(p, point.x, point.z);
      if (!room(p, point, 0) || !std::isfinite(floor)) return {};
      const f64 foot = floor - .05 - .15 * r;
      point.y = lerp(foot, point.y, smoothstep01(t));
      out.points.push_back(point);
      out.normals.push_back(normalized(from_f32({a.nx, a.ny, a.nz})));
      out.uv.push_back({a.u, a.v});
    }
  }
  for (u32 row = 0; row + 1 < stem.rows; ++row) {
    for (u32 j = 0; j < stem.sides; ++j) {
      const u32 a = row * (stem.sides + 1) + j, b = a + 1;
      const u32 c = a + stem.sides + 1, d = c + 1;
      if (!collar_face_fits(p, out.points[a], out.points[c], out.points[b]) ||
          !collar_face_fits(p, out.points[b], out.points[c], out.points[d]))
        return {};
    }
    for (u32 j = 0; j <= stem.sides; ++j) {
      const u32 col = j % stem.sides, index = row * (stem.sides + 1) + j;
      const u32 lo = (row > 0 ? row - 1 : row) * (stem.sides + 1);
      const u32 hi = (row + 1) * (stem.sides + 1);
      const D3 along = out.points[hi + col] - out.points[lo + col];
      const u32 start = row * (stem.sides + 1);
      const D3 around = out.points[start + (col + 1) % stem.sides] -
                        out.points[start + (col + stem.sides - 1) % stem.sides];
      D3 normal = normalized(cross(along, around));
      if (dot(normal, out.normals[index]) < 0) normal = normal * -1;
      const f64 blend = smoothstep01((static_cast<f64>(row) / (stem.rows - 1) - .65) / .35);
      out.normals[index] = normalized(normal * (1 - blend) + out.normals[index] * blend);
    }
  }
  return out;
}
void emit_collar(const Collar& collar, Emitter& emit, RootReport& report) {
  if (collar.points.empty()) return;
  const u32 first = emit.mesh.positions.size();
  for (u32 i = 0; i < collar.points.size(); ++i)
    emit.vertex(collar.points[i], collar.normals[i], static_cast<f64>(collar.uv[i].x),
                static_cast<f64>(collar.uv[i].y));
  for (u32 row = 0; row + 1 < collar.rows; ++row) {
    for (u32 j = 0; j < collar.sides; ++j) {
      const u32 a = first + row * (collar.sides + 1) + j, b = a + 1;
      const u32 c = a + collar.sides + 1, d = c + 1;
      const D3 normal = collar.normals[row * (collar.sides + 1) + j];
      triangles(emit, nullptr, a, c, b, normal);
      triangles(emit, nullptr, b, c, d, normal);
      triangles(emit, &report.coarse_indices, a, c, b, normal);
      triangles(emit, &report.coarse_indices, b, c, d, normal);
      for (u32 k = 0; k < 2; ++k) {
        report.fine_element.push_back(report.elements);
        report.coarse_element.push_back(report.elements);
      }
      report.collar_triangles += 2;
    }
  }
  ++report.elements;
  D3 centre{};
  for (u32 j = 0; j < collar.sides; ++j)
    centre = centre + collar.points[j] * (1.0 / collar.sides);
  f64 reach = 0;
  for (const D3 point : collar.points) {
    const D3 offset{point.x - centre.x, 0, point.z - centre.z};
    reach = std::max(reach, length_of(offset));
  }
  report.exposed.push_back({to_f32(centre), to_f32(centre), static_cast<f32>(reach)});
}
}  // namespace

bool roots_in_range(const Roots& p, std::string& error) {
  if (!p.enabled) return true;
  if (!known(p.species)) return fail(error, "unsupported woody species");
  if (!between(p.trunk_radius_m, .003, 12.5) || !between(p.crown_radius_m, .1, 128) ||
      !between(p.max_radius_m, 0, 128) || !between(static_cast<f64>(p.exposure), 0, 1) ||
      (p.primary_count != 0 && (p.primary_count < 3 || p.primary_count > 16)) ||
      p.triangle_budget < 128 || p.triangle_budget > 250000)
    return fail(error, "dimensions, exposure, primary_count or triangle_budget out of range");
  if (p.bounds) {
    const auto& b = *p.bounds;
    if (!between(static_cast<f64>(b.min_x_m), -256, 256) ||
        !between(static_cast<f64>(b.min_z_m), -256, 256) ||
        !between(static_cast<f64>(b.max_x_m), -256, 256) ||
        !between(static_cast<f64>(b.max_z_m), -256, 256) ||
        !(static_cast<f64>(b.min_x_m) < static_cast<f64>(b.max_x_m) &&
          static_cast<f64>(b.min_z_m) < static_cast<f64>(b.max_z_m)))
      return fail(error, "invalid planting bounds");
  }
  if (p.exclusions.size() > 256) return fail(error, "at most 256 exclusions per root site");
  for (const auto& e : p.exclusions)
    if (!between(static_cast<f64>(e.x_m), -256, 256) ||
        !between(static_cast<f64>(e.z_m), -256, 256) ||
        !between(static_cast<f64>(e.radius_m), .001, 256))
      return fail(error, "invalid exclusion");
  if (p.stem) {
    const auto& stem = *p.stem;
    if (stem.sides < 8 || stem.sides > 128 || stem.rows < 3 || stem.rows > 33 ||
        stem.samples.size() != (stem.sides + 1) * stem.rows)
      return fail(error, "invalid resolved stem dimensions");
    for (u32 row = 0; row < stem.rows; ++row) {
      const auto& first = stem.samples[row * (stem.sides + 1)];
      const auto& last = stem.samples[(row + 1) * (stem.sides + 1) - 1];
      if (std::abs(first.x - last.x) + std::abs(first.y - last.y) + std::abs(first.z - last.z) >
          .0001f)
        return fail(error, "resolved stem ring is not closed");
      if (row > 0 && !(first.y > stem.samples[(row - 1) * (stem.sides + 1)].y))
        return fail(error, "resolved stem rings must rise monotonically");
      f64 area = 0;
      for (u32 j = 0; j < stem.sides; ++j) {
        const auto& a = stem.samples[row * (stem.sides + 1) + j];
        const auto& b = stem.samples[row * (stem.sides + 1) + j + 1];
        if (std::abs(a.y - first.y) > .0001f)
          return fail(error, "resolved stem rings must be horizontal");
        area += static_cast<f64>(a.x) * static_cast<f64>(b.z) -
                static_cast<f64>(b.x) * static_cast<f64>(a.z);
      }
      if (!(area > 1e-8)) return fail(error, "resolved stem ring has no positive area");
    }
    for (const auto& a : stem.samples) {
      if (!between(a.x, -64, 64) || !between(a.y, -64, 64) || !between(a.z, -64, 64) ||
          !between(a.nx, -1, 1) || !between(a.ny, -1, 1) || !between(a.nz, -1, 1) ||
          !between(a.u, -1000, 1000) || !between(a.v, -1000, 1000) ||
          a.nx * a.nx + a.ny * a.ny + a.nz * a.nz < .5f)
        return fail(error, "nonfinite or invalid resolved stem sample");
    }
  }
  if (p.ground) {
    const auto& g = *p.ground;
    if (g.columns < 2 || g.rows < 2 || g.columns > 513 || g.rows > 513 ||
        !between(static_cast<f64>(g.spacing_m), .03125, 4) ||
        !between(static_cast<f64>(g.min_x_m), -512, 512) ||
        !between(static_cast<f64>(g.min_z_m), -512, 512) ||
        g.heights_m.size() != g.columns * g.rows)
      return fail(error, "invalid ground grid (2..513 rows/columns, 0.03125..4 m spacing)");
    for (const f32 h : g.heights_m)
      if (!between(h, -1000, 1000)) return fail(error, "nonfinite or out-of-range ground height");
  }
  return true;
}

bool solve_roots(const Roots& p, assets::MeshData& mesh, Motion& motion, RootReport& report,
                 std::string& error) {
  mesh = {};
  motion = {};
  report = {};
  if (!roots_in_range(p, error)) return false;
  if (!p.enabled || static_cast<f64>(p.exposure) == 0) return true;
  const f64 size = smoothstep01((static_cast<f64>(p.trunk_radius_m) - .04) / .21);
  if (size == 0) return true;
  Vector<Root> roots;
  const u32 count = p.primary_count != 0 ? p.primary_count : 5 + static_cast<u32>(4 * size);
  Rng rng = stream_of(p.seed, 9100);
  const f64 turn = rng.unit() * 2 * k_pi;
  for (u32 i = 0; i < count; ++i) {
    const f64 angle = turn + (i + .3 * rng.symmetric()) * 2 * k_pi / count;
    const D3 start{det::cos(angle) * static_cast<f64>(p.trunk_radius_m) * .55, 0,
                   det::sin(angle) * static_cast<f64>(p.trunk_radius_m) * .55};
    const f64 length = reach_of(p) * (.5 + .45 * rng.unit()) * size;
    const f64 radius = static_cast<f64>(p.trunk_radius_m) * (.33 + .12 * rng.unit()) * size;
    Root root = grow(p, start, angle, length, radius, false, 9200 + i);
    if (root.nodes.empty()) continue;
    ++report.primary;
    const u32 fork = static_cast<u32>(root.nodes.size() * (.3 + .15 * rng.unit()));
    const auto& n = root.nodes[fork];
    const f64 heading = angle + (i % 2 == 0 ? .65 : -.65);
    Root lateral = grow(p, n.point, heading, length * .45, n.radius * .55, true, 9300 + i);
    roots.push_back(std::move(root));
    if (!lateral.nodes.empty()) {
      roots.push_back(std::move(lateral));
      ++report.secondary;
    }
  }
  if (roots.empty()) return true;
  u32 quads = 0;
  for (const auto& r : roots)
    quads += r.nodes.size();  // rows-1 sides + two half cap fans
  const Collar collar = make_collar(p, roots);
  const u32 collar_cost = collar.points.empty() ? 0 : (collar.rows - 1) * collar.sides * 2;
  const u32 sides = quads * 16 + collar_cost <= p.triangle_budget ? 8 : 4;
  if (quads * sides * 2 + collar_cost > p.triangle_budget)
    return fail(
        error,
        "triangle_budget cannot preserve terrain stations; increase budget or constrain reach");
  Emitter emit{mesh, motion};
  begin_structural_part(emit, {}, k_up, 1, 0xFFFFFFFFu, 0, 0, 0);
  for (const auto& r : roots)
    emit_root(r, sides, emit, report);
  emit_collar(collar, emit, report);
  mesh.materials.push_back(material("root bark", {.24f, .17f, .10f, 1}, .9f, false));
  assets::Primitive primitive;
  primitive.first_index = 0;
  primitive.index_count = mesh.indices.size();
  primitive.material = 0;
  mesh.primitives.push_back(primitive);
  report.triangles = mesh.indices.size() / 3;
  return true;
}

bool roots_for_tree(const FloraFile& tree, Roots& roots, std::string& error, bool bind_stem) {
  if (is_tree_species(tree.species)) {
    if (!tree_root_dimensions(tree.species, tree.tree, roots.trunk_radius_m, roots.crown_radius_m,
                              error))
      return false;
    roots.seed = tree.tree.seed;
  } else {
    FloraFile f;
    if (!read_flora_file(canonical_flora_json(tree), f, error)) return false;
    FloraFile r;
    if (!resolve_legacy_maturity(f, r, error)) return false;
    if (f.species == "broadleaf") {
      const auto& b = r.broadleaf;
      roots.trunk_radius_m =
          b.trunk_radius_m > 0
              ? b.trunk_radius_m
              : static_cast<f32>(.35 * det::exp(det::log(static_cast<f64>(b.height_m) / 12.0) /
                                                static_cast<f64>(b.allometry_exponent)));
      roots.crown_radius_m = b.height_m * b.limb_length;
      roots.seed = b.seed;
    } else if (f.species == "conifer") {
      roots.trunk_radius_m = r.conifer.trunk_radius_m;
      roots.crown_radius_m = r.conifer.crown_radius_m;
      roots.seed = r.conifer.seed;
    } else if (f.species == "birch") {
      roots.trunk_radius_m = r.birch.stem_radius_m;
      roots.crown_radius_m = r.birch.height_m * r.birch.limb_length;
      roots.seed = r.birch.seed;
    } else if (f.species == "willow") {
      roots.trunk_radius_m = r.willow.trunk_radius_m;
      roots.crown_radius_m = r.willow.height_m * r.willow.limb_length;
      roots.seed = r.willow.seed;
    } else if (f.species == "orchard") {
      roots.trunk_radius_m = r.orchard.trunk_radius_m;
      roots.crown_radius_m = r.orchard.height_m * r.orchard.limb_length;
      roots.seed = r.orchard.seed;
    } else if (f.species == "ancient") {
      roots.trunk_radius_m = r.ancient.trunk_radius_m;
      roots.crown_radius_m = r.ancient.height_m * .35f;
      roots.seed = r.ancient.seed;
    } else
      return fail(error, "tree has no supported structural root profile");
  }
  roots.species = tree.species;
  roots.stem.reset();
  if (!roots_in_range(roots, error)) return false;
  if (!bind_stem || !roots.enabled || static_cast<f64>(roots.exposure) == 0 ||
      static_cast<f64>(roots.trunk_radius_m) <= .04)
    return true;
  assets::MeshData mesh;
  Motion motion;
  if (!solve_flora(tree, mesh, motion, nullptr, nullptr, error)) return false;
  return fit_root_stem(mesh, motion, roots, error);
}

bool sample_root_ground(const scene_gen::Ground& ground, WorldPos origin, f32 yaw, f32 scale,
                        f32 reach, f32 spacing, RootGround& out, std::string& error) {
  out = {};
  if (!between(origin.x, -1e11, 1e11) || !between(origin.y, -1e6, 1e6) ||
      !between(origin.z, -1e11, 1e11) || !between(yaw, -1e6, 1e6) || !between(scale, .01, 100) ||
      !between(reach, .1, 128) || !between(spacing, .03125, 4))
    return fail(error, "invalid terrain sampling transform or dimensions");
  const u32 cells =
      static_cast<u32>(std::ceil(2.0 * static_cast<f64>(reach) / static_cast<f64>(spacing)));
  if (cells > 512) return fail(error, "terrain patch exceeds 513 samples per axis");
  out.columns = out.rows = cells + 1;
  out.spacing_m = spacing;
  out.min_x_m = out.min_z_m = -reach;
  out.heights_m.reserve(out.columns * out.rows);
  const f64 c = det::cos(static_cast<f64>(yaw)), s = det::sin(static_cast<f64>(yaw));
  for (u32 z = 0; z < out.rows; ++z)
    for (u32 x = 0; x < out.columns; ++x) {
      const f64 lx = -static_cast<f64>(reach) + static_cast<f64>(x) * static_cast<f64>(spacing);
      const f64 lz = -static_cast<f64>(reach) + static_cast<f64>(z) * static_cast<f64>(spacing);
      const WorldPos world{origin.x + static_cast<f64>(scale) * (c * lx + s * lz), origin.y,
                           origin.z + static_cast<f64>(scale) * (-s * lx + c * lz)};
      const f64 height = (ground.surface(world) - origin.y) / static_cast<f64>(scale);
      if (!between(height, -1000, 1000)) {
        out = {};
        return fail(error, "terrain query returned invalid height");
      }
      out.heights_m.push_back(static_cast<f32>(height));
    }
  return true;
}

bool root_covers(const RootReport& report, f32 x, f32 z) noexcept {
  for (const auto& span : report.exposed) {
    const D3 from = from_f32(span.from), to = from_f32(span.to);
    const f64 dx = to.x - from.x, dz = to.z - from.z;
    const f64 length2 = dx * dx + dz * dz;
    const f64 t =
        length2 > 0
            ? clamp01(((static_cast<f64>(x) - from.x) * dx + (static_cast<f64>(z) - from.z) * dz) /
                      length2)
            : 0;
    const f64 ex = static_cast<f64>(x) - from.x - t * dx,
              ez = static_cast<f64>(z) - from.z - t * dz;
    if (ex * ex + ez * ez <= static_cast<f64>(span.radius) * static_cast<f64>(span.radius))
      return true;
  }
  return false;
}
}  // namespace engine::flora
