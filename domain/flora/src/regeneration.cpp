// Every botanical observation and authored coefficient is recorded in flora-regeneration.md.
#include "kit.h"

#include <domain/flora/regeneration.h>
#include <domain/flora/structure.h>
#include <domain/flora/understory.h>
#include <domain/geometry/cluster_lod.h>

#include <algorithm>
#include <utility>

namespace engine::flora {
namespace {
using namespace detail;
using Species = geometry::FloraSpecies;
constexpr RegenerationProfile k_profiles[] = {
    {Species::maple, .10f, .08f, .007f, .27f, 48, 2, 1, 12000, true, false, false},
    {Species::oak, .12f, .06f, .010f, .32f, 55, 1, 1, 12000, false, false, true},
    {Species::birch, .05f, .035f, .005f, .23f, 38, 1, 1, 12000, false, false, false},
    {Species::pine, .08f, .0009f, .008f, .30f, 65, 4, 1, 18000, false, true, false},
    {Species::spruce, .02f, .001f, .008f, .28f, 80, 5, 1, 18000, false, true, false},
    {Species::beaked_hazel, .07f, .05f, .009f, .32f, 40, 1, 3, 24000, false, false, true},
    {Species::red_osier_dogwood, .09f, .045f, .007f, .34f, 48, 2, 3, 24000, true, false, true},
};
constexpr u32 k_rows = 16;
constexpr u32 k_far_triangles = 2 * 4 * ImpostorSpec::k_tree_slices;

D3 centre(const RegenerationObstacle& obstacle) {
  return {static_cast<f64>(obstacle.x_m), static_cast<f64>(obstacle.y_m),
          static_cast<f64>(obstacle.z_m)};
}
f64 segment_distance(D3 point, D3 a, D3 b) {
  const D3 edge = b - a;
  const f64 squared = dot(edge, edge);
  const f64 t = squared > 0 ? clamp01(dot(point - a, edge) / squared) : 0;
  return length_of(point - (a + edge * t));
}
bool clear(const Regeneration& p, D3 a, D3 b, f64 radius) {
  for (const auto& obstacle : p.obstacles)
    if (segment_distance(centre(obstacle), a, b) <=
        static_cast<f64>(obstacle.radius_m) + radius + .001)
      return false;
  return true;
}

// Grow a prescribed axis, with geometric steering around the supplied solids. Each step is
// checked as a swept segment, not merely its endpoint. Every rung retains these same stations.
bool grow(const Regeneration& p, D3 base, D3 heading, f64 length, f64 radius, u64 stream,
          Axis& axis, std::string& error) {
  if (length < .001) {
    error = "regeneration: a shoot is shorter than 1 mm; increase recovery or set it to zero";
    return false;
  }
  if (!clear(p, base, base, radius)) {
    error = "regeneration.obstacles: a shoot attachment lies inside an obstacle";
    return false;
  }
  Rng rng = stream_of(p.seed, stream);
  const f64 turn = rng.unit() * 2 * k_pi;
  const D3 wander{det::cos(turn), 0, det::sin(turn)};
  axis.point.push_back(base);
  const f64 step = length / k_rows;
  D3 direction = normalized(heading);
  for (u32 row = 1; row <= k_rows; ++row) {
    const D3 previous = axis.point.back();
    const f64 t = static_cast<f64>(row) / k_rows;
    D3 desired = normalized(heading * (1 - .35 * t) + k_up * (.35 * t) + wander * (.03 * t));
    for (const auto& obstacle : p.obstacles) {
      const D3 away = previous - centre(obstacle);
      const f64 distance = length_of(away);
      const f64 margin = static_cast<f64>(obstacle.radius_m) + radius + step * 3;
      if (distance < margin) {
        // Tangential deflection leaves a vertical stem alongside a rock instead of entering it.
        D3 tangent = away - desired * dot(away, desired);
        if (length_of(tangent) < .0001) tangent = wander;
        desired = normalized(desired + normalized(tangent) * (2 * (1 - distance / margin)));
      }
    }
    const D3 aim = normalized(direction * .4 + desired * .6);
    D3 next = previous + aim * step;
    bool found = next.y >= 0 && clear(p, previous, next, radius);
    // A finite, documented search, followed by a refusal if no local candidate clears.
    // No root moves, no branch disappears, and no unchecked segment crosses a solid.
    for (u32 trial = 0; !found && trial < 16; ++trial) {
      const f64 azimuth = turn + trial * k_golden_angle;
      const D3 alternative = normalized(D3{det::cos(azimuth), .25, det::sin(azimuth)});
      next = previous + alternative * step;
      found = next.y >= 0 && clear(p, previous, next, radius);
    }
    if (!found) {
      error = "regeneration.obstacles: no clear shoot step; move the plant or revise obstacles";
      return false;
    }
    direction = normalized(next - previous);
    axis.point.push_back(next);
  }
  axis.arc.resize(k_rows + 1);
  axis.tangent.resize(k_rows + 1);
  axis.normal.resize(k_rows + 1);
  axis.arc[0] = 0;
  for (u32 row = 0; row <= k_rows; ++row) {
    axis.tangent[row] =
        normalized(axis.point[std::min(row + 1, k_rows)] - axis.point[row == 0 ? 0 : row - 1]);
    D3 hint = row == 0 ? D3{1, 0, 0} : axis.normal[row - 1];
    if (std::abs(dot(hint, axis.tangent[row])) > .95) hint = D3{0, 0, 1};
    axis.normal[row] = perpendicular(axis.tangent[row], hint);
    if (row != 0)
      axis.arc[row] = axis.arc[row - 1] + length_of(axis.point[row] - axis.point[row - 1]);
  }
  axis.length = axis.arc.back();
  axis.uniform_steps = false;
  return true;
}

struct Shoot {
  Axis axis;
  f64 radius = 0;
  u32 parent = 0;
  u32 nodes = 0;
  u64 stream = 0;
  bool basal = false;
  bool broken = false;
};

bool structure(const RegenerationProfile& profile, const Regeneration& p, Vector<Shoot>& shoots,
               std::string& error) {
  const f64 height = static_cast<f64>(p.height_m) * (1 - .55 * static_cast<f64>(p.suppression));
  for (u32 plant = 0; plant < p.plants; ++plant) {
    Rng rng = stream_of(p.seed, plant + 1);
    const f64 angle = rng.unit() * 2 * k_pi;
    const f64 scale = p.plants == 1 ? 1 : .75 + .25 * rng.unit();
    const f64 distance = p.plants == 1 ? 0
                                       : static_cast<f64>(p.group_radius_m) *
                                             std::sqrt((static_cast<f64>(plant) + .5) / p.plants);
    const D3 foot{distance * det::cos(angle), 0, distance * det::sin(angle)};
    for (u32 stem = 0; stem < profile.basal_stems; ++stem) {
      const u64 id = 1000 + plant * 10000 + stem * 1000;
      const f64 azimuth = angle + stem * k_golden_angle;
      const D3 out{det::cos(azimuth), 0, det::sin(azimuth)};
      const f64 h = height * scale * (1 - .12 * stem);
      const f64 radius =
          std::max(.001, static_cast<f64>(p.height_m * profile.radius_per_height) * scale);
      Shoot trunk;
      trunk.radius = radius;
      trunk.basal = true;
      trunk.broken = p.leader_loss > 0;
      trunk.stream = id;
      trunk.nodes = 3 + static_cast<u32>(h * 2);
      const D3 base = foot + out * (stem * radius * 1.5);
      const D3 heading = normalized(k_up + out * (profile.basal_stems > 1 ? .28 : .04));
      if (!grow(p, base, heading, h * static_cast<f64>(1 - p.leader_loss), radius, id, trunk.axis,
                error))
        return false;
      const u32 parent = shoots.size() + 1;
      shoots.push_back(std::move(trunk));
      // Established seedlings below 45 cm carry leaves directly; saplings add lateral cohorts.
      const u32 stations = p.height_m < .45f ? 0u : 2u + static_cast<u32>(p.height_m);
      for (u32 node = 0; node < stations; ++node) {
        const f64 fraction = .25 + .6 * (static_cast<f64>(node) + .5) / stations;
        // Never retain a branch whose attachment was lost with the leader.
        if (fraction >= static_cast<f64>(1 - p.leader_loss)) continue;
        const Axis& trunk_axis = shoots[parent - 1].axis;
        const f64 at = fraction * h;
        const D3 attachment = axis_point(trunk_axis, at);
        const f64 reach = h * static_cast<f64>(profile.lateral_reach) * (1 - .65 * fraction) *
                          (1 + .5 * static_cast<f64>(p.suppression));
        for (u32 branch = 0; branch < profile.branches_per_node; ++branch) {
          Shoot child;
          child.parent = parent;
          child.radius = radius * .32 * (1 - .5 * fraction);
          child.stream = id + 10 + node * 10 + branch;
          child.nodes = 3 + static_cast<u32>(reach * 8);
          Rng fate = stream_of(p.seed, child.stream);
          const f64 browse = fate.unit();
          const f64 turn = azimuth + node * (profile.opposite ? k_pi * .5 : k_golden_angle) +
                           branch * 2 * k_pi / profile.branches_per_node;
          const D3 dir =
              branch_direction(shoots[parent - 1].axis, at, turn,
                               static_cast<f64>(profile.lateral_angle_deg) * k_degree, nullptr);
          const D3 tip = attachment + dir * reach;
          child.broken = p.browse_fraction > 0 && tip.y <= static_cast<f64>(p.browse_height_m) &&
                         browse < static_cast<f64>(p.browse_fraction);
          if (!grow(p, attachment, dir, reach * (child.broken ? .45 : 1), child.radius,
                    child.stream, child.axis, error))
            return false;
          shoots.push_back(std::move(child));
        }
      }
      const bool recover = p.recovery > 0 &&
                           (p.leader_loss > 0 || (profile.basal_recovery && p.browse_fraction > 0));
      if (recover) {
        const u32 count = profile.opposite || profile.evergreen ? 2 : 1;
        for (u32 replacement = 0; replacement < count; ++replacement) {
          Shoot child;
          child.parent = parent;
          child.radius = radius * .35;
          child.stream = id + 900 + replacement;
          child.nodes = 4;
          const Axis& trunk_axis = shoots[parent - 1].axis;
          const f64 station = profile.basal_recovery ? .08 : .85;
          const D3 attachment = axis_point(trunk_axis, trunk_axis.length * station);
          const f64 turn = azimuth + replacement * k_pi;
          const D3 direction = normalized(D3{.8 * det::cos(turn), 1, .8 * det::sin(turn)});
          const f64 extent =
              h * static_cast<f64>(std::max(.25f, p.leader_loss)) * static_cast<f64>(p.recovery);
          if (!grow(p, attachment, direction, extent, child.radius, child.stream, child.axis,
                    error))
            return false;
          shoots.push_back(std::move(child));
        }
      }
    }
  }
  return true;
}

struct Blade {
  D3 base;
  D3 direction;
  D3 side;
  f64 length;
  f64 width;
  f64 station;
  u32 part;
};
Vector<Blade> foliage(const RegenerationProfile& profile, const Regeneration& p,
                      const Vector<Shoot>& shoots) {
  Vector<Blade> blades;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& shoot = shoots[i];
    Rng rng = stream_of(p.seed, shoot.stream + 5000000);
    const bool pine = profile.species == Species::pine;
    const u32 nodes =
        profile.evergreen
            ? std::max(pine ? 4u : 8u, static_cast<u32>(shoot.axis.length * (pine ? 25 : 200)))
            : shoot.nodes;
    for (u32 node = 0; node < nodes; ++node) {
      const f64 fraction = .2 + (shoot.broken ? .55 : .75) * (static_cast<f64>(node) + .5) / nodes;
      const D3 base = axis_point(shoot.axis, shoot.axis.length * fraction);
      const D3 tangent = axis_tangent(shoot.axis, shoot.axis.length * fraction);
      const f64 occupied = rng.unit();
      const f64 browsed = rng.unit();
      if (occupied < static_cast<f64>(p.suppression) * .35 ||
          (base.y <= static_cast<f64>(p.browse_height_m) &&
           browsed < static_cast<f64>(p.browse_fraction)))
        continue;
      const D3 normal = perpendicular(tangent, std::abs(tangent.y) < .9 ? k_up : D3{1, 0, 0});
      const u32 count = pine ? 5 : profile.opposite ? 2 : 1;
      for (u32 leaf = 0; leaf < count; ++leaf) {
        const f64 turn = node * (profile.opposite ? k_pi * .5 : k_golden_angle) +
                         (pine ? .12 * (static_cast<f64>(leaf) - 2) : leaf * k_pi);
        const D3 radial = rotate(normal, tangent, turn);
        const f64 size = .9 + .1 * rng.unit();
        const f64 length = static_cast<f64>(profile.leaf_length_m);
        Blade blade{base,
                    normalized(tangent * .4 + radial),
                    normalized(cross(radial, tangent)),
                    length * size,
                    length * static_cast<f64>(profile.leaf_width_m) /
                        static_cast<f64>(profile.leaf_length_m) * size,
                    fraction,
                    i + 1};
        // The capsule encloses both diamond triangles, including the downward tip droop.
        const f64 bound = blade.width * .5 + blade.length * .04;
        if (clear(p, base, base + blade.direction * blade.length, bound)) blades.push_back(blade);
      }
    }
  }
  return blades;
}
}  // namespace

std::span<const RegenerationProfile> regeneration_profiles() noexcept { return k_profiles; }
const RegenerationProfile* regeneration_profile(std::string_view species) noexcept {
  for (const auto& p : k_profiles)
    if (geometry::flora_species_info(p.species).name == species) return &p;
  return nullptr;
}

bool regeneration_in_range(std::string_view species, const Regeneration& p, std::string& error) {
  const auto bad = [&](const char* field) {
    error = std::string("engine.flora.Regeneration.") + field + " is out of range";
    return false;
  };
  if (regeneration_profile(species) == nullptr) {
    error = "regeneration unavailable for " + std::string(species) +
            "; available: maple oak birch pine spruce beaked_hazel red_osier_dogwood";
    return false;
  }
  if (!(p.height_m >= .15f && p.height_m <= 4)) return bad("height_m");
  if (p.plants < 1 || p.plants > 64) return bad("plants");
  if (!(p.group_radius_m >= 0 && p.group_radius_m <= 8) || (p.plants > 1 && p.group_radius_m == 0))
    return bad("group_radius_m");
  if (!(p.suppression >= 0 && p.suppression <= 1)) return bad("suppression");
  if (!(p.leader_loss >= 0 && p.leader_loss <= .8f)) return bad("leader_loss");
  if (!(p.recovery >= 0 && p.recovery <= 1) ||
      (p.recovery > 0 && p.leader_loss == 0 &&
       (p.browse_fraction == 0 || !regeneration_profile(species)->basal_recovery)))
    return bad("recovery");
  if (!(p.browse_height_m >= 0 && p.browse_height_m <= 4)) return bad("browse_height_m");
  if (!(p.browse_fraction >= 0 && p.browse_fraction <= 1) ||
      (p.browse_fraction > 0 && p.browse_height_m == 0))
    return bad("browse_fraction");
  if (!(p.triangle_budget == 0 || (p.triangle_budget >= 256 && p.triangle_budget <= 200000)))
    return bad("triangle_budget");
  if (!(p.impostor_px == 0 || p.impostor_px == 128 || p.impostor_px == 256 || p.impostor_px == 512))
    return bad("impostor_px");
  if (p.obstacles.size() > 64) return bad("obstacles");
  for (const auto& o : p.obstacles)
    if (!(o.x_m >= -16 && o.x_m <= 16 && o.y_m >= -16 && o.y_m <= 16 && o.z_m >= -16 &&
          o.z_m <= 16 && o.radius_m >= .01f && o.radius_m <= 8))
      return bad("obstacles");
  return true;
}

bool regeneration_collides(std::string_view species, const Regeneration& p) noexcept {
  const auto* profile = regeneration_profile(species);
  if (profile == nullptr || p.plants > 64) return false;
  for (u32 plant = 0; plant < p.plants; ++plant) {
    Rng rng = stream_of(p.seed, plant + 1);
    (void)rng.unit();  // The azimuth draw preceding scale in structure().
    const f64 scale = p.plants == 1 ? 1 : .75 + .25 * rng.unit();
    if (static_cast<f64>(p.height_m * profile->radius_per_height) * scale >=
        static_cast<f64>(k_shrub_collider_radius_m))
      return true;
  }
  return false;
}

bool solve_regeneration(std::string_view species, const Regeneration& p, assets::MeshData& mesh,
                        Motion& motion, TreeReport* report, std::string& error,
                        TreeLevels* levels) {
  mesh = {};
  motion = {};
  if (report) *report = {};
  if (levels) *levels = {};
  if (!regeneration_in_range(species, p, error)) return false;
  const auto& profile = *regeneration_profile(species);
  Vector<Shoot> shoots;
  if (!structure(profile, p, shoots, error)) return false;
  const auto blades = foliage(profile, p, shoots);
  const u32 budget = p.triangle_budget == 0 ? profile.triangle_budget : p.triangle_budget;
  const u32 far = p.impostor_px == 0 || blades.empty() ? 0 : k_far_triangles;
  u32 sides = 8;
  while (sides > 4 &&
         shoots.size() * tube_triangles(k_rows, sides) + blades.size() * 2 + far > budget)
    sides -= 2;
  const u32 needed = shoots.size() * tube_triangles(k_rows, sides) + blades.size() * 2 + far;
  if (needed > budget) {
    error = "engine.flora.Regeneration.triangle_budget cannot hold the shoots and leaves; needs " +
            std::to_string(needed);
    return false;
  }
  const Vec4 bark =
      species == "red_osier_dogwood" ? Vec4{.37f, .065f, .045f, 1} : Vec4{.29f, .20f, .12f, 1};
  mesh.materials.push_back(material("bark", bark, .85f, false));
  mesh.materials.push_back(material("leaf", {.12f, .28f, .06f, 1}, .65f, true));
  Emitter emit{mesh, motion};
  emit.wood = {};  // Young smooth stems; clearance uses the exact tube envelope.
  begin_structural_part(emit, {}, k_up, .1, geometry::k_motion_no_parent, 0, 0, 0);
  u32 colliders = 0;
  for (const Shoot& s : shoots) {
    const bool collider = s.basal && s.radius >= static_cast<f64>(k_shrub_collider_radius_m);
    colliders += collider ? 1u : 0u;
    const f64 tip = s.radius * (s.broken ? .55 : .15);
    begin_structural_part(emit, s.axis.point[0], s.axis.tangent[0], s.axis.length, s.parent,
                          s.radius, tip, collider ? k_collider : 0u);
    build_tube(s.axis, s.radius, tip, sides, {}, emit, s.broken);
  }
  const u32 leaf_first = mesh.indices.size();
  mesh.primitives.push_back({0, leaf_first, 0});
  for (const Blade& b : blades) {
    emit.part = b.part;
    emit.weight = b.station;
    build_leaf_card(b.base, b.direction, b.side, b.length, b.width, .04, emit);
  }
  const u32 leaf_end = mesh.indices.size();
  if (leaf_end > leaf_first) mesh.primitives.push_back({leaf_first, leaf_end - leaf_first, 1});
  if (report) {
    report->triangles = leaf_end / 3;
    report->wood_triangles = leaf_first / 3;
    report->leaf_triangles = (leaf_end - leaf_first) / 3;
    report->leaf_cards = blades.size();
    report->vertices = mesh.positions.size();
    report->parts = motion.parts.size();
    report->shoots = shoots.size();
    report->stems = p.plants * profile.basal_stems;
    report->colliders = colliders;
    report->trunk_sides = report->shoot_sides = sides;
    report->steps = (8 - sides) / 2;
    report->card_length = profile.leaf_length_m;
  }
  if (levels && far != 0) {
    levels->leaf_first_index = leaf_first;
    levels->leaf_index_end = leaf_end;
    ImpostorSpec spec;
    spec.card_px = 0;
    spec.tree_px = p.impostor_px;
    spec.coverage_dither = true;
    spec.coverage_floor_m = static_cast<f32>(2 * p.height_m * profile.radius_per_height *
                                             geometry::k_default_foliage_px);
    tree_star(spec, 0, emit, *levels);
  }
  return true;
}
}  // namespace engine::flora
