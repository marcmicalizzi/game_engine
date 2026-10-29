#include "surface.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/deadwood/deadwood.h>
#include <domain/geometry/flora_species.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace engine::deadwood {
namespace {
bool fail(std::string& error, std::string_view field) {
  error = "deadwood: " + std::string(field);
  return false;
}
bool between(f32 v, f32 a, f32 b) { return std::isfinite(v) && v >= a && v <= b; }
bool finite(Vec3 v) {
  return between(v.x, -1000, 1000) && between(v.y, -1000, 1000) && between(v.z, -1000, 1000);
}
f32 draw(u64 seed, u64 key) {
  u64 h = hash_combine(seed, key);
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return static_cast<f32>(h >> 40) * (1.0f / 16777216.0f);
}
f32 branch_share(const DeadwoodFile& p) {
  if (p.branches >= 0) return p.branches;
  if (p.kind == RemainKind::log || p.kind == RemainKind::stump) return 0;
  return p.kind == RemainKind::snag ? .35f : 1.0f;
}
f32 leaf_share(const DeadwoodFile& p) {
  if (p.foliage >= 0) return p.foliage;
  return p.kind == RemainKind::fallen || p.kind == RemainKind::limb ? 1.0f : 0.0f;
}
f32 pitch_of(const DeadwoodFile& p) {
  if (p.pitch_deg != -1) return p.pitch_deg;
  return p.kind == RemainKind::fallen || p.kind == RemainKind::log || p.kind == RemainKind::limb
             ? 90.0f
             : 0.0f;
}
// Explicit deterministic rotations; libc trigonometry never chooses geometry.
Vec3 turn_y(Vec3 v, f32 deg) {
  const f64 angle = static_cast<f64>(deg) * (3.14159265358979323846 / 180.0);
  const f32 c = static_cast<f32>(det::cos(angle)), s = static_cast<f32>(det::sin(angle));
  return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}
Vec3 turn_x(Vec3 v, f32 deg) {
  const f64 angle = static_cast<f64>(deg) * (3.14159265358979323846 / 180.0);
  const f32 c = static_cast<f32>(det::cos(angle)), s = static_cast<f32>(det::sin(angle));
  return {v.x, c * v.y - s * v.z, s * v.y + c * v.z};
}
struct Frame {
  Vec3 origin{}, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
  const DeadwoodFile& p;
  Vec3 local(Vec3 v) const {
    v = v - origin;
    return {dot(v, x), dot(v, y), dot(v, z)};
  }
  Vec3 pose(Vec3 v) const { return turn_y(turn_x(turn_y(v, p.roll_deg), pitch_of(p)), p.yaw_deg); }
  Vec3 unpose(Vec3 v) const {
    return turn_y(turn_x(turn_y(v, -p.yaw_deg), -pitch_of(p)), -p.roll_deg);
  }
};
f32 ground_at(const DeadwoodFile& p, Vec3 v) {
  if (!p.ground) return p.ground_y_m;
  const auto& g = *p.ground;
  const f32 x = (v.x - g.min_x_m) / g.spacing_m, z = (v.z - g.min_z_m) / g.spacing_m;
  if (!(x >= 0 && z >= 0 && x <= static_cast<f32>(g.columns - 1) &&
        z <= static_cast<f32>(g.rows - 1)))
    return std::numeric_limits<f32>::quiet_NaN();
  const u32 ix = std::min(static_cast<u32>(x), g.columns - 2);
  const u32 iz = std::min(static_cast<u32>(z), g.rows - 2);
  const f32 a =
      lerp(g.heights_m[iz * g.columns + ix], g.heights_m[iz * g.columns + ix + 1], x - ix);
  const f32 b = lerp(g.heights_m[(iz + 1) * g.columns + ix],
                     g.heights_m[(iz + 1) * g.columns + ix + 1], x - ix);
  return lerp(a, b, z - iz);
}
f32 support_gap(const Support& support, Vec3 p, Vec3& normal) {
  const Vec3 axis = support.to - support.from;
  const f32 t = clamp(dot(p - support.from, axis) / dot(axis, axis), 0, 1);
  const Vec3 offset = p - (support.from + axis * t);
  const f32 d = length(offset);
  normal = d > 1e-6f ? offset / d : Vec3{1, 0, 0};
  return d - support.radius_m;
}
f32 clearance(const DeadwoodFile& p, Vec3 v, Vec3& normal) {
  f32 gap = v.y - ground_at(p, v);
  normal = {0, 1, 0};
  for (const auto& support : p.supports) {
    Vec3 n;
    const f32 d = support_gap(support, v, n);
    if (d < gap) {
      gap = d;
      normal = n;
    }
  }
  return gap;
}
struct Vertex {
  Vec3 p, n;
  Vec2 uv;
  f32 w = 0;
};
Vertex blend(const Vertex& a, const Vertex& b, f32 t) {
  return {a.p + (b.p - a.p) * t, a.n + (b.n - a.n) * t, a.uv + (b.uv - a.uv) * t,
          lerp(a.w, b.w, t)};
}
template <class Distance>
void clip(Vector<Vertex>& polygon, Distance distance) {
  Vector<Vertex> next;
  if (polygon.empty()) return;
  Vertex a = polygon.back();
  f32 da = distance(a);
  for (const Vertex& b : polygon) {
    const f32 db = distance(b);
    if ((da >= 0) != (db >= 0)) {
      // Always interpolate from the lexicographically lesser endpoint, so a shared edge
      // has exactly the same intersection from either winding, including UV seams.
      if (std::tie(a.p.x, a.p.y, a.p.z) < std::tie(b.p.x, b.p.y, b.p.z))
        next.push_back(blend(a, b, da / (da - db)));
      else
        next.push_back(blend(b, a, db / (db - da)));
    }
    if (db >= 0) next.push_back(b);
    a = b;
    da = db;
  }
  polygon = std::move(next);
}
struct Triangle {
  Vertex a, b, c;
  u32 material;
  u32 part = 0;
};
void triangle(Vector<Triangle>& out, Vertex a, Vertex b, Vertex c, u32 material, u32 part = 0) {
  const Vec3 area = cross(b.p - a.p, c.p - a.p);
  const f32 squared = dot(area, area);
  if (squared <= 1e-18f) return;
  for (Vertex* v : {&a, &b, &c})
    if (dot(v->n, v->n) < .5f) v->n = area / std::sqrt(squared);
  out.push_back({a, b, c, material, part});
}
void bark_triangle(Vector<Triangle>& out, const DeadwoodFile& p, u32 wood_material, Vertex a,
                   Vertex b, Vertex c, u32 part, u32 depth = 0) {
  for (Vertex* v : {&a, &b, &c})
    v->w = detail::bark_field(v->uv, p.seed) - p.bark;
  const f32 lo = std::min({a.w, b.w, c.w}), hi = std::max({a.w, b.w, c.w});
  const auto span = [](Vec2 u, Vec2 v) {
    const Vec2 d = u - v;
    return d.x * d.x + d.y * d.y;
  };
  const f32 extent = std::max({span(a.uv, b.uv), span(b.uv, c.uv), span(c.uv, a.uv)});
  if (depth == 0 || (depth < 3 && lo < .04f && hi > 0 && extent > .0144f)) {
    const auto mid = [](const Vertex& x, const Vertex& y) {
      return std::tie(x.p.x, x.p.y, x.p.z) < std::tie(y.p.x, y.p.y, y.p.z) ? blend(x, y, .5f)
                                                                           : blend(y, x, .5f);
    };
    const Vertex ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
    bark_triangle(out, p, wood_material, a, ab, ca, part, depth + 1);
    bark_triangle(out, p, wood_material, ab, b, bc, part, depth + 1);
    bark_triangle(out, p, wood_material, ca, bc, c, part, depth + 1);
    bark_triangle(out, p, wood_material, ab, bc, ca, part, depth + 1);
    return;
  }
  Vector<Vertex> bark{a, b, c}, edge = bark, exposed = bark;
  clip(bark, [](const Vertex& v) { return -v.w; });
  clip(edge, [](const Vertex& v) { return v.w; });
  clip(edge, [](const Vertex& v) { return .04f - v.w; });
  clip(exposed, [](const Vertex& v) { return v.w - .04f; });
  const Vector<Vertex>* polygons[] = {&bark, &edge, &exposed};
  const u32 materials[] = {0, wood_material + 2, wood_material};
  for (u32 role = 0; role < 3; ++role)
    for (u32 k = 1; k + 1 < polygons[role]->size(); ++k)
      triangle(out, (*polygons[role])[0], (*polygons[role])[k], (*polygons[role])[k + 1],
               materials[role], part);
}
// Boundary matching is a topology operation on final positions, independent of UV/normal seams.
// A 10-micrometre key absorbs opposite-edge interpolation rounding; it never moves a vertex.
using Key = std::array<i64, 3>;
Key key(Vec3 p) {
  return {static_cast<i64>(std::floor(static_cast<f64>(p.x) * 100000 + .5)),
          static_cast<i64>(std::floor(static_cast<f64>(p.y) * 100000 + .5)),
          static_cast<i64>(std::floor(static_cast<f64>(p.z) * 100000 + .5))};
}
struct Edge {
  Key lo, hi;
  Vertex a, b;
};
Vector<Edge> boundaries(const Vector<Triangle>& triangles) {
  Vector<Edge> edges;
  for (const auto& t : triangles) {
    const Vertex v[] = {t.a, t.b, t.c};
    for (u32 k = 0; k < 3; ++k) {
      const Key a = key(v[k].p), b = key(v[(k + 1) % 3].p);
      if (a != b) edges.push_back({std::min(a, b), std::max(a, b), v[k], v[(k + 1) % 3]});
    }
  }
  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
    return std::tie(a.lo, a.hi) < std::tie(b.lo, b.hi);
  });
  Vector<Edge> result;
  for (u32 i = 0; i < edges.size();) {
    u32 j = i + 1;
    while (j < edges.size() && edges[j].lo == edges[i].lo && edges[j].hi == edges[i].hi)
      ++j;
    if (j == i + 1) result.push_back(edges[i]);
    i = j;
  }
  return result;
}
void cap_solid(Vector<Triangle>& triangles, const Vector<Edge>& edges, u32 material) {
  struct Endpoint {
    Key point;
    u32 edge;
  };
  Vector<Endpoint> nodes;
  for (u32 i = 0; i < edges.size(); ++i) {
    nodes.push_back({key(edges[i].a.p), i});
    nodes.push_back({key(edges[i].b.p), i});
  }
  std::sort(nodes.begin(), nodes.end(), [](const Endpoint& a, const Endpoint& b) {
    return std::tie(a.point, a.edge) < std::tie(b.point, b.edge);
  });
  Vector<u8> used(edges.size(), 0);
  for (u32 start = 0; start < edges.size(); ++start) {
    if (used[start]) continue;
    Vector<u32> loop;
    u32 at = start;
    Key end = key(edges[at].a.p), first = end;
    Vec3 centre{};
    bool closed = false;
    while (!used[at]) {
      used[at] = 1;
      loop.push_back(at);
      centre = centre + edges[at].a.p + edges[at].b.p;
      end = end == key(edges[at].a.p) ? key(edges[at].b.p) : key(edges[at].a.p);
      if (end == first) {
        closed = true;
        break;
      }
      const auto it = std::lower_bound(nodes.begin(), nodes.end(), end,
                                       [](const Endpoint& a, const Key& b) { return a.point < b; });
      u32 next = ~0u;
      for (auto n = it; n != nodes.end() && n->point == end; ++n)
        if (!used[n->edge]) {
          next = n->edge;
          break;
        }
      if (next == ~0u) break;
      at = next;
    }
    if (!closed || loop.size() < 3) continue;  // overlapping source joins are not welded unions
    centre = centre / (2.0f * static_cast<f32>(loop.size()));
    Vec3 plane{};
    for (const u32 index : loop)
      plane = plane + cross(edges[index].a.p - edges[index].b.p, centre - edges[index].b.p);
    if (length(plane) < 1e-9f) continue;
    plane = normalize(plane);
    const Vec3 u =
        normalize(cross(std::fabs(plane.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}, plane));
    const Vec3 v_axis = cross(plane, u);
    for (const u32 index : loop) {
      Vertex a = edges[index].b, b = edges[index].a, c = a;
      c.p = centre;
      const Vec3 n = normalize(cross(b.p - a.p, c.p - a.p));
      a.n = b.n = c.n = n;
      // One centred plane per entire cut: growth rings stay centred when the tree is posed,
      // including oblique branch fractures. Keep metre scale for material overrides.
      for (Vertex* v : {&a, &b, &c})
        v->uv = {dot(v->p - centre, u), dot(v->p - centre, v_axis)};
      triangle(triangles, a, b, c, material);
    }
  }
}
u32 component(Vector<u32>& parents, u32 x) {
  while (parents[x] != x) {
    parents[x] = parents[parents[x]];
    x = parents[x];
  }
  return x;
}
}  // namespace

bool in_range(const DeadwoodFile& p, std::string& error) {
  flora::FloraFile checked;
  if (!flora::read_flora_file(flora::canonical_flora_json(p.tree), checked, error)) return false;
  const auto* species = geometry::flora_species_info(p.tree.species);
  if (species == nullptr || !flora::flora_collides(p.tree) || p.tree.species == "shrub")
    return fail(error, "tree must name a tree species");
  if (static_cast<u32>(p.kind) > static_cast<u32>(RemainKind::limb)) return fail(error, "kind");
  if (!between(p.start_m, 0, 150) || !between(p.end_m, 0, 200)) return fail(error, "start_m/end_m");
  if (p.end_m != 0 && p.end_m <= p.start_m) return fail(error, "end_m must exceed start_m");
  if (!(p.branches == -1 || between(p.branches, 0, 1))) return fail(error, "branches");
  if (!(p.foliage == -1 || between(p.foliage, 0, 1))) return fail(error, "foliage");
  if (!between(p.bark, 0, 1) || !between(p.hollow, 0, .9f) || !between(p.collapse, 0, .85f))
    return fail(error, "bark/hollow/collapse");
  if (!between(p.decay, 0, 1) || static_cast<u32>(p.decay_pattern) > 1)
    return fail(error, "decay/decay_pattern");
  if (p.surface_resolution != 0 && (p.surface_resolution < 128 || p.surface_resolution > 1024 ||
                                    (p.surface_resolution & (p.surface_resolution - 1)) != 0))
    return fail(error, "surface_resolution must be 0 or a power of two in 128..1024");
  if (p.hollow > 0 && p.kind != RemainKind::log && p.kind != RemainKind::stump)
    return fail(error, "hollow requires a log or stump");
  if (p.hollow > 0 && branch_share(p) != 0) return fail(error, "hollow requires branches=0");
  if (p.collapse > 0 && p.kind != RemainKind::log) return fail(error, "collapse requires a log");
  if (p.collapse > 0 && pitch_of(p) != 90) return fail(error, "collapse requires pitch_deg=90");
  if (!between(p.splinter_m, 0, 2) || !between(p.yaw_deg, -360, 360) ||
      !between(p.pitch_deg, -360, 360) || !between(p.roll_deg, -360, 360))
    return fail(error, "splinter_m or pose angle");
  if (!between(p.lift_m, -1000, 1000) || !between(p.ground_y_m, -1000, 1000) ||
      !between(p.contact_tolerance_m, .001f, .1f) || !between(p.max_bend, 0, .5f))
    return fail(error, "site or contact controls");
  if (p.triangle_budget != 0 && (p.triangle_budget < 128 || p.triangle_budget > 4000000))
    return fail(error, "triangle_budget");
  if (p.supports.size() > 64) return fail(error, "supports exceeds 64");
  if (p.require_support && p.supports.empty()) return fail(error, "require_support needs supports");
  for (const auto& support : p.supports)
    if (!finite(support.from) || !finite(support.to) || !between(support.radius_m, .01f, 20) ||
        length(support.to - support.from) < .001f)
      return fail(error, "support capsule");
  if (p.ground) {
    const auto& g = *p.ground;
    if (g.columns < 2 || g.rows < 2 || g.columns > 513 || g.rows > 513 ||
        !between(g.spacing_m, .03125f, 4) || !between(g.min_x_m, -1000, 1000) ||
        !between(g.min_z_m, -1000, 1000) || g.heights_m.size() != g.columns * g.rows)
      return fail(error, "ground grid dimensions");
    for (const f32 h : g.heights_m)
      if (!between(h, -1000, 1000)) return fail(error, "ground height");
  }
  return true;
}
bool read_file(std::string_view text, DeadwoodFile& out, std::string& error) {
  out = {};
  JsonValue root;
  const auto parsed = parse_json(text, root);
  if (!parsed.ok) return fail(error, parsed.message);
  schema::ReadContext ctx;
  if (!schema::from_json(out, root, ctx) || !ctx.ok()) {
    error = "deadwood recipe:";
    for (const auto& d : ctx.diagnostics)
      error += " " + d.path + ": " + d.message;
    return false;
  }
  return in_range(out, error);
}
std::string canonical_json(const DeadwoodFile& file) {
  JsonValue root = schema::to_json(file);
  JsonValue tree;
  (void)parse_json(flora::canonical_flora_json(file.tree), tree);
  // Schema serialization keeps inactive source blocks; replace with flora's own identity.
  if (auto* member = root.find("tree")) *member = std::move(tree);
  JsonWriteOptions options;
  options.pretty = false;
  return write_json(root, options);
}
u64 source_hash(const DeadwoodFile& file) {
  const std::string json = canonical_json(file);
  return hash_combine(hash_combine(hash_bytes(json.data(), json.size()), k_deadwood_version),
                      flora::flora_source_hash(file.tree));
}

bool solve(const DeadwoodFile& p, assets::MeshData& mesh, flora::Motion& motion, Report& report,
           std::string& error) {
  mesh = {};
  motion = {};
  report = {};
  if (!in_range(p, error)) return false;
  assets::MeshData source;
  flora::Motion growth;
  if (!flora::solve_flora(p.tree, source, growth, nullptr, nullptr, error)) return false;
  report.source_triangles = source.indices.size() / 3;
  const u32 root = p.kind == RemainKind::limb ? p.source_part : 0;
  if (root >= growth.parts.size() || (p.kind == RemainKind::limb && root == 0))
    return fail(error, "source_part is not a detached limb in this tree");
  Frame frame{{}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, p};
  frame.origin = growth.parts[root].pivot;
  if (root != 0) {
    frame.y = normalize(growth.parts[root].direction);
    frame.x = normalize(cross(std::fabs(frame.y.z) < .9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}, frame.y));
    frame.z = cross(frame.x, frame.y);
  }
  const f32 full = growth.parts[root].length;
  f32 end = p.end_m;
  if (end == 0)
    end = p.kind == RemainKind::stump     ? std::min(1.0f, full * .2f)
          : p.kind == RemainKind::snapped ? full * .45f
          : p.kind == RemainKind::log     ? full * .6f
                                          : full + 10;
  if (p.start_m >= std::min(end, full)) return fail(error, "start_m lies past the source part");
  const f32 start = p.start_m;
  const f32 phase = draw(p.seed, 0x700000) * 6.2831853f;
  auto fracture = [&](const Vertex& v) {
    return p.splinter_m *
           static_cast<f32>(
               .5 + .3 * det::sin(static_cast<f64>(v.p.x) * 13.0 + static_cast<f64>(phase)) +
               .2 * det::sin(static_cast<f64>(v.p.z) * 19.0 - static_cast<f64>(phase)));
  };
  const bool bole_only = p.kind == RemainKind::log || p.kind == RemainKind::stump;
  const f32 leaves = leaf_share(p), branches = branch_share(p);
  Vector<u8> keep(growth.parts.size(), 0);
  Vector<f32> cut(growth.parts.size(), 1);
  Vector<Vec3> shift(growth.parts.size(), Vec3{}), bend(growth.parts.size(), Vec3{});
  keep[root] = 1;
  for (u32 i = root + 1; i < growth.parts.size(); ++i) {
    const auto& part = growth.parts[i];
    if (part.parent >= i || !keep[part.parent] || bole_only) continue;
    if (frame.local(part.pivot).y > end || frame.local(part.pivot).y < start) continue;
    if (cut[part.parent] < 1) continue;
    keep[i] = 1;
    if (draw(p.seed, i) >= branches) {
      cut[i] = branches == 0 ? 0 : .08f + .12f * draw(p.seed, 0x100000ull + i);
      ++report.broken_parts;
    }
  }
  Vector<Vec3> local(source.positions.size());
  for (u32 v = 0; v < local.size(); ++v)
    local[v] = frame.local(source.positions[v]);
  Vector<u8> woody_vertex(local.size(), 0);
  for (const auto& prim : source.primitives)
    if (prim.material == 0)
      for (u32 i = prim.first_index; i < prim.first_index + prim.index_count; ++i)
        woody_vertex[source.indices[i]] = 1;
  // Estimate settling from the retained bole, never from underside branches. This leaves
  // impact work for the branch solver instead of floating the whole tree on an intact crown.
  f32 lift = -std::numeric_limits<f32>::infinity();
  for (const auto& prim : source.primitives)
    if (prim.material == 0)
      for (u32 i = prim.first_index; i < prim.first_index + prim.index_count; i += 3) {
        if (growth.vertex_part[source.indices[i]] != root) continue;
        Vector<Vertex> poly;
        for (u32 k = 0; k < 3; ++k)
          poly.push_back({local[source.indices[i + k]], {}, {}});
        clip(poly, [&](const Vertex& v) { return v.p.y - start - (start > 0 ? fracture(v) : 0); });
        clip(poly, [&](const Vertex& v) { return end - fracture(v) - v.p.y; });
        for (const auto& v : poly) {
          Vec3 at = frame.pose(v.p - Vec3{0, start, 0});
          at.y *= 1 - p.collapse;
          const f32 g = ground_at(p, at);
          if (!std::isfinite(g)) return fail(error, "ground does not cover the posed bole");
          lift = std::max(lift, g - at.y);
        }
      }
  if (!std::isfinite(lift)) return fail(error, "selected interval contains no source bole surface");
  report.lift_m = (p.settle ? lift : 0) + p.lift_m;
  auto posed = [&](Vec3 v) {
    Vec3 at = frame.pose(v - Vec3{0, start, 0});
    at.y *= 1 - p.collapse;
    at.y += report.lift_m;
    return at;
  };
  // Parent-first fitting carries a parent's endpoint displacement into its children.
  // A bounded quadratic cantilever shape is a geometric fit, not an impact/strength prediction.
  Vector<Vector<u32>> part_vertices(growth.parts.size());
  for (u32 v = 0; v < local.size(); ++v)
    part_vertices[growth.vertex_part[v]].push_back(v);
  for (u32 i = root; i < growth.parts.size(); ++i) {
    if (!keep[i] || cut[i] == 0) continue;
    const auto& part = growth.parts[i];
    if (i != root) {
      if (!keep[part.parent]) {
        keep[i] = 0;
        continue;
      }
      const auto& parent = growth.parts[part.parent];
      const f32 attach = clamp(
          dot(part.pivot - parent.pivot, parent.direction) / std::max(parent.length, .0001f), 0, 1);
      if (attach >= cut[part.parent]) {
        keep[i] = 0;
        continue;
      }
      shift[i] = shift[part.parent] + bend[part.parent] * (attach * attach);
    }
    for (u32 iteration = 0; iteration < 4 && i != root; ++iteration) {
      for (const u32 v : part_vertices[i]) {
        if (!woody_vertex[v]) continue;
        const f32 w = growth.vertex_weight[v];
        if (w > cut[i] || local[v].y < start || local[v].y > end) continue;
        const Vec3 at = posed(local[v]) + shift[i] + bend[i] * (w * w);
        Vec3 n;
        const f32 gap = clearance(p, at, n);
        if (!std::isfinite(gap)) return fail(error, "ground does not cover retained branches");
        if (gap < -p.contact_tolerance_m) {
          const Vec3 correction = n * (-gap / std::max(w * w, .0001f));
          if (length(bend[i] + correction) <= p.max_bend * part.length)
            bend[i] = bend[i] + correction;
          else {
            cut[i] = std::min(cut[i], std::max(0.0f, w - .06f));
            ++report.broken_parts;
          }
        }
      }
    }
    if (length(bend[i]) > 0) ++report.bent_parts;
    if (cut[i] > 0) ++report.retained_parts;
  }
  // Keep/drop complete connected foliage units: independent blades stay whole and older
  // connected shoot cards are thinned by spray, never by arbitrary individual triangles.
  Vector<u32> parents(source.positions.size());
  for (u32 i = 0; i < parents.size(); ++i)
    parents[i] = i;
  for (const auto& prim : source.primitives)
    if (prim.material != 0)
      for (u32 i = prim.first_index; i < prim.first_index + prim.index_count; i += 3) {
        for (u32 k = 1; k < 3; ++k) {
          const u32 a = component(parents, source.indices[i]);
          const u32 b = component(parents, source.indices[i + k]);
          parents[std::max(a, b)] = std::min(a, b);
        }
      }
  Vector<u8> leaf_keep(source.positions.size(), 1);
  for (const auto& prim : source.primitives)
    if (prim.material != 0)
      for (u32 i = prim.first_index; i < prim.first_index + prim.index_count; ++i) {
        const u32 v = source.indices[i], id = growth.vertex_part[v];
        const f32 w = growth.vertex_weight[v];
        Vec3 n;
        const f32 gap = clearance(p, posed(local[v]) + shift[id] + bend[id] * (w * w), n);
        const Vertex sample{local[v], {}, {}};
        if (!std::isfinite(gap) || gap < -p.contact_tolerance_m ||
            local[v].y < start + (start > 0 ? fracture(sample) : 0) ||
            local[v].y > end - fracture(sample))
          leaf_keep[component(parents, v)] = 0;
      }
  Vector<Triangle> wood, foliage;
  const u32 wood_material = source.materials.size();
  for (const auto& prim : source.primitives) {
    const bool woody = prim.material == 0;
    if (!woody && leaves == 0) continue;
    for (u32 t = prim.first_index; t < prim.first_index + prim.index_count; t += 3) {
      const u32 id = growth.vertex_part[source.indices[t]];
      if (!keep[id] || cut[id] == 0) continue;
      if (!woody && (cut[id] < 1 || !leaf_keep[component(parents, source.indices[t])] ||
                     draw(p.seed, 0x200000ull + component(parents, source.indices[t])) >= leaves))
        continue;
      Vector<Vertex> poly;
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = source.indices[t + k];
        const Vec3 n = source.normals[v];
        poly.push_back({local[v],
                        {dot(n, frame.x), dot(n, frame.y), dot(n, frame.z)},
                        source.uvs[v],
                        growth.vertex_weight[v]});
      }
      clip(poly, [&](const Vertex& v) { return v.p.y - start - (start > 0 ? fracture(v) : 0); });
      clip(poly, [&](const Vertex& v) { return end - fracture(v) - v.p.y; });
      if (cut[id] < 1) clip(poly, [&](const Vertex& v) { return cut[id] - v.w; });
      if (poly.size() < 3) continue;
      // Collapse opens a longitudinal roof strip, exposing the hollow's inner wall.
      if (p.hollow > 0 && p.collapse > 0) {
        const Vec3 centre = (poly[0].p + poly[1].p + poly[2].p) / 3.0f;
        const Vec3 radial = frame.pose({centre.x, 0, centre.z});
        if (radial.y > (1 - p.collapse) * length(radial)) continue;
      }
      u32 material = static_cast<u32>(prim.material);
      if (woody && p.bark == 0) material = wood_material;
      for (auto& v : poly) {
        v.p = posed(v.p) + shift[id] + bend[id] * (v.w * v.w);
        v.n = frame.pose(v.n);
        v.n.y /= 1 - p.collapse;
        // Inverse transpose of p' = p + bend*w*w, with the source axial
        // weight gradient. Translation inherited from ancestors has no gradient.
        const auto& part = growth.parts[id];
        const Vec3 direction{dot(part.direction, frame.x), dot(part.direction, frame.y),
                             dot(part.direction, frame.z)};
        Vec3 gradient = frame.pose(direction) / std::max(part.length, .0001f);
        gradient.y /= 1 - p.collapse;
        const Vec3 change = bend[id] * (2 * v.w);
        const f32 determinant = 1 + dot(gradient, change);
        if (std::fabs(determinant) > .0001f)
          v.n = v.n - gradient * (dot(change, v.n) / determinant);
        v.n = normalize(v.n);
      }
      for (u32 k = 1; k + 1 < poly.size(); ++k)
        triangle(woody ? wood : foliage, poly[0], poly[k], poly[k + 1], material, id);
    }
  }
  // A cut interpolates new vertices between samples; quadratic fitting of the original
  // vertices alone cannot guarantee that this new fracture clears a support. Break an
  // unsafe branch at its attachment and discard its children before making closure faces.
  for (const auto& t : wood) {
    if (t.part == root) continue;
    for (const Vec3 at : {t.a.p, t.b.p, t.c.p, (t.a.p + t.b.p) * .5f, (t.b.p + t.c.p) * .5f,
                          (t.c.p + t.a.p) * .5f}) {
      Vec3 n;
      const f32 gap = clearance(p, at, n);
      if (!std::isfinite(gap) || gap < -p.contact_tolerance_m) keep[t.part] = 0;
    }
  }
  report.broken_parts = report.bent_parts = report.retained_parts = 0;
  for (u32 i = root; i < growth.parts.size(); ++i) {
    if (i != root && !keep[growth.parts[i].parent]) keep[i] = 0;
    if (keep[i] && cut[i] > 0) {
      ++report.retained_parts;
      if (length(bend[i]) > 0) ++report.bent_parts;
    }
    if (!keep[i] || cut[i] < 1) ++report.broken_parts;
  }
  auto discard = [&](Vector<Triangle>& tris) {
    tris.erase(
        std::remove_if(tris.begin(), tris.end(), [&](const Triangle& t) { return !keep[t.part]; }),
        tris.end());
  };
  discard(wood);
  discard(foliage);
  if (wood.empty()) return fail(error, "selection has no wood");
  const Vector<Edge> edges = boundaries(wood);
  if (p.hollow > 0) {
    auto inner = [&](Vertex v) {
      Vec3 at = v.p;
      at.y = (at.y - report.lift_m) / (1 - p.collapse);
      at = frame.unpose(at) + Vec3{0, start, 0};
      at.x *= p.hollow;
      at.z *= p.hollow;
      v.p = posed(at);
      v.n = v.n * -1;
      v.uv.x *= p.hollow;
      return v;
    };
    const u32 count = wood.size();
    for (u32 i = 0; i < count; ++i) {
      const Triangle t = wood[i];
      triangle(wood, inner(t.c), inner(t.b), inner(t.a), wood_material);
    }
    for (const auto& e : edges) {
      Vertex a = e.a, b = e.b, ai = inner(a), bi = inner(b);
      const Vec3 n = normalize(cross(a.p - b.p, ai.p - b.p));
      a.n = b.n = ai.n = bi.n = n;
      const bool transverse = std::fabs(dot(n, frame.pose({0, 1, 0}))) > .5f;
      if (transverse)
        for (Vertex* v : {&a, &b, &ai, &bi}) {
          Vec3 at = v->p;
          at.y = (at.y - report.lift_m) / (1 - p.collapse);
          at = frame.unpose(at);
          v->uv = {at.x, at.z};
        }
      triangle(wood, b, a, ai, wood_material + (transverse ? 1 : 2));
      triangle(wood, b, ai, bi, wood_material + (transverse ? 1 : 2));
    }
  } else
    cap_solid(wood, edges, wood_material + 1);
  // Physical closures precede material contours. UV seams can split at different points on
  // either side of one physical edge; those material T-junctions are never holes to cap.
  if (p.bark > 0 && p.bark < 1) {
    Vector<Triangle> surfaced;
    for (const auto& t : wood) {
      if (t.material == 0)
        bark_triangle(surfaced, p, wood_material, t.a, t.b, t.c, t.part);
      else
        surfaced.push_back(t);
    }
    wood = std::move(surfaced);
  }
  report.wood_triangles = wood.size();
  report.foliage_triangles = foliage.size();
  report.triangles = wood.size() + foliage.size();
  report.budget = p.triangle_budget;
  if (report.budget == 0) {
    report.budget = p.kind == RemainKind::stump  ? 8000
                    : p.kind == RemainKind::log  ? 20000
                    : p.kind == RemainKind::limb ? 60000
                    : p.kind == RemainKind::snag ? 100000
                                                 : report.source_triangles + 16384;
  }
  if (report.triangles > report.budget)
    return fail(error, "triangle_budget needs " + std::to_string(report.triangles) +
                           " triangles; lower the source tree detail or raise the cap");
  // Validate the finished surface, including edge midpoints. Never silently accept a
  // different support pose just because source vertices happened to miss a contact.
  f32 support_min = std::numeric_limits<f32>::infinity();
  report.minimum_clearance_m = std::numeric_limits<f32>::infinity();
  for (const auto& t : wood) {
    const Vec3 samples[] = {
        t.a.p, t.b.p, t.c.p, (t.a.p + t.b.p) * .5f, (t.b.p + t.c.p) * .5f, (t.c.p + t.a.p) * .5f};
    for (const Vec3 at : samples) {
      Vec3 n;
      const f32 gap = clearance(p, at, n);
      if (!std::isfinite(gap)) return fail(error, "ground does not cover the finished surface");
      report.minimum_clearance_m = std::min(report.minimum_clearance_m, gap);
      if (gap < -p.contact_tolerance_m)
        return fail(
            error,
            "pose intersects ground/support after fitting; change lift, pitch or contact site");
      for (const auto& support : p.supports)
        support_min = std::min(support_min, support_gap(support, at, n));
    }
  }
  if (p.require_support && support_min > p.contact_tolerance_m)
    return fail(error, "pose does not touch a required support");
  assets::MeshData result;
  result.materials = source.materials;
  assets::Material exposed;
  exposed.name = "exposed_wood";
  exposed.base_color = {.52f, .32f, .15f, 1};
  exposed.metallic = 0;
  exposed.roughness = .9f;
  result.materials.push_back(exposed);
  exposed.name = "end_grain";
  result.materials.push_back(exposed);
  exposed.name = "bark_fracture";
  exposed.base_color = {.24f, .13f, .06f, 1};
  result.materials.push_back(exposed);
  for (u32 material = 0; material < result.materials.size(); ++material) {
    const u32 first = result.indices.size();
    auto emit = [&](const Vector<Triangle>& tris) {
      for (const auto& t : tris)
        if (t.material == material)
          for (const Vertex& v : {t.a, t.b, t.c}) {
            result.indices.push_back(result.positions.size());
            result.positions.push_back(v.p);
            result.normals.push_back(normalize(v.n));
            result.uvs.push_back(v.uv);
          }
    };
    emit(wood);
    emit(foliage);
    if (result.indices.size() > first)
      result.primitives.push_back(
          {first, result.indices.size() - first, static_cast<i32>(material), -1});
  }
  assets::weld_vertices(result);
  // Two fixed parentless parts preserve collision/host eligibility without retaining a
  // live tree's pose hierarchy. Zero weights make every frame exactly the cooked pose.
  motion.parts.push_back({{}, {0, 1, 0}, 1, geometry::k_motion_no_parent});
  motion.parts.push_back({{}, {0, 1, 0}, 1, geometry::k_motion_no_parent});
  motion.structure.push_back({0, 1, 1, geometry::k_structural_collider});
  motion.structure.push_back({1, 0, 0, 0});
  motion.vertex_part.resize(result.positions.size(), 0);
  motion.vertex_weight.resize(result.positions.size(), 0);
  for (const auto& prim : result.primitives)
    if (prim.material > 0 && prim.material < static_cast<i32>(wood_material))
      for (u32 i = prim.first_index; i < prim.first_index + prim.index_count; ++i)
        motion.vertex_part[result.indices[i]] = 1;
  mesh = std::move(result);
  return true;
}
}  // namespace engine::deadwood
