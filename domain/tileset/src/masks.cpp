// The mask level (masks.h; docs/subsystems/tileset.md, "The mask level"). Every decision is
// integers or f64 polynomials and square roots — no libm call — so a level is the same bytes from
// every toolchain, as the heights are (determinism_tests.cpp pins it).
#include "archive.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>
#include <domain/tileset/masks.h>
#include <foundation/image/decode.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace engine::tileset {

namespace {

i64 floor_div(i64 a, i64 b) noexcept { return scene_gen::mask_floor_div(a, b); }

std::string hex(u64 v) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
  return std::string(text);
}

std::string resolve(std::string_view dir, const std::string& path) {
  return path.empty() || io::is_absolute_path(path) || dir.empty() ? path
                                                                   : io::join_path(dir, path);
}

// ---- deterministic arithmetic -------------------------------------------------------------------

constexpr f64 k_pi = 3.14159265358979323846;

// sin and cos by their series on [0, pi/2 + a little]: the angles a prior names. Polynomials only,
// so every toolchain computes the same thresholds (no libm in a decision).
f64 series_sin(f64 x) noexcept {
  f64 term = x, sum = x;
  for (u32 n = 1; n < 14; ++n) {
    term *= -x * x / static_cast<f64>((2 * n) * (2 * n + 1));
    sum += term;
  }
  return sum;
}
f64 series_cos(f64 x) noexcept {
  f64 term = 1, sum = 1;
  for (u32 n = 1; n < 14; ++n) {
    term *= -x * x / static_cast<f64>((2 * n - 1) * (2 * n));
    sum += term;
  }
  return sum;
}
// The slope gradient (rise over run) of an angle in degrees, clamped below vertical.
f64 slope_of_deg(f64 deg) noexcept {
  const f64 d = std::clamp(deg, 0.0, 89.5);
  const f64 r = d * (k_pi / 180.0);
  return series_sin(r) / series_cos(r);
}
f64 smoothstep(f64 lo, f64 hi, f64 v) noexcept {
  if (hi <= lo) return v >= hi ? 1.0 : 0.0;
  const f64 t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
u32 quantize(f64 w) noexcept {
  const f64 q = w * 255.0 + 0.5;
  return q <= 0.0 ? 0u : (q >= 255.0 ? 255u : static_cast<u32>(q));
}

// ---- the inputs, resolved for evaluation --------------------------------------------------------

struct Image {
  u32 w = 0, h = 0;
  Vector<u8> r;  // the R channel, rows top first: row 0 is the map frame's z0 edge
};

struct Region {
  Vector<f64> x, z;  // metres; empty for a circle
  f64 cx = 0, cz = 0, radius = 0;
  f64 min_x = 0, max_x = 0, min_z = 0, max_z = 0;  // the outline's box
  f64 reach = 0;                                   // how far outside it anything is laid
  i32 material = -1, base = -1, bed = -1, margin = -1;
  f64 feather = 0, margin_m = 0, wet_m = 0;
};

struct Field {
  const TileSet* set = nullptr;
  const MaskInputs* in = nullptr;
  u32 palette = 0;
  // priors
  bool priors = false;
  i32 soil = -1, scree = -1, bedrock = -1, snow = -1;
  f64 scree_lo = 0, scree_hi = 0, rock_lo = 0, rock_hi = 0, sluff_lo = 0, sluff_hi = 0;
  f64 snowline = 0, snow_blend = 0, snow_aspect = 0, pole_x = 0, pole_z = -1;
  Vector<Image> paints;
  Vector<i32> paint_layer;
  Image index;
  Vector<i32> index_layer;
  bool has_index = false;
  Vector<Region> regions;
};

i32 layer_of(const MaskInputs& in, const std::string& name) {
  if (name.empty()) return -1;
  for (u32 i = 0; i < in.palette.size(); ++i)
    if (in.palette[i] == name) return static_cast<i32>(i);
  return -2;
}

bool read_r(const std::string& path, Image& out, std::string& error) {
  image::Image decoded;
  std::string why;
  if (image::read_image(path, decoded, 4, &why) != io::Status::Ok) {
    error = "terrain.masks: the image '" + path + "' does not read: " + why;
    return false;
  }
  out.w = decoded.width;
  out.h = decoded.height;
  out.r.resize(static_cast<u32>(out.w) * out.h);
  for (u32 p = 0; p < out.r.size(); ++p)
    out.r[p] = decoded.pixels[p * 4];
  return true;
}

// The image at map-frame (u, v), texel centres at (i + 0.5) / w, the edge clamped: the GPU's
// bilinear filter over the same texels, in f64.
f64 bilinear(const Image& img, f64 u, f64 v) noexcept {
  const f64 fx = std::clamp(u * img.w - 0.5, 0.0, static_cast<f64>(img.w - 1));
  const f64 fz = std::clamp(v * img.h - 0.5, 0.0, static_cast<f64>(img.h - 1));
  const u32 x0 = static_cast<u32>(fx), z0 = static_cast<u32>(fz);
  const u32 x1 = std::min(x0 + 1, img.w - 1), z1 = std::min(z0 + 1, img.h - 1);
  const f64 ax = fx - x0, az = fz - z0;
  const auto at = [&](u32 x, u32 z) { return img.r[static_cast<u32>(z) * img.w + x] / 255.0; };
  return (at(x0, z0) * (1 - ax) + at(x1, z0) * ax) * (1 - az) +
         (at(x0, z1) * (1 - ax) + at(x1, z1) * ax) * az;
}
u32 nearest(const Image& img, f64 u, f64 v) noexcept {
  const i64 x = std::clamp<i64>(static_cast<i64>(u * img.w >= 0 ? u * img.w : -1), 0, img.w - 1);
  const i64 z = std::clamp<i64>(static_cast<i64>(v * img.h >= 0 ? v * img.h : -1), 0, img.h - 1);
  return img.r[static_cast<u32>(z) * img.w + static_cast<u32>(x)];
}

// Signed distance to the outline, metres: positive inside.
f64 signed_distance(const Region& r, f64 x, f64 z) noexcept {
  if (r.x.empty()) {
    const f64 dx = x - r.cx, dz = z - r.cz;
    return r.radius - std::sqrt(dx * dx + dz * dz);
  }
  f64 best = 1e300;
  bool inside = false;
  const u32 n = r.x.size();
  for (u32 i = 0, j = n - 1; i < n; j = i++) {
    const f64 ax = r.x[j], az = r.z[j], bx = r.x[i], bz = r.z[i];
    if (((bz > z) != (az > z)) && (x < (ax - bx) * (z - bz) / (az - bz) + bx)) inside = !inside;
    const f64 ex = bx - ax, ez = bz - az;
    const f64 len2 = ex * ex + ez * ez;
    const f64 t = len2 > 0 ? std::clamp(((x - ax) * ex + (z - az) * ez) / len2, 0.0, 1.0) : 0.0;
    const f64 px = ax + t * ex - x, pz = az + t * ez - z;
    best = std::min(best, px * px + pz * pz);
  }
  const f64 d = std::sqrt(best);
  return inside ? d : -d;
}

bool make_field(const TileSet& set, const MaskInputs& in, Field& f, std::string& error) {
  if (!validate_mask_inputs(in, error)) return false;
  f.set = &set;
  f.in = &in;
  f.palette = in.palette.size();
  const scene::TerrainMasks& m = in.masks;
  if (m.priors.has_value()) {
    const scene::TerrainMaskPriors& p = *m.priors;
    f.priors = true;
    f.soil = layer_of(in, p.soil);
    f.scree = layer_of(in, p.scree);
    f.bedrock = layer_of(in, p.bedrock);
    f.snow = layer_of(in, p.snow);
    const auto d = [](f32 v) { return static_cast<f64>(v); };
    const f64 b = d(p.slope_blend_deg);
    f.scree_lo = slope_of_deg(d(p.scree_deg) - b);
    f.scree_hi = slope_of_deg(d(p.scree_deg) + b);
    f.rock_lo = slope_of_deg(d(p.bedrock_deg) - b);
    f.rock_hi = slope_of_deg(d(p.bedrock_deg) + b);
    f.sluff_lo = slope_of_deg(d(p.snow_max_deg) - b);
    f.sluff_hi = slope_of_deg(d(p.snow_max_deg) + b);
    f.snowline = d(p.snowline_m);
    f.snow_blend = d(p.snow_blend_m);
    f.snow_aspect = d(p.snow_aspect_m);
    const f64 len = std::sqrt(d(p.pole_x) * d(p.pole_x) + d(p.pole_z) * d(p.pole_z));
    f.pole_x = d(p.pole_x) / len;
    f.pole_z = d(p.pole_z) / len;
  }
  for (const scene::TerrainMaskPaint& paint : m.paint) {
    Image img;
    if (!read_r(paint.image, img, error)) return false;
    f.paints.push_back(std::move(img));
    f.paint_layer.push_back(layer_of(in, paint.material));
  }
  if (m.index.has_value()) {
    if (!read_r(m.index->image, f.index, error)) return false;
    f.has_index = true;
    for (const std::string& name : m.index->materials)
      f.index_layer.push_back(layer_of(in, name));
  }
  for (const scene::TerrainMaskRegion& src : m.regions) {
    Region r;
    if (src.radius_m > 0) {
      r.cx = src.center.x;
      r.cz = src.center.z;
      r.radius = static_cast<f64>(src.radius_m);
      r.min_x = r.cx - r.radius;
      r.max_x = r.cx + r.radius;
      r.min_z = r.cz - r.radius;
      r.max_z = r.cz + r.radius;
    } else {
      r.min_x = r.min_z = 1e300;
      r.max_x = r.max_z = -1e300;
      for (const WorldPos& p : src.polygon) {
        r.x.push_back(p.x);
        r.z.push_back(p.z);
        r.min_x = std::min(r.min_x, p.x);
        r.max_x = std::max(r.max_x, p.x);
        r.min_z = std::min(r.min_z, p.z);
        r.max_z = std::max(r.max_z, p.z);
      }
    }
    r.material = layer_of(in, src.material);
    r.base = layer_of(in, src.base);
    r.bed = layer_of(in, src.bed);
    r.margin = layer_of(in, src.margin);
    r.feather = static_cast<f64>(src.feather_m);
    r.margin_m = r.margin >= 0 ? static_cast<f64>(src.margin_m) : 0.0;
    r.wet_m = static_cast<f64>(src.wet_m);
    r.reach = std::max(r.margin_m, r.wet_m);
    f.regions.push_back(std::move(r));
  }
  return true;
}

// Inward coverage: 1 at `feather` inside, 0 at the outline, a hard step with no feather.
f64 inside_share(f64 d, f64 feather) noexcept {
  if (feather <= 0) return d >= 0 ? 1.0 : 0.0;
  return smoothstep(0.0, feather, d);
}

void lay(Vector<f64>& w, i32 layer, f64 share) noexcept {
  if (layer < 0 || share <= 0) return;
  for (f64& v : w)
    v *= 1.0 - share;
  w[static_cast<u32>(layer)] += share;
}

// The composite at one point: priors, paints, the index, the regions, each over the last.
void evaluate(const Field& f, i64 x_mm, i64 z_mm, i64 step_mm, Vector<f64>& w, f64& wet,
              Vector<f64>& region_d) {
  w.assign(f.palette, 0.0);
  wet = 0.0;
  const f64 xm = static_cast<f64>(x_mm) / 1000.0;
  const f64 zm = static_cast<f64>(z_mm) / 1000.0;
  region_d.resize(f.regions.size());
  for (u32 i = 0; i < f.regions.size(); ++i) {
    const Region& r = f.regions[i];
    const f64 reach = r.reach + 1.0;
    region_d[i] =
        xm < r.min_x - reach || xm > r.max_x + reach || zm < r.min_z - reach || zm > r.max_z + reach
            ? -1e300
            : signed_distance(r, xm, zm);
  }
  if (f.priors) {
    const TileSet& set = *f.set;
    const f64 h = set.point_mm(x_mm, z_mm);
    const f64 gx = (set.point_mm(x_mm + step_mm, z_mm) - set.point_mm(x_mm - step_mm, z_mm)) /
                   static_cast<f64>(2 * step_mm);
    const f64 gz = (set.point_mm(x_mm, z_mm + step_mm) - set.point_mm(x_mm, z_mm - step_mm)) /
                   static_cast<f64>(2 * step_mm);
    const f64 g = std::sqrt(gx * gx + gz * gz);
    const f64 rock = smoothstep(f.rock_lo, f.rock_hi, g);
    const f64 scree = smoothstep(f.scree_lo, f.scree_hi, g) * (1.0 - rock);
    const f64 soil = 1.0 - rock - scree;
    // Facing the pole: the downhill direction (-gradient) along the pole's, fading out on the flat.
    const f64 facing = std::clamp(-(gx * f.pole_x + gz * f.pole_z) / std::max(g, 0.1), -1.0, 1.0);
    const f64 line = f.snowline - f.snow_aspect * facing;
    const f64 snow =
        f.snow < 0 ? 0.0
                   : smoothstep(line - 0.5 * f.snow_blend, line + 0.5 * f.snow_blend, h / 1000.0) *
                         (1.0 - smoothstep(f.sluff_lo, f.sluff_hi, g));
    // A region's `base` stands in for the soil inside it.
    f64 soil_left = soil;
    for (u32 i = 0; i < f.regions.size(); ++i) {
      const Region& r = f.regions[i];
      if (r.base < 0) continue;
      const f64 share = inside_share(region_d[i], r.feather);
      if (share <= 0) continue;
      w[static_cast<u32>(r.base)] += (1.0 - snow) * soil_left * share;
      soil_left *= 1.0 - share;
    }
    if (f.soil >= 0) w[static_cast<u32>(f.soil)] += (1.0 - snow) * soil_left;
    if (f.scree >= 0) w[static_cast<u32>(f.scree)] += (1.0 - snow) * scree;
    if (f.bedrock >= 0) w[static_cast<u32>(f.bedrock)] += (1.0 - snow) * rock;
    if (f.snow >= 0) w[static_cast<u32>(f.snow)] += snow;
  }
  const MaskInputs& in = *f.in;
  const f64 u = static_cast<f64>(x_mm - in.map_x0_mm) / static_cast<f64>(in.map_size_mm);
  const f64 v = static_cast<f64>(z_mm - in.map_z0_mm) / static_cast<f64>(in.map_size_mm);
  for (u32 i = 0; i < f.paints.size(); ++i)
    lay(w, f.paint_layer[i], bilinear(f.paints[i], u, v));
  if (f.has_index) {
    const u32 code = nearest(f.index, u, v);
    if (code < f.index_layer.size()) lay(w, f.index_layer[code], 1.0);
  }
  for (u32 i = 0; i < f.regions.size(); ++i) {
    const Region& r = f.regions[i];
    const f64 d = region_d[i];
    if (d <= -1e299) continue;
    if (r.margin >= 0 && r.margin_m > 0) lay(w, r.margin, smoothstep(-r.margin_m, 0.0, d));
    if (r.bed >= 0) lay(w, r.bed, inside_share(d, r.feather));
    if (r.material >= 0) lay(w, r.material, inside_share(d, r.feather));
    if (r.wet_m > 0) wet = std::max(wet, d >= 0 ? 1.0 : std::clamp(1.0 + d / r.wet_m, 0.0, 1.0));
  }
}

// A tile's record in the level's archive (archive.h), byte for byte what its own `.mask` file held
// before the archive (k_mask_version 1). Its version is the record's own, and moves only when a
// record's layout does.
inline constexpr u32 k_mask_record_version = 1;
struct MaskHeader {
  char magic[4] = {'E', 'M', 'S', 'K'};
  u32 version = k_mask_record_version;
  u32 cells = 0;
  u32 words = 0;
  i64 spacing_mm = 0;
  i32 tx = 0;
  i32 tz = 0;
};
static_assert(sizeof(MaskHeader) == 32);

using archive::tile_key;

u32 record_bytes(u32 cells) noexcept {
  return static_cast<u32>(sizeof(MaskHeader)) + scene_gen::mask_tile_words(cells) * 4;
}

// The mips of one channel (or the wetness): mip m's point is mip 0's filtered over the points its
// cell spans — weights 1, 2, ..., 2, 1 (the box over the cell, as the height chain's) — except
// along an axis where it lies on the tile's edge, where it is mip 0's edge point alone, so both
// tiles holding an edge compute it from the points they share. Integers, rounded half up.
void fill_mips(u32 cells, Vector<u32>& points) {
  const u32 n0 = cells + 1;
  const u32 mips = scene_gen::mask_mips(cells);
  for (u32 m = 1; m < mips; ++m) {
    const u32 side = scene_gen::mask_mip_side(cells, m);
    const u32 base = scene_gen::mask_mip_offset(cells, m);
    const i64 r = i64{1} << m;
    const i64 half = r / 2;
    for (u32 b = 0; b < side; ++b) {
      for (u32 a = 0; a < side; ++a) {
        const bool edge_x = a == 0 || a == side - 1;
        const bool edge_z = b == 0 || b == side - 1;
        const i64 cx = static_cast<i64>(a) * r, cz = static_cast<i64>(b) * r;
        const i64 hx = edge_x ? 0 : half, hz = edge_z ? 0 : half;
        u64 sum = 0, den = 0;
        for (i64 dz = -hz; dz <= hz; ++dz) {
          const u64 wz = hz == 0 ? 1 : (dz == -hz || dz == hz ? 1 : 2);
          for (i64 dx = -hx; dx <= hx; ++dx) {
            const u64 wx = hx == 0 ? 1 : (dx == -hx || dx == hx ? 1 : 2);
            sum += wx * wz * points[static_cast<u32>((cz + dz) * n0 + (cx + dx))];
            den += wx * wz;
          }
        }
        points[base + b * side + a] = static_cast<u32>((2 * sum + den) / (2 * den));
      }
    }
  }
}

}  // namespace

bool mask_inputs(const scene::Terrain& entry, std::string_view dir, MaskInputs& out,
                 std::string& error) {
  out = MaskInputs{};
  if (!entry.masks.has_value()) {
    error = "terrain.masks: the terrain has no masks block";
    return false;
  }
  if (!entry.materials.has_value()) {
    error = "terrain.masks: masks weigh the palette of terrain.materials, which the terrain lacks";
    return false;
  }
  out.masks = *entry.masks;
  for (auto& paint : out.masks.paint)
    paint.image = resolve(dir, paint.image);
  if (out.masks.index.has_value()) out.masks.index->image = resolve(dir, out.masks.index->image);
  for (const auto& layer : entry.materials->layers)
    out.palette.push_back(layer.name);
  out.budget = entry.materials->layer_budget;
  const i64 extent = static_cast<i64>(static_cast<f64>(entry.extent) * 1000.0 + 0.5);
  out.map_x0_mm = -extent;
  out.map_z0_mm = -extent;
  out.map_size_mm = 2 * extent;
  return true;
}

bool validate_mask_inputs(const MaskInputs& in, std::string& error) {
  const auto fail = [&](const std::string& why) {
    error = "terrain.masks: " + why;
    return false;
  };
  const scene::TerrainMasks& m = in.masks;
  if (m.spacing_mm < 250 || m.spacing_mm > 4000)
    return fail("spacing_mm must be 250 to 4000 (a quarter metre to four metres)");
  if (m.tile_cells < 4 || m.tile_cells > 256 || (m.tile_cells & (m.tile_cells - 1)) != 0)
    return fail("tile_cells must be a power of two from 4 to 256, so every mip halves a tile");
  if (in.budget < 1 || in.budget > scene_gen::k_mask_channels)
    return fail(
        "materials.layer_budget is the per-tile budget with masks and must be 1 to 4 (a mask texel "
        "carries four weights); set it");
  if (in.palette.empty() || in.palette.size() > scene_gen::k_mask_max_palette)
    return fail("the palette (materials.layers) must hold 1 to 254 layers");
  for (u32 i = 0; i < in.palette.size(); ++i) {
    if (in.palette[i].empty()) return fail("every palette layer needs a name the masks can use");
    for (u32 j = 0; j < i; ++j)
      if (in.palette[j] == in.palette[i])
        return fail("the palette names \"" + in.palette[i] + "\" twice");
  }
  if (in.map_size_mm <= 0) return fail("the terrain's map frame (its extent) is empty");
  const auto known = [&](const std::string& name, const std::string& where) {
    if (layer_of(in, name) == -2)
      return fail(where + " names \"" + name + "\", which the palette does not have");
    return true;
  };
  const auto unit = [](f32 v) { return v >= 0 && v <= 1; };
  if (!unit(m.wetness) || !unit(m.height_blend))
    return fail("wetness and height_blend must be in [0, 1]");
  if (m.resident_mib < 1 || m.resident_mib > 65536) return fail("resident_mib must be 1 to 65536");
  if (m.priors.has_value()) {
    const scene::TerrainMaskPriors& p = *m.priors;
    for (const auto* name : {&p.soil, &p.scree, &p.bedrock, &p.snow})
      if (!known(*name, "priors")) return false;
    if (!(p.slope_blend_deg >= 0 && p.slope_blend_deg <= 15 && p.scree_deg > 0 &&
          p.bedrock_deg > p.scree_deg && p.bedrock_deg < 90 && p.snow_max_deg > 0 &&
          p.snow_max_deg <= 90))
      return fail(
          "priors: slope_blend_deg must be 0 to 15 and 0 < scree_deg < bedrock_deg < 90, "
          "0 < snow_max_deg <= 90");
    if (!(p.snow_blend_m >= 0 && p.snow_aspect_m >= 0 && p.snowline_m == p.snowline_m &&
          p.snowline_m < 1e7f))
      return fail("priors: the snowline must be finite and its blend and aspect not negative");
    if (!(p.pole_x * p.pole_x + p.pole_z * p.pole_z > 1e-6f))
      return fail("priors: the pole's direction must not be zero");
  }
  for (const scene::TerrainMaskPaint& paint : m.paint) {
    if (!known(paint.material, "a paint")) return false;
    if (paint.material.empty()) return fail("a paint must name its material");
    if (!io::exists(paint.image)) return fail("the paint image '" + paint.image + "' is missing");
  }
  if (m.index.has_value()) {
    for (const std::string& name : m.index->materials)
      if (!known(name, "the index map")) return false;
    if (!io::exists(m.index->image))
      return fail("the index image '" + m.index->image + "' is missing");
  }
  for (const scene::TerrainMaskRegion& r : m.regions) {
    const std::string where = "region \"" + r.name + "\"";
    if (!r.water_body.empty())
      return fail(where + " still names a water body; the host resolves it before a build");
    const bool circle = r.radius_m > 0;
    if (circle == (r.polygon.size() >= 3) ||
        (!circle && !r.polygon.empty() && r.polygon.size() < 3))
      return fail(where + " needs one outline: a polygon of 3 or more points, or a radius");
    if (!r.material.empty() && !r.base.empty())
      return fail(where +
                  " gives both material and base; one replaces everything, the other "
                  "the soil alone");
    for (const auto* name : {&r.material, &r.base, &r.bed, &r.margin})
      if (!known(*name, where)) return false;
    if (!(r.feather_m >= 0 && r.feather_m <= 1000 && r.margin_m >= 0 && r.margin_m <= 1000 &&
          r.wet_m >= 0 && r.wet_m <= 1000))
      return fail(where + ": feather_m, margin_m and wet_m must be 0 to 1000 metres");
  }
  return true;
}

bool mask_key(const TileSetManifest& set, const MaskInputs& in, u64& out, std::string& error) {
  MaskInputs identity = in;
  for (auto& paint : identity.masks.paint)
    paint.image.clear();
  if (identity.masks.index.has_value()) identity.masks.index->image.clear();
  // The resident budget is the renderer's, not the level's: it changes no byte.
  identity.masks.resident_mib = 0;
  const std::string text = write_json(schema::to_json(identity.masks));
  u64 h = hash_combine(k_hash_seed, k_mask_version);
  h = hash_combine(h, hash_bytes(set.hash.data(), set.hash.size()));
  h = hash_combine(h, hash_bytes(text.data(), text.size()));
  for (const std::string& name : in.palette)
    h = hash_combine(h, hash_bytes(name.data(), name.size()));
  h = hash_combine(h, in.budget);
  h = hash_combine(h, static_cast<u64>(in.map_x0_mm));
  h = hash_combine(h, static_cast<u64>(in.map_z0_mm));
  h = hash_combine(h, static_cast<u64>(in.map_size_mm));
  Vector<std::string> images;
  for (const auto& paint : in.masks.paint)
    images.push_back(paint.image);
  if (in.masks.index.has_value()) images.push_back(in.masks.index->image);
  for (const std::string& path : images) {
    std::string bytes;
    if (io::read_file(path, bytes) != io::Status::Ok) {
      error = "terrain.masks: the image '" + path + "' could not be read";
      return false;
    }
    h = hash_combine(h, hash_bytes(bytes.data(), bytes.size()));
  }
  out = h;
  return true;
}

std::string mask_cache_dir(std::string_view ddc, u64 key) {
  return io::join_path(io::join_path(ddc, "tilemasks"), hex(key));
}

std::string mask_archive(i64 spacing_mm) { return std::to_string(spacing_mm) + ".masks"; }

bool mask_point(const TileSet& set, const MaskInputs& in, i64 x_mm, i64 z_mm, Vector<f64>& weights,
                f64& wetness, std::string& error) {
  Field f;
  if (!make_field(set, in, f, error)) return false;
  Vector<f64> scratch;
  evaluate(f, x_mm, z_mm, in.masks.spacing_mm, weights, wetness, scratch);
  return true;
}

bool build_mask_level(const TileSet& set, const MaskInputs& in, const std::string& dir,
                      MaskBuildResult& out, std::string& error) {
  out = MaskBuildResult{};
  Field f;
  if (!make_field(set, in, f, error)) return false;
  if (!mask_key(set.manifest(), in, out.key, error)) return false;
  const TileSetManifest& sm = set.manifest();
  const i64 s = in.masks.spacing_mm;
  const u32 c = in.masks.tile_cells;
  // The tiles over the heightfield's data: what the priors read and the ground draws.
  const i64 x0 = sm.origin_mm[0], z0 = sm.origin_mm[1];
  const i64 x1 = x0 + static_cast<i64>(sm.width - 1) * sm.spacing_mm;
  const i64 z1 = z0 + static_cast<i64>(sm.height - 1) * sm.spacing_mm;
  const i32 tx0 = static_cast<i32>(floor_div(floor_div(x0, s), c));
  const i32 tz0 = static_cast<i32>(floor_div(floor_div(z0, s), c));
  const i32 tx1 = static_cast<i32>(floor_div(floor_div(x1, s), c));
  const i32 tz1 = static_cast<i32>(floor_div(floor_div(z1, s), c));
  if (io::make_directories(dir) != io::Status::Ok && !io::exists(dir)) {
    error = "'" + dir + "' could not be made";
    return false;
  }
  MaskLevelManifest manifest;
  manifest.format = std::string(k_mask_format);
  manifest.version = k_mask_version;
  manifest.key = hex(out.key);
  manifest.set_hash = sm.hash;
  manifest.spacing_mm = s;
  manifest.cells = c;
  manifest.tile_min = {tx0, tz0};
  manifest.tile_max = {tx1, tz1};
  for (const std::string& name : in.palette)
    manifest.palette.push_back(name);
  manifest.budget = in.budget;
  manifest.wetness = in.masks.wetness;
  manifest.height_blend = in.masks.height_blend;
  manifest.resident_mib = in.masks.resident_mib;

  const u32 n0 = c + 1;
  const u32 points = scene_gen::mask_points(c);
  const u32 words = scene_gen::mask_tile_words(c);
  Vector<f64> dense(static_cast<u32>(n0) * n0 * f.palette);
  Vector<f64> wet(static_cast<u32>(n0) * n0);
  Vector<f64> w;
  Vector<f64> scratch;
  Vector<u32> channel[scene_gen::k_mask_channels];
  Vector<u32> wet_points;
  Vector<u32> tile_words(words);
  std::string bytes;
  u64 level_hash = hash_combine(k_hash_seed, out.key);
  archive::Writer writer("EMSK", s, record_bytes(c));
  for (i32 tz = tz0; tz <= tz1; ++tz) {
    for (i32 tx = tx0; tx <= tx1; ++tx) {
      bool any_wet = false;
      Vector<u32> used(f.palette, 0u);
      for (u32 b = 0; b < n0; ++b) {
        for (u32 a = 0; a < n0; ++a) {
          const i64 xm = (static_cast<i64>(tx) * c + a) * s;
          const i64 zm = (static_cast<i64>(tz) * c + b) * s;
          f64 point_wet = 0;
          evaluate(f, xm, zm, s, w, point_wet, scratch);
          const u32 p = b * n0 + a;
          for (u32 k = 0; k < f.palette; ++k) {
            dense[static_cast<u32>(p) * f.palette + k] = w[k];
            if (quantize(w[k]) != 0) used[k] = 1;
          }
          wet[p] = point_wet;
          if (quantize(point_wet) != 0) any_wet = true;
        }
      }
      Vector<u32> table;
      for (u32 k = 0; k < f.palette; ++k)
        if (used[k] != 0) table.push_back(k);
      if (table.empty() && !any_wet) continue;
      if (table.size() > in.budget) {
        std::string names;
        for (const u32 k : table)
          names += (names.empty() ? "" : ", ") + in.palette[k];
        char place[160];
        std::snprintf(place, sizeof(place), "x %.1f to %.1f m, z %.1f to %.1f m",
                      static_cast<f64>(static_cast<i64>(tx) * c * s) / 1000.0,
                      static_cast<f64>((static_cast<i64>(tx) + 1) * c * s) / 1000.0,
                      static_cast<f64>(static_cast<i64>(tz) * c * s) / 1000.0,
                      static_cast<f64>((static_cast<i64>(tz) + 1) * c * s) / 1000.0);
        error = "terrain.masks: tile (" + std::to_string(tx) + ", " + std::to_string(tz) + ") at " +
                place + " names " + std::to_string(table.size()) + " layers (" + names +
                ") where materials.layer_budget is " + std::to_string(in.budget) +
                "; narrow a region's feather or margin, or a prior's blend, so fewer meet there, "
                "or raise the budget";
        return false;
      }
      std::fill(tile_words.begin(), tile_words.end(), 0u);
      u32 packed = 0xFFFFFFFFu;
      for (u32 ch = 0; ch < table.size(); ++ch)
        packed = (packed & ~(0xFFu << (8 * ch))) | (table[ch] << (8 * ch));
      tile_words[0] = packed;
      for (u32 ch = 0; ch < scene_gen::k_mask_channels; ++ch) {
        channel[ch].assign(points, 0u);
        if (ch >= table.size()) continue;
        for (u32 p = 0; p < n0 * n0; ++p)
          channel[ch][p] = quantize(dense[static_cast<u32>(p) * f.palette + table[ch]]);
        fill_mips(c, channel[ch]);
      }
      wet_points.assign(points, 0u);
      for (u32 p = 0; p < n0 * n0; ++p)
        wet_points[p] = quantize(wet[p]);
      fill_mips(c, wet_points);
      for (u32 p = 0; p < points; ++p) {
        tile_words[1 + p] =
            channel[0][p] | (channel[1][p] << 8) | (channel[2][p] << 16) | (channel[3][p] << 24);
        tile_words[1 + points + p / 4] |= wet_points[p] << (8 * (p % 4));
      }
      MaskHeader header;
      header.cells = c;
      header.words = words;
      header.spacing_mm = s;
      header.tx = tx;
      header.tz = tz;
      bytes.assign(sizeof(header) + static_cast<u32>(words) * 4, '\0');
      std::memcpy(bytes.data(), &header, sizeof(header));
      std::memcpy(bytes.data() + sizeof(header), tile_words.data(), static_cast<u32>(words) * 4);
      if (!writer.add(tx, tz, bytes)) {
        error = "terrain.masks: the tile (" + std::to_string(tx) + ", " + std::to_string(tz) +
                ") is out of the archive's order";
        return false;
      }
      level_hash = hash_combine(level_hash, hash_bytes(bytes.data(), bytes.size()));
      manifest.present.push_back(tx);
      manifest.present.push_back(tz);
      out.bytes += bytes.size();
      ++out.tiles;
    }
  }
  // The level as one file: the present tiles' records in the order written, and their table.
  manifest.archive = mask_archive(s);
  if (!writer.write(io::join_path(dir, manifest.archive), manifest.archive_bytes, error))
    return false;
  out.hash = level_hash;
  manifest.bytes = out.bytes;
  manifest.hash = hex(level_hash);
  const std::string text = write_json(schema::to_json(manifest));
  out.manifest = io::join_path(dir, k_manifest_name);
  if (io::write_file_atomic(out.manifest, text) != io::Status::Ok) {
    error = "'" + out.manifest + "' could not be written";
    return false;
  }
  return true;
}

bool ensure_mask_level(const TileSet& set, const MaskInputs& in, const std::string& dir,
                       const std::string& ddc, bool force, MaskBuildResult& out,
                       std::string& error) {
  out = MaskBuildResult{};
  u64 key = 0;
  if (!validate_mask_inputs(in, error) || !mask_key(set.manifest(), in, key, error)) return false;
  const std::string target = dir.empty() ? mask_cache_dir(ddc, key) : dir;
  const std::string manifest_path = io::join_path(target, k_manifest_name);
  if (!force && io::exists(manifest_path)) {
    MaskLevel level;
    std::string why;
    if (level.open(manifest_path, why) && level.manifest().key == hex(key)) {
      out.manifest = manifest_path;
      out.key = key;
      out.hit = true;
      out.tiles = level.manifest().present.size() / 2;
      out.bytes = level.manifest().bytes;
      u64 h = 0;
      for (const char ch : level.manifest().hash)
        h = (h << 4) | static_cast<u64>(ch <= '9' ? ch - '0' : ch - 'a' + 10);
      out.hash = h;
      return true;
    }
  }
  return build_mask_level(set, in, target, out, error);
}

bool write_mask_priors(const TileSet& set, const MaskInputs& in, const std::string& dir,
                       Vector<std::string>& written, std::string& error) {
  written.clear();
  Field f;
  if (!make_field(set, in, f, error)) return false;
  if (!f.priors) {
    error = "terrain.masks: there are no priors to write; the block has no priors";
    return false;
  }
  // Priors alone: the field with no paints, index or regions over it.
  MaskInputs alone = in;
  alone.masks.paint.clear();
  alone.masks.index.reset();
  alone.masks.regions.clear();
  Field p;
  if (!make_field(set, alone, p, error)) return false;
  const i64 s = in.masks.spacing_mm;
  const i64 side64 = in.map_size_mm / s;
  if (side64 < 1 || side64 > 16384) {
    error = "terrain.masks: the map frame is " + std::to_string(side64) +
            " mask texels a side; priors are written up to 16384";
    return false;
  }
  const u32 side = static_cast<u32>(side64);
  if (io::make_directories(dir) != io::Status::Ok && !io::exists(dir)) {
    error = "'" + dir + "' could not be made";
    return false;
  }
  const struct {
    i32 layer;
    const char* role;
  } roles[] = {{p.soil, "soil"}, {p.scree, "scree"}, {p.bedrock, "bedrock"}, {p.snow, "snow"}};
  Vector<f64> w;
  Vector<f64> scratch;
  Vector<u8> pixels;
  for (const auto& role : roles) {
    if (role.layer < 0) continue;
    pixels.assign(static_cast<u32>(side) * side * 4, 255);
    for (u32 z = 0; z < side; ++z) {
      for (u32 x = 0; x < side; ++x) {
        // The texel's centre in the map frame, at whole millimetres.
        const i64 xm = in.map_x0_mm + (2 * static_cast<i64>(x) + 1) * in.map_size_mm / (2 * side);
        const i64 zm = in.map_z0_mm + (2 * static_cast<i64>(z) + 1) * in.map_size_mm / (2 * side);
        f64 wet = 0;
        evaluate(p, xm, zm, s, w, wet, scratch);
        const u8 v = static_cast<u8>(quantize(w[static_cast<u32>(role.layer)]));
        const u32 at = (static_cast<u32>(z) * side + x) * 4;
        pixels[at] = pixels[at + 1] = pixels[at + 2] = v;
      }
    }
    const std::string path = io::join_path(dir, std::string(role.role) + ".png");
    if (image::write_png(path, side, side, 4, {pixels.data(), pixels.size()}) != io::Status::Ok) {
      error = "'" + path + "' could not be written";
      return false;
    }
    written.push_back(path);
  }
  return true;
}

// ---- the reader ---------------------------------------------------------------------------------

// The archive and the read-ahead window (masks.h): the records from `first` onwards in file order,
// `count` of them, each `ok` when it arrived whole.
struct MaskLevel::Reading {
  archive::Reader archive;
  std::mutex mutex;
  Vector<u8> window;
  Vector<u8> ok;
  u32 first = 0;
  u32 count = 0;
  ReadStats stats;
};

namespace {
// The window: about 2 MiB, read as runs of 256 KiB, eight in flight.
constexpr u64 k_mask_window_bytes = 2u << 20;
}  // namespace

MaskLevel::MaskLevel() : reading_(std::make_unique<Reading>()) {}
MaskLevel::~MaskLevel() = default;
MaskLevel::MaskLevel(MaskLevel&&) noexcept = default;
MaskLevel& MaskLevel::operator=(MaskLevel&&) noexcept = default;

MaskLevel::ReadStats MaskLevel::read_stats() const noexcept {
  if (reading_ == nullptr) return ReadStats{};
  const std::lock_guard<std::mutex> lock(reading_->mutex);
  return reading_->stats;
}

bool MaskLevel::open(const std::string& manifest_path, std::string& error, bool unbuffered) {
  info_ = scene_gen::GroundMaskInfo{};
  present_.clear();
  if (reading_ == nullptr) reading_ = std::make_unique<Reading>();
  reading_->archive.close();
  reading_->count = 0;
  reading_->stats = ReadStats{};
  std::string bytes;
  if (io::read_file(manifest_path, bytes) != io::Status::Ok) {
    error = "'" + manifest_path + "' could not be read";
    return false;
  }
  JsonValue json;
  if (!parse_json(bytes, json).ok) {
    error = "'" + manifest_path + "' is not JSON";
    return false;
  }
  schema::ReadContext ctx;
  manifest_ = MaskLevelManifest{};
  if (!schema::from_json(manifest_, json, ctx) || !ctx.ok() || manifest_.format != k_mask_format ||
      manifest_.version != k_mask_version || manifest_.cells < 4 || manifest_.cells > 256 ||
      (manifest_.cells & (manifest_.cells - 1)) != 0 || manifest_.spacing_mm < 1 ||
      manifest_.present.size() % 2 != 0) {
    error = "'" + manifest_path + "' is not a mask level this build reads (" +
            std::string(k_mask_format) + " version " + std::to_string(k_mask_version) + ")";
    return false;
  }
  dir_ = std::string(io::parent_path(manifest_path));
  info_.spacing_mm = manifest_.spacing_mm;
  info_.cells = manifest_.cells;
  info_.tile_min_x = manifest_.tile_min[0];
  info_.tile_min_z = manifest_.tile_min[1];
  info_.tile_max_x = manifest_.tile_max[0];
  info_.tile_max_z = manifest_.tile_max[1];
  info_.palette = manifest_.palette.size();
  info_.budget = manifest_.budget;
  info_.wetness = manifest_.wetness;
  info_.height_blend = manifest_.height_blend;
  info_.resident_mib = manifest_.resident_mib;
  u64 h = hash_combine(k_hash_seed, hash_bytes(manifest_.key.data(), manifest_.key.size()));
  info_.hash = hash_combine(h, hash_bytes(manifest_.hash.data(), manifest_.hash.size()));
  for (u32 i = 0; i < manifest_.present.size(); i += 2)
    present_.push_back(tile_key(manifest_.present[i], manifest_.present[i + 1]));
  std::sort(present_.begin(), present_.end());
  // The archive, opened once, its table read: at most the tiles the manifest lists.
  std::string why;
  if (manifest_.archive.empty() ||
      !reading_->archive.open(io::join_path(dir_, manifest_.archive), "EMSK", manifest_.spacing_mm,
                              record_bytes(manifest_.cells), manifest_.present.size() / 2,
                              unbuffered ? io::ReadMode::unbuffered : io::ReadMode::buffered,
                              why)) {
    error = "'" + manifest_path + "' has no archive it can read" +
            (why.empty() ? std::string() : ": " + why);
    info_ = scene_gen::GroundMaskInfo{};
    return false;
  }
  return true;
}

bool MaskLevel::present(i32 tx, i32 tz) const noexcept {
  return std::binary_search(present_.begin(), present_.end(), tile_key(tx, tz));
}

bool MaskLevel::tile(i32 tx, i32 tz, std::span<u32> words) const noexcept {
  const u32 n = scene_gen::mask_tile_words(info_.cells);
  if (!present(tx, tz) || words.size() < n || reading_ == nullptr) return false;
  Reading& r = *reading_;
  const u32 at = r.archive.find(tx, tz);
  if (at == archive::Reader::k_absent) return false;
  const u32 rb = record_bytes(info_.cells);
  const std::lock_guard<std::mutex> lock(r.mutex);
  if (at < r.first || at >= r.first + r.count) {
    // Not in the window: fill it from this tile onwards, the records' destinations contiguous so
    // each run is read straight into place.
    const u32 table = static_cast<u32>(r.archive.table().size());
    const u32 count =
        std::min<u32>(table - at, std::max<u32>(1, static_cast<u32>(k_mask_window_bytes / rb)));
    r.window.resize(count * rb);
    r.ok.assign(count, u8{0});
    Vector<archive::Fetch> fetches;
    fetches.reserve(count);
    for (u32 k = 0; k < count; ++k)
      fetches.push_back(archive::Fetch{at + k, r.window.data() + u64{k} * rb, false});
    archive::FetchOptions options;
    options.depth = 8;
    options.run_bytes = 256 * 1024;
    archive::FetchStats stats;
    r.archive.fetch(std::span<archive::Fetch>(fetches.data(), fetches.size()), options, nullptr,
                    nullptr, &stats);
    for (u32 k = 0; k < count; ++k)
      r.ok[k] = fetches[k].ok ? 1 : 0;
    r.first = at;
    r.count = count;
    ++r.stats.windows;
    r.stats.bytes += stats.bytes;
  }
  if (r.ok[at - r.first] == 0) return false;
  const u8* bytes = r.window.data() + u64{at - r.first} * rb;
  MaskHeader header;
  std::memcpy(&header, bytes, sizeof(header));
  if (std::memcmp(header.magic, "EMSK", 4) != 0 || header.version != k_mask_record_version ||
      header.cells != info_.cells || header.words != n || header.tx != tx || header.tz != tz ||
      header.spacing_mm != info_.spacing_mm)
    return false;
  std::memcpy(words.data(), bytes + sizeof(header), static_cast<u32>(n) * 4);
  return true;
}

bool MaskLevel::sample(i64 x_mm, f64 x_frac, i64 z_mm, f64 z_frac, f32 level, Vector<f32>& weights,
                       f32& wetness) const {
  weights.assign(info_.palette, 0.0f);
  wetness = 0;
  const scene_gen::MaskTileRef at = scene_gen::mask_tile_of(info_, x_mm, x_frac, z_mm, z_frac);
  Vector<u32> words(scene_gen::mask_tile_words(info_.cells));
  if (!tile(at.tx, at.tz, {words.data(), words.size()})) return false;
  const scene_gen::MaskSample s =
      scene_gen::mask_sample(info_, {words.data(), words.size()}, at, level);
  for (u32 ch = 0; ch < scene_gen::k_mask_channels; ++ch) {
    const u32 layer = (words[0] >> (8 * ch)) & 0xFFu;
    if (layer < info_.palette) weights[layer] += s.weight[ch];
  }
  wetness = s.wetness;
  return true;
}

namespace {
bool level_info(const void* state, scene_gen::GroundMaskInfo& out) noexcept {
  const MaskLevel& level = *static_cast<const MaskLevel*>(state);
  if (!level.is_open()) return false;
  out = level.info();
  return true;
}
bool level_tile(const void* state, i32 tx, i32 tz, std::span<u32> words) noexcept {
  return static_cast<const MaskLevel*>(state)->tile(tx, tz, words);
}
bool level_present(const void* state, i32 tx, i32 tz) noexcept {
  return static_cast<const MaskLevel*>(state)->present(tx, tz);
}
constexpr scene_gen::GroundMaskOps k_level_ops{
    .present = &level_present, .info = &level_info, .tile = &level_tile};
}  // namespace

scene_gen::GroundMasks MaskLevel::masks() const noexcept {
  return is_open() ? scene_gen::GroundMasks{&k_level_ops, this} : scene_gen::GroundMasks{};
}

}  // namespace engine::tileset
