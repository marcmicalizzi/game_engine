// G107: botanical envelopes and every authored approximation are in shrub-growth-forms.md.
#include "kit.h"

#include <domain/flora/shrubs.h>
#include <domain/flora/structure.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::flora {
namespace {
using namespace detail;
using Species = geometry::FloraSpecies;
constexpr ShrubProfile k_profiles[] = {
    {Species::beaked_hazel,
     ShrubForm::upright,
     4,
     1,
     6,
     .8f,
     .022f,
     .07f,
     .05f,
     .03f,
     .10f,
     .08f,
     12,
     1,
     7,
     3,
     8,
     .25f,
     {.31f, .23f, .16f, 1},
     {.12f, .28f, .06f, 1}},
    {Species::bearberry,
     ShrubForm::spreading,
     .18f,
     .05f,
     .30f,
     .65f,
     .003f,
     .02f,
     .01f,
     .01f,
     .025f,
     .05f,
     8,
     1,
     6,
     3,
     6,
     .06f,
     {.30f, .12f, .08f, 1},
     {.08f, .22f, .06f, 1}},
    {Species::blackberry,
     ShrubForm::arching,
     1.5f,
     .5f,
     3.4f,
     2,
     .006f,
     .06f,
     .04f,
     .04f,
     .08f,
     .22f,
     9,
     1,
     8,
     2,
     4,
     .18f,
     {.25f, .18f, .10f, 1},
     {.08f, .23f, .05f, 1}},
    {Species::red_osier_dogwood,
     ShrubForm::thicket,
     2.5f,
     1,
     6,
     1,
     .012f,
     .09f,
     .045f,
     .06f,
     .12f,
     .10f,
     12,
     3,
     8,
     3,
     8,
     .24f,
     {.37f, .065f, .045f, 1},
     {.10f, .26f, .055f, 1}},
    {Species::big_sagebrush,
     ShrubForm::sparse,
     1.2f,
     .4f,
     4.5f,
     .65f,
     .018f,
     .025f,
     .006f,
     .01f,
     .04f,
     .08f,
     7,
     1,
     5,
     3,
     8,
     .09f,
     {.29f, .26f, .21f, 1},
     {.32f, .37f, .24f, 1}},
};
constexpr u32 k_axis_rows = 16;
constexpr u32 k_far_triangles = 2 * 4 * ImpostorSpec::k_tree_slices;

f64 height_of(const ShrubProfile& profile, const ShrubGrowth& p) {
  return static_cast<f64>(p.height_m == 0 ? profile.height_m : p.height_m);
}
f64 radius_of(const ShrubProfile& profile, const ShrubGrowth& p) {
  // Retained elastic-similarity exponent; a profile reference radius is an authored calibration.
  return static_cast<f64>(profile.stem_radius_m) *
         det::exp(1.5 * det::log(height_of(profile, p) / static_cast<f64>(profile.height_m)));
}
u32 stems_of(const ShrubProfile& profile, const ShrubGrowth& p) {
  return p.stems == 0 ? profile.stems : p.stems;
}
u32 colonies_of(const ShrubProfile& profile, const ShrubGrowth& p) {
  return p.colonies == 0 ? profile.colonies : p.colonies;
}
f64 plane(const ShrubGrowth& p, D3 v) {
  return static_cast<f64>(p.slope_x) * v.x + static_cast<f64>(p.slope_z) * v.z;
}

// Recompute arc lengths and transported frames after fitting a curve to its planting plane.
// No terrain read or work in the runtime frame loop.
void frame_axis(Axis& a) {
  const u32 count = a.point.size();
  a.tangent.resize(count);
  a.normal.resize(count);
  a.arc.resize(count);
  a.arc[0] = 0;
  for (u32 i = 0; i < count; ++i) {
    a.tangent[i] = normalized(a.point[std::min(i + 1, count - 1)] - a.point[i == 0 ? 0 : i - 1]);
    D3 hint = i == 0 ? D3{1, 0, 0} : a.normal[i - 1];
    if (std::abs(dot(hint, a.tangent[i])) > .95) hint = D3{0, 0, 1};
    a.normal[i] = perpendicular(a.tangent[i], hint);
    if (i != 0) a.arc[i] = a.arc[i - 1] + length_of(a.point[i] - a.point[i - 1]);
  }
  a.length = a.arc.back();
  a.uniform_steps = false;
}

struct Shoot {
  Axis axis;
  f64 radius = 0;
  u32 parent = 0;  // motion part, shoot index + 1; zero is the geometry-less root
  u32 stem = 0;
  u32 order = 0;
  bool dead = false;
};

Axis stem_axis(const ShrubProfile& profile, const ShrubGrowth& p, D3 foot, D3 heading, f64 size,
               Rng& rng) {
  Axis axis;
  const f64 height = height_of(profile, p) * size;
  const f64 reach = static_cast<f64>(profile.reach_m * p.spread) * size * height_of(profile, p) /
                    static_cast<f64>(profile.height_m);
  const f64 bend = rng.symmetric() * .15;
  const D3 side = cross(k_up, heading);
  for (u32 j = 0; j <= k_axis_rows; ++j) {
    const f64 t = static_cast<f64>(j) / k_axis_rows;
    f64 y = height * t;
    f64 x = reach * t * t;
    if (profile.form == ShrubForm::arching) {
      x = reach * t;
      y = height * (4 * t * (1 - t) + .1 * t) / 1.050625;
    } else if (profile.form == ShrubForm::spreading) {
      x = reach * t;
      y = height * (.12 * t + .28 * det::sin(k_pi * t));
    } else if (profile.form == ShrubForm::thicket) {
      x = reach * (1.5 * t - .5 * t * t);
      y = height * (1.8 * t - .8 * t * t);
    } else if (profile.form == ShrubForm::sparse) {
      x = reach * (.6 * t + .4 * t * t);
    }
    D3 v = foot + heading * x + side * (reach * bend * det::sin(k_pi * t));
    v.y = plane(p, v) + y;
    axis.point.push_back(v);
  }
  frame_axis(axis);
  return axis;
}

Vector<Shoot> grow(const ShrubProfile& profile, const ShrubGrowth& p) {
  Vector<Shoot> shoots;
  const u32 stems = stems_of(profile, p);
  const u32 colonies = colonies_of(profile, p);
  const u32 total = stems * colonies;
  const f64 dead_fraction =
      static_cast<f64>(p.dead_fraction < 0 ? profile.dead_fraction : p.dead_fraction);
  const u32 dead_count = static_cast<u32>(static_cast<f64>(total) * dead_fraction + .5);
  // Exact count, seeded ranks. Fate draws never perturb a surviving stem's growth stream.
  Vector<std::pair<u64, u32>> ranks;
  for (u32 i = 0; i < total; ++i) {
    Rng rng = stream_of(p.seed, 1000000 + i);
    ranks.push_back({rng.next(), i});
  }
  std::sort(ranks.begin(), ranks.end());
  Vector<u8> dead(total, 0);
  for (u32 i = 0; i < dead_count; ++i)
    dead[ranks[i].second] = 1;
  const f64 radius = radius_of(profile, p);
  const f64 colony_radius = p.colony_radius_m > 0 ? static_cast<f64>(p.colony_radius_m)
                                                  : static_cast<f64>(profile.reach_m) * .75;
  Rng turn_rng = stream_of(p.seed, 1);
  const f64 turn = turn_rng.unit() * 2 * k_pi;
  for (u32 s = 0; s < total; ++s) {
    Rng rng = stream_of(p.seed, 10 + s);
    const u32 colony = s / stems;
    const u32 local = s % stems;
    const f64 ca = turn + colony * k_golden_angle;
    const f64 cr =
        colonies > 1 ? colony_radius * std::sqrt(static_cast<f64>(colony) / (colonies - 1)) : 0;
    const f64 azimuth = turn + local * k_golden_angle + rng.symmetric() * .25 + colony;
    const D3 heading{det::cos(azimuth), 0, det::sin(azimuth)};
    const f64 stool = profile.form == ShrubForm::spreading ? .08 : .18;
    const D3 foot = D3{cr * det::cos(ca), 0, cr * det::sin(ca)} +
                    heading * (stool * std::sqrt(static_cast<f64>(local) / stems));
    const f64 size = local == 0 ? 1 : .65 + .35 * rng.unit();
    const u32 stem_index = shoots.size();
    Shoot stem;
    stem.axis = stem_axis(profile, p, foot, heading, size, rng);
    stem.radius = radius * size;
    stem.stem = s;
    stem.dead = dead[s] != 0;
    shoots.push_back(std::move(stem));
    // Two sampled woody orders retain the silhouette at a bounded source cost. Their counts
    // are explicit profile choices, not claims of measured bud production.
    for (u32 b = 0; b < profile.branches; ++b) {
      const Axis& parent = shoots[stem_index].axis;
      const bool opposite = profile.form == ShrubForm::thicket;
      const f64 node = opposite ? static_cast<f64>(b / 2) * 2 : b;
      const f64 station = (.22 + .70 * (node + .5) / profile.branches) * parent.length;
      const D3 base = axis_point(parent, station);
      const f64 angle =
          azimuth + (opposite ? (b / 2) * k_pi * .5 + (b % 2) * k_pi : b * k_golden_angle);
      const f64 rise = profile.form == ShrubForm::spreading ? .12 : .4;
      const D3 dir = normalized(D3{det::cos(angle), rise, det::sin(angle)});
      const f64 lateral =
          static_cast<f64>(profile.reach_m * p.spread) * (.35 + .25 * rng.unit()) * size;
      Shoot branch;
      branch.axis = grow_axis(base, dir, lateral, k_axis_rows, k_up, .25 / lateral, .025, rng);
      branch.radius = radius * size * .32;
      branch.parent = stem_index + 1;
      branch.stem = s;
      branch.order = 1;
      branch.dead = dead[s] != 0;
      for (D3& v : branch.axis.point)
        v.y = std::max(v.y, plane(p, v) + branch.radius);
      frame_axis(branch.axis);
      const u32 branch_index = shoots.size();
      shoots.push_back(std::move(branch));
      for (u32 t = 0; t < profile.twigs; ++t) {
        const Axis& limb = shoots[branch_index].axis;
        const f64 at = (.35 + .6 * (static_cast<f64>(t) + .5) / profile.twigs) * limb.length;
        const f64 ta = angle + (t % 2 == 0 ? 1 : -1) * .9;
        const D3 td = normalized(D3{det::cos(ta), rise + .15, det::sin(ta)});
        Shoot twig;
        twig.axis =
            grow_axis(axis_point(limb, at), td, static_cast<f64>(profile.twig_length_m),
                      k_axis_rows, k_up, .2 / static_cast<f64>(profile.twig_length_m), .025, rng);
        twig.radius = std::min(branch.radius * .35, static_cast<f64>(profile.twig_length_m) * .006);
        twig.parent = branch_index + 1;
        twig.stem = s;
        twig.order = 2;
        twig.dead = dead[s] != 0;
        for (D3& v : twig.axis.point)
          v.y = std::max(v.y, plane(p, v) + twig.radius);
        frame_axis(twig.axis);
        shoots.push_back(std::move(twig));
      }
    }
  }
  return shoots;
}

u32 leaflets(const ShrubProfile& profile, u32 stem) {
  return profile.form == ShrubForm::arching   ? (stem % 2 == 0 ? 5u : 3u)
         : profile.form == ShrubForm::thicket ? 2u
                                              : 1u;
}
bool occupied(const ShrubProfile& profile, const ShrubGrowth& p, u32 shoot, u32 node) {
  Rng rng = stream_of(p.seed, 2000000 + static_cast<u64>(shoot) * 32 + node);
  const f64 density = profile.form == ShrubForm::sparse ? .45 : 1;
  return rng.unit() < static_cast<f64>(p.foliage) * density;
}
u32 count_leaves(const ShrubProfile& profile, const ShrubGrowth& p, const Vector<Shoot>& shoots) {
  u32 count = 0;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    if (s.order != 2 || s.dead) continue;
    for (u32 j = 0; j < profile.leaf_nodes; ++j)
      if (occupied(profile, p, i, j)) count += leaflets(profile, s.stem);
  }
  return count;
}
struct Detail {
  u32 rows[3];
  u32 sides[3];
};
constexpr Detail k_details[] = {{{16, 8, 3}, {6, 4, 3}},
                                {{12, 6, 2}, {5, 4, 3}},
                                {{8, 4, 2}, {4, 3, 3}},
                                {{4, 2, 1}, {3, 3, 3}}};
u32 count_wood(const Vector<Shoot>& shoots, const Detail& detail) {
  u32 triangles = 0;
  for (const Shoot& s : shoots)
    triangles += tube_triangles(detail.rows[s.order], detail.sides[s.order]) - (s.dead ? 2u : 0u);
  return triangles;
}
}  // namespace

std::span<const ShrubProfile> shrub_profiles() noexcept { return k_profiles; }
const ShrubProfile* shrub_profile(std::string_view species) noexcept {
  for (const auto& profile : k_profiles)
    if (geometry::flora_species_info(profile.species).name == species) return &profile;
  return nullptr;
}
bool shrub_growth_collides(std::string_view species, const ShrubGrowth& p) noexcept {
  const auto* profile = shrub_profile(species);
  return profile != nullptr &&
         radius_of(*profile, p) >= static_cast<f64>(k_shrub_collider_radius_m);
}
bool shrub_growth_in_range(std::string_view species, const ShrubGrowth& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.ShrubGrowth.") + field + " is out of range (" + range + ")";
    return false;
  };
  const auto* profile = shrub_profile(species);
  if (profile == nullptr) return bad("species", "registered shrub profile");
  if (!(p.height_m == 0 ||
        (p.height_m >= profile->height_min_m && p.height_m <= profile->height_max_m)))
    return bad("height_m", "0 or the profile's botanical height range");
  if (!(p.spread >= .5f && p.spread <= 2)) return bad("spread", "0.5..2");
  if (p.stems > 40) return bad("stems", "0..40");
  if (p.colonies > 16) return bad("colonies", "0..16");
  if (!(p.colony_radius_m == 0 || (p.colony_radius_m >= .05f && p.colony_radius_m <= 8)))
    return bad("colony_radius_m", "0 or 0.05..8 m");
  if (!(p.dead_fraction == -1 || (p.dead_fraction >= 0 && p.dead_fraction <= 1)))
    return bad("dead_fraction", "-1 or 0..1");
  if (!(p.foliage >= 0 && p.foliage <= 1)) return bad("foliage", "0..1");
  if (!(p.slope_x >= -1 && p.slope_x <= 1)) return bad("slope_x", "-1..1");
  if (!(p.slope_z >= -1 && p.slope_z <= 1)) return bad("slope_z", "-1..1");
  if (!(p.triangle_budget == 0 || (p.triangle_budget >= 4000 && p.triangle_budget <= 400000)))
    return bad("triangle_budget", "0 or 4000..400000");
  if (!(p.impostor_px == 0 || p.impostor_px == 128 || p.impostor_px == 256 ||
        p.impostor_px == 512 || p.impostor_px == 1024))
    return bad("impostor_px", "0,128,256,512,1024");
  if (p.detail_tier != DetailTier::essential && p.detail_tier != DetailTier::standard &&
      p.detail_tier != DetailTier::rich)
    return bad("detail_tier", "essential,standard,rich");
  if (p.detail_tier == DetailTier::rich && shrub_growth_collides(species, p))
    return bad("detail_tier", "a colliding shrub cannot be rich");
  return true;
}

bool solve_shrub_growth(std::string_view species, const ShrubGrowth& p, assets::MeshData& mesh,
                        Motion& motion, TreeReport* report, std::string& error,
                        TreeLevels* levels) {
  mesh = {};
  motion = {};
  if (report) *report = {};
  if (levels) *levels = {};
  if (!shrub_growth_in_range(species, p, error)) return false;
  const ShrubProfile& profile = *shrub_profile(species);
  const auto shoots = grow(profile, p);
  const u32 leaves = count_leaves(profile, p, shoots);
  const u32 budget = p.triangle_budget == 0
                         ? geometry::flora_species_info(profile.species).triangle_budget
                         : p.triangle_budget;
  const u32 far_triangles = p.impostor_px == 0 ? 0 : k_far_triangles;
  u32 rung = 0;
  while (rung < std::size(k_details) &&
         count_wood(shoots, k_details[rung]) + 2 * leaves + far_triangles > budget)
    ++rung;
  if (rung == std::size(k_details)) {
    error =
        "engine.flora.ShrubGrowth.triangle_budget cannot hold this structure and real-sized "
        "leaves; needs " +
        std::to_string(count_wood(shoots, k_details[rung - 1]) + 2 * leaves + far_triangles);
    return false;
  }
  const Detail& detail = k_details[rung];
  mesh.materials.push_back(material("bark", profile.bark, .85f, false));
  mesh.materials.push_back(material("leaf", profile.leaf, .65f, true));
  Emitter emit{mesh, motion};
  begin_structural_part(emit, {}, k_up, .1, geometry::k_motion_no_parent, 0, 0, 0);
  u32 colliders = 0;
  for (const Shoot& s : shoots) {
    const bool collider = s.order == 0 && s.radius >= static_cast<f64>(k_shrub_collider_radius_m);
    colliders += collider ? 1u : 0u;
    begin_structural_part(emit, s.axis.point[0], s.axis.tangent[0], s.axis.length, s.parent,
                          s.radius, s.radius * .18, collider ? k_collider : 0u);
    build_tube(resample_axis(s.axis, detail.rows[s.order]), s.radius, s.radius * .18,
               detail.sides[s.order], {}, emit, s.dead);
  }
  const u32 leaf_first = mesh.indices.size();
  mesh.primitives.push_back({0, leaf_first, 0});
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    if (s.dead || s.order != 2) continue;
    emit.part = i + 1;
    Rng rng = stream_of(p.seed, 3000000 + i);
    for (u32 j = 0; j < profile.leaf_nodes; ++j) {
      if (!occupied(profile, p, i, j)) continue;
      const f64 fraction = (.12 + .82 * (static_cast<f64>(j) + .5) / profile.leaf_nodes);
      const f64 at = fraction * s.axis.length;
      const D3 base = axis_point(s.axis, at);
      const D3 tangent = axis_tangent(s.axis, at);
      const D3 normal = perpendicular(tangent, std::abs(tangent.y) < .9 ? k_up : D3{1, 0, 0});
      const f64 turn = j * (profile.form == ShrubForm::thicket ? k_pi * .5 : k_golden_angle) +
                       rng.symmetric() * .12;
      const D3 side = rotate(normal, tangent, turn);
      const u32 count = leaflets(profile, s.stem);
      emit.weight = fraction;
      for (u32 k = 0; k < count; ++k) {
        const f64 fan = count > 2 ? (-1.1 + 2.2 * k / (count - 1)) : k * k_pi;
        const D3 direction = normalized(tangent * .45 + rotate(side, tangent, fan));
        const D3 across = normalized(cross(direction, tangent));
        // Botanical dimensions stay fixed with plant height and source budget.
        const f64 variation = .9 + .1 * rng.unit();
        build_leaf_card(base, direction, across,
                        static_cast<f64>(profile.leaf_length_m) * variation,
                        static_cast<f64>(profile.leaf_width_m) * variation, .04, emit);
      }
    }
  }
  const u32 leaf_indices = mesh.indices.size() - leaf_first;
  if (leaf_indices != 0) mesh.primitives.push_back({leaf_first, leaf_indices, 1});
  if (report) {
    report->triangles = mesh.indices.size() / 3;
    report->wood_triangles = leaf_first / 3;
    report->leaf_triangles = leaf_indices / 3;
    report->leaf_cards = leaves;
    report->vertices = mesh.positions.size();
    report->parts = motion.parts.size();
    report->shoots = shoots.size();
    report->orders = 3;
    report->steps = rung;
    report->colliders = colliders;
    report->card_length = profile.leaf_length_m;
    report->leaf_area_m2 = static_cast<f32>(triangles_area(mesh, leaf_first));
  }
  if (levels && p.impostor_px != 0 && leaf_indices != 0) {
    levels->leaf_first_index = leaf_first;
    levels->leaf_index_end = mesh.indices.size();
    ImpostorSpec spec;
    spec.card_px = 0;
    spec.tree_px = p.impostor_px;
    tree_star(spec, 0, emit, *levels);
  }
  return true;
}
}  // namespace engine::flora
