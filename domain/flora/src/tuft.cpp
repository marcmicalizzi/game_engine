// The grass tuft's solver (understory.h; docs/subsystems/flora.md, "The grass tuft").
#include "kit.h"

#include <domain/flora/structure.h>
#include <domain/flora/understory.h>

#include <cmath>
#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

constexpr u64 k_blade_stream = 11;  // one stream a blade, after the tuft's turn
constexpr u32 k_fine_rows = 16;     // a blade's resolution, whatever the budget

u32 count_triangles(const Tuft& p, u32 segments, bool crossed) {
  return p.blades * blade_triangles(segments) * (crossed ? 2u : 1u);
}

}  // namespace

bool tuft_in_range(const Tuft& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Tuft.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.blades >= 4u && p.blades <= 400u)) return bad("blades", "4 to 400");
  if (!(p.blade_length_m >= 0.05f && p.blade_length_m <= 1.2f))
    return bad("blade_length_m", "0.05 to 1.2 m");
  if (!(p.blade_width_m >= 0.002f && p.blade_width_m <= 0.03f))
    return bad("blade_width_m", "0.002 to 0.03 m");
  if (!(p.spread_deg >= 0.0f && p.spread_deg <= 80.0f)) return bad("spread_deg", "0 to 80 degrees");
  if (!(p.droop >= 0.0f && p.droop <= 1.0f)) return bad("droop", "0 to 1");
  if (!(p.base_radius_m >= 0.002f && p.base_radius_m <= 0.3f))
    return bad("base_radius_m", "0.002 to 0.3 m");
  if (!(p.segments >= 2u && p.segments <= 8u)) return bad("segments", "2 to 8");
  if (!(p.triangle_budget >= 1500u && p.triangle_budget <= 50000u))
    return bad("triangle_budget", "1,500 to 50,000");
  return true;
}

bool solve_tuft(const Tuft& p, assets::MeshData& out, TreeReport* report, std::string* error,
                Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!tuft_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  // The ladder: the crossing first, then the rows, never a blade.
  u32 segments = p.segments;
  bool crossed = p.crossed;
  u32 steps = 0;
  while (count_triangles(p, segments, crossed) > p.triangle_budget) {
    if (crossed) {
      crossed = false;
    } else if (segments > 2u) {
      --segments;
    } else {
      break;
    }
    ++steps;
  }
  if (count_triangles(p, segments, crossed) > p.triangle_budget) {
    if (error != nullptr) {
      *error = "engine.flora.Tuft.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tuft: its coarsest form needs " +
               std::to_string(count_triangles(p, segments, crossed)) + " triangles";
    }
    return false;
  }

  out.materials.push_back(material("blade", Vec4{0.22f, 0.34f, 0.09f, 1.0f}, 0.7f, true));
  Motion parts;
  Emitter emit{out, parts};
  const f64 base_r = static_cast<f64>(p.base_radius_m);
  // Part 0 is the tuft's base: a pivot at the ground and no geometry.
  begin_structural_part(emit, D3{}, k_up, 0.02, geometry::k_motion_no_parent, base_r, base_r, 0u);

  Rng turn_rng = stream_of(p.seed, k_blade_stream - 1);
  const f64 turn = turn_rng.unit() * 2.0 * k_pi;
  const f64 spread = static_cast<f64>(p.spread_deg) * k_degree;
  const f64 width = static_cast<f64>(p.blade_width_m);
  for (u32 i = 0; i < p.blades; ++i) {
    Rng rng = stream_of(p.seed, k_blade_stream + i);
    // The blades fill the base as a sunflower does; the inner ones stand upright and the outer
    // ones lean out and arch further (a tussock's fountain).
    const f64 out_share = (static_cast<f64>(i) + 0.5) / static_cast<f64>(p.blades);
    const f64 jitter_spread = rng.unit();
    const f64 angle = spread * std::sqrt(out_share) * (0.85 + 0.3 * jitter_spread);
    const f64 jitter_around = rng.symmetric();
    const f64 around = turn + static_cast<f64>(i) * k_golden_angle + 0.2 * jitter_around;
    const D3 heading{det::cos(around), 0.0, det::sin(around)};
    const D3 dir = k_up * det::cos(angle) + heading * det::sin(angle);
    const f64 jitter_length = rng.unit();
    const f64 length = static_cast<f64>(p.blade_length_m) * (0.6 + 0.4 * jitter_length);
    const f64 jitter_droop = rng.unit();
    const f64 droop = static_cast<f64>(p.droop) * (0.4 + 0.6 * std::sqrt(out_share)) *
                      (0.7 + 0.6 * jitter_droop) * 0.5 * k_pi;
    const D3 foot = heading * (base_r * std::sqrt(out_share));
    const Axis axis =
        grow_axis(foot, dir, length, k_fine_rows, k_up * -1.0, droop / length, 0.03, rng);
    begin_structural_part(emit, axis.point[0], axis.tangent[0], axis.length, 0, 0.0, 0.0, 0u);
    // The blade's width lies across its arch, so its face looks up and out.
    const D3 side = normalized(cross(heading, k_up));
    const Axis mesh_axis = resample_axis(axis, segments);
    build_blade(mesh_axis, width, side, emit);
    if (crossed) build_blade(mesh_axis, width, normalized(cross(axis.tangent[0], side)), emit);
  }
  assets::Primitive blades;
  blades.first_index = 0;
  blades.index_count = out.indices.size();
  blades.material = 0;
  out.primitives.push_back(blades);

  if (report != nullptr) {
    *report = TreeReport{};
    report->triangles = out.indices.size() / 3;
    report->leaf_triangles = report->triangles;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->sprays = p.blades;
    report->cards_per_side = segments;
    report->steps = steps;
    report->leaf_cards = p.blades * (crossed ? 2u : 1u);
    report->leaf_area_m2 = static_cast<f32>(triangles_area(out, 0));
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora
