// The scatter layer's placement (docs/subsystems/scatter.md): a Poisson-disc per kind that any
// rectangle of the world can be asked for and that answers the same points wherever it is asked
// from, then the density, the rules and the exclusions, each a function of the point and what lies
// within a fixed distance of it.
//
// **Why a lattice and two rounds, and not Bridson's queue.** Bridson's algorithm grows a pattern
// from a seed point, so a point's fate depends on the order the pattern reached it: run per tile,
// two tiles disagree at their border, and run over the union, a tile depends on which neighbours
// were present. What a streamed world needs is a pattern any tile computes alone and every tile
// agrees on. So every cell of a global lattice (side r / √2, r the kind's spacing) holds one
// candidate at a hashed place with a hashed priority, and a candidate is kept by two rounds of a
// local maximum rule (Luby's): in round one, a candidate with no higher candidate within r; in
// round two, among the candidates round one neither kept nor shadowed, the same rule again. A
// candidate's fate then depends on the candidates within 3 r of it, which a tile computes from the
// lattice over its own cells and a six-cell apron — the "border apron": a point near a border is
// decided by the tile that owns it (the one its position is in), with its neighbour's candidates
// in view, and never by which tiles are resident. No two kept points of a kind are within r, by
// construction.
//
// Everything that decides is integer millimetres and a 64-bit hash; the density field, the slope
// and the rotation are f64 + - * / and sqrt, which IEEE rounds the same on every toolchain with
// floating-point contraction off (ADR-0035). No C-library function rounds inside a decision: sin,
// cos and atan2 are this file's own polynomials.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/io/vfs.h>
#include <systems/scatter/scatter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace engine::scatter {

namespace {

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_degrees = k_pi / 180.0;

// The salts that keep one hash's uses apart.
constexpr u64 k_salt_kind = 0x6B696E6473636174ull;
constexpr u64 k_salt_x = 0x5843414E44494441ull;
constexpr u64 k_salt_z = 0x5A43414E44494441ull;
constexpr u64 k_salt_priority = 0x5052494F52495459ull;
constexpr u64 k_salt_keep = 0x4B45455044454E53ull;
constexpr u64 k_salt_scale = 0x5343414C45524E44ull;
constexpr u64 k_salt_yaw = 0x59415752414E444Dull;
constexpr u64 k_salt_rank = 0x425544474554524Bull;
constexpr u64 k_salt_noise = 0x4E4F495345464C44ull;

// The lattice candidates of one cell decide among those within this many cells: two cells reach a
// spacing (the cell is r / √2), and two rounds look three spacings out.
constexpr i64 k_reach = 2;
constexpr i64 k_apron = 6;

// splitmix64's finalizer: every bit of the input moves every bit of the output.
constexpr u64 mix(u64 x) noexcept {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}
constexpr u64 mix2(u64 a, u64 b) noexcept { return mix(a ^ mix(b + 0x9E3779B97F4A7C15ull)); }
// The top 53 bits as a double in [0, 1): exact.
constexpr f64 unit(u64 h) noexcept {
  return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}

constexpr i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// The nearest whole millimetre to metres, halves up (scene_gen's `nearest_mm`, f32 inputs here).
i64 to_mm(f64 metres) noexcept { return scene_gen::nearest_mm(metres); }

struct Cand {
  i64 x = 0;
  i64 z = 0;
  u64 priority = 0;
};

// Priorities are hashes; two equal ones fall back to the place, which is unique (one candidate a
// cell), so the order is total.
bool above(const Cand& a, const Cand& b) noexcept {
  if (a.priority != b.priority) return a.priority > b.priority;
  if (a.z != b.z) return a.z > b.z;
  return a.x > b.x;
}

f64 smooth(f64 t) noexcept { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Value noise on the integer lattice of (x, z), in [0, 1].
f64 value_noise(u64 seed, f64 x, f64 z) noexcept {
  const f64 fx = std::floor(x);
  const f64 fz = std::floor(z);
  const i64 i = static_cast<i64>(fx);
  const i64 j = static_cast<i64>(fz);
  const f64 tx = smooth(x - fx);
  const f64 tz = smooth(z - fz);
  const auto at = [&](i64 a, i64 b) {
    return unit(mix2(seed, mix2(static_cast<u64>(a), static_cast<u64>(b))));
  };
  const f64 a = at(i, j) + (at(i + 1, j) - at(i, j)) * tx;
  const f64 b = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * tx;
  return a + (b - a) * tz;
}

// atan on [-1, 1]: the argument halved twice (atan x = 2 atan(x / (1 + √(1 + x²)))), then the
// series to x^23, which at |x| < 0.2 is past double precision.
f64 atan_unit(f64 x) noexcept {
  for (u32 k = 0; k < 2; ++k)
    x = x / (1.0 + std::sqrt(1.0 + x * x));
  const f64 x2 = x * x;
  f64 sum = 0.0;
  for (i32 n = 11; n >= 0; --n) {
    const f64 term = 1.0 / static_cast<f64>(2 * n + 1);
    sum = (n & 1) != 0 ? sum * x2 - term : sum * x2 + term;
    // Horner over alternating signs: sum = 1/1 - x²/3 + x⁴/5 - ...
  }
  return 4.0 * x * sum;
}

f64 atan2_det(f64 y, f64 x) noexcept {
  if (x == 0.0 && y == 0.0) return 0.0;
  const f64 ax = std::fabs(x);
  const f64 ay = std::fabs(y);
  f64 a = ay <= ax ? atan_unit(ay / ax) : 0.5 * k_pi - atan_unit(ax / ay);
  if (x < 0.0) a = k_pi - a;
  return y < 0.0 ? -a : a;
}

// A rotation of `angle` about +y, in double.
struct QuatD {
  f64 x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};
QuatD about_y(f64 angle) noexcept {
  f64 s = 0.0;
  f64 c = 1.0;
  sin_cos(0.5 * angle, s, c);
  return QuatD{0.0, s, 0.0, c};
}
QuatD multiply(const QuatD& a, const QuatD& b) noexcept {
  return QuatD{
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

std::string resolve(std::string_view dir, const std::string& path) {
  return io::is_absolute_path(path) || dir.empty() ? path : io::join_path(dir, path);
}

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not a scatter layer") : text;
}

bool read_mask(const std::string& path, const std::array<f64, 2>& lo, const std::array<f64, 2>& hi,
               Mask& out, std::string* error) {
  texture::TextureData data;
  std::string why;
  if (!texture::read_texture_file(path, data, &why)) {
    if (error != nullptr) *error = "the mask " + path + ": " + why;
    return false;
  }
  Vector<u8> rgba;
  if (!texture::decode_level(data.format, data.level_bytes(0), data.width, data.height, rgba,
                             &why)) {
    if (error != nullptr) *error = "the mask " + path + ": " + why;
    return false;
  }
  if (!(hi[0] > lo[0] && hi[1] > lo[1])) {
    if (error != nullptr)
      *error = "the mask " + path + " covers no rectangle: its max is not past its min";
    return false;
  }
  out.width = data.width;
  out.height = data.height;
  out.red.resize(data.width * data.height);
  for (u32 i = 0; i < out.red.size(); ++i)
    out.red[i] = rgba[i * 4];
  out.min_x = lo[0];
  out.min_z = lo[1];
  out.max_x = hi[0];
  out.max_z = hi[1];
  return true;
}

struct KindRt {
  i64 r_mm = 0;
  i64 r2 = 0;
  i64 cell_mm = 0;
  i64 clear_mm = 0;
  i64 clear2 = 0;
  f64 share = 0.0;  // weight over the weights' sum
  f64 rho = 0.0;    // the lattice's packing at this spacing, points a square metre
  f64 tan2_min = 0.0;
  f64 tan2_max = 0.0;  // < 0: no upper limit
  bool needs_gradient = false;
  u64 seed = 0;
};

struct Circle {
  i64 x = 0;
  i64 z = 0;
  i64 r2 = 0;
};

}  // namespace

f64 Mask::sample(f64 x_m, f64 z_m) const noexcept {
  if (width == 0 || x_m < min_x || x_m >= max_x || z_m < min_z || z_m >= max_z) return 0.0;
  const f64 u = (x_m - min_x) / (max_x - min_x) * static_cast<f64>(width) - 0.5;
  const f64 v = (z_m - min_z) / (max_z - min_z) * static_cast<f64>(height) - 0.5;
  const f64 fu = std::floor(u);
  const f64 fv = std::floor(v);
  const f64 tu = u - fu;
  const f64 tv = v - fv;
  const auto texel = [&](f64 a, f64 b) {
    const i64 i = std::clamp<i64>(static_cast<i64>(a), 0, static_cast<i64>(width) - 1);
    const i64 j = std::clamp<i64>(static_cast<i64>(b), 0, static_cast<i64>(height) - 1);
    return static_cast<f64>(red[static_cast<u32>(j * width + i)]) / 255.0;
  };
  const f64 a = texel(fu, fv) + (texel(fu + 1.0, fv) - texel(fu, fv)) * tu;
  const f64 b = texel(fu, fv + 1.0) + (texel(fu + 1.0, fv + 1.0) - texel(fu, fv + 1.0)) * tu;
  return a + (b - a) * tv;
}

void sin_cos(f64 radians, f64& s, f64& c) noexcept {
  // To [-π, π], then to an octant by the quarter turns, then the two series to x^21 / x^20, which
  // at |x| <= π/4 are past double precision.
  const f64 two_pi = 2.0 * k_pi;
  f64 x = radians - two_pi * std::floor(radians / two_pi + 0.5);
  const f64 quarter = std::floor(x / (0.5 * k_pi) + 0.5);
  x -= quarter * (0.5 * k_pi);
  const f64 x2 = x * x;
  f64 sn = 0.0;
  f64 cs = 0.0;
  // sin x = x (1 - x²/3! + x⁴/5! - ...), cos x = 1 - x²/2! + x⁴/4! - ..., by Horner from the end.
  for (i32 n = 10; n >= 0; --n) {
    f64 fs = 1.0;  // (2n + 1)!
    f64 fc = 1.0;  // (2n)!
    for (i32 k = 2; k <= 2 * n + 1; ++k) {
      fs *= static_cast<f64>(k);
      if (k <= 2 * n) fc *= static_cast<f64>(k);
    }
    const f64 sign = (n & 1) != 0 ? -1.0 : 1.0;
    sn = sn * x2 + sign / fs;
    cs = cs * x2 + sign / fc;
  }
  sn *= x;
  const i64 q = static_cast<i64>(quarter) & 3;
  switch (q) {
    case 0:
      s = sn;
      c = cs;
      break;
    case 1:
      s = cs;
      c = -sn;
      break;
    case 2:
      s = -sn;
      c = -cs;
      break;
    default:
      s = -cs;
      c = sn;
      break;
  }
}

u64 hash_points(std::span<const Point> points) noexcept {
  u64 h = hash_bytes("scatter.points", 14);
  for (const Point& p : points) {
    u8 bytes[48];
    std::memcpy(bytes, &p.x_mm, 8);
    std::memcpy(bytes + 8, &p.z_mm, 8);
    std::memcpy(bytes + 16, &p.y, 8);
    std::memcpy(bytes + 24, &p.rotation, 16);
    std::memcpy(bytes + 40, &p.scale, 4);
    std::memcpy(bytes + 44, &p.kind, 4);
    h = hash_bytes(bytes, sizeof(bytes), h);
  }
  return h;
}

struct Placer::Impl {
  Layer layer;
  u64 seed = 0;
  i64 tile_mm = 0;
  Vector<KindRt> kinds;
  // The region.
  Vector<f64> poly_x;
  Vector<f64> poly_z;
  // The density's map and the exclusions'.
  Mask density_mask;
  Mask exclude_mask;
  f64 exclude_threshold = 0.5;
  Vector<Circle> circles;
  struct Excluded {
    std::unique_ptr<Placer> placer;
    i64 clear_mm = 0;
    i64 clear2 = 0;
  };
  Vector<Excluded> excluded;

  bool in_region(i64 x, i64 z) const noexcept;
  f64 density(i64 x, i64 z) const noexcept;
  void lattice(u32 k, const RectMm& rect, Vector<Cand>& out) const;
  void place_kind(u32 k, const RectMm& rect, const scene_gen::Ground& ground,
                  std::span<const Point> earlier, Vector<Point>& out) const;
  void place_upto(u32 k, const RectMm& rect, const scene_gen::Ground& ground,
                  Vector<Point>& out) const;
};

bool Placer::Impl::in_region(i64 x, i64 z) const noexcept {
  switch (layer.region.kind) {
    case RegionKind::Scene: return true;
    case RegionKind::Tiles: {
      const i64 tx = floor_div(x, tile_mm);
      const i64 tz = floor_div(z, tile_mm);
      return tx >= layer.region.tile_min[0] && tx <= layer.region.tile_max[0] &&
             tz >= layer.region.tile_min[1] && tz <= layer.region.tile_max[1];
    }
    case RegionKind::Polygon: {
      // Even-odd, in metres.
      const f64 px = static_cast<f64>(x) / 1000.0;
      const f64 pz = static_cast<f64>(z) / 1000.0;
      bool inside = false;
      const u32 n = poly_x.size();
      for (u32 i = 0, j = n - 1; i < n; j = i++) {
        const bool crosses = (poly_z[i] > pz) != (poly_z[j] > pz);
        if (crosses) {
          const f64 at =
              poly_x[i] + (pz - poly_z[i]) / (poly_z[j] - poly_z[i]) * (poly_x[j] - poly_x[i]);
          if (px < at) inside = !inside;
        }
      }
      return inside;
    }
  }
  return false;
}

f64 Placer::Impl::density(i64 x, i64 z) const noexcept {
  const Density& d = layer.density;
  switch (d.kind) {
    case DensityKind::Constant: return d.per_m2;
    case DensityKind::Noise: {
      const f64 xm = static_cast<f64>(x) / 1000.0;
      const f64 zm = static_cast<f64>(z) / 1000.0;
      f64 wavelength = d.wavelength;
      f64 amplitude = 1.0;
      f64 sum = 0.0;
      f64 total = 0.0;
      for (u32 o = 0; o < d.octaves; ++o) {
        sum +=
            amplitude * value_noise(mix2(seed ^ k_salt_noise, o), xm / wavelength, zm / wavelength);
        total += amplitude;
        amplitude *= d.gain;
        wavelength /= d.lacunarity;
      }
      const f64 v = (sum / total - d.low) / (d.high - d.low);
      return d.per_m2 * std::clamp(v, 0.0, 1.0);
    }
    case DensityKind::Mask:
      return d.per_m2 *
             density_mask.sample(static_cast<f64>(x) / 1000.0, static_cast<f64>(z) / 1000.0);
  }
  return 0.0;
}

// The kept lattice points of kind `k` whose place is in `rect`, by the two rounds.
void Placer::Impl::lattice(u32 k, const RectMm& rect, Vector<Cand>& out) const {
  const KindRt& kind = kinds[k];
  const i64 c = kind.cell_mm;
  const i64 ci0 = floor_div(rect.x0, c);
  const i64 ci1 = floor_div(rect.x1 - 1, c);
  const i64 cj0 = floor_div(rect.z0, c);
  const i64 cj1 = floor_div(rect.z1 - 1, c);
  if (ci1 < ci0 || cj1 < cj0) return;
  const i64 i0 = ci0 - k_apron;
  const i64 j0 = cj0 - k_apron;
  const i64 w = ci1 - ci0 + 1 + 2 * k_apron;
  const i64 h = cj1 - cj0 + 1 + 2 * k_apron;
  Vector<Cand> grid(static_cast<u32>(w * h));
  for (i64 j = 0; j < h; ++j) {
    for (i64 i = 0; i < w; ++i) {
      const u64 cell = mix2(kind.seed, mix2(static_cast<u64>(i0 + i), static_cast<u64>(j0 + j)));
      Cand& cand = grid[static_cast<u32>(j * w + i)];
      cand.x = (i0 + i) * c + static_cast<i64>(mix(cell ^ k_salt_x) % static_cast<u64>(c));
      cand.z = (j0 + j) * c + static_cast<i64>(mix(cell ^ k_salt_z) % static_cast<u64>(c));
      cand.priority = mix(cell ^ k_salt_priority);
    }
  }
  // Bits: 1 kept in round one, 2 eligible for round two, 4 kept in round two.
  Vector<u8> state(static_cast<u32>(w * h), u8{0});
  const auto conflicts = [&](i64 i, i64 j, auto&& test) {
    const Cand& a = grid[static_cast<u32>(j * w + i)];
    for (i64 dj = -k_reach; dj <= k_reach; ++dj) {
      for (i64 di = -k_reach; di <= k_reach; ++di) {
        if (di == 0 && dj == 0) continue;
        const i64 ni = i + di;
        const i64 nj = j + dj;
        const Cand& b = grid[static_cast<u32>(nj * w + ni)];
        const i64 dx = a.x - b.x;
        const i64 dz = a.z - b.z;
        if (dx * dx + dz * dz < kind.r2 && test(b, state[static_cast<u32>(nj * w + ni)]))
          return true;
      }
    }
    return false;
  };
  // Round one over the region and four cells round it.
  for (i64 j = k_apron - 4; j < h - (k_apron - 4); ++j) {
    for (i64 i = k_apron - 4; i < w - (k_apron - 4); ++i) {
      const Cand& a = grid[static_cast<u32>(j * w + i)];
      if (!conflicts(i, j, [&](const Cand& b, u8) { return above(b, a); }))
        state[static_cast<u32>(j * w + i)] |= 1u;
    }
  }
  // Eligible for round two: not kept, and no kept point within a spacing; over two cells round.
  for (i64 j = k_apron - 2; j < h - (k_apron - 2); ++j) {
    for (i64 i = k_apron - 2; i < w - (k_apron - 2); ++i) {
      if ((state[static_cast<u32>(j * w + i)] & 1u) != 0) continue;
      if (!conflicts(i, j, [](const Cand&, u8 s) { return (s & 1u) != 0; }))
        state[static_cast<u32>(j * w + i)] |= 2u;
    }
  }
  // Round two over the region itself, and the region's kept points out.
  for (i64 j = k_apron; j < h - k_apron; ++j) {
    for (i64 i = k_apron; i < w - k_apron; ++i) {
      const u32 at = static_cast<u32>(j * w + i);
      const Cand& a = grid[at];
      bool kept = (state[at] & 1u) != 0;
      if (!kept && (state[at] & 2u) != 0) {
        kept = !conflicts(i, j, [&](const Cand& b, u8 s) { return (s & 2u) != 0 && above(b, a); });
      }
      if (kept && rect.contains(a.x, a.z)) out.push_back(a);
    }
  }
}

void Placer::Impl::place_kind(u32 k, const RectMm& rect, const scene_gen::Ground& ground,
                              std::span<const Point> earlier, Vector<Point>& out) const {
  const KindRt& rt = kinds[k];
  const Kind& kind = layer.kinds[k];
  Vector<Cand> kept;
  lattice(k, rect, kept);
  if (kept.empty()) return;
  // The excluded layers' placements near the rectangle, each from its own file and rules.
  Vector<Vector<Point>> others(excluded.size());
  for (u32 e = 0; e < excluded.size(); ++e)
    excluded[e].placer->place_rect(rect.grown(excluded[e].clear_mm), ground, others[e]);
  const auto height = [&](i64 x, i64 z) {
    if (!ground.present()) return 0.0;
    return kind.stand == Stand::Floor ? ground.floor_mm(x, z) : ground.surface_mm(x, z);
  };
  const auto near_any = [](std::span<const Point> points, i64 x, i64 z, i64 r, i64 r2,
                           u32 kind_filter) {
    for (const Point& p : points) {
      if (kind_filter != ~0u && p.kind >= kind_filter) continue;
      const i64 dx = p.x_mm - x;
      const i64 dz = p.z_mm - z;
      if (dx >= r || dx <= -r || dz >= r || dz <= -r) continue;
      if (dx * dx + dz * dz < r2) return true;
    }
    return false;
  };
  for (const Cand& c : kept) {
    // The density, by a keep test whose odds are the density over the lattice's packing.
    const f64 odds = density(c.x, c.z) * rt.share / rt.rho;
    if (!(unit(mix(c.priority ^ k_salt_keep)) < odds)) continue;
    if (!in_region(c.x, c.z)) continue;
    // The exclusions that need no ground first: circles, the mask, the other layers, the earlier
    // kinds.
    bool excluded_here = false;
    for (const Circle& circle : circles) {
      const i64 dx = c.x - circle.x;
      const i64 dz = c.z - circle.z;
      if (dx * dx + dz * dz < circle.r2) {
        excluded_here = true;
        break;
      }
    }
    if (excluded_here) continue;
    if (exclude_mask.present() &&
        exclude_mask.sample(static_cast<f64>(c.x) / 1000.0, static_cast<f64>(c.z) / 1000.0) >=
            exclude_threshold)
      continue;
    for (u32 e = 0; e < excluded.size() && !excluded_here; ++e) {
      excluded_here = near_any(std::span<const Point>(others[e].data(), others[e].size()), c.x, c.z,
                               excluded[e].clear_mm, excluded[e].clear2, ~0u);
    }
    if (excluded_here) continue;
    if (rt.clear_mm > 0 && near_any(earlier, c.x, c.z, rt.clear_mm, rt.clear2, k)) continue;
    // The ground: its height, and its slope where a rule asks for it.
    const f64 y = height(c.x, c.z);
    if (y < kind.height_min || y > kind.height_max) continue;
    f64 gx = 0.0;
    f64 gz = 0.0;
    if (rt.needs_gradient) {
      const f64 step = 2.0 * static_cast<f64>(k_slope_step_mm) / 1000.0;
      gx = (height(c.x + k_slope_step_mm, c.z) - height(c.x - k_slope_step_mm, c.z)) / step;
      gz = (height(c.x, c.z + k_slope_step_mm) - height(c.x, c.z - k_slope_step_mm)) / step;
      const f64 g2 = gx * gx + gz * gz;
      if (g2 < rt.tan2_min) continue;
      if (rt.tan2_max >= 0.0 && g2 > rt.tan2_max) continue;
    }
    // The scale, the yaw and the alignment.
    const f64 su = unit(mix(c.priority ^ k_salt_scale));
    const f64 scale =
        static_cast<f64>(kind.scale_min) + static_cast<f64>(kind.scale_max - kind.scale_min) * su;
    const f64 random_yaw = 2.0 * k_pi * unit(mix(c.priority ^ k_salt_yaw));
    const f64 offset = static_cast<f64>(kind.yaw_deg) * k_degrees;
    f64 yaw = 0.0;
    switch (kind.yaw) {
      case YawRule::Random: yaw = random_yaw + offset; break;
      case YawRule::Uphill:
        yaw = (gx * gx + gz * gz > 1e-12 ? atan2_det(gx, gz) : random_yaw) + offset;
        break;
      case YawRule::Fixed: yaw = offset; break;
    }
    QuatD q = about_y(yaw);
    if (kind.align) {
      // The shortest arc from +y to the normal (-gx, 1, -gz) / |...|, then the yaw before it.
      const f64 len = std::sqrt(gx * gx + 1.0 + gz * gz);
      const f64 nx = -gx / len;
      const f64 ny = 1.0 / len;
      const f64 nz = -gz / len;
      QuatD a{nz, 0.0, -nx, 1.0 + ny};
      const f64 an = std::sqrt(a.x * a.x + a.z * a.z + a.w * a.w);
      a = QuatD{a.x / an, 0.0, a.z / an, a.w / an};
      q = multiply(a, q);
    }
    Point p;
    p.x_mm = c.x;
    p.z_mm = c.z;
    p.y = y - static_cast<f64>(kind.sink) * scale;
    p.rotation = Quat{static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z),
                      static_cast<f32>(q.w)};
    p.yaw = static_cast<f32>(yaw);
    p.scale = static_cast<f32>(scale);
    p.kind = k;
    p.rank = static_cast<u32>(mix(c.priority ^ k_salt_rank) >> 32);
    out.push_back(p);
  }
}

// Kinds 0 .. k-1 over `rect`: each kind after the earlier ones it keeps its clearance from, which
// are placed over the rectangle grown by that clearance, so a point's clearance sees every earlier
// point near it whichever rectangle asked.
void Placer::Impl::place_upto(u32 k, const RectMm& rect, const scene_gen::Ground& ground,
                              Vector<Point>& out) const {
  if (k == 0) return;
  const i64 grow = kinds[k - 1].clear_mm;
  Vector<Point> earlier;
  place_upto(k - 1, grow > 0 ? rect.grown(grow) : rect, ground, earlier);
  for (const Point& p : earlier) {
    if (rect.contains(p.x_mm, p.z_mm)) out.push_back(p);
  }
  place_kind(k - 1, rect, ground, std::span<const Point>(earlier.data(), earlier.size()), out);
}

Placer::Placer() : impl_(std::make_unique<Impl>()) {}
Placer::~Placer() = default;
Placer::Placer(Placer&&) noexcept = default;
Placer& Placer::operator=(Placer&&) noexcept = default;

const Layer& Placer::layer() const noexcept { return impl_->layer; }
u64 Placer::seed() const noexcept { return impl_->seed; }
i64 Placer::tile_mm() const noexcept { return impl_->tile_mm; }
u32 Placer::kind_count() const noexcept { return impl_->kinds.size(); }

bool Placer::open(const Layer& desc, std::string_view dir, u64 world_seed, std::string* error,
                  bool layers_allowed) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  Impl& im = *impl_;
  im = Impl{};
  im.layer = desc;
  const Layer& layer = im.layer;
  im.seed = layer.seed != 0 ? layer.seed : world_seed;
  im.tile_mm = to_mm(static_cast<f64>(layer.tile_size));
  if (im.tile_mm <= 0) return fail("its tile_size must be positive");
  if (layer.kinds.empty()) return fail("it places no kinds");
  if (layer.kinds.size() > k_max_kinds)
    return fail("it has " + std::to_string(layer.kinds.size()) + " kinds; a layer has at most " +
                std::to_string(k_max_kinds));
  f64 weights = 0.0;
  for (const Kind& kind : layer.kinds) {
    if (!(kind.weight >= 0.0) || !std::isfinite(kind.weight))
      return fail("kind \"" + kind.name + "\" has a weight that is not a non-negative number");
    weights += kind.weight;
  }
  if (!(weights > 0.0)) return fail("its kinds' weights add up to nothing");
  for (u32 k = 0; k < layer.kinds.size(); ++k) {
    const Kind& kind = layer.kinds[k];
    const std::string name = "kind \"" + kind.name + "\"";
    if (kind.mesh.empty() && !kind.rock.has_value())
      return fail(name + " names no mesh and no rock");
    if (!(kind.spacing >= 0.01f)) return fail(name + ": its spacing must be at least 1 cm");
    if (!(kind.clearance >= 0.0f)) return fail(name + ": its clearance must not be negative");
    if (!(kind.scale_min > 0.0f) || !(kind.scale_max >= kind.scale_min))
      return fail(name + ": its scale range must be positive and in order");
    if (!(kind.slope_min_deg >= 0.0f) || !(kind.slope_max_deg <= 90.0f) ||
        !(kind.slope_min_deg <= kind.slope_max_deg))
      return fail(name + ": its slope limits must be in order within [0, 90] degrees");
    if (!(kind.height_min <= kind.height_max))
      return fail(name + ": its height limits must be in order");
    if (!(kind.far >= 0.0f)) return fail(name + ": its far must not be negative");
    KindRt rt;
    rt.r_mm = to_mm(static_cast<f64>(kind.spacing));
    rt.r2 = rt.r_mm * rt.r_mm;
    // r / √2, rounded down, so two cells always reach a spacing.
    rt.cell_mm = std::max<i64>(1, static_cast<i64>(static_cast<f64>(rt.r_mm) / std::sqrt(2.0)));
    rt.clear_mm = to_mm(static_cast<f64>(kind.clearance));
    rt.clear2 = rt.clear_mm * rt.clear_mm;
    rt.share = kind.weight / weights;
    const f64 r_m = static_cast<f64>(rt.r_mm) / 1000.0;
    rt.rho = k_packing / (r_m * r_m);
    f64 s = 0.0;
    f64 c = 1.0;
    sin_cos(static_cast<f64>(kind.slope_min_deg) * k_degrees, s, c);
    rt.tan2_min = kind.slope_min_deg > 0.0f ? (s * s) / (c * c) : 0.0;
    sin_cos(static_cast<f64>(kind.slope_max_deg) * k_degrees, s, c);
    rt.tan2_max = kind.slope_max_deg < 90.0f ? (s * s) / (c * c) : -1.0;
    rt.needs_gradient = kind.slope_min_deg > 0.0f || kind.slope_max_deg < 90.0f ||
                        kind.yaw == YawRule::Uphill || kind.align;
    rt.seed = mix2(im.seed ^ k_salt_kind, k);
    im.kinds.push_back(rt);
  }
  // The region.
  const Region& region = layer.region;
  if (region.kind == RegionKind::Tiles &&
      (region.tile_min[0] > region.tile_max[0] || region.tile_min[1] > region.tile_max[1]))
    return fail("its region's tile_min is past its tile_max");
  if (region.kind == RegionKind::Polygon) {
    if (region.polygon.size() < 3) return fail("its region's polygon has fewer than three corners");
    for (const auto& corner : region.polygon) {
      if (!std::isfinite(corner[0]) || !std::isfinite(corner[1]))
        return fail("its region's polygon has a corner that is not a number");
      im.poly_x.push_back(corner[0]);
      im.poly_z.push_back(corner[1]);
    }
  }
  // The density.
  const Density& density = layer.density;
  if (!(density.per_m2 >= 0.0) || !std::isfinite(density.per_m2))
    return fail("its density's per_m2 must be a non-negative number");
  if (density.kind == DensityKind::Noise) {
    if (density.octaves < 1 || density.octaves > 16)
      return fail("its density's noise has " + std::to_string(density.octaves) +
                  " octaves; it takes 1 to 16");
    if (!(density.wavelength > 0.0) || !(density.lacunarity >= 1.0) || !(density.gain > 0.0))
      return fail(
          "its density's noise needs a positive wavelength and gain and a lacunarity of "
          "at least 1");
    if (!(density.high > density.low)) return fail("its density's noise needs high above low");
  }
  std::string why;
  if (density.kind == DensityKind::Mask) {
    if (density.mask.empty()) return fail("its density is a mask and names no file");
    if (!read_mask(resolve(dir, density.mask), density.mask_min, density.mask_max, im.density_mask,
                   &why))
      return fail("its density: " + why);
  }
  // The exclusions.
  const Exclusions& exclude = layer.exclude;
  for (const ExcludeCircle& circle : exclude.circles) {
    if (!(circle.radius >= 0.0f)) return fail("an exclusion circle has a negative radius");
    const i64 r = to_mm(static_cast<f64>(circle.radius));
    im.circles.push_back(
        Circle{scene_gen::nearest_mm(circle.at.x), scene_gen::nearest_mm(circle.at.z), r * r});
  }
  if (!exclude.mask.empty()) {
    if (!read_mask(resolve(dir, exclude.mask), exclude.mask_min, exclude.mask_max, im.exclude_mask,
                   &why))
      return fail("its exclusion: " + why);
    im.exclude_threshold = static_cast<f64>(exclude.mask_threshold);
  }
  if (!exclude.layers.empty() && !layers_allowed)
    return fail(
        "it is excluded by another layer and excludes by layers itself; a layer an "
        "exclusion names may not");
  for (const ExcludeLayer& other : exclude.layers) {
    if (other.layer.empty()) return fail("an exclusion names a layer with no file");
    if (!(other.clearance >= 0.0f)) return fail("an exclusion's clearance must not be negative");
    const std::string path = resolve(dir, other.layer);
    std::string text;
    if (io::read_file(path, text) != io::Status::Ok)
      return fail("the excluded layer " + path + " cannot be read");
    JsonValue json;
    const JsonParseResult parsed = parse_json(text, json);
    if (!parsed.ok) return fail("the excluded layer " + path + " is not JSON: " + parsed.message);
    Layer other_desc;
    schema::ReadContext ctx;
    if (!schema::from_json(other_desc, json, ctx) || !ctx.ok())
      return fail("the excluded layer " + path + ": " + schema_errors(ctx));
    Impl::Excluded entry;
    entry.placer = std::make_unique<Placer>();
    if (!entry.placer->open(other_desc, io::parent_path(path), world_seed, &why, false))
      return fail("the excluded layer " + path + ": " + why);
    entry.clear_mm = to_mm(static_cast<f64>(other.clearance));
    entry.clear2 = entry.clear_mm * entry.clear_mm;
    im.excluded.push_back(std::move(entry));
  }
  if (!(exclude.mask_threshold >= 0.0f)) return fail("its exclusion mask's threshold is negative");
  return true;
}

bool Placer::occupies(scene_gen::TileCoord tile) const noexcept {
  const Impl& im = *impl_;
  const Region& region = im.layer.region;
  switch (region.kind) {
    case RegionKind::Scene: return true;
    case RegionKind::Tiles:
      return tile.x >= region.tile_min[0] && tile.x <= region.tile_max[0] &&
             tile.z >= region.tile_min[1] && tile.z <= region.tile_max[1];
    case RegionKind::Polygon: {
      // Whether the polygon reaches the tile: one of its corners in the tile, the tile's centre in
      // it, or one of its edges crossing one of the tile's — inclusive, so a tile it only touches
      // is asked and its points decided one by one. A bent corridor's bounding box is mostly
      // tiles it does not reach (desert-endless's: 62,000 tiles for 12,700).
      const f64 t = static_cast<f64>(im.tile_mm) / 1000.0;
      const f64 x0 = static_cast<f64>(tile.x) * t;
      const f64 z0 = static_cast<f64>(tile.z) * t;
      const f64 x1 = x0 + t;
      const f64 z1 = z0 + t;
      const u32 n = im.poly_x.size();
      for (u32 i = 0; i < n; ++i) {
        if (im.poly_x[i] >= x0 && im.poly_x[i] <= x1 && im.poly_z[i] >= z0 && im.poly_z[i] <= z1)
          return true;
      }
      if (im.in_region(scene_gen::nearest_mm(0.5 * (x0 + x1)),
                       scene_gen::nearest_mm(0.5 * (z0 + z1))))
        return true;
      const auto side = [](f64 px, f64 pz, f64 qx, f64 qz, f64 rx, f64 rz) {
        return (qx - px) * (rz - pz) - (qz - pz) * (rx - px);
      };
      const auto apart = [](f64 a, f64 b) { return (a < 0.0 && b < 0.0) || (a > 0.0 && b > 0.0); };
      const f64 rx[4] = {x0, x1, x1, x0};
      const f64 rz[4] = {z0, z0, z1, z1};
      for (u32 i = 0, j = n - 1; i < n; j = i++) {
        for (u32 e = 0; e < 4; ++e) {
          const u32 f = (e + 1) & 3u;
          const f64 ax = im.poly_x[j], az = im.poly_z[j], bx = im.poly_x[i], bz = im.poly_z[i];
          if (!apart(side(rx[e], rz[e], rx[f], rz[f], ax, az),
                     side(rx[e], rz[e], rx[f], rz[f], bx, bz)) &&
              !apart(side(ax, az, bx, bz, rx[e], rz[e]), side(ax, az, bx, bz, rx[f], rz[f])))
            return true;
        }
      }
      return false;
    }
  }
  return false;
}

bool Placer::region_tiles(Vector<scene_gen::TileCoord>& out, std::string* error) const {
  const Impl& im = *impl_;
  const Region& region = im.layer.region;
  i64 tx0 = 0, tx1 = -1, tz0 = 0, tz1 = -1;
  switch (region.kind) {
    case RegionKind::Scene:
      if (error != nullptr)
        *error =
            "its region is the whole scene, which has no whole to read: name tiles or a "
            "polygon, or stream it in a world";
      return false;
    case RegionKind::Tiles:
      tx0 = region.tile_min[0];
      tx1 = region.tile_max[0];
      tz0 = region.tile_min[1];
      tz1 = region.tile_max[1];
      break;
    case RegionKind::Polygon: {
      f64 lo_x = im.poly_x[0], hi_x = im.poly_x[0], lo_z = im.poly_z[0], hi_z = im.poly_z[0];
      for (u32 i = 1; i < im.poly_x.size(); ++i) {
        lo_x = std::min(lo_x, im.poly_x[i]);
        hi_x = std::max(hi_x, im.poly_x[i]);
        lo_z = std::min(lo_z, im.poly_z[i]);
        hi_z = std::max(hi_z, im.poly_z[i]);
      }
      tx0 = floor_div(scene_gen::nearest_mm(lo_x), im.tile_mm);
      tx1 = floor_div(scene_gen::nearest_mm(hi_x), im.tile_mm);
      tz0 = floor_div(scene_gen::nearest_mm(lo_z), im.tile_mm);
      tz1 = floor_div(scene_gen::nearest_mm(hi_z), im.tile_mm);
      break;
    }
  }
  const i64 count = (tx1 - tx0 + 1) * (tz1 - tz0 + 1);
  if (count > 16 * 1024 * 1024) {
    if (error != nullptr)
      *error = "its region covers " + std::to_string(count) +
               " tiles; a whole read takes at most 16,777,216";
    return false;
  }
  for (i64 z = tz0; z <= tz1; ++z) {
    for (i64 x = tx0; x <= tx1; ++x) {
      const scene_gen::TileCoord tile{static_cast<i32>(x), static_cast<i32>(z)};
      if (occupies(tile)) out.push_back(tile);
    }
  }
  return true;
}

void Placer::place_rect(const RectMm& rect, const scene_gen::Ground& ground,
                        Vector<Point>& out) const {
  const Impl& im = *impl_;
  const u32 first = out.size();
  im.place_upto(im.kinds.size(), rect, ground, out);
  std::sort(out.begin() + first, out.end(), [](const Point& a, const Point& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.z_mm != b.z_mm) return a.z_mm < b.z_mm;
    return a.x_mm < b.x_mm;
  });
}

void Placer::place_tile(scene_gen::TileCoord tile, const scene_gen::Ground& ground, u32 kind_mask,
                        Vector<Point>& out) const {
  const Impl& im = *impl_;
  if (!occupies(tile)) return;
  const RectMm rect{static_cast<i64>(tile.x) * im.tile_mm, static_cast<i64>(tile.z) * im.tile_mm,
                    (static_cast<i64>(tile.x) + 1) * im.tile_mm,
                    (static_cast<i64>(tile.z) + 1) * im.tile_mm};
  Vector<Point> all;
  place_rect(rect, ground, all);
  const u32 cap = im.layer.budget.per_tile;
  if (cap > 0 && all.size() > cap) {
    // The budget keeps the placements of lowest rank, a hash of each one's own candidate, so which
    // survive is the tile's and not the order they were made in; then back to the key order.
    std::sort(all.begin(), all.end(), [](const Point& a, const Point& b) {
      if (a.rank != b.rank) return a.rank < b.rank;
      if (a.kind != b.kind) return a.kind < b.kind;
      if (a.z_mm != b.z_mm) return a.z_mm < b.z_mm;
      return a.x_mm < b.x_mm;
    });
    all.resize(cap);
    std::sort(all.begin(), all.end(), [](const Point& a, const Point& b) {
      if (a.kind != b.kind) return a.kind < b.kind;
      if (a.z_mm != b.z_mm) return a.z_mm < b.z_mm;
      return a.x_mm < b.x_mm;
    });
  }
  for (const Point& p : all) {
    if ((kind_mask >> p.kind) & 1u) out.push_back(p);
  }
}

f64 Placer::density_at(i64 x_mm, i64 z_mm) const noexcept { return impl_->density(x_mm, z_mm); }

u32 Placer::ring_kind_mask(const scene_gen::Context& context) const noexcept {
  const Impl& im = *impl_;
  u32 mask = 0;
  f64 nearest = 0.0;
  if (context.world != nullptr && context.ring != scene_gen::k_no_ring && context.ring > 0 &&
      context.ring - 1u < context.world->rings.size()) {
    nearest = static_cast<f64>(context.world->rings[context.ring - 1u].radius) *
              static_cast<f64>(context.tile_size);
  }
  for (u32 k = 0; k < im.kinds.size(); ++k) {
    const f32 far = im.layer.kinds[k].far;
    if (far <= 0.0f || static_cast<f64>(far) > nearest) mask |= 1u << k;
  }
  return mask;
}

}  // namespace engine::scatter
