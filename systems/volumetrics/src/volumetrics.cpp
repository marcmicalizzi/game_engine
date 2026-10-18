#include <core/schema/json_reflect.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/shader_library.h>
#include <domain/terrain/crests.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/scene_ground.h>
#include <domain/terrain/tile.h>
#include <domain/terrain/wind.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/terrain.h>
#include <systems/volumetrics/volumetrics.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <schemas/volumetrics.h>
#include <shaders/medium.spv.h>

namespace engine::volumetrics {

namespace {

// The froxel grid (volumetrics.md, "The froxels"): read once, when a pass is made.
tunables::Int froxels_x{"volumetrics.froxels.x", 160, 8, 512, "Froxels across a view."};
tunables::Int froxels_y{"volumetrics.froxels.y", 90, 8, 512, "Froxels down a view."};
tunables::Int froxels_z{"volumetrics.froxels.z", 64, 8, 256,
                        "Froxel slices from the near distance to the far one, exponential."};
tunables::Float froxel_near{"volumetrics.froxels.near_m", 0.5, 0.05, 50.0,
                            "Where the first froxel slice begins, metres from the eye."};
tunables::Float froxel_far{"volumetrics.froxels.far_m", 1500.0, 50.0, 50000.0,
                           "Where the last froxel slice ends; the dust past it is closed form."};

// The cascaded maps' depth bias for a froxel, in texels of the cascade it reads: no receiver
// plane, so a few texels for the point's own surface.
constexpr f32 k_froxel_shadow_bias_texels = 2.0f;
// The dust's colour: the sand's, lighter, since a fine grain scatters more of every wavelength.
constexpr Vec3 k_dust_albedo{0.93f, 0.83f, 0.68f};

constexpr u32 k_params_bytes = sizeof(MediumParams);
constexpr gfx::BufferUsage k_address =
    gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;

u32 groups(u32 n, u32 size) { return (n + size - 1) / size; }

}  // namespace

// ---- the scene's block -------------------------------------------------------------------------

bool read_medium(const JsonValue& block, MediumDesc& out, bool& present, std::string* error) {
  out = MediumDesc{};
  present = false;
  if (block.is_null()) return true;
  scene::Medium medium;
  schema::ReadContext ctx;
  if (!schema::from_json(medium, block, ctx) || !ctx.ok()) {
    if (error != nullptr) {
      std::string text;
      for (const schema::Diagnostic& d : ctx.diagnostics) {
        if (!text.empty()) text += "; ";
        text += d.path.empty() ? d.message : "medium." + d.path + ": " + d.message;
      }
      *error = text.empty() ? std::string("medium: not an engine.scene.Medium") : text;
    }
    return false;
  }
  if (medium.clouds.has_value()) {
    const scene::MediumClouds& c = *medium.clouds;
    if (!(c.top_m > c.base_m) || c.base_m < 0.0f || c.top_m > 20000.0f) {
      if (error != nullptr) *error = "medium.clouds: base_m < top_m, within 0 and 20,000 m";
      return false;
    }
    out.clouds = true;
    out.cloud_base_m = c.base_m;
    out.cloud_top_m = c.top_m;
    out.cloud_coverage = std::clamp(c.coverage, 0.0f, 1.0f);
    out.cloud_type = std::clamp(c.type, 0.0f, 1.0f);
    out.cloud_seed = c.seed;
  }
  out.dust = medium.dust.enabled;
  out.dust_scale = std::max(medium.dust.scale, 0.0f);
  out.dust_height_m = std::max(medium.dust.height_m, 0.5f);
  out.plumes = medium.plumes.enabled;
  out.plume_strength = std::max(medium.plumes.strength, 0.0f);
  present = true;
  return true;
}

bool parse_medium_debug(std::string_view text, MediumDebug& out) noexcept {
  if (text == "dust" || text == "froxels") {
    out = MediumDebug::dust;
  } else if (text == "plumes") {
    out = MediumDebug::plumes;
  } else if (text == "clouds") {
    out = MediumDebug::clouds;
  } else if (text == "none") {
    out = MediumDebug::none;
  } else {
    return false;
  }
  return true;
}

const char* medium_debug_name(MediumDebug debug) noexcept {
  switch (debug) {
    case MediumDebug::dust: return "froxels";
    case MediumDebug::plumes: return "plumes";
    case MediumDebug::clouds: return "clouds";
    case MediumDebug::none: break;
  }
  return "none";
}

// ---- the wind --------------------------------------------------------------------------------

MediumWind wind_now(const terrain::DuneField* field, f64 time_s) noexcept {
  MediumWind out;
  if (field == nullptr) return out;
  const terrain::WindAt now = field->wind().wind_at(static_cast<i64>(std::floor(time_s * 1.0e6)));
  out.direction = Vec2{static_cast<f32>(terrain::fx::cos_q15(now.turn)) / 32768.0f,
                       static_cast<f32>(terrain::fx::sin_q15(now.turn)) / 32768.0f};
  out.strength = static_cast<f32>(now.speed_q16) / 65536.0f;
  out.speed_mps = out.strength * terrain::k_mean_wind_mps;
  out.flux_m2_per_day = static_cast<f32>(now.flux.magnitude * 24) * 1e-4f;
  out.storm = now.storm;
  return out;
}

// ---- the dust haze -----------------------------------------------------------------------------

DustState dust_state(const MediumDesc& desc, const MediumWind& wind) noexcept {
  DustState out;
  const f32 s = std::max(wind.strength, 0.0f);
  out.height_m = desc.dust_height_m * std::clamp(0.2f + 0.8f * s, 0.2f, 4.0f);
  if (!desc.dust) {
    out.extinction_per_m = 0.0f;
    return out;
  }
  // The floor of a calm, the haze the day's wind lifts (the square of its strength: the sand it
  // moves over the speed it is carried at), and in a storm hour the wall its front carries, at the
  // square of the storm's strength against its peak.
  f32 sigma = k_dust_clear_per_m + k_dust_haze_per_m * s * s;
  if (wind.storm) {
    const f32 r = s / k_dust_storm_peak;
    sigma += k_dust_wall_per_m * r * r;
  }
  out.extinction_per_m = sigma * desc.dust_scale;
  return out;
}

f32 dust_extinction(const DustState& dust, f32 height_m) noexcept {
  return dust.extinction_per_m * std::exp(-std::max(height_m, 0.0f) / dust.height_m);
}

f32 dust_optical_depth(const DustState& dust, f32 eye_height_m, f32 dy, f32 a, f32 b) noexcept {
  if (b <= a) return 0.0f;
  const f32 sigma0 = dust.extinction_per_m;
  const f32 h = dust.height_m;
  const f32 g = -eye_height_m;  // the ground's y in the eye's frame
  f32 t0 = 3.0e38f;
  if (dy < 0.0f) t0 = std::max(g / dy, 0.0f);
  f32 tau = 0.0f;
  const f32 e_hi = std::min(b, t0);
  if (e_hi > a) {
    if (std::fabs(dy) < 1.0e-5f) {
      tau += sigma0 * std::exp(std::min(g / h, 0.0f)) * (e_hi - a);
    } else {
      const f32 k = dy / h;
      tau += sigma0 * std::exp(g / h) * (std::exp(-a * k) - std::exp(-e_hi * k)) / k;
    }
  }
  if (b > t0) tau += sigma0 * (b - std::max(a, t0));
  return tau;
}

f32 visibility_m(f32 extinction_per_m) noexcept {
  const f32 depth = -std::log(k_visibility_contrast);
  if (extinction_per_m <= depth / k_max_visibility_m) return k_max_visibility_m;
  return depth / extinction_per_m;
}

// ---- the pass
// ------------------------------------------------------------------------------------

// ---- the crest plumes (W45) ---------------------------------------------------------------------

f32 plume_peak(f32 saltation_m2_per_day, f32 sharpness, f32 strength) noexcept {
  if (saltation_m2_per_day <= 0.0f || sharpness < k_plume_min_sharpness) return 0.0f;
  return k_plume_peak_per_m * strength * sharpness * sharpness *
         std::sqrt(saltation_m2_per_day / k_plume_mean_saltation);
}

u32 crest_plume_segments(const terrain::CrestLine& crest, const terrain::TileWind& wind,
                         const WorldPos& eye, f32 strength, PlumeSegment* out,
                         u32 capacity) noexcept {
  if (crest.height_m < k_plume_min_height_m) return 0;
  const f32 peak = plume_peak(wind.saltation_m2_per_day, crest.sharpness, strength);
  if (peak <= 0.0f || wind.speed_mps <= 0.0f) return 0;
  const f32 decay = k_plume_decay_s * wind.speed_mps;
  u32 written = 0;
  for (u32 k = 0; k + 1 < terrain::k_crest_points && written < capacity; ++k) {
    // The crest's world metres less the eye's, in f64, and only then a float: the frame's space.
    auto rel = [&](const Vec3& p) {
      return Vec3{static_cast<f32>(static_cast<f64>(p.x) - eye.x),
                  static_cast<f32>(static_cast<f64>(p.y) - eye.y),
                  static_cast<f32>(static_cast<f64>(p.z) - eye.z)};
    };
    PlumeSegment& s = out[written++];
    s.a = Vec4{rel(crest.points[k]), peak};
    s.b = Vec4{rel(crest.points[k + 1]), decay};
    s.shape = Vec4{wind.direction.x, wind.direction.y, k_plume_spread_m, k_plume_spread_per_m};
  }
  return written;
}

f32 plume_density(const PlumeSegment& s, const Vec3& x) noexcept {
  // Where the point is in the ribbon: x - a = u (b - a) + t w over the ground, u along the brink
  // and t downwind of it.
  const f32 ex = s.b.x - s.a.x;
  const f32 ez = s.b.z - s.a.z;
  const f32 dx = x.x - s.a.x;
  const f32 dz = x.z - s.a.z;
  const f32 wx = s.shape.x;
  const f32 wz = s.shape.y;
  const f32 det = ex * wz - ez * wx;
  const f32 len = std::sqrt(ex * ex + ez * ez);
  if (std::fabs(det) < 1.0e-3f * len || len <= 0.0f) return 0.0f;  // a brink along the wind
  const f32 u = (dx * wz - dz * wx) / det;
  const f32 t = (ex * dz - ez * dx) / det;
  if (t < 0.0f) return 0.0f;
  const f32 sigma = s.shape.z + s.shape.w * t;
  const f32 over = (u < 0.0f ? -u : (u > 1.0f ? u - 1.0f : 0.0f)) * len;
  const f32 lateral = std::exp(-0.5f * (over / sigma) * (over / sigma));
  const f32 uc = std::clamp(u, 0.0f, 1.0f);
  const f32 centre = s.a.y + (s.b.y - s.a.y) * uc + k_plume_lift * t;
  const f32 dy = x.y - centre;
  const f32 spread = dy >= 0.0f ? sigma : sigma * k_plume_floor;
  const f32 vertical = std::exp(-0.5f * (dy / spread) * (dy / spread));
  return s.a.w * (s.shape.z / sigma) * std::exp(-t / s.b.w) * lateral * vertical;
}

// The tiles' crest lines, by tile and hour, and the query's scratch: kept so a frame queries only
// the tiles it has not seen this hour, and grown once, so a frame that has seen them all allocates
// nothing. An entry's lines are the hour's start's: a function of the tile and the hour.
struct MediumPass::PlumeCache {
  struct Entry {
    i64 x = 0;
    i64 z = 0;
    i64 hour = -1;
    u64 used = 0;
    Vector<terrain::CrestLine> crests;
    terrain::TileWind wind;
  };
  static constexpr u32 k_entries = 4096;
  static constexpr u32 k_slots = 8192;
  Vector<Entry> entries;
  Vector<i32> slots;  // open addressing over (x, z, hour) -> entry, -1 empty
  u64 clock = 0;
  terrain::CrestScratch scratch;
  Vector<PlumeSegment> segments;                // the frame's, before binning
  Vector<u32> counts;                           // per cell
  Vector<gfx::BufferResource> segment_buffers;  // per frame slot
  Vector<gfx::BufferResource> cell_buffers;     // per frame slot: offsets, then indices

  PlumeCache() {
    entries.resize(k_entries);
    slots.resize(k_slots);
    for (i32& s : slots)
      s = -1;
    segments.reserve(k_plume_max_segments);
    counts.resize(k_plume_cells * k_plume_cells + 1);
    scratch.gather.primitives.reserve(8192);
    scratch.gather.grid.reserve(65536);
  }
  static u32 hash(i64 x, i64 z, i64 hour) {
    u64 h = static_cast<u64>(x) * 0x9E3779B97F4A7C15ull ^
            static_cast<u64>(z) * 0xC2B2AE3D27D4EB4Full ^
            static_cast<u64>(hour) * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    return static_cast<u32>(h % k_slots);
  }
  // The entry for a tile at an hour, queried if it is not held. The least recently used goes when
  // every entry is taken (its lines' capacity is reused).
  const Entry& at(const terrain::DuneField& field, i64 x, i64 z, i64 hour) {
    ++clock;
    u32 slot = hash(x, z, hour);
    for (u32 probe = 0; probe < k_slots; ++probe, slot = (slot + 1) % k_slots) {
      const i32 e = slots[slot];
      if (e < 0) break;
      Entry& entry = entries[static_cast<u32>(e)];
      if (entry.x == x && entry.z == z && entry.hour == hour) {
        entry.used = clock;
        return entry;
      }
    }
    // Not held: the first free entry, or the least recently used one, whose slot is rebuilt.
    u32 pick = 0;
    for (u32 i = 0; i < k_entries; ++i) {
      if (entries[i].hour < 0) {
        pick = i;
        break;
      }
      if (entries[i].used < entries[pick].used) pick = i;
    }
    Entry& entry = entries[pick];
    if (entry.hour >= 0) rebuild_slots_without(pick);
    entry.x = x;
    entry.z = z;
    entry.hour = hour;
    entry.used = clock;
    terrain::TileOptions options;
    options.tile_mm = k_plume_tile_mm;
    options.cells = 4;  // the sand share over 25 vertices: the plume's flux, not the tile's mesh
    options.normals = false;
    terrain::tile_crests(field, terrain::TileCoord{static_cast<i32>(x), static_cast<i32>(z)},
                         hour * 3'600'000'000ll, options, nullptr, scratch, entry.crests,
                         entry.wind);
    slot = hash(x, z, hour);
    while (slots[slot] >= 0)
      slot = (slot + 1) % k_slots;
    slots[slot] = static_cast<i32>(pick);
    return entry;
  }
  void rebuild_slots_without(u32 gone) {
    for (i32& s : slots)
      s = -1;
    for (u32 i = 0; i < k_entries; ++i) {
      if (i == gone || entries[i].hour < 0) continue;
      u32 slot = hash(entries[i].x, entries[i].z, entries[i].hour);
      while (slots[slot] >= 0)
        slot = (slot + 1) % k_slots;
      slots[slot] = static_cast<i32>(i);
    }
  }
};

// ---- the far-field clouds (R83) -----------------------------------------------------------------

namespace {

// medium.slang's `cloud_hash`, `value_noise`, `worley`: the same integer hash and lattice
// arithmetic, so the CPU's field is the GPU's (to the floats the two compilers round alike).
u32 cloud_hash(i32 x, i32 y, i32 z, u32 seed) noexcept {
  u32 h = seed * 0x9E3779B9u ^ static_cast<u32>(x) * 0x85EBCA6Bu ^
          static_cast<u32>(y) * 0xC2B2AE35u ^ static_cast<u32>(z) * 0x27D4EB2Fu;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
}
i32 wrap(i32 v, i32 period) noexcept {
  const i32 m = v % period;
  return m < 0 ? m + period : m;
}
f32 unit(u32 h) noexcept { return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); }
f32 value_noise(f32 x, f32 y, f32 z, i32 period, u32 seed) noexcept {
  const f32 fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
  const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy), iz = static_cast<i32>(fz);
  f32 tx = x - fx, ty = y - fy, tz = z - fz;
  tx = tx * tx * (3.0f - 2.0f * tx);
  ty = ty * ty * (3.0f - 2.0f * ty);
  tz = tz * tz * (3.0f - 2.0f * tz);
  f32 c[8];
  for (u32 k = 0; k < 8; ++k) {
    c[k] = unit(cloud_hash(wrap(ix + static_cast<i32>(k & 1), period),
                           iy + static_cast<i32>((k >> 1) & 1),
                           wrap(iz + static_cast<i32>(k >> 2), period), seed));
  }
  const f32 x00 = c[0] + (c[1] - c[0]) * tx, x10 = c[2] + (c[3] - c[2]) * tx;
  const f32 x01 = c[4] + (c[5] - c[4]) * tx, x11 = c[6] + (c[7] - c[6]) * tx;
  const f32 y0 = x00 + (x10 - x00) * ty, y1 = x01 + (x11 - x01) * ty;
  return y0 + (y1 - y0) * tz;
}
// Inverted Worley: 1 at a cell's feature point, falling to 0 a cell away.
f32 worley(f32 x, f32 y, f32 z, i32 period, u32 seed) noexcept {
  const f32 fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
  const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy), iz = static_cast<i32>(fz);
  f32 best = 9.0f;
  for (i32 dz = -1; dz <= 1; ++dz) {
    for (i32 dy = -1; dy <= 1; ++dy) {
      for (i32 dx = -1; dx <= 1; ++dx) {
        const u32 h =
            cloud_hash(wrap(ix + dx, period), iy + dy, wrap(iz + dz, period), seed ^ 0x5bd1e995u);
        const f32 px = static_cast<f32>(dx) + unit(h) - (x - fx);
        const f32 py = static_cast<f32>(dy) + unit(h * 0x27d4eb2du) - (y - fy);
        const f32 pz = static_cast<f32>(dz) + unit(h * 0x165667b1u) - (z - fz);
        best = std::min(best, px * px + py * py + pz * pz);
      }
    }
  }
  return 1.0f - std::min(std::sqrt(best), 1.0f);
}
f32 saturate(f32 v) noexcept { return std::clamp(v, 0.0f, 1.0f); }
f32 smoothstep(f32 a, f32 b, f32 v) noexcept {
  const f32 t = saturate((v - a) / (b - a));
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

CloudParams cloud_params(const MediumDesc& desc) noexcept {
  CloudParams p;
  p.base_m = desc.cloud_base_m;
  p.top_m = desc.cloud_top_m;
  p.coverage = desc.cloud_coverage;
  p.type = desc.cloud_type;
  p.seed = desc.cloud_seed;
  return p;
}

f32 cloud_density(const CloudParams& p, f32 qx, f32 altitude_m, f32 qz) noexcept {
  const f32 h = (altitude_m - p.base_m) / (p.top_m - p.base_m);
  if (h <= 0.0f || h >= 1.0f || p.coverage <= 0.0f) return 0.0f;
  const i32 shape_period = static_cast<i32>(k_cloud_period_m / k_cloud_shape_m);
  const f32 sx = qx / k_cloud_shape_m, sy = altitude_m / k_cloud_shape_m, sz = qz / k_cloud_shape_m;
  const f32 fbm =
      0.625f * value_noise(sx, sy, sz, shape_period, p.seed) +
      0.25f * value_noise(2.0f * sx, 2.0f * sy, 2.0f * sz, 2 * shape_period, p.seed + 1u) +
      0.125f * value_noise(4.0f * sx, 4.0f * sy, 4.0f * sz, 4 * shape_period, p.seed + 2u);
  const i32 cell_period = static_cast<i32>(k_cloud_period_m / k_cloud_cell_m);
  const f32 w = worley(qx / k_cloud_cell_m, altitude_m / k_cloud_cell_m, qz / k_cloud_cell_m,
                       cell_period, p.seed);
  // Perlin-Worley: the fbm remapped from [w - 1, 1] to [0, 1], so a cell's feature point keeps it.
  const f32 pw = saturate((fbm - (w - 1.0f)) / (2.0f - w));
  const f32 top = 0.35f + 0.65f * p.type;
  const f32 profile = smoothstep(0.0f, 0.08f, h) * (1.0f - smoothstep(0.6f * top, top, h));
  f32 d = saturate((pw * profile - (1.0f - p.coverage)) / p.coverage);
  if (d <= 0.0f) return 0.0f;
  const i32 detail_period = static_cast<i32>(k_cloud_period_m / k_cloud_detail_m);
  const f32 e = worley(qx / k_cloud_detail_m, altitude_m / k_cloud_detail_m, qz / k_cloud_detail_m,
                       detail_period, p.seed + 7u);
  d = saturate((d - 0.3f * (1.0f - e)) / (1.0f - 0.3f * (1.0f - e)));
  return d * k_cloud_extinction_per_m;
}

namespace {
Vec2 aloft_velocity(const terrain::WindAt& now) noexcept {
  const f32 speed =
      static_cast<f32>(now.speed_q16) / 65536.0f * terrain::k_mean_wind_mps * k_cloud_speed_factor;
  return Vec2{static_cast<f32>(terrain::fx::cos_q15(now.turn)) / 32768.0f * speed,
              static_cast<f32>(terrain::fx::sin_q15(now.turn)) / 32768.0f * speed};
}
}  // namespace

void CloudDrift::build(const terrain::DuneField* field) {
  field_ = field;
  prefix_.clear();
  hours_ = 0;
  if (field == nullptr) return;
  hours_ = static_cast<u32>(terrain::k_record_days) * 24u;
  prefix_.resize((u64{hours_} + 1) * 2);
  f64 x = 0.0, z = 0.0;
  for (u32 h = 0; h < hours_; ++h) {
    prefix_[u64{h} * 2] = x;
    prefix_[u64{h} * 2 + 1] = z;
    const Vec2 v = aloft_velocity(field->wind().wind_at(i64{h} * 3'600'000'000ll + 1));
    x += static_cast<f64>(v.x) * 3600.0;
    z += static_cast<f64>(v.y) * 3600.0;
  }
  prefix_[u64{hours_} * 2] = x;
  prefix_[u64{hours_} * 2 + 1] = z;
}

Vec2 CloudDrift::offset_m(f64 time_s) const noexcept {
  const f64 period = static_cast<f64>(k_cloud_period_m);
  f64 x = 0.0, z = 0.0;
  if (field_ == nullptr || hours_ == 0) {
    x = static_cast<f64>(terrain::k_mean_wind_mps * k_cloud_speed_factor) * time_s;
  } else {
    const f64 span = static_cast<f64>(hours_) * 3600.0;
    const f64 cycles = std::floor(time_s / span);
    const f64 within = time_s - cycles * span;
    const u32 h = std::min(static_cast<u32>(within / 3600.0), hours_ - 1);
    const Vec2 v =
        aloft_velocity(field_->wind().wind_at(static_cast<i64>(std::floor(time_s * 1.0e6))));
    const f64 into = within - static_cast<f64>(h) * 3600.0;
    // The periods' sums reduced first, so a century of drift stays exact in f64.
    x = std::fmod(cycles * std::fmod(prefix_[u64{hours_} * 2], period), period) +
        prefix_[u64{h} * 2] + static_cast<f64>(v.x) * into;
    z = std::fmod(cycles * std::fmod(prefix_[u64{hours_} * 2 + 1], period), period) +
        prefix_[u64{h} * 2 + 1] + static_cast<f64>(v.y) * into;
  }
  x -= std::floor(x / period) * period;
  z -= std::floor(z / period) * period;
  return Vec2{static_cast<f32>(x), static_cast<f32>(z)};
}

struct MediumPass::Pipelines {
  gfx::ComputePipeline fill;
  gfx::ComputePipeline integrate;
  gfx::ComputePipeline apply;
  gfx::ComputePipeline cloud_shadow;
  gfx::ComputePipeline cloud_march;
  // The pass bodies' push blocks, one per view: the graph records them after `record` returns.
  u64 push[renderer::k_max_views] = {};
};

MediumPass::MediumPass() noexcept = default;
MediumPass::~MediumPass() { destroy(); }

bool MediumPass::create(const gfx::Device& device, renderer::SceneRenderer& renderer,
                        const renderer::SceneData& scene, const MediumDesc& desc,
                        const MediumOptions& options, std::string* error) {
  destroy();
  device_ = &device;
  desc_ = desc;
  options_ = options;
  if (options_.froxels_x == 0) options_.froxels_x = static_cast<u32>(froxels_x.get());
  if (options_.froxels_y == 0) options_.froxels_y = static_cast<u32>(froxels_y.get());
  if (options_.froxels_z == 0) options_.froxels_z = static_cast<u32>(froxels_z.get());
  if (options_.near_m <= 0.0f) options_.near_m = static_cast<f32>(froxel_near.get());
  if (options_.far_m <= 0.0f) options_.far_m = static_cast<f32>(froxel_far.get());
  if (options_.far_m <= options_.near_m * 2.0f) options_.far_m = options_.near_m * 2.0f;
  // The sources this pass draws: the block's, or the one the debug view asks for alone.
  sources_ = 0;
  if (desc_.dust) sources_ |= k_source_dust;
  // The plumes need the dunes' crest lines: a scene whose ground is not the dune field has none.
  switch (options_.debug) {
    case MediumDebug::dust: sources_ &= k_source_dust; break;
    case MediumDebug::plumes: sources_ &= k_source_plumes; break;
    case MediumDebug::clouds: sources_ &= k_source_clouds; break;
    case MediumDebug::none: break;
  }
  // The ground the dust lies on and the wind that lifts it: the scene's terrain, through the same
  // provider the renderer drew it with (and the dunes' own field behind it, for the wind record).
  if (scene.terrain.enabled) {
    ground_ = std::make_unique<renderer::TerrainSampler>(scene.terrain);
    if (ground_->ok()) {
      field_ = terrain::dune_field(ground_->provider());
    } else {
      ground_.reset();
    }
  }
  if (desc_.clouds &&
      (options_.debug == MediumDebug::none || options_.debug == MediumDebug::clouds)) {
    sources_ |= k_source_clouds;
    drift_.build(field_);
  }
  if (desc_.plumes && field_ != nullptr) {
    plumes_ = std::make_unique<PlumeCache>();
    if (options_.debug == MediumDebug::none || options_.debug == MediumDebug::plumes)
      sources_ |= k_source_plumes;
  }
  gfx::ShaderLibrary& shaders = renderer.shaders();
  shaders.add_embedded("medium", shaders::k_medium_spirv, shaders::k_medium_spirv_size);
  const gfx::Shader* shader = shaders.get("medium", error);
  if (shader == nullptr) {
    destroy();
    return false;
  }
  pipelines_ = std::make_unique<Pipelines>();
  const gfx::DescriptorSetLayoutHandle set_layout = renderer.scene().bindless().layout();
  const std::span<const gfx::DescriptorSetLayoutHandle> sets(&set_layout, 1);
  if (!gfx::create_compute_pipeline(device, shader->module, "froxel_fill_main", sets, sizeof(u64),
                                    pipelines_->fill, error) ||
      !gfx::create_compute_pipeline(device, shader->module, "froxel_integrate_main", sets,
                                    sizeof(u64), pipelines_->integrate, error) ||
      !gfx::create_compute_pipeline(device, shader->module, "medium_apply_main", sets, sizeof(u64),
                                    pipelines_->apply, error) ||
      !gfx::create_compute_pipeline(device, shader->module, "cloud_shadow_main", sets, sizeof(u64),
                                    pipelines_->cloud_shadow, error) ||
      !gfx::create_compute_pipeline(device, shader->module, "cloud_march_main", sets, sizeof(u64),
                                    pipelines_->cloud_march, error)) {
    destroy();
    return false;
  }
  if (!renderer.add_frame_pass(renderer::FramePassStage::AfterResolve, &MediumPass::record_thunk,
                               this, "medium", error)) {
    destroy();
    return false;
  }
  renderer_ = &renderer;
  stats_ = MediumStats{};
  stats_.froxels_x = options_.froxels_x;
  stats_.froxels_y = options_.froxels_y;
  stats_.froxels_z = options_.froxels_z;
  stats_.near_m = options_.near_m;
  stats_.far_m = options_.far_m;
  return true;
}

void MediumPass::destroy() noexcept {
  if (renderer_ != nullptr) {
    renderer_->remove_frame_pass(&MediumPass::record_thunk, this);  // waits for the device
    renderer_ = nullptr;
  }
  if (device_ != nullptr) {
    if (pipelines_ != nullptr) {
      gfx::destroy_compute_pipeline(*device_, pipelines_->fill);
      gfx::destroy_compute_pipeline(*device_, pipelines_->integrate);
      gfx::destroy_compute_pipeline(*device_, pipelines_->apply);
      gfx::destroy_compute_pipeline(*device_, pipelines_->cloud_shadow);
      gfx::destroy_compute_pipeline(*device_, pipelines_->cloud_march);
    }
    for (gfx::BufferResource& b : params_)
      gfx::destroy_buffer(*device_, b);
    if (froxels_.buffer.valid()) gfx::destroy_buffer(*device_, froxels_);
    if (clouds_.buffer.valid()) gfx::destroy_buffer(*device_, clouds_);
    if (cloud_shadow_.buffer.valid()) gfx::destroy_buffer(*device_, cloud_shadow_);
    if (plumes_ != nullptr) {
      for (gfx::BufferResource& b : plumes_->segment_buffers)
        gfx::destroy_buffer(*device_, b);
      for (gfx::BufferResource& b : plumes_->cell_buffers)
        gfx::destroy_buffer(*device_, b);
    }
  }
  plumes_.reset();
  pipelines_.reset();
  params_.clear();
  froxels_ = gfx::BufferResource{};
  clouds_ = gfx::BufferResource{};
  cloud_shadow_ = gfx::BufferResource{};
  cloud_w_ = cloud_h_ = 0;
  views_ = 0;
  ground_.reset();
  field_ = nullptr;
  device_ = nullptr;
}

bool MediumPass::record_thunk(const renderer::FramePassContext& context, void* user,
                              std::string* error) {
  return static_cast<MediumPass*>(user)->record(context, error);
}

// The crest plumes of the frame (volumetrics.md, "The crest plumes"): every crest line of every
// dune tile whose plumes can reach within `k_plume_radius_m` of the eye, at the hour's start, as
// brink segments in the frame's space; then binned into a grid of `k_plume_cells` cells a side
// centred on the eye, so a froxel visits only the plumes over its own cell. Into this slot's
// buffers.
u32 MediumPass::build_plumes(const renderer::FramePassContext& c, const MediumWind& wind,
                             Vec4& grid) {
  PlumeCache& cache = *plumes_;
  const WorldPos eye = c.eye_world;
  const i64 hour = static_cast<i64>(std::floor(c.world_time_s / 3600.0));
  const f64 longest = 3.0 * static_cast<f64>(k_plume_decay_s * std::max(wind.speed_mps, 0.0f));
  const f64 reach = static_cast<f64>(k_plume_radius_m) + longest;
  const f64 tile_m = static_cast<f64>(k_plume_tile_mm) / 1000.0;
  const i64 tx0 = static_cast<i64>(std::floor((eye.x - reach) / tile_m));
  const i64 tx1 = static_cast<i64>(std::floor((eye.x + reach) / tile_m));
  const i64 tz0 = static_cast<i64>(std::floor((eye.z - reach) / tile_m));
  const i64 tz1 = static_cast<i64>(std::floor((eye.z + reach) / tile_m));
  cache.segments.clear();
  u32 tiles = 0;
  const f32 radius2 = static_cast<f32>(reach * reach);
  for (i64 tz = tz0; tz <= tz1; ++tz) {
    for (i64 tx = tx0; tx <= tx1; ++tx) {
      const PlumeCache::Entry& entry = cache.at(*field_, tx, tz, hour);
      ++tiles;
      for (const terrain::CrestLine& crest : entry.crests) {
        PlumeSegment made[terrain::k_crest_points - 1];
        const u32 n = crest_plume_segments(crest, entry.wind, eye, desc_.plume_strength, made,
                                           terrain::k_crest_points - 1);
        for (u32 i = 0; i < n; ++i) {
          if (cache.segments.size() >= k_plume_max_segments) break;
          const Vec4& a = made[i].a;
          if (a.x * a.x + a.z * a.z > radius2) continue;
          cache.segments.push_back(made[i]);
        }
      }
    }
  }
  // The binning: each segment's footprint over the ground — its brink and the brink moved three
  // decay lengths downwind, widened by the spread there — counted into the cells it covers.
  const f32 half = 0.5f * static_cast<f32>(k_plume_cells) * k_plume_cell_m;
  grid = Vec4{-half, -half, k_plume_cell_m, static_cast<f32>(k_plume_cells)};
  const u32 cells = k_plume_cells * k_plume_cells;
  for (u32& n : cache.counts)
    n = 0;
  auto footprint = [&](const PlumeSegment& s, i32& c0x, i32& c0z, i32& c1x, i32& c1z) {
    const f32 run = 3.0f * s.b.w;
    const f32 widen = s.shape.z + s.shape.w * run;
    const f32 xs[4] = {s.a.x, s.b.x, s.a.x + s.shape.x * run, s.b.x + s.shape.x * run};
    const f32 zs[4] = {s.a.z, s.b.z, s.a.z + s.shape.y * run, s.b.z + s.shape.y * run};
    f32 lo_x = xs[0], hi_x = xs[0], lo_z = zs[0], hi_z = zs[0];
    for (u32 i = 1; i < 4; ++i) {
      lo_x = std::min(lo_x, xs[i]);
      hi_x = std::max(hi_x, xs[i]);
      lo_z = std::min(lo_z, zs[i]);
      hi_z = std::max(hi_z, zs[i]);
    }
    const i32 last = static_cast<i32>(k_plume_cells) - 1;
    c0x = std::clamp(static_cast<i32>(std::floor((lo_x - widen + half) / k_plume_cell_m)), 0, last);
    c1x = std::clamp(static_cast<i32>(std::floor((hi_x + widen + half) / k_plume_cell_m)), 0, last);
    c0z = std::clamp(static_cast<i32>(std::floor((lo_z - widen + half) / k_plume_cell_m)), 0, last);
    c1z = std::clamp(static_cast<i32>(std::floor((hi_z + widen + half) / k_plume_cell_m)), 0, last);
  };
  u32 kept = 0;
  u32 refs = 0;
  for (; kept < cache.segments.size(); ++kept) {
    i32 c0x, c0z, c1x, c1z;
    footprint(cache.segments[kept], c0x, c0z, c1x, c1z);
    const u32 adds = static_cast<u32>((c1x - c0x + 1) * (c1z - c0z + 1));
    if (refs + adds > k_plume_max_refs) break;
    refs += adds;
    for (i32 z = c0z; z <= c1z; ++z)
      for (i32 x = c0x; x <= c1x; ++x)
        ++cache.counts[static_cast<u32>(z) * k_plume_cells + static_cast<u32>(x)];
  }
  u32* out = static_cast<u32*>(cache.cell_buffers[c.slot].mapped);
  u32 running = cells + 1;
  for (u32 i = 0; i < cells; ++i) {
    out[i] = running;
    running += cache.counts[i];
    cache.counts[i] = out[i];  // now each cell's cursor
  }
  out[cells] = running;
  for (u32 s = 0; s < kept; ++s) {
    i32 c0x, c0z, c1x, c1z;
    footprint(cache.segments[s], c0x, c0z, c1x, c1z);
    for (i32 z = c0z; z <= c1z; ++z)
      for (i32 x = c0x; x <= c1x; ++x)
        out[cache.counts[static_cast<u32>(z) * k_plume_cells + static_cast<u32>(x)]++] = s;
  }
  if (kept > 0) {
    std::memcpy(cache.segment_buffers[c.slot].mapped, cache.segments.data(),
                sizeof(PlumeSegment) * kept);
  }
  stats_.plume_tiles = tiles;
  return kept;
}

// The first frame makes the per-slot blocks and the grids; a frame after it makes nothing, so the
// steady-state frame allocates nothing (renderer.md, "The frame loop allocates nothing").
bool MediumPass::ensure_buffers(const renderer::FramePassContext& context, std::string* error) {
  if (views_ == context.view_count && params_.size() == context.frames_in_flight) return true;
  for (gfx::BufferResource& b : params_)
    gfx::destroy_buffer(*device_, b);
  params_.clear();
  if (froxels_.buffer.valid()) gfx::destroy_buffer(*device_, froxels_);
  views_ = context.view_count;
  params_.resize(context.frames_in_flight);
  for (gfx::BufferResource& b : params_) {
    if (!gfx::create_buffer(*device_, u64{k_params_bytes} * views_, k_address, true, b, error))
      return false;
  }
  if (plumes_ != nullptr) {
    for (gfx::BufferResource& b : plumes_->segment_buffers)
      gfx::destroy_buffer(*device_, b);
    for (gfx::BufferResource& b : plumes_->cell_buffers)
      gfx::destroy_buffer(*device_, b);
    plumes_->segment_buffers.resize(context.frames_in_flight);
    plumes_->cell_buffers.resize(context.frames_in_flight);
    for (u32 s = 0; s < context.frames_in_flight; ++s) {
      if (!gfx::create_buffer(*device_, sizeof(PlumeSegment) * k_plume_max_segments, k_address,
                              true, plumes_->segment_buffers[s], error) ||
          !gfx::create_buffer(*device_,
                              sizeof(u32) * (k_plume_cells * k_plume_cells + 1 + k_plume_max_refs),
                              k_address, true, plumes_->cell_buffers[s], error)) {
        return false;
      }
    }
  }
  const u64 cells = u64{options_.froxels_x} * options_.froxels_y * options_.froxels_z;
  return gfx::create_buffer(*device_, cells * 8 * views_, k_address, false, froxels_, error);
}

f64 MediumPass::corner_height(i64 x, i64 z) {
  ++corner_clock_;
  u32 pick = 0;
  for (u32 i = 0; i < k_corners; ++i) {
    if (corner_used_[i] != 0 && corner_x_[i] == x && corner_z_[i] == z) {
      corner_used_[i] = corner_clock_;
      return corner_h_[i];
    }
    if (corner_used_[i] < corner_used_[pick]) pick = i;
  }
  corner_x_[pick] = x;
  corner_z_[pick] = z;
  corner_used_[pick] = corner_clock_;
  corner_h_[pick] = static_cast<f64>(ground_->height(static_cast<f32>(static_cast<f64>(x) * 32.0),
                                                     static_cast<f32>(static_cast<f64>(z) * 32.0)));
  return corner_h_[pick];
}

bool MediumPass::record(const renderer::FramePassContext& c, std::string* error) {
  if (!ensure_buffers(c, error)) return false;
  // ---- the frame's medium on the CPU, in f64 where it is the world's --------------------------
  const MediumWind wind = wind_now(field_, c.world_time_s);
  const DustState dust = dust_state(desc_, wind);
  const WorldPos eye = c.eye_world;
  f64 ground_y = 0.0;
  if (ground_ != nullptr) {
    // Bilinear between the 32 m lattice's corners round the eye: a function of the eye, and a frame
    // that has seen its corners allocates nothing (the provider's height query does).
    const f64 gx = eye.x / 32.0;
    const f64 gz = eye.z / 32.0;
    const i64 cx = static_cast<i64>(std::floor(gx));
    const i64 cz = static_cast<i64>(std::floor(gz));
    const f64 fx = gx - static_cast<f64>(cx);
    const f64 fz = gz - static_cast<f64>(cz);
    const f64 h0 = corner_height(cx, cz) + (corner_height(cx + 1, cz) - corner_height(cx, cz)) * fx;
    const f64 h1 = corner_height(cx, cz + 1) +
                   (corner_height(cx + 1, cz + 1) - corner_height(cx, cz + 1)) * fx;
    ground_y = h0 + (h1 - h0) * fz;
  }
  const f32 eye_height = static_cast<f32>(eye.y - ground_y);
  const f32 at_eye = (sources_ & k_source_dust) != 0 ? dust_extinction(dust, eye_height) : 0.0f;
  stats_.active = true;
  stats_.wind = wind;
  stats_.eye_height_m = eye_height;
  stats_.dust_extinction_per_m = at_eye;
  stats_.dust_height_m = dust.height_m;
  stats_.visibility_m = visibility_m(at_eye);
  c.report->visibility_m = stats_.visibility_m;
  Vec4 plume_grid{};
  stats_.plume_segments =
      (sources_ & k_source_plumes) != 0 ? build_plumes(c, wind, plume_grid) : 0u;
  // The far clouds: the march's texels (a quarter of the largest view a side), made on the first
  // frame and again only when the views change size, and the noise's coordinate at the eye — the
  // eye less the wind's drift, reduced to the field's period in f64 (ADR-0053).
  const bool clouds = (sources_ & k_source_clouds) != 0;
  Vec2 cloud_origin{};
  if (clouds) {
    u32 cw = 0, ch = 0;
    for (u32 v = 0; v < c.view_count; ++v) {
      cw = std::max(cw, groups(c.views[v].width, k_cloud_scale));
      ch = std::max(ch, groups(c.views[v].height, k_cloud_scale));
    }
    if (cw != cloud_w_ || ch != cloud_h_ || !clouds_.buffer.valid()) {
      if (clouds_.buffer.valid()) gfx::destroy_buffer(*device_, clouds_);
      cloud_w_ = cw;
      cloud_h_ = ch;
      if (!gfx::create_buffer(*device_, u64{cw} * ch * 8 * c.view_count, k_address, false, clouds_,
                              error))
        return false;
    }
    if (!cloud_shadow_.buffer.valid() &&
        !gfx::create_buffer(*device_, u64{k_cloud_shadow_texels} * k_cloud_shadow_texels * 4,
                            k_address, false, cloud_shadow_, error))
      return false;
    const Vec2 drift = drift_.offset_m(c.world_time_s);
    const f64 period = static_cast<f64>(k_cloud_period_m);
    f64 ox = eye.x - static_cast<f64>(drift.x);
    f64 oz = eye.z - static_cast<f64>(drift.y);
    ox -= std::floor(ox / period) * period;
    oz -= std::floor(oz / period) * period;
    cloud_origin = Vec2{static_cast<f32>(ox), static_cast<f32>(oz)};
    stats_.cloud_drift_m = drift;
    stats_.cloud_width = cloud_w_;
    stats_.cloud_height = cloud_h_;
  }

  // ---- one block per view ----------------------------------------------------------------------
  u8* bytes = static_cast<u8*>(params_[c.slot].mapped);
  const u64 cells = u64{options_.froxels_x} * options_.froxels_y * options_.froxels_z;
  const f32 slice_log = std::log(options_.far_m / options_.near_m);
  const bool sky = c.sky_params != 0;
  u32 sources = sources_;
  if (c.cascades) sources |= k_source_shadows;
  for (u32 v = 0; v < c.view_count; ++v) {
    const renderer::FramePassView& view = c.views[v];
    MediumParams p{};
    p.clip_to_eye = view.clip_to_eye;
    p.clip_to_ray = view.clip_to_ray;
    p.grid[0] = options_.froxels_x;
    p.grid[1] = options_.froxels_y;
    p.grid[2] = options_.froxels_z;
    p.grid[3] = sources;
    p.slices = Vec4{options_.near_m, options_.far_m, slice_log,
                    static_cast<f32>(options_.froxels_z) / slice_log};
    p.sun = Vec4{c.sun_direction, k_froxel_shadow_bias_texels};
    p.sun_light = Vec4{c.sun_light, sky ? 1.0f : 0.0f};
    p.ambient = Vec4{c.ambient, 1.0f};
    p.dust = Vec4{dust.extinction_per_m, dust.height_m, -eye_height, static_cast<f32>(eye.y)};
    p.dust_albedo = Vec4{k_dust_albedo, k_dust_phase_g};
    p.terms = Vec4{k_dust_multiple, 0.0f, 0.0f, 0.0f};
    p.rect[0] = view.x;
    p.rect[1] = view.y;
    p.rect[2] = view.width;
    p.rect[3] = view.height;
    p.source[0] = view.source_width;
    p.source[1] = view.source_height;
    p.source[2] = c.color_storage;
    p.visibility = view.visibility;
    p.froxels = froxels_.address + cells * 8 * v;
    p.sky = c.sky_params;
    p.shadow_maps = c.cascades ? c.shadow_maps : 0;
    if (clouds) {
      const CloudParams cp = cloud_params(desc_);
      const f32 half = options_.far_m;
      p.cloud = Vec4{cp.base_m, cp.top_m, cp.coverage, cp.type};
      p.cloud_origin = Vec4{cloud_origin.x, 0.0f, cloud_origin.y, k_cloud_extinction_per_m};
      p.cloud_size = Vec4{static_cast<f32>(cloud_w_), static_cast<f32>(cloud_h_),
                          static_cast<f32>(view.width), static_cast<f32>(view.height)};
      p.cloud_shadow = Vec4{-half, -half, 2.0f * half / static_cast<f32>(k_cloud_shadow_texels),
                            static_cast<f32>(k_cloud_shadow_texels)};
      p.source[3] = cp.seed;
      p.clouds = clouds_.address + u64{cloud_w_} * cloud_h_ * 8 * v;
      p.cloud_shadow_map = cloud_shadow_.address;
    }
    if (stats_.plume_segments > 0) {
      p.plumes = plumes_->segment_buffers[c.slot].address;
      p.plume_cells = plumes_->cell_buffers[c.slot].address;
      p.plume_grid = plume_grid;
    }
    std::memcpy(bytes + u64{k_params_bytes} * v, &p, sizeof(p));
    pipelines_->push[v] = params_[c.slot].address + u64{k_params_bytes} * v;
  }

  // ---- the passes ------------------------------------------------------------------------------
  gfx::RenderGraph& graph = *c.graph;
  // The grids are this frame's from the fill on; the last frame's apply read them, which the fill
  // has to wait for.
  const gfx::RgBuffer rg_froxels =
      graph.import_buffer("medium froxels", froxels_, gfx::PipelineStage::ComputeShader,
                          gfx::MemoryAccess::ShaderRead | gfx::MemoryAccess::ShaderWrite);
  const gfx::RgBuffer sky_transmittance = c.sky.transmittance;
  const gfx::RgBuffer sky_frame = c.sky.frame;
  const gfx::RgImage atlas = c.cascades ? c.shadow_atlas : gfx::RgImage{};
  const gfx::RgBuffer visibility = c.visibility;
  const gfx::RgImage color = c.color;
  gfx::GpuTimer* timer = c.timer;
  gfx::BindlessSet* bindless = c.bindless;
  Pipelines* pipes = pipelines_.get();
  const u32 views = c.view_count;
  const u32 gx = groups(options_.froxels_x, 4);
  const u32 gy = groups(options_.froxels_y, 4);
  const u32 gz = groups(options_.froxels_z, 4);
  const u32 ix = groups(options_.froxels_x, 8);
  const u32 iy = groups(options_.froxels_y, 8);
  u32 rect_w[renderer::k_max_views] = {};
  u32 rect_h[renderer::k_max_views] = {};
  bool skip[renderer::k_max_views] = {};
  for (u32 v = 0; v < views && v < renderer::k_max_views; ++v) {
    rect_w[v] = c.views[v].width;
    rect_h[v] = c.views[v].height;
    skip[v] = c.views[v].panini;  // a resampled view's region is not its picture
  }
  // The far clouds first: the map of their transmittance towards the sun, which the fill reads, and
  // each view's march, which the apply composites into its sky pixels.
  const gfx::RgBuffer sky_aerial = c.sky.aerial;
  gfx::RgBuffer rg_clouds{};
  gfx::RgBuffer rg_cloud_shadow{};
  if (clouds) {
    rg_clouds = graph.import_buffer("medium clouds", clouds_, gfx::PipelineStage::ComputeShader,
                                    gfx::MemoryAccess::ShaderRead);
    rg_cloud_shadow =
        graph.import_buffer("medium cloud shadow", cloud_shadow_, gfx::PipelineStage::ComputeShader,
                            gfx::MemoryAccess::ShaderRead);
    const u32 shadow_groups = groups(k_cloud_shadow_texels, 8);
    const u32 mx = groups(cloud_w_, 8);
    const u32 my = groups(cloud_h_, 8);
    graph.add_pass(
        "medium clouds", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.write(rg_clouds, gfx::Access::ComputeWrite);
          b.write(rg_cloud_shadow, gfx::Access::ComputeWrite);
          if (sky) {
            b.read(sky_transmittance, gfx::Access::ComputeRead);
            b.read(sky_aerial, gfx::Access::ComputeRead);
            b.read(sky_frame, gfx::Access::ComputeRead);
          }
        },
        [pipes, timer, bindless, views, shadow_groups, mx, my, skip](gfx::CommandList cb,
                                                                     gfx::RenderGraph&) {
          timer->begin(cb, "medium");
          bindless->bind(cb, gfx::BindPoint::Compute);
          cb.bind_pipeline(gfx::BindPoint::Compute, pipes->cloud_shadow.pipeline);
          cb.push_constants(pipes->cloud_shadow.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &pipes->push[0]);
          cb.dispatch(shadow_groups, shadow_groups, 1);
          cb.bind_pipeline(gfx::BindPoint::Compute, pipes->cloud_march.pipeline);
          for (u32 v = 0; v < views; ++v) {
            if (skip[v]) continue;
            cb.push_constants(pipes->cloud_march.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                              &pipes->push[v]);
            cb.dispatch(mx, my, 1);
          }
          timer->end(cb);
        });
  }
  graph.add_pass(
      "medium fill", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.write(rg_froxels, gfx::Access::ComputeWrite);
        if (rg_cloud_shadow.valid()) b.read(rg_cloud_shadow, gfx::Access::ComputeRead);
        if (sky) {
          b.read(sky_transmittance, gfx::Access::ComputeRead);
          b.read(sky_frame, gfx::Access::ComputeRead);
        }
        if (atlas.valid()) b.read(atlas, gfx::Access::SampledRead);
      },
      [pipes, timer, bindless, views, gx, gy, gz, skip](gfx::CommandList cb, gfx::RenderGraph&) {
        timer->begin(cb, "medium");
        cb.bind_pipeline(gfx::BindPoint::Compute, pipes->fill.pipeline);
        bindless->bind(cb, gfx::BindPoint::Compute);
        for (u32 v = 0; v < views; ++v) {
          if (skip[v]) continue;
          cb.push_constants(pipes->fill.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &pipes->push[v]);
          cb.dispatch(gx, gy, gz);
        }
        timer->end(cb);
      });
  graph.add_pass(
      "medium integrate", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) { b.write(rg_froxels, gfx::Access::ComputeReadWrite); },
      [pipes, timer, bindless, views, ix, iy, skip](gfx::CommandList cb, gfx::RenderGraph&) {
        timer->begin(cb, "medium");
        cb.bind_pipeline(gfx::BindPoint::Compute, pipes->integrate.pipeline);
        bindless->bind(cb, gfx::BindPoint::Compute);
        for (u32 v = 0; v < views; ++v) {
          if (skip[v]) continue;
          cb.push_constants(pipes->integrate.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &pipes->push[v]);
          cb.dispatch(ix, iy, 1);
        }
        timer->end(cb);
      });
  graph.add_pass(
      "medium apply", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.read(rg_froxels, gfx::Access::ComputeRead);
        if (rg_clouds.valid()) {
          b.read(rg_clouds, gfx::Access::ComputeRead);
          if (sky) b.read(sky_aerial, gfx::Access::ComputeRead);
        }
        b.read(visibility, gfx::Access::ComputeRead);
        if (sky) {
          b.read(sky_transmittance, gfx::Access::ComputeRead);
          b.read(sky_frame, gfx::Access::ComputeRead);
        }
        b.write(color, gfx::Access::ComputeReadWrite);
      },
      [pipes, timer, bindless, views, rect_w, rect_h, skip](gfx::CommandList cb,
                                                            gfx::RenderGraph&) {
        timer->begin(cb, "medium");
        cb.bind_pipeline(gfx::BindPoint::Compute, pipes->apply.pipeline);
        bindless->bind(cb, gfx::BindPoint::Compute);
        for (u32 v = 0; v < views; ++v) {
          if (skip[v]) continue;
          cb.push_constants(pipes->apply.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &pipes->push[v]);
          cb.dispatch(groups(rect_w[v], 8), groups(rect_h[v], 8), 1);
        }
        timer->end(cb);
      });
  return true;
}

}  // namespace engine::volumetrics
