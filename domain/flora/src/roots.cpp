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
  const u32 sides = quads * 16 <= p.triangle_budget ? 8 : 4;
  if (quads * sides * 2 > p.triangle_budget)
    return fail(
        error,
        "triangle_budget cannot preserve terrain stations; increase budget or constrain reach");
  Emitter emit{mesh, motion};
  begin_structural_part(emit, {}, k_up, 1, 0xFFFFFFFFu, 0, 0, 0);
  for (const auto& r : roots)
    emit_root(r, sides, emit, report);
  mesh.materials.push_back(material("root bark", {.24f, .17f, .10f, 1}, .9f, false));
  assets::Primitive primitive;
  primitive.first_index = 0;
  primitive.index_count = mesh.indices.size();
  primitive.material = 0;
  mesh.primitives.push_back(primitive);
  report.triangles = mesh.indices.size() / 3;
  return true;
}

bool roots_for_tree(const FloraFile& tree, Roots& roots, std::string& error) {
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
  return roots_in_range(roots, error);
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
