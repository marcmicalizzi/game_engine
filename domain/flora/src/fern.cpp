// The fern's solver (understory.h; docs/subsystems/flora.md, "The fern").
#include "ground_plant.h"

#include <domain/flora/structure.h>
#include <domain/flora/understory.h>

#include <cmath>
#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

constexpr u64 k_frond_stream = 11;  // one stream a frond, after the crown's turn
constexpr u32 k_fine_rows = 16;     // a rachis's resolution, whatever the budget
constexpr u32 k_min_stations = 3;   // the ladder's floor: three cards a side
constexpr f64 k_stipe_half_width = 0.004;

// Stations along a frond, stipe and blade: the blade's `pinnae` and as many again in the stipe's
// share, so the cards are evenly spaced from the crown to the tip.
u32 stations_of(const Fern& p, u32 pinnae) {
  const f64 stipe = static_cast<f64>(p.stipe);
  const f64 total = static_cast<f64>(pinnae) / (1.0 - stipe);
  u32 n = static_cast<u32>(total + 0.5);
  return n < pinnae + 1 ? pinnae + 1 : n;
}

// The card lengths along a frond: a few millimetres on the stipe (the stalk as a ribbon of the
// same surface), and on the blade a lanceolate outline — the longest pinnae a little below the
// middle, the lowest about half of them, the tip closing to a few percent.
void profile_of(const Fern& p, f64 length, u32 stations, Vector<f64>& out) {
  out.resize(stations + 1);
  const f64 stipe = static_cast<f64>(p.stipe);
  const f64 longest = static_cast<f64>(p.pinna_length) * length;
  for (u32 k = 0; k <= stations; ++k) {
    const f64 t = static_cast<f64>(k) / static_cast<f64>(stations);
    if (t < stipe) {
      out[k] = k_stipe_half_width;
      continue;
    }
    const f64 u = (t - stipe) / (1.0 - stipe);
    f64 shape = det::sin(k_pi * (0.15 + 0.85 * u));
    if (shape < 0.06) shape = 0.06;
    out[k] = longest * shape;
  }
}

// Separate pinnae leave real gaps in the silhouette. Their width is 20% of length at full
// detail; a coarser pair covers at most twice that width, without changing frond structure.

void build_pinnate_frond(const Fern& p, const Axis& axis, u32 pinnae, D3 side, Emitter& emit) {
  build_blade(resample_axis(axis, fern_rachis_rows(pinnae)), 0.003, side, emit);
  const f64 angle = static_cast<f64>(p.pinna_angle_deg) * k_degree;
  const f64 stipe = static_cast<f64>(p.stipe);
  const f64 merged = static_cast<f64>(p.pinnae) / pinnae;
  const f64 width_scale = merged < 2.0 ? merged : 2.0;
  for (u32 i = 0; i < pinnae; ++i) {
    const f64 u = (static_cast<f64>(i) + 0.5) / pinnae;
    const f64 t = stipe + (1.0 - stipe) * u;
    const D3 centre = axis_point(axis, axis.length * t);
    const D3 tangent = axis_tangent(axis, axis.length * t);
    const D3 lateral = perpendicular(tangent, side);
    D3 normal = normalized(cross(lateral, tangent));
    if (normal.y < 0.0) normal = normal * -1.0;
    const f64 length =
        static_cast<f64>(p.pinna_length) * axis.length * det::sin(k_pi * (0.15 + 0.85 * u));
    for (u32 j = 0; j < 2u; ++j) {
      const D3 direction =
          tangent * det::cos(angle) + lateral * ((j == 0u ? -1.0 : 1.0) * det::sin(angle));
      const D3 across = normalized(cross(direction, normal));
      emit.weight = t;  // the whole pinna rides its rachis station, with no new motion part
      const u32 first = emit.mesh.positions.size();
      build_leaf_card(centre, direction, across, length, length * 0.2 * width_scale, 0.0, emit);
      for (u32 v = first; v < emit.mesh.positions.size(); ++v)
        if (emit.mesh.positions[v].y < 0.002f) emit.mesh.positions[v].y = 0.002f;
    }
  }
}

}  // namespace

bool fern_in_range(const Fern& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Fern.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.fronds >= 1u && p.fronds <= 40u)) return bad("fronds", "1 to 40");
  if (!(p.frond_length_m >= 0.15f && p.frond_length_m <= 2.5f))
    return bad("frond_length_m", "0.15 to 2.5 m");
  if (!(p.stipe >= 0.05f && p.stipe <= 0.5f)) return bad("stipe", "0.05 to 0.5");
  if (!(p.pinnae >= 3u && p.pinnae <= 40u)) return bad("pinnae", "3 to 40");
  if (!(p.pinna_length >= 0.05f && p.pinna_length <= 0.4f))
    return bad("pinna_length", "0.05 to 0.4");
  if (!(p.pinna_angle_deg >= 30.0f && p.pinna_angle_deg <= 90.0f))
    return bad("pinna_angle_deg", "30 to 90 degrees");
  if (!(p.spread_deg >= 5.0f && p.spread_deg <= 85.0f)) return bad("spread_deg", "5 to 85 degrees");
  if (!(p.droop >= 0.0f && p.droop <= 1.0f)) return bad("droop", "0 to 1");
  if (!(p.crown_radius_m >= 0.005f && p.crown_radius_m <= 0.3f))
    return bad("crown_radius_m", "0.005 to 0.3 m");
  if (!(p.triangle_budget >= 500u && p.triangle_budget <= 100000u))
    return bad("triangle_budget", "500 to 100,000");
  if (!(p.rhizome_length_m >= 0.0f && p.rhizome_length_m <= 4.0f))
    return bad("rhizome_length_m", "0 to 4 m");
  if (p.dead_fronds > 40u) return bad("dead_fronds", "0 to 40");
  return true;
}

bool solve_fern(const Fern& p, assets::MeshData& out, TreeReport* report, std::string* error,
                Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!fern_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  // The ladder: the blade's pinnae halved until the fronds fit, never a frond dropped.
  u32 pinnae = p.pinnae;
  u32 steps = 0;
  const auto triangles = [&](u32 n) {
    return p.fronds * (p.arching ? fern_pinnate_triangles(n) : spray_triangles(stations_of(p, n))) +
           p.dead_fronds * fern_pinnate_triangles(n);
  };
  while (triangles(pinnae) > p.triangle_budget && pinnae > k_min_stations) {
    pinnae = (pinnae + 1) / 2 < k_min_stations ? k_min_stations : (pinnae + 1) / 2;
    ++steps;
  }
  if (triangles(pinnae) > p.triangle_budget) {
    if (error != nullptr) {
      *error = "engine.flora.Fern.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this fern: its coarsest form needs " +
               std::to_string(triangles(pinnae)) + " triangles";
    }
    return false;
  }
  const u32 stations = stations_of(p, pinnae);

  out.materials.push_back(material("frond", Vec4{0.12f, 0.30f, 0.07f, 1.0f}, 0.55f, true));
  if (p.dead_fronds > 0u)
    out.materials.push_back(material("dead_frond", Vec4{0.28f, 0.13f, 0.045f, 1.0f}, 0.85f, true));
  Motion parts;
  Emitter emit{out, parts};
  const f64 crown = static_cast<f64>(p.crown_radius_m);
  // Part 0 is the rhizome's crown: a pivot at the ground and no geometry.
  begin_structural_part(emit, D3{}, k_up, 0.05, geometry::k_motion_no_parent, crown, crown, 0u);

  Rng turn_rng = stream_of(p.seed, k_frond_stream - 1);
  const f64 turn = turn_rng.unit() * 2.0 * k_pi;
  const f64 spread = static_cast<f64>(p.spread_deg) * k_degree;
  Vector<f64> lengths;
  u32 dead_first = 0;
  for (u32 i = 0; i < p.fronds + p.dead_fronds; ++i) {
    const bool dead = i >= p.fronds;
    if (i == p.fronds) dead_first = out.indices.size();
    Rng rng = stream_of(p.seed, k_frond_stream + i);
    // The youngest frond (0) stands nearly upright at the crown's middle, the oldest leans out
    // furthest: a shuttlecock, its spread by the square root of age as the palm's crown's is.
    const f64 age = dead ? (static_cast<f64>(i - p.fronds) + 0.5) / p.dead_fronds
                         : (static_cast<f64>(i) + 0.5) / p.fronds;
    const f64 jitter_spread = rng.unit();
    const f64 angle = dead ? (78.0 + 8.0 * jitter_spread) * k_degree
                           : spread * std::sqrt(age) * (0.94 + 0.12 * jitter_spread);
    const f64 jitter_around = rng.symmetric();
    const f64 around = turn + static_cast<f64>(i) * k_golden_angle + 0.15 * jitter_around;
    const D3 heading{det::cos(around), 0.0, det::sin(around)};
    const D3 dir = k_up * det::cos(angle) + heading * det::sin(angle);
    const f64 jitter_length = rng.unit();
    const f64 length =
        static_cast<f64>(p.frond_length_m) * (0.75 + 0.25 * age) * (0.95 + 0.1 * jitter_length);
    // The rachis arches over under its own weight, the older fronds more.
    const f64 jitter_droop = rng.unit();
    const f64 droop =
        static_cast<f64>(p.droop) * (0.5 + 0.5 * age) * (0.9 + 0.2 * jitter_droop) * 0.5 * k_pi;
    D3 base = heading * (crown * std::sqrt(age));
    if (p.rhizome_length_m > 0.0f) {
      // Each frond is a ramet on a local meandering rhizome; count is not a field density.
      const u32 slot = dead ? i - p.fronds : i;
      const u32 count = dead ? p.dead_fronds : p.fronds;
      const f64 along = count > 1u ? static_cast<f64>(slot) / (count - 1u) : 0.5;
      const D3 run{det::cos(turn), 0.0, det::sin(turn)};
      const D3 across = cross(run, k_up);
      base = run * ((along - 0.5) * static_cast<f64>(p.rhizome_length_m)) +
             across * (crown * det::sin(along * 2.0 * k_pi));
    }
    const Axis axis =
        (p.arching || dead)
            ? ground_plant_axis(base, heading, angle, dead ? 25.0 * k_degree : droop, length)
            : grow_axis(base, dir, length, k_fine_rows, k_up * -1.0, droop / length, 0.02, rng);
    begin_structural_part(emit, axis.point[0], axis.tangent[0], axis.length, 0, 0.0, 0.0, 0u);
    profile_of(p, length, stations, lengths);
    D3 side = cross(dir, k_up);
    if (dot(side, side) < 1e-9) side = cross(heading, k_up);
    if (p.arching || dead) {
      build_pinnate_frond(p, axis, pinnae, normalized(side), emit);
    } else {
      build_spray_profile(axis, 0.0, stations, lengths,
                          static_cast<f64>(p.pinna_angle_deg) * k_degree, 8.0 * k_degree, 0.05,
                          normalized(side), emit);
    }
  }
  assets::Primitive fronds;
  fronds.first_index = 0;
  fronds.index_count = p.dead_fronds > 0u ? dead_first : out.indices.size();
  fronds.material = 0;
  out.primitives.push_back(fronds);
  if (p.dead_fronds > 0u) {
    assets::Primitive dead;
    dead.first_index = dead_first;
    dead.index_count = out.indices.size() - dead_first;
    dead.material = 1;
    out.primitives.push_back(dead);
  }
  smooth_normals(out, 0, 0);

  if (report != nullptr) {
    *report = TreeReport{};
    report->triangles = out.indices.size() / 3;
    report->leaf_triangles = report->triangles;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->sprays = p.fronds + p.dead_fronds;
    report->cards_per_side = p.arching ? pinnae : stations;
    report->steps = steps;
    report->leaf_cards = report->leaf_triangles / 2;
    report->leaf_area_m2 = static_cast<f32>(triangles_area(out, 0));
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora
