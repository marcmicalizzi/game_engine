#include "kit.h"

#include <domain/flora/alpine.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::flora {
namespace {
using namespace detail;
using Species = geometry::FloraSpecies;
// Every botanical number and sampling choice is recorded in alpine-growth-forms.md.
constexpr AlpineProfile k_profiles[] = {
    {Species::silene_acaulis,
     AlpineForm::cushion,
     .08f,
     .03f,
     .009f,
     .0012f,
     1536,
     {.075f, .18f, .035f, 1}},
    {Species::saxifraga_paniculata,
     AlpineForm::rosette,
     .065f,
     .012f,
     .025f,
     .008f,
     7,
     {.22f, .32f, .19f, 1}},
    {Species::salix_herbacea,
     AlpineForm::dwarf_scrub,
     .16f,
     .015f,
     .016f,
     .014f,
     96,
     {.12f, .27f, .045f, 1}},
};

struct Shoot {
  D3 root;
  D3 tip;
  D3 runner_base;
  f64 turn;
  f64 vigour;
  u32 pocket;
};

f64 plane(const Alpine& p, f64 x, f64 z) {
  return x * static_cast<f64>(p.slope_x) + z * static_cast<f64>(p.slope_z);
}

Vector<Shoot> grow(const AlpineProfile& profile, const Alpine& p) {
  const u32 count = p.shoots == 0 ? profile.shoots : p.shoots;
  const u32 pockets = std::max(1u, p.pockets.size());
  Vector<Shoot> out;
  for (u32 i = 0; i < count; ++i) {
    const u32 pocket = i % pockets;
    const u32 ordinal = i / pockets;
    const u32 local_count = (count - 1 - pocket) / pockets + 1;
    Rng rng = stream_of(p.seed, i + 1);
    const f64 occupied = rng.unit();
    const f64 jitter = rng.symmetric();
    const f64 turn = rng.unit() * 2 * k_pi;
    const f64 vigour = .9 + .1 * rng.unit();
    if (ordinal != 0 && occupied >= static_cast<f64>(p.occupancy)) continue;
    const f64 radius = p.pockets.empty()
                           ? static_cast<f64>(p.radius_m == 0 ? profile.radius_m : p.radius_m)
                           : static_cast<f64>(p.pockets[pocket].radius_m);
    const f64 cx = p.pockets.empty() ? 0.0 : static_cast<f64>(p.pockets[pocket].x);
    const f64 cz = p.pockets.empty() ? 0.0 : static_cast<f64>(p.pockets[pocket].z);
    const f64 cy = p.pockets.empty() ? 0.0 : static_cast<f64>(p.pockets[pocket].y);
    const f64 fraction = ordinal == 0 ? 0.0 : std::sqrt(static_cast<f64>(ordinal) / local_count);
    // A centred shoot always survives, so an explicitly authored pocket is never silently empty.
    const f64 angle = ordinal * k_golden_angle + .15 * jitter;
    const f64 x = cx + radius * fraction * det::cos(angle);
    const f64 z = cz + radius * fraction * det::sin(angle);
    const D3 root{x, cy + plane(p, x, z), z};
    f64 height = static_cast<f64>(profile.height_m) * vigour;
    if (profile.form == AlpineForm::cushion) height *= std::sqrt(1.0 - fraction * fraction);
    if (profile.form == AlpineForm::rosette) height = 0;
    D3 runner_base{cx, cy + plane(p, cx, cz), cz};
    if (profile.form == AlpineForm::dwarf_scrub) {
      f64 best = dot(root - runner_base, root - runner_base);
      for (const auto& earlier : out) {
        if (earlier.pocket != pocket) continue;
        const D3 delta = root - earlier.root;
        const f64 distance = dot(delta, delta);
        if (distance < best) {
          best = distance;
          runner_base = earlier.root;
        }
      }
    }
    out.push_back({root, root + k_up * height, runner_base, turn, vigour, pocket});
  }
  return out;
}

// Blade topology preserves the shoulder at t=.5 on both rungs. The final tip is one vertex,
// not two coincident corners. Width is a complete physical blade, never a patch-sized card.
void blade(Emitter& emit, AlpineForm form, D3 base, D3 heading, f64 length, f64 width, f64 lift,
           u32 rows, const Alpine& p) {
  // Rotate into the planting plane, rather than shearing blades longer on steep slopes.
  const D3 up = normalized(D3{-static_cast<f64>(p.slope_x), 1, -static_cast<f64>(p.slope_z)});
  if (p.slope_x != 0 || p.slope_z != 0)
    heading = normalized(D3{heading.x, plane(p, heading.x, heading.z), heading.z});
  const D3 side = cross(heading, up);
  const u32 first = emit.mesh.positions.size();
  for (u32 r = 0; r < rows; ++r) {
    const f64 t = static_cast<f64>(r) / rows;
    f64 shape = 1;
    if (form == AlpineForm::cushion) shape = .4 + .6 * (1.0 - t);
    if (form == AlpineForm::rosette) shape = .15 + .85 * det::sin(k_pi * t * .8);
    if (form == AlpineForm::dwarf_scrub) shape = .12 + .88 * det::sin(k_pi * t);
    const f64 along = form == AlpineForm::rosette       ? det::sin(k_pi * .5 * t)
                      : form == AlpineForm::dwarf_scrub ? .5 * (1 - det::cos(k_pi * t))
                                                        : t;
    D3 centre = base + heading * (length * along);
    centre = centre + up * (length * lift * along * along);
    for (u32 edge = 0; edge < 2; ++edge) {
      const D3 offset = side * (width * shape * (edge == 0 ? -.5 : .5));
      D3 point = centre + offset;
      point = point + up * (width * .06 * shape);
      emit.vertex(point, up, edge, t);
    }
  }
  D3 tip = base + heading * length;
  tip = tip + up * (length * lift);
  const u32 end = emit.vertex(tip, up, .5, 1);
  for (u32 r = 0; r + 1 < rows; ++r) {
    const u32 a = first + 2 * r;
    emit.face(a, a + 1, a + 2, up);
    emit.face(a + 1, a + 3, a + 2, up);
  }
  const u32 last = first + 2 * (rows - 1);
  emit.face(last, last + 1, end, up);
}

void far_form(const Alpine& p, Emitter& emit, TreeLevels& chain) {
  auto& mesh = emit.mesh;
  D3 lo = from_f32(mesh.positions[0]), hi = lo;
  for (Vec3 v : mesh.positions) {
    lo = {std::min(lo.x, static_cast<f64>(v.x)), std::min(lo.y, static_cast<f64>(v.y)),
          std::min(lo.z, static_cast<f64>(v.z))};
    hi = {std::max(hi.x, static_cast<f64>(v.x)), std::max(hi.y, static_cast<f64>(v.y)),
          std::max(hi.z, static_cast<f64>(v.z))};
  }
  const D3 centre = (lo + hi) * .5;
  const D3 extent = (hi - lo) * .5;
  TreeLevel level;
  level.kind = ImpostorKind::tree;
  level.tile_px = p.impostor_px;
  level.columns = level.rows = 2;
  level.groups = 3;
  level.elements = 1;
  level.parallax_m = static_cast<f32>(std::max({extent.x, extent.y, extent.z}));
  level.texel_m = level.parallax_m / static_cast<f32>(p.impostor_px - 2);
  level.error = std::max(level.parallax_m, level.texel_m);
  level.feature_m = level.parallax_m * 2;
  // A whole patch reaches eight pixels before a multi-plane proxy can replace it. In particular,
  // sparse pockets retain their separated plants at walking distance.
  level.coverage_floor_m = level.feature_m * geometry::k_default_foliage_px / 8;
  level.coverage_dither = true;
  emit.part = 0;
  emit.weight = 0;
  const u32 first = mesh.indices.size();
  for (u32 axis = 0; axis < 3; ++axis) {
    BakeJob job;
    job.tile = job.card_tile = axis;
    job.ranges.push_back({0, chain.leaf_index_end});
    job.card.origin = to_f32(centre);
    if (axis == 0) {
      job.card.u_axis = {1, 0, 0};
      job.card.v_axis = {0, 1, 0};
      job.card.normal = {0, 0, 1};
      job.card.u_min = static_cast<f32>(-extent.x);
      job.card.u_max = static_cast<f32>(extent.x);
      job.card.v_min = static_cast<f32>(-extent.y);
      job.card.v_max = static_cast<f32>(extent.y);
    } else if (axis == 1) {
      job.card.u_axis = {0, 0, 1};
      job.card.v_axis = {0, 1, 0};
      job.card.normal = {-1, 0, 0};
      job.card.u_min = static_cast<f32>(-extent.z);
      job.card.u_max = static_cast<f32>(extent.z);
      job.card.v_min = static_cast<f32>(-extent.y);
      job.card.v_max = static_cast<f32>(extent.y);
    } else {
      job.card.u_axis = {1, 0, 0};
      job.card.v_axis = {0, 0, -1};
      job.card.normal = {0, 1, 0};
      job.card.u_min = static_cast<f32>(-extent.x);
      job.card.u_max = static_cast<f32>(extent.x);
      job.card.v_min = static_cast<f32>(-extent.z);
      job.card.v_max = static_cast<f32>(extent.z);
    }
    job.view = job.card;
    const f32 um = (job.card.u_max - job.card.u_min) / static_cast<f32>(p.impostor_px - 2);
    const f32 vm = (job.card.v_max - job.card.v_min) / static_cast<f32>(p.impostor_px - 2);
    job.view.u_min -= um;
    job.view.u_max += um;
    job.view.v_min -= vm;
    job.view.v_max += vm;
    const u32 start = mesh.positions.size();
    for (u32 v = 0; v < 2; ++v) {
      for (u32 u = 0; u < 2; ++u) {
        const D3 point =
            centre +
            from_f32(job.card.u_axis) * static_cast<f64>(u ? job.card.u_max : job.card.u_min) +
            from_f32(job.card.v_axis) * static_cast<f64>(v ? job.card.v_max : job.card.v_min);
        const f64 tu = static_cast<f64>((axis % 2) * p.impostor_px + 1 + u * (p.impostor_px - 2)) /
                       (2 * p.impostor_px);
        const f64 tv = static_cast<f64>((axis / 2) * p.impostor_px + 1 + v * (p.impostor_px - 2)) /
                       (2 * p.impostor_px);
        emit.vertex(point, from_f32(job.card.normal), tu, tv);
      }
    }
    emit.face(start, start + 1, start + 2, from_f32(job.card.normal));
    emit.face(start + 1, start + 3, start + 2, from_f32(job.card.normal));
    level.jobs.push_back(std::move(job));
  }
  for (u32 i = first; i < mesh.indices.size(); ++i)
    level.indices.push_back(mesh.indices[i]);
  level.element_of_triangle.resize(6, 0);
  chain.levels.push_back(std::move(level));
}
}  // namespace

std::span<const AlpineProfile> alpine_profiles() noexcept { return k_profiles; }
const AlpineProfile* alpine_profile(std::string_view species) noexcept {
  for (const auto& p : k_profiles)
    if (geometry::flora_species_info(p.species).name == species) return &p;
  return nullptr;
}

bool alpine_in_range(std::string_view species, const Alpine& p, std::string& error) {
  const auto bad = [&](const char* field) {
    error = std::string("engine.flora.Alpine.") + field + " is out of range";
    return false;
  };
  const AlpineProfile* profile = alpine_profile(species);
  if (!profile) return bad("species");
  if (!(p.radius_m == 0 || (p.radius_m >= .04f && p.radius_m <= .2f))) return bad("radius_m");
  if (p.shoots > 2048) return bad("shoots");
  if (!(p.occupancy >= .05f && p.occupancy <= 1)) return bad("occupancy");
  if (!(p.slope_x >= -1 && p.slope_x <= 1)) return bad("slope_x");
  if (!(p.slope_z >= -1 && p.slope_z <= 1)) return bad("slope_z");
  if (p.pockets.size() > 16 || (p.shoots == 0 ? profile->shoots : p.shoots) < p.pockets.size())
    return bad("pockets/shoots");
  for (const auto& pocket : p.pockets) {
    if (!(pocket.x >= -8 && pocket.x <= 8 && pocket.y >= -8 && pocket.y <= 8 && pocket.z >= -8 &&
          pocket.z <= 8))
      return bad("pockets.position");
    if (!(pocket.radius_m >= .005f && pocket.radius_m <= .2f)) return bad("pockets.radius_m");
  }
  if (!(p.triangle_budget == 0 || (p.triangle_budget >= 64 && p.triangle_budget <= 200000)))
    return bad("triangle_budget");
  if (p.impostor_px != 0 && p.impostor_px != 64 && p.impostor_px != 128 && p.impostor_px != 256)
    return bad("impostor_px");
  if (p.detail_tier != DetailTier::essential && p.detail_tier != DetailTier::standard &&
      p.detail_tier != DetailTier::rich)
    return bad("detail_tier");
  return true;
}

bool solve_alpine(std::string_view species, const Alpine& p, assets::MeshData& mesh, Motion& motion,
                  TreeReport* report, std::string& error, TreeLevels* levels) {
  mesh = {};
  motion = {};
  if (report) *report = {};
  if (levels) *levels = {};
  if (!alpine_in_range(species, p, error)) return false;
  const auto& profile = *alpine_profile(species);
  const auto shoots = grow(profile, p);
  const u32 leaves = profile.form == AlpineForm::cushion   ? 6u
                     : profile.form == AlpineForm::rosette ? 18u
                                                           : 3u;
  const bool wood = profile.form == AlpineForm::dwarf_scrub;
  const u32 budget = p.triangle_budget == 0
                         ? geometry::flora_species_info(profile.species).triangle_budget
                         : p.triangle_budget;
  const u32 far = p.impostor_px == 0 ? 0u : 6u;
  // Wood is one 2-row shoot and one 1-row prostrate attachment, each capped at the tip.
  const auto count = [&](u32 rows, u32 sides) {
    return shoots.size() * (leaves * (2 * rows - 1) + (wood ? 8 * sides : 0u)) + far;
  };
  u32 rows = 4, sides = 5;
  if (count(rows, sides) > budget) {
    rows = 2;
    sides = 3;
  }
  if (count(rows, sides) > budget) {
    error = "engine.flora.Alpine.triangle_budget needs " + std::to_string(count(rows, sides));
    return false;
  }
  mesh.materials.push_back(material("bark", {.22f, .12f, .065f, 1}, .85f, false));
  mesh.materials.push_back(material("leaf", profile.leaf, .7f, true));
  Emitter emit{mesh, motion};
  begin_structural_part(emit, {}, k_up, .1, geometry::k_motion_no_parent, 0, 0, 0);
  for (const auto& s : shoots)
    begin_structural_part(emit, s.root, k_up, static_cast<f64>(profile.height_m), 0,
                          wood ? .0008 : 0, 0, 0);
  if (wood) {
    for (u32 i = 0; i < shoots.size(); ++i) {
      const auto& s = shoots[i];
      emit.part = i + 1;
      Rng rng = stream_of(p.seed, 10000 + i);
      const Axis stem = grow_axis(s.root, k_up, length_of(s.tip - s.root), 2, k_up, 0, 0, rng);
      build_tube(stem, .0008, .0003, sides, {}, emit);
      // Connect to the nearest earlier rooted shoot: an acyclic prostrate network in each pocket.
      const D3 delta = s.root - s.runner_base;
      if (length_of(delta) > 0) {
        const Axis runner =
            grow_axis(s.runner_base, normalized(delta), length_of(delta), 1, k_up, 0, 0, rng);
        build_tube(runner, .001, .0008, sides, {}, emit);
      }
    }
  }
  const u32 leaf_first = mesh.indices.size();
  const u32 leaf_vertex_first = mesh.positions.size();
  if (leaf_first) mesh.primitives.push_back({0, leaf_first, 0});
  for (u32 i = 0; i < shoots.size(); ++i) {
    const auto& s = shoots[i];
    emit.part = i + 1;
    emit.weight = 0;
    for (u32 j = 0; j < leaves; ++j) {
      f64 angle = s.turn + j * k_golden_angle;
      f64 lift = .15;
      f64 leaf_scale = 1;
      D3 base = s.tip;
      if (profile.form == AlpineForm::cushion) {
        angle = s.turn + (j / 2) * k_pi * .5 + (j % 2) * k_pi;
        base.y = std::max(s.root.y, base.y - (2 - j / 2) * .002);
        lift = .35;
      } else if (profile.form == AlpineForm::rosette) {
        lift = .1 + .8 * static_cast<f64>(j) / (leaves - 1);
        leaf_scale = 1 - .65 * static_cast<f64>(j) / (leaves - 1);
      } else {
        base = s.root + (s.tip - s.root) * (.4 + .3 * j);
        lift = .2;
      }
      blade(emit, profile.form, base, {det::cos(angle), 0, det::sin(angle)},
            static_cast<f64>(profile.leaf_length_m) * s.vigour * leaf_scale,
            static_cast<f64>(profile.leaf_width_m) * s.vigour * leaf_scale, lift, rows, p);
    }
  }
  mesh.primitives.push_back({leaf_first, mesh.indices.size() - leaf_first, 1});
  smooth_normals(mesh, leaf_vertex_first, leaf_first);
  TreeLevels chain;
  chain.leaf_first_index = leaf_first;
  chain.leaf_index_end = mesh.indices.size();
  if (levels && p.impostor_px != 0) {
    // A generic decimator deletes tiny independent blades long before the patch is subpixel.
    // One authored element retains their mass until the coverage-fitted whole patch takes over.
    chain.leaf_element.resize((chain.leaf_index_end - leaf_first) / 3, 0u);
    far_form(p, emit, chain);
  }
  if (report) {
    report->triangles = mesh.indices.size() / 3;
    report->wood_triangles = leaf_first / 3;
    report->leaf_triangles = (chain.leaf_index_end - leaf_first) / 3;
    report->vertices = mesh.positions.size();
    report->parts = motion.parts.size();
    report->shoots = shoots.size();
    report->leaf_cards = shoots.size() * leaves;
    report->card_length = profile.leaf_length_m;
    report->steps = rows == 4 ? 0u : 1u;
  }
  if (levels) *levels = std::move(chain);
  return true;
}
}  // namespace engine::flora
