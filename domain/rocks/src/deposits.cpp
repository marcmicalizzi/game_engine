#include "detail.h"
#include "geology.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/rocks/deposits.h>
#include <domain/texture/material_textures.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::rocks {
namespace {
using namespace detail;
constexpr i32 k_dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr i32 k_dz[] = {-1, -1, -1, 0, 0, 1, 1, 1};
constexpr f64 k_diag = 1.4142135623730951;
constexpr f32 k_min_depth = 0.001f;
// G143: squared, a block's sampled top may be 3% steeper than the repose (a cone's facets are its
// secants), still under the 39-40 degree friction of talus (rocks.md, "Cones and shed blocks").
constexpr f64 k_rest_slack = 1.0609;
bool fail(std::string& error, const char* field) {
  error = std::string("engine.rocks deposits: invalid ") + field;
  return false;
}
bool in(f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; }
f64 tangent(f32 degrees) {
  const f64 a = wide(degrees) * (k_pi / 180.0);
  return det::sin(a) / det::cos(a);
}
u32 neighbour(u32 i, u32 w, u32 h, u32 d) {
  const i32 x = static_cast<i32>(i % w) + k_dx[d];
  const i32 z = static_cast<i32>(i / w) + k_dz[d];
  return x < 0 || z < 0 || x >= static_cast<i32>(w) || z >= static_cast<i32>(h)
             ? ~0u
             : static_cast<u32>(z) * w + static_cast<u32>(x);
}
f64 step(u32 d, f64 spacing) { return (k_dx[d] != 0 && k_dz[d] != 0) ? spacing * k_diag : spacing; }
std::string canonical(const DepositPatch& p) {
  JsonWriteOptions o;
  o.pretty = false;
  JsonValue json = schema::to_json(p);
  drop_absent_sampling(json);
  // G143 (DepositPatch version 2): a patch without blocks says nothing of them, so every patch
  // written before keeps its key.
  if (p.blocks.empty() && json.is_object()) {
    json.as_object().erase(std::string("blocks"));
    json.as_object().erase(std::string("block_triangles"));
  }
  return write_json(json, o);
}
struct Front {
  f64 head;
  f64 distance;
  u32 cell;
};
bool less_front(const Front& a, const Front& b) {
  if (a.head != b.head) return a.head < b.head;
  if (a.distance != b.distance) return a.distance > b.distance;
  return a.cell > b.cell;
}
// Same diagonal as the surface mesh, not bilinear interpolation through a different surface.
f64 sample(const DepositPatch& p, const Vector<f32>& values, f64 x, f64 z) {
  const f64 fx = std::clamp(x / wide(p.spacing_m), 0.0, static_cast<f64>(p.width - 1));
  const f64 fz = std::clamp(z / wide(p.spacing_m), 0.0, static_cast<f64>(p.depth - 1));
  const u32 ix = std::min(static_cast<u32>(fx), p.width - 2);
  const u32 iz = std::min(static_cast<u32>(fz), p.depth - 2);
  const f64 u = fx - ix, v = fz - iz;
  const u32 a = iz * p.width + ix;
  if (u + v <= 1.0)
    return wide(values[a]) * (1.0 - u - v) + wide(values[a + 1]) * u +
           wide(values[a + p.width]) * v;
  return wide(values[a + 1]) * (1.0 - v) + wide(values[a + p.width]) * (1.0 - u) +
         wide(values[a + p.width + 1]) * (u + v - 1.0);
}
// ---- G143: cones, shed blocks and the formation's beds (rocks.md, "Cones and shed blocks") ----
f64 cube_root(f64 v) { return v > 0.0 ? det::exp(det::log(v) / 3.0) : 0.0; }
i64 floor_mod(i64 a, i64 b) {
  const i64 r = a % b;
  return r < 0 ? r + b : r;
}
// The plan's f64 values on the patch mesh's diagonal, at world millimetres inside the window.
f64 plan_sample(const DepositPlan& p, const Vector<f64>& values, i64 x_mm, i64 z_mm) {
  const f64 s = static_cast<f64>(p.spacing_mm);
  const f64 fx =
      std::clamp(static_cast<f64>(x_mm - p.x_mm) / s, 0.0, static_cast<f64>(p.width - 1));
  const f64 fz =
      std::clamp(static_cast<f64>(z_mm - p.z_mm) / s, 0.0, static_cast<f64>(p.depth - 1));
  const u32 ix = std::min(static_cast<u32>(fx), p.width - 2);
  const u32 iz = std::min(static_cast<u32>(fz), p.depth - 2);
  const f64 u = fx - ix, v = fz - iz;
  const u32 a = iz * p.width + ix;
  if (u + v <= 1.0) return values[a] * (1.0 - u - v) + values[a + 1] * u + values[a + p.width] * v;
  return values[a + 1] * (1.0 - v) + values[a + p.width] * (1.0 - u) +
         values[a + p.width + 1] * (u + v - 1.0);
}
// Horizontal distance between two samples, metres: integer millimetres, one correctly rounded root.
f64 sample_distance(const DepositPlan& p, u32 a, u32 b) {
  const i64 dx = (static_cast<i64>(a % p.width) - static_cast<i64>(b % p.width)) * p.spacing_mm;
  const i64 dz = (static_cast<i64>(a / p.width) - static_cast<i64>(b / p.width)) * p.spacing_mm;
  return std::sqrt(static_cast<f64>(dx * dx + dz * dz)) / 1000.0;
}
// The cone's surface `r` metres from its apex: straight at the repose slope `t` for three quarters
// of its nominal length (`height / t`), then easing linearly to the toe slope `t2` over one more
// nominal length, then `t2`: the straight-over-concave profile of Kirkby and Statham's screes.
f64 cone_surface(f64 apex, f64 height, f64 t, f64 t2, f64 r) {
  const f64 rb = 0.75 * height / t, len = height / t;
  if (r <= rb) return apex - t * r;
  const f64 u = r - rb;
  if (u <= len) return apex - t * rb - t * u + (t - t2) * u * u / (2.0 * len);
  return apex - t * rb - 0.5 * (t + t2) * len - t2 * (u - len);
}
bool ascii_equal(std::string_view a, const char* b) {
  usize i = 0;
  for (; i < a.size() && b[i] != 0; ++i) {
    const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
    const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
    if (x != y) return false;
  }
  return i == a.size() && b[i] == 0;
}
// The rock a bed sheds (DepositLithology): a row naming it, else its name as a type's, else none.
bool bed_rock(const DepositLayer& layer, std::string_view name, RockType& out) {
  for (const auto& row : layer.lithology)
    if (row.bed == name) {
      out = row.type;
      return true;
    }
  for (u32 t = 0; t <= static_cast<u32>(RockType::Dolostone); ++t)
    if (valid_rock_type(static_cast<RockType>(t)) &&
        ascii_equal(name, rock_type_name(static_cast<RockType>(t)))) {
      out = static_cast<RockType>(t);
      return true;
    }
  return false;
}
struct BedAt {
  RockType type;
  f64 thickness_m;  // the bed's thickness; 0 without a formation
};
}  // namespace

f64 deposit_convergence(const DepositPlan& p, u32 i, u32 k) {
  const u32 w = p.width, x = i % w, z = i / w;
  if (k == 0 || x < k || z < k || x + k >= w || z + k >= p.depth) return 0.0;
  const f64 s = static_cast<f64>(p.spacing_mm) / 1000.0 * static_cast<f64>(k);
  auto h = [&](u32 xx, u32 zz) { return p.ground[zz * w + xx]; };
  const f64 c = h(x, z);
  const f64 hx = (h(x + k, z) - h(x - k, z)) / (2.0 * s);
  const f64 hz = (h(x, z + k) - h(x, z - k)) / (2.0 * s);
  const f64 hxx = (h(x + k, z) - 2.0 * c + h(x - k, z)) / (s * s);
  const f64 hzz = (h(x, z + k) - 2.0 * c + h(x, z - k)) / (s * s);
  const f64 hxz =
      (h(x + k, z + k) - h(x + k, z - k) - h(x - k, z + k) + h(x - k, z - k)) / (4.0 * s * s);
  const f64 q = hx * hx + hz * hz;
  if (q <= 0.0) return 0.0;
  // Contour (plan) curvature, the sign a hollow is positive in: zero where contours are straight.
  return (hxx * hz * hz - 2.0 * hxz * hx * hz + hzz * hx * hx) / (q * std::sqrt(q));
}

bool plan_deposits(const DepositLayer& p, const scene_gen::Ground& ground, DepositPlan& out,
                   std::string& error) {
  out = {};
  if (!ground.present()) return fail(error, "ground (required)");
  if (!in(wide(p.spacing_m), .25, 8)) return fail(error, "spacing_m");
  const i64 spacing = scene_gen::nearest_mm(wide(p.spacing_m));
  if (static_cast<f32>(static_cast<f64>(spacing) / 1000.0) != p.spacing_m)
    return fail(error, "spacing_m (whole millimetres)");
  for (u32 a = 0; a < 2; ++a) {
    if (!in(p.region_min[a], -1e8, 1e8) || !in(p.region_max[a], -1e8, 1e8) ||
        p.region_max[a] <= p.region_min[a])
      return fail(error, "region_min/max");
    if (scene_gen::nearest_mm(p.region_min[a]) % spacing != 0 ||
        scene_gen::nearest_mm(p.region_max[a]) % spacing != 0)
      return fail(error, "region alignment");
  }
  if (p.max_samples < 4 || p.max_samples > 1048576) return fail(error, "max_samples");
  if (!in(wide(p.source_deg), 40, 85) || !in(wide(p.repose_deg), 25, 40) ||
      p.source_deg <= p.repose_deg)
    return fail(error, "source_deg/repose_deg");
  if (!in(wide(p.talus_depth_m), 0, 20) || !in(wide(p.runout_m), 1, 1000) ||
      !in(wide(p.channel_area_m2), 1, 1e6) || !in(wide(p.channel_deg), 1, 30) ||
      !in(wide(p.sediment_depth_m), 0, 2) || !in(wide(p.clear_m), 0, 100))
    return fail(error, "deposition controls");
  // G143's controls. The gullies' are read only by the cones, so only a layer that asks for cones
  // is held to them (a 0.3 m spacing has no whole 2 m scale, and asks for none).
  const i64 gully_mm = scene_gen::nearest_mm(wide(p.gully_scale_m));
  if (p.cone_supply_m > 0 &&
      (!in(wide(p.gully_deg), 30, 80) || !in(wide(p.gully_scale_m), wide(p.spacing_m), 32) ||
       gully_mm % spacing != 0 || !in(wide(p.gully_convergence), .001, 10) ||
       !in(wide(p.gully_area_m2), 1, 1e6)))
    return fail(error, "gully controls (gully_deg, gully_scale_m in whole spacings, ...)");
  if (!in(wide(p.cone_supply_m), 0, 50) || !in(wide(p.cone_max_m), .5, 20) ||
      !in(wide(p.toe_deg), 20, 40) || (p.cone_supply_m > 0 && p.toe_deg > p.repose_deg))
    return fail(error, "cone controls (cone_supply_m, cone_max_m, toe_deg at most repose_deg)");
  if (!in(wide(p.shadow_deg), 15, 45) || !in(wide(p.blocks_per_100m2), 0, 400) ||
      !in(wide(p.block_decay_m), 1, 500) || !in(wide(p.block_m), .1, 6) ||
      !in(wide(p.buried_min), 0, .9) || !in(wide(p.buried_max), 0, .9) ||
      p.buried_min > p.buried_max)
    return fail(error, "block controls (shadow_deg, blocks_per_100m2, block_m, buried_min/max)");
  if (p.lithology.size() > 64) return fail(error, "lithology (at most 64 rows)");
  for (const auto& row : p.lithology)
    if (!valid_rock_type(row.type)) return fail(error, "lithology type");
  gfx::FormationParams formation;
  if (p.formation.has_value()) {
    std::string why;
    if (!gfx::validate_formation(*p.formation, &why) ||
        !gfx::pack_formation(*p.formation, formation, &why)) {
      error = "engine.rocks deposits: formation: " + why;
      return false;
    }
  }
  DepositPlan plan;
  plan.x_mm = scene_gen::nearest_mm(p.region_min[0]);
  plan.z_mm = scene_gen::nearest_mm(p.region_min[1]);
  plan.spacing_mm = spacing;
  const u64 w =
      static_cast<u64>((scene_gen::nearest_mm(p.region_max[0]) - plan.x_mm) / spacing) + 1;
  const u64 h =
      static_cast<u64>((scene_gen::nearest_mm(p.region_max[1]) - plan.z_mm) / spacing) + 1;
  if (w > p.max_samples || h > p.max_samples || w * h > p.max_samples)
    return fail(error, "max_samples (region exceeds grid budget)");
  plan.width = static_cast<u32>(w);
  plan.depth = static_cast<u32>(h);
  const u32 n = plan.width * plan.depth;
  plan.ground.resize(n);
  plan.thickness.resize(n, 0.0f);
  plan.coarse.resize(n, 0.0f);
  plan.drainage_m2.resize(n, wide(p.spacing_m) * wide(p.spacing_m));
  Vector<u32> order(n), next(n, ~0u);
  Vector<f64> slopes(n, 0.0), heads(n, -std::numeric_limits<f64>::infinity()), distances(n, 0.0);
  for (u32 i = 0; i < n; ++i) {
    plan.ground[i] = ground.surface_mm(plan.x_mm + static_cast<i64>(i % plan.width) * spacing,
                                       plan.z_mm + static_cast<i64>(i / plan.width) * spacing);
    if (!in(plan.ground[i], -1e6, 1e6)) return fail(error, "ground height (finite +/-1e6 m)");
    order[i] = i;
  }
  for (u32 i = 0; i < n; ++i)
    for (u32 d = 0; d < 8; ++d) {
      const u32 j = neighbour(i, plan.width, plan.depth, d);
      if (j == ~0u) continue;
      const f64 slope = (plan.ground[i] - plan.ground[j]) / step(d, wide(p.spacing_m));
      if (slope > slopes[i]) {
        slopes[i] = slope;
        next[i] = j;
      }
    }
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return plan.ground[a] != plan.ground[b] ? plan.ground[a] > plan.ground[b] : a < b;
  });
  for (u32 i : order)
    if (next[i] != ~0u) plan.drainage_m2[next[i]] += plan.drainage_m2[i];
  Vector<Front> queue;
  // G143: the steep sample whose envelope reached each sample, the face a talus is of.
  Vector<u32> origin(n, ~0u);
  const f64 source = tangent(p.source_deg), repose = tangent(p.repose_deg);
  if (p.talus_depth_m > 0)
    for (u32 i = 0; i < n; ++i)
      if (slopes[i] >= source) {
        heads[i] = plan.ground[i];
        origin[i] = i;
        queue.push_back({heads[i], 0.0, i});
      }
  std::make_heap(queue.begin(), queue.end(), less_front);
  while (!queue.empty()) {
    std::pop_heap(queue.begin(), queue.end(), less_front);
    const Front f = queue.back();
    queue.pop_back();
    if (f.head != heads[f.cell] || f.distance != distances[f.cell]) continue;
    for (u32 d = 0; d < 8; ++d) {
      const u32 j = neighbour(f.cell, plan.width, plan.depth, d);
      if (j == ~0u || plan.ground[j] > plan.ground[f.cell]) continue;
      const f64 travel = f.distance + step(d, wide(p.spacing_m));
      const f64 carried = slopes[f.cell] < source
                              ? std::min(f.head, plan.ground[f.cell] + wide(p.talus_depth_m))
                              : f.head;
      const f64 head = carried - repose * step(d, wide(p.spacing_m));
      if (travel > wide(p.runout_m) || head <= plan.ground[j] || head < heads[j] ||
          (head == heads[j] && travel >= distances[j]))
        continue;
      heads[j] = head;
      distances[j] = travel;
      origin[j] = origin[f.cell];
      queue.push_back({head, travel, j});
      std::push_heap(queue.begin(), queue.end(), less_front);
    }
  }
  const f64 channel = tangent(p.channel_deg);
  for (u32 i = 0; i < n; ++i) {
    // Talus occupies reachable ground below the source threshold, including the foot transition.
    const f64 talus = slopes[i] < source
                          ? std::clamp(heads[i] - plan.ground[i], 0.0, wide(p.talus_depth_m))
                          : 0.0;
    const f64 sediment = plan.drainage_m2[i] >= wide(p.channel_area_m2)
                             ? wide(p.sediment_depth_m) * clamp01(1.0 - slopes[i] / channel)
                             : 0.0;
    plan.thickness[i] = static_cast<f32>(std::max(talus, sediment));
    plan.coarse[i] = talus > sediment
                         ? static_cast<f32>(.2 + .8 * clamp01(distances[i] / wide(p.runout_m)))
                         : 0.0f;
  }
  // ---- G143 (rocks.md, "Cones and shed blocks"). Each part is skipped whole at its default, so
  // a version-1 layer's plan is the bytes it was.
  const u32 pw = plan.width, pd = plan.depth;
  auto world_x = [&](u32 cell) { return plan.x_mm + static_cast<i64>(cell % pw) * spacing; };
  auto world_z = [&](u32 cell) { return plan.z_mm + static_cast<i64>(cell / pw) * spacing; };
  Vector<u32> from(n, ~0u);  // the face sample a deposit sample's rock is read at
  for (u32 i = 0; i < n; ++i)
    if (plan.coarse[i] > 0) from[i] = origin[i];
  if (p.cone_supply_m > 0) {
    // Gullies: steep, convergent, draining ground; each leads by steepest descent to its mouth.
    const f64 gully = tangent(p.gully_deg);
    const u32 scale = static_cast<u32>(gully_mm / spacing);
    Vector<u8> is_mouth(n, 0);
    Vector<u32> mouths;
    for (u32 i = 0; i < n; ++i) {
      if (plan.drainage_m2[i] < wide(p.gully_area_m2) || slopes[i] < gully ||
          deposit_convergence(plan, i, scale) < wide(p.gully_convergence))
        continue;
      u32 c = i;
      for (u32 guard = 0; guard < n && slopes[c] >= gully && next[c] != ~0u; ++guard)
        c = next[c];
      if (slopes[c] >= gully || is_mouth[c]) continue;
      is_mouth[c] = 1;
      mouths.push_back(c);
    }
    // One cone a gully: the largest catchment first, and no second mouth within the merge radius.
    std::sort(mouths.begin(), mouths.end(), [&](u32 a, u32 b) {
      return plan.drainage_m2[a] != plan.drainage_m2[b] ? plan.drainage_m2[a] > plan.drainage_m2[b]
                                                        : a < b;
    });
    const i64 merge_mm = std::max<i64>(2 * gully_mm, 4 * spacing);
    const f64 toe = tangent(p.toe_deg);
    Vector<f64> cone_depth(n, 0.0), cone_coarse(n, 0.0);
    Vector<u32> cone_of(n, ~0u), seen(n, ~0u), fifo;
    for (u32 m : mouths) {
      bool merged = false;
      for (const DepositCone& other : plan.cones) {
        const i64 dx = world_x(m) - world_x(other.mouth), dz = world_z(m) - world_z(other.mouth);
        merged = merged || dx * dx + dz * dz < merge_mm * merge_mm;
      }
      if (merged) continue;
      // The supply as a half cone at the repose angle against the face: V = pi H^3 / (6 tan^2).
      const f64 volume = plan.drainage_m2[m] * wide(p.cone_supply_m);
      const f64 height =
          std::min(cube_root(6.0 * volume * repose * repose / k_pi), wide(p.cone_max_m));
      if (height < .05) continue;
      DepositCone cone;
      cone.x_mm = world_x(m);
      cone.z_mm = world_z(m);
      cone.height_m = height;
      cone.apex_m = plan.ground[m] + height;
      cone.catchment_m2 = plan.drainage_m2[m];
      cone.mouth = m;
      // The face the cone is of: up the gully along its largest inflow, to above the apex.
      u32 c = m;
      for (u32 guard = 0; guard < 4096 && plan.ground[c] < cone.apex_m; ++guard) {
        u32 up = ~0u;
        for (u32 d = 0; d < 8; ++d) {
          const u32 j = neighbour(c, pw, pd, d);
          if (j != ~0u && next[j] == c &&
              (up == ~0u || plan.drainage_m2[j] > plan.drainage_m2[up] ||
               (plan.drainage_m2[j] == plan.drainage_m2[up] && j < up)))
            up = j;
        }
        if (up == ~0u) break;
        c = up;
      }
      // The apex stands where the gully's floor reaches its height: the cone starts in the gully
      // and fills its mouth, rather than standing as a point in front of it.
      cone.source = c;
      cone.x_mm = world_x(c);
      cone.z_mm = world_z(c);
      const u32 id = plan.cones.size();
      // The samples the surface stands over, connected to the mouth: never over a ridge behind it.
      fifo.clear();
      fifo.push_back(m);
      seen[m] = id;
      for (u32 at = 0; at < fifo.size(); ++at) {
        const u32 cell = fifo[at];
        cone.reach_m = std::max(cone.reach_m, sample_distance(plan, cell, c));
        for (u32 d = 0; d < 8; ++d) {
          const u32 j = neighbour(cell, pw, pd, d);
          if (j == ~0u || seen[j] == id) continue;
          const f64 r = sample_distance(plan, j, c);
          if (r > wide(p.runout_m) ||
              cone_surface(cone.apex_m, height, repose, toe, r) - plan.ground[j] <= k_min_depth)
            continue;
          seen[j] = id;
          fifo.push_back(j);
        }
      }
      for (u32 cell : fifo) {
        const f64 r = sample_distance(plan, cell, c);
        // The thickness cap is the patch's (deposit_patch_in_range), not a solver choice.
        const f64 depth =
            std::min(cone_surface(cone.apex_m, height, repose, toe, r) - plan.ground[cell], 20.0);
        if (depth <= cone_depth[cell]) continue;
        cone_depth[cell] = depth;
        // Fall sorting: fine at the apex, coarse at the toe.
        cone_coarse[cell] = .2 + .8 * clamp01(r / std::max(cone.reach_m, wide(p.spacing_m)));
        cone_of[cell] = id;
      }
      plan.cones.push_back(cone);
    }
    for (u32 i = 0; i < n; ++i)
      if (cone_depth[i] > wide(plan.thickness[i])) {
        plan.thickness[i] = static_cast<f32>(cone_depth[i]);
        plan.coarse[i] = static_cast<f32>(cone_coarse[i]);
        from[i] = plan.cones[cone_of[i]].source;
      }
  }
  // The bed the formation puts at a face sample, and the rock it sheds.
  gfx::FormationFrame frame;
  if (p.formation.has_value()) frame = gfx::formation_frame(formation);
  auto bed_at = [&](u32 face) {
    BedAt bed{p.patch.type, 0.0};
    if (!p.formation.has_value() || face == ~0u || formation.beds.count == 0) return bed;
    const gfx::FormationCoordinate at =
        gfx::formation_coordinate(frame, static_cast<f64>(world_x(face)) / 1000.0,
                                  plan.ground[face], static_cast<f64>(world_z(face)) / 1000.0);
    const gfx::FormationBedSample b = gfx::formation_bed(formation.beds, at.coordinate);
    bed.thickness_m = static_cast<f64>(formation.beds.beds[b.index].thickness_m);
    RockType type{};
    if (bed_rock(p, p.formation->beds[b.index].name, type)) bed.type = type;
    return bed;
  };
  if (p.formation.has_value()) {
    plan.rock.assign(n, p.patch.type);
    for (u32 i = 0; i < n; ++i)
      if (plan.thickness[i] > k_min_depth && from[i] != ~0u) plan.rock[i] = bed_at(from[i]).type;
  }
  if (p.blocks_per_100m2 > 0) {
    // The top of the talus: the first gentle sample below each face. A shed block runs from it
    // along the deposit's top, never uphill, inside the shadow line (Evans and Hungr 1993).
    Vector<f64> top(n), shade(n, -std::numeric_limits<f64>::infinity()), travelled(n, 0.0);
    Vector<u32> seed_of(n, ~0u), face_of(n, ~0u);
    for (u32 i = 0; i < n; ++i)
      top[i] = plan.ground[i] + wide(plan.thickness[i]);
    // A face is what stands steep above the deposits, not the ground they bury.
    Vector<u8> face_sample(n, 0);
    for (u32 i = 0; i < n; ++i)
      for (u32 d = 0; d < 8; ++d) {
        const u32 j = neighbour(i, pw, pd, d);
        if (j != ~0u && (top[i] - top[j]) / step(d, wide(p.spacing_m)) >= source)
          face_sample[i] = 1;
      }
    queue.clear();
    for (u32 i = 0; i < n; ++i) {
      // The top of a talus: a deposit's gentle sample under a face, with no drop below it (a
      // notch's floor at the brink of the face is the head of a gully, not a talus' top).
      if (face_sample[i] || plan.thickness[i] <= k_min_depth) continue;
      bool brink = false;
      for (u32 d = 0; d < 8; ++d) {
        const u32 j = neighbour(i, pw, pd, d);
        brink = brink || (j != ~0u && face_sample[j] && top[j] < top[i]);
      }
      if (brink) continue;
      u32 face = ~0u;
      for (u32 d = 0; d < 8; ++d) {
        const u32 j = neighbour(i, pw, pd, d);
        if (j != ~0u && face_sample[j] && top[j] > top[i] &&
            (face == ~0u || plan.ground[j] > plan.ground[face] ||
             (plan.ground[j] == plan.ground[face] && j < face)))
          face = j;
      }
      if (face == ~0u) continue;
      shade[i] = top[i];
      seed_of[i] = i;
      face_of[i] = face;
      queue.push_back({shade[i], 0.0, i});
    }
    const f64 shadow = tangent(p.shadow_deg);
    std::make_heap(queue.begin(), queue.end(), less_front);
    while (!queue.empty()) {
      std::pop_heap(queue.begin(), queue.end(), less_front);
      const Front f = queue.back();
      queue.pop_back();
      if (f.head != shade[f.cell] || f.distance != travelled[f.cell]) continue;
      for (u32 d = 0; d < 8; ++d) {
        const u32 j = neighbour(f.cell, pw, pd, d);
        if (j == ~0u || top[j] > top[f.cell]) continue;
        const f64 travel = f.distance + step(d, wide(p.spacing_m));
        const f64 head = f.head - shadow * step(d, wide(p.spacing_m));
        if (travel > 1000.0 || head < top[j] || head < shade[j] ||
            (head == shade[j] && travel >= travelled[j]))
          continue;
        shade[j] = head;
        travelled[j] = travel;
        seed_of[j] = seed_of[f.cell];
        queue.push_back({head, travel, j});
        std::push_heap(queue.begin(), queue.end(), less_front);
      }
    }
    // Candidates on the world's lattice near a metre, so a block is where it is whatever window
    // or tile asks; the count a candidate carries falls as exp(-d / decay) with its run-out.
    const i64 lattice = spacing * std::max<i64>(1, (1000 + spacing / 2) / spacing);
    const f64 lattice_m = static_cast<f64>(lattice) / 1000.0;
    const f64 per_candidate = wide(p.blocks_per_100m2) / 100.0 * lattice_m * lattice_m;
    const f64 decay = wide(p.block_decay_m);
    const i64 end_x = world_x(n - 1), end_z = world_z(n - 1);
    const i64 half_step = std::max<i64>(1, spacing / 2);
    for (u32 i = 0; i < n; ++i) {
      const i64 cx = world_x(i), cz = world_z(i);
      if (seed_of[i] == ~0u || floor_mod(cx, lattice) != 0 || floor_mod(cz, lattice) != 0) continue;
      Rng rng(mix64(p.seed ^ mix64(static_cast<u64>(cx) ^ mix64(static_cast<u64>(cz) + 143u))));
      const f64 expected = per_candidate * det::exp(-travelled[i] / decay);
      u32 count = static_cast<u32>(expected);
      if (rng.unit() < expected - static_cast<f64>(count)) ++count;
      count = std::min(count, 8u);
      for (u32 b = 0; b < count; ++b) {
        const i64 x = cx + static_cast<i64>(rng.unit() * static_cast<f64>(lattice)) - lattice / 2;
        const i64 z = cz + static_cast<i64>(rng.unit() * static_cast<f64>(lattice)) - lattice / 2;
        const f64 r0 = rng.unit(), r1 = rng.unit(), r2 = rng.unit(), r3 = rng.unit();
        const f64 yaw = rng.range(0, 2 * k_pi), age = rng.unit();
        const u64 block_seed = rng.next();
        if (x < plan.x_mm || z < plan.z_mm || x > end_x || z > end_z) continue;
        const f64 y = plan_sample(plan, top, x, z);
        const f64 e = static_cast<f64>(half_step) / 1000.0;
        const f64 sx =
            (plan_sample(plan, top, x + half_step, z) - plan_sample(plan, top, x - half_step, z)) /
            (2.0 * e);
        const f64 sz =
            (plan_sample(plan, top, x, z + half_step) - plan_sample(plan, top, x, z - half_step)) /
            (2.0 * e);
        // A block that could not stop there is not there: no block on a top past the repose. A
        // talus at the repose angle holds blocks; the slack is the sampled facets'.
        if (sx * sx + sz * sz > k_rest_slack * repose * repose) continue;
        const u32 near_x = static_cast<u32>((x - plan.x_mm + spacing / 2) / spacing);
        const u32 near_z = static_cast<u32>((z - plan.z_mm + spacing / 2) / spacing);
        const u32 nearest = std::min(near_z, pd - 1) * pw + std::min(near_x, pw - 1);
        if (face_sample[nearest] ||
            (p.sediment_depth_m > 0 && plan.drainage_m2[nearest] >= wide(p.channel_area_m2)))
          continue;
        const u32 s = seed_of[i];
        const i64 tx = world_x(s) - x, tz = world_z(s) - z;
        const f64 reach = std::sqrt(static_cast<f64>(tx * tx + tz * tz)) / 1000.0;
        if (top[s] - y < shadow * reach) continue;
        const BedAt bed = bed_at(face_of[s]);
        // Fall sorting: the farther from the top of its talus, the larger.
        const f64 edge = wide(p.block_m) * (.5 + reach / (reach + decay)) * (.6 + .8 * r0);
        DepositPlanBlock blk;
        blk.x_mm = x;
        blk.z_mm = z;
        blk.y_m = y;
        blk.half[0] = std::clamp(.5 * edge * (.85 + .3 * r1), .02, 4.0);
        blk.half[2] = std::clamp(.5 * edge * (.6 + .35 * r2), .02, 4.0);
        // Broken along its bedding, a block is the type's plate of its width, and never thicker
        // than the bed it came from.
        f64 thick = std::max(blk.half[0], blk.half[2]) *
                    std::clamp(lithology(bed.type).plate, .15, 1.0) * (.6 + .3 * r3);
        if (bed.thickness_m > 0) thick = std::min(thick, .5 * bed.thickness_m);
        blk.half[1] = std::clamp(thick, .02, 4.0);
        blk.slope[0] = sx;
        blk.slope[1] = sz;
        blk.yaw = yaw;
        blk.buried = lerp(wide(p.buried_min), wide(p.buried_max), age);
        blk.distance_m = reach;
        blk.top_x_mm = world_x(s);
        blk.top_z_mm = world_z(s);
        blk.top_m = top[s];
        blk.seed = block_seed;
        blk.type = bed.type;
        plan.blocks.push_back(blk);
      }
    }
  }
  out = std::move(plan);
  return true;
}

bool deposit_patch_in_range(const DepositPatch& p, std::string& error) {
  if (p.width < 2 || p.width > 65 || p.depth < 2 || p.depth > 65)
    return fail(error, "patch dimensions");
  if (!in(wide(p.spacing_m), .25, 8) || wide(p.spacing_m) * (p.width - 1) > 32 ||
      wide(p.spacing_m) * (p.depth - 1) > 32)
    return fail(error, "patch spacing/span");
  const u32 n = p.width * p.depth;
  if (p.ground.size() != n || p.thickness.size() != n || p.coarse.size() != n)
    return fail(error, "patch array sizes");
  for (u32 i = 0; i < n; ++i)
    if (!in(wide(p.ground[i]), -10000, 10000) || !in(wide(p.thickness[i]), 0, 20) ||
        !in(wide(p.coarse[i]), 0, 1))
      return fail(error, "patch samples");
  if (p.fragments > 4096 || p.triangle_budget < 500 || p.triangle_budget > 1000000 ||
      !in(wide(p.fragment_m), .02, std::min(2.0, wide(p.spacing_m) * .5)))
    return fail(error, "patch fragment/budget");
  // G143's blocks: a centre inside the patch's span, a block's own size, burial and support.
  if (p.blocks.size() > 1024 || p.block_triangles < 192 || p.block_triangles > 12288)
    return fail(error, "patch blocks (at most 1024) / block_triangles (192..12288)");
  const f64 span_x = wide(p.spacing_m) * (p.width - 1), span_z = wide(p.spacing_m) * (p.depth - 1);
  for (const DepositBlock& b : p.blocks)
    if (!in(wide(b.x_m), 0, span_x) || !in(wide(b.z_m), 0, span_z) ||
        !in(wide(b.y_m), -10000, 10000) || !in(wide(b.half[0]), .02, 4) ||
        !in(wide(b.half[1]), .02, 4) || !in(wide(b.half[2]), .02, 4) ||
        !in(wide(b.yaw), -100, 100) || !in(wide(b.buried), 0, .9) ||
        !in(wide(b.slope[0]), -10, 10) || !in(wide(b.slope[1]), -10, 10))
      return fail(error, "patch block");
  // Reuse the existing rock/material validator, but this patch has repeated UVs, not a baked atlas.
  RockFile rock;
  rock.boulder = Boulder{};
  rock.boulder->type = p.type;
  rock.boulder->material = p.material;
  if (!rock_file_in_range(rock, error)) return false;
  if (p.material.surface_maps) return fail(error, "patch material.surface_maps (unsupported)");
  return true;
}

bool solve_deposit(const DepositPatch& p, assets::MeshData& out, std::string& error) {
  out = {};
  if (!deposit_patch_in_range(p, error)) return false;
  const u32 w = p.width, h = p.depth, n = w * h;
  Vector<u8> active((w - 1) * (h - 1), 0);
  Vector<u32> cells;
  for (u32 z = 0; z + 1 < h; ++z)
    for (u32 x = 0; x + 1 < w; ++x) {
      const u32 a = z * w + x;
      if (std::max({p.thickness[a], p.thickness[a + 1], p.thickness[a + w],
                    p.thickness[a + w + 1]}) > k_min_depth) {
        active[z * (w - 1) + x] = 1;
      }
    }
  // A pair of cells meeting only at a corner would share a vertical boundary edge four ways.
  // Bridge that ambiguous corner with a zero-depth substrate cell. Its top stays buried, but
  // the closed bed remains two-manifold for simplification and collision. Each cell is added
  // at most once; only its four corners need another look.
  Vector<u32> corners;
  for (u32 z = 1; z + 1 < h; ++z)
    for (u32 x = 1; x + 1 < w; ++x)
      corners.push_back(z * w + x);
  while (!corners.empty()) {
    const u32 vertex = corners.back();
    corners.pop_back();
    const u32 x = vertex % w, z = vertex / w;
    const u32 nw = (z - 1) * (w - 1) + x - 1, ne = nw + 1, sw = nw + w - 1, se = sw + 1;
    u32 fill = ~0u;
    if (active[nw] && active[se] && !active[ne] && !active[sw]) fill = ne;
    if (active[ne] && active[sw] && !active[nw] && !active[se]) fill = nw;
    if (fill == ~0u) continue;
    active[fill] = 1;
    const u32 cx = fill % (w - 1), cz = fill / (w - 1);
    for (u32 dz = 0; dz < 2; ++dz)
      for (u32 dx = 0; dx < 2; ++dx)
        if (cx + dx > 0 && cx + dx + 1 < w && cz + dz > 0 && cz + dz + 1 < h)
          corners.push_back((cz + dz) * w + cx + dx);
  }
  for (u32 z = 0; z + 1 < h; ++z)
    for (u32 x = 0; x + 1 < w; ++x)
      if (active[z * (w - 1) + x]) cells.push_back(z * w + x);
  // A patch of the meadow may carry shed blocks and no bed (G143).
  if (cells.empty() && p.blocks.empty()) return fail(error, "patch (no deposit)");
  auto present = [&](i32 x, i32 z) {
    return x >= 0 && z >= 0 && x < static_cast<i32>(w - 1) && z < static_cast<i32>(h - 1) &&
           active[static_cast<u32>(z) * (w - 1) + static_cast<u32>(x)] != 0;
  };
  u32 surface_triangles = 4 * cells.size();
  for (u32 a : cells) {
    const i32 x = static_cast<i32>(a % w), z = static_cast<i32>(a / w);
    surface_triangles += 2u * static_cast<u32>(!present(x - 1, z) + !present(x + 1, z) +
                                               !present(x, z - 1) + !present(x, z + 1));
  }
  if (static_cast<u64>(surface_triangles) + 192ull * p.fragments > p.triangle_budget)
    return fail(error, "patch triangle_budget (surface plus every fragment at minimum rung)");
  u32 rung = 4;
  const u32 each = p.fragments > 0 ? (p.triangle_budget - surface_triangles) / p.fragments : 0;
  for (u32 r : k_grid_rungs)
    if (r <= 20 && triangles_of_grid(r) <= each) rung = r;
  assets::MeshData mesh;
  assets::Material mat = rock_material(p.type);
  material_factors(p.material, mat.base_color, mat.roughness);
  mesh.materials.push_back(mat);
  Vector<f32> surface(n), sink(n, 0.0f);
  for (u32 i = 0; i < n; ++i) {
    surface[i] = p.ground[i] + p.thickness[i] - k_min_depth;
    // G143: a bed's bare rim on steep ground sinks by half the relief round it, so a triangle of
    // the bed between it and a deep sample never stands out of a face the terrain triangulates
    // the other way (the saw-tooth where a cone met its gully's walls). Depths over 1 mm and
    // gentle ground (under 10 cm of relief a sample) are untouched.
    if (p.thickness[i] > k_min_depth) continue;
    const u32 x = i % w, z = i / w;
    f32 relief = 0;
    for (u32 dz = z > 0 ? z - 1 : 0; dz <= std::min(z + 1, h - 1); ++dz)
      for (u32 dx = x > 0 ? x - 1 : 0; dx <= std::min(x + 1, w - 1); ++dx)
        relief = std::max(relief, std::abs(p.ground[dz * w + dx] - p.ground[i]));
    if (relief > .1f) {
      sink[i] = .5f * relief;
      surface[i] -= sink[i];
    }
  }
  for (u32 bottom = 0; bottom < 2; ++bottom)
    for (u32 i = 0; i < n; ++i) {
      const f32 x = static_cast<f32>(i % w) * p.spacing_m,
                z = static_cast<f32>(i / w) * p.spacing_m;
      mesh.positions.push_back({x, bottom ? p.ground[i] - .05f - sink[i] : surface[i], z});
      mesh.normals.push_back({0, 0, 0});
      mesh.uvs.push_back({x / p.material.tile_m, z / p.material.tile_m});
    }
  auto tri = [&](u32 a, u32 b, u32 c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
  };
  auto edge = [&](u32 a, u32 b) {
    // Boundary walls have their own normals; their buried vertical faces must not bend the bed
    // shading.
    const u32 first = mesh.positions.size();
    for (u32 v : {a, b, b + n, a + n}) {
      const Vec3 position = mesh.positions[v];
      const Vec2 uv = mesh.uvs[v];
      mesh.positions.push_back(position);
      mesh.uvs.push_back(uv);
      mesh.normals.push_back({0, 0, 0});
    }
    tri(first, first + 1, first + 2);
    tri(first, first + 2, first + 3);
  };
  for (u32 a : cells) {
    const u32 b = a + 1, c = a + w, d = c + 1;
    tri(a, c, b);
    tri(b, c, d);
    tri(a + n, b + n, c + n);
    tri(b + n, d + n, c + n);
    const i32 x = static_cast<i32>(a % w), z = static_cast<i32>(a / w);
    if (!present(x, z - 1)) edge(a, b);
    if (!present(x + 1, z)) edge(b, d);
    if (!present(x, z + 1)) edge(d, c);
    if (!present(x - 1, z)) edge(c, a);
  }
  // Area-weighted normals for the connected bed; fragment normals stay their own.
  for (u32 t = 0; t < mesh.indices.size(); t += 3) {
    const u32 a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
    const Vec3 normal =
        cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
    mesh.normals[a] = mesh.normals[a] + normal;
    mesh.normals[b] = mesh.normals[b] + normal;
    mesh.normals[c] = mesh.normals[c] + normal;
  }
  for (Vec3& normal : mesh.normals)
    normal = length(normal) > 0 ? normalize(normal) : Vec3{0, 1, 0};
  for (u32 f = 0; f < p.fragments && !cells.empty(); ++f) {
    Rng rng(mix64(p.seed ^ mix64(static_cast<u64>(f) + 1)));
    const u32 a = cells[static_cast<u32>(static_cast<u64>(f) * cells.size() / p.fragments)];
    const f64 x = (static_cast<f64>(a % w) + rng.range(.25, .75)) * wide(p.spacing_m);
    const f64 z = (static_cast<f64>(a / w) + rng.range(.25, .75)) * wide(p.spacing_m);
    const f64 coarse = sample(p, p.coarse, x, z), pick = rng.unit();
    const f64 size = pick < .65 ? rng.range(.004, .032)
                                : (pick < .92 ? wide(p.fragment_m) * rng.range(.08, .3)
                                              : wide(p.fragment_m) * rng.range(.4, 1.0));
    const f64 radius =
        std::min(size * (coarse > 0 ? .35 + .65 * coarse : .08), wide(p.spacing_m) * .2);
    const f64 r = coarse > 0 ? std::max(.004, radius) : rng.range(.004, .032);
    const f64 y = sample(p, surface, x, z);
    const u32 first = mesh.positions.size();
    const D3 half{r, r * rng.range(.35, .7), r * rng.range(.6, 1)};
    const f64 yaw = rng.range(0, 2 * k_pi);
    const u64 seed = rng.next();
    emit_fragment(p.type, half, {x, y, z}, yaw, .3, seed, rung, wide(p.material.tile_m), mesh);
    // Support shear is local to each fragment; its existing angular fabric is retained.
    const f64 eps = wide(p.spacing_m) * .01;
    const f64 sx = (sample(p, surface, x + eps, z) - sample(p, surface, x - eps, z)) / (2 * eps);
    const f64 sz = (sample(p, surface, x, z + eps) - sample(p, surface, x, z - eps)) / (2 * eps);
    for (u32 v = first; v < mesh.positions.size(); ++v) {
      Vec3& pos = mesh.positions[v];
      pos.y = static_cast<f32>(wide(pos.y) + sample(p, surface, wide(pos.x), wide(pos.z)) - y);
      const Vec3 q = mesh.normals[v];
      const D3 nn = normalized({wide(q.x) - sx * wide(q.y), wide(q.y), wide(q.z) - sz * wide(q.y)});
      mesh.normals[v] = {static_cast<f32>(nn.x), static_cast<f32>(nn.y), static_cast<f32>(nn.z)};
    }
  }
  // G143's blocks: each at the finest rung its allowance holds, sunk its buried share and sheared
  // onto the plane of the top under its centre, so it rests on it on every side and never stands
  // proud of a slope. The plane, not the patch's grid, so a block over the patch's edge sits as
  // well as one inside it.
  u32 block_rung = 4;
  for (u32 r : k_grid_rungs)
    if (triangles_of_grid(r) <= p.block_triangles) block_rung = r;
  for (const DepositBlock& b : p.blocks) {
    const u32 first = mesh.positions.size();
    const f64 x = wide(b.x_m), y = wide(b.y_m), z = wide(b.z_m);
    const f64 sx = wide(b.slope[0]), sz = wide(b.slope[1]);
    emit_fragment(p.type, {wide(b.half[0]), wide(b.half[1]), wide(b.half[2])}, {x, y, z},
                  wide(b.yaw), wide(b.buried), b.seed, block_rung, wide(p.material.tile_m), mesh);
    for (u32 v = first; v < mesh.positions.size(); ++v) {
      Vec3& pos = mesh.positions[v];
      pos.y = static_cast<f32>(wide(pos.y) + sx * (wide(pos.x) - x) + sz * (wide(pos.z) - z));
      const Vec3 q = mesh.normals[v];
      const D3 nn = normalized({wide(q.x) - sx * wide(q.y), wide(q.y), wide(q.z) - sz * wide(q.y)});
      mesh.normals[v] = {static_cast<f32>(nn.x), static_cast<f32>(nn.y), static_cast<f32>(nn.z)};
    }
  }
  Vector<u32> remap(mesh.positions.size(), ~0u);
  for (u32 index : mesh.indices)
    remap[index] = 0;
  u32 used = 0;
  for (u32 i = 0; i < remap.size(); ++i)
    if (remap[i] != ~0u) {
      remap[i] = used;
      mesh.positions[used] = mesh.positions[i];
      mesh.normals[used] = mesh.normals[i];
      mesh.uvs[used] = mesh.uvs[i];
      ++used;
    }
  for (u32& index : mesh.indices)
    index = remap[index];
  mesh.positions.resize(used);
  mesh.normals.resize(used);
  mesh.uvs.resize(used);
  assets::Primitive part;
  part.index_count = mesh.indices.size();
  part.material = 0;
  mesh.primitives.push_back(part);
  out = std::move(mesh);
  return true;
}

u64 deposit_source_hash(const DepositPatch& p) {
  const std::string text = canonical(p);
  return hash_combine(hash_bytes(text.data(), text.size()),
                      hash_combine(k_deposit_version, k_rocks_version));
}

bool build_deposit_container(const DepositPatch& input, const geometry::ClusterLodOptions& lod,
                             u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                             const materials::Pack* pack) {
  out = {};
  DepositPatch p = input;
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
  if (!solve_deposit(p, mesh, error)) return false;
  geometry::ClusterLodOptions options = lod;
  options.normal_cones = true;
  if (!geometry::build_cluster_lod(mesh.positions, {mesh.indices.data(), mesh.indices.size()},
                                   options, out.mesh, &error, assets::attribute_source(mesh)))
    return false;
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions pages;
    pages.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(out.mesh, pages, out.pages, &error)) return false;
  }
  out.cluster_material.assign(out.mesh.mesh.clusters.size(), 0u);
  geometry::ClusterFileMaterial material;
  material.base_color = mesh.materials[0].base_color;
  material.metallic = 0;
  material.roughness = mesh.materials[0].roughness;
  out.materials.push_back(material);
  if (chosen) {
    // apply_set requires a sampling record beside the material. Keep the optional section
    // absent for untextured deposits so their existing container bytes remain unchanged.
    out.material_sampling.push_back(geometry::encode_material_sampling(
        mesh.materials[0].sampling, mesh.materials[0].occlusion_strength));
    // The patch's `sampling`, when it names one, places the chosen set's texels (R131).
    materials::MaterialSet applied = *chosen;
    if (p.material.sampling.has_value())
      applied.stochastic = *p.material.sampling == RockSampling::Stochastic;
    materials::apply_set(applied, materials::UvLayout::repeats, out, 0);
    material_factors(p.material, out.materials[0].base_color, out.materials[0].roughness);
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  out.source_hash = deposit_source_hash(input);
  out.build_key = assets::generated_mesh_build_key(
      out.source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  out.generator = "deposit";
  out.generator_kind = "sediment";
  out.generator_params = canonical(input);
  return true;
}
namespace {
bool read_patch(std::span<const u8> bytes, DepositPatch& out, std::string& error) {
  JsonValue json;
  const auto parsed = parse_json({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, json);
  if (!parsed.ok) {
    error = parsed.message;
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, json, ctx) || !ctx.ok()) {
    error = "engine.rocks.DepositPatch";
    for (const auto& d : ctx.diagnostics)
      error += "; " + d.path + ": " + d.message;
    return false;
  }
  return deposit_patch_in_range(out, error);
}
bool source_hash(std::span<const u8> bytes, u64& out, std::string& error) {
  DepositPatch p;
  if (!read_patch(bytes, p, error)) return false;
  out = deposit_source_hash(p);
  return true;
}
bool build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
           const geometry::ClusterLodOptions& lod, u32 pages, geometry::ClusterFileData& out,
           std::string& error) {
  DepositPatch p;
  return read_patch(bytes, p, error) &&
         build_deposit_container(p, lod, pages, out, error, inputs.materials);
}
constexpr assets::MeshGeneratorDesc k_mesh{
    .name = "deposit", .source_hash = &source_hash, .build = &build};
const assets::MeshGeneratorRegistrar k_registrar{k_mesh};
}  // namespace
}  // namespace engine::rocks
