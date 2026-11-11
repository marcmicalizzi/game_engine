#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/flora/flora.h>
#include <domain/flora/meadow.h>
#include <domain/flora/sward.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <schemas/flora.h>

namespace engine::flora {
// ---- G140, the meadow community (meadow.h; flora.md, "The meadow community") -------------------
namespace {
// F and L: Hill et al. (1999), final values, read in the text. Cover: a Domin class's midpoint,
// chosen from the constancy the grounding page records (Domin 4, 4-10 %: 0.07; 5, 11-25 %: 0.18;
// 6, 26-33 %: 0.30). Height, heads, petals and colour: chosen, for the stand-in herb only.
constexpr MeadowSpeciesTraits k_meadow[] = {
    // The wet margin.
    {"caltha_palustris", MeadowRole::forb, 9, 7, .18, .30, 3, 5, .018, 60, {.80f, .55f, .02f}},
    {"filipendula_ulmaria", MeadowRole::forb, 8, 7, .18, 1.0, 24, 5, .004, 80, {.80f, .76f, .56f}},
    {"cardamine_pratensis", MeadowRole::forb, 8, 7, .07, .35, 8, 4, .008, 75, {.56f, .46f, .70f}},
    {"carex_nigra", MeadowRole::sedge, 8, 7, .18, .40, 0, 0, 0, 0, {0, 0, 0}},
    {"juncus_effusus", MeadowRole::rush, 7, 7, .30, .80, 0, 0, 0, 0, {0, 0, 0}},
    // The mesic sward.
    {"ranunculus_acris", MeadowRole::forb, 6, 7, .18, .60, 5, 5, .010, 75, {.85f, .65f, .02f}},
    {"geranium_sylvaticum", MeadowRole::forb, 5, 6, .30, .60, 6, 5, .014, 80, {.35f, .15f, .55f}},
    {"trifolium_pratense", MeadowRole::forb, 5, 7, .18, .35, 3, 30, .012, 35, {.55f, .08f, .30f}},
    {"conopodium_majus", MeadowRole::forb, 5, 6, .07, .40, 1, 30, .020, 85, {.80f, .80f, .75f}},
    // The shade edge.
    {"vaccinium_myrtillus", MeadowRole::dwarf_shrub, 6, 6, .18, .30, 0, 0, 0, 0, {0, 0, 0}},
    // The dry slope.
    {"leucanthemum_vulgare", MeadowRole::forb, 4, 8, .07, .50, 1, 20, .020, 85, {.85f, .85f, .78f}},
    {"lotus_corniculatus", MeadowRole::forb, 4, 7, .07, .15, 4, 6, .008, 60, {.85f, .55f, .02f}},
    {"thymus_polytrichus", MeadowRole::forb, 4, 8, .07, .10, 6, 10, .004, 70, {.50f, .25f, .45f}},
};
f64 clamp_to(f64 v, f64 lo, f64 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

std::span<const MeadowSpeciesTraits> meadow_species() noexcept { return k_meadow; }
const MeadowSpeciesTraits* meadow_species(std::string_view name) noexcept {
  for (const auto& species : k_meadow)
    if (species.name == name) return &species;
  return nullptr;
}
MeadowNiche meadow_niche(const MeadowRule& rule, const scene_gen::SiteSample& site, f64 wet,
                         f64 shade) noexcept {
  // The ground's own F: drier on a sunny slope and a steep one, wetter in a hollow, held to the
  // scale's 1 to the wet margin's 9.
  f64 f = static_cast<f64>(rule.moisture_base) -
          static_cast<f64>(rule.heat_dry) * (site.heat_load - 1) -
          static_cast<f64>(rule.slope_dry) * site.slope -
          static_cast<f64>(rule.hollow_wet) * site.curvature;
  f = clamp_to(f, 1, k_meadow_wet_margin);
  // The water's reach: 1 at a channel's bank or a body's outline falling on a smoothstep to 0 at
  // `wet_reach_m`, or the paint's wet band, whichever is wetter, lifts F towards the margin's.
  const f64 t = clamp_to(site.water_m() / static_cast<f64>(rule.wet_reach_m), 0, 1);
  const f64 near = 1 - t * t * (3 - 2 * t);
  const f64 w = std::max(clamp_to(wet, 0, 1), near);
  MeadowNiche niche;
  niche.moisture = f + (k_meadow_wet_margin - f) * w;
  niche.light = static_cast<f64>(rule.light_open) +
                static_cast<f64>(rule.light_shade - rule.light_open) * clamp_to(shade, 0, 1);
  return niche;
}
f64 meadow_wetness(const MeadowRule& rule, const MeadowNiche& niche) noexcept {
  const f64 base = static_cast<f64>(rule.moisture_base);
  if (!(base < k_meadow_wet_margin)) return niche.moisture >= base ? 1 : 0;
  return clamp_to((niche.moisture - base) / (k_meadow_wet_margin - base), 0, 1);
}
f64 meadow_response(const MeadowSpeciesTraits& species, const MeadowNiche& niche) noexcept {
  const f64 df = (niche.moisture - species.moisture) / k_meadow_moisture_tolerance;
  const f64 dl = (niche.light - species.light) / k_meadow_light_tolerance;
  const f64 r2 = df * df + dl * dl;
  // Past `k_meadow_reach` tolerances the species is absent, not a 1/65536 tail on every cell.
  if (r2 > k_meadow_reach * k_meadow_reach) return 0;
  return det::exp(-.5 * r2);
}
u32 meadow_cover(std::span<const MeadowSpeciesTraits* const> species, const MeadowNiche& niche,
                 std::span<u32> out) noexcept {
  u64 sum = 0;
  for (usize i = 0; i < species.size(); ++i) {
    const f64 cover = species[i]->cover * meadow_response(*species[i], niche);
    out[i] = static_cast<u32>(clamp_to(cover, 0, 1) * static_cast<f64>(k_meadow_cover_one));
    sum += out[i];
  }
  // A community's covers may sum past one where several species' optima meet (Domin covers do,
  // the NVC handbook says); a cell holds one plant, so the share is scaled down, in integers.
  if (sum > k_meadow_cover_one) {
    u64 scaled = 0;
    for (usize i = 0; i < species.size(); ++i) {
      out[i] = static_cast<u32>(static_cast<u64>(out[i]) * k_meadow_cover_one / sum);
      scaled += out[i];
    }
    sum = scaled;
  }
  return static_cast<u32>(sum);
}
Herb meadow_herb(const MeadowSpeciesTraits& species, u64 seed) {
  Herb h;
  h.seed = seed;
  h.height_m = static_cast<f32>(clamp_to(species.height_m, .1, 2));
  if (species.flowers > 0) {
    h.flowers = species.flowers;
    h.spike = species.flowers > 1 ? .3f : 0.0f;
    h.petals = species.petals;
    h.petal_length_m = static_cast<f32>(clamp_to(species.petal_m, .004, .08));
    h.petal_angle_deg = static_cast<f32>(species.petal_angle_deg);
    h.flower_red = species.flower_rgb[0];
    h.flower_green = species.flower_rgb[1];
    h.flower_blue = species.flower_rgb[2];
    h.leaves = 6;
    h.leaf_length_m = static_cast<f32>(clamp_to(species.height_m * .15, .01, .3));
  } else {
    // A sedge's, a rush's or a dwarf shrub's stand-in: leaves alone, narrow and upright for the
    // graminoids, small and spreading for the shrub, from a root with no scape.
    h.flowers = 0;
    h.rosette = true;
    const bool shrub = species.role == MeadowRole::dwarf_shrub;
    h.leaves = shrub ? 30 : 24;
    h.leaf_length_m = static_cast<f32>(shrub ? .03 : clamp_to(species.height_m * .8, .01, .3));
    h.leaf_width = shrub ? .5f : .1f;
    h.leaf_angle_deg = shrub ? 60.0f : 20.0f;
  }
  return h;
}
u32 meadow_far_thin(const MeadowSpeciesTraits& species) noexcept {
  if (species.flowers == 0) return 0;
  return std::clamp<u32>(k_meadow_far_heads / species.flowers, 1, k_meadow_far_thin);
}
Herb meadow_far_herb(const MeadowSpeciesTraits& species, u64 seed) {
  Herb h = meadow_herb(species, seed);
  const u32 thin = meadow_far_thin(species);
  if (thin == 0) return h;
  // The heads of `thin` plants on one stem, and no leaves (the sward's card is the green there).
  h.flowers = std::min(species.flowers * thin, k_meadow_far_heads);
  h.spike = h.flowers > 1 ? .5f : 0.0f;
  h.leaves = 0;
  // Fewer, larger petal cards a head, the head's coloured area kept: n l^2 = n' l'^2.
  const u32 petals = std::min<u32>(species.petals, 5);
  h.petals = std::max<u32>(petals, 3);
  const f64 scale = std::sqrt(static_cast<f64>(species.petals) / static_cast<f64>(h.petals));
  h.petal_length_m = static_cast<f32>(clamp_to(species.petal_m * scale, .004, .08));
  return h;
}

namespace {
// The far ring's plants (G140): stream 12 of the patch's hash keeps one plant in `thin` of a
// flowering species', standing for all of them.
constexpr u64 k_far_stream = 12;
// Where a member that names no mesh has its stand-in written: `<ddc>/meadow`, or `meadow` beside
// the scene where no derived-data root is found (the cliffs generator's rule).
std::string recipe_dir(const scene_gen::Context& context) {
  std::string ddc(context.ddc);
  if (ddc.empty()) {
    std::error_code ec;
    const std::filesystem::path from = std::filesystem::absolute(
        std::filesystem::path(context.dir.empty() ? "." : std::string(context.dir)), ec);
    if (!ec) ddc = geometry::find_ddc_root(from.generic_string());
  }
  const std::filesystem::path base =
      !ddc.empty() ? std::filesystem::path(ddc) : std::filesystem::path(std::string(context.dir));
  return (base / "meadow").lexically_normal().generic_string();
}
// Writes the stand-in recipe under its content hash (once: the name is what it says) and names it.
bool stand_in(const std::string& dir, const MeadowSpeciesTraits& species, const Herb& herb,
              const char* form, scene_gen::PlacementMesh& out, std::string* error) {
  FloraFile file;
  file.species = k_species_herb;
  file.herb = herb;
  const std::string text = canonical_flora_json(file);
  char name[96];
  std::snprintf(name, sizeof(name), "%.40s-%s-%016llx.flora.json",
                std::string(species.name).c_str(), form,
                static_cast<unsigned long long>(flora_source_hash(file)));
  const std::filesystem::path path = std::filesystem::path(dir) / name;
  out.path = path.generic_string();
  out.name = std::string("meadow/") + name;
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return true;
  std::filesystem::create_directories(dir, ec);
  // A partial name of the writer's own, so two hosts opening the same scene at once never write
  // into one file; the rename then lands the same bytes either way.
  const std::filesystem::path partial =
      std::filesystem::path(dir) /
      (std::string(name) + "." +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
       std::to_string(reinterpret_cast<std::uintptr_t>(&text)) + ".partial");
  {
    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    stream << text;
    if (!stream.good()) {
      if (error) *error = "sward: cannot write " + partial.generic_string();
      return false;
    }
  }
  std::filesystem::rename(partial, path, ec);
  if (ec) {
    std::error_code removed;
    std::filesystem::remove(partial, removed);
  }
  if (ec && !std::filesystem::exists(path)) {
    if (error) *error = "sward: cannot write " + out.path;
    return false;
  }
  return true;
}
struct Entry {
  SwardPlacements params;
  Vector<scene_gen::PlacementMesh> meshes;
  Vector<u32> family_base;
  u32 cells = 0;
  // G137: each form's families in ascending density (indices into `params.families`). With no form
  // named, form 0 (`any`) holds every family and the pick is the one it always was.
  Vector<u32> form_families[5];
  bool forms = false;
  // The first community mesh in `meshes`; the community's follow every family's.
  u32 forb_base = 0;
  // G140: the community's species, in the entry's order, and each one's far form: its mesh in
  // `meshes` and how many plants it stands for (0: none, a species with no flowers).
  Vector<const MeadowSpeciesTraits*> community;
  Vector<u32> far_mesh;
  Vector<u32> far_thin;
};
constexpr u32 k_forms = 5;
constexpr u32 k_forb_id_bit = 0x80000000u;
// How far a patch's centre moves from its cell's, a side, as a fraction of the cell.
constexpr f64 k_jitter = 0.25;
// Uniform in [0, 1) from stream `stream` of a patch's hash.
f64 unit_of(u64 random, u64 stream) {
  return static_cast<f64>(hash_combine(random, stream) >> 11) / 9007199254740992.0;
}
// The patch's turn: `yaw` about +y, then the tip of +y onto the ground's normal at the patch,
// capped at `max_tilt_deg`. The normal is from central differences of the ground half a cell to
// each side (the scale the patch covers), so a patch follows the ground it stands on and not a
// bump a few millimetres wide. The tip is about the axis up x normal, so it leans downhill.
Quat patch_rotation(const scene_gen::Ground& ground, i64 x_mm, i64 z_mm, f64 cell_m,
                    f32 max_tilt_deg, f64 yaw) {
  const Quat turn = quat_from_axis_angle(Vec3::unit_y(), static_cast<f32>(yaw));
  if (!(max_tilt_deg > 0)) return turn;
  const i64 d = std::max<i64>(1, scene_gen::nearest_mm(.5 * cell_m));
  const f64 gx = (ground.surface_mm(x_mm + d, z_mm) - ground.surface_mm(x_mm - d, z_mm)) /
                 static_cast<f64>(2 * d) * 1000.0;
  const f64 gz = (ground.surface_mm(x_mm, z_mm + d) - ground.surface_mm(x_mm, z_mm - d)) /
                 static_cast<f64>(2 * d) * 1000.0;
  const f64 slope = std::sqrt(gx * gx + gz * gz);
  if (!(slope > 1e-9)) return turn;
  const f64 cap = static_cast<f64>(max_tilt_deg) * 0.017453292519943295;
  const f64 angle = std::min(sward_atan(slope), cap);
  // The normal's horizontal part points downhill, -grad/|grad|; up x n = (n.z, 0, -n.x).
  const Vec3 axis{static_cast<f32>(-gz / slope), 0.0f, static_cast<f32>(gx / slope)};
  return quat_from_axis_angle(axis, static_cast<f32>(angle)) * turn;
}
// **The hand-off by distance** (G136; flora.md, "Grass sward (G90)"; renderer.md, "The hand-off by
// distance"). The inner ring places each patch twice, as its near form and as its card, the near
// form named `k_handoff_near` and the card `k_handoff_far`, and the renderer hands one to the other
// across a band of distance from the eye — the near form's blades thinning out as the card's
// thicken in — instead of at the edge of the ring's tiles, where the switch was a straight line
// that jumped as the walker crossed a tile. The card is appended after every near form, so the
// near forms keep the places, ids and order they had; it is the same patch, so it keeps its id.
// Ring 1 places the card alone, with no role: it draws at every distance. A read outside a
// streamed world (`k_no_ring`: `expand`, the whole read) places the near form alone, as before.
u32 card_of(const Entry& entry, u32 near_mesh) {
  u32 family = 0;
  while (family + 1 < entry.family_base.size() && entry.family_base[family + 1] <= near_mesh)
    ++family;
  return near_mesh + static_cast<u32>(entry.params.families[family].near.size());
}
void hand_off(const Entry& entry, scene_gen::Placements& out) {
  const u32 nears = out.instances.size();
  out.instances.reserve(2 * nears);
  for (u32 i = 0; i < nears; ++i) {
    scene_gen::Placement card = out.instances[i];
    out.instances[i].handoff = scene_gen::k_handoff_near;
    card.handoff = scene_gen::k_handoff_far;
    card.mesh = card_of(entry, card.mesh);
    out.instances.push_back(card);
  }
}
bool fail(std::string* error, std::string text) {
  if (error) *error = "sward: " + text;
  return false;
}
// G137: the forms' and the forbs' half of `open`, after the families are read: either every family
// names a form or none does, each form's classes reach weight 1, and the forbs' meshes follow.
bool open_forms(Entry& entry, const scene_gen::Context& context, std::string* error) {
  const auto& p = entry.params;
  entry.forms = entry.form_families[0].empty();
  for (u32 form = 0; form < k_forms; ++form) {
    const auto& list = entry.form_families[form];
    if (!list.empty() && p.families[list.back()].density != 1)
      return fail(error, form == 0 ? "last density family must cover weight 1"
                                   : "last density family of each form must cover weight 1");
  }
  if (!entry.forms && entry.form_families[0].size() != p.families.size())
    return fail(error, "either every family names a form or none does");
  if (entry.forms) {
    const auto& s = p.stature;
    if (!(s.mosaic_m >= 4 && s.mosaic_m <= 200) || !(s.slope_hi_deg >= s.slope_lo_deg) ||
        (s.shade_layer != scene_gen::k_mask_no_layer &&
         s.shade_layer >= scene_gen::k_mask_max_palette) ||
        !(s.band_m >= 0 && s.band_m <= s.mosaic_m))
      return fail(error,
                  "stature needs mosaic_m 4..200, slope_hi_deg >= slope_lo_deg, a palette "
                  "shade_layer or 255 and band_m 0..mosaic_m");
  }
  const auto& m = p.meadow;
  if (!(m.moisture_base >= 1 && m.moisture_base <= 9) ||
      !(m.wet_reach_m >= .5f &&
        static_cast<f64>(m.wet_reach_m) <= scene_gen::k_site_water_reach_m) ||
      !(m.heat_dry >= 0 && m.heat_dry <= 10) || !(m.slope_dry >= 0 && m.slope_dry <= 10) ||
      !(m.hollow_wet >= 0 && m.hollow_wet <= 200) || !(m.light_open >= 1 && m.light_open <= 9) ||
      !(m.light_shade >= 1 && m.light_shade <= 9))
    return fail(error,
                "meadow needs moisture_base 1..9, wet_reach_m 0.5..128, heat_dry and slope_dry "
                "0..10, hollow_wet 0..200 and light_open, light_shade 1..9");
  entry.forb_base = entry.meshes.size();
  // G140: the one-mesh forbs are the community's now; an entry still naming them is refused rather
  // than read as something it did not say.
  if (!p.forbs.empty()) return fail(error, "forbs is replaced by community (G140)");
  if (p.community.empty()) return true;
  if (p.community.size() > 16 || p.forb_tile_cap > 4096)
    return fail(error, "community needs 1..16 species and forb_tile_cap 0..4096");
  // A plant's ID is its cell's with the top bit set, so the cells must fit in 31 bits.
  const u64 width = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 depth = static_cast<u64>(static_cast<i64>(p.max_z) - p.min_z + 1);
  if (width * depth * entry.cells * entry.cells >= k_forb_id_bit)
    return fail(error, "tile window with a community exceeds the 31-bit stable cell ID space");
  // The near meshes first, a member's own or its stand-in's, then the far forms (G140).
  std::string dir;
  const auto authored = [&](const std::string& path) {
    const auto resolved = (std::filesystem::path(context.dir) / path).lexically_normal();
    entry.meshes.push_back({resolved.generic_string(), "meadow", 0});
  };
  const auto generated = [&](const MeadowSpeciesTraits& species, const Herb& herb,
                             const char* form) {
    if (dir.empty()) dir = recipe_dir(context);
    scene_gen::PlacementMesh mesh;
    if (!stand_in(dir, species, herb, form, mesh, error)) return false;
    entry.meshes.push_back(std::move(mesh));
    return true;
  };
  for (const auto& member : p.community) {
    const auto* species = meadow_species(member.species);
    if (species == nullptr)
      return fail(error, "unknown community species '" + member.species + "'");
    entry.community.push_back(species);
    if (!member.mesh.empty())
      authored(member.mesh);
    else if (!generated(*species, meadow_herb(*species, p.seed), "near"))
      return false;
  }
  for (u32 i = 0; i < p.community.size(); ++i) {
    const auto& species = *entry.community[i];
    const u32 thin = meadow_far_thin(species);
    entry.far_thin.push_back(thin);
    entry.far_mesh.push_back(entry.meshes.size());
    if (thin == 0) continue;
    if (!p.community[i].far.empty())
      authored(p.community[i].far);
    else if (!generated(species, meadow_far_herb(species, p.seed), "far"))
      return false;
  }
  return true;
}
// The stature rule's steepness: `sward_site_form`'s smoothstep of the slope between its thresholds.
f64 steepness(const SwardStature& r, f64 slope_deg) noexcept {
  const f64 lo = static_cast<f64>(r.slope_lo_deg), hi = static_cast<f64>(r.slope_hi_deg);
  if (!(hi > lo)) return slope_deg >= lo ? 1 : 0;
  const f64 t = clamp_to((slope_deg - lo) / (hi - lo), 0, 1);
  return t * t * (3 - 2 * t);
}
// The C library's arctangent rounds differently on MSVC and glibc, and this slope decides the
// form: the sward's own, in plain arithmetic (sward.h).
f64 slope_degrees(f64 slope) noexcept { return sward_atan(slope) * 57.29577951308232; }
// What a patch's cell reads of its site (G137, G140): the niche the community answers to, and the
// form the stature rule picks there, or the nearest form the entry has families for (the other
// habit, then the other stature, then both).
struct CellSite {
  MeadowNiche niche;
  u32 form = 0;
};
constexpr u32 k_cell_site_fields = scene_gen::k_site_form | scene_gen::k_site_water;
// G137's inputs and G140's (flora.md, "Growth forms by site", "The meadow community"): the site at
// the patch's moved centre (its slope over the site's 10 m baseline, its heat load, curvature and
// distances to water), the paint's wet band and its shade layer's weight, become the niche; the
// stature rule reads the niche's wetness, the site's slope, the shade and the mosaic noise.
CellSite cell_site(const Entry& entry, const scene_gen::Context& context, u32 table,
                   const scene_gen::MaskSample& sample, i64 x_mm, i64 z_mm, u64 random) {
  const auto& p = entry.params;
  const auto& rule = p.stature;
  CellSite out;
  SwardSite site;
  const f64 paint_wet = static_cast<f64>(sample.wetness);
  if (rule.shade_layer != scene_gen::k_mask_no_layer)
    for (u32 ch = 0; ch < scene_gen::k_mask_channels; ++ch)
      if (((table >> (ch * 8)) & 255u) == rule.shade_layer)
        site.shade += static_cast<f64>(sample.weight[ch]);
  const auto here = context.site_mm(x_mm, z_mm, k_cell_site_fields);
  out.niche = meadow_niche(p.meadow, here, paint_wet, site.shade);
  if (!entry.forms) return out;
  site.wet = meadow_wetness(p.meadow, out.niche);
  site.slope_deg = slope_degrees(here.slope);
  const u64 base = hash_combine(context.world_seed, p.seed);
  const f64 x = static_cast<f64>(x_mm) * .001, z = static_cast<f64>(z_mm) * .001;
  const f64 scale = static_cast<f64>(rule.mosaic_m);
  // G138: each noise's gradient from the same four nodes, and the cell's two draws (streams 10 and
  // 11 of the patch's hash) for where the band mixes the forms; at band_m 0 neither is read.
  f64 tall_dx = 0, tall_dz = 0, tussock_dx = 0, tussock_dz = 0;
  site.noise_tall =
      sward_mosaic_noise_gradient(hash_combine(base, 0x74616c6cull), x, z, scale, tall_dx, tall_dz);
  site.gradient_tall = std::sqrt(tall_dx * tall_dx + tall_dz * tall_dz);
  site.noise_tussock = sward_mosaic_noise_gradient(hash_combine(base, 0x7475737365ull), x, z, scale,
                                                   tussock_dx, tussock_dz);
  site.gradient_tussock = std::sqrt(tussock_dx * tussock_dx + tussock_dz * tussock_dz);
  site.draw_tall = unit_of(random, 10);
  site.draw_tussock = unit_of(random, 11);
  // **G139, the band where a site field draws the boundary** (flora.md, "Where a site field draws
  // the boundary"). G138's band turns metres into score by the noise's gradient alone, so where the
  // wetness or the slope moves the score the band narrowed to a line. The fields' part of each
  // score is taken again a cell along x and along z, and the band reads the whole score's gradient:
  // the noise's (analytic) plus the fields' (forward differences), as vectors. `sward_site_form`
  // reads a gradient in units of the noise's own (it multiplies by 2 |noise|), so the whole is
  // handed over divided by that; with no field gradient the noise's is handed over as it was.
  const f64 weight = 2 * std::fabs(static_cast<f64>(rule.noise));
  if (rule.band_m > 0 && weight > 0) {
    const i64 h_mm = scene_gen::nearest_mm(static_cast<f64>(p.cell_m));
    const f64 h = static_cast<f64>(h_mm) * .001;
    const auto fields = [&](const scene_gen::SiteSample& s, f64& tall, f64& tussock) {
      const f64 wet = meadow_wetness(p.meadow, meadow_niche(p.meadow, s, paint_wet, site.shade));
      const f64 steep = steepness(rule, slope_degrees(s.slope));
      tall = static_cast<f64>(rule.wet_tall) * wet + static_cast<f64>(rule.steep_tall) * steep;
      tussock =
          static_cast<f64>(rule.wet_tussock) * wet + static_cast<f64>(rule.steep_tussock) * steep;
    };
    f64 tall0 = 0, tussock0 = 0, tall_x = 0, tussock_x = 0, tall_z = 0, tussock_z = 0;
    fields(here, tall0, tussock0);
    fields(context.site_mm(x_mm + h_mm, z_mm, k_cell_site_fields), tall_x, tussock_x);
    fields(context.site_mm(x_mm, z_mm + h_mm, k_cell_site_fields), tall_z, tussock_z);
    const f64 noise = static_cast<f64>(rule.noise);
    const auto whole = [&](f64 field_x, f64 field_z, f64 noise_dx, f64 noise_dz, f64& gradient) {
      if (field_x == 0 && field_z == 0) return;
      const f64 gx = 2 * noise * noise_dx + field_x / h;
      const f64 gz = 2 * noise * noise_dz + field_z / h;
      gradient = std::sqrt(gx * gx + gz * gz) / weight;
    };
    whole(tall_x - tall0, tall_z - tall0, tall_dx, tall_dz, site.gradient_tall);
    whole(tussock_x - tussock0, tussock_z - tussock0, tussock_dx, tussock_dz,
          site.gradient_tussock);
  }
  const u32 form = static_cast<u32>(sward_site_form(rule, site));
  // 1 tall sod, 2 tall tussock, 3 short sod, 4 short tussock.
  const u32 order[4] = {form, form % 2 ? form + 1 : form - 1, form <= 2 ? form + 2 : form - 2,
                        5 - form};
  out.form = form;
  for (const u32 f : order)
    if (!entry.form_families[f].empty()) {
      out.form = f;
      break;
    }
  return out;
}
// The density classes' unbiased stochastic interpolation over one form's families (the pick the
// placement always made, over a list): false where the cell draws no patch.
bool pick_family(const SwardPlacements& p, const Vector<u32>& list, f32 density, u64 random,
                 u32& family) {
  u32 upper = 0;
  while (upper + 1 < list.size() && p.families[list[upper]].density < density)
    ++upper;
  const f32 low = upper == 0 ? 0 : p.families[list[upper - 1]].density;
  const f32 t = (density - low) / (p.families[list[upper]].density - low);
  const f64 unit = static_cast<f64>(random >> 11) / 9007199254740992.0;
  if (unit >= static_cast<f64>(t)) {
    if (upper == 0) return false;
    --upper;
  }
  family = list[upper];
  return true;
}
bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto entry = std::make_unique<Entry>();
  schema::ReadContext read;
  if (!schema::from_json(entry->params, params, read) || !read.ok())
    return fail(error, "invalid engine.flora.SwardPlacements");
  const auto& p = entry->params;
  if ((p.cell_m != 1 && p.cell_m != 2) || !(context.tile_size >= p.cell_m) ||
      context.tile_size > 1024 || std::fmod(context.tile_size, p.cell_m) != 0)
    return fail(error, "cell_m must be 1 or 2 and divide the tile size (at most 1024 m)");
  entry->cells = static_cast<u32>(context.tile_size / p.cell_m);
  if (!(p.max_tilt_deg >= 0 && p.max_tilt_deg <= 45))
    return fail(error, "max_tilt_deg must be 0..45");
  if (p.min_x > p.max_x || p.min_z > p.max_z || p.layer >= scene_gen::k_mask_max_palette)
    return fail(error, "invalid tile window or palette layer");
  const u64 width = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 depth = static_cast<u64>(static_cast<i64>(p.max_z) - p.min_z + 1);
  const u64 tile_cells = static_cast<u64>(entry->cells) * entry->cells;
  if (width > 0xffffffffull / depth || width * depth > 0xffffffffull / tile_cells)
    return fail(error, "tile window exceeds the nonzero 32-bit stable cell ID space");
  if (p.families.empty() || p.families.size() > 16)
    return fail(error, "need 1..16 ascending density families");
  f32 previous[k_forms] = {};
  for (u32 index = 0; index < p.families.size(); ++index) {
    const auto& family = p.families[index];
    const u32 form = static_cast<u32>(family.form);
    if (form >= k_forms) return fail(error, "unknown family form");
    if (!(family.density > previous[form] && family.density <= 1) || family.near.empty() ||
        family.near.size() > 16 || family.cards.size() != family.near.size())
      return fail(error, "families need ascending density and equal near/cards lists (1..16)");
    previous[form] = family.density;
    entry->form_families[form].push_back(index);
    entry->family_base.push_back(entry->meshes.size());
    for (const auto* paths : {&family.near, &family.cards}) {
      for (const auto& path : *paths) {
        if (path.empty()) return fail(error, "empty family mesh path");
        const auto resolved = (std::filesystem::path(context.dir) / path).lexically_normal();
        entry->meshes.push_back({resolved.generic_string(), "sward", 0});
      }
    }
  }
  if (!open_forms(*entry, context, error)) return false;
  scene_gen::GroundMasks masks;
  if (!context.ground.masks(masks, error)) return false;
  if (!masks.valid()) return fail(error, "requires the cooked terrain mask level");
  *state = entry.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  const auto& p = static_cast<const Entry*>(state)->params;
  return tile.x >= p.min_x && tile.x <= p.max_x && tile.z >= p.min_z && tile.z <= p.max_z;
}
u8 representation(const void*, const scene_gen::Context& context) noexcept {
  return context.ring == scene_gen::k_no_ring ? 0 : std::min<u8>(context.ring, 2);
}
bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  out = static_cast<const Entry*>(state)->meshes;
  return true;
}
// G140, the meadow's community (G137's forbs before it): a cell that drew a patch draws one plant,
// its species by the covers its niche gives the community — stream 4 of the patch's hash, its top
// 16 bits against the covers' running sum in 1/65536, so a species stands in its cover's share of
// the cells and a cell past the sum holds grass alone. Within 0.35 of a cell a side of the patch's
// moved centre, so inside the circle the patch cleared earlier structures by, and asked of the
// exclusion at its own point. Upright (a herb grows vertical) and turned by stream 7; its rank
// among a tile's from stream 9.
//
// **The far ring** (representation 1) draws the same plant in the same cell and keeps one in
// `meadow_far_thin` of a flowering species' (stream 12), as its far form: the heads of that many
// plants on one stem, leafless, their coloured area kept (`meadow_far_herb`). A species with no
// flowers has no far form; the sward's card is its colour there.
struct Forb {
  u64 rank = 0;
  u32 plants = 1;  // how many plants it stands for
  scene_gen::Placement placed;
};
void add_forb(const Entry& entry, const scene_gen::Context& context, u64 random, u32 id, f64 px,
              f64 pz, const MeadowNiche& niche, bool far, Vector<Forb>& forbs) {
  const auto& p = entry.params;
  u32 covers[16] = {};
  const u32 count = entry.community.size();
  meadow_cover({entry.community.data(), count}, niche, {covers, count});
  const u32 draw = static_cast<u32>(hash_combine(random, 4) >> 48);
  u32 sum = 0, species = count;
  for (u32 i = 0; i < count; ++i) {
    sum += covers[i];
    if (draw < sum) {
      species = i;
      break;
    }
  }
  if (species == count) return;
  const u32 thin = far ? entry.far_thin[species] : 1;
  if (thin == 0 || hash_combine(random, k_far_stream) % thin != 0) return;
  const f64 cell = static_cast<f64>(p.cell_m);
  const f64 fx = px + (unit_of(random, 5) * 2 - 1) * .35 * cell;
  const f64 fz = pz + (unit_of(random, 6) * 2 - 1) * .35 * cell;
  const i64 fx_mm = scene_gen::nearest_mm(fx), fz_mm = scene_gen::nearest_mm(fz);
  if (context.excluded_mm(fx_mm, fz_mm)) return;
  Forb forb;
  forb.rank = hash_combine(random, 9);
  forb.placed.id = id | k_forb_id_bit;
  forb.plants = thin;
  forb.placed.mesh = far ? entry.far_mesh[species] : entry.forb_base + species;
  forb.placed.position = {fx, context.ground.surface_mm(fx_mm, fz_mm), fz};
  forb.placed.rotation = quat_from_axis_angle(
      Vec3::unit_y(), static_cast<f32>(unit_of(random, 7) * 6.283185307179586));
  forbs.push_back(forb);
}
// A tile keeps at most `forb_tile_cap` plants, those of lowest rank (every one is a run of the
// frame's pose table: the herb's motion kind is `parts`), and appends them after the patches in
// their cells' order, so the answer is the tile's whatever the request order. A far form counts as
// the plants it stands for, so both rings hold the same flowers a tile at the cap.
void keep_forbs(const Entry& entry, Vector<Forb>& forbs, scene_gen::Placements& out) {
  if (forbs.empty()) return;
  const u64 cap = entry.params.forb_tile_cap;
  u64 total = 0;
  for (const auto& forb : forbs)
    total += forb.plants;
  if (total > cap) {
    std::sort(forbs.begin(), forbs.end(), [](const Forb& a, const Forb& b) {
      return a.rank != b.rank ? a.rank < b.rank : a.placed.id < b.placed.id;
    });
    u32 kept = 0;
    u64 sum = 0;
    while (kept < forbs.size() && sum + forbs[kept].plants <= cap)
      sum += forbs[kept++].plants;
    forbs.resize(kept);
    std::sort(forbs.begin(), forbs.end(),
              [](const Forb& a, const Forb& b) { return a.placed.id < b.placed.id; });
  }
  for (const auto& forb : forbs)
    out.instances.push_back(forb.placed);
}
bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string* error) {
  out.clear();
  out.kind = representation(state, context);
  if (out.kind >= 2 || !occupies(state, at)) return true;
  const auto& entry = *static_cast<const Entry*>(state);
  const auto& p = entry.params;
  scene_gen::GroundMasks masks;
  if (!context.ground.masks(masks, error)) return false;
  scene_gen::GroundMaskInfo info;
  if (!masks.valid() || masks.ops->info == nullptr || masks.ops->tile == nullptr ||
      !masks.ops->info(masks.state, info) || info.spacing_mm <= 0 || info.cells < 1 ||
      (info.cells & (info.cells - 1)) != 0 || info.cells > 1024 || p.layer >= info.palette)
    return fail(error, "invalid or missing mask level/layer");
  Vector<u32> words(scene_gen::mask_tile_words(info.cells));
  Vector<Forb> forbs;
  i32 last_x = 0, last_z = 0;
  bool loaded = false, found = false;
  const u64 width = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 tile_id = static_cast<u64>(static_cast<i64>(at.z) - p.min_z) * width +
                      static_cast<u64>(static_cast<i64>(at.x) - p.min_x);
  for (u32 z = 0; z < entry.cells; ++z) {
    for (u32 x = 0; x < entry.cells; ++x) {
      const f64 wx = static_cast<f64>(at.x) * static_cast<f64>(context.tile_size) +
                     (x + .5) * static_cast<f64>(p.cell_m);
      const f64 wz = static_cast<f64>(at.z) * static_cast<f64>(context.tile_size) +
                     (z + .5) * static_cast<f64>(p.cell_m);
      const i64 x_mm = scene_gen::nearest_mm(wx), z_mm = scene_gen::nearest_mm(wz);
      // Check in i64 before mask_tile_of narrows its tile coordinates to i32. An unrelated
      // distant placement window must not wrap into the authored mask level.
      const i64 mask_x =
          scene_gen::mask_floor_div(scene_gen::mask_floor_div(x_mm, info.spacing_mm), info.cells);
      const i64 mask_z =
          scene_gen::mask_floor_div(scene_gen::mask_floor_div(z_mm, info.spacing_mm), info.cells);
      if (mask_x < info.tile_min_x || mask_x > info.tile_max_x || mask_z < info.tile_min_z ||
          mask_z > info.tile_max_z)
        continue;
      const auto ref = scene_gen::mask_tile_of(info, x_mm, 0, z_mm, 0);
      if (!loaded || last_x != ref.tx || last_z != ref.tz) {
        last_x = ref.tx;
        last_z = ref.tz;
        loaded = true;
        found = masks.ops->tile(masks.state, ref.tx, ref.tz, words);
      }
      if (!found) continue;
      const auto sample = scene_gen::mask_sample(info, words, ref, 0);
      f32 density = 0;
      for (u32 ch = 0; ch < scene_gen::k_mask_channels; ++ch)
        if (((words[0] >> (ch * 8)) & 255u) == p.layer) density += sample.weight[ch];
      if (!(density > 0)) continue;
      const u32 id =
          static_cast<u32>(tile_id * entry.cells * entry.cells + z * entry.cells + x + 1);
      const u64 random = hash_combine(hash_combine(context.world_seed, p.seed), id);
      // The patch's own place, turn and lean come from independent streams of the same hash, so
      // they are the patch's whatever the ring, the tile order or the density that kept it. The
      // centre moves up to `k_jitter` of a cell a side: a patch's half-diagonal is the same at
      // any yaw, so the exclusion below tests the *moved* centre against the same radius and the
      // whole patch still clears an earlier structure. (A quarter cell keeps neighbours' edges
      // within half a cell of touching; their leaves overhang the rest.)
      const f64 cell = static_cast<f64>(p.cell_m);
      const f64 px = wx + (unit_of(random, 1) * 2 - 1) * k_jitter * cell;
      const f64 pz = wz + (unit_of(random, 2) * 2 - 1) * k_jitter * cell;
      bool excluded = false;
      for (const auto& earlier : context.earlier) {
        if (!(earlier.radius > 0)) continue;
        const f64 dx = px - static_cast<f64>(earlier.x_mm) * .001;
        const f64 dz = pz - static_cast<f64>(earlier.z_mm) * .001;
        // The full patch clears earlier structures, not merely its centre.
        const f64 radius = earlier.radius + .7071067811865476 * cell;
        if (dx * dx + dz * dz < radius * radius) {
          excluded = true;
          break;
        }
      }
      if (excluded) continue;
      // G135: nothing grows in a hole, a stream's bed and banks or under water, and a bank's
      // grass thins towards it (scene_gen.md, "Exclusion"). Not asked of a scene with none, so its
      // density is the mask's to the bit.
      if (context.exclusion != nullptr) {
        const i64 jx_mm = scene_gen::nearest_mm(px), jz_mm = scene_gen::nearest_mm(pz);
        if (context.excluded_mm(jx_mm, jz_mm)) continue;
        density *= static_cast<f32>(context.riparian_factor_mm(jx_mm, jz_mm));
      }
      // Unbiased stochastic interpolation between adjacent authored density classes. The field
      // is identical in all rings; ring and request order never participate in the hash. G137: of
      // the families of the form the site picks, when the entry names forms.
      const bool plants = !entry.community.empty() && out.kind <= 1;
      CellSite at_cell;
      if (entry.forms || plants)
        at_cell = cell_site(entry, context, words[0], sample, scene_gen::nearest_mm(px),
                            scene_gen::nearest_mm(pz), random);
      const u32 form = at_cell.form;
      u32 upper = 0;
      if (!pick_family(p, entry.form_families[form], density, random, upper)) continue;
      const auto& family = p.families[upper];
      scene_gen::Placement placed;
      placed.id = id;
      placed.mesh = entry.family_base[upper] + static_cast<u32>(random % family.near.size()) +
                    (out.kind == 1 ? family.near.size() : 0);
      const i64 px_mm = scene_gen::nearest_mm(px), pz_mm = scene_gen::nearest_mm(pz);
      placed.position = {px, context.ground.surface_mm(px_mm, pz_mm), pz};
      placed.rotation = patch_rotation(context.ground, px_mm, pz_mm, cell, p.max_tilt_deg,
                                       unit_of(random, 3) * 6.283185307179586);
      // A height field is sampled per patch, which is turned about its axis, moved in its cell and
      // tipped onto the ground. Wetness-specific morphology requires separately cooked families,
      // and the scale stays 1: silently scaling real leaf dimensions is forbidden.
      out.instances.push_back(placed);
      if (plants) add_forb(entry, context, random, id, px, pz, at_cell.niche, out.kind == 1, forbs);
    }
  }
  if (context.ring == 0) hand_off(entry, out);
  // G137's forbs follow the hand-off's pair, with no role: a herb draws at every distance of the
  // near ring, and hand_off must never see one (its card twin would be a mesh that is no card).
  keep_forbs(entry, forbs, out);
  return true;
}
bool expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
            std::string* error) {
  out.clear();
  const auto& entry = *static_cast<const Entry*>(state);
  out.meshes = entry.meshes;
  scene_gen::Placements one;
  // i64 loop counters avoid overflow at an inclusive INT_MAX tile bound.
  for (i64 z = entry.params.min_z; z <= entry.params.max_z; ++z)
    for (i64 x = entry.params.min_x; x <= entry.params.max_x; ++x) {
      if (!tile(state, {static_cast<i32>(x), static_cast<i32>(z)}, context, one, error))
        return false;
      for (const auto& p : one.instances)
        out.instances.push_back(p);
    }
  return true;
}
const scene_gen::PlacementGeneratorDesc k_generator{.name = "sward",
                                                    .open = &open,
                                                    .close = &close,
                                                    .expand = &expand,
                                                    .meshes = &meshes,
                                                    .tile = &tile,
                                                    .occupies = &occupies,
                                                    .representation = &representation};
const scene_gen::Registrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::flora
