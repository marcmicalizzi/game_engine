#include "kit.h"

#include <core/hash/hash.h>
#include <domain/flora/sward.h>
#include <foundation/image/decode.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace engine::flora {
namespace {
using namespace detail;
// Exact source ranges and missing density evidence: docs/experiments/grass-sward.md.
constexpr SwardProfile k_profiles[] = {
    {"tall_fescue", .05f, .70f, .003f, .013f, 3080, 3093, "open panicle"},
    {"perennial_ryegrass", .03f, .20f, .002f, .006f, 5815, 7414, "spike"},
    {"timothy", .04f, .20f, .003f, .009f, 0, 0, "spiciform panicle"},
    {"cocksfoot", .10f, .45f, .002f, .014f, 0, 0, "fascicled panicle"},
    {"wood_melick", .05f, .20f, .003f, .007f, 0, 0, "open panicle"},
    {"wavy_hairgrass", .05f, .20f, .0003f, .0008f, 0, 0, "open panicle"},
    {"bent", .02f, .15f, .001f, .004f, 0, 0, "open panicle"},
    {"red_fescue", .03f, .40f, .0005f, .001f, 0, 0, "panicle"},
    // G137: Kew GrassBase (Clayton et al.): Deschampsia cespitosa blades 10-60 cm x 2-5 mm, densely
    // caespitose; Nardus stricta blades 4-30 cm x 0.5-1 mm, caespitose (grass-sward.md).
    {"tufted_hairgrass", .10f, .60f, .002f, .005f, 0, 0, "open panicle"},
    {"mat_grass", .04f, .30f, .0005f, .001f, 0, 0, "one-sided spike"},
};
struct Blade {
  D3 root, heading;
  f64 length, width, lean, droop, twist;
  f64 tip = 1;
  u32 group;
  u8 hue, age;
};
SwardCoverage measure_sward_impl(const assets::MeshData& mesh, const TreeLevels& levels,
                                 const image::Image& card_image, u32 pixels, u32 first, u32 end,
                                 f64 across = .8660254037844386, f64 up = .5);
void fit_lods(assets::MeshData& mesh, TreeLevels& levels, u32 coarse_first,
              const Vector<Blade>& blades);

// A quadratic centre line, normalized to the requested polyline arc length. Both rungs use
// these same fine samples, so lowering tessellation does not redraw the plant's random choices.
Axis blade_axis(const Blade& b) {
  Axis axis;
  constexpr u32 rows = 24;
  f64 arc = 0;
  for (u32 r = 0; r <= rows; ++r) {
    const f64 t = static_cast<f64>(r) / rows;
    const D3 p = k_up * (t - b.droop * t * t) + b.heading * (b.lean * t * t);
    if (r != 0) arc += length_of(p - axis.point.back());
    axis.point.push_back(p);
    axis.tangent.push_back(normalized(k_up * (1 - 2 * b.droop * t) + b.heading * (2 * b.lean * t)));
    axis.normal.push_back(normalized(cross(b.heading, k_up)));
    axis.arc.push_back(arc);
  }
  const f64 scale = b.length / arc;
  for (u32 r = 0; r <= rows; ++r) {
    axis.point[r] = b.root + axis.point[r] * scale;
    axis.arc[r] *= scale;
  }
  axis.length = b.length;
  return axis;
}

// Defoliation removes the upper axis, retaining the original taper and a full-width cut end.
// Keep the first upward crossing, removing the detached tip even when an outer leaf arches down.
void clip_blade(Blade& b, Axis& axis, f64 height) {
  if (!(height > 0)) return;
  for (u32 r = 1; r < axis.point.size(); ++r) {
    if (axis.point[r].y < height) continue;
    const f64 t = (height - axis.point[r - 1].y) / (axis.point[r].y - axis.point[r - 1].y);
    axis.point[r] = axis.point[r - 1] + (axis.point[r] - axis.point[r - 1]) * t;
    axis.tangent[r] = normalized(axis.tangent[r - 1] * (1 - t) + axis.tangent[r] * t);
    axis.arc[r] = lerp(axis.arc[r - 1], axis.arc[r], t);
    axis.length = axis.arc[r];
    b.tip = axis.length / b.length;
    b.length = axis.length;
    axis.point.resize(r + 1);
    axis.tangent.resize(r + 1);
    axis.normal.resize(r + 1);
    axis.arc.resize(r + 1);
    return;
  }
}

void ribbon(const Blade& b, const Axis& fine, u32 segments, f64 width_scale, Emitter& emit) {
  const u32 first = emit.mesh.positions.size();
  for (u32 r = 0; r <= segments; ++r) {
    // Both rungs include the taper shoulder: uniform samples lost leaf area at two segments.
    const f64 t = r == segments ? 1
                                : (b.tip > .65 ? .65 / b.tip * r / (segments - 1)
                                               : static_cast<f64>(r) / segments);
    const f64 arc = t * fine.length;
    u32 row = 1;
    while (row + 1 < fine.arc.size() && fine.arc[row] < arc)
      ++row;
    const f64 blend = (arc - fine.arc[row - 1]) / (fine.arc[row] - fine.arc[row - 1]);
    const D3 point = fine.point[row - 1] + (fine.point[row] - fine.point[row - 1]) * blend;
    const D3 tangent = normalized(fine.tangent[row - 1] * (1 - blend) + fine.tangent[row] * blend);
    D3 side = rotate(normalized(cross(b.heading, k_up)), tangent, b.twist * t);
    if (r == segments && b.tip < 1) side = normalized(D3{side.x, 0, side.z});
    const D3 normal = normalized(cross(side, tangent));
    emit.weight = t;
    if (r == segments && b.tip == 1) {
      emit.vertex(point, normal, .5, 1);
    } else {
      const f64 natural_t = t * b.tip;
      const f64 taper = natural_t <= .65 ? 1 : (1 - natural_t) / .35;
      const D3 half = side * (b.width * width_scale * taper * .5);
      emit.vertex(point - half, normal, 0, t);
      emit.vertex(point + half, normal, 1, t);
    }
  }
  for (u32 r = 0; r < segments; ++r) {
    const u32 a = first + r * 2;
    emit.triangle(a, a + 1, a + 2);
    if (r + 1 < segments || b.tip < 1) emit.triangle(a + 1, a + 3, a + 2);
  }
}

void card(const BakeView& view, u32 tile, u32 columns, u32 rows, u32 px, Emitter& emit) {
  const u32 first = emit.mesh.positions.size();
  const f64 u0 = (static_cast<f64>((tile % columns) * px) + 1) / (columns * px);
  const f64 v0 = (static_cast<f64>((tile / columns) * px) + 1) / (rows * px);
  for (u32 v = 0; v < 2; ++v) {
    for (u32 u = 0; u < 2; ++u) {
      const f64 a = static_cast<f64>(u ? view.u_max : view.u_min);
      const f64 b = static_cast<f64>(v ? view.v_max : view.v_min);
      const D3 p = from_f32(view.origin) + from_f32(view.u_axis) * a + from_f32(view.v_axis) * b;
      emit.weight = clamp01(p.y / static_cast<f64>(emit.motion.parts[emit.part].length));
      emit.vertex(p, from_f32(view.normal), u0 + static_cast<f64>(u * (px - 2)) / (columns * px),
                  v0 + static_cast<f64>(v * (px - 2)) / (rows * px));
    }
  }
  emit.triangle(first, first + 1, first + 2);
  emit.triangle(first + 1, first + 3, first + 2);
}
}  // namespace

std::span<const SwardProfile> sward_profiles() noexcept { return k_profiles; }
const SwardProfile* sward_profile(std::string_view name) noexcept {
  for (const auto& p : k_profiles)
    if (p.name == name) return &p;
  return nullptr;
}
bool sward_in_range(const Sward& p, std::string& error) {
  const char* field = nullptr;
  if (sward_profile(p.species) == nullptr)
    field = "species";
  else if (p.variant > 15)
    field = "variant (0..15)";
  else if (p.cell_m != 1 && p.cell_m != 2)
    field = "cell_m (1 or 2)";
  else if (p.blades_m2 < 16 || p.blades_m2 > 1024)
    field = "blades_m2 (16..1024)";
  else if (!(p.density >= .0625f && p.density <= 1))
    field = "density (0.0625..1)";
  else if (!(p.growth >= 0 && p.growth <= 1))
    field = "growth (0..1)";
  else if (!(p.growth_min >= 0 && p.growth_min <= p.growth))
    field = "growth_min (0..growth)";
  else if (p.leaves_per_tiller < 1 || p.leaves_per_tiller > 8)
    field = "leaves_per_tiller (1..8)";
  else if (p.tillers_per_clump < 1 || p.tillers_per_clump > 32)
    field = "tillers_per_clump (1..32)";
  else if (!(p.clumpiness >= 0 && p.clumpiness <= 1))
    field = "clumpiness (0..1)";
  else if (!(p.clump_radius_m >= 0 && p.clump_radius_m <= .25f))
    field = "clump_radius_m (0..0.25)";
  else if (!(p.cut_height_m >= 0 && p.cut_height_m <= 1))
    field = "cut_height_m (0..1)";
  else if (!(p.cut_variation >= 0 && p.cut_variation <= .9f))
    field = "cut_variation (0..0.9)";
  else if (!(p.tussock_spacing_m == 0 || (p.tussock_spacing_m >= .2f && p.tussock_spacing_m <= 1 &&
                                          p.tussock_spacing_m >= 2 * p.clump_radius_m)))
    field = "tussock_spacing_m (0, or 0.2..1 and at least 2 clump_radius_m)";
  else if (!(p.crown_base_m >= 0 && p.crown_base_m <= .3f) ||
           (p.crown_base_m > 0 && p.tussock_spacing_m == 0))
    field = "crown_base_m (0..0.3, a tussock's only)";
  else if (!(p.between_share >= 0 && p.between_share <= .9f) ||
           (p.between_share > 0 && p.tussock_spacing_m == 0))
    field = "between_share (0..0.9, a tussock's only)";
  else if (!(p.between_growth >= 0 && p.between_growth <= 1))
    field = "between_growth (0..1)";
  else if (!(p.straw_share >= 0 && p.straw_share <= 1))
    field = "straw_share (0..1)";
  else if (!(p.card_cutoff >= .05f && p.card_cutoff <= .95f))
    field = "card_cutoff (0.05..0.95)";
  else if (p.segments < 2 || p.segments > 3)
    field = "segments (2 or 3)";
  else if (p.triangle_budget > 100000)
    field = "triangle_budget (0..100000)";
  else if (p.card_px < 32 || p.card_px > 256 || (p.card_px & (p.card_px - 1)) != 0)
    field = "card_px (power of two 32..256)";
  if (field != nullptr) {
    error = std::string("engine.flora.Sward invalid ") + field;
    return false;
  }
  return true;
}

bool solve_sward(const Sward& p, assets::MeshData& out, TreeReport* report, std::string* error,
                 Motion* motion, TreeLevels* levels,
                 Vector<geometry::ClusterFileSwardVertex>* attributes) {
  out = {};
  if (motion != nullptr) *motion = {};
  if (levels != nullptr) *levels = {};
  if (attributes != nullptr) attributes->clear();
  std::string why;
  if (!sward_in_range(p, why)) {
    if (error) *error = why;
    return false;
  }
  const auto& profile = *sward_profile(p.species);
  const u32 area = static_cast<u32>(p.cell_m * p.cell_m);
  const u32 count = std::max(2u, static_cast<u32>(p.blades_m2 * area * p.density));
  const u32 group_side = 4u * static_cast<u32>(p.cell_m);
  const u32 groups = group_side * group_side;
  // A cut leaf can have one extra end triangle. Budget conservatively before allocating.
  const u32 cut_extra = p.cut_height_m > 0 ? 1u : 0u;
  const u32 triangles =
      count * (blade_triangles(p.segments) + blade_triangles(2) + 2 * cut_extra) + groups * 6;
  const u32 budget = p.triangle_budget != 0 ? p.triangle_budget : 2400u * area;
  if (triangles > budget) {
    if (error)
      *error = "engine.flora.Sward triangle_budget needs " + std::to_string(triangles) +
               " for all levels; lower blades_m2 explicitly";
    return false;
  }
  // Leaf material stays index 0: no bark, no collider, and no dependency on a material pack.
  out.materials.push_back(material("blade", {.09f, .22f, .06f, 1}, .7f, true));
  Motion parts;
  Emitter emit{out, parts};
  Vector<Blade> blades;
  Vector<Axis> axes;
  Vector<BakeRange> ranges;
  const u32 leaves_per_crown = p.leaves_per_tiller * p.tillers_per_clump;
  const u32 crowns = (count + leaves_per_crown - 1) / leaves_per_crown;
  u32 side = 1;
  while (side * side < crowns)
    ++side;
  const f64 cell = static_cast<f64>(p.cell_m);
  const f64 half = cell * .5;
  const u64 seed = p.seed ^ (static_cast<u64>(p.variant) * 0x9e3779b97f4a7c15ull);
  // G137, the tussock layout: `t_side` crowns a side on a lattice of the spacing, each holding
  // `per_crown` of the first `crown_blades` blades, and the rest spread between them as short turf.
  // A crown moves at most 0.4 of what the spacing leaves beyond its radius, so neighbours' roots
  // stay at least a fifth of (spacing - 2 radius) apart. The sod layout below is untouched.
  const bool tussock = p.tussock_spacing_m > 0;
  const u32 between =
      tussock ? static_cast<u32>(static_cast<f64>(count) * static_cast<f64>(p.between_share)) : 0;
  const u32 crown_blades = count - between;
  const u32 t_side =
      tussock ? std::max(1u, static_cast<u32>(cell / static_cast<f64>(p.tussock_spacing_m) + .5))
              : 1u;
  const u32 per_crown = (crown_blades + t_side * t_side - 1) / (t_side * t_side);
  const u32 t_crowns = tussock ? (crown_blades + per_crown - 1) / per_crown : 0;
  const f64 crown_r = static_cast<f64>(p.clump_radius_m);
  for (u32 i = 0; i < count; ++i) {
    Rng rng = stream_of(seed, i + 1);
    Blade b{};
    u32 leaf = i % p.leaves_per_tiller;
    f64 shoot_angle = 0;
    if (!tussock) {
      const u32 crown = i / leaves_per_crown;
      const u32 tiller = i / p.leaves_per_tiller;
      Rng crng = stream_of(seed ^ 0x63726f776eull, crown + 1);
      Rng trng = stream_of(seed ^ 0x74696c6c6572ull, tiller + 1);
      const u32 row_count = std::min(side, crowns - (crown / side) * side);
      const u32 rows = (crowns + side - 1) / side;
      const D3 centre{((crown % side + crng.unit()) / row_count - .5) * cell, 0,
                      ((crown / side + crng.unit()) / rows - .5) * cell};
      shoot_angle = trng.unit() * 2 * k_pi;
      const f64 radius = std::sqrt(trng.unit()) * static_cast<f64>(p.clump_radius_m);
      const f64 spread = cell / side * .45;
      const f64 gather = static_cast<f64>(p.clumpiness);
      b.root =
          centre + D3{lerp(trng.symmetric() * spread, det::cos(shoot_angle) * radius, gather), 0,
                      lerp(trng.symmetric() * spread, det::sin(shoot_angle) * radius, gather)};
    } else if (i < crown_blades) {
      const u32 crown = i / per_crown;
      const u32 local = i - crown * per_crown;
      leaf = local % p.leaves_per_tiller;
      const u32 tiller = local / p.leaves_per_tiller;
      Rng crng = stream_of(seed ^ 0x7475737365ull, crown + 1);
      Rng trng = stream_of(seed ^ 0x74696c6c6572ull, (static_cast<u64>(crown) << 20) + tiller + 1);
      const f64 s = cell / t_side;
      const f64 slack = std::max(0.0, s - 2 * crown_r) * .4;
      const D3 centre{(crown % t_side + .5) * s - half + crng.symmetric() * slack, 0,
                      (crown / t_side + .5) * s - half + crng.symmetric() * slack};
      shoot_angle = trng.unit() * 2 * k_pi;
      const f64 radius = std::sqrt(trng.unit()) * crown_r;
      // The raised base: a dome over the crown, its rim at the ground.
      const f64 rim = crown_r > 0 ? radius / crown_r : 0;
      b.root = centre + D3{det::cos(shoot_angle) * radius,
                           static_cast<f64>(p.crown_base_m) * (1 - rim * rim),
                           det::sin(shoot_angle) * radius};
    } else {
      // Short turf between the crowns, single leaves anywhere in the cell.
      Rng trng = stream_of(seed ^ 0x6265747765656eull, i + 1);
      shoot_angle = trng.unit() * 2 * k_pi;
      b.root = D3{(trng.unit() - .5) * cell, 0, (trng.unit() - .5) * cell};
      leaf = 0;
    }
    const bool turf = tussock && i >= crown_blades;
    // Leaves alternate around a shared basal shoot; a little twist avoids identical fans.
    const f64 angle = shoot_angle + (leaf % 2) * k_pi + .15 * rng.symmetric();
    b.heading = {det::cos(angle), 0, det::sin(angle)};
    const f64 grow =
        turf ? static_cast<f64>(p.between_growth) * rng.unit()
             : lerp(static_cast<f64>(p.growth_min), static_cast<f64>(p.growth), rng.unit());
    b.length =
        lerp(static_cast<f64>(profile.length_min_m), static_cast<f64>(profile.length_max_m), grow);
    b.width = lerp(static_cast<f64>(profile.width_min_m), static_cast<f64>(profile.width_max_m),
                   rng.unit());
    const f64 outer =
        !turf && p.leaves_per_tiller > 1 ? static_cast<f64>(leaf) / (p.leaves_per_tiller - 1) : 0;
    b.lean = .2 + .7 * outer + .35 * rng.unit();
    b.droop = .65 * outer;
    b.twist = .2 * rng.symmetric();
    b.hue = static_cast<u8>(102 + rng.next() % 52);
    b.age = rng.unit() < static_cast<f64>(p.straw_share) ? 255 : 0;
    Axis axis = blade_axis(b);
    clip_blade(b, axis,
               static_cast<f64>(p.cut_height_m) *
                   (1 + static_cast<f64>(p.cut_variation) * rng.symmetric()));
    blades.push_back(b);
    axes.push_back(std::move(axis));
  }
  // Roots belong to the cell; foliage may overhang it. Confining the entire leaf created bald
  // seams between tiled stands. Actual mesh bounds include the overhang for culling/streaming.
  // Translate a whole crown at a boundary so individual shoots keep their shared roots.
  const u32 groups_of_roots = tussock ? t_crowns + between : crowns;
  for (u32 crown = 0; crown < groups_of_roots; ++crown) {
    u32 first = crown * leaves_per_crown, end = std::min(count, first + leaves_per_crown);
    if (tussock) {
      first = crown < t_crowns ? crown * per_crown : crown_blades + (crown - t_crowns);
      end = crown < t_crowns ? std::min(crown_blades, first + per_crown) : first + 1;
    }
    D3 lo{1e30, 0, 1e30}, hi{-1e30, 0, -1e30};
    for (u32 i = first; i < end; ++i) {
      const D3 v = blades[i].root;
      lo.x = std::min(lo.x, v.x);
      lo.z = std::min(lo.z, v.z);
      hi.x = std::max(hi.x, v.x);
      hi.z = std::max(hi.z, v.z);
    }
    const D3 shift{std::max(0.0, -half - lo.x) - std::max(0.0, hi.x - half), 0,
                   std::max(0.0, -half - lo.z) - std::max(0.0, hi.z - half)};
    for (u32 i = first; i < end; ++i) {
      blades[i].root = blades[i].root + shift;
      for (D3& v : axes[i].point)
        v = v + shift;
    }
  }
  for (u32 i = 0; i < count; ++i) {
    Blade& b = blades[i];
    const Axis& axis = axes[i];
    const u32 gx =
        static_cast<u32>(std::clamp((b.root.x + half) * 4, 0.0, static_cast<f64>(group_side - 1)));
    const u32 gz =
        static_cast<u32>(std::clamp((b.root.z + half) * 4, 0.0, static_cast<f64>(group_side - 1)));
    b.group = gz * group_side + gx;
    begin_structural_part(emit, b.root, axis.tangent[0], axis.length, geometry::k_motion_no_parent,
                          0, 0, 0);
    const u32 first = out.indices.size();
    ribbon(b, axis, p.segments, 1, emit);
    ranges.push_back({first, out.indices.size() - first});
  }
  const u32 near_end = out.indices.size();
  const f32 near_area = static_cast<f32>(triangles_area(out, 0));
  out.primitives.push_back({0, near_end, 0});
  const u32 coarse_first_vertex = out.positions.size();
  TreeLevels chain;
  chain.leaf_first_index = 0;
  chain.leaf_index_end = near_end;
  TreeLevel coarse;
  coarse.error = profile.width_max_m * 2;
  coarse.feature_m = profile.length_max_m;
  // Empty jobs distinguish a geometric rung from baked impostors in the sward build.
  // Keep every shoot and its width. Removing leaves from unrelated crowns made sparse
  // patches shift across the LOD boundary; reduce curve sampling instead.
  for (u32 i = 0; i < count; ++i) {
    emit.part = i;
    ribbon(blades[i], axes[i], 2, 1, emit);
  }
  for (u32 i = near_end; i < out.indices.size(); ++i)
    coarse.indices.push_back(out.indices[i]);
  out.indices.resize(near_end);
  chain.levels.push_back(std::move(coarse));
  TreeLevel cards;
  cards.error = std::max(.05f, profile.length_max_m * .5f);
  cards.feature_m = profile.length_max_m;
  cards.tile_px = p.card_px;
  cards.alpha_cutoff = p.card_cutoff;
  cards.columns = group_side * 3;
  cards.rows = group_side;
  for (u32 group = 0; group < groups; ++group) {
    Vector<BakeRange> selected;
    D3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (u32 i = 0; i < count; ++i) {
      if (blades[i].group != group) continue;
      selected.push_back(ranges[i]);
      for (u32 j = ranges[i].first_index; j < ranges[i].first_index + ranges[i].index_count; ++j) {
        const Vec3 v = out.positions[out.indices[j]];
        lo = {std::min(lo.x, static_cast<f64>(v.x)), std::min(lo.y, static_cast<f64>(v.y)),
              std::min(lo.z, static_cast<f64>(v.z))};
        hi = {std::max(hi.x, static_cast<f64>(v.x)), std::max(hi.y, static_cast<f64>(v.y)),
              std::max(hi.z, static_cast<f64>(v.z))};
      }
    }
    if (selected.empty()) continue;
    const D3 centre = (lo + hi) * .5;
    begin_structural_part(emit, {centre.x, 0, centre.z}, k_up, hi.y, geometry::k_motion_no_parent,
                          0, 0, 0);
    for (u32 plane = 0; plane < 3; ++plane) {
      BakeJob job;
      job.group = group;
      job.tile = group * 3 + plane;
      job.card_tile = job.tile;
      job.ranges = selected;
      job.view.origin = to_f32(centre);
      if (plane == 0) {
        job.view.u_axis = {1, 0, 0};
        job.view.v_axis = {0, 1, 0};
        job.view.normal = {0, 0, 1};
        job.view.u_min = static_cast<f32>(lo.x - centre.x);
        job.view.u_max = static_cast<f32>(hi.x - centre.x);
        job.view.v_min = static_cast<f32>(lo.y - centre.y);
        job.view.v_max = static_cast<f32>(hi.y - centre.y);
      } else if (plane == 1) {
        job.view.u_axis = {0, 0, 1};
        job.view.v_axis = {0, 1, 0};
        job.view.normal = {-1, 0, 0};
        job.view.u_min = static_cast<f32>(lo.z - centre.z);
        job.view.u_max = static_cast<f32>(hi.z - centre.z);
        job.view.v_min = static_cast<f32>(lo.y - centre.y);
        job.view.v_max = static_cast<f32>(hi.y - centre.y);
      } else {
        job.view.u_axis = {1, 0, 0};
        job.view.v_axis = {0, 0, 1};
        job.view.normal = {0, -1, 0};
        job.view.u_min = static_cast<f32>(lo.x - centre.x);
        job.view.u_max = static_cast<f32>(hi.x - centre.x);
        job.view.v_min = static_cast<f32>(lo.z - centre.z);
        job.view.v_max = static_cast<f32>(hi.z - centre.z);
      }
      job.card = job.view;
      const f32 um = (job.view.u_max - job.view.u_min) / (p.card_px - 2);
      const f32 vm = (job.view.v_max - job.view.v_min) / (p.card_px - 2);
      job.view.u_min -= um;
      job.view.u_max += um;
      job.view.v_min -= vm;
      job.view.v_max += vm;
      card(job.card, job.tile, cards.columns, cards.rows, p.card_px, emit);
      cards.jobs.push_back(std::move(job));
    }
    ++cards.groups;
  }
  for (u32 i = near_end; i < out.indices.size(); ++i)
    cards.indices.push_back(out.indices[i]);
  out.indices.resize(near_end);
  cards.texel_m = profile.length_max_m / (2 * p.card_px);
  cards.parallax_m = cards.error;
  chain.levels.push_back(std::move(cards));
  if (p.fit_lods) fit_lods(out, chain, coarse_first_vertex, blades);
  if (attributes != nullptr) {
    for (u32 v = 0; v < parts.vertex_part.size(); ++v) {
      const u32 part = parts.vertex_part[v];
      const u8 hue = part < blades.size() ? blades[part].hue : 128;
      const u8 age = part < blades.size() ? blades[part].age : 0;
      attributes->push_back({static_cast<u8>(89 + 166 * parts.vertex_weight[v]), 128, hue, age});
    }
  }
  if (report != nullptr) {
    *report = {};
    report->triangles = near_end / 3;
    for (const auto& level : chain.levels)
      report->triangles += level.indices.size() / 3;
    report->leaf_triangles = report->triangles;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->sprays = count;
    report->leaf_area_m2 = near_area;
    report->leaf_area_index = near_area / static_cast<f32>(area);
  }
  if (motion != nullptr) *motion = std::move(parts);
  if (levels != nullptr) *levels = std::move(chain);
  return true;
}

void bake_sward_ground(const assets::MeshData& mesh, const TreeLevels& levels, f32 cell_m,
                       u32 pixels, BakedAtlas& out, Vector<f32>* height) {
  TreeLevel level;
  level.tile_px = pixels;
  BakeJob job;
  job.view.u_axis = {1, 0, 0};
  job.view.v_axis = {0, 0, 1};
  job.view.normal = {0, 1, 0};
  job.view.u_min = job.view.v_min = -.5f * cell_m;
  job.view.u_max = job.view.v_max = .5f * cell_m;
  job.ranges.push_back({0, levels.leaf_index_end});
  job.card = job.view;
  level.jobs.push_back(std::move(job));
  BakeLook look;
  look.leaf = mesh.materials[0].base_color;
  bake_level(mesh, levels, level, look, out, height);
}
namespace {
image::Image card_image_for(const assets::MeshData& mesh, const TreeLevels& levels) {
  BakeLook look;
  look.leaf = mesh.materials[0].base_color;
  BakedAtlas cards;
  bake_level(mesh, levels, levels.levels[1], look, cards);
  image::Image image;
  image.width = cards.width;
  image.height = cards.height;
  image.channels = 4;
  image.pixels = std::move(cards.colour);
  return image;
}
// `across` and `up` are the view direction's horizontal and vertical parts: 30 degrees of
// elevation, the cook's fit, unless a caller names another (G136's coverage against elevation).
SwardCoverage measure_sward_impl(const assets::MeshData& mesh, const TreeLevels& levels,
                                 const image::Image& card_image, u32 pixels, u32 first, u32 end,
                                 f64 across, f64 up) {
  SwardCoverage result;
  if (levels.levels.size() != 2 || mesh.materials.empty()) return result;
  BakeLook look;
  look.leaf = mesh.materials[0].base_color;
  for (u32 view = 0; view < 4; ++view) {
    const f64 angle = view * .5 * k_pi;
    const D3 n{det::cos(angle) * across, up, det::sin(angle) * across};
    const D3 u = normalized(cross(k_up, n));
    const D3 v = normalized(cross(n, u));
    BakeView frame;
    frame.u_axis = to_f32(u);
    frame.v_axis = to_f32(v);
    frame.normal = to_f32(n);
    f64 u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
    for (const Vec3& p : mesh.positions) {
      const D3 q = from_f32(p);
      const f64 pu = detail::dot(q, u), pv = detail::dot(q, v);
      u0 = std::min(u0, pu);
      u1 = std::max(u1, pu);
      v0 = std::min(v0, pv);
      v1 = std::max(v1, pv);
    }
    frame.u_min = static_cast<f32>(u0);
    frame.u_max = static_cast<f32>(u1);
    frame.v_min = static_cast<f32>(v0);
    frame.v_max = static_cast<f32>(v1);
    for (u32 tier = first; tier < end; ++tier) {
      assets::MeshData source = mesh;
      if (tier != 0) source.indices = levels.levels[tier - 1].indices;
      TreeLevels source_levels;
      source_levels.leaf_index_end = source.indices.size();
      TreeLevel probe;
      probe.tile_px = pixels;
      BakeJob job;
      job.view = job.card = frame;
      job.ranges.push_back({0, source.indices.size()});
      probe.jobs.push_back(std::move(job));
      BakeLook sample = look;
      if (tier == 2) {
        sample.leaf = {1, 1, 1, 1};
        sample.leaf_image = &card_image;
        sample.leaf_cutoff = levels.levels[1].alpha_cutoff;
      }
      BakedAtlas projected;
      bake_level(source, source_levels, probe, sample, projected);
      u64 coverage = 0;
      for (u32 i = 3; i < projected.colour.size(); i += 4)
        coverage += projected.colour[i];
      result.fraction[tier][view] =
          static_cast<f32>(static_cast<f64>(coverage) / (pixels * pixels * 255));
    }
  }
  return result;
}

void fit_lods(assets::MeshData& mesh, TreeLevels& levels, u32 coarse_first,
              const Vector<Blade>& blades) {
  // Cold cook only. Four directions prevent fitting one flattering side; the public 256 px
  // probe independently measures the result. This fits area, not pixelwise shape or parallax.
  constexpr u32 pixels = 128;
  const image::Image empty;
  const auto reference = measure_sward_impl(mesh, levels, empty, pixels, 0, 2);
  auto measured = reference;
  f64 near_sum = 0;
  for (f32 v : measured.fraction[0])
    near_sum += static_cast<f64>(v);
  if (!(near_sum > 0)) return;
  f64 width = 1;
  // Coarse proxy width may change by at most a quarter; the natural near leaves never change.
  for (u32 iteration = 0; iteration < 3; ++iteration) {
    f64 coarse_sum = 0;
    for (f32 v : measured.fraction[1])
      coarse_sum += static_cast<f64>(v);
    if (!(coarse_sum > 0)) break;
    const f64 next = std::clamp(width * near_sum / coarse_sum, .75, 1.25);
    if (std::abs(next - width) < .001) break;
    const f32 factor = static_cast<f32>(next / width);
    // Each ribbon is pairs of edge vertices, optionally followed by one pointed tip.
    u32 at = coarse_first;
    for (const auto& blade : blades) {
      const u32 pairs = blade.tip < 1 ? 3u : 2u;
      for (u32 row = 0; row < pairs; ++row) {
        const Vec3 centre = (mesh.positions[at] + mesh.positions[at + 1]) * .5f;
        mesh.positions[at] = centre + (mesh.positions[at] - centre) * factor;
        mesh.positions[at + 1] = centre + (mesh.positions[at + 1] - centre) * factor;
        at += 2;
      }
      if (blade.tip == 1) ++at;
    }
    width = next;
    measured = measure_sward_impl(mesh, levels, empty, pixels, 1, 2);
  }
  const auto image = card_image_for(mesh, levels);
  f32 low = .05f, high = .95f, best = levels.levels[1].alpha_cutoff;
  f64 best_error = 1e30;
  for (u32 iteration = 0; iteration < 7; ++iteration) {
    const auto cards = measure_sward_impl(mesh, levels, image, pixels, 2, 3);
    f64 error = 0, sum = 0;
    for (u32 view = 0; view < 4; ++view) {
      const f64 near = static_cast<f64>(reference.fraction[0][view]);
      if (near > 0)
        error = std::max(error, std::abs(static_cast<f64>(cards.fraction[2][view]) / near - 1));
      sum += static_cast<f64>(cards.fraction[2][view]);
    }
    const f32 cutoff = levels.levels[1].alpha_cutoff;
    if (error < best_error) {
      best_error = error;
      best = cutoff;
    }
    if (sum > near_sum)
      low = cutoff;
    else
      high = cutoff;
    levels.levels[1].alpha_cutoff = (low + high) * .5f;
  }
  levels.levels[1].alpha_cutoff = best;
}
}  // namespace
// fdlibm's s_atan.c (Sun Microsystems, 1993), the reference the FreeBSD and glibc arctangents
// descend from: reduce |x| to |t| <= 7/16 against atan(0.5), atan(1), atan(1.5) or atan(inf), each
// in a high and a low part, then an odd polynomial of degree 23. Its coefficients are written as
// their bit patterns, as core/math/deterministic.h writes its own, so no decimal conversion stands
// between the source and the constant. IEEE + - * / only and contraction off: the same bits on
// every toolchain. fdlibm states an error under 1 ulp; the forms tests hold it to 2 against
// `std::atan`.
f64 sward_atan(f64 x) noexcept {
  constexpr auto b = [](u64 v) { return std::bit_cast<f64>(v); };
  static constexpr f64 hi[4] = {b(0x3FDDAC670561BB4Full), b(0x3FE921FB54442D18ull),
                                b(0x3FEF730BD281F69Bull), b(0x3FF921FB54442D18ull)};
  static constexpr f64 lo[4] = {b(0x3C7A2B7F222F65E2ull), b(0x3C81A62633145C07ull),
                                b(0x3C7007887AF0CBBDull), b(0x3C91A62633145C07ull)};
  static constexpr f64 at[11] = {
      b(0x3FD555555555550Dull), b(0xBFC999999998EBC4ull), b(0x3FC24924920083FFull),
      b(0xBFBC71C6FE231671ull), b(0x3FB745CDC54C206Eull), b(0xBFB3B0F2AF749A6Dull),
      b(0x3FB10D66A0D03D51ull), b(0xBFADDE2D52DEFD9Aull), b(0x3FA97B4B24760DEBull),
      b(0xBFA2B4442C6A6C2Full), b(0x3F90AD3AE322DA11ull)};
  const u64 bits = std::bit_cast<u64>(x);
  const bool negative = (bits >> 63) != 0;
  const u32 ix = static_cast<u32>(bits >> 32) & 0x7fffffffu;
  if (ix >= 0x44100000u) {  // |x| >= 2^66, or not a number
    if (x != x) return x;
    return negative ? -(hi[3] + lo[3]) : hi[3] + lo[3];
  }
  int id = -1;
  f64 t = x;
  if (ix < 0x3fdc0000u) {            // |x| < 7/16
    if (ix < 0x3e400000u) return x;  // |x| < 2^-27: atan(x) rounds to x
  } else {
    t = negative ? -x : x;
    if (ix < 0x3ff30000u) {    // |x| < 19/16
      if (ix < 0x3fe60000u) {  // 7/16 <= |x| < 11/16
        id = 0;
        t = (2.0 * t - 1.0) / (2.0 + t);
      } else {  // 11/16 <= |x| < 19/16
        id = 1;
        t = (t - 1.0) / (t + 1.0);
      }
    } else if (ix < 0x40038000u) {  // 19/16 <= |x| < 39/16
      id = 2;
      t = (t - 1.5) / (1.0 + 1.5 * t);
    } else {  // 39/16 <= |x| < 2^66
      id = 3;
      t = -1.0 / t;
    }
  }
  const f64 z = t * t, w = z * z;
  const f64 s1 = z * (at[0] + w * (at[2] + w * (at[4] + w * (at[6] + w * (at[8] + w * at[10])))));
  const f64 s2 = w * (at[1] + w * (at[3] + w * (at[5] + w * (at[7] + w * at[9]))));
  if (id < 0) return t - t * (s1 + s2);
  const f64 r = hi[id] - ((t * (s1 + s2) - lo[id]) - t);
  return negative ? -r : r;
}
SwardForm sward_site_form(const SwardStature& r, const SwardSite& s) noexcept {
  const f64 lo = static_cast<f64>(r.slope_lo_deg), hi = static_cast<f64>(r.slope_hi_deg);
  const f64 steep =
      hi > lo ? smoothstep01((s.slope_deg - lo) / (hi - lo)) : (s.slope_deg >= lo ? 1 : 0);
  const f64 wet = clamp01(s.wet), shade = clamp01(s.shade);
  const f64 tall = static_cast<f64>(r.tall_bias) +
                   static_cast<f64>(r.noise) * (2 * s.noise_tall - 1) +
                   static_cast<f64>(r.wet_tall) * wet + static_cast<f64>(r.steep_tall) * steep +
                   static_cast<f64>(r.shade_tall) * shade;
  const f64 tussock =
      static_cast<f64>(r.tussock_bias) + static_cast<f64>(r.noise) * (2 * s.noise_tussock - 1) +
      static_cast<f64>(r.wet_tussock) * wet + static_cast<f64>(r.steep_tussock) * steep +
      static_cast<f64>(r.shade_tussock) * shade;
  if (tall > 0) return tussock > 0 ? SwardForm::tall_tussock : SwardForm::tall_sod;
  return tussock > 0 ? SwardForm::short_tussock : SwardForm::short_sod;
}
f64 sward_mosaic_noise(u64 seed, f64 x_m, f64 z_m, f64 scale_m) noexcept {
  const f64 fx = x_m / scale_m, fz = z_m / scale_m;
  const f64 x0 = std::floor(fx), z0 = std::floor(fz);
  const i64 ix = static_cast<i64>(x0), iz = static_cast<i64>(z0);
  const auto ease = [](f64 t) { return t * t * (3 - 2 * t); };
  const f64 tx = ease(fx - x0), tz = ease(fz - z0);
  const auto node = [seed](i64 a, i64 b) {
    const u64 h = hash_combine(hash_combine(seed, static_cast<u64>(a)), static_cast<u64>(b));
    return static_cast<f64>(h >> 11) / 9007199254740992.0;
  };
  return lerp(lerp(node(ix, iz), node(ix + 1, iz), tx),
              lerp(node(ix, iz + 1), node(ix + 1, iz + 1), tx), tz);
}
SwardCoverage measure_sward_coverage(const assets::MeshData& mesh, const TreeLevels& levels) {
  if (levels.levels.size() != 2 || mesh.materials.empty()) return {};
  return measure_sward_impl(mesh, levels, card_image_for(mesh, levels), 256, 0, 3);
}
SwardCoverage measure_sward_coverage_at(const assets::MeshData& mesh, const TreeLevels& levels,
                                        f64 elevation_deg) {
  if (levels.levels.size() != 2 || mesh.materials.empty()) return {};
  const f64 e = elevation_deg * (k_pi / 180.0);
  return measure_sward_impl(mesh, levels, card_image_for(mesh, levels), 256, 0, 3, det::cos(e),
                            det::sin(e));
}
}  // namespace engine::flora
