#include "body.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine::tissue::detail {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

template <class T>
bool read_block(const TissueFile& file, const std::string& name, BlockKind kind, Vector<T>& out,
                u32 per_element = 1) {
  const TissueBlock* b = file.find(name);
  if (b == nullptr || b->kind != kind) return false;
  out.resize(static_cast<u32>(b->count * per_element));
  if (out.size() * sizeof(T) != b->bytes.size()) return false;
  if (!b->bytes.empty()) std::memcpy(out.data(), b->bytes.data(), b->bytes.size());
  return true;
}

inline D3 min3(D3 a, D3 b) noexcept {
  return D3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline D3 max3(D3 a, D3 b) noexcept {
  return D3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

f64 det3(D3 a, D3 b, D3 c) noexcept { return query::dot(a, query::cross(b, c)); }

}  // namespace

f64 Body::cell_volume(u32 c, const Vector<Vec3>& x) const noexcept {
  D3 p[10];
  for (u32 k = 0; k < per_cell; ++k)
    p[k] = query::d3(x[cell(c)[k]]);
  return quadratic() ? cells::quadratic_volume(p) : query::tet_volume(p[0], p[1], p[2], p[3]);
}

const BodyState* Body::state(std::string_view name) const noexcept {
  for (const BodyState& s : states)
    if (s.name == name) return &s;
  return nullptr;
}

const BodyState* Body::state_with_role(StateRole role) const noexcept {
  for (const BodyState& s : states)
    if (s.def != nullptr && s.def->role == role) return &s;
  return nullptr;
}

bool read_node_set(const TissueFile& file, const Region& region, std::string_view set,
                   Vector<u32>& out) {
  for (const NodeSetRef& s : region.node_sets)
    if (s.name == set) return read_block(file, s.nodes, BlockKind::NodeSet, out);
  return false;
}

bool load_body(const TissueFile& file, std::string_view name, Body& out, std::string* error) {
  out = Body{};
  const TissueDefinition& d = file.definition;
  for (const Region& r : d.regions)
    if (r.name == name) out.region = &r;
  if (out.region == nullptr) return fail(error, "no region '" + std::string(name) + "'");
  const Region& region = *out.region;
  const std::string who = "region " + region.name;
  out.per_cell = cells::nodes_per_cell(region.cage);
  if (out.per_cell == 0)
    return fail(error, who + " is a " + cells::cage_kind_name(region.cage) +
                           " cage, which this build does not read");
  if (!read_block(file, region.nodes, BlockKind::RegionNodes, out.nodes))
    return fail(error, who + "'s nodes do not resolve to a RegionNodes block");
  if (!read_block(file, region.tetrahedra, cells::cell_block(region.cage), out.cells, out.per_cell))
    return fail(error, who + "'s cells do not resolve to a " +
                           block_kind_name(static_cast<u32>(cells::cell_block(region.cage))) +
                           " block");
  const u32 n = out.nodes.size();
  for (const u32 v : out.cells)
    if (v >= n) return fail(error, who + " has a cell naming node " + std::to_string(v));
  if (out.cells.empty()) return fail(error, who + " has no cells");
  const u32 count = out.cell_count();
  if (out.quadratic()) {
    for (u32 c = 0; c < count; ++c) {
      D3 x[10];
      for (u32 k = 0; k < 10; ++k)
        x[k] = query::d3(out.nodes[out.cell(c)[k]]);
      u32 pieces[32];
      cells::subdivide(out.cell(c), x, pieces);
      for (const u32 v : pieces)
        out.tets.push_back(v);
      for (u32 k = 0; k < 8; ++k)
        out.parent.push_back(c);
    }
  } else {
    out.tets = out.cells;
    for (u32 c = 0; c < count; ++c)
      out.parent.push_back(c);
  }
  BodyState construction;
  construction.name = "construction";
  construction.nodes = out.nodes;
  out.states.push_back(std::move(construction));
  for (const RegionState& s : d.states) {
    if (s.region != region.name) continue;
    BodyState state;
    state.name = s.name;
    state.def = &s;
    if (!read_block(file, s.nodes, BlockKind::StateNodes, state.nodes) || state.nodes.size() != n)
      return fail(error, who + " state " + s.name + " is not one StateNodes position per node");
    out.states.push_back(std::move(state));
  }
  if (region.phases.empty()) return fail(error, who + " has no material");
  out.phase_count = region.phases.size();
  out.fractions.assign(count * out.phase_count, 0.0);
  out.bulk_pa.assign(count, 0.0);
  out.shear_pa.assign(count, 0.0);
  out.density.assign(count, 0.0);
  Vector<Vector<f32>> declared(out.phase_count);
  for (u32 p = 1; p < out.phase_count; ++p)
    if (!read_block(file, region.phases[p].fraction, BlockKind::PhaseFraction, declared[p]) ||
        declared[p].size() != count)
      return fail(error,
                  who + " phase " + region.phases[p].material.name + " has no fraction per cell");
  for (u32 c = 0; c < count; ++c) {
    f64 first = 1.0;
    for (u32 p = 1; p < out.phase_count; ++p) {
      const f64 f = static_cast<f64>(declared[p][c]);
      out.fractions[c * out.phase_count + p] = f;
      first -= f;
    }
    out.fractions[c * out.phase_count] = first;
    for (u32 p = 0; p < out.phase_count; ++p) {
      const f64 f = out.fractions[c * out.phase_count + p];
      const Material& m = region.phases[p].material;
      out.bulk_pa[c] += f * m.bulk_modulus_pa;
      out.shear_pa[c] += f * m.shear_modulus_pa;
      out.density[c] += f * m.density_kg_m3;
    }
  }
  return true;
}

BodyMass body_mass(const Body& body) {
  BodyMass out;
  for (u32 c = 0; c < body.cell_count(); ++c) {
    const f64 v = body.cell_volume(c, body.nodes);
    out.volume_m3 += v;
    out.mass_kg += v * body.density[c];
  }
  for (u32 t = 0; t < body.parent.size(); ++t) {
    const u32* v = body.tets.data() + 4 * t;
    const f64 volume = query::tet_volume(query::d3(body.nodes[v[0]]), query::d3(body.nodes[v[1]]),
                                         query::d3(body.nodes[v[2]]), query::d3(body.nodes[v[3]]));
    out.linear_volume_m3 += volume;
    out.linear_mass_kg += volume * body.density[body.parent[t]];
  }
  return out;
}

// ---- locating a point
// ----------------------------------------------------------------------------

void shape_functions(const f64 l[4], u32 per_cell, f64 out[10]) noexcept {
  if (per_cell != 10) {
    for (u32 k = 0; k < 4; ++k)
      out[k] = l[k];
    return;
  }
  for (u32 k = 0; k < 4; ++k)
    out[k] = l[k] * (2.0 * l[k] - 1.0);
  for (u32 e = 0; e < 6; ++e)
    out[4 + e] = 4.0 * l[cells::k_quadratic_edges[e][0]] * l[cells::k_quadratic_edges[e][1]];
}

void CellLocator::build(const Body& body) {
  body_ = &body;
  const u32 count = body.cell_count();
  lo_.assign(count, D3{});
  hi_.assign(count, D3{});
  D3 all_lo{std::numeric_limits<f64>::infinity(), std::numeric_limits<f64>::infinity(),
            std::numeric_limits<f64>::infinity()};
  D3 all_hi{-all_lo.x, -all_lo.y, -all_lo.z};
  f64 extent = 0.0;
  for (u32 c = 0; c < count; ++c) {
    D3 lo = query::d3(body.nodes[body.cell(c)[0]]);
    D3 hi = lo;
    for (u32 k = 1; k < body.per_cell; ++k) {
      const D3 p = query::d3(body.nodes[body.cell(c)[k]]);
      lo = min3(lo, p);
      hi = max3(hi, p);
    }
    lo_[c] = lo;
    hi_[c] = hi;
    all_lo = min3(all_lo, lo);
    all_hi = max3(all_hi, hi);
    extent += std::max(hi.x - lo.x, std::max(hi.y - lo.y, hi.z - lo.z));
  }
  // Voxels about a cell's size: a handful of cells a voxel.
  voxel_ = count > 0 ? extent / static_cast<f64>(count) : 1.0;
  if (!(voxel_ > 0.0)) voxel_ = 1.0;
  origin_ = all_lo;
  const f64 span[3] = {all_hi.x - all_lo.x, all_hi.y - all_lo.y, all_hi.z - all_lo.z};
  for (u32 a = 0; a < 3; ++a) {
    const f64 cells_along = std::floor(span[a] / voxel_) + 1.0;
    dims_[a] = static_cast<u32>(std::min(cells_along, 256.0));
  }
  // Make the voxel cover the whole box along the axis the cap cut short.
  for (u32 a = 0; a < 3; ++a)
    voxel_ = std::max(voxel_, span[a] / static_cast<f64>(dims_[a]) * 1.0000001);
  const auto voxel_of = [&](f64 v, u32 axis, f64 o) {
    const f64 i = std::floor((v - o) / voxel_);
    return static_cast<u32>(std::clamp(i, 0.0, static_cast<f64>(dims_[axis] - 1)));
  };
  const u32 total = dims_[0] * dims_[1] * dims_[2];
  Vector<u32> counts(total + 1, 0u);
  const auto each_voxel = [&](u32 c, auto&& visit) {
    const u32 x0 = voxel_of(lo_[c].x, 0, origin_.x);
    const u32 x1 = voxel_of(hi_[c].x, 0, origin_.x);
    const u32 y0 = voxel_of(lo_[c].y, 1, origin_.y);
    const u32 y1 = voxel_of(hi_[c].y, 1, origin_.y);
    const u32 z0 = voxel_of(lo_[c].z, 2, origin_.z);
    const u32 z1 = voxel_of(hi_[c].z, 2, origin_.z);
    for (u32 z = z0; z <= z1; ++z)
      for (u32 y = y0; y <= y1; ++y)
        for (u32 x = x0; x <= x1; ++x)
          visit((z * dims_[1] + y) * dims_[0] + x);
  };
  for (u32 c = 0; c < count; ++c)
    each_voxel(c, [&](u32 v) { ++counts[v + 1]; });
  for (u32 v = 0; v < total; ++v)
    counts[v + 1] += counts[v];
  voxel_start_ = counts;
  voxel_cells_.assign(counts[total], 0u);
  Vector<u32> fill = counts;
  for (u32 c = 0; c < count; ++c)
    each_voxel(c, [&](u32 v) { voxel_cells_[fill[v]++] = c; });
}

bool CellLocator::try_cell(u32 c, D3 p, Location& out) const {
  const Body& b = *body_;
  const u32* nodes = b.cell(c);
  D3 x[10];
  for (u32 k = 0; k < b.per_cell; ++k)
    x[k] = query::d3(b.nodes[nodes[k]]);
  // The corner tetrahedron's barycentric coordinates: exact for a four-node cell, and Newton's
  // starting point for a ten-node one.
  const D3 e1 = x[1] - x[0];
  const D3 e2 = x[2] - x[0];
  const D3 e3 = x[3] - x[0];
  const f64 det = det3(e1, e2, e3);
  if (!(std::fabs(det) > 0.0)) return false;
  const D3 r = p - x[0];
  f64 xi[3] = {det3(r, e2, e3) / det, det3(e1, r, e3) / det, det3(e1, e2, r) / det};
  if (b.quadratic()) {
    // Newton on x(xi) = p, the Jacobian's columns the derivatives of the shape functions; it stops
    // when a step or the residual is below 1e-13 of the cell's first edge.
    const f64 scale = std::sqrt(query::dot(e1, e1));
    for (u32 it = 0; it < 32; ++it) {
      const f64 l[4] = {1.0 - xi[0] - xi[1] - xi[2], xi[0], xi[1], xi[2]};
      f64 n[10];
      shape_functions(l, 10, n);
      D3 at{};
      for (u32 k = 0; k < 10; ++k)
        at = at + x[k] * n[k];
      // dN/dl for every node, then dN/dxi_j = dN/dl_j - dN/dl_0.
      D3 col[3] = {{}, {}, {}};
      for (u32 k = 0; k < 4; ++k) {
        const f64 dk = 4.0 * l[k] - 1.0;  // dN_k / dl_k
        for (u32 j = 0; j < 3; ++j) {
          const f64 d = (k == j + 1 ? dk : 0.0) - (k == 0 ? dk : 0.0);
          col[j] = col[j] + x[k] * d;
        }
      }
      for (u32 e = 0; e < 6; ++e) {
        const u32 a = cells::k_quadratic_edges[e][0];
        const u32 c2 = cells::k_quadratic_edges[e][1];
        // N = 4 l_a l_b: dN/dl_a = 4 l_b, dN/dl_b = 4 l_a.
        for (u32 j = 0; j < 3; ++j) {
          f64 d = 0.0;
          if (a == j + 1) d += 4.0 * l[c2];
          if (c2 == j + 1) d += 4.0 * l[a];
          if (a == 0) d -= 4.0 * l[c2];
          if (c2 == 0) d -= 4.0 * l[a];
          col[j] = col[j] + x[4 + e] * d;
        }
      }
      const D3 residual = p - at;
      const f64 jd = det3(col[0], col[1], col[2]);
      if (!(std::fabs(jd) > 0.0)) break;
      const f64 d0 = det3(residual, col[1], col[2]) / jd;
      const f64 d1 = det3(col[0], residual, col[2]) / jd;
      const f64 d2 = det3(col[0], col[1], residual) / jd;
      xi[0] += d0;
      xi[1] += d1;
      xi[2] += d2;
      if (!(std::fabs(xi[0]) < 1.0e3 && std::fabs(xi[1]) < 1.0e3 && std::fabs(xi[2]) < 1.0e3))
        return false;
      const f64 step = std::fabs(d0) + std::fabs(d1) + std::fabs(d2);
      if (step < 1.0e-14 || std::sqrt(query::dot(residual, residual)) < 1.0e-13 * scale) break;
    }
  }
  out.cell = c;
  out.l[0] = 1.0 - xi[0] - xi[1] - xi[2];
  out.l[1] = xi[0];
  out.l[2] = xi[1];
  out.l[3] = xi[2];
  f64 least = out.l[0];
  for (u32 k = 1; k < 4; ++k)
    least = std::min(least, out.l[k]);
  out.outside = least < 0.0 ? -least : 0.0;
  return true;
}

CellLocator::Location CellLocator::locate(D3 p) const {
  Location best;
  best.outside = std::numeric_limits<f64>::infinity();
  if (body_ == nullptr || lo_.empty()) return best;
  const auto consider = [&](u32 c) {
    Location at;
    if (!try_cell(c, p, at)) return;
    // The least outside wins; a tie goes to the lower cell, which the ascending walk gives.
    if (at.outside < best.outside) best = at;
  };
  const auto voxel = [&](f64 v, f64 o) {
    const f64 i = std::floor((v - o) / voxel_);
    // A point far off the grid is clamped to a ring past its edge: the whole-body walk finds it.
    return static_cast<i64>(std::clamp(i, -2.0, 1.0e6));
  };
  const i64 vx = voxel(p.x, origin_.x);
  const i64 vy = voxel(p.y, origin_.y);
  const i64 vz = voxel(p.z, origin_.z);
  // The voxel's cells, then its neighbours', then every cell: a point inside the body is found in
  // the first; one just outside it in the second; one far outside in the last.
  for (i64 ring = 0; ring <= 1; ++ring) {
    Vector<u32> candidates;
    for (i64 z = vz - ring; z <= vz + ring; ++z)
      for (i64 y = vy - ring; y <= vy + ring; ++y)
        for (i64 x = vx - ring; x <= vx + ring; ++x) {
          if (x < 0 || y < 0 || z < 0 || x >= dims_[0] || y >= dims_[1] || z >= dims_[2]) continue;
          const u32 v = static_cast<u32>((z * dims_[1] + y) * dims_[0] + x);
          for (u32 i = voxel_start_[v]; i < voxel_start_[v + 1]; ++i)
            candidates.push_back(voxel_cells_[i]);
        }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const u32 c : candidates) {
      // A box test first: a point well outside a cell's box is not in it.
      const f64 margin = 1.0e-9 + 1.0e-6 * voxel_;
      if (ring == 0 &&
          (p.x < lo_[c].x - margin || p.x > hi_[c].x + margin || p.y < lo_[c].y - margin ||
           p.y > hi_[c].y + margin || p.z < lo_[c].z - margin || p.z > hi_[c].z + margin))
        continue;
      consider(c);
    }
    if (best.cell != query::k_none && best.outside <= 1.0e-9) return best;
  }
  for (u32 c = 0; c < lo_.size(); ++c)
    consider(c);
  return best;
}

D3 CellLocator::interpolate(const Location& at, const Vector<Vec3>& field) const {
  D3 out{};
  if (at.cell == query::k_none) return out;
  f64 n[10];
  shape_functions(at.l, body_->per_cell, n);
  for (u32 k = 0; k < body_->per_cell; ++k)
    out = out + query::d3(field[body_->cell(at.cell)[k]]) * n[k];
  return out;
}

// ---- the volume two tetrahedra share
// -------------------------------------------------------------

namespace {

struct Piece {
  D3 p[4];
};

// Splits a prism, ends (a0, a1, a2) and (b0, b1, b2) with a_k joined to b_k, into three tetrahedra.
void prism(D3 a0, D3 a1, D3 a2, D3 b0, D3 b1, D3 b2, Vector<Piece>& out) {
  out.push_back(Piece{{a0, a1, a2, b2}});
  out.push_back(Piece{{a0, a1, b1, b2}});
  out.push_back(Piece{{a0, b0, b1, b2}});
}

// The part of a tetrahedron on the side of a plane where s <= 0, as tetrahedra.
void clip(const Piece& t, D3 normal, f64 offset, Vector<Piece>& out) {
  f64 s[4];
  u32 in[4];
  u32 outside[4];
  u32 n_in = 0;
  u32 n_out = 0;
  for (u32 k = 0; k < 4; ++k) {
    s[k] = query::dot(normal, t.p[k]) - offset;
    if (s[k] <= 0.0)
      in[n_in++] = k;
    else
      outside[n_out++] = k;
  }
  const auto cut = [&](u32 i, u32 o) {
    const f64 w = s[i] / (s[i] - s[o]);  // s[i] <= 0 < s[o]
    return t.p[i] + (t.p[o] - t.p[i]) * w;
  };
  switch (n_in) {
    case 0: return;
    case 4: out.push_back(t); return;
    case 1: {
      const u32 i = in[0];
      out.push_back(Piece{{t.p[i], cut(i, outside[0]), cut(i, outside[1]), cut(i, outside[2])}});
      return;
    }
    case 2: {
      const u32 i0 = in[0];
      const u32 i1 = in[1];
      const u32 o0 = outside[0];
      const u32 o1 = outside[1];
      // A wedge: the edge (i0, i1) and the four cut points; ends (i0, c00, c01), (i1, c10, c11).
      prism(t.p[i0], cut(i0, o0), cut(i0, o1), t.p[i1], cut(i1, o0), cut(i1, o1), out);
      return;
    }
    default: {
      const u32 o = outside[0];
      // The tetrahedron less the corner beyond the plane: a prism from the inside face to the cut.
      prism(t.p[in[0]], t.p[in[1]], t.p[in[2]], cut(in[0], o), cut(in[1], o), cut(in[2], o), out);
      return;
    }
  }
}

}  // namespace

f64 tet_overlap(const D3 a[4], const D3 b[4]) noexcept {
  Vector<Piece> pieces;
  pieces.push_back(Piece{{a[0], a[1], a[2], a[3]}});
  Vector<Piece> next;
  // b's four faces, each opposite a corner, with the normal pointing away from that corner.
  const u32 faces[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
  for (u32 f = 0; f < 4; ++f) {
    const D3 p0 = b[faces[f][0]];
    D3 normal = query::cross(b[faces[f][1]] - p0, b[faces[f][2]] - p0);
    // Outward: away from the opposite corner, whatever the tetrahedron's orientation.
    if (query::dot(normal, b[f] - p0) > 0.0) normal = normal * -1.0;
    const f64 offset = query::dot(normal, p0);
    next.clear();
    for (const Piece& piece : pieces)
      clip(piece, normal, offset, next);
    pieces.swap(next);
    if (pieces.empty()) return 0.0;
  }
  f64 volume = 0.0;
  for (const Piece& piece : pieces)
    volume += std::fabs(query::tet_volume(piece.p[0], piece.p[1], piece.p[2], piece.p[3]));
  return volume;
}

// ---- boundaries ---------------------------------------------------------------------------------

Vector<u32> boundary_faces(const Vector<u32>& tets, const Vector<Vec3>& nodes) {
  struct Face {
    u32 key[3];
    u32 wound[3];
  };
  Vector<Face> faces;
  for (u32 t = 0; t + 3 < tets.size(); t += 4) {
    const u32 v[4] = {tets[t], tets[t + 1], tets[t + 2], tets[t + 3]};
    const u32 local[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
    const bool positive = query::tet_volume(query::d3(nodes[v[0]]), query::d3(nodes[v[1]]),
                                            query::d3(nodes[v[2]]), query::d3(nodes[v[3]])) > 0.0;
    for (const auto& f : local) {
      Face face;
      for (u32 k = 0; k < 3; ++k)
        face.wound[k] = v[f[k]];
      if (!positive) std::swap(face.wound[1], face.wound[2]);
      for (u32 k = 0; k < 3; ++k)
        face.key[k] = face.wound[k];
      std::sort(face.key, face.key + 3);
      faces.push_back(face);
    }
  }
  std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) {
    return std::lexicographical_compare(a.key, a.key + 3, b.key, b.key + 3);
  });
  Vector<u32> out;
  for (u32 i = 0; i < faces.size();) {
    u32 j = i + 1;
    while (j < faces.size() && std::equal(faces[i].key, faces[i].key + 3, faces[j].key))
      ++j;
    if (j - i == 1)
      for (u32 k = 0; k < 3; ++k)
        out.push_back(faces[i].wound[k]);
    i = j;
  }
  return out;
}

}  // namespace engine::tissue::detail
