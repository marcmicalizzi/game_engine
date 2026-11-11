#include "detail.h"
#include "geology.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/rocks/cliffs.h>
#include <domain/rocks/ledges.h>
#include <domain/texture/material_textures.h>

#include <algorithm>
#include <cmath>
#include <limits>

// The ledge solver (rocks.md, "Exposed bedrock (G142)"; ledges.h says why it is shaped this way).
// Arithmetic only: heights in whole millimetres, then IEEE `+ - * /`, `sqrt` and `floor` in f64
// with contraction off, the fold's sine `det::sin`, so a patch is the same bytes everywhere.

namespace engine::rocks {
namespace {
using namespace detail;

// How far under the ground a tread's back goes on, so the line where rock meets soil is the depth
// buffer's and not two coplanar surfaces'.
constexpr f64 k_back_m = 0.08;
constexpr u32 k_max_rows = 8;
// A bed thicker than this parts into rows about this thick (flaggy weathering along its own
// laminae): an authoring choice, exposed-bedrock.md.
constexpr f64 k_parting_m = 0.45;
// How far an edge of a rock of rounding 1 is worn back, metres: sandstone's 0.4 is 12 cm.
constexpr f64 k_wear_m = 0.3;
// The far form has no joint blocks: every front keeps the blocks' mean share of its step.
constexpr f64 k_far_share = 0.8;
// UVs are cut into charts this many repeats square (the content build warns past 16).
constexpr f64 k_chart = 16.0;
// What a tread carries when the patch names no colour: a dark turf, linear.
constexpr f32 k_soil[3] = {0.07f, 0.09f, 0.04f};
// PolyVertex::lines: the level lines a vertex lies on.
constexpr u8 k_on_front = 1;
constexpr u8 k_on_joint_lo = 2;
constexpr u8 k_on_joint_hi = 4;
constexpr u8 k_on_corner_lo = 8;  // a corner worn in plan, at the block's low joint and its high
constexpr u8 k_on_corner_hi = 16;
constexpr u8 k_on_face = k_on_front | k_on_corner_lo | k_on_corner_hi;  // where rock stands
// A corner is worn this much further than an edge: it is attacked through three faces, an edge
// through two (docs/experiments/exposed-bedrock.md, "How a jointed block weathers").
constexpr f64 k_corner = 1.5;
// The least wear for which a joint's edge is worn round too (rocks.md, "A block is a body"):
// sandstone's 12 cm and limestone's 13.5 are, slate's 2.4 is not.
constexpr f64 k_rim_m = 0.04;

bool bad(std::string& error, const char* field, const char* range) {
  error = std::string("engine.rocks.LedgePatch.") + field + " must be " + range;
  return false;
}
bool in(f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; }

std::string canonical(const LedgePatch& p) {
  JsonWriteOptions o;
  o.pretty = false;
  JsonValue json = schema::to_json(p);
  drop_absent_sampling(json);
  return write_json(json, o);
}

f64 draw(u64 seed, i64 index, u64 salt) {
  return static_cast<f64>(mix64(seed ^ mix64(static_cast<u64>(index) + salt)) >> 11) *
         (1.0 / 9007199254740992.0);
}

// A resistant unit: a run of beds at or past the rule's resistance, top first, each bed parted
// into rows.
struct Unit {
  f64 top = 0.0;                   // its top plane's coordinate within the period, (0, period]
  f64 thickness = 0.0;             // across the beds
  f64 factor = 0.0;                // the resistance factor of its mean resistance
  u32 rows = 0;                    // its rows, top first (the last takes what is past k_max_rows)
  f64 depth[k_max_rows + 1] = {};  // across the beds from the top; depth[rows] is the thickness
  f64 lead[k_max_rows] = {};       // each row's bed's resistance less the unit's mean
  u32 bed = 0;                     // its top bed, whose tint and roughness the unit draws with
  // How much weaker the bed under it is, 0 to 1: how far its foot is undercut (rocks.md,
  // "A block is a body"). Nothing when every bed of the sequence stands.
  f64 undercut = 0.0;
};

void find_units(const gfx::FormationSequence& s, const ExposureThresholds& rule, u64 seed,
                Vector<Unit>& out) {
  const u32 n = s.count;
  bool hard[gfx::k_formation_max_beds] = {};
  u32 hard_count = 0;
  for (u32 i = 0; i < n; ++i) {
    hard[i] = resistance_factor(rule, wide(s.beds[i].resistance)) > 0.0;
    hard_count += hard[i] ? 1u : 0u;
  }
  if (hard_count == 0) return;
  const auto add = [&](u32 top_bed, u32 length) {
    Unit u;
    u.bed = top_bed;
    u.top = wide(s.beds[top_bed].start_m) + wide(s.beds[top_bed].thickness_m);
    f64 weighted = 0.0;
    for (u32 k = 0; k < length; ++k) {
      const u32 b = (top_bed + n - k % n) % n;
      const f64 t = wide(s.beds[b].thickness_m);
      weighted += t * wide(s.beds[b].resistance);
      u.thickness += t;
    }
    const f64 mean = weighted / u.thickness;
    u.factor = resistance_factor(rule, mean);
    f64 below = 0.0;
    for (u32 k = 0; k < length; ++k) {
      const u32 b = (top_bed + n - k % n) % n;
      const f64 t = wide(s.beds[b].thickness_m);
      // The bed's rows: unequal by a seeded weight each, so the partings are not a ruler's.
      u32 parts = static_cast<u32>(std::floor(t / k_parting_m + 0.5));
      parts = parts < 1 ? 1u : (parts > k_max_rows ? k_max_rows : parts);
      f64 weight[k_max_rows] = {};
      f64 total = 0.0;
      for (u32 q = 0; q < parts; ++q) {
        weight[q] = 0.6 + 0.8 * draw(seed, static_cast<i64>(b) * 16 + q, 0x70ull + out.size());
        total += weight[q];
      }
      for (u32 q = 0; q < parts; ++q) {
        below += t * weight[q] / total;
        const u32 row = u.rows < k_max_rows ? u.rows : k_max_rows - 1;
        u.depth[row + 1] = below;
        u.lead[row] = wide(s.beds[b].resistance) - mean;
        u.rows = row + 1;
      }
    }
    u.depth[u.rows] = u.thickness;
    if (length < n) {
      // The bed under the unit's lowest weathers back under it (Niemann 2007, the shale under
      // sandstone and limestone ledges of roadcuts): the contrast in resistance, over 0.6.
      const u32 under = (top_bed + 2 * n - length) % n;
      u.undercut = clamp01((mean - wide(s.beds[under].resistance)) / 0.6);
    }
    out.push_back(u);
  };
  if (hard_count == n) {
    add(n - 1, n);  // every bed stands: one unit a period, its top the period's
    return;
  }
  for (u32 i = 0; i < n; ++i) {
    if (!hard[i] || hard[(i + 1) % n]) continue;
    u32 length = 1;
    while (hard[(i + n - length % n) % n])
      ++length;
    add(i, length);
  }
}

// A joint's station along the strike: `spacing` apart on average, each moved by up to three
// tenths of it, so a block is one interval and the same one from every tile.
f64 station(i64 k, f64 spacing, u64 seed) {
  return (static_cast<f64>(k) + 0.6 * (draw(seed, k, 0x33ull) - 0.5)) * spacing;
}
i64 block_of(f64 strike, f64 spacing, u64 seed) {
  i64 k = static_cast<i64>(std::floor(strike / spacing));
  if (strike < station(k, spacing, seed))
    --k;
  else if (strike >= station(k + 1, spacing, seed))
    ++k;
  return k;
}
// A joint block as it stands: the share of the unit's step its front keeps at its middle (0: the
// block has fallen out, one in five), how that share changes along it (a front is not parallel
// to the ground's contour), where it is, and how many of the unit's rows it has lost off its top.
struct Block {
  f64 share = 0.0;
  f64 tilt = 0.0;
  f64 mid = 0.0;
  f64 length = 1.0;
  u32 lost = 0;
  // **Cambered** (rocks.md, "A block is a body"): a block that has crept out over the weak bed
  // under it. `slip_m` out along the ground's fall (and down with the ground, so it stays seated),
  // and `roll`: how far it has settled toward one of its joints. Zero for most.
  f64 slip_m = 0.0;
  f64 roll = 0.0;  // down toward its high joint, metres a metre along the strike
};
// The open width of joint `k` (between block k - 1 and block k), metres: a worn joint is never
// shut (half the rock's wear wide), and a few are opened into clefts by the blocks' creep, up to
// a fifth of the unit's thickness, 0.4 m and a fifth of the joints' spacing (Cooper's GCR account
// of the Bath University cutting: gulls up to 0.4 m wide in a massive bed 1.9 m thick;
// docs/experiments/exposed-bedrock.md). The cube of a draw: most are narrow, a few open.
f64 cleft_width(u64 seed, i64 k, f64 thickness, f64 spacing, f64 round_m) {
  const f64 d = draw(seed, k, 0xC1ull);
  const f64 open = std::min(std::min(0.4, 0.21 * thickness), 0.2 * spacing);
  return 0.5 * round_m + open * d * d * d;
}
Block block_at(u64 seed, i64 block, f64 spacing, u32 rows, f64 thickness) {
  Block b;
  const f64 lo = station(block, spacing, seed), hi = station(block + 1, spacing, seed);
  b.mid = 0.5 * (lo + hi);
  b.length = hi - lo;
  if (draw(seed, block, 0x51ull) < 0.2) return b;
  b.share = 0.55 + 0.45 * draw(seed, block, 0xA7ull);
  b.tilt = 0.7 * (draw(seed, block, 0x61ull) - 0.5);
  const f64 r = draw(seed, block, 0x67ull);
  b.lost = r < 0.6 ? 0u : (r < 0.9 ? 1u : 2u);
  if (b.lost + 1 > rows) b.lost = rows - 1;
  // One standing block in four has crept: out by up to a fifth of the unit's thickness (0.3 m at
  // most), and settled toward its high joint by 0.02 to 0.08 m a metre (1.1 to 4.6 degrees,
  // never so little that a part of its tread stays parallel to its bed). Cambered blocks over
  // thick caps are reported rotated 26 to 37 degrees; a ledge's block turned that far would have
  // toppled off its tread.
  if (draw(seed, block, 0xB1ull) < 0.25) {
    b.slip_m = std::min(0.3, 0.2 * thickness) * (0.25 + 0.75 * draw(seed, block, 0xB3ull));
    b.roll = 0.02 + 0.06 * draw(seed, block, 0xB7ull);
  }
  return b;
}
// The share of the step the block keeps at a place along the strike.
f64 kept_at(const Block& b, f64 strike) {
  if (!(b.share > 0.0)) return 0.0;
  return std::clamp(b.share + b.tilt * (strike - b.mid) / b.length, 0.2, 1.0);
}

// A vertex of a clipped lattice triangle: a corner, a point of one of its lattice edges (between
// corners `a` and `b`, `a` the earlier node of the world, at `t`), or a point inside it.
struct PolyVertex {
  f64 w[3] = {};
  u8 kind = 0;  // 0 corner, 1 lattice edge, 2 inside
  u8 a = 0;
  u8 b = 0;
  u8 lines = 0;  // k_on_*
  f64 t = 0.0;
};
f64 at(const PolyVertex& v, const f64 f[3]) {
  if (v.kind == 0) return f[v.a];
  if (v.kind == 1) return f[v.a] + v.t * (f[v.b] - f[v.a]);
  return v.w[0] * f[0] + v.w[1] * f[1] + v.w[2] * f[2];
}

// Where the segment P-Q crosses f = 0. On a lattice edge the crossing is computed from the
// edge's two ends in the world's order, so both triangles (and both tiles) get the same vertex.
PolyVertex crossing(const PolyVertex& p, const PolyVertex& q, const f64 f[3], const u32 node[3],
                    u8 line) {
  PolyVertex x;
  // On the new line, and on any joint or worn corner both ends are on.
  x.lines =
      static_cast<u8>(line | (p.lines & q.lines &
                              (k_on_joint_lo | k_on_joint_hi | k_on_corner_lo | k_on_corner_hi)));
  if (p.kind < 2 && q.kind < 2) {
    bool on[3] = {};
    on[p.a] = on[p.b] = on[q.a] = on[q.b] = true;
    const u32 count = (on[0] ? 1u : 0u) + (on[1] ? 1u : 0u) + (on[2] ? 1u : 0u);
    if (count == 2) {
      u8 lo = on[0] ? u8{0} : u8{1};
      u8 hi = on[2] ? u8{2} : u8{1};
      if (node[lo] > node[hi]) std::swap(lo, hi);
      x.kind = 1;
      x.a = lo;
      x.b = hi;
      x.t = f[lo] / (f[lo] - f[hi]);
      x.w[lo] = 1.0 - x.t;
      x.w[hi] = x.t;
      return x;
    }
  }
  const f64 fp = at(p, f);
  const f64 fq = at(q, f);
  const f64 t = fp / (fp - fq);
  x.kind = 2;
  for (u32 k = 0; k < 3; ++k)
    x.w[k] = p.w[k] + t * (q.w[k] - p.w[k]);
  return x;
}

// Sutherland-Hodgman against one level line: keeps f > 0 (`keep_positive`) or f < 0.
u32 clip(const PolyVertex* in_poly, u32 count, const f64 f[3], const u32 node[3],
         bool keep_positive, u8 line, PolyVertex* out_poly) {
  u32 made = 0;
  for (u32 i = 0; i < count; ++i) {
    const PolyVertex& p = in_poly[i];
    const PolyVertex& q = in_poly[(i + 1) % count];
    const f64 fp = at(p, f);
    const f64 fq = at(q, f);
    const bool p_in = keep_positive ? fp > 0.0 : fp < 0.0;
    const bool q_in = keep_positive ? fq > 0.0 : fq < 0.0;
    if (p_in) out_poly[made++] = p;
    if (p_in != q_in) out_poly[made++] = crossing(p, q, f, node, line);
  }
  return made;
}

struct Corner {
  u64 cls = 0;
  i64 cu = 0;
  i64 cv = 0;
  i64 qx = 0;
  i64 qy = 0;
  i64 qz = 0;
  Vec3 position{};
  Vec2 uv{};
  D3 normal{};
};
bool corner_less(const Corner& a, const Corner& b) {
  if (a.cls != b.cls) return a.cls < b.cls;
  if (a.cu != b.cu) return a.cu < b.cu;
  if (a.cv != b.cv) return a.cv < b.cv;
  if (a.qx != b.qx) return a.qx < b.qx;
  if (a.qy != b.qy) return a.qy < b.qy;
  return a.qz < b.qz;
}
bool corner_same(const Corner& a, const Corner& b) {
  return !corner_less(a, b) && !corner_less(b, a);
}
i64 quantum(f64 v) { return static_cast<i64>(std::floor(v * 8000.0 + 0.5)); }  // an eighth of a mm

enum class Facing { Up, Down, Toward };

struct Mesher {
  Vector<Corner> corners;
  // The material the next triangles draw with, and each triangle's: 0 the treads' soil, 1 + u
  // unit u's rock.
  u32 material = 0;
  Vector<u32> tri_material;
  f64 tile_m = 1.0;
  // Appends the triangle wound so its normal faces as asked; false for one with no area.
  bool emit(u64 cls, D3 p[3], f64 uv[3][2], Facing facing, D3 toward) {
    D3 n = cross(p[1] - p[0], p[2] - p[0]);
    if (dot(n, n) < 1e-16) return false;
    const bool ok = facing == Facing::Up     ? n.y > 0.0
                    : facing == Facing::Down ? n.y < 0.0
                                             : dot(n, toward) > 0.0;
    if (!ok) {
      std::swap(p[1], p[2]);
      std::swap(uv[1][0], uv[2][0]);
      std::swap(uv[1][1], uv[2][1]);
      n = n * -1.0;
    }
    const f64 mu = (uv[0][0] + uv[1][0] + uv[2][0]) / (3.0 * tile_m);
    const f64 mv = (uv[0][1] + uv[1][1] + uv[2][1]) / (3.0 * tile_m);
    const f64 cu = std::floor(mu / k_chart);
    const f64 cv = std::floor(mv / k_chart);
    for (u32 k = 0; k < 3; ++k) {
      Corner c;
      c.cls = cls;
      c.cu = static_cast<i64>(cu);
      c.cv = static_cast<i64>(cv);
      c.qx = quantum(p[k].x);
      c.qy = quantum(p[k].y);
      c.qz = quantum(p[k].z);
      c.position = {static_cast<f32>(p[k].x), static_cast<f32>(p[k].y), static_cast<f32>(p[k].z)};
      c.uv = {static_cast<f32>(uv[k][0] / tile_m - cu * k_chart),
              static_cast<f32>(uv[k][1] / tile_m - cv * k_chart)};
      c.normal = n;
      corners.push_back(c);
    }
    tri_material.push_back(material);
    return true;
  }
};

}  // namespace

bool ledge_patch_in_range(const LedgePatch& p, std::string& error) {
  if (!valid_rock_type(p.type)) return bad(error, "type", "a rock type");
  std::string why;
  if (!gfx::validate_formation(p.formation, &why)) {
    error = "engine.rocks.LedgePatch." + why;
    return false;
  }
  ExposureThresholds rule;
  if (!prepare_exposure(p.rule, rule, error)) return false;
  if (!(p.cell_mm >= 250 && p.cell_mm <= 16000)) return bad(error, "cell_mm", "in [250, 16000]");
  if (!(p.cells >= 1 && p.cells <= 256)) return bad(error, "cells", "in [1, 256]");
  if (!(p.apron >= 1 && p.apron <= 64) ||
      static_cast<i64>(p.apron) * static_cast<i64>(p.cell_mm) != rule.arm_mm)
    return bad(error, "apron", "in [1, 64] and rule.scale_m over the cell");
  const u64 side = static_cast<u64>(p.cells) + 1 + 2 * static_cast<u64>(p.apron);
  if (static_cast<u64>(p.heights_mm.size()) != side * side)
    return bad(error, "heights_mm", "(cells + 1 + 2 apron) squared heights");
  for (const i64 v : {p.x_mm, p.y_mm, p.z_mm})
    if (v < -100000000000ll || v > 100000000000ll)
      return bad(error, "x_mm, y_mm, z_mm", "within 100,000 km");
  if (!in(wide(p.joint_ratio), 0.5, 4.0)) return bad(error, "joint_ratio", "in [0.5, 4]");
  if (!in(wide(p.max_step_m), 0.2, 30.0)) return bad(error, "max_step_m", "in [0.2, 30]");
  if (!in(wide(p.min_step_m), 0.02, 30.0)) return bad(error, "min_step_m", "in [0.02, 30]");
  if (!in(wide(p.bury_m), 0.05, 5.0)) return bad(error, "bury_m", "in [0.05, 5]");
  if (!in(wide(p.relief_m), 0.0, 2.0)) return bad(error, "relief_m", "in [0, 2]");
  if (!(p.cliff_slope_deg == 0.0f || in(wide(p.cliff_slope_deg), 30.0, 85.0)))
    return bad(error, "cliff_slope_deg", "0 or in [30, 85]");
  if (p.tread_tint.has_value())
    for (const f32 channel : *p.tread_tint)
      if (!(channel >= 0.0f && channel <= 1.0f)) return bad(error, "tread_tint", "in [0, 1]");
  if (p.material.surface_maps.has_value())
    return bad(error, "material.surface_maps", "absent (a patch takes a tiled set)");
  return material_factors_in_range(p.material, error);
}

bool sample_ledge_patch(const LedgeLayer& layer, const scene_gen::Ground& ground, i64 x_mm,
                        i64 z_mm, i64 tile_mm, bool far, LedgePatch& out, std::string& error) {
  out = {};
  const auto refuse = [&](const char* why) {
    error = std::string("engine.rocks.LedgeLayer: ") + why;
    return false;
  };
  if (!layer.formation.has_value()) return refuse("formation is required (the scene's, repeated)");
  const i64 cell_mm = scene_gen::nearest_mm(wide(far ? layer.far_cell_m : layer.cell_m));
  const i64 scale_mm = scene_gen::nearest_mm(wide(layer.rule.scale_m));
  if (cell_mm < 250 || cell_mm > 16000 || tile_mm <= 0 || tile_mm % cell_mm != 0 ||
      scale_mm % cell_mm != 0 || tile_mm / cell_mm > 256 || scale_mm / cell_mm > 64)
    return refuse(far ? "far_cell_m must be 0.25 to 16 m and divide the tile and rule.scale_m"
                      : "cell_m must be 0.25 to 16 m and divide the tile and rule.scale_m");
  out.seed = layer.seed;
  out.type = layer.type;
  out.formation = *layer.formation;
  out.rule = layer.rule;
  out.x_mm = x_mm;
  out.z_mm = z_mm;
  out.cell_mm = static_cast<u32>(cell_mm);
  out.cells = static_cast<u32>(tile_mm / cell_mm);
  out.apron = static_cast<u32>(scale_mm / cell_mm);
  out.far = far;
  out.joint_ratio = layer.joint_ratio;
  out.max_step_m = layer.max_step_m;
  out.min_step_m = far ? layer.far_min_step_m : layer.min_step_m;
  out.bury_m = layer.bury_m;
  out.relief_m = layer.relief_m;
  out.cliff_slope_deg = layer.cliff_slope_deg;
  out.material = layer.material;
  out.tread_tint = layer.tread_tint;
  const f64 corner = ground.surface_mm(x_mm, z_mm);
  if (!std::isfinite(corner)) return refuse("the ground's height is not finite");
  out.y_mm = scene_gen::nearest_mm(corner);
  const i32 side = static_cast<i32>(out.cells + 1 + 2 * out.apron);
  const i32 apron = static_cast<i32>(out.apron);
  for (i32 j = 0; j < side; ++j)
    for (i32 i = 0; i < side; ++i) {
      const f64 h = ground.surface_mm(x_mm + static_cast<i64>(i - apron) * cell_mm,
                                      z_mm + static_cast<i64>(j - apron) * cell_mm);
      if (!std::isfinite(h)) return refuse("the ground's height is not finite");
      const i64 above = scene_gen::nearest_mm(h) - out.y_mm;
      if (above < std::numeric_limits<i32>::min() || above > std::numeric_limits<i32>::max())
        return refuse("the ground's relief over a patch is past an i32 of millimetres");
      out.heights_mm.push_back(static_cast<i32>(above));
    }
  return ledge_patch_in_range(out, error);
}

namespace {
bool solve_patch(const LedgePatch& p, assets::MeshData& out, LedgeReport* report,
                 Vector<LedgeGap>* gaps, std::string& error) {
  out = {};
  if (report != nullptr) *report = {};
  if (gaps != nullptr) gaps->clear();
  // A fallen block's pieces, one a lattice triangle it crosses: sums, divided at the end.
  Vector<LedgeGap> pieces;
  if (!ledge_patch_in_range(p, error)) return false;
  ExposureThresholds rule;
  if (!prepare_exposure(p.rule, rule, error)) return false;
  rule.seed = p.seed;  // the layer's: the soil's patches are the world's, not the tile's
  gfx::FormationParams field;
  std::string why;
  if (!gfx::pack_formation(p.formation, field, &why)) {
    error = "engine.rocks.LedgePatch." + why;
    return false;
  }
  const gfx::FormationFrame frame = gfx::formation_frame(field);
  Vector<Unit> units;
  find_units(field.beds, rule, p.seed, units);
  if (units.empty()) return true;

  const u32 side = p.cells + 1 + 2 * p.apron;  // heights a side
  const u32 m = p.cells + 1;                   // the cells' corners a side
  const i64 cell_mm = static_cast<i64>(p.cell_mm);
  const f64 cell = static_cast<f64>(p.cell_mm) / 1000.0;
  const f64 ny = frame.normal[1];
  const f64 period = static_cast<f64>(field.beds.period_mm) / 1000.0;
  const D3 along_strike{-frame.dip[2], 0.0, frame.dip[0]};
  const f64 tile_m = wide(p.material.tile_m);
  const f64 max_step = wide(p.max_step_m);
  const f64 min_step = wide(p.min_step_m);
  const f64 bury = wide(p.bury_m);
  const f64 relief = p.far ? 0.0 : wide(p.relief_m);
  // **Worn round** (rocks.md, "Blocks"): an edge is worn back by the rock type's rounding, the
  // one the cliff faces use, and a face is fluted by as much again. The far form has neither.
  const f64 round_m = p.far ? 0.0 : k_wear_m * cliff_type_rounding(p.type);
  const u64 flute_seed = mix64(p.seed ^ 0x464C555445ull);  // "FLUTE"
  const D3 world0{static_cast<f64>(p.x_mm) / 1000.0, static_cast<f64>(p.y_mm) / 1000.0,
                  static_cast<f64>(p.z_mm) / 1000.0};
  // Ground this steep is the cliffs generator's: no ledge under or behind one of its faces.
  const f64 cliff_angle = wide(p.cliff_slope_deg) * (k_pi / 180.0);
  const f64 cliff_slope =
      p.cliff_slope_deg > 0.0f ? det::sin(cliff_angle) / det::cos(cliff_angle) : 1.0e30;

  // The fields at the cells' corners: the ground (metres above the patch's corner), the
  // formation's coordinate there, the share of a bed that stands, the downhill direction, and
  // the place along the strike.
  Vector<f64> ground(m * m), coordinate(m * m), fraction(m * m), down_x(m * m), down_z(m * m),
      strike(m * m), down_dip(m * m), per_slope(m * m);
  const auto height_mm = [&](u32 i, u32 j) { return static_cast<i64>(p.heights_mm[j * side + i]); };
  for (u32 j = 0; j < m; ++j)
    for (u32 i = 0; i < m; ++i) {
      const u32 a = p.apron;
      const u32 gi = i + a, gj = j + a;
      const GroundShape shape =
          ground_shape(height_mm(gi, gj), height_mm(gi - a, gj), height_mm(gi + a, gj),
                       height_mm(gi, gj - a), height_mm(gi, gj + a), rule.arm_mm);
      const u32 k = j * m + i;
      const i64 wx_mm = p.x_mm + static_cast<i64>(i) * cell_mm;
      const i64 wz_mm = p.z_mm + static_cast<i64>(j) * cell_mm;
      const f64 wx = static_cast<f64>(wx_mm) / 1000.0;
      const f64 wz = static_cast<f64>(wz_mm) / 1000.0;
      const f64 wy = static_cast<f64>(p.y_mm + height_mm(gi, gj)) / 1000.0;
      ground[k] = static_cast<f64>(height_mm(gi, gj)) / 1000.0;
      coordinate[k] = gfx::formation_coordinate(frame, wx, wy, wz).coordinate;
      fraction[k] =
          standing_share(rule, exposure_fraction(rule, shape), soil_cover(rule, wx_mm, wz_mm));
      if (shape.slope >= cliff_slope) fraction[k] = 0.0;
      // Metres behind a front a metre of height under its step, for the corners worn in plan.
      per_slope[k] = 1.0 / (shape.slope > 0.3 ? shape.slope : 0.3);
      const bool sloped = shape.slope > 1e-9;
      down_x[k] = sloped ? -shape.slope_x / shape.slope : 1.0;
      down_z[k] = sloped ? -shape.slope_z / shape.slope : 0.0;
      strike[k] = along_strike.x * (wx - frame.origin[0]) + along_strike.z * (wz - frame.origin[2]);
      down_dip[k] = frame.dip[0] * (wx - frame.origin[0]) + frame.dip[2] * (wz - frame.origin[2]);
    }
  const f64 strike0 = strike[0];
  const f64 dip0 = down_dip[0];
  // The ground under a point of the patch, on the lattice's own triangles, over the apron too: a
  // crept block, a recess or an undercut reaches past the tile's edge, and what covers it there
  // is the world's lattice, the same from the tile beside.
  const auto ground_at = [&](f64 x, f64 z) {
    const f64 a0 = static_cast<f64>(p.apron);
    const f64 last = static_cast<f64>(side - 1);
    const f64 fx = std::clamp(x / cell + a0, 0.0, last);
    const f64 fz = std::clamp(z / cell + a0, 0.0, last);
    const u32 ix = std::min(static_cast<u32>(fx), side - 2);
    const u32 iz = std::min(static_cast<u32>(fz), side - 2);
    const f64 u = fx - static_cast<f64>(ix), v = fz - static_cast<f64>(iz);
    const auto at_mm = [&](u32 i, u32 j) { return static_cast<f64>(height_mm(i, j)) / 1000.0; };
    if (u + v <= 1.0)
      return at_mm(ix, iz) * (1.0 - u - v) + at_mm(ix + 1, iz) * u + at_mm(ix, iz + 1) * v;
    return at_mm(ix + 1, iz) * (1.0 - v) + at_mm(ix, iz + 1) * (1.0 - u) +
           at_mm(ix + 1, iz + 1) * (u + v - 1.0);
  };

  Mesher mesher;
  mesher.tile_m = tile_m;
  LedgeReport made;
  const auto count = [&](bool tread, D3 q[3]) {
    ++made.triangles;
    if (tread)
      ++made.tread_triangles;
    else
      ++made.front_triangles;
    const f64 cx = (q[0].x + q[1].x + q[2].x) / 3.0, cz = (q[0].z + q[1].z + q[2].z) / 3.0;
    if ((q[0].y + q[1].y + q[2].y) / 3.0 > ground_at(cx, cz)) ++made.above_ground;
  };
  // A triangle of a front, a worn edge or a joint face that the ground covers whole (every corner
  // 2 cm or more under the lattice's own ground) is not drawn: a face set back into the slope by
  // its row's recess and its flute is under the turf there.
  const auto covered = [&](const D3 q[3]) {
    for (u32 k = 0; k < 3; ++k)
      if (q[k].y > ground_at(q[k].x, q[k].z) - 0.02) return false;
    return true;
  };
  Vector<u64> cropped;  // the unit instances that made a triangle
  // How far a front goes on under the ground at its foot: less under a low step.
  const auto under_of = [&](f64 step) {
    const f64 under = 0.05 + 0.15 * (step > 0.0 ? step : 0.0);
    return bury < under ? bury : under;
  };

  for (u32 cz = 0; cz < p.cells; ++cz)
    for (u32 cx = 0; cx < p.cells; ++cx)
      for (u32 half = 0; half < 2; ++half) {
        const u32 n00 = cz * m + cx, n10 = n00 + 1, n01 = n00 + m, n11 = n01 + 1;
        const u32 node[3] = {half == 0 ? n00 : n10, n01, half == 0 ? n10 : n11};
        f64 lx[3], lz[3], g[3], c[3], fr[3], dx[3], dz[3], st0[3], dp[3], ps[3];
        for (u32 k = 0; k < 3; ++k) {
          dp[k] = down_dip[node[k]];
          ps[k] = per_slope[node[k]];
          lx[k] = static_cast<f64>(node[k] % m) * cell;
          lz[k] = static_cast<f64>(node[k] / m) * cell;
          g[k] = ground[node[k]];
          c[k] = coordinate[node[k]];
          fr[k] = fraction[node[k]];
          dx[k] = down_x[node[k]];
          dz[k] = down_z[node[k]];
          st0[k] = strike[node[k]];
        }
        if (!(fr[0] > 0.0 || fr[1] > 0.0 || fr[2] > 0.0)) continue;
        const f64 c_min = std::min(c[0], std::min(c[1], c[2]));
        const f64 c_max = std::max(c[0], std::max(c[1], c[2]));
        for (u32 ui = 0; ui < units.size(); ++ui) {
          const Unit& unit = units[ui];
          const f64 plumb = unit.thickness / ny;  // the unit's thickness along a plumb line
          const f64 cap = plumb < max_step ? plumb : max_step;
          const f64 spacing = std::clamp(unit.thickness / wide(p.joint_ratio), 0.5, 8.0);
          const i64 first =
              static_cast<i64>(std::floor((c_min - k_back_m * ny - unit.top) / period));
          const i64 last = static_cast<i64>(std::floor((c_max + cap * ny - unit.top) / period)) + 1;
          for (i64 inst = first; inst <= last; ++inst) {
            const f64 c_top = static_cast<f64>(inst) * period + unit.top;
            const u64 uid = mix64(p.seed ^ mix64(static_cast<u64>(ui) + 1) ^
                                  mix64(static_cast<u64>(inst) * 0x9E3779B97F4A7C15ull));
            // Outcrops, not stripes (exposure.h): the bed stands only inside a cliff-forming
            // member, and along its own stretches.
            const f64 member = member_factor(rule, field, c_top - 0.5 * unit.thickness);
            if (!(member > 0.0)) continue;
            const u64 crag = member_id(rule, field, c_top - 0.5 * unit.thickness);
            // The top plane's height over the ground, and the step with every block whole.
            f64 d[3], whole[3];
            bool any_h = false, any_g = false, any_f = false;
            for (u32 k = 0; k < 3; ++k) {
              d[k] = (c_top - c[k]) / ny;
              whole[k] = cap * fr[k] * unit.factor * member;
              any_h = any_h || whole[k] >= min_step;
              any_g = any_g || d[k] + k_back_m > 0.0;
              any_f = any_f || d[k] - whole[k] < 0.0;
            }
            if (!any_h || !any_g || !any_f) continue;
            any_h = false;
            any_f = false;
            for (u32 k = 0; k < 3; ++k) {
              whole[k] *=
                  stretch_factor(rule, crag, p.x_mm + static_cast<i64>(node[k] % m) * cell_mm,
                                 p.z_mm + static_cast<i64>(node[k] / m) * cell_mm);
              any_h = any_h || whole[k] >= min_step;
              any_f = any_f || d[k] - whole[k] < 0.0;
            }
            if (!any_h || !any_f) continue;
            // The joints of this bed top run up to 20 degrees off the dip direction, so blocks
            // are not all square to the ledge: the place along the strike, sheared.
            const f64 skew = p.far ? 0.0 : 0.72 * (draw(uid, 0, 0x99ull) - 0.5);
            f64 st[3];
            for (u32 k = 0; k < 3; ++k)
              st[k] = st0[k] + skew * dp[k];
            const f64 st_min = std::min(st[0], std::min(st[1], st[2]));
            const f64 st_max = std::max(st[0], std::max(st[1], st[2]));
            const u32 unit_rows = p.far ? 1u : unit.rows;
            const auto block_shape = [&](i64 index) {
              if (!p.far) return block_at(uid, index, spacing, unit_rows, unit.thickness);
              Block whole_ledge;
              whole_ledge.share = k_far_share;
              return whole_ledge;
            };
            // A block's step at a place: the unit's there times what the block keeps, less the
            // rows it lost off its top; nothing where that is under the least step.
            const auto step_of = [&](const Block& shape, f64 whole_step, f64 place) {
              const f64 stood = whole_step * kept_at(shape, place) - unit.depth[shape.lost] / ny;
              return stood < min_step ? 0.0 : stood;
            };
            // The joint blocks the triangle reaches; the far form is one block.
            const i64 block_lo = p.far ? 0 : block_of(st_min, spacing, uid);
            const i64 block_hi = p.far ? 0 : block_of(st_max, spacing, uid);
            for (i64 block = block_lo; block <= block_hi; ++block) {
              Block shape = block_shape(block);
              const bool fallen = !(shape.share > 0.0);
              if (fallen) {
                // A gap: nothing is drawn. Asked for, its footprint is the one the block had,
                // at the blocks' mean share and square to its bed.
                if (gaps == nullptr || p.far) continue;
                shape.share = k_far_share;
              }
              const f64 lost_m =
                  unit.depth[shape.lost] / ny;  // the tread's drop under the unit's top
              PolyVertex tri[3], stage_a[12], stage_b[12], stage_c[12], stage_d[12], behind[12],
                  poly[12];
              for (u8 k = 0; k < 3; ++k) {
                tri[k].w[k] = 1.0;
                tri[k].a = tri[k].b = k;
              }
              const PolyVertex* current = tri;
              u32 n = 3;
              // The block's own faces stand half its two joints' open widths in from their
              // stations, so every joint is a cleft between two flanks (none on the far form).
              const f64 lo =
                  p.far ? 0.0
                        : station(block, spacing, uid) +
                              0.5 * cleft_width(uid, block, unit.thickness, spacing, round_m);
              const f64 hi =
                  p.far ? 0.0
                        : station(block + 1, spacing, uid) -
                              0.5 * cleft_width(uid, block + 1, unit.thickness, spacing, round_m);
              if (!p.far) {
                if (st_min < lo) {
                  const f64 f[3] = {st[0] - lo, st[1] - lo, st[2] - lo};
                  n = clip(current, n, f, node, true, k_on_joint_lo, stage_a);
                  current = stage_a;
                }
                if (n >= 3 && st_max >= hi) {
                  const f64 f[3] = {st[0] - hi, st[1] - hi, st[2] - hi};
                  n = clip(current, n, f, node, false, k_on_joint_hi, stage_b);
                  current = stage_b;
                }
                if (n < 3) continue;
              }
              // `db`: this block's tread over the ground (the unit's top less what it lost).
              f64 h[3], gl[3], fl[3], db[3];
              for (u32 k = 0; k < 3; ++k) {
                h[k] = step_of(shape, whole[k], st[k]);
                db[k] = d[k] - lost_m;
                gl[k] = db[k] + (0.5 * h[k] < k_back_m ? 0.5 * h[k] : k_back_m);
                fl[k] = db[k] - h[k];
              }
              if (!p.far && round_m > 0.0) {
                // **Corners worn in plan**: where the front meets a joint the block is cut back
                // at 45 degrees, half as far again as an edge is worn and never more than a
                // quarter of the block, so two blocks meet in a notch and not in a line.
                const f64 worn = std::min(k_corner * round_m, 0.25 * shape.length);
                f64 c_lo[3], c_hi[3];
                bool cut_lo = false, cut_hi = false;
                for (u32 k = 0; k < 3; ++k) {
                  const f64 behind_front = -fl[k] * per_slope[node[k]];
                  c_lo[k] = st[k] - lo + behind_front - worn;
                  c_hi[k] = hi - st[k] + behind_front - worn;
                  cut_lo = cut_lo || c_lo[k] < 0.0;
                  cut_hi = cut_hi || c_hi[k] < 0.0;
                }
                if (cut_lo) {
                  n = clip(current, n, c_lo, node, true, k_on_corner_lo, stage_c);
                  current = stage_c;
                }
                if (n >= 3 && cut_hi) {
                  n = clip(current, n, c_hi, node, true, k_on_corner_hi, stage_d);
                  current = stage_d;
                }
                if (n < 3) continue;
              }
              n = clip(current, n, gl, node, true, 0, behind);
              if (n < 3) continue;
              n = clip(behind, n, fl, node, false, k_on_front, poly);
              if (n < 3) continue;
              if (fallen) {
                LedgeGap piece;
                piece.block = mix64(uid ^ mix64(static_cast<u64>(block) + 0x474150ull));
                piece.length_m = shape.length;
                piece.height_m = std::max(h[0], std::max(h[1], h[2]));
                const f64 x0 = at(poly[0], lx), z0 = at(poly[0], lz);
                for (u32 k = 1; k + 1 < n; ++k) {
                  const f64 x1 = at(poly[k], lx), z1 = at(poly[k], lz);
                  const f64 x2 = at(poly[k + 1], lx), z2 = at(poly[k + 1], lz);
                  const f64 twice = (x1 - x0) * (z2 - z0) - (x2 - x0) * (z1 - z0);
                  const f64 area = 0.5 * (twice < 0.0 ? -twice : twice);
                  const f64 mx = (x0 + x1 + x2) / 3.0, mz = (z0 + z1 + z2) / 3.0;
                  piece.area_m2 += area;
                  piece.x += area * mx;
                  piece.y += area * ground_at(mx, mz);
                  piece.z += area * mz;
                }
                if (piece.area_m2 > 0.0) pieces.push_back(piece);
                continue;
              }

              D3 top[12];
              f64 mid_x = 0.0, mid_z = 0.0;
              for (u32 k = 0; k < n; ++k) {
                top[k] = {at(poly[k], lx), at(poly[k], g) + at(poly[k], db), at(poly[k], lz)};
                mid_x += top[k].x / static_cast<f64>(n);
                mid_z += top[k].z / static_cast<f64>(n);
              }
              // The lip: where the tread meets the front its edge is worn back along the bedding
              // plane, by the rounding and never more than 0.45 of the first row under it.
              const u32 block_rows = unit_rows - shape.lost;
              D3 lip_top[12];
              f64 lip[12];
              // **Rounded all round**: a joint's edge is worn as the lip is, the tread drawn in
              // along the strike off the cleft, so a block has no sharp edge where it is seen.
              for (u32 k = 0; k < n; ++k) {
                lip[k] = 0.0;
                lip_top[k] = top[k];
                const bool on_face = (poly[k].lines & k_on_face) != 0;
                const u8 joint = poly[k].lines & (k_on_joint_lo | k_on_joint_hi);
                // A rock that wears less than k_rim_m (slate) keeps sharp joints: a rim that
                // narrow is under a pixel at a few metres and would cost two triangles a joint.
                if ((!on_face && (joint == 0 || round_m < k_rim_m)) || !(round_m > 0.0)) continue;
                const f64 step = at(poly[k], db);
                f64 first_row = step + under_of(step);
                if (block_rows > 1)
                  first_row = std::min(first_row, unit.depth[shape.lost + 1] / ny - lost_m);
                lip[k] = std::max(0.0, std::min(round_m, 0.45 * first_row));
                D3 back{1.0, 0.0, 0.0};
                if (on_face) {
                  const f64 ddx = at(poly[k], dx), ddz = at(poly[k], dz);
                  const f64 len = std::sqrt(ddx * ddx + ddz * ddz);
                  if (len > 1e-9) back = D3{ddx / len, 0.0, ddz / len};
                } else {
                  // Into the block: along the strike from its low joint, against it from its high.
                  back = (joint & k_on_joint_lo) != 0 ? along_strike * -1.0 : along_strike;
                }
                const f64 rise =
                    lip[k] * (frame.normal[0] * back.x + frame.normal[2] * back.z) / ny;
                lip_top[k] = {top[k].x - lip[k] * back.x, top[k].y + rise,
                              top[k].z - lip[k] * back.z};
              }
              // **Cambered**: a crept block is moved out along the ground's fall and down with it.
              // Every vertex of the block moves by a function of its own place, so the pieces of
              // one block in two lattice triangles (or tiles) still meet.
              const bool slipped = shape.slip_m > 0.0 && !p.far;
              // And rolled down toward its high joint by a function of where each output vertex
              // is (the strike is linear in the place), so the block turns as one body, out of
              // its bed everywhere, and a worn rim folded over a vertex beside it turns with it.
              // `top` keeps the translation only; every vertex drawn from it is rolled.
              const auto rolled = [&](const D3& q) {
                if (!slipped) return q;
                const f64 at_q = strike0 + skew * dip0 +
                                 (along_strike.x + skew * frame.dip[0]) * q.x +
                                 (along_strike.z + skew * frame.dip[2]) * q.z;
                const f64 drop = shape.roll * (at_q - lo);
                return D3{q.x, q.y - (drop > 0.0 ? drop : 0.0), q.z};
              };
              if (slipped) {
                for (u32 k = 0; k < n; ++k) {
                  const f64 ddx = at(poly[k], dx), ddz = at(poly[k], dz);
                  const f64 len = std::sqrt(ddx * ddx + ddz * ddz);
                  const D3 out_dir = len > 1e-9 ? D3{ddx / len, 0.0, ddz / len} : D3{1.0, 0.0, 0.0};
                  const f64 fall = 1.0 / at(poly[k], ps);
                  const D3 shift = out_dir * shape.slip_m + D3{0.0, -shape.slip_m * fall, 0.0};
                  top[k] = top[k] + shift;
                  lip_top[k] = rolled(lip_top[k] + shift);
                }
                ++made.slipped;
              }
              bool drew = false;
              // The tread: the polygon, in the bedding plane. It carries soil to its lip.
              mesher.material = 0;
              for (u32 k = 1; k + 1 < n; ++k) {
                D3 q[3] = {lip_top[0], lip_top[k], lip_top[k + 1]};
                f64 uv[3][2] = {{q[0].x, q[0].z}, {q[1].x, q[1].z}, {q[2].x, q[2].z}};
                if (mesher.emit(mix64(uid), q, uv, Facing::Up, {})) {
                  count(true, q);
                  drew = true;
                }
              }
              mesher.material = 1 + ui;
              for (u32 k = 0; k < n; ++k) {
                const u32 e[2] = {k, (k + 1) % n};
                const u8 shared = poly[e[0]].lines & poly[e[1]].lines;
                const D3 run = top[e[1]] - top[e[0]];
                if ((shared & k_on_face) != 0) {
                  if ((shared & k_on_front) == 0)
                    made.corner_length_m += std::sqrt(run.x * run.x + run.z * run.z);
                  // The front: down from the tread's edge to the ground and under it, a face a
                  // row, each row standing back under the one above by its own seeded share.
                  D3 outward{run.z, 0.0, -run.x};
                  if (outward.x * (mid_x - top[e[0]].x) + outward.z * (mid_z - top[e[0]].z) > 0.0)
                    outward = outward * -1.0;
                  // The rows the block still has, and under them, where the bed below is weaker,
                  // its **undercut**: a last row from a third of the step above the ground down
                  // to the foot, standing back under the rows above.
                  const bool notched = unit.undercut > 0.0 && relief > 0.0;
                  const u32 rows = block_rows + (notched ? 1u : 0u);
                  f64 depth[2][k_max_rows + 2], back_m[2][k_max_rows + 1], place_u[2], grown_at[2];
                  D3 lean[2];
                  for (u32 v = 0; v < 2; ++v) {
                    const PolyVertex& pv = poly[e[v]];
                    const f64 step = at(pv, db);
                    const f64 foot = step + under_of(step);
                    const f64 ddx = at(pv, dx), ddz = at(pv, dz);
                    const f64 len = std::sqrt(ddx * ddx + ddz * ddz);
                    lean[v] = len > 1e-9 ? D3{ddx / len, 0.0, ddz / len} : D3{1.0, 0.0, 0.0};
                    place_u[v] = at(pv, st) - strike0;
                    // A low step's rows stand flush: the recesses grow in with the step.
                    const f64 grown = relief > 0.0 ? clamp01(step / (2.0 * relief)) : 0.0;
                    grown_at[v] = grown;
                    depth[v][0] = 0.0;
                    back_m[v][0] = 0.0;
                    for (u32 r = 1; r < block_rows; ++r) {
                      const f64 at_row = unit.depth[shape.lost + r] / ny - lost_m;
                      depth[v][r] = at_row < foot ? at_row : foot;
                      // A weaker row stands further back under the row above it.
                      const f64 s = 0.15 + 0.85 * draw(uid, block, 0x100ull + r) -
                                    1.2 * unit.lead[shape.lost + r];
                      back_m[v][r] = grown * relief * clamp01(s);
                    }
                    if (notched) {
                      // A third of the step high (0.5 m at most), and back by the bed's contrast
                      // times 0.42 to 0.6 of the step (0.6 m at most), grown in with the step.
                      const f64 high = std::min(0.35 * (step > 0.0 ? step : 0.0), 0.5);
                      const f64 top_of = std::max(step - high, depth[v][block_rows - 1]);
                      depth[v][block_rows] = top_of < foot ? top_of : foot;
                      back_m[v][block_rows] = back_m[v][block_rows - 1] +
                                              grown * unit.undercut * std::min(0.6 * step, 0.6) *
                                                  (0.7 + 0.3 * draw(uid, block, 0x1C1ull));
                    }
                    depth[v][rows] = foot;
                    made.tallest_m = made.tallest_m > step ? made.tallest_m : step;
                  }
                  made.front_length_m += std::sqrt(run.x * run.x + run.z * run.z);
                  const auto place = [&](u32 v, f64 down, f64 standing_back) {
                    const D3& t = top[e[v]];
                    // Back along the bedding plane, so a row's edge stays in its plane.
                    const f64 rise = standing_back *
                                     (frame.normal[0] * lean[v].x + frame.normal[2] * lean[v].z) /
                                     ny;
                    return rolled(D3{t.x - standing_back * lean[v].x, t.y - down + rise,
                                     t.z - standing_back * lean[v].z});
                  };
                  // The fluting: how far a face stands back at a depth under the tread, two scales
                  // of the world's noise, so the two segments that share a vertex agree.
                  const auto flute = [&](u32 v, f64 down) {
                    if (!(round_m > 0.0)) return 0.0;
                    const D3 q = world0 + top[e[v]] + D3{0.0, -down, 0.0};
                    return grown_at[v] * round_m *
                           (0.7 * noise3(flute_seed, q * (1.0 / 1.1)) +
                            0.3 * noise3(flute_seed + 1, q * (1.0 / 0.37)));
                  };
                  for (u32 r = 0; r < rows; ++r) {
                    // A row's top edge is worn where it is an edge: the tread's lip, and a row
                    // that stands out past the row above it.
                    const bool edge =
                        r == 0 || back_m[0][r] + back_m[1][r] < back_m[0][r - 1] + back_m[1][r - 1];
                    f64 worn[2] = {0.0, 0.0};
                    for (u32 v = 0; v < 2 && edge; ++v) {
                      worn[v] = r == 0
                                    ? lip[e[v]]
                                    : std::min(round_m,
                                               std::min(0.45 * (depth[v][r + 1] - depth[v][r]),
                                                        0.8 * (back_m[v][r - 1] - back_m[v][r])));
                      worn[v] = worn[v] > 0.0 ? worn[v] : 0.0;
                    }
                    const D3 ea =
                        place(0, depth[0][r],
                              back_m[0][r] + worn[0] + (r == 0 ? 0.0 : flute(0, depth[0][r])));
                    const D3 eb =
                        place(1, depth[1][r],
                              back_m[1][r] + worn[1] + (r == 0 ? 0.0 : flute(1, depth[1][r])));
                    const D3 a = place(0, depth[0][r] + worn[0],
                                       back_m[0][r] + flute(0, depth[0][r] + worn[0]));
                    const D3 b = place(1, depth[1][r] + worn[1],
                                       back_m[1][r] + flute(1, depth[1][r] + worn[1]));
                    const D3 cc =
                        place(1, depth[1][r + 1], back_m[1][r] + flute(1, depth[1][r + 1]));
                    const D3 dd =
                        place(0, depth[0][r + 1], back_m[0][r] + flute(0, depth[0][r + 1]));
                    const u64 cls = mix64(uid + 0x1000ull + r) ^ mix64(static_cast<u64>(block));
                    if (worn[0] > 0.0 || worn[1] > 0.0) {
                      // The worn edge, in the face's own class so the two shade as one round.
                      D3 w0[3] = {ea, eb, b};
                      f64 wuv0[3][2] = {{place_u[0], ea.y}, {place_u[1], eb.y}, {place_u[1], b.y}};
                      if (!covered(w0) && mesher.emit(cls, w0, wuv0, Facing::Toward, outward))
                        count(false, w0);
                      D3 w1[3] = {ea, b, a};
                      f64 wuv1[3][2] = {{place_u[0], ea.y}, {place_u[1], b.y}, {place_u[0], a.y}};
                      if (!covered(w1) && mesher.emit(cls, w1, wuv1, Facing::Toward, outward))
                        count(false, w1);
                    }
                    D3 q0[3] = {a, b, cc};
                    f64 uv0[3][2] = {{place_u[0], a.y}, {place_u[1], b.y}, {place_u[1], cc.y}};
                    if (!covered(q0) && mesher.emit(cls, q0, uv0, Facing::Toward, outward))
                      count(false, q0);
                    D3 q1[3] = {a, cc, dd};
                    f64 uv1[3][2] = {{place_u[0], a.y}, {place_u[1], cc.y}, {place_u[0], dd.y}};
                    if (!covered(q1) && mesher.emit(cls, q1, uv1, Facing::Toward, outward))
                      count(false, q1);
                    if (r == 0) continue;
                    // The underside of the row above, in the parting plane. Where either end's
                    // parting is under the front's foot it is under the ground: left out, so
                    // every one drawn is whole and in its plane.
                    if (depth[0][r] >= depth[0][rows] || depth[1][r] >= depth[1][rows]) continue;
                    const D3 a0 = place(0, depth[0][r], back_m[0][r - 1] + flute(0, depth[0][r]));
                    const D3 b0 = place(1, depth[1][r], back_m[1][r - 1] + flute(1, depth[1][r]));
                    const bool up =
                        back_m[0][r] + back_m[1][r] < back_m[0][r - 1] + back_m[1][r - 1];
                    const u64 shelf = mix64(uid + (up ? 0x2000ull : 0x3000ull) + r) ^
                                      mix64(static_cast<u64>(block));
                    D3 s0[3] = {a0, b0, eb};
                    f64 suv0[3][2] = {{a0.x, a0.z}, {b0.x, b0.z}, {eb.x, eb.z}};
                    D3 s1[3] = {a0, eb, ea};
                    f64 suv1[3][2] = {{a0.x, a0.z}, {eb.x, eb.z}, {ea.x, ea.z}};
                    const bool undercut = notched && r == block_rows;
                    for (u32 which = 0; which < 2; ++which) {
                      D3* sq = which == 0 ? s0 : s1;
                      if (covered(sq) || !mesher.emit(shelf, sq, which == 0 ? suv0 : suv1,
                                                      up ? Facing::Up : Facing::Down, {}))
                        continue;
                      count(up, sq);
                      if (undercut) {
                        const D3 area = cross(sq[1] - sq[0], sq[2] - sq[0]);
                        made.undercut_m2 += 0.5 * std::sqrt(dot(area, area));
                      }
                    }
                  }
                  drew = true;
                } else if (!p.far && (shared & (k_on_joint_lo | k_on_joint_hi)) != 0) {
                  // A joint's **cleft**: the block's flank, open from the tread down to the
                  // ground and under it whatever the block beside does, because the two stand
                  // the joint's open width apart, and the tread's edge above it worn round.
                  const bool low_side = (shared & k_on_joint_lo) != 0;
                  f64 step[2];
                  D3 rim[2], foot_at[2];
                  for (u32 v = 0; v < 2; ++v) {
                    step[v] = at(poly[e[v]], db);
                    const D3& t = top[e[v]];
                    rim[v] = rolled(D3{t.x, t.y - lip[e[v]], t.z});
                    foot_at[v] = rolled(
                        D3{t.x, t.y - (step[v] > 0.0 ? step[v] : 0.0) - under_of(step[v]), t.z});
                  }
                  const D3 outward = low_side ? along_strike * -1.0 : along_strike;
                  const u64 cls = mix64(uid + (low_side ? 0x4000ull : 0x5000ull)) ^
                                  mix64(static_cast<u64>(block));
                  const auto u_of = [&](const D3& q) {
                    return q.x * frame.dip[0] + q.z * frame.dip[2];
                  };
                  const D3& a = lip_top[e[0]];
                  const D3& b = lip_top[e[1]];
                  if (lip[e[0]] > 0.0 || lip[e[1]] > 0.0) {
                    D3 w0[3] = {a, b, rim[1]};
                    f64 wuv0[3][2] = {{u_of(a), a.y}, {u_of(b), b.y}, {u_of(rim[1]), rim[1].y}};
                    if (!covered(w0) && mesher.emit(cls, w0, wuv0, Facing::Toward, outward)) {
                      count(false, w0);
                      const D3 area = cross(w0[1] - w0[0], w0[2] - w0[0]);
                      made.joint_worn_m2 += 0.5 * std::sqrt(dot(area, area));
                    }
                    D3 w1[3] = {a, rim[1], rim[0]};
                    f64 wuv1[3][2] = {
                        {u_of(a), a.y}, {u_of(rim[1]), rim[1].y}, {u_of(rim[0]), rim[0].y}};
                    if (!covered(w1) && mesher.emit(cls, w1, wuv1, Facing::Toward, outward)) {
                      count(false, w1);
                      const D3 area = cross(w1[1] - w1[0], w1[2] - w1[0]);
                      made.joint_worn_m2 += 0.5 * std::sqrt(dot(area, area));
                    }
                  }
                  D3 q0[3] = {rim[0], rim[1], foot_at[1]};
                  f64 uv0[3][2] = {{u_of(rim[0]), rim[0].y},
                                   {u_of(rim[1]), rim[1].y},
                                   {u_of(foot_at[1]), foot_at[1].y}};
                  if (!covered(q0) && mesher.emit(cls, q0, uv0, Facing::Toward, outward))
                    count(false, q0);
                  D3 q1[3] = {rim[0], foot_at[1], foot_at[0]};
                  f64 uv1[3][2] = {{u_of(rim[0]), rim[0].y},
                                   {u_of(foot_at[1]), foot_at[1].y},
                                   {u_of(foot_at[0]), foot_at[0].y}};
                  if (!covered(q1) && mesher.emit(cls, q1, uv1, Facing::Toward, outward))
                    count(false, q1);
                  const D3 run_xz{run.x, 0.0, run.z};
                  made.joint_length_m += std::sqrt(dot(run_xz, run_xz));
                  const f64 deepest = std::max(step[0], step[1]);
                  made.cleft_depth_m = made.cleft_depth_m > deepest ? made.cleft_depth_m : deepest;
                }
              }
              if (drew) cropped.push_back(uid);
            }
          }
        }
      }
  if (gaps != nullptr && !pieces.empty()) {
    // One entry a block, in order of identity: the pieces' areas summed and their places meaned.
    Vector<u32> by(pieces.size());
    for (u32 i = 0; i < by.size(); ++i)
      by[i] = i;
    std::sort(by.begin(), by.end(), [&](u32 a, u32 b) {
      return pieces[a].block != pieces[b].block ? pieces[a].block < pieces[b].block : a < b;
    });
    for (const u32 i : by) {
      const LedgeGap& piece = pieces[i];
      if (gaps->empty() || (*gaps)[gaps->size() - 1].block != piece.block) {
        gaps->push_back(piece);
        continue;
      }
      LedgeGap& gap = (*gaps)[gaps->size() - 1];
      gap.area_m2 += piece.area_m2;
      gap.x += piece.x;
      gap.y += piece.y;
      gap.z += piece.z;
      gap.height_m = gap.height_m > piece.height_m ? gap.height_m : piece.height_m;
    }
    for (LedgeGap& gap : *gaps) {
      gap.x /= gap.area_m2;
      gap.y /= gap.area_m2;
      gap.z /= gap.area_m2;
    }
  }
  if (mesher.corners.empty()) return true;

  // Weld: corners of one surface at one place are one vertex, its normal the area-weighted sum.
  const u32 corner_count = mesher.corners.size();
  Vector<u32> order(corner_count);
  for (u32 i = 0; i < corner_count; ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    if (corner_less(mesher.corners[a], mesher.corners[b])) return true;
    if (corner_less(mesher.corners[b], mesher.corners[a])) return false;
    return a < b;
  });
  assets::MeshData mesh;
  // The materials that drew a triangle, in order: the treads' soil, then each unit's rock, the
  // type's colour times its top bed's tint and roughness (the formation's, so beds are told
  // apart).
  const u32 material_count = 1 + units.size();
  Vector<u32> used(material_count), slot(material_count);
  for (const u32 m_of : mesher.tri_material)
    ++used[m_of];
  for (u32 k = 0; k < material_count; ++k) {
    if (used[k] == 0) continue;
    slot[k] = mesh.materials.size();
    assets::Material material = rock_material(p.type);
    material_factors(p.material, material.base_color, material.roughness);
    if (k == 0) {
      material.name = "soil";
      material.base_color = p.tread_tint.has_value() ? Vec4{(*p.tread_tint)[0], (*p.tread_tint)[1],
                                                            (*p.tread_tint)[2], 1.0f}
                                                     : Vec4{k_soil[0], k_soil[1], k_soil[2], 1.0f};
      material.roughness = 0.95f;
    } else {
      const Vec4 bed = field.beds.beds[units[k - 1].bed].value;
      material.name = "rock";
      material.base_color.x = std::clamp(material.base_color.x * bed.x, 0.0f, 1.0f);
      material.base_color.y = std::clamp(material.base_color.y * bed.y, 0.0f, 1.0f);
      material.base_color.z = std::clamp(material.base_color.z * bed.z, 0.0f, 1.0f);
      material.roughness = std::clamp(material.roughness + bed.w, 0.0f, 1.0f);
    }
    mesh.materials.push_back(material);
  }
  Vector<u32> welded(corner_count);
  Vector<D3> normals;
  for (u32 i = 0; i < corner_count; ++i) {
    const Corner& c = mesher.corners[order[i]];
    if (i == 0 || !corner_same(c, mesher.corners[order[i - 1]])) {
      mesh.positions.push_back(c.position);
      mesh.uvs.push_back(c.uv);
      normals.push_back(D3{});
    }
    const u32 vertex = mesh.positions.size() - 1;
    normals[vertex] = normals[vertex] + c.normal;
    welded[order[i]] = vertex;
  }
  // One primitive a material, the triangles in the order they were made.
  for (u32 k = 0; k < material_count; ++k) {
    if (used[k] == 0) continue;
    assets::Primitive part;
    part.first_index = mesh.indices.size();
    part.material = static_cast<i32>(slot[k]);
    for (u32 t = 0; t < mesher.tri_material.size(); ++t) {
      if (mesher.tri_material[t] != k) continue;
      for (u32 c = 0; c < 3; ++c)
        mesh.indices.push_back(welded[3 * t + c]);
    }
    part.index_count = mesh.indices.size() - part.first_index;
    mesh.primitives.push_back(part);
  }
  for (u32 i = 0; i < normals.size(); ++i) {
    const D3 n = normalized(normals[i]);
    mesh.normals.push_back({static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z)});
  }
  out = std::move(mesh);
  if (report != nullptr) {
    std::sort(cropped.begin(), cropped.end());
    made.units = static_cast<u32>(std::unique(cropped.begin(), cropped.end()) - cropped.begin());
    *report = made;
  }
  return true;
}
}  // namespace

bool solve_ledges(const LedgePatch& p, assets::MeshData& out, LedgeReport* report,
                  std::string& error) {
  return solve_patch(p, out, report, nullptr, error);
}

bool ledge_gaps(const LedgePatch& p, Vector<LedgeGap>& out, std::string& error) {
  assets::MeshData mesh;
  return solve_patch(p, mesh, nullptr, &out, error);
}

u64 ledge_source_hash(const LedgePatch& p) {
  const std::string text = canonical(p);
  return hash_combine(hash_bytes(text.data(), text.size()),
                      hash_combine(k_ledge_version, k_rocks_version));
}

bool build_ledge_container(const LedgePatch& input, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                           const materials::Pack* pack) {
  out = {};
  LedgePatch p = input;
  materials::MaterialSet set;
  set.base_color = p.material.base_color.value_or("");
  set.normal = p.material.normal.value_or("");
  set.roughness = p.material.roughness.value_or("");
  set.metallic = p.material.metallic.value_or("");
  set.height = p.material.height.value_or("");
  set.tile_m = p.material.tile_m;
  set.normal_scale = p.material.normal_scale;
  bool from_pack = false;
  const auto* chosen = materials::resolve_set(&set, pack, rock_role(p.type), &from_pack);
  if (from_pack) p.material.tile_m = chosen->tile_m;
  assets::MeshData mesh;
  if (!solve_ledges(p, mesh, nullptr, error)) return false;
  if (mesh.indices.empty()) {
    error = "engine.rocks.LedgePatch: no ledges (nothing crops out in this patch)";
    return false;
  }
  geometry::ClusterLodOptions options = lod;
  // A ledge is a sheet whose back is the ground: wound outwards like a closed body's, so the
  // cone test drops what faces away, and a traced shadow keeps it as a caster.
  options.normal_cones = true;
  // A DAG a material (the treads' soil, each unit's rock), merged: a cluster names one material.
  const geometry::AttributeSource attributes = assets::attribute_source(mesh);
  Vector<geometry::ClusterLodMesh> parts;
  Vector<u32> part_material;
  for (const assets::Primitive& primitive : mesh.primitives) {
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(
            mesh.positions, {mesh.indices.data() + primitive.first_index, primitive.index_count},
            options, part, &error, attributes))
      return false;
    parts.push_back(std::move(part));
    part_material.push_back(static_cast<u32>(primitive.material));
  }
  Vector<u32> part_of_cluster;
  if (!geometry::merge_cluster_lod(parts, out.mesh, &part_of_cluster, &error)) return false;
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions pages;
    pages.page_bytes = page_bytes;
    Vector<u32> source_of_cluster;
    if (!geometry::build_cluster_pages(out.mesh, pages, out.pages, &error, &source_of_cluster))
      return false;
    geometry::permute_cluster_array(source_of_cluster, part_of_cluster);
  }
  for (const u32 part : part_of_cluster)
    out.cluster_material.push_back(part_material[part]);
  // What the type's rock is before a bed's tint, to carry the tint over a pack's set.
  assets::Material plain = rock_material(p.type);
  material_factors(p.material, plain.base_color, plain.roughness);
  for (u32 k = 0; k < mesh.materials.size(); ++k) {
    geometry::ClusterFileMaterial material;
    material.base_color = mesh.materials[k].base_color;
    material.metallic = 0;
    material.roughness = mesh.materials[k].roughness;
    out.materials.push_back(material);
    if (chosen)
      out.material_sampling.push_back(geometry::encode_material_sampling(
          mesh.materials[k].sampling, mesh.materials[k].occlusion_strength));
  }
  for (u32 k = 0; chosen && k < mesh.materials.size(); ++k) {
    if (mesh.materials[k].name == "soil") continue;  // no set: the pack has no turf
    materials::MaterialSet applied = *chosen;
    if (p.material.sampling.has_value())
      applied.stochastic = *p.material.sampling == RockSampling::Stochastic;
    materials::apply_set(applied, materials::UvLayout::repeats, out, k);
    material_factors(p.material, out.materials[k].base_color, out.materials[k].roughness);
    // The bed's tint and roughness, over whatever the set left as the factor.
    const Vec4 solved = mesh.materials[k].base_color;
    const auto over = [](f32 tinted, f32 base) { return base > 1e-6f ? tinted / base : 1.0f; };
    out.materials[k].base_color.x *= over(solved.x, plain.base_color.x);
    out.materials[k].base_color.y *= over(solved.y, plain.base_color.y);
    out.materials[k].base_color.z *= over(solved.z, plain.base_color.z);
    out.materials[k].roughness = std::clamp(
        out.materials[k].roughness + mesh.materials[k].roughness - plain.roughness, 0.0f, 1.0f);
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  out.source_hash = ledge_source_hash(input);
  out.build_key = assets::generated_mesh_build_key(
      out.source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  out.generator = "ledge";
  out.generator_kind = input.far ? "far" : "near";
  out.generator_params = canonical(input);
  return true;
}

namespace {

// ---- the mesh generator "ledge" (assets/mesh_generator.h): `<name>.ledge.json` ----------------

bool read_patch(std::span<const u8> bytes, LedgePatch& out, std::string& error) {
  JsonValue json;
  const auto parsed = parse_json({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, json);
  if (!parsed.ok) {
    error = parsed.message;
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, json, ctx) || !ctx.ok()) {
    error = "engine.rocks.LedgePatch";
    for (const auto& d : ctx.diagnostics)
      error += "; " + d.path + ": " + d.message;
    return false;
  }
  return ledge_patch_in_range(out, error);
}
bool generator_source_hash(std::span<const u8> bytes, u64& out, std::string& error) {
  LedgePatch p;
  if (!read_patch(bytes, p, error)) return false;
  out = ledge_source_hash(p);
  return true;
}
bool generator_build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, std::string& error) {
  LedgePatch p;
  if (!read_patch(bytes, p, error)) return false;
  return build_ledge_container(p, lod, page_bytes, out, error, inputs.materials);
}
constexpr assets::MeshGeneratorDesc k_ledge_generator{
    .name = "ledge",
    .source_hash = generator_source_hash,
    .build = generator_build,
};
const assets::MeshGeneratorRegistrar k_ledge_registrar{k_ledge_generator};

}  // namespace
}  // namespace engine::rocks
