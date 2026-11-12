// A network's cells (network.h; docs/subsystems/caves.md, "Cells"): the recipe a cell is built
// from, its surface (the field sampled on the world's lattice and meshed by surface nets), its
// dressing, its container, and the mesh generator "cavecell".
#include "field.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/caves/caves.h>
#include <domain/caves/network.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>

#include <algorithm>
#include <cstdlib>
#include <numbers>
#include <span>

namespace engine::caves {

using namespace detail;

namespace {

f64 margin_of(i32 apron_mm) noexcept {
  return std::max(1.0, static_cast<f64>(apron_mm) / 1000.0 + 0.75);
}

i32 reach_mm(const CellStation& s) noexcept {
  return std::max(std::max(s.a, s.b),
                  std::max(s.slot_hw, std::max(std::abs(s.slot_top), std::abs(s.slot_bot))));
}

// What every recipe of a plan says alike.
void recipe_head(const Plan& plan, CellRecipe& out) {
  out = CellRecipe{};
  out.format = std::string(k_cell_format);
  out.version = 1;
  out.seed = plan.network.seed;
  out.voxel_mm = static_cast<i32>(plan.voxel_mm);
  out.anchor_x = plan.anchor[0];
  out.anchor_y = plan.anchor[1];
  out.anchor_z = plan.anchor[2];
  out.formation = plan.formation;
  out.has_water = plan.has_water;
  out.water = plan.water_mm;
  out.apron_mm = static_cast<i32>(scene_gen::nearest_mm(static_cast<f64>(plan.network.apron_m)));
  out.ledge_mm = static_cast<i32>(scene_gen::nearest_mm(static_cast<f64>(plan.network.ledge_m)));
  out.scallop_mm = static_cast<i32>(scene_gen::nearest_mm(scallop_length(
      static_cast<f64>(plan.network.flow_speed_m_s), static_cast<f64>(plan.voxel_mm) / 1000.0)));
  out.base_color = plan.network.base_color;
}

// The field a recipe describes. It borrows the recipe's patch: the recipe outlives it.
void recipe_field(const CellRecipe& rc, Field& field) {
  gfx::FormationParams params;
  if (rc.formation.has_value()) gfx::pack_formation(*rc.formation, params, nullptr);
  field.frame = gfx::formation_frame(params);
  field.set_beds(params.beds);
  field.ax = rc.anchor_x;
  field.ay = rc.anchor_y;
  field.az = rc.anchor_z;
  field.anchor_m = {static_cast<f64>(rc.anchor_x) / 1000.0, static_cast<f64>(rc.anchor_y) / 1000.0,
                    static_cast<f64>(rc.anchor_z) / 1000.0};
  field.voxel_mm = rc.voxel_mm;
  field.margin = margin_of(rc.apron_mm);
  field.apron = static_cast<f64>(rc.apron_mm) / 1000.0;
  field.ledge = static_cast<f64>(rc.ledge_mm) / 1000.0;
  field.scallop = static_cast<f64>(rc.scallop_mm) / 1000.0;
  field.seed = rc.seed;
  field.has_water = rc.has_water;
  field.water_y = static_cast<f64>(rc.water - rc.anchor_y) / 1000.0;
  if (rc.patch_nx >= 2 && rc.patch_nz >= 2 && rc.patch.size() == rc.patch_nx * rc.patch_nz) {
    field.patch_i = rc.patch_i;
    field.patch_j = rc.patch_j;
    field.patch_nx = rc.patch_nx;
    field.patch_nz = rc.patch_nz;
    field.patch = rc.patch.data();
  }
  const V3 n{field.frame.normal[0], field.frame.normal[1], field.frame.normal[2]};
  const V3 strike = normalized(V3{-field.frame.dip[2], 0.0, field.frame.dip[0]}, V3{0.0, 0.0, 1.0});
  for (const CellStation& s : rc.stations)
    field.add_station(s, n, strike);
  for (const CellHall& h : rc.halls)
    field.add_hall(h);
  field.finish();
}

}  // namespace

void plan_field(const Plan& plan, Field& field) {
  CellRecipe all;
  recipe_head(plan, all);
  for (const PlanStation& ps : plan.stations)
    all.stations.push_back(ps.station);
  for (const PlanHall& h : plan.halls)
    all.halls.push_back(h.hall);
  recipe_field(all, field);
}

struct PlanField::Impl {
  Field field;
  i64 anchor[3] = {};
};

PlanField::PlanField(const Plan& plan) : impl_(std::make_unique<Impl>()) {
  plan_field(plan, impl_->field);
  for (u32 k = 0; k < 3; ++k)
    impl_->anchor[k] = plan.anchor[k];
}

PlanField::~PlanField() = default;

f64 PlanField::void_at(i64 x_mm, i64 y_mm, i64 z_mm) const {
  return impl_->field
      .sample(V3{static_cast<f64>(x_mm - impl_->anchor[0]) / 1000.0,
                 static_cast<f64>(y_mm - impl_->anchor[1]) / 1000.0,
                 static_cast<f64>(z_mm - impl_->anchor[2]) / 1000.0})
      .void_f;
}

void cell_recipe(const Plan& plan, u32 cell, u32 part, CellRecipe& out) {
  recipe_head(plan, out);
  const PlanCell& c = plan.cells[cell];
  out.part = part;
  out.cell = cell;
  out.lo_x = c.lo[0];
  out.lo_y = c.lo[1];
  out.lo_z = c.lo[2];
  out.hi_x = c.hi[0];
  out.hi_y = c.hi[1];
  out.hi_z = c.hi[2];
  if (part == 1) out.speleothems = plan.network.speleothems;
  // The cell's bounds, millimetres from the anchor, and what reaches them: a segment whose own
  // bounds (its stations' reach and the field's margin) touch the cell's is the cell's to know,
  // whichever cell owns it.
  const i64 pad = static_cast<i64>(margin_of(out.apron_mm) * 1000.0) + 1;
  i64 lo[3], hi[3];
  for (u32 k = 0; k < 3; ++k) {
    lo[k] = plan.anchor[k] + c.lo[k] * plan.voxel_mm;
    hi[k] = plan.anchor[k] + c.hi[k] * plan.voxel_mm;
  }
  const auto touches = [&](const i64 p[3], i64 rx, i64 ry) {
    return p[0] + rx >= lo[0] && p[0] - rx <= hi[0] && p[1] + ry >= lo[1] && p[1] - ry <= hi[1] &&
           p[2] + rx >= lo[2] && p[2] - rx <= hi[2];
  };
  Vector<u8> keep(plan.stations.size(), u8{0});
  for (u32 i = 0; i + 1 < plan.stations.size(); ++i) {
    const CellStation& s0 = plan.stations[i].station;
    const CellStation& s1 = plan.stations[i + 1].station;
    if (s0.edge != s1.edge) continue;
    const i64 r = std::max(reach_mm(s0), reach_mm(s1)) + pad + 1000;
    const i64 mid[3] = {(s0.x + s1.x) / 2, (s0.y + s1.y) / 2, (s0.z + s1.z) / 2};
    if (touches(mid, r, r)) keep[i] = keep[i + 1] = 1;
  }
  for (u32 i = 0; i < plan.stations.size(); ++i)
    if (keep[i] != 0) out.stations.push_back(plan.stations[i].station);
  for (const PlanHall& h : plan.halls) {
    const i64 p[3] = {h.hall.x, h.hall.y, h.hall.z};
    if (touches(p, std::max(h.hall.ra, h.hall.rb) + pad, h.hall.rh + pad))
      out.halls.push_back(h.hall);
  }
  // The ground: the union of the windows of the mouths that reach the cell, a column no mouth's
  // window covers left as rock.
  bool any = false;
  i64 wi0 = 0, wi1 = 0, wj0 = 0, wj1 = 0;
  for (const PlanMouth& m : plan.mouths) {
    const i64 i1 = m.patch_i + m.patch_nx - 1, j1 = m.patch_j + m.patch_nz - 1;
    if (i1 < c.lo[0] || m.patch_i > c.hi[0] || j1 < c.lo[2] || m.patch_j > c.hi[2]) continue;
    wi0 = any ? std::min(wi0, m.patch_i) : m.patch_i;
    wi1 = any ? std::max(wi1, i1) : i1;
    wj0 = any ? std::min(wj0, m.patch_j) : m.patch_j;
    wj1 = any ? std::max(wj1, j1) : j1;
    any = true;
  }
  if (any) {
    out.patch_i = wi0;
    out.patch_j = wj0;
    out.patch_nx = static_cast<u32>(wi1 - wi0 + 1);
    out.patch_nz = static_cast<u32>(wj1 - wj0 + 1);
    out.patch.assign(out.patch_nx * out.patch_nz, k_no_ground);
    for (const PlanMouth& m : plan.mouths) {
      const i64 i1 = m.patch_i + m.patch_nx - 1, j1 = m.patch_j + m.patch_nz - 1;
      if (i1 < c.lo[0] || m.patch_i > c.hi[0] || j1 < c.lo[2] || m.patch_j > c.hi[2]) continue;
      for (u32 j = 0; j < m.patch_nz; ++j)
        for (u32 i = 0; i < m.patch_nx; ++i)
          out.patch[static_cast<u32>(m.patch_j - wj0 + j) * out.patch_nx +
                    static_cast<u32>(m.patch_i - wi0 + i)] = m.patch[j * m.patch_nx + i];
    }
  }
}

std::string cell_recipe_json(const CellRecipe& recipe) {
  return write_json(schema::to_json(recipe), {.pretty = false});
}

u64 cell_source_hash(const CellRecipe& recipe) {
  const std::string text = cell_recipe_json(recipe);
  return hash_combine(hash_bytes(text.data(), text.size()), k_network_version);
}

bool read_cell_recipe(std::string_view text, CellRecipe& out, std::string& error) {
  out = CellRecipe{};
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = "cave cell: line " + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, root, ctx) || !ctx.ok()) {
    error = ctx.diagnostics.empty()
                ? std::string("cave cell: not an engine.caves.CellRecipe")
                : "cave cell: " + ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message;
    return false;
  }
  const auto fail = [&](const char* why) {
    error = std::string("cave cell: ") + why;
    return false;
  };
  if (out.format != k_cell_format) return fail("format is not \"engine.caves.cell\"");
  if (out.version != 1) return fail("version is not 1");
  if (out.voxel_mm != 125 && out.voxel_mm != 250 && out.voxel_mm != 500)
    return fail("voxel_mm is not 125, 250 or 500");
  if (out.part > 1) return fail("part is not 0 or 1");
  if (out.hi_x - out.lo_x < 2 || out.hi_y - out.lo_y < 2 || out.hi_z - out.lo_z < 2 ||
      out.hi_x - out.lo_x > 2048 || out.hi_y - out.lo_y > 2048 || out.hi_z - out.lo_z > 2048 ||
      (out.hi_x - out.lo_x + 1) * (out.hi_y - out.lo_y + 1) * (out.hi_z - out.lo_z + 1) > 96000000)
    return fail("the bounds are not 3 to 2,049 lattice points a side and 96 million in all");
  if (out.anchor_x % out.voxel_mm != 0 || out.anchor_y % out.voxel_mm != 0 ||
      out.anchor_z % out.voxel_mm != 0)
    return fail("the anchor is not a point of the lattice");
  if (out.stations.size() > 100000 || out.halls.size() > 64)
    return fail("more than 100,000 stations or 64 halls");
  if (out.patch.size() != static_cast<u64>(out.patch_nx) * out.patch_nz || out.patch_nx > 4096 ||
      out.patch_nz > 4096)
    return fail("the ground patch is not patch_nx by patch_nz");
  if (out.apron_mm < 0 || out.apron_mm > 6000 || out.ledge_mm < 0 || out.ledge_mm > 600 ||
      out.scallop_mm < 0 || out.scallop_mm > 5000)
    return fail("apron_mm, ledge_mm or scallop_mm is out of range");
  for (const CellStation& s : out.stations)
    if (s.a < 0 || s.b < 0 || s.slot_hw < 0 || s.a > 40000 || s.b > 40000 || s.slot_hw > 40000 ||
        s.slot_top < s.slot_bot || std::abs(s.slot_top) > 80000 || std::abs(s.slot_bot) > 80000 ||
        static_cast<u8>(s.frame) > static_cast<u8>(SectionFrame::Shaft))
      return fail("a station's section is out of range");
  for (const CellHall& h : out.halls)
    if (h.ra < 500 || h.rb < 500 || h.rh < 500 || h.ra > 100000 || h.rb > 100000 || h.rh > 100000 ||
        h.pile < 0)
      return fail("a hall's radii are out of range");
  if (!out.formation.has_value()) return fail("no formation");
  std::string why;
  if (!gfx::validate_formation(*out.formation, &why)) {
    error = "cave cell: " + why;
    return false;
  }
  return true;
}

namespace {

// ---- the rock: surface nets over the lattice ----------------------------------------------------

struct Tri {
  u32 v[3];
  u8 cls;
};

bool mesh_rock(const CellRecipe& rc, assets::MeshData& out, CellReport* report,
               std::string& error) {
  Field field;
  recipe_field(rc, field);
  const u32 nx = static_cast<u32>(rc.hi_x - rc.lo_x + 1);
  const u32 ny = static_cast<u32>(rc.hi_y - rc.lo_y + 1);
  const u32 nz = static_cast<u32>(rc.hi_z - rc.lo_z + 1);
  const u32 points = nx * ny * nz;
  const f64 step = static_cast<f64>(rc.voxel_mm) / 1000.0;
  Vector<f32> phi(points, 0.0f);
  Vector<u32> owner(points, k_no_cell);
  Vector<u8> info(points, u8{0});
  const auto at = [&](u32 i, u32 j, u32 k) { return (k * ny + j) * nx + i; };
  for (u32 k = 0; k < nz; ++k) {
    for (u32 j = 0; j < ny; ++j) {
      for (u32 i = 0; i < nx; ++i) {
        const V3 p{static_cast<f64>((rc.lo_x + i) * rc.voxel_mm) / 1000.0,
                   static_cast<f64>((rc.lo_y + j) * rc.voxel_mm) / 1000.0,
                   static_cast<f64>((rc.lo_z + k) * rc.voxel_mm) / 1000.0};
        const FieldSample s = field.sample(p);
        const u32 n = at(i, j, k);
        phi[n] = static_cast<f32>(std::max(-8.0, std::min(8.0, s.phi)));
        owner[n] = s.cell;
        info[n] = static_cast<u8>(s.cls | (s.ground ? 8u : 0u) | (s.raw < field.apron ? 16u : 0u));
      }
    }
  }
  // A vertex a cube the surface crosses: the mean of the crossings on its edges, on the world's
  // 1/1024 m grid; its normal the field's gradient over the cube, toward the air.
  const u32 cx = nx - 1, cy = ny - 1, cz = nz - 1;
  Vector<i32> cube_vertex(cx * cy * cz, i32{-1});
  Vector<V3> vpos, vnormal;
  static constexpr u8 corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                      {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
  static constexpr u8 edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  const auto cube_at = [&](u32 i, u32 j, u32 k) { return (k * cy + j) * cx + i; };
  for (u32 k = 0; k < cz; ++k) {
    for (u32 j = 0; j < cy; ++j) {
      for (u32 i = 0; i < cx; ++i) {
        f64 v[8];
        u32 inside = 0;
        for (u32 c = 0; c < 8; ++c) {
          v[c] = static_cast<f64>(phi[at(i + corner[c][0], j + corner[c][1], k + corner[c][2])]);
          if (v[c] < 0.0) ++inside;
        }
        if (inside == 0 || inside == 8) continue;
        f64 sum[3] = {0.0, 0.0, 0.0};
        u32 crossings = 0;
        for (u32 e = 0; e < 12; ++e) {
          const u32 a = edges[e][0], b = edges[e][1];
          if ((v[a] < 0.0) == (v[b] < 0.0)) continue;
          const f64 t = v[a] / (v[a] - v[b]);
          for (u32 d = 0; d < 3; ++d)
            sum[d] += static_cast<f64>(corner[a][d]) +
                      t * (static_cast<f64>(corner[b][d]) - static_cast<f64>(corner[a][d]));
          ++crossings;
        }
        const f64 inv = 1.0 / static_cast<f64>(crossings);
        const f64 lattice[3] = {static_cast<f64>(rc.lo_x + i) + sum[0] * inv,
                                static_cast<f64>(rc.lo_y + j) + sum[1] * inv,
                                static_cast<f64>(rc.lo_z + k) + sum[2] * inv};
        V3 p;
        p.x = std::floor(lattice[0] * step * 1024.0 + 0.5) / 1024.0;
        p.y = std::floor(lattice[1] * step * 1024.0 + 0.5) / 1024.0;
        p.z = std::floor(lattice[2] * step * 1024.0 + 0.5) / 1024.0;
        const V3 g{(v[1] + v[3] + v[5] + v[7]) - (v[0] + v[2] + v[4] + v[6]),
                   (v[2] + v[3] + v[6] + v[7]) - (v[0] + v[1] + v[4] + v[5]),
                   (v[4] + v[5] + v[6] + v[7]) - (v[0] + v[1] + v[2] + v[3])};
        cube_vertex[cube_at(i, j, k)] = static_cast<i32>(vpos.size());
        vpos.push_back(p);
        vnormal.push_back(normalized(g * -1.0));
      }
    }
  }
  // A quad a lattice edge the surface crosses, between the four cubes round the edge; it is this
  // cell's when the point on its air side is, so every quad of the network is exactly one cell's.
  Vector<Tri> tris;
  u32 crossed = 0, others = 0, grounds = 0, clipped = 0;
  const u32 dims[3] = {nx, ny, nz};
  for (u32 axis = 0; axis < 3; ++axis) {
    const u32 b_axis = (axis + 1) % 3, c_axis = (axis + 2) % 3;
    u32 q[3];
    for (q[2] = 0; q[2] < nz; ++q[2]) {
      for (q[1] = 0; q[1] < ny; ++q[1]) {
        for (q[0] = 0; q[0] < nx; ++q[0]) {
          if (q[axis] + 1 >= dims[axis]) continue;
          if (q[b_axis] == 0 || q[c_axis] == 0 || q[b_axis] + 1 >= dims[b_axis] ||
              q[c_axis] + 1 >= dims[c_axis])
            continue;
          u32 next[3] = {q[0], q[1], q[2]};
          ++next[axis];
          const u32 pa = at(q[0], q[1], q[2]), pb = at(next[0], next[1], next[2]);
          const bool air_a = phi[pa] < 0.0f, air_b = phi[pb] < 0.0f;
          if (air_a == air_b) continue;
          ++crossed;
          const u32 air = air_a ? pa : pb, rock = air_a ? pb : pa;
          if (owner[air] != rc.cell) {
            ++others;
            continue;
          }
          u8 cls = static_cast<u8>(info[rock] & 7u);
          if ((info[rock] & 8u) != 0) {
            // The ground: the mesh's own only as far as the apron reaches.
            if ((info[rock] & 16u) == 0) {
              ++grounds;
              continue;
            }
            cls = k_surface_apron;
          }
          u32 quad[4];
          const i32 offsets[4][2] = {{-1, -1}, {0, -1}, {0, 0}, {-1, 0}};
          bool whole = true;
          for (u32 n = 0; n < 4; ++n) {
            u32 cube[3] = {q[0], q[1], q[2]};
            cube[b_axis] = static_cast<u32>(static_cast<i32>(q[b_axis]) + offsets[n][0]);
            cube[c_axis] = static_cast<u32>(static_cast<i32>(q[c_axis]) + offsets[n][1]);
            const i32 vertex = cube_vertex[cube_at(cube[0], cube[1], cube[2])];
            if (vertex < 0) whole = false;
            quad[n] = static_cast<u32>(vertex);
          }
          if (!whole) {
            ++clipped;
            continue;
          }
          // Counter-clockwise about +axis as listed: its front is toward +axis. Turn it when the
          // air is on the other side.
          if (!air_b) std::swap(quad[1], quad[3]);
          // Split along the shorter diagonal where both triangles then face the air; the four
          // vertices are not in a plane, and where one diagonal folds a triangle over, the other.
          const f64 toward = air_b ? 1.0 : -1.0;
          const auto facing = [&](u32 a, u32 b, u32 c) {
            const V3 n = cross(vpos[quad[b]] - vpos[quad[a]], vpos[quad[c]] - vpos[quad[a]]);
            const f64 len = length(n);
            const f64 along = axis == 0 ? n.x : (axis == 1 ? n.y : n.z);
            return len > 0.0 ? toward * along / len : -1.0;
          };
          const f64 split_a = std::min(facing(0, 1, 2), facing(0, 2, 3));
          const f64 split_b = std::min(facing(0, 1, 3), facing(1, 2, 3));
          const V3 d02 = vpos[quad[2]] - vpos[quad[0]], d13 = vpos[quad[3]] - vpos[quad[1]];
          const bool both = split_a > 0.1 && split_b > 0.1;
          if (both ? dot(d02, d02) <= dot(d13, d13) : split_a >= split_b) {
            tris.push_back(Tri{{quad[0], quad[1], quad[2]}, cls});
            tris.push_back(Tri{{quad[0], quad[2], quad[3]}, cls});
          } else {
            tris.push_back(Tri{{quad[0], quad[1], quad[3]}, cls});
            tris.push_back(Tri{{quad[1], quad[2], quad[3]}, cls});
          }
        }
      }
    }
  }
  // Out: the triangles a class at a time, a vertex per (lattice vertex, projection axis) — the
  // UVs are the world's metres from the anchor on the plane the triangle faces, so they run on
  // from cell to cell, and the bedding reads level on every wall.
  const V3 origin{static_cast<f64>(rc.lo_x * rc.voxel_mm) / 1000.0,
                  static_cast<f64>(rc.lo_y * rc.voxel_mm) / 1000.0,
                  static_cast<f64>(rc.lo_z * rc.voxel_mm) / 1000.0};
  out = assets::MeshData{};
  Vector<i32> remap(vpos.size() * 3, i32{-1});
  CellReport rep;
  rep.lattice_points = points;
  rep.origin[0] = rc.anchor_x + rc.lo_x * rc.voxel_mm;
  rep.origin[1] = rc.anchor_y + rc.lo_y * rc.voxel_mm;
  rep.origin[2] = rc.anchor_z + rc.lo_z * rc.voxel_mm;
  for (u32 cls = 0; cls < k_surface_classes; ++cls) {
    assets::Primitive primitive;
    primitive.first_index = out.indices.size();
    primitive.material = static_cast<i32>(cls);
    for (const Tri& t : tris) {
      if (t.cls != cls) continue;
      const V3 a = vpos[t.v[0]], b = vpos[t.v[1]], c = vpos[t.v[2]];
      const V3 n = cross(b - a, c - a);
      if (dot(n, n) <= 0.0) continue;
      const f64 ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
      const u32 axis = (ay >= ax && ay >= az) ? 1u : (ax >= az ? 0u : 2u);
      // Where the surface folds inside a cube a quad's corner is on the far sheet and a triangle
      // comes out backwards: it is turned to face where the field falls, as its vertices' normals
      // say.
      const bool turned = dot(n, vnormal[t.v[0]] + vnormal[t.v[1]] + vnormal[t.v[2]]) < 0.0;
      const u32 corners[3] = {t.v[0], turned ? t.v[2] : t.v[1], turned ? t.v[1] : t.v[2]};
      for (u32 k = 0; k < 3; ++k) {
        i32& slot = remap[corners[k] * 3 + axis];
        if (slot < 0) {
          const V3 p = vpos[corners[k]], nn = vnormal[corners[k]];
          slot = static_cast<i32>(out.positions.size());
          out.positions.push_back(Vec3{static_cast<f32>(p.x - origin.x),
                                       static_cast<f32>(p.y - origin.y),
                                       static_cast<f32>(p.z - origin.z)});
          out.normals.push_back(
              Vec3{static_cast<f32>(nn.x), static_cast<f32>(nn.y), static_cast<f32>(nn.z)});
          const Vec2 uv = axis == 1
                              ? Vec2{static_cast<f32>(p.x), static_cast<f32>(p.z)}
                              : (axis == 0 ? Vec2{static_cast<f32>(p.z), static_cast<f32>(p.y)}
                                           : Vec2{static_cast<f32>(p.x), static_cast<f32>(p.y)});
          out.uvs.push_back(uv);
        }
        out.indices.push_back(static_cast<u32>(slot));
      }
      ++rep.class_triangles[cls];
    }
    primitive.index_count = out.indices.size() - primitive.first_index;
    if (primitive.index_count > 0) out.primitives.push_back(primitive);
  }
  rep.vertices = out.positions.size();
  rep.triangles = out.indices.size() / 3;
  if (rep.triangles == 0) {
    error = "cave cell " + std::to_string(rc.cell) + " has no surface inside its bounds (" +
            std::to_string(crossed) +
            " lattice edges cross the surface: " + std::to_string(others) + " another cell's, " +
            std::to_string(grounds) + " the open ground's, " + std::to_string(clipped) +
            " at the bounds' edge; " + std::to_string(rc.stations.size()) + " stations)";
    return false;
  }
  // The five surfaces' fallback colours, linear; a pack's sets replace them (the container).
  const Vec3 rock = (rc.base_color.x > 0.0f || rc.base_color.y > 0.0f || rc.base_color.z > 0.0f)
                        ? rc.base_color
                        : Vec3{0.55f, 0.50f, 0.42f};
  const Vec3 colors[k_surface_classes] = {rock, Vec3{0.30f, 0.24f, 0.17f},
                                          Vec3{0.50f, 0.46f, 0.40f}, Vec3{0.02f, 0.03f, 0.035f},
                                          Vec3{rock.x * 0.85f, rock.y * 0.86f, rock.z * 0.9f}};
  const f32 rough[k_surface_classes] = {0.85f, 0.95f, 0.9f, 0.05f, 0.9f};
  const char* names[k_surface_classes] = {"limestone", "sediment", "breakdown", "water", "apron"};
  for (u32 cls = 0; cls < k_surface_classes; ++cls) {
    assets::Material m;
    m.name = names[cls];
    m.base_color = Vec4{colors[cls].x, colors[cls].y, colors[cls].z, 1.0f};
    m.metallic = 0.0f;
    m.roughness = rough[cls];
    out.materials.push_back(m);
  }
  if (report != nullptr) *report = rep;
  return true;
}

// ---- the dressing -------------------------------------------------------------------------------

// The ideal stalactite's radius `along` metres from its tip (Short, Baygents and Goldstein 2005:
// radius as length to the 3/4), from the drop's bore at the tip: a square root of a square root.
f64 ideal_radius(f64 along, f64 len, f64 base, f64 tip) noexcept {
  const f64 x = clamp01(along / len);
  return tip + (base - tip) * std::sqrt(x) * std::sqrt(std::sqrt(x));
}

struct Lathe {
  assets::MeshData& mesh;
  V3 origin;
  u32 sides;
  u32 triangles = 0;
  // A body of revolution about a plumb axis through (x, z): rings of `radius` at heights `y`,
  // listed upward, closed by a point at each end; wound outward.
  void turn(f64 x, f64 z, const f64* y, const f64* radius, u32 rings) {
    const u32 first = mesh.positions.size();
    const auto push = [&](V3 p, V3 n, f64 u, f64 v) {
      mesh.positions.push_back(Vec3{static_cast<f32>(p.x - origin.x),
                                    static_cast<f32>(p.y - origin.y),
                                    static_cast<f32>(p.z - origin.z)});
      mesh.normals.push_back(
          Vec3{static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z)});
      mesh.uvs.push_back(Vec2{static_cast<f32>(u), static_cast<f32>(v)});
    };
    for (u32 r = 0; r < rings; ++r) {
      const f64 slope = r + 1 < rings
                            ? (radius[r + 1] - radius[r]) / std::max(1e-6, y[r + 1] - y[r])
                            : (radius[r] - radius[r - 1]) / std::max(1e-6, y[r] - y[r - 1]);
      for (u32 s = 0; s <= sides; ++s) {
        const f64 angle =
            2.0 * std::numbers::pi * static_cast<f64>(s % sides) / static_cast<f64>(sides);
        const f64 c = det::cos(angle), sn = det::sin(angle);
        push(V3{x + radius[r] * c, y[r], z + radius[r] * sn}, normalized(V3{c, -slope, sn}),
             static_cast<f64>(s) / static_cast<f64>(sides) * 2.0 * std::numbers::pi *
                 std::max(radius[r], 0.02),
             y[r]);
      }
    }
    const u32 row = sides + 1;
    for (u32 r = 0; r + 1 < rings; ++r) {
      for (u32 s = 0; s < sides; ++s) {
        const u32 a = first + r * row + s, b = a + 1, c = a + row, d = c + 1;
        for (const u32 i : {a, c, b, b, c, d})
          mesh.indices.push_back(i);
        triangles += 2;
      }
    }
    // The ends: a fan to a point on the axis.
    for (u32 end = 0; end < 2; ++end) {
      const u32 r = end == 0 ? 0 : rings - 1;
      const u32 centre = mesh.positions.size();
      push(V3{x, y[r], z}, V3{0.0, end == 0 ? -1.0 : 1.0, 0.0}, 0.0, y[r]);
      for (u32 s = 0; s < sides; ++s) {
        const u32 a = first + r * row + s, b = a + 1;
        if (end == 0) {
          for (const u32 i : {centre, a, b})
            mesh.indices.push_back(i);
        } else {
          for (const u32 i : {centre, b, a})
            mesh.indices.push_back(i);
        }
        ++triangles;
      }
    }
  }
};

bool mesh_dressing(const CellRecipe& rc, assets::MeshData& out, CellReport* report,
                   std::string& error) {
  if (!rc.speleothems.has_value()) {
    error = "cave cell " + std::to_string(rc.cell) + ": a dressing recipe with no speleothems";
    return false;
  }
  const Speleothems& sp = *rc.speleothems;
  Field field;
  recipe_field(rc, field);
  const V3 origin{static_cast<f64>(rc.lo_x * rc.voxel_mm) / 1000.0,
                  static_cast<f64>(rc.lo_y * rc.voxel_mm) / 1000.0,
                  static_cast<f64>(rc.lo_z * rc.voxel_mm) / 1000.0};
  out = assets::MeshData{};
  CellReport rep;
  rep.origin[0] = rc.anchor_x + rc.lo_x * rc.voxel_mm;
  rep.origin[1] = rc.anchor_y + rc.lo_y * rc.voxel_mm;
  rep.origin[2] = rc.anchor_z + rc.lo_z * rc.voxel_mm;
  Lathe lathe{out, origin, sp.sides, 0};
  const f64 step = static_cast<f64>(rc.voxel_mm) / 1000.0;
  // Where the rock begins over (dir +1) or under (dir -1) a point in the void: marched a half
  // lattice step at a time, then halved six times. False when none is met within `reach`.
  const auto meet = [&](V3 from, f64 dir, f64 reach, f64& y_out, u8& cls) {
    f64 inside = from.y;
    for (f64 d = 0.5 * step; d <= reach; d += 0.5 * step) {
      const FieldSample s = field.sample(V3{from.x, from.y + dir * d, from.z});
      if (s.void_f >= 0.0) {
        f64 rock = from.y + dir * d;
        cls = s.cls;
        for (u32 k = 0; k < 6; ++k) {
          const f64 mid = 0.5 * (inside + rock);
          if (field.sample(V3{from.x, mid, from.z}).void_f >= 0.0) {
            rock = mid;
          } else {
            inside = mid;
          }
        }
        y_out = rock;
        return true;
      }
      inside = from.y + dir * d;
    }
    return false;
  };
  // A drip: a stalactite or a straw from the roof, a stalagmite under it, a column where they
  // would meet, a flowstone boss where the floor under it slopes.
  const f64 tip = static_cast<f64>(sp.straw_radius_m);
  const auto drip = [&](V3 p, u64 h) {
    if (rep.drips != 0 && lathe.triangles + 40u * sp.sides > sp.triangle_budget) return;
    if (field.sample(p).void_f >= -0.2) return;
    f64 roof = 0.0, ground = 0.0;
    u8 roof_cls = 0, floor_cls = 0;
    if (!meet(p, 1.0, 12.0, roof, roof_cls) || roof_cls != k_surface_rock) return;
    // Past a mouth the tube's roof is in the open air (the ground cut it away): no rock there and
    // no drip, nor within half a metre of the ground. (The first pictures had a stalactite
    // hanging in the sky at a mouth.)
    f64 open_y = 0.0;
    if (field.ground_at(p.x, p.z, open_y) && roof > open_y - 0.5) return;
    const bool has_floor = meet(p, -1.0, 30.0, ground, floor_cls);
    const f64 gap = has_floor ? roof - ground : 30.0;
    if (gap < 0.4) return;
    ++rep.drips;
    // Seated in the rock: the mesh's roof stands up to three quarters of a lattice step off the
    // field's zero the drip was marched to, and a body sunk less than that hangs in the air.
    const f64 sink = 0.08 + 0.75 * step;
    if (unit(mix64(h + 1)) < static_cast<f64>(sp.straw_share)) {
      const f64 len =
          std::min(0.5 * gap, static_cast<f64>(sp.straw_max_m) * (0.3 + 0.7 * unit(mix64(h + 2))));
      const f64 ys[2] = {roof - len, roof + sink};
      const f64 rs[2] = {tip, tip};
      lathe.turn(p.x, p.z, ys, rs, 2);
      ++rep.stalactites;
      return;
    }
    f64 len = static_cast<f64>(sp.stalactite_min_m) +
              (static_cast<f64>(sp.stalactite_max_m) - static_cast<f64>(sp.stalactite_min_m)) *
                  unit(mix64(h + 3)) * unit(mix64(h + 4));
    const bool wet = floor_cls == k_surface_water;
    f64 stand = 0.0;
    if (has_floor && !wet && unit(mix64(h + 5)) < 0.7)
      stand = std::min(static_cast<f64>(sp.stalagmite_max_m) * (0.25 + 0.75 * unit(mix64(h + 6))),
                       0.6 * gap);
    const bool column = stand > 0.0 && gap - len - stand < static_cast<f64>(sp.column_gap_m);
    if (column) {
      len = gap * 0.55;
      stand = gap - len;
    } else {
      len = std::min(len, gap - stand - 0.1);
      if (len < 0.08) return;
    }
    const f64 base = static_cast<f64>(sp.slenderness) * len;
    constexpr u32 k_rings = 7;
    f64 ys[k_rings + 1], rs[k_rings + 1];
    for (u32 r = 0; r <= k_rings; ++r) {
      const f64 along = len * static_cast<f64>(r) / static_cast<f64>(k_rings);
      ys[r] = roof - len + along + (r == k_rings ? sink : 0.0);
      rs[r] = ideal_radius(along, len, base, column ? 0.5 * base : tip);
    }
    lathe.turn(p.x, p.z, ys, rs, k_rings + 1);
    ++rep.stalactites;
    if (column) ++rep.columns;
    if (stand <= 0.0) return;
    // The floor's slope under the drip: on a slope the water runs and the calcite spreads.
    f64 beside = 0.0;
    u8 unused = 0;
    const bool sloped = meet(V3{p.x + 0.4, ground + 0.5, p.z}, -1.0, 2.0, beside, unused) &&
                        std::fabs(beside - ground) > 0.14;
    const f64 radius = static_cast<f64>(sp.stalagmite_radius_m) * (1.0 + unit(mix64(h + 7)));
    if (sloped && !column) {
      const f64 wide = 0.3 + 0.3 * unit(mix64(h + 8)), high = 0.15 + 0.15 * unit(mix64(h + 9));
      const f64 fy[5] = {ground - 0.12, ground + 0.1 * high, ground + 0.5 * high,
                         ground + 0.9 * high, ground + high};
      const f64 fr[5] = {1.25 * wide, wide, 0.8 * wide, 0.4 * wide, 0.05 * wide};
      lathe.turn(p.x, p.z, fy, fr, 5);
      ++rep.flowstones;
      return;
    }
    const f64 my[4] = {ground - sink, ground + 0.6 * stand, ground + 0.9 * stand, ground + stand};
    const f64 mr[4] = {radius * (column ? 1.6 : 1.25), radius, 0.8 * radius,
                       column ? 0.5 * base : 0.3 * radius};
    lathe.turn(p.x, p.z, my, mr, 4);
    ++rep.stalagmites;
  };
  // Drips stand in rows along the joints the water comes down: a joint every few metres of the
  // edge, most of them dry, a wet one a row of drips across the passage
  // (docs/experiments/cave-networks.md, "Speleothems").
  const f64 wet_share = clamp01(static_cast<f64>(sp.drip_per_m2) * 0.8);
  for (u32 i = 0; i + 1 < field.stations.size(); ++i) {
    const FieldStation& s0 = field.stations[i];
    const FieldStation& s1 = field.stations[i + 1];
    if (s0.edge != s1.edge || s1.index != s0.index + 1 || s0.shaft || s1.shaft) continue;
    const i64 joint_mm = 2500;
    const i64 bucket = static_cast<i64>(std::floor(s0.s * 1000.0)) / joint_mm;
    const u64 h = hash3(rc.seed, 0xD21Bu + s0.edge, static_cast<u64>(bucket));
    const f64 at_s =
        (static_cast<f64>(bucket) + 0.15 + 0.7 * unit(h)) * static_cast<f64>(joint_mm) / 1000.0;
    if (!(at_s >= s0.s && at_s < s1.s)) continue;
    if (unit(mix64(h + 11)) >= wet_share) continue;
    const f64 t = (at_s - s0.s) / std::max(1e-6, s1.s - s0.s);
    if ((t < 0.5 ? s0.cell : s1.cell) != rc.cell) continue;
    const V3 centre = lerp(s0.p, s1.p, t);
    const V3 across = normalized(lerp(s0.level, s1.level, t), s0.level);
    const f64 half = std::max(lerp(s0.a, s1.a, t), lerp(s0.hw, s1.hw, t)) * 0.85;
    f64 x = -half + 0.5 * unit(mix64(h + 12));
    for (u32 n = 0; x <= half && n < 64; ++n) {
      const u64 hd = hash3(h, n, 0xD0u);
      if (unit(hd) < 0.7) drip(centre + across * x, hd);
      x += 0.35 + 0.55 * unit(mix64(hd + 13));
    }
  }
  for (u32 k = 0; k < field.halls.size(); ++k) {
    const FieldHall& hall = field.halls[k];
    if (hall.cell != rc.cell) continue;
    for (f64 lx = -hall.ra + 1.0; lx < hall.ra - 1.0; lx += 2.5) {
      const u64 h = hash3(rc.seed, 0x4A11D21Bu + k, static_cast<u64>(static_cast<i64>(lx * 8.0)));
      if (unit(mix64(h + 11)) >= wet_share) continue;
      f64 lz = -hall.rb + unit(mix64(h + 12));
      for (u32 n = 0; lz < hall.rb && n < 96; ++n) {
        const u64 hd = hash3(h, n, 0xD1u);
        if (unit(hd) < 0.7)
          drip(hall.c + hall.axis * (lx + 0.4 * (unit(mix64(hd + 14)) - 0.5)) + hall.perp * lz +
                   V3{0.0, 0.45 * hall.rh, 0.0},
               hd);
        lz += 0.5 + 0.8 * unit(mix64(hd + 13));
      }
    }
  }
  rep.vertices = out.positions.size();
  rep.triangles = out.indices.size() / 3;
  assets::Primitive primitive;
  primitive.first_index = 0;
  primitive.index_count = out.indices.size();
  primitive.material = 0;
  out.primitives.push_back(primitive);
  assets::Material m;
  m.name = "calcite";
  m.base_color = Vec4{0.78f, 0.72f, 0.60f, 1.0f};
  m.metallic = 0.0f;
  m.roughness = 0.45f;
  out.materials.push_back(m);
  if (report != nullptr) *report = rep;
  return true;
}

}  // namespace

bool solve_cell(const CellRecipe& recipe, assets::MeshData& out, CellReport* report,
                std::string& error) {
  return recipe.part == 0 ? mesh_rock(recipe, out, report, error)
                          : mesh_dressing(recipe, out, report, error);
}

void boundary_vertices(const assets::MeshData& mesh, Vector<u8>& locked) {
  const u32 count = mesh.positions.size();
  locked.assign(count, u8{0});
  // Weld by exact position: a vertex per projection axis is one point.
  Vector<u32> order;
  for (u32 i = 0; i < count; ++i)
    order.push_back(i);
  const auto less = [&](u32 a, u32 b) {
    const Vec3 p = mesh.positions[a], q = mesh.positions[b];
    if (p.x != q.x) return p.x < q.x;
    if (p.y != q.y) return p.y < q.y;
    if (p.z != q.z) return p.z < q.z;
    return a < b;
  };
  std::sort(order.begin(), order.end(), less);
  Vector<u32> point(count, 0u);
  u32 points = 0;
  for (u32 i = 0; i < count; ++i) {
    if (i > 0) {
      const Vec3 p = mesh.positions[order[i - 1]], q = mesh.positions[order[i]];
      if (p.x != q.x || p.y != q.y || p.z != q.z) ++points;
    }
    point[order[i]] = points;
  }
  Vector<u8> open(points + 1, u8{0});
  Vector<u64> sides;
  for (const assets::Primitive& primitive : mesh.primitives) {
    sides.clear();
    for (u32 i = 0; i + 2 < primitive.index_count; i += 3) {
      for (u32 k = 0; k < 3; ++k) {
        const u32 a = point[mesh.indices[primitive.first_index + i + k]];
        const u32 b = point[mesh.indices[primitive.first_index + i + (k + 1) % 3]];
        sides.push_back((static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b));
      }
    }
    std::sort(sides.begin(), sides.end());
    for (u32 i = 0; i < sides.size();) {
      u32 j = i;
      while (j < sides.size() && sides[j] == sides[i])
        ++j;
      if (j - i == 1) {
        open[static_cast<u32>(sides[i] >> 32)] = 1;
        open[static_cast<u32>(sides[i] & 0xFFFFFFFFu)] = 1;
      }
      i = j;
    }
  }
  for (u32 i = 0; i < count; ++i)
    locked[i] = open[point[i]];
}

bool build_cell_container(const CellRecipe& recipe, const assets::MeshGeneratorInputs& inputs,
                          const geometry::ClusterLodOptions& lod, u32 page_bytes,
                          geometry::ClusterFileData& out, CellReport* report, std::string& error) {
  out = {};
  assets::MeshData mesh;
  if (!solve_cell(recipe, mesh, report, error)) return false;
  geometry::ClusterFileData result;
  geometry::AttributeSource attributes = assets::attribute_source(mesh);
  // The rock's open edges — a portal's ring, an apron's rim — and the seams between its surfaces
  // are kept by every cut, so two cells meet at every level and a floor never parts from its wall.
  Vector<u8> locked;
  if (recipe.part == 0) {
    boundary_vertices(mesh, locked);
    attributes.locked = std::span<const u8>(locked.data(), locked.size());
  }
  Vector<geometry::ClusterLodMesh> parts;
  Vector<u32> part_material;
  for (const assets::Primitive& primitive : mesh.primitives) {
    if (primitive.index_count == 0) continue;
    geometry::ClusterLodOptions options = lod;
    options.normal_cones = true;  // every surface is one-sided: its front is toward the air
    geometry::ClusterLodMesh part;
    std::string why;
    if (!geometry::build_cluster_lod(
            mesh.positions, {mesh.indices.data() + primitive.first_index, primitive.index_count},
            options, part, &why, attributes)) {
      error = "the cluster build refused cave cell " + std::to_string(recipe.cell) + ": " + why;
      return false;
    }
    parts.push_back(std::move(part));
    part_material.push_back(static_cast<u32>(primitive.material));
  }
  Vector<u32> part_of_cluster;
  if (!geometry::merge_cluster_lod(parts, result.mesh, &part_of_cluster, &error)) return false;
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions paging;
    paging.page_bytes = page_bytes;
    Vector<u32> order;
    if (!geometry::build_cluster_pages(result.mesh, paging, result.pages, &error, &order))
      return false;
    geometry::permute_cluster_array(order, part_of_cluster);
  }
  for (const u32 part : part_of_cluster)
    result.cluster_material.push_back(part_material[part]);
  for (const assets::Material& m : mesh.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = m.base_color;
    material.metallic = m.metallic;
    material.roughness = m.roughness;
    material.normal_scale = m.normal_scale;
    material.alpha = geometry::encode_alpha_word(m.alpha_mode, m.double_sided, m.alpha_cutoff);
    result.materials.push_back(material);
    result.material_sampling.push_back(
        geometry::encode_material_sampling(m.sampling, m.occlusion_strength));
  }
  // A pack's sets by role, placed stochastically so no repeat shows down a passage; the limestone
  // falls back to the pack's sandstone, the one rock set the engine's pack has today.
  const auto apply = [&](u32 material, std::string_view role, std::string_view fallback) {
    if (material >= result.materials.size()) return;
    const materials::MaterialSet* set =
        materials::resolve_set(nullptr, inputs.materials, role, nullptr);
    if (set == nullptr && !fallback.empty())
      set = materials::resolve_set(nullptr, inputs.materials, fallback, nullptr);
    if (set == nullptr) return;
    materials::MaterialSet applied = *set;
    applied.stochastic = true;
    materials::apply_set(applied, materials::UvLayout::metres, result, material);
  };
  if (recipe.part == 0) {
    apply(k_surface_rock, k_limestone_role, "rock/sandstone");
    apply(k_surface_sediment, k_sediment_role, "");
    apply(k_surface_breakdown, k_breakdown_role, "rock/sandstone");
    apply(k_surface_apron, k_limestone_role, "rock/sandstone");
  } else {
    apply(0, k_calcite_role, "");
  }
  if (!result.image_paths.empty()) texture::fill_cluster_texture_records(result);
  result.source_hash = cell_source_hash(recipe);
  result.build_key = assets::generated_mesh_build_key(result.source_hash, inputs, lod, page_bytes);
  result.generator = "cavecell";
  result.generator_kind = recipe.part == 0 ? "rock" : "dressing";
  result.generator_params = cell_recipe_json(recipe);
  out = std::move(result);
  return true;
}

namespace {

std::string_view text_of(std::span<const u8> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool hash_recipe(std::span<const u8> bytes, u64& out, std::string& error) {
  CellRecipe recipe;
  if (!read_cell_recipe(text_of(bytes), recipe, error)) return false;
  out = cell_source_hash(recipe);
  return true;
}
bool build_recipe(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                  const geometry::ClusterLodOptions& lod, u32 page_bytes,
                  geometry::ClusterFileData& out, std::string& error) {
  CellRecipe recipe;
  return read_cell_recipe(text_of(bytes), recipe, error) &&
         build_cell_container(recipe, inputs, lod, page_bytes, out, nullptr, error);
}
constexpr assets::MeshGeneratorDesc k_generator{
    .name = "cavecell", .source_hash = hash_recipe, .build = build_recipe};
const assets::MeshGeneratorRegistrar k_registrar{k_generator};

}  // namespace

}  // namespace engine::caves
