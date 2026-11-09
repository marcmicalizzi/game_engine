#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/flora/sward.h>
#include <domain/scene_gen/scene_gen.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <schemas/flora.h>

namespace engine::flora {
namespace {
struct Entry {
  SwardPlacements params;
  Vector<scene_gen::PlacementMesh> meshes;
  Vector<u32> family_base;
  u32 cells = 0;
  // G137: each form's families in ascending density (indices into `params.families`). With no form
  // named, form 0 (`any`) holds every family and the pick is the one it always was.
  Vector<u32> form_families[5];
  bool forms = false;
  // The first forb mesh in `meshes`; the forbs follow every family's.
  u32 forb_base = 0;
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
  const f64 angle = std::min(std::atan(slope), cap);
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
         s.shade_layer >= scene_gen::k_mask_max_palette))
      return fail(error,
                  "stature needs mosaic_m 4..200, slope_hi_deg >= slope_lo_deg and a palette "
                  "shade_layer or 255");
  }
  entry.forb_base = entry.meshes.size();
  if (p.forbs.empty()) return true;
  if (p.forbs.size() > 16 || !(p.forb_share >= 0 && p.forb_share <= 1) || p.forb_tile_cap > 4096)
    return fail(error, "forbs need 1..16 meshes, forb_share 0..1 and forb_tile_cap 0..4096");
  // A forb's ID is its cell's with the top bit set, so the cells must fit in 31 bits.
  const u64 width = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 depth = static_cast<u64>(static_cast<i64>(p.max_z) - p.min_z + 1);
  if (width * depth * entry.cells * entry.cells >= k_forb_id_bit)
    return fail(error, "tile window with forbs exceeds the 31-bit stable cell ID space");
  for (const auto& path : p.forbs) {
    if (path.empty()) return fail(error, "empty forb mesh path");
    const auto resolved = (std::filesystem::path(context.dir) / path).lexically_normal();
    entry.meshes.push_back({resolved.generic_string(), "forb", 0});
  }
  return true;
}
// G137, the stature rule's inputs at a patch (flora.md, "Growth forms by site"): the wetness (the
// mask's wet band, or a stream's nearness over its riparian reach), the slope at the patch's scale,
// the shade layer's weight and the mosaic noise of the place; then the form, or the nearest form
// the entry has families for (the other habit, then the other stature, then both).
u32 site_form(const Entry& entry, const scene_gen::Context& context, u32 table,
              const scene_gen::MaskSample& sample, i64 x_mm, i64 z_mm) {
  const auto& p = entry.params;
  const auto& rule = p.stature;
  SwardSite site;
  const f64 stream =
      (1 - context.riparian_factor_mm(x_mm, z_mm)) / (1 - scene_gen::k_riparian_floor);
  site.wet = std::max(static_cast<f64>(sample.wetness), stream);
  const f64 cell = static_cast<f64>(p.cell_m);
  const i64 d = std::max<i64>(1, scene_gen::nearest_mm(.5 * cell));
  const auto& g = context.ground;
  const f64 gx = (g.surface_mm(x_mm + d, z_mm) - g.surface_mm(x_mm - d, z_mm)) /
                 static_cast<f64>(2 * d) * 1000.0;
  const f64 gz = (g.surface_mm(x_mm, z_mm + d) - g.surface_mm(x_mm, z_mm - d)) /
                 static_cast<f64>(2 * d) * 1000.0;
  site.slope_deg = std::atan(std::sqrt(gx * gx + gz * gz)) * 57.29577951308232;
  if (rule.shade_layer != scene_gen::k_mask_no_layer)
    for (u32 ch = 0; ch < scene_gen::k_mask_channels; ++ch)
      if (((table >> (ch * 8)) & 255u) == rule.shade_layer)
        site.shade += static_cast<f64>(sample.weight[ch]);
  const u64 base = hash_combine(context.world_seed, p.seed);
  const f64 x = static_cast<f64>(x_mm) * .001, z = static_cast<f64>(z_mm) * .001;
  const f64 scale = static_cast<f64>(rule.mosaic_m);
  site.noise_tall = sward_mosaic_noise(hash_combine(base, 0x74616c6cull), x, z, scale);
  site.noise_tussock = sward_mosaic_noise(hash_combine(base, 0x7475737365ull), x, z, scale);
  const u32 form = static_cast<u32>(sward_site_form(rule, site));
  // 1 tall sod, 2 tall tussock, 3 short sod, 4 short tussock.
  const u32 order[4] = {form, form % 2 ? form + 1 : form - 1, form <= 2 ? form + 2 : form - 2,
                        5 - form};
  for (const u32 f : order)
    if (!entry.form_families[f].empty()) return f;
  return form;
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
// G137, the meadow's forbs: a candidate in a share of the cells that drew a patch (stream 4 of the
// patch's hash), within 0.35 of a cell a side of the patch's moved centre, so inside the circle the
// patch cleared earlier structures by, and asked of the exclusion at its own point. Upright (a herb
// grows vertical) and turned by stream 7; its mesh from stream 8, its rank among a tile's from 9.
struct Forb {
  u64 rank = 0;
  scene_gen::Placement placed;
};
void add_forb(const Entry& entry, const scene_gen::Context& context, u64 random, u32 id, f64 px,
              f64 pz, Vector<Forb>& forbs) {
  const auto& p = entry.params;
  if (!(unit_of(random, 4) < static_cast<f64>(p.forb_share))) return;
  const f64 cell = static_cast<f64>(p.cell_m);
  const f64 fx = px + (unit_of(random, 5) * 2 - 1) * .35 * cell;
  const f64 fz = pz + (unit_of(random, 6) * 2 - 1) * .35 * cell;
  const i64 fx_mm = scene_gen::nearest_mm(fx), fz_mm = scene_gen::nearest_mm(fz);
  if (context.excluded_mm(fx_mm, fz_mm)) return;
  Forb forb;
  forb.rank = hash_combine(random, 9);
  forb.placed.id = id | k_forb_id_bit;
  forb.placed.mesh = entry.forb_base + static_cast<u32>(hash_combine(random, 8) % p.forbs.size());
  forb.placed.position = {fx, context.ground.surface_mm(fx_mm, fz_mm), fz};
  forb.placed.rotation = quat_from_axis_angle(
      Vec3::unit_y(), static_cast<f32>(unit_of(random, 7) * 6.283185307179586));
  forbs.push_back(forb);
}
// A tile keeps at most `forb_tile_cap` forbs, those of lowest rank (every forb is a run of the
// frame's pose table: the herb's motion kind is `parts`), and appends them after the patches in
// their cells' order, so the answer is the tile's whatever the request order.
void keep_forbs(const Entry& entry, Vector<Forb>& forbs, scene_gen::Placements& out) {
  if (forbs.empty()) return;
  const u32 cap = entry.params.forb_tile_cap;
  if (forbs.size() > cap) {
    std::sort(forbs.begin(), forbs.end(), [](const Forb& a, const Forb& b) {
      return a.rank != b.rank ? a.rank < b.rank : a.placed.id < b.placed.id;
    });
    forbs.resize(cap);
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
      const u32 form = entry.forms ? site_form(entry, context, words[0], sample,
                                               scene_gen::nearest_mm(px), scene_gen::nearest_mm(pz))
                                   : 0;
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
      if (!p.forbs.empty() && out.kind == 0) add_forb(entry, context, random, id, px, pz, forbs);
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
