// The herb's solver (understory.h; docs/subsystems/flora.md, "The herb").
#include "kit.h"

#include <domain/flora/structure.h>
#include <domain/flora/understory.h>

#include <cmath>
#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

constexpr u64 k_stem_stream = 11;
constexpr u64 k_leaf_stream = 12;
constexpr u64 k_flower_stream = 13;
constexpr u32 k_fine_rows = 16;

struct Plan {
  u32 stem_sides = 6;
  u32 stem_rows = 10;
  u32 petals = 0;
};

u32 count_triangles(const Herb& p, const Plan& plan) {
  return tube_triangles(plan.stem_rows, plan.stem_sides) + p.leaves * k_leaf_card_triangles +
         p.flowers * plan.petals * 3u;
}

// A flower: a fan of petal cards about `axis` from `centre` — each petal a diamond from the centre
// out at `angle` from the axis, widest at 0.4 of its length, and a wedge between neighbours, so a
// head is one connected surface (its disc the wedges) the LOD builder collapses whole.
void build_flower(D3 centre, D3 axis, u32 petals, f64 length, f64 width, f64 angle, f64 turn,
                  Emitter& out) {
  const D3 n = perpendicular(axis, (axis.y < 0.9 && axis.y > -0.9) ? k_up : D3{1.0, 0.0, 0.0});
  const D3 b = cross(axis, n);
  out.weight = 0.0;
  const u32 c = out.vertex(centre, axis, 0.5, 0.0);
  Vector<u32> left(petals);
  Vector<u32> right(petals);
  const f64 ca = det::cos(angle);
  const f64 sa = det::sin(angle);
  for (u32 k = 0; k < petals; ++k) {
    const f64 a = turn + 2.0 * k_pi * static_cast<f64>(k) / static_cast<f64>(petals);
    const D3 radial = n * det::cos(a) + b * det::sin(a);
    const D3 across = cross(axis, radial);
    const D3 d = normalized(axis * ca + radial * sa);
    out.weight = 0.4;
    left[k] = out.vertex(centre + d * (0.4 * length) - across * (0.5 * width), axis, 0.0, 0.4);
    right[k] = out.vertex(centre + d * (0.4 * length) + across * (0.5 * width), axis, 1.0, 0.4);
    out.weight = 1.0;
    const u32 tip = out.vertex(centre + d * length, axis, 0.5, 1.0);
    out.face(c, right[k], tip, axis);
    out.face(c, tip, left[k], axis);
  }
  for (u32 k = 0; k < petals; ++k)
    out.face(c, left[k], right[(k + 1) % petals], axis);
}

}  // namespace

bool herb_in_range(const Herb& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Herb.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.height_m >= 0.1f && p.height_m <= 2.0f)) return bad("height_m", "0.1 to 2 m");
  if (!(p.stem_radius_m >= 0.001f && p.stem_radius_m <= 0.015f))
    return bad("stem_radius_m", "0.001 to 0.015 m");
  if (!(p.lean_deg >= 0.0f && p.lean_deg <= 30.0f)) return bad("lean_deg", "0 to 30 degrees");
  if (!(p.wander_deg >= 0.0f && p.wander_deg <= 20.0f)) return bad("wander_deg", "0 to 20 degrees");
  if (p.leaves > 40u) return bad("leaves", "0 to 40");
  if (!(p.leaf_top >= 0.1f && p.leaf_top <= 0.95f)) return bad("leaf_top", "0.1 to 0.95");
  if (!(p.leaf_length_m >= 0.01f && p.leaf_length_m <= 0.3f))
    return bad("leaf_length_m", "0.01 to 0.3 m");
  if (!(p.leaf_width >= 0.1f && p.leaf_width <= 1.0f)) return bad("leaf_width", "0.1 to 1");
  if (!(p.leaf_angle_deg >= 20.0f && p.leaf_angle_deg <= 100.0f))
    return bad("leaf_angle_deg", "20 to 100 degrees");
  if (p.flowers > 40u) return bad("flowers", "0 to 40");
  if (!(p.spike >= 0.0f && p.spike <= 0.8f)) return bad("spike", "0 to 0.8");
  if (!(p.petals >= 3u && p.petals <= 40u)) return bad("petals", "3 to 40");
  if (!(p.petal_length_m >= 0.004f && p.petal_length_m <= 0.08f))
    return bad("petal_length_m", "0.004 to 0.08 m");
  if (!(p.petal_width >= 0.1f && p.petal_width <= 1.0f)) return bad("petal_width", "0.1 to 1");
  if (!(p.petal_angle_deg >= 10.0f && p.petal_angle_deg <= 100.0f))
    return bad("petal_angle_deg", "10 to 100 degrees");
  const f32 colour[3] = {p.flower_red, p.flower_green, p.flower_blue};
  const char* names[3] = {"flower_red", "flower_green", "flower_blue"};
  for (u32 i = 0; i < 3; ++i)
    if (!(colour[i] >= 0.0f && colour[i] <= 1.0f)) return bad(names[i], "0 to 1");
  if (!(p.triangle_budget >= 1000u && p.triangle_budget <= 50000u))
    return bad("triangle_budget", "1,000 to 50,000");
  return true;
}

bool solve_herb(const Herb& p, assets::MeshData& out, TreeReport* report, std::string* error,
                Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!herb_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  // The ladder: the stem's rows and sides, then the petals halved; never a leaf or a flower.
  Plan plan;
  plan.petals = p.petals;
  u32 steps = 0;
  const auto fits = [&] { return count_triangles(p, plan) <= p.triangle_budget; };
  if (!fits()) {
    plan.stem_rows = 5;
    plan.stem_sides = 4;
    ++steps;
  }
  if (!fits()) {
    plan.stem_rows = 3;
    plan.stem_sides = 3;
    ++steps;
  }
  while (!fits() && plan.petals > 3u) {
    plan.petals = (plan.petals + 1) / 2 < 3u ? 3u : (plan.petals + 1) / 2;
    ++steps;
  }
  if (!fits()) {
    if (error != nullptr) {
      *error = "engine.flora.Herb.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this herb: its coarsest form needs " +
               std::to_string(count_triangles(p, plan)) + " triangles";
    }
    return false;
  }

  out.materials.push_back(material("leaf", Vec4{0.16f, 0.32f, 0.08f, 1.0f}, 0.6f, true));
  if (p.flowers > 0u) {
    out.materials.push_back(
        material("flower", Vec4{p.flower_red, p.flower_green, p.flower_blue, 1.0f}, 0.5f, true));
  }
  Motion parts;
  Emitter emit{out, parts};
  const f64 r_stem = static_cast<f64>(p.stem_radius_m);
  // Part 0 is the root: a pivot at the ground and no geometry. The stem hangs from it, so a
  // walker's push bends the stem too.
  begin_structural_part(emit, D3{}, k_up, 0.02, geometry::k_motion_no_parent, r_stem, r_stem, 0u);

  Rng stem_rng = stream_of(p.seed, k_stem_stream);
  const f64 lean = static_cast<f64>(p.lean_deg) * k_degree;
  const f64 azimuth = stem_rng.unit() * 2.0 * k_pi;
  const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
  const f64 height = static_cast<f64>(p.height_m);
  const Axis stem = grow_axis(D3{}, k_up * det::cos(lean) + heading * det::sin(lean),
                              height / det::cos(lean), k_fine_rows, k_up, 0.6 * lean / height,
                              static_cast<f64>(p.wander_deg) * k_degree, stem_rng);
  const f64 r_top = 0.45 * r_stem;
  const u32 stem_part = begin_structural_part(emit, stem.point[0], stem.tangent[0], stem.length, 0,
                                              r_stem, r_top, 0u);
  build_tube(resample_axis(stem, plan.stem_rows), r_stem, r_top, plan.stem_sides, Vec2{0.0f, 0.0f},
             emit);

  // The leaves: one card each, its own part hanging from the stem, the lowest the longest.
  Rng leaf_rng = stream_of(p.seed, k_leaf_stream);
  const f64 turn = leaf_rng.unit() * 2.0 * k_pi;
  const f64 leaf_angle = static_cast<f64>(p.leaf_angle_deg) * k_degree;
  for (u32 j = 0; j < p.leaves; ++j) {
    const u32 slot = p.opposite ? j / 2u : j;
    const u32 slots = p.opposite ? (p.leaves + 1u) / 2u : p.leaves;
    const f64 t = slots > 1u ? static_cast<f64>(slot) / static_cast<f64>(slots - 1u) : 0.0;
    const f64 s = stem.length * (0.05 + (static_cast<f64>(p.leaf_top) - 0.05) * t);
    const f64 jitter_around = leaf_rng.symmetric();
    const f64 around = p.opposite
                           ? turn + 0.5 * k_pi * static_cast<f64>(slot) +
                                 (j % 2u == 1u ? k_pi : 0.0) + 0.1 * jitter_around
                           : turn + static_cast<f64>(j) * k_golden_angle + 0.15 * jitter_around;
    const f64 jitter_length = leaf_rng.unit();
    const f64 length =
        static_cast<f64>(p.leaf_length_m) * (1.0 - 0.5 * t) * (0.9 + 0.2 * jitter_length);
    D3 radial;
    const D3 dir = branch_direction(stem, s, around, leaf_angle, &radial);
    const f64 r_here = lerp(r_stem, r_top, s / stem.length);
    const D3 base = axis_point(stem, s) + radial * r_here;
    begin_structural_part(emit, base, dir, length, stem_part, 0.0, 0.0, 0u);
    D3 side = cross(dir, k_up);
    if (dot(side, side) < 1e-9) side = radial;
    const u32 first = out.positions.size();
    build_leaf_card(base, dir, normalized(side), length, static_cast<f64>(p.leaf_width) * length,
                    0.15, emit);
    // A leaf's bend weight by its station: the base 0, the widest 0.35, the tip 1.
    const f32 weights[4] = {0.0f, 0.35f, 0.35f, 1.0f};
    for (u32 v = 0; v < 4u; ++v)
      parts.vertex_weight[first + v] = weights[v];
  }
  assets::Primitive leaves;
  leaves.first_index = 0;
  leaves.index_count = out.indices.size();
  leaves.material = 0;
  out.primitives.push_back(leaves);

  // The flowers: one head at the stem's top, or a spike up its top `spike` share, each its own
  // part. A spike's flowers nod out from the stem, a single head faces along it.
  u32 flower_triangles = 0;
  if (p.flowers > 0u) {
    const u32 flower_first = out.indices.size();
    Rng flower_rng = stream_of(p.seed, k_flower_stream);
    const f64 spin = flower_rng.unit() * 2.0 * k_pi;
    const f64 petal_length = static_cast<f64>(p.petal_length_m);
    const f64 spike = static_cast<f64>(p.spike);
    for (u32 f = 0; f < p.flowers; ++f) {
      const bool head = p.flowers == 1u && spike <= 0.0;
      const f64 t = p.flowers > 1u ? static_cast<f64>(f) / static_cast<f64>(p.flowers - 1u) : 1.0;
      const f64 s = head ? stem.length : stem.length * (1.0 - spike * (1.0 - t));
      D3 axis = axis_tangent(stem, s);
      D3 centre = axis_point(stem, s);
      if (!head) {
        D3 radial;
        const f64 jitter_around = flower_rng.symmetric();
        axis = branch_direction(stem, s,
                                spin + static_cast<f64>(f) * k_golden_angle + 0.2 * jitter_around,
                                70.0 * k_degree, &radial);
        centre = centre + radial * (r_top + 0.3 * petal_length);
      }
      begin_structural_part(emit, centre, axis, petal_length, stem_part, 0.0, 0.0, 0u);
      const f64 jitter_turn = flower_rng.unit();
      build_flower(centre, axis, plan.petals, petal_length,
                   static_cast<f64>(p.petal_width) * petal_length,
                   static_cast<f64>(p.petal_angle_deg) * k_degree, jitter_turn * 2.0 * k_pi, emit);
    }
    assets::Primitive flowers;
    flowers.first_index = flower_first;
    flowers.index_count = out.indices.size() - flower_first;
    flowers.material = 1;
    out.primitives.push_back(flowers);
    flower_triangles = flowers.index_count / 3;
  }

  if (report != nullptr) {
    *report = TreeReport{};
    report->triangles = out.indices.size() / 3;
    report->wood_triangles = 0;
    report->leaf_triangles = report->triangles - flower_triangles;
    report->fruit_triangles = flower_triangles;  // the flower heads, the herb's third kind
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->shoots = 1;
    report->sprays = p.flowers;
    report->cards_per_side = plan.petals;
    report->trunk_sides = plan.stem_sides;
    report->steps = steps;
    report->leaf_cards = p.leaves;
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora
