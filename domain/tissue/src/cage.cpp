// Cage derivation, `corner-collapse-v1` (domain/tissue/cage.h; docs/subsystems/tissue.md, "Cage
// derivation"). Content-build code: single-threaded, in double, every decision broken by index.

#include "body.h"
#include "cells.h"
#include "mesh_query.h"

#include <core/json/json.h>
#include <domain/physics/deformable.h>
#include <domain/tissue/cage.h>
#include <domain/tissue/sha256.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace engine::tissue {

namespace {

using detail::Body;
using detail::BodyState;
using query::D3;
using query::d3;

constexpr u32 k_omega = ~0u;  // the virtual vertex the boundary is coned to, for the link condition
// A collapse may leave no re-coned cell worse than this at the construction, or than the worst of
// the cells it replaces where that is lower: above the validator's 0.1 warning line.
constexpr f64 k_quality_floor = 0.15;
// A boundary collapse may turn no boundary face by more than 45 degrees at the construction, and
// none over in any state.
constexpr f64 k_face_turn_cos = 0.70710678118654752;
// The resolution a boundary collapse's deviation is compared at: 0.1 um, so that rounding noise on
// a flat stretch of boundary does not decide the order.
constexpr f64 k_deviation_quantum_m = 1.0e-7;

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

std::string str(u64 v) { return std::to_string(v); }

// The working mesh of the collapses: tetrahedra over the reference's own node numbering.
struct Mesh {
  Vector<u32> tet;           // four node indices a tetrahedron, positively oriented
  Vector<u8> alive;          // per tetrahedron
  Vector<Vector<u32>> star;  // per node: the live tetrahedra holding it, ascending
  Vector<u8> node_alive;     // per node: a corner node not yet collapsed
  Vector<u8> boundary;       // per node: on a boundary face
  Vector<u32> stamp;         // per node: bumped whenever its star changes
  const u32* nodes(u32 t) const noexcept { return tet.data() + 4 * t; }
  bool holds(u32 t, u32 v) const noexcept {
    const u32* n = nodes(t);
    return n[0] == v || n[1] == v || n[2] == v || n[3] == v;
  }
};

// How many live tetrahedra hold the face (x, y, z).
u32 face_count(const Mesh& m, u32 x, u32 y, u32 z) {
  u32 count = 0;
  for (const u32 t : m.star[x])
    if (m.holds(t, y) && m.holds(t, z)) ++count;
  return count;
}

// The face of tetrahedron t opposite its node `v`, wound outward (facing away from v) for a
// positive tetrahedron — the validator's boundary winding. The three faces holding v are then (v,
// f1, f0), (v, f2, f1) and (v, f0, f2) wound outward: (v, f_k, f_k+1) faces inward.
void opposite(const Mesh& m, u32 t, u32 v, u32 out[3]) {
  const u32* n = m.nodes(t);
  static const u32 face[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
  for (u32 k = 0; k < 4; ++k)
    if (n[k] == v) {
      for (u32 j = 0; j < 3; ++j)
        out[j] = n[face[k][j]];
      return;
    }
}

void refresh_boundary(Mesh& m, u32 v) {
  m.boundary[v] = 0;
  for (const u32 t : m.star[v]) {
    u32 f[3];
    opposite(m, t, v, f);
    // The three faces of t that hold v: (v, f0, f1), (v, f1, f2), (v, f2, f0).
    for (u32 k = 0; k < 3; ++k)
      if (face_count(m, v, f[k], f[(k + 1) % 3]) == 1) {
        m.boundary[v] = 1;
        return;
      }
  }
}

u64 edge_key(u32 a, u32 b) {
  return (static_cast<u64>(std::min(a, b)) << 32) | static_cast<u64>(std::max(a, b));
}

struct Tri {
  u32 v[3];
  bool operator<(const Tri& o) const noexcept {
    return std::lexicographical_compare(v, v + 3, o.v, o.v + 3);
  }
  bool operator==(const Tri& o) const noexcept {
    return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2];
  }
};

Tri tri(u32 a, u32 b, u32 c) {
  Tri t{{a, b, c}};
  std::sort(t.v, t.v + 3);
  return t;
}

// The link of a node, with the boundary coned to the virtual vertex (Dey, Edelsbrunner, Guha and
// Nekhayev 1999, the link condition for an edge contraction in a 3-manifold with boundary).
struct Link {
  Vector<u32> vertices;
  Vector<u64> edges;
  Vector<Tri> triangles;
  void finish() {
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::sort(triangles.begin(), triangles.end());
    triangles.erase(std::unique(triangles.begin(), triangles.end()), triangles.end());
  }
};

Link link_of(const Mesh& m, u32 v) {
  Link out;
  for (const u32 t : m.star[v]) {
    u32 f[3];
    opposite(m, t, v, f);
    for (u32 k = 0; k < 3; ++k) {
      out.vertices.push_back(f[k]);
      out.edges.push_back(edge_key(f[k], f[(k + 1) % 3]));
      // A boundary face (v, f_k, f_k+1) cones to omega: (omega, f_k, f_k+1) is in the link.
      if (face_count(m, v, f[k], f[(k + 1) % 3]) == 1) {
        out.vertices.push_back(k_omega);
        out.edges.push_back(edge_key(k_omega, f[k]));
        out.edges.push_back(edge_key(k_omega, f[(k + 1) % 3]));
        out.triangles.push_back(tri(k_omega, f[k], f[(k + 1) % 3]));
      }
    }
    out.triangles.push_back(tri(f[0], f[1], f[2]));
  }
  out.finish();
  return out;
}

// Lk(a) and Lk(b) meet exactly in Lk(ab): the contraction changes no topology.
bool link_condition(const Mesh& m, u32 a, u32 b) {
  const Link la = link_of(m, a);
  const Link lb = link_of(m, b);
  // Lk(ab): for each tetrahedron (a, b, x, y), the edge (x, y) and its ends; for each boundary face
  // (a, b, x), the edge (omega, x); omega itself when ab is a boundary edge.
  Vector<u32> ab_vertices;
  Vector<u64> ab_edges;
  for (const u32 t : m.star[a]) {
    if (!m.holds(t, b)) continue;
    u32 others[2];
    u32 k = 0;
    for (u32 j = 0; j < 4; ++j)
      if (m.nodes(t)[j] != a && m.nodes(t)[j] != b) others[k++] = m.nodes(t)[j];
    ab_vertices.push_back(others[0]);
    ab_vertices.push_back(others[1]);
    ab_edges.push_back(edge_key(others[0], others[1]));
    for (u32 j = 0; j < 2; ++j)
      if (face_count(m, a, b, others[j]) == 1) {
        ab_vertices.push_back(k_omega);
        ab_edges.push_back(edge_key(k_omega, others[j]));
      }
  }
  std::sort(ab_vertices.begin(), ab_vertices.end());
  ab_vertices.erase(std::unique(ab_vertices.begin(), ab_vertices.end()), ab_vertices.end());
  std::sort(ab_edges.begin(), ab_edges.end());
  ab_edges.erase(std::unique(ab_edges.begin(), ab_edges.end()), ab_edges.end());
  // Every vertex both links share is Lk(ab)'s (a and b themselves excepted: each is in the
  // other's).
  for (const u32 v : la.vertices) {
    if (v == b) continue;
    if (!std::binary_search(lb.vertices.begin(), lb.vertices.end(), v)) continue;
    if (!std::binary_search(ab_vertices.begin(), ab_vertices.end(), v)) return false;
  }
  for (const u64 e : la.edges) {
    const u32 lo = static_cast<u32>(e >> 32);
    const u32 hi = static_cast<u32>(e & 0xffffffffu);
    if (lo == b || hi == b) continue;
    if (!std::binary_search(lb.edges.begin(), lb.edges.end(), e)) continue;
    if (!std::binary_search(ab_edges.begin(), ab_edges.end(), e)) return false;
  }
  for (const Tri& t : la.triangles) {
    if (t.v[0] == b || t.v[1] == b || t.v[2] == b) continue;
    if (std::binary_search(lb.triangles.begin(), lb.triangles.end(), t)) return false;
  }
  return true;
}

// What a candidate collapse a -> b would do, and whether it may.
struct Candidate {
  bool valid = false;
  u8 tier = 0;          // 0: a is interior; 1: a is on the boundary
  f64 primary = 0.0;    // interior: edge length over the worst quality left; boundary: deviation
  f64 secondary = 0.0;  // boundary: edge length over the worst quality left
  f64 deviation = 0.0;  // how far the boundary moves, metres
};

struct Collapser {
  const Body& body;
  const Vector<Vector<u32>>& sets_of;  // per node: the node sets holding it, ascending
  Mesh& m;

  f64 sicn(const u32 n[4], const Vector<Vec3>& x) const {
    return query::tet_sicn(d3(x[n[0]]), d3(x[n[1]]), d3(x[n[2]]), d3(x[n[3]]));
  }
  f64 volume(const u32 n[4], const Vector<Vec3>& x) const {
    return query::tet_volume(d3(x[n[0]]), d3(x[n[1]]), d3(x[n[2]]), d3(x[n[3]]));
  }

  Candidate evaluate(u32 a, u32 b) const {
    Candidate c;
    if (a == b || !m.node_alive[a] || !m.node_alive[b]) return c;
    // Every node set that holds a holds b: a set never loses its reach, and never empties.
    const Vector<u32>& sa = sets_of[a];
    const Vector<u32>& sb = sets_of[b];
    if (!std::includes(sb.begin(), sb.end(), sa.begin(), sa.end())) return c;
    // The edge exists.
    bool edge = false;
    for (const u32 t : m.star[a])
      edge = edge || m.holds(t, b);
    if (!edge) return c;
    const bool boundary = m.boundary[a] != 0;
    // A boundary node moves only along the boundary, onto a boundary node.
    if (boundary && m.boundary[b] == 0) return c;
    if (!link_condition(m, a, b)) return c;
    // The re-coned cells: every tetrahedron of a's star that does not hold b, with b for a.
    const u32 states = body.states.size();
    Vector<f64> old_worst(states, std::numeric_limits<f64>::infinity());
    for (const u32 t : m.star[a])
      for (u32 s = 0; s < states; ++s)
        old_worst[s] = std::min(old_worst[s], sicn(m.nodes(t), body.states[s].nodes));
    f64 new_worst = std::numeric_limits<f64>::infinity();
    u32 recone = 0;
    for (const u32 t : m.star[a]) {
      if (m.holds(t, b)) continue;
      ++recone;
      u32 n[4];
      for (u32 k = 0; k < 4; ++k)
        n[k] = m.nodes(t)[k] == a ? b : m.nodes(t)[k];
      for (u32 s = 0; s < states; ++s) {
        const Vector<Vec3>& x = body.states[s].nodes;
        if (!(volume(n, x) > 0.0)) return c;
        const f64 q = sicn(n, x);
        if (q < std::min(k_quality_floor, old_worst[s])) return c;
        if (s == 0) new_worst = std::min(new_worst, q);
      }
    }
    if (recone == 0) return c;
    const f64 length = query::length(d3(body.nodes[a]) - d3(body.nodes[b]));
    if (!boundary) {
      c.valid = true;
      c.tier = 0;
      c.primary = length / new_worst;
      return c;
    }
    // The boundary faces around a that survive, re-coned to b: none may turn by more than 45
    // degrees at the construction or over in any state; the deviation is how far a's place is from
    // the new faces.
    f64 deviation = std::numeric_limits<f64>::infinity();
    u32 faces = 0;
    for (const u32 t : m.star[a]) {
      u32 f[3];
      opposite(m, t, a, f);
      for (u32 k = 0; k < 3; ++k) {
        const u32 x = f[k];
        const u32 y = f[(k + 1) % 3];
        if (face_count(m, a, x, y) != 1) continue;
        if (x == b || y == b) continue;  // it collapses with the edge
        ++faces;
        // (a, x, y) as wound here faces into the tetrahedron; the outward face is (a, y, x).
        for (u32 s = 0; s < states; ++s) {
          const Vector<Vec3>& p = body.states[s].nodes;
          const D3 n_old = query::cross(d3(p[y]) - d3(p[a]), d3(p[x]) - d3(p[a]));
          const D3 n_new = query::cross(d3(p[y]) - d3(p[b]), d3(p[x]) - d3(p[b]));
          const f64 lo = query::length(n_old);
          const f64 ln = query::length(n_new);
          if (!(lo > 0.0) || !(ln > 0.0)) return c;
          const f64 cosine = query::dot(n_old, n_new) / (lo * ln);
          if (s == 0 ? cosine < k_face_turn_cos : !(cosine > 0.0)) return c;
        }
        deviation = std::min(deviation,
                             query::point_triangle_distance(d3(body.nodes[a]), d3(body.nodes[b]),
                                                            d3(body.nodes[y]), d3(body.nodes[x])));
      }
    }
    if (faces == 0) return c;
    c.valid = true;
    c.tier = 1;
    c.deviation = deviation;
    c.primary = std::floor(deviation / k_deviation_quantum_m);
    c.secondary = length / new_worst;
    return c;
  }

  // Moves a onto b. Returns the nodes whose stars changed.
  Vector<u32> apply(u32 a, u32 b) {
    Vector<u32> touched;
    const Vector<u32> star_a = m.star[a];
    for (const u32 t : star_a) {
      u32* n = m.tet.data() + 4 * t;
      for (u32 k = 0; k < 4; ++k)
        touched.push_back(n[k]);
      if (m.holds(t, b)) {
        m.alive[t] = 0;
        for (u32 k = 0; k < 4; ++k) {
          Vector<u32>& s = m.star[n[k]];
          s.erase(std::lower_bound(s.begin(), s.end(), t));
        }
        continue;
      }
      for (u32 k = 0; k < 4; ++k)
        if (n[k] == a) n[k] = b;
      Vector<u32>& sb = m.star[b];
      sb.insert(static_cast<u32>(std::lower_bound(sb.begin(), sb.end(), t) - sb.begin()), t);
    }
    m.star[a].clear();
    m.node_alive[a] = 0;
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    Vector<u32> live;
    for (const u32 v : touched)
      if (v != a) live.push_back(v);
    for (const u32 v : live) {
      refresh_boundary(m, v);
      ++m.stamp[v];
    }
    ++m.stamp[a];
    return live;
  }
};

struct Entry {
  u8 tier;
  f64 primary;
  f64 secondary;
  u32 a;
  u32 b;
  u32 stamp_a;
  u32 stamp_b;
};

// The heap's order: the cheapest first; a tie goes to the lower (a, b).
bool later(const Entry& x, const Entry& y) {
  if (x.tier != y.tier) return x.tier > y.tier;
  if (x.primary != y.primary) return x.primary > y.primary;
  if (x.secondary != y.secondary) return x.secondary > y.secondary;
  if (x.a != y.a) return x.a > y.a;
  return x.b > y.b;
}

// A uniform grid of boxes, for the overlap's candidates.
struct BoxGrid {
  D3 origin;
  f64 size = 1.0;
  u32 dims[3] = {1, 1, 1};
  Vector<u32> start;
  Vector<u32> items;

  u32 at(f64 v, f64 o, u32 axis) const {
    const f64 i = std::floor((v - o) / size);
    return static_cast<u32>(std::clamp(i, 0.0, static_cast<f64>(dims[axis] - 1)));
  }
  void build(const Vector<D3>& lo, const Vector<D3>& hi) {
    const u32 count = lo.size();
    D3 a = lo[0];
    D3 b = hi[0];
    f64 extent = 0.0;
    for (u32 i = 0; i < count; ++i) {
      a = D3{std::min(a.x, lo[i].x), std::min(a.y, lo[i].y), std::min(a.z, lo[i].z)};
      b = D3{std::max(b.x, hi[i].x), std::max(b.y, hi[i].y), std::max(b.z, hi[i].z)};
      extent += std::max(hi[i].x - lo[i].x, std::max(hi[i].y - lo[i].y, hi[i].z - lo[i].z));
    }
    origin = a;
    size = extent / static_cast<f64>(count);
    if (!(size > 0.0)) size = 1.0;
    const f64 span[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
    for (u32 k = 0; k < 3; ++k)
      dims[k] = static_cast<u32>(std::min(std::floor(span[k] / size) + 1.0, 128.0));
    for (u32 k = 0; k < 3; ++k)
      size = std::max(size, span[k] / static_cast<f64>(dims[k]) * 1.0000001);
    const u32 total = dims[0] * dims[1] * dims[2];
    Vector<u32> counts(total + 1, 0u);
    const auto each = [&](u32 i, auto&& visit) {
      for (u32 z = at(lo[i].z, origin.z, 2); z <= at(hi[i].z, origin.z, 2); ++z)
        for (u32 y = at(lo[i].y, origin.y, 1); y <= at(hi[i].y, origin.y, 1); ++y)
          for (u32 x = at(lo[i].x, origin.x, 0); x <= at(hi[i].x, origin.x, 0); ++x)
            visit((z * dims[1] + y) * dims[0] + x);
    };
    for (u32 i = 0; i < count; ++i)
      each(i, [&](u32 v) { ++counts[v + 1]; });
    for (u32 v = 0; v < total; ++v)
      counts[v + 1] += counts[v];
    start = counts;
    items.assign(counts[total], 0u);
    Vector<u32> fill = counts;
    for (u32 i = 0; i < count; ++i)
      each(i, [&](u32 v) { items[fill[v]++] = i; });
  }
  void query(D3 lo, D3 hi, Vector<u32>& out) const {
    out.clear();
    for (u32 z = at(lo.z, origin.z, 2); z <= at(hi.z, origin.z, 2); ++z)
      for (u32 y = at(lo.y, origin.y, 1); y <= at(hi.y, origin.y, 1); ++y)
        for (u32 x = at(lo.x, origin.x, 0); x <= at(hi.x, origin.x, 0); ++x) {
          const u32 v = (z * dims[1] + y) * dims[0] + x;
          for (u32 i = start[v]; i < start[v + 1]; ++i)
            out.push_back(items[i]);
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }
};

template <class T>
void put(TissueFile& file, const std::string& name, BlockKind kind, const Vector<T>& data,
         u32 per_element = 1) {
  add_block(file, name, kind, std::span<const T>(data.data(), data.size()), per_element);
}

void copy_block(const TissueFile& from, const std::string& name, TissueFile& to) {
  if (name.empty() || to.find(name) != nullptr) return;
  const TissueBlock* b = from.find(name);
  if (b == nullptr) return;
  TissueBlock copy = *b;
  seal_block(to, copy);
  to.blocks.push_back(std::move(copy));
}

}  // namespace

std::string cage_build_key(const std::string& source_sha256, const std::string& region,
                           u32 node_budget, bool hero, const Vector<std::string>& omitted) {
  std::string text = std::string(k_cage_method) + "\n" + source_sha256 + "\n" + region + "\n" +
                     str(node_budget) + "\n" + (hero ? "hero" : "ambient") + "\n";
  Vector<std::string> sorted = omitted;
  std::sort(sorted.begin(), sorted.end());
  for (const std::string& s : sorted)
    text += "omit " + s + "\n";
  return sha256_hex(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

bool derive_cage(const TissueFile& reference, const CageOptions& options, TissueFile& out,
                 CageSummary& summary, std::string* error) {
  out = TissueFile{};
  summary = CageSummary{};
  const TissueDefinition& d = reference.definition;
  // ---- the reference region
  std::string region_name = options.region;
  if (region_name.empty()) {
    u32 found = 0;
    for (const Region& r : d.regions)
      if (r.role == RegionRole::Reference) {
        region_name = r.name;
        ++found;
      }
    if (found == 0) return fail(error, "the file has no Reference region to derive a cage from");
    if (found > 1)
      return fail(error, "the file has " + str(found) +
                             " Reference regions: name the one to derive from (--region)");
  }
  Body body;
  if (!detail::load_body(reference, region_name, body, error)) return false;
  const Region& source = *body.region;
  if (source.role != RegionRole::Reference)
    return fail(error, "region " + source.name +
                           " is a Runtime cage, not a Reference body: a cage is derived from a "
                           "reference body");
  const u32 budget = options.node_budget != 0 ? options.node_budget
                                              : (options.hero ? physics::k_cage_elements_max
                                                              : physics::k_cage_elements_default);
  if (budget > physics::k_cage_elements_max)
    return fail(error, "a budget of " + str(budget) + " nodes is past ADR-0029's " +
                           str(physics::k_cage_elements_max) + " for any cage");
  if (!options.hero && budget > physics::k_cage_elements_default)
    return fail(error, "a budget of " + str(budget) + " nodes is wider than one solve group (" +
                           str(physics::k_cage_elements_default) +
                           "): that is a hero volume, which is declared (--hero), never implied");
  if (budget < 4) return fail(error, "a cage needs at least four nodes");
  for (const Attachment& a : d.attachments)
    if (a.region == source.name && a.enforcement == AttachmentEnforcement::Essential)
      return fail(error, "attachment " + a.name +
                             " is essential, and its patch lies on the ten-node material surface a "
                             "four-node cage does not have: the cage derivation does not carry the "
                             "layered model's essential attachments yet");
  std::string source_sha = options.source_sha256;
  if (source_sha.empty()) {
    Vector<u8> bytes;
    if (!encode_tissue_file(reference, bytes, error)) return false;
    source_sha = sha256_hex(std::span<const u8>(bytes.data(), bytes.size()));
  }
  // The states left out: each one a state of the region, never the construction, and the rest of
  // the derivation sees only the states it carries.
  Vector<std::string> omitted = options.omit_states;
  std::sort(omitted.begin(), omitted.end());
  omitted.erase(std::unique(omitted.begin(), omitted.end()), omitted.end());
  for (const std::string& s : omitted)
    if (s == "construction" || body.state(s) == nullptr)
      return fail(error, "state '" + s + "' is not a state of region " + source.name +
                             " that a cage can leave out");
  {
    Vector<BodyState> carried;
    for (BodyState& s : body.states)
      if (!std::binary_search(omitted.begin(), omitted.end(), s.name))
        carried.push_back(std::move(s));
    body.states = std::move(carried);
  }
  summary.source_region = source.name;
  summary.source_sha256 = source_sha;
  summary.node_budget = budget;
  summary.hero = options.hero;
  summary.omitted_states = omitted;
  summary.build_key = cage_build_key(source_sha, source.name, budget, options.hero, omitted);
  summary.source_nodes = body.nodes.size();
  summary.source_cells = body.cell_count();

  // ---- the node sets, per node
  const u32 n = body.nodes.size();
  Vector<Vector<u32>> sets_of(n);
  Vector<Vector<u32>> set_nodes(source.node_sets.size());
  for (u32 s = 0; s < source.node_sets.size(); ++s) {
    if (!detail::read_node_set(reference, source, source.node_sets[s].name, set_nodes[s]))
      return fail(error, "node set " + source.node_sets[s].name + " does not resolve");
    for (const u32 v : set_nodes[s]) {
      if (v >= n)
        return fail(error, "node set " + source.node_sets[s].name + " names node " + str(v));
      sets_of[v].push_back(s);
    }
  }
  for (Vector<u32>& s : sets_of) {
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
  }

  // ---- the corner tetrahedra
  Mesh m;
  const u32 cells = body.cell_count();
  m.tet.resize(cells * 4);
  m.alive.assign(cells, 1);
  m.star.resize(n);
  m.node_alive.assign(n, 0);
  m.boundary.assign(n, 0);
  m.stamp.assign(n, 0);
  for (u32 c = 0; c < cells; ++c) {
    for (u32 k = 0; k < 4; ++k) {
      m.tet[4 * c + k] = body.cell(c)[k];
      m.node_alive[body.cell(c)[k]] = 1;
      m.star[body.cell(c)[k]].push_back(c);
    }
    // Every carried state must be one the corner tetrahedra can represent: a ten-node cell valid
    // only through its curvature has a corner tetrahedron turned inside out, and a cage written
    // with it would fail in that state.
    const u32* t = m.tet.data() + 4 * c;
    for (const BodyState& s : body.states)
      if (!(query::tet_volume(d3(s.nodes[t[0]]), d3(s.nodes[t[1]]), d3(s.nodes[t[2]]),
                              d3(s.nodes[t[3]])) > 0.0))
        return fail(error,
                    "state " + s.name + ": cell " + str(c) +
                        "'s corner tetrahedron is not positive there — the ten-node cell is "
                        "valid in it only through its curvature, which a four-node cage does not "
                        "have — so a cage cannot carry the state; leave it out (--omit-state " +
                        s.name + ") or derive from a reference whose states its corners hold");
  }
  for (u32 v = 0; v < n; ++v)
    if (m.node_alive[v]) refresh_boundary(m, v);
  u32 alive_nodes = 0;
  for (u32 v = 0; v < n; ++v) {
    alive_nodes += m.node_alive[v];
    summary.corner_boundary_nodes += m.node_alive[v] != 0 && m.boundary[v] != 0 ? 1u : 0u;
  }
  summary.corner_nodes = alive_nodes;
  for (u32 s = 0; s < set_nodes.size(); ++s) {
    bool any = false;
    for (const u32 v : set_nodes[s])
      any = any || m.node_alive[v] != 0;
    if (!any)
      return fail(error, "node set " + source.node_sets[s].name +
                             " holds no corner node: a four-node cage cannot carry it");
  }

  // ---- the collapses
  Collapser collapser{body, sets_of, m};
  Vector<Entry> heap;
  const auto push = [&](u32 a, u32 b) {
    const Candidate c = collapser.evaluate(a, b);
    if (!c.valid) return;
    heap.push_back(Entry{c.tier, c.primary, c.secondary, a, b, m.stamp[a], m.stamp[b]});
    std::push_heap(heap.begin(), heap.end(), later);
  };
  const auto push_node = [&](u32 v) {
    Vector<u32> neighbours;
    for (const u32 t : m.star[v])
      for (u32 k = 0; k < 4; ++k)
        if (m.nodes(t)[k] != v) neighbours.push_back(m.nodes(t)[k]);
    std::sort(neighbours.begin(), neighbours.end());
    neighbours.erase(std::unique(neighbours.begin(), neighbours.end()), neighbours.end());
    for (const u32 w : neighbours) {
      push(v, w);
      push(w, v);
    }
  };
  if (alive_nodes > budget) {
    for (u32 v = 0; v < n; ++v)
      if (m.node_alive[v]) push_node(v);
  }
  while (alive_nodes > budget && !heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), later);
    const Entry e = heap.back();
    heap.pop_back();
    if (!m.node_alive[e.a] || !m.node_alive[e.b] || m.stamp[e.a] != e.stamp_a ||
        m.stamp[e.b] != e.stamp_b)
      continue;
    const Candidate c = collapser.evaluate(e.a, e.b);
    if (!c.valid) continue;
    if (c.tier == 0)
      ++summary.collapses_interior;
    else
      ++summary.collapses_boundary;
    summary.boundary_moved_max_m = std::max(summary.boundary_moved_max_m, c.deviation);
    const Vector<u32> touched = collapser.apply(e.a, e.b);
    --alive_nodes;
    for (const u32 v : touched)
      push_node(v);
  }
  if (alive_nodes > budget)
    return fail(error, "the collapses stopped at " + str(alive_nodes) +
                           " nodes, over the budget of " + str(budget) +
                           ": no further collapse keeps every cell positive in every "
                           "state and no worse than it was, the boundary unfolded and every node "
                           "set whole (a coarser cage needs a coarsened boundary, which this "
                           "derivation does not make)");

  // ---- the cage: live nodes in the reference's order, live cells in the cells' order
  Vector<u32> cage_of(n, query::k_none);
  Vector<u32> source_of;
  for (u32 v = 0; v < n; ++v)
    if (m.node_alive[v]) {
      cage_of[v] = source_of.size();
      source_of.push_back(v);
    }
  Vector<u32> tets;
  for (u32 t = 0; t < cells; ++t)
    if (m.alive[t])
      for (u32 k = 0; k < 4; ++k)
        tets.push_back(cage_of[m.nodes(t)[k]]);
  const u32 cage_nodes = source_of.size();
  const u32 cage_cells = tets.size() / 4;
  summary.cage_nodes = cage_nodes;
  summary.cage_cells = cage_cells;
  Vector<Vec3> nodes(cage_nodes);
  for (u32 i = 0; i < cage_nodes; ++i)
    nodes[i] = body.nodes[source_of[i]];
  summary.sicn_min = std::numeric_limits<f64>::infinity();
  for (u32 t = 0; t < cage_cells; ++t) {
    const u32* v = tets.data() + 4 * t;
    const f64 q =
        query::tet_sicn(d3(nodes[v[0]]), d3(nodes[v[1]]), d3(nodes[v[2]]), d3(nodes[v[3]]));
    if (q < summary.sicn_min) {
      summary.sicn_min = q;
      summary.sicn_min_cell = t;
    }
  }
  for (u32 v = 0; v < n; ++v)
    summary.cage_boundary_nodes += m.node_alive[v] != 0 && m.boundary[v] != 0 ? 1u : 0u;

  // ---- the phases by volume: each cage cell's fractions are the mean of the reference's over the
  // part of the reference's linear subdivision it overlaps.
  const u32 linear = body.parent.size();
  Vector<D3> lo(linear);
  Vector<D3> hi(linear);
  for (u32 t = 0; t < linear; ++t) {
    const u32* v = body.tets.data() + 4 * t;
    D3 a = d3(body.nodes[v[0]]);
    D3 b = a;
    for (u32 k = 1; k < 4; ++k) {
      const D3 p = d3(body.nodes[v[k]]);
      a = D3{std::min(a.x, p.x), std::min(a.y, p.y), std::min(a.z, p.z)};
      b = D3{std::max(b.x, p.x), std::max(b.y, p.y), std::max(b.z, p.z)};
    }
    lo[t] = a;
    hi[t] = b;
  }
  BoxGrid grid;
  grid.build(lo, hi);
  const u32 phases = body.phase_count;
  Vector<f64> cage_fraction(cage_cells * phases, 0.0);
  CageLedger& ledger = summary.ledger;
  const detail::BodyMass mass = detail::body_mass(body);
  ledger.source_volume_m3 = mass.volume_m3;
  ledger.source_mass_kg = mass.mass_kg;
  ledger.source_linear_volume_m3 = mass.linear_volume_m3;
  ledger.source_linear_mass_kg = mass.linear_mass_kg;
  Vector<u32> candidates;
  for (u32 t = 0; t < cage_cells; ++t) {
    const u32* v = tets.data() + 4 * t;
    D3 c[4];
    for (u32 k = 0; k < 4; ++k)
      c[k] = d3(nodes[v[k]]);
    D3 a = c[0];
    D3 b = c[0];
    for (u32 k = 1; k < 4; ++k) {
      a = D3{std::min(a.x, c[k].x), std::min(a.y, c[k].y), std::min(a.z, c[k].z)};
      b = D3{std::max(b.x, c[k].x), std::max(b.y, c[k].y), std::max(b.z, c[k].z)};
    }
    grid.query(a, b, candidates);
    f64 overlap = 0.0;
    for (const u32 r : candidates) {
      if (hi[r].x < a.x || lo[r].x > b.x || hi[r].y < a.y || lo[r].y > b.y || hi[r].z < a.z ||
          lo[r].z > b.z)
        continue;
      const u32* w = body.tets.data() + 4 * r;
      D3 q[4];
      for (u32 k = 0; k < 4; ++k)
        q[k] = d3(body.nodes[w[k]]);
      // A subdivision piece may be wound either way; the overlap takes both as they are.
      if (query::tet_volume(q[0], q[1], q[2], q[3]) < 0.0) std::swap(q[1], q[2]);
      const f64 shared = detail::tet_overlap(c, q);
      if (!(shared > 0.0)) continue;
      overlap += shared;
      const u32 parent = body.parent[r];
      for (u32 p = 0; p < phases; ++p)
        cage_fraction[t * phases + p] += shared * body.fractions[parent * phases + p];
      ledger.overlap_volume_m3 += shared;
      ledger.overlap_mass_kg += shared * body.density[parent];
    }
    if (overlap > 0.0) {
      for (u32 p = 0; p < phases; ++p)
        cage_fraction[t * phases + p] /= overlap;
    } else {
      // A cell the reference does not overlap at all: the reference cell whose corner it shares
      // most is not defined for it either; it takes the first cell holding its first node's.
      const u32 source_node = source_of[v[0]];
      for (u32 cell = 0; cell < cells; ++cell) {
        bool holds = false;
        for (u32 k = 0; k < body.per_cell; ++k)
          holds = holds || body.cell(cell)[k] == source_node;
        if (!holds) continue;
        for (u32 p = 0; p < phases; ++p)
          cage_fraction[t * phases + p] = body.fractions[cell * phases + p];
        break;
      }
    }
  }
  // The fractions as the file stores them (f32, the first phase what the others leave), and the
  // cage's mass exactly as region.materials computes it from them.
  Vector<Vector<f32>> stored(phases);
  for (u32 p = 1; p < phases; ++p) {
    stored[p].resize(cage_cells);
    for (u32 t = 0; t < cage_cells; ++t)
      stored[p][t] = static_cast<f32>(std::clamp(cage_fraction[t * phases + p], 0.0, 1.0));
  }
  for (u32 t = 0; t < cage_cells; ++t) {
    const u32* v = tets.data() + 4 * t;
    const f64 volume =
        query::tet_volume(d3(nodes[v[0]]), d3(nodes[v[1]]), d3(nodes[v[2]]), d3(nodes[v[3]]));
    f64 rest = 1.0;
    f64 density = 0.0;
    for (u32 p = 1; p < phases; ++p) {
      const f64 f = static_cast<f64>(stored[p][t]);
      density += f * source.phases[p].material.density_kg_m3;
      rest -= f;
    }
    density += rest * source.phases[0].material.density_kg_m3;
    ledger.cage_volume_m3 += volume;
    ledger.cage_mass_kg += density * volume;
  }

  // ---- the file
  TissueDefinition& o = out.definition;
  o.format = k_tissue_format;
  o.name = d.name + " (runtime cage)";
  o.description = "The runtime cage of region " + source.name + " of " + d.name + ", derived by " +
                  k_cage_method + " within " + str(budget) + " nodes.";
  o.units = d.units;
  o.provenance = "engine-content tissue cage (" + std::string(k_cage_method) + ") from " + d.name +
                 ", SHA-256 " + source_sha;
  const std::string prefix = "cage.";
  put(out, prefix + "nodes", BlockKind::RegionNodes, nodes);
  put(out, prefix + "tets", BlockKind::Tetrahedra, tets, 4);
  Region cage;
  cage.name = source.name;
  cage.kind = source.kind;
  cage.cage = CageKind::Tetrahedral;
  cage.nodes = prefix + "nodes";
  cage.tetrahedra = prefix + "tets";
  cage.hero = options.hero;
  cage.role = RegionRole::Runtime;
  for (u32 p = 0; p < phases; ++p) {
    MaterialPhase phase;
    phase.material = source.phases[p].material;
    if (p > 0) {
      phase.fraction = prefix + "phase." + str(p);
      put(out, phase.fraction, BlockKind::PhaseFraction, stored[p]);
    }
    cage.phases.push_back(phase);
  }
  for (u32 s = 0; s < source.node_sets.size(); ++s) {
    Vector<u32> mapped;
    for (const u32 v : set_nodes[s])
      if (v < n && cage_of[v] != query::k_none) mapped.push_back(cage_of[v]);
    std::sort(mapped.begin(), mapped.end());
    mapped.erase(std::unique(mapped.begin(), mapped.end()), mapped.end());
    NodeSetRef ref = source.node_sets[s];
    ref.nodes = prefix + "set." + str(s);
    put(out, ref.nodes, BlockKind::NodeSet, mapped);
    cage.node_sets.push_back(ref);
  }
  CageDerivation derivation;
  derivation.method = k_cage_method;
  derivation.source_sha256 = source_sha;
  derivation.source_definition = d.name;
  derivation.source_region = source.name;
  derivation.node_budget = budget;
  derivation.corner_nodes = summary.corner_nodes;
  for (const std::string& s : omitted)
    derivation.omitted_states.push_back(s);
  derivation.build_key = summary.build_key;
  derivation.source_volume_m3 = ledger.source_volume_m3;
  derivation.source_mass_kg = ledger.source_mass_kg;
  derivation.cage_volume_m3 = ledger.cage_volume_m3;
  derivation.cage_mass_kg = ledger.cage_mass_kg;
  derivation.note = str(summary.corner_nodes) + " corner nodes of " + str(summary.source_nodes) +
                    ", " + str(summary.collapses_interior) + " interior and " +
                    str(summary.collapses_boundary) + " boundary collapses";
  cage.derivation = derivation;
  o.regions.push_back(cage);

  // Frames, surfaces and frame states, untouched: their blocks by the reference's names.
  for (const Frame& f : d.frames) {
    o.frames.push_back(f);
    for (const std::string* name : {&f.vertices, &f.triangles, &f.cover, &f.cover_provenance})
      copy_block(reference, *name, out);
  }
  for (const Surface& s : d.surfaces) {
    o.surfaces.push_back(s);
    copy_block(reference, s.vertices, out);
    copy_block(reference, s.triangles, out);
  }
  // A frame state of a state the cage leaves out goes with it: it would name a state the file does
  // not have.
  for (const FrameState& fs : d.frame_states) {
    if (std::binary_search(omitted.begin(), omitted.end(), fs.state)) {
      summary.not_carried.push_back("frame " + fs.frame + "'s state at " + fs.state +
                                    " (the state is left out)");
      continue;
    }
    o.frame_states.push_back(fs);
  }
  for (const Attachment& a : d.attachments) {
    if (a.region != source.name) continue;
    if (a.target_kind == TargetKind::RegionSheet) {
      summary.not_carried.push_back("attachment " + a.name + " (to region sheet " + a.target + ")");
      continue;
    }
    o.attachments.push_back(a);
  }
  // Every state of the region: the reference's field at the cage's nodes, which are its own nodes.
  u32 state_index = 0;
  for (const RegionState& s : d.states) {
    if (s.region != source.name) continue;
    if (std::binary_search(omitted.begin(), omitted.end(), s.name)) {
      summary.not_carried.push_back("state " + s.name + " (left out: --omit-state)");
      continue;
    }
    const BodyState* bs = body.state(s.name);
    Vector<Vec3> at(cage_nodes);
    for (u32 i = 0; i < cage_nodes; ++i)
      at[i] = bs->nodes[source_of[i]];
    RegionState state = s;
    state.nodes = prefix + "state." + str(state_index++);
    state.expected_visible.clear();
    state.binding.clear();
    put(out, state.nodes, BlockKind::StateNodes, at);
    o.states.push_back(state);
  }
  // What the cage does not carry, said once.
  for (const Region& r : d.regions)
    if (r.name != source.name) summary.not_carried.push_back("region " + r.name);
  if (!source.sheets.empty())
    summary.not_carried.push_back(str(source.sheets.size()) +
                                  " Loop sheets and the shell stitch (the render binding's)");
  if (!source.membranes.empty())
    summary.not_carried.push_back(str(source.membranes.size()) + " membrane sets");
  if (!source.cables.empty())
    summary.not_carried.push_back(str(source.cables.size()) + " cable sets");
  if (source.rest_driver.has_value()) summary.not_carried.push_back("the active rest driver");
  if (!(d.observation == Observation{})) summary.not_carried.push_back("the observation");
  if (!d.bindings.empty()) summary.not_carried.push_back(str(d.bindings.size()) + " skin bindings");
  if (!d.budgets.empty()) summary.not_carried.push_back(str(d.budgets.size()) + " depth budgets");
  const auto layered = [&](const auto& list, const char* what) {
    if (!list.empty()) summary.not_carried.push_back(str(list.size()) + " " + what);
  };
  layered(d.parameter_domains, "parameter domains");
  layered(d.parameter_samples, "parameter samples");
  layered(d.thickness_fields, "thickness fields");
  layered(d.depot_partitions, "depot partitions");
  layered(d.material_surfaces, "material surfaces");
  layered(d.material_skins, "material skins");
  layered(d.contact_policies, "contact policies");
  layered(d.contact_pairs, "contact pairs");
  layered(d.certificates, "certificates");
  summary.carried.push_back(str(phases) + " material phases, by volume");
  summary.carried.push_back(str(source.node_sets.size()) + " node sets");
  summary.carried.push_back(str(o.attachments.size()) + " attachments");
  summary.carried.push_back(str(o.frames.size()) + " frames, " + str(o.frame_states.size()) +
                            " frame states and " + str(o.surfaces.size()) + " surfaces, untouched");
  summary.carried.push_back(str(o.states.size()) + " states");
  // The block table is the blocks', in their order; names are unique by construction.
  for (u32 i = 0; i < out.blocks.size(); ++i)
    for (u32 j = i + 1; j < out.blocks.size(); ++j)
      if (out.blocks[i].name == out.blocks[j].name)
        return fail(error, "block name '" + out.blocks[i].name + "' is used twice");
  return true;
}

JsonValue cage_summary_json(const CageSummary& s) {
  const auto num = [](f64 v) { return std::isfinite(v) ? JsonValue(v) : JsonValue::null(); };
  JsonValue out = JsonValue::object();
  out.set("method", JsonValue(k_cage_method));
  out.set("source_region", JsonValue(s.source_region));
  out.set("source_sha256", JsonValue(s.source_sha256));
  out.set("build_key", JsonValue(s.build_key));
  out.set("node_budget", JsonValue(s.node_budget));
  out.set("hero", JsonValue(s.hero));
  JsonValue omitted = JsonValue::array();
  for (const std::string& o : s.omitted_states)
    omitted.push_back(JsonValue(o));
  out.set("omitted_states", std::move(omitted));
  JsonValue source = JsonValue::object();
  source.set("nodes", JsonValue(s.source_nodes));
  source.set("cells", JsonValue(s.source_cells));
  source.set("corner_nodes", JsonValue(s.corner_nodes));
  source.set("corner_boundary_nodes", JsonValue(s.corner_boundary_nodes));
  out.set("source", std::move(source));
  JsonValue collapses = JsonValue::object();
  collapses.set("interior", JsonValue(s.collapses_interior));
  collapses.set("boundary", JsonValue(s.collapses_boundary));
  collapses.set("boundary_moved_max_mm", num(s.boundary_moved_max_m * 1.0e3));
  out.set("collapses", std::move(collapses));
  JsonValue cage = JsonValue::object();
  cage.set("nodes", JsonValue(s.cage_nodes));
  cage.set("cells", JsonValue(s.cage_cells));
  cage.set("boundary_nodes", JsonValue(s.cage_boundary_nodes));
  cage.set("sicn_min", num(s.sicn_min));
  cage.set("sicn_min_cell", JsonValue(s.sicn_min_cell));
  const physics::CageSizeVerdict v = physics::cage_size_verdict(s.cage_nodes, s.hero);
  const char* names[] = {"Ok", "Wide", "Refused"};
  cage.set("verdict", JsonValue(names[static_cast<u32>(v)]));
  out.set("cage", std::move(cage));
  const CageLedger& l = s.ledger;
  JsonValue ledger = JsonValue::object();
  ledger.set("source_volume_ml", num(l.source_volume_m3 * 1.0e6));
  ledger.set("source_mass_kg", num(l.source_mass_kg));
  ledger.set("source_linear_volume_ml", num(l.source_linear_volume_m3 * 1.0e6));
  ledger.set("source_linear_mass_kg", num(l.source_linear_mass_kg));
  ledger.set("cage_volume_ml", num(l.cage_volume_m3 * 1.0e6));
  ledger.set("cage_mass_kg", num(l.cage_mass_kg));
  ledger.set("difference_kg", num(l.source_mass_kg - l.cage_mass_kg));
  ledger.set(
      "difference_relative",
      num(l.source_mass_kg > 0.0 ? (l.source_mass_kg - l.cage_mass_kg) / l.source_mass_kg : 0.0));
  // The difference, closed: curvature + the reference outside the cage - the cage outside the
  // reference, an identity on these numbers.
  JsonValue split = JsonValue::object();
  split.set("curvature_kg", num(l.source_mass_kg - l.source_linear_mass_kg));
  split.set("source_outside_cage_kg", num(l.source_linear_mass_kg - l.overlap_mass_kg));
  split.set("cage_outside_source_kg", num(l.cage_mass_kg - l.overlap_mass_kg));
  split.set("overlap_volume_ml", num(l.overlap_volume_m3 * 1.0e6));
  ledger.set("split", std::move(split));
  out.set("mass_ledger", std::move(ledger));
  JsonValue carried = JsonValue::array();
  for (const std::string& c : s.carried)
    carried.push_back(JsonValue(c));
  out.set("carried", std::move(carried));
  JsonValue not_carried = JsonValue::array();
  for (const std::string& c : s.not_carried)
    not_carried.push_back(JsonValue(c));
  out.set("not_carried", std::move(not_carried));
  return out;
}

}  // namespace engine::tissue
