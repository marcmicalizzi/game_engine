// The recipe, its key, the finishing every kind shares, and the default sets (matgen.h;
// docs/subsystems/matgen.md).
#include "field.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/texture/texture_build.h>

#include <algorithm>

namespace engine::matgen {

namespace {

bool fail(std::string& error, const char* what) {
  error = what;
  return false;
}

bool is_role(std::string_view role) noexcept {
  const usize slash = role.find('/');
  return slash != std::string_view::npos && slash > 0 && slash + 1 < role.size() &&
         role.find('/', slash + 1) == std::string_view::npos;
}

bool constituents_ok(const Vector<Constituent>& mix_of) noexcept {
  if (mix_of.empty() || mix_of.size() > 16) return false;
  u64 shares = 0;
  for (const Constituent& c : mix_of) {
    if (c.r_permille > 1000 || c.g_permille > 1000 || c.b_permille > 1000 ||
        c.tone_permille > 900 || c.roughness_permille > 1000 || c.speckle_permille > 900 ||
        c.vein_permille > 1000 || c.share_permille > 1000000)
      return false;
    shares += c.share_permille;
  }
  return shares > 0;
}

}  // namespace

bool validate_set(const Set& set, std::string& error) {
  if (!is_role(set.role)) return fail(error, "role is not <word>/<word>");
  if (set.tile_mm < k_tile_min_mm || set.tile_mm > k_tile_max_mm)
    return fail(error, "tile_mm is out of range (50 to 20000)");
  if (set.texels < k_texels_min || set.texels > k_texels_max ||
      (set.texels & (set.texels - 1)) != 0)
    return fail(error, "texels is not a power of two from 64 to 4096");
  if (set.wet_permille > 1000 || set.cavity_permille > 1000)
    return fail(error, "wet_permille or cavity_permille is over 1000");
  if (set.normal_scale_permille > 4000) return fail(error, "normal_scale_permille is over 4000");
  switch (set.kind) {
    case Kind::granular: {
      const Granular& g = set.granular;
      if (!constituents_ok(g.lithologies))
        return fail(error, "granular.lithologies is empty, too long or out of range");
      if (g.d50_um < 100 || g.d50_um > 2000000)
        return fail(error, "granular.d50_um is out of range (100 to 2000000)");
      if (g.sorting_milliphi < 100 || g.sorting_milliphi > 3000 || g.truncate_milliphi < 500 ||
          g.truncate_milliphi > 4000)
        return fail(error, "granular sorting or truncation is out of range");
      if (g.roundness_permille < 100 || g.roundness_permille > 900)
        return fail(error, "granular.roundness_permille is out of range (100 to 900)");
      if (g.ba_min_permille < 200 || g.ba_max_permille > 1000 ||
          g.ba_min_permille > g.ba_max_permille || g.cb_min_permille < 200 ||
          g.cb_max_permille > 1000 || g.cb_min_permille > g.cb_max_permille)
        return fail(error, "granular axis ratios are out of range (200 to 1000, min <= max)");
      if (g.cover_permille > 1000 || g.alignment_permille > 1000 || g.openwork_permille > 1000)
        return fail(error, "a granular share is over 1000");
      if (g.imbrication_deg > 45 || g.scatter_deg > 45)
        return fail(error, "granular.imbrication_deg or scatter_deg is over 45");
      if (g.matrix_um < 50 || g.matrix_um > 20000)
        return fail(error, "granular.matrix_um is out of range (50 to 20000)");
      if (g.buried_permille > 1000 || g.film_permille > 1000 || g.broken_permille > 1000 ||
          g.fines_permille > 1000)
        return fail(error, "granular buried, film, broken or fines share is over 1000");
      if (g.depth_layers < 1 || g.depth_layers > 3)
        return fail(error, "granular.depth_layers is out of range (1 to 3)");
      break;
    }
    case Kind::rock: {
      const Rock& r = set.rock;
      if (!constituents_ok(r.minerals))
        return fail(error, "rock.minerals is empty, too long or out of range");
      if (r.grain_um < 50 || r.grain_um > 50000)
        return fail(error, "rock.grain_um is out of range (50 to 50000)");
      if (r.relief_um > 50000) return fail(error, "rock.relief_um is over 50000");
      if (r.joint_sets > 2 || r.joint_winding < 1 || r.joint_winding > 6 ||
          r.joint_width_um > 50000)
        return fail(error, "rock joints are out of range");
      if (r.rind_permille > 1000 || r.lichen_permille > 1000 || r.pits_permille > 250 ||
          r.rind_r_permille > 1500 || r.rind_g_permille > 1500 || r.rind_b_permille > 1500)
        return fail(error, "a rock share or tint is out of range");
      if (r.laminae > 64) return fail(error, "rock.laminae is over 64");
      if (r.form_um > 200000 || r.streak_permille > 1000 || r.moss_permille > 1000)
        return fail(error, "rock form, streak or moss is out of range");
      if (r.lichen_max_mm < 4 || r.lichen_max_mm > 300)
        return fail(error, "rock.lichen_max_mm is out of range (4 to 300)");
      break;
    }
    case Kind::ground: {
      const Ground& g = set.ground;
      if (g.moss_permille > 1000 || g.soil_permille > 1000 || g.thatch_permille > 1000 ||
          g.live_permille > 1000 || g.moss_permille + g.soil_permille == 0)
        return fail(error, "a ground share is out of range");
      if (g.leaf_min_mm < 5 || g.leaf_max_mm > 300 || g.leaf_min_mm > g.leaf_max_mm ||
          g.leaf_width_min_um < 200 || g.leaf_width_max_um > 20000 ||
          g.leaf_width_min_um > g.leaf_width_max_um)
        return fail(error, "ground leaf sizes are out of range");
      if (g.tuft_mm > 1000 || g.cushion_mm > 2000 || g.clod_mm > 1000 || g.relief_um > 100000 ||
          g.mound_um > 200000 || g.mound_spacing_mm < 50 || g.mound_spacing_mm > 20000)
        return fail(error, "ground tufts, cushions, clods, relief or mounds are out of range");
      if (g.dead_permille > 1000 || g.patch_permille > 1000 || g.decay_permille > 1000 ||
          g.aligned_permille > 1000 || g.stones_permille > 900)
        return fail(error, "a ground share is over its limit");
      if (g.stone_min_mm < 2 || g.stone_min_mm > g.stone_max_mm || g.stone_max_mm > 500)
        return fail(error, "ground stone sizes are out of range");
      break;
    }
    case Kind::wood: {
      const Wood& w = set.wood;
      if (w.form != WoodForm::sawn && w.form != WoodForm::log)
        return fail(error, "wood.form is not sawn or log");
      if (w.figure_um < 500 || w.figure_um > 100000)
        return fail(error, "wood.figure_um is out of range (500 to 100000)");
      if (w.latewood_permille < 50 || w.latewood_permille > 800)
        return fail(error, "wood.latewood_permille is out of range (50 to 800)");
      if (w.wander_mm > 500 || w.spiral_ddeg > 200 || w.relief_um > 5000)
        return fail(error, "wood wander, spiral or relief is out of range");
      if (w.fresh_r_permille > 1000 || w.fresh_g_permille > 1000 || w.fresh_b_permille > 1000 ||
          w.grey_r_permille > 1000 || w.grey_g_permille > 1000 || w.grey_b_permille > 1000 ||
          w.grey_permille > 1000 || w.grey_patch_permille > 1000 || w.stain_permille > 1000)
        return fail(error, "a wood colour or share is over 1000");
      if (w.checks_per_m2 > 1000 || w.check_min_mm < 2 || w.check_min_mm > w.check_max_mm ||
          w.check_max_mm > 5000 || w.check_width_um > 50000 || w.check_depth_um > 100000)
        return fail(error, "wood checks are out of range");
      if (w.knots_per_m2 > 100 || w.knot_min_mm < 2 || w.knot_min_mm > w.knot_max_mm ||
          w.knot_max_mm > 300)
        return fail(error, "wood knots are out of range");
      if (w.bark_permille > 1000 || w.bark_plate_mm < 5 || w.bark_plate_mm > 500)
        return fail(error, "wood bark is out of range");
      break;
    }
    default: return fail(error, "kind is not granular, rock, ground or wood");
  }
  return true;
}

bool read_set(std::string_view text, Set& out, std::string& error) {
  JsonValue value;
  const auto parsed = parse_json(text, value);
  if (!parsed.ok) {
    error = "material set: " + std::string(parsed.message);
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, value, ctx) || !ctx.ok()) {
    error = ctx.diagnostics.empty()
                ? std::string("material set: invalid engine.matgen.Set")
                : "material set: " + ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message;
    return false;
  }
  std::string why;
  if (!validate_set(out, why)) {
    error = "material set: " + why;
    return false;
  }
  return true;
}

std::string canonical_json(const Set& set) {
  return write_json(schema::to_json(set), {.pretty = false});
}

u64 set_key(const Set& set) {
  const std::string text = canonical_json(set);
  return hash_combine(hash_bytes(text.data(), text.size()), k_matgen_version);
}

namespace {

// One wrapped box pass of radius `r` along x or y over integers: exact, so the same wherever the
// image starts.
void box_pass(const Vector<i64>& in, Vector<i64>& out, u32 n, u32 r, bool along_x) {
  out.resize(n * n);
  for (u32 line = 0; line < n; ++line) {
    const auto at = [&](u32 k) -> i64 { return along_x ? in[line * n + k] : in[k * n + line]; };
    i64 sum = 0;
    for (u32 k = 0; k <= 2 * r; ++k)
      sum += at((k + n - r) % n);
    for (u32 k = 0; k < n; ++k) {
      (along_x ? out[line * n + k] : out[k * n + line]) = sum;
      sum += at((k + r + 1) % n) - at((k + n - r) % n);
    }
  }
}

u8 to_byte(f64 unit) noexcept {
  return static_cast<u8>(std::floor(detail::clamp01(unit) * 255.0 + 0.5));
}

}  // namespace

bool build_set(const Set& set, SetImages& out, SetStats* stats_out, std::string& error,
               Window window) {
  if (!validate_set(set, error)) return false;
  const u32 n = set.texels;
  detail::Field field;
  field.reset(n, static_cast<f64>(set.tile_mm) * 0.001, Window{window.x % n, window.y % n});
  SetStats stats;
  detail::Shade shade;
  switch (set.kind) {
    case Kind::granular: detail::build_granular(set, field, stats, shade); break;
    case Kind::rock: detail::build_rock(set, field, stats, shade); break;
    case Kind::wood: detail::build_wood(set, field, stats, shade); break;
    default: detail::build_ground(set, field, stats, shade); break;
  }
  const u32 count = n * n;
  const f64 texel = field.texel_m;

  // ---- hollows: how far a texel lies under the surface around it, its shade baked into the
  // colour. The resolve has no occlusion at this scale, and a bed of cobbles with every gap as
  // bright as the tops reads as a painted floor; a photographed set carries the same shade.
  const f64 unit = 1.0e7;  // a tenth of a micrometre
  Vector<i64> level, pass, blurred;
  level.resize(count);
  for (u32 i = 0; i < count; ++i)
    level[i] = static_cast<i64>(std::floor(static_cast<f64>(field.texels[i].height) * unit + 0.5));
  const u32 r = std::min({shade.cavity_px, 64u, (n - 1) / 2});
  box_pass(level, pass, n, r, true);
  box_pass(pass, blurred, n, r, false);
  box_pass(blurred, pass, n, r, true);
  box_pass(pass, blurred, n, r, false);
  const f64 window_texels = static_cast<f64>(2 * r + 1);
  const f64 weight = window_texels * window_texels * window_texels * window_texels;
  const f64 cavity = detail::permille(set.cavity_permille);
  const f64 wet = detail::permille(set.wet_permille);
  f64 lowest = 1.0e30, highest = -1.0e30;
  for (u32 i = 0; i < count; ++i) {
    detail::Texel& t = field.texels[i];
    const f64 around = static_cast<f64>(blurred[i]) / weight;
    const f64 hollow =
        detail::clamp01((around - static_cast<f64>(level[i])) / (shade.cavity_m * unit));
    // Wet: a surface under a film of water is darker (Angstrom's sand: 0.182 dry, 0.091 wet) and
    // its highlight is the film's.
    const f64 scale = (1.0 - cavity * hollow) * (1.0 - 0.5 * wet);
    t.r = static_cast<f32>(static_cast<f64>(t.r) * scale);
    t.g = static_cast<f32>(static_cast<f64>(t.g) * scale);
    t.b = static_cast<f32>(static_cast<f64>(t.b) * scale);
    const f64 rough = static_cast<f64>(t.rough);
    t.rough = static_cast<f32>(detail::mix(rough, 0.2 + 0.25 * rough, wet));
    stats.mean_color[0] += static_cast<f64>(t.r);
    stats.mean_color[1] += static_cast<f64>(t.g);
    stats.mean_color[2] += static_cast<f64>(t.b);
    stats.mean_roughness += static_cast<f64>(t.rough);
    lowest = std::min(lowest, static_cast<f64>(t.height));
    highest = std::max(highest, static_cast<f64>(t.height));
  }
  for (f64& c : stats.mean_color)
    c /= static_cast<f64>(count);
  stats.mean_roughness /= static_cast<f64>(count);

  // ---- the tile's self-similarity at its own period (matgen.md, "Repetition"): how far the
  // brightest or darkest sixteenth of the tile stands from the mean. What a repeat shows is its
  // landmarks; a block that stands out is one, seen again every tile.
  {
    constexpr u32 k_blocks = 16;
    const u32 side = n / k_blocks;
    f64 blocks[k_blocks * k_blocks] = {};
    f64 total = 0.0;
    for (u32 y = 0; y < n; ++y) {
      for (u32 x = 0; x < n; ++x) {
        const detail::Texel& t = field.texels[y * n + x];
        const f64 lum = 0.2126 * static_cast<f64>(t.r) + 0.7152 * static_cast<f64>(t.g) +
                        0.0722 * static_cast<f64>(t.b);
        blocks[(y / side) * k_blocks + x / side] += lum;
        total += lum;
      }
    }
    const f64 mean = total / static_cast<f64>(count);
    const f64 per_block = static_cast<f64>(side) * static_cast<f64>(side);
    f64 most = 0.0, squares = 0.0;
    for (const f64 b : blocks) {
      const f64 d = std::fabs(b / per_block - mean) / std::max(mean, 1.0e-9);
      most = std::max(most, d);
      squares += d * d;
    }
    stats.landmark_contrast = most;
    stats.block_contrast = std::sqrt(squares / static_cast<f64>(k_blocks * k_blocks));
  }
  // ---- what a standing eye reads (G187; matgen.md, "What a standing eye reads"): the RMS Weber
  // contrast of the luminance's block means over blocks of the power-of-two side nearest 5 cm and
  // 25 cm, and the height's RMS about its mean.
  {
    Vector<f64> lum;
    lum.resize(count);
    f64 total = 0.0, height_sum = 0.0;
    for (u32 i = 0; i < count; ++i) {
      const detail::Texel& t = field.texels[i];
      lum[i] = 0.2126 * static_cast<f64>(t.r) + 0.7152 * static_cast<f64>(t.g) +
               0.0722 * static_cast<f64>(t.b);
      total += lum[i];
      height_sum += static_cast<f64>(t.height);
    }
    const f64 mean = std::max(total / static_cast<f64>(count), 1.0e-9);
    const auto contrast_at = [&](f64 size_m) {
      u32 side = 1;
      while (side * 2 <= n && std::fabs(static_cast<f64>(side * 2) * texel - size_m) <
                                  std::fabs(static_cast<f64>(side) * texel - size_m))
        side *= 2;
      const u32 blocks = n / side;
      if (blocks < 2) return 0.0;
      f64 squares = 0.0;
      for (u32 by = 0; by < blocks; ++by) {
        for (u32 bx = 0; bx < blocks; ++bx) {
          f64 sum = 0.0;
          for (u32 y = by * side; y < (by + 1) * side; ++y)
            for (u32 x = bx * side; x < (bx + 1) * side; ++x)
              sum += lum[y * n + x];
          const f64 d = (sum / (static_cast<f64>(side) * static_cast<f64>(side)) - mean) / mean;
          squares += d * d;
        }
      }
      return std::sqrt(squares / (static_cast<f64>(blocks) * static_cast<f64>(blocks)));
    };
    stats.contrast_5cm = contrast_at(0.05);
    stats.contrast_25cm = contrast_at(0.25);
    const f64 height_mean = height_sum / static_cast<f64>(count);
    f64 squares = 0.0;
    for (u32 i = 0; i < count; ++i) {
      const f64 d = static_cast<f64>(field.texels[i].height) - height_mean;
      squares += d * d;
    }
    stats.relief_rms_mm = std::sqrt(squares / static_cast<f64>(count)) * 1000.0;
  }
  stats.height_min_m = lowest;
  stats.height_max_m = highest;

  // ---- the four images
  out.size = n;
  out.base_color.resize(count * 3);
  out.normal.resize(count * 3);
  out.roughness.resize(count);
  out.height.resize(count);
  const f64 span = highest > lowest ? highest - lowest : 1.0;
  for (u32 y = 0; y < n; ++y) {
    const u32 up = (y + n - 1) % n, down = (y + 1) % n;
    for (u32 x = 0; x < n; ++x) {
      const u32 i = y * n + x;
      const detail::Texel& t = field.texels[i];
      out.base_color[i * 3 + 0] = texture::linear_to_srgb8(t.r);
      out.base_color[i * 3 + 1] = texture::linear_to_srgb8(t.g);
      out.base_color[i * 3 + 2] = texture::linear_to_srgb8(t.b);
      out.roughness[i] = to_byte(static_cast<f64>(t.rough));
      out.height[i] = to_byte((static_cast<f64>(t.height) - lowest) / span);
      // The normal from the height's central differences, wrapped; +Y is up the image.
      const f64 left = static_cast<f64>(field.texels[y * n + (x + n - 1) % n].height);
      const f64 right = static_cast<f64>(field.texels[y * n + (x + 1) % n].height);
      const f64 above = static_cast<f64>(field.texels[up * n + x].height);
      const f64 below = static_cast<f64>(field.texels[down * n + x].height);
      const f64 nx = -(right - left) / (2.0 * texel);
      const f64 ny = (below - above) / (2.0 * texel);
      const f64 inv = 1.0 / std::sqrt(nx * nx + ny * ny + 1.0);
      out.normal[i * 3 + 0] = to_byte(0.5 + 0.5 * nx * inv);
      out.normal[i * 3 + 1] = to_byte(0.5 + 0.5 * ny * inv);
      out.normal[i * 3 + 2] = to_byte(0.5 + 0.5 * inv);
    }
  }
  if (stats_out != nullptr) *stats_out = stats;
  return true;
}

// ---- the defaults ------------------------------------------------------------------------------

namespace {

Constituent part(u32 share, u32 r, u32 g, u32 b, u32 tone, u32 roughness, u32 speckle = 0,
                 u32 vein = 0) {
  Constituent c;
  c.share_permille = share;
  c.r_permille = r;
  c.g_permille = g;
  c.b_permille = b;
  c.tone_permille = tone;
  c.roughness_permille = roughness;
  c.speckle_permille = speckle;
  c.vein_permille = vein;
  return c;
}

Set base(const char* role, Kind kind, u64 seed, u32 tile_mm) {
  Set s;
  s.role = role;
  s.kind = kind;
  s.seed = seed;
  s.tile_mm = tile_mm;
  return s;
}

// A mountain stream's bed (matgen.md, "The defaults"): a mix of the catchment's
// rocks, rounded, imbricated, and wet through: a channel's bed lies under its water.
// The tile is two metres (G153): a one-metre tile held a dozen clasts past D84, each a landmark
// seen again every metre; at two it holds four times as many and none stands alone. A stable bed
// carries a film on its tops, its lower clasts lie part buried in silt and sand (embeddedness, the
// 25 to 50 % class of Platts et al. 1983), and a few clasts are broken.
Set gravel_bed() {
  Set s = base("gravel/bed", Kind::granular, 0x6772617665ull, 2000);
  s.wet_permille = 1000;
  s.cavity_permille = 550;
  Granular& g = s.granular;
  g.cover_permille = 1000;
  g.matrix_um = 800;
  g.buried_permille = 300;
  g.film_permille = 550;
  g.broken_permille = 120;
  g.lithologies.push_back(part(450, 360, 350, 330, 150, 800, 250, 60));  // granite and gneiss
  g.lithologies.push_back(part(250, 520, 500, 460, 120, 850, 40, 150));  // limestone
  g.lithologies.push_back(part(150, 120, 120, 130, 200, 780, 60, 100));  // schist, basalt
  g.lithologies.push_back(part(100, 420, 360, 280, 150, 820, 80, 0));    // quartzite, sandstone
  g.lithologies.push_back(part(50, 640, 630, 610, 80, 700, 30, 0));      // vein quartz
  return s;
}

// A bar's sand with the granules and small pebbles left on it, dry.
Set sand_bar() {
  Set s = base("sand/bar", Kind::granular, 0x73616e64ull, 500);
  s.cavity_permille = 300;
  Granular& g = s.granular;
  g.d50_um = 8000;
  g.sorting_milliphi = 800;
  g.roundness_permille = 650;
  g.cover_permille = 120;
  g.imbrication_deg = 0;
  g.scatter_deg = 8;
  g.alignment_permille = 0;
  g.matrix_um = 400;
  g.lithologies.push_back(part(600, 480, 450, 390, 120, 900));  // quartz
  g.lithologies.push_back(part(200, 520, 470, 400, 120, 900));  // feldspar
  g.lithologies.push_back(part(120, 150, 140, 130, 150, 850));  // dark lithic grains
  g.lithologies.push_back(part(80, 90, 85, 80, 100, 800));      // heavy minerals
  return s;
}

// A talus: angular blocks fallen from one wall, openwork, lying any way round, three layers deep
// where a gap shows down, a few pockets of fines (a talus fills from the surface as it ages), on a
// three-metre tile (G153: a two-metre one held a dozen blocks past D84).
Set rock_scree() {
  Set s = base("rock/scree", Kind::granular, 0x7363726565ull, 3000);
  s.cavity_permille = 500;
  Granular& g = s.granular;
  g.d50_um = 110000;
  g.sorting_milliphi = 900;
  g.roundness_permille = 200;
  g.ba_min_permille = 500;
  g.cb_min_permille = 300;
  g.cb_max_permille = 800;
  g.cover_permille = 1000;
  g.imbrication_deg = 0;
  g.scatter_deg = 26;
  g.alignment_permille = 0;
  g.openwork_permille = 1000;
  g.matrix_um = 2000;
  g.fines_permille = 150;
  g.depth_layers = 3;
  g.broken_permille = 0;
  g.lithologies.push_back(part(700, 400, 390, 370, 180, 880, 80, 40));  // the wall's rock
  g.lithologies.push_back(part(300, 300, 290, 270, 150, 900, 60, 0));   // the same, weathered
  return s;
}

void granite_minerals(Rock& r) {
  r.minerals.push_back(part(280, 300, 300, 310, 100, 550));  // quartz
  r.minerals.push_back(part(300, 580, 500, 440, 100, 700));  // alkali feldspar
  r.minerals.push_back(part(320, 620, 610, 580, 80, 720));   // plagioclase
  r.minerals.push_back(part(100, 35, 33, 30, 100, 500));     // biotite
  // An old face: a fresh break is the exception. Its form above the decimetre (scallops, ribs, a
  // broken step) is chosen at 22 mm half range; its joints are worn gaps; the run-off below them
  // stains it; its lichens are colonies up to 120 mm across (Rhizocarpon's largest living thalli
  // run past 150 mm; Armstrong 1983's sample, 2 to 65 mm).
  r.rind_permille = 920;
  r.form_um = 22000;
  r.streak_permille = 500;
  r.lichen_max_mm = 120;
  r.lichen_permille = 160;
}

Set rock_granite() {
  Set s = base("rock/granite", Kind::rock, 0x6772616e697465ull, 1500);
  s.stochastic = true;
  granite_minerals(s.rock);
  return s;
}

// The same face wet through: darker rind, little lichen, moss in its joints, strong streaks.
Set rock_granite_wet() {
  Set s = base("rock/granite_wet", Kind::rock, 0x77657467ull, 1500);
  s.stochastic = true;
  s.wet_permille = 1000;
  granite_minerals(s.rock);
  s.rock.rind_r_permille = 520;
  s.rock.rind_g_permille = 510;
  s.rock.rind_b_permille = 500;
  s.rock.lichen_permille = 40;
  s.rock.streak_permille = 800;
  s.rock.moss_permille = 550;
  return s;
}

Set rock_basalt() {
  Set s = base("rock/basalt", Kind::rock, 0x626173616c74ull, 1500);
  s.stochastic = true;
  Rock& r = s.rock;
  r.grain_um = 600;
  r.minerals.push_back(part(850, 70, 70, 75, 150, 850));     // groundmass
  r.minerals.push_back(part(100, 220, 220, 210, 100, 750));  // plagioclase laths
  r.minerals.push_back(part(50, 110, 130, 60, 100, 700));    // olivine
  r.relief_um = 3000;
  r.joint_winding = 3;
  r.rind_permille = 850;
  r.rind_r_permille = 900;
  r.rind_g_permille = 800;
  r.rind_b_permille = 680;
  r.lichen_permille = 100;
  r.pits_permille = 60;
  r.form_um = 15000;
  r.streak_permille = 350;
  r.lichen_max_mm = 80;
  return s;
}

Set rock_limestone() {
  Set s = base("rock/limestone", Kind::rock, 0x6c696d65ull, 2000);
  s.stochastic = true;
  Rock& r = s.rock;
  r.grain_um = 300;
  r.minerals.push_back(part(900, 500, 490, 460, 60, 880));  // micrite
  r.minerals.push_back(part(100, 600, 590, 560, 60, 800));  // sparry calcite
  r.relief_um = 5000;
  r.rind_permille = 850;
  r.rind_r_permille = 700;
  r.rind_g_permille = 700;
  r.rind_b_permille = 690;
  r.lichen_permille = 150;
  r.pits_permille = 40;
  // Limestone's ink stripes (the Alps' "Tintenstriche": dark run-off streaks of cyanobacteria and
  // lichen where water runs down a face) are its best-known staining.
  r.form_um = 25000;
  r.streak_permille = 700;
  r.lichen_max_mm = 80;
  return s;
}

Set rock_sandstone() {
  Set s = base("rock/sandstone", Kind::rock, 0x73616e6473746full, 1500);
  s.stochastic = true;
  Rock& r = s.rock;
  r.grain_um = 500;
  r.minerals.push_back(part(800, 520, 440, 330, 150, 900));  // quartz grains, iron-stained
  r.minerals.push_back(part(120, 600, 500, 400, 100, 880));  // feldspar
  r.minerals.push_back(part(80, 150, 120, 100, 150, 850));   // lithic grains
  r.relief_um = 3000;
  r.joint_sets = 1;
  r.rind_permille = 700;
  r.rind_r_permille = 800;
  r.rind_g_permille = 720;
  r.rind_b_permille = 620;
  r.lichen_permille = 80;
  r.laminae = 12;
  r.form_um = 12000;
  r.streak_permille = 300;
  r.lichen_max_mm = 60;
  return s;
}

// Sawn softwood boards, weathered grey to brown (generated-material-sets.md, "Weathered wood"):
// flat-sawn figure of 8 mm bands, a spruce's latewood share, latewood ridges, short checks along
// the grain, two knots a square metre. Two metres at 2,048 texels: a millimetre a texel, and a
// deck's boards each show their own band of it.
Set wood_plank() {
  Set s = base("wood/plank", Kind::wood, 0x706c616e6bull, 2000);
  s.texels = 2048;
  s.cavity_permille = 400;
  Wood& w = s.wood;
  w.form = WoodForm::sawn;
  return s;
}

// The outside of a debarked log left to silver: broad bands where the surface cuts the rings, a
// spruce's spiral grain, long drying checks, knots, bark left on in patches.
Set wood_log() {
  Set s = base("wood/log", Kind::wood, 0x6c6f67ull, 2000);
  s.texels = 2048;
  s.cavity_permille = 500;
  Wood& w = s.wood;
  w.form = WoodForm::log;
  w.figure_um = 30000;
  w.latewood_permille = 250;
  w.wander_mm = 80;
  w.spiral_ddeg = 22;
  w.relief_um = 300;
  w.fresh_r_permille = 420;
  w.fresh_g_permille = 300;
  w.fresh_b_permille = 180;
  w.grey_r_permille = 200;
  w.grey_g_permille = 195;
  w.grey_b_permille = 185;
  w.grey_permille = 850;
  w.grey_patch_permille = 200;
  w.stain_permille = 400;
  w.checks_per_m2 = 8;
  w.check_min_mm = 150;
  w.check_max_mm = 900;
  w.check_width_um = 7000;
  w.check_depth_um = 15000;
  w.knots_per_m2 = 1;
  w.knot_min_mm = 25;
  w.knot_max_mm = 70;
  w.bark_permille = 150;
  w.bark_plate_mm = 45;
  return s;
}

// The floors (G187; matgen.md, "A floor at standing height"): two metres at 2,048 texels.
Set ground_of(const char* role, u64 seed, u32 moss, u32 soil, u32 thatch, u32 live) {
  Set s = base(role, Kind::ground, seed, 2000);
  s.texels = 2048;
  s.cavity_permille = 450;
  s.ground.moss_permille = moss;
  s.ground.soil_permille = soil;
  s.ground.thatch_permille = thatch;
  s.ground.live_permille = live;
  s.ground.mound_spacing_mm = 900;
  return s;
}

Set ground_moss() {
  Set s = ground_of("ground/moss", 0x6d6f7373ull, 1000, 0, 20, 0);
  Ground& g = s.ground;
  g.cushion_mm = 220;
  g.relief_um = 8000;
  g.mound_um = 25000;
  g.patch_permille = 500;
  g.decay_permille = 800;
  return s;
}

Set ground_soil() {
  Set s = ground_of("ground/soil", 0x736f696cull, 0, 1000, 30, 0);
  Ground& g = s.ground;
  g.clod_mm = 35;
  g.relief_um = 5000;
  g.mound_um = 12000;
  g.patch_permille = 500;
  g.decay_permille = 600;
  g.stones_permille = 80;
  return s;
}

Set ground_thatch() {
  Set s = ground_of("ground/thatch", 0x746861746368ull, 0, 1000, 970, 0);
  Ground& g = s.ground;
  g.mound_um = 15000;
  g.patch_permille = 600;
  g.decay_permille = 500;
  g.aligned_permille = 350;
  return s;
}

Set ground_turf() {
  Set s = ground_of("ground/turf", 0x74757266ull, 250, 750, 600, 800);
  s.cavity_permille = 550;
  Ground& g = s.ground;
  g.leaf_min_mm = 40;
  g.leaf_max_mm = 110;
  g.tuft_mm = 110;
  g.dead_permille = 200;
  g.clod_mm = 35;
  g.relief_um = 3000;
  g.mound_um = 20000;
  g.patch_permille = 600;
  g.decay_permille = 500;
  g.stones_permille = 10;
  return s;
}

}  // namespace

Set variant_of(const Set& set, u32 index) {
  Set v = set;
  v.role = set.role + "." + std::to_string(index);
  v.seed = detail::mix64(set.seed ^ (0x76617269616e74ull + index));
  return v;
}

Vector<Set> default_sets() {
  Vector<Set> sets;
  // A tiled granular role and its variants (G153); a stochastic rock set breaks its own repeat.
  const auto with_variants = [&sets](const Set& s) {
    sets.push_back(s);
    for (u32 k = 1; k <= k_tiled_variants; ++k)
      sets.push_back(variant_of(s, k));
  };
  with_variants(gravel_bed());
  sets.push_back(ground_moss());
  sets.push_back(ground_soil());
  sets.push_back(ground_thatch());
  sets.push_back(ground_turf());
  sets.push_back(rock_basalt());
  sets.push_back(rock_granite());
  sets.push_back(rock_granite_wet());
  sets.push_back(rock_limestone());
  sets.push_back(rock_sandstone());
  with_variants(rock_scree());
  with_variants(sand_bar());
  sets.push_back(wood_log());
  sets.push_back(wood_plank());
  return sets;
}

bool default_set(std::string_view role, Set& out) {
  const Vector<Set> sets = default_sets();
  for (const Set& s : sets) {
    if (s.role == role) {
      out = s;
      return true;
    }
  }
  return false;
}

}  // namespace engine::matgen
