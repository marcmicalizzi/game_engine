// The rock solver (rocks.h; docs/subsystems/rocks.md, "The model by type").
//
// Every body is star-shaped about its centre: a cube-sphere grid of directions, each pushed out to
// the body's radius in that direction. The radius is the smooth minimum of the unit sphere's and
// every planar cut's (a cut is a plane of unit normal `n` at distance `d`; along a direction `u`
// it is `d / (u . n)` away), so the body before weathering is the convex intersection of a ball and
// half-spaces with the edges rounded; the warp and the surface noise scale that radius, the body
// is stretched to its half-extents, banded along its bedding, and lowered to its burial. A radial
// map of a sphere is closed and cannot fold, which is the whole of the topology argument.
#include "detail.h"

#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <domain/rocks/rocks.h>

#include <cmath>
#include <optional>
#include <string>

namespace engine::rocks {

namespace {

using namespace detail;

// One stream per (part, purpose), so a draw added to one never moves another.
enum class Stream : u64 { layout = 1, joints = 2, cuts = 3, noise = 4, bedding = 5 };
Rng stream(u64 seed, u32 part, Stream s) {
  return Rng(mix64(seed ^ mix64((static_cast<u64>(part) << 8) + static_cast<u64>(s))));
}

// ---- the model by type (rocks.md) --------------------------------------------------------------

struct TypeModel {
  u32 facets;        // seeded cuts beyond the joint sets
  f64 rounding;      // the default `rounding`
  u32 layers;        // the default `layers`
  f64 layer_depth;   // a bedding groove's depth, as a share of the smallest half-extent
  f64 warp;          // the low-frequency warp's amplitude, a share of the radius
  f64 grain;         // the surface noise's amplitude
  f64 grain_freq;    // and its first octave's frequency over the unit sphere
  bool pitted;       // the noise cuts pits (solution) rather than bumps
  bool column_ends;  // a pair of cuts across the columns, whatever the joint sets (basalt)
  Vec4 colour;       // linear
  f32 roughness;
};

TypeModel model_of(RockType type) {
  switch (type) {
    case RockType::Sandstone:
      return {2,   0.45, 7, 2.0, 0.03, 0.006, 9.0, false, false, Vec4{0.56f, 0.36f, 0.21f, 1.0f},
              0.9f};
    case RockType::Basalt:
      return {0,   0.1, 0, 0.0, 0.015, 0.008, 8.0, false, true, Vec4{0.09f, 0.09f, 0.10f, 1.0f},
              0.7f};
    case RockType::Limestone:
      return {2,    0.65, 3, 0.6, 0.06, 0.03, 5.0, true, false, Vec4{0.66f, 0.63f, 0.56f, 1.0f},
              0.85f};
    case RockType::Granite:
    default:
      return {3,    0.3, 0, 0.0, 0.035, 0.012, 7.0, false, false, Vec4{0.40f, 0.38f, 0.36f, 1.0f},
              0.75f};
  }
}

// The joint sets' directions, in the unit body's frame: granite's near-orthogonal, sandstone's
// bedding first, basalt's vertical at sixty degrees (a hexagonal prism) and then its column ends,
// limestone's like granite's but looser. Drawn from the rock's own stream, so an outcrop's
// boulders share them.
void joint_directions(RockType type, u32 sets, Rng& rng, D3 out[4]) {
  const f64 yaw = rng.range(0.0, 2.0 * k_pi);
  const f64 slack = type == RockType::Limestone ? 0.35 : (type == RockType::Basalt ? 0.04 : 0.15);
  const auto horizontal = [&](f64 a) {
    return normalized(D3{det::cos(a), rng.range(-slack, slack), det::sin(a)});
  };
  const auto up = [&]() {
    return normalized(D3{rng.range(-slack, slack), 1.0, rng.range(-slack, slack)});
  };
  for (u32 s = 0; s < 4; ++s) {
    if (s >= sets) {
      out[s] = D3{0.0, 1.0, 0.0};
      continue;
    }
    switch (type) {
      case RockType::Basalt:
        out[s] = s < 3 ? horizontal(yaw + static_cast<f64>(s) * k_pi / 3.0) : up();
        break;
      case RockType::Sandstone:
        out[s] = s == 0 ? up()
                        : (s < 3 ? horizontal(yaw + static_cast<f64>(s - 1) * k_pi * 0.5)
                                 : rng.direction());
        break;
      default:
        out[s] = s < 2 ? horizontal(yaw + static_cast<f64>(s) * k_pi * 0.5)
                       : (s == 2 ? up() : rng.direction());
        break;
    }
  }
}

struct Cut {
  D3 n;   // unit normal, in the unit body's frame
  f64 d;  // distance from the centre
};

// One body, everything its radius and its placement need.
struct Body {
  D3 half;        // half-extents
  D3 at;          // the body's centre in the mesh, x and z; y is set by the burial
  f64 yaw = 0.0;  // about +y
  f64 buried = 0.0;
  f64 smooth = 0.0;  // the smooth minimum's band, in the unit body
  Vector<Cut> cuts;
  f64 warp = 0.0;
  f64 grain = 0.0;
  f64 grain_freq = 1.0;
  bool pitted = false;
  bool column_ends = false;
  u32 layers = 0;
  f64 layer_depth = 0.0;  // metres
  D3 bedding{0.0, 1.0, 0.0};
  f64 layer_phase = 0.0;
  u64 noise_seed = 0;
  u32 n = 4;  // the rung
  f64 budget_share = 1.0;
};

// The body's radius along unit direction `u`, in the unit body, with the noise the rung carries.
f64 radius(const Body& b, D3 u) {
  f64 r = 1.0;
  for (const Cut& c : b.cuts) {
    const f64 along = dot(u, c.n);
    const f64 rc = along > 1e-6 ? c.d / along : 1.0e3;
    r = smooth_min(r, rc, b.smooth);
  }
  // The warp: one low octave, always (it is the body's shape, not its surface).
  r *= 1.0 + b.warp * (2.0 * noise3(b.noise_seed, u * 1.3 + D3{11.5, 3.25, 7.75}) - 1.0);
  // The grain: up to three octaves, each only where its wavelength spans four cells of the rung,
  // so a coarse rung is the same rock with the detail it cannot carry left out.
  f64 freq = b.grain_freq;
  f64 amp = b.grain;
  const f64 limit = static_cast<f64>(b.n) * 0.25;
  for (u32 o = 0; o < 3 && freq <= limit; ++o) {
    const f64 v = noise3(b.noise_seed + 0x9E37u * (o + 1), u * freq);
    r *= b.pitted ? 1.0 - amp * std::fabs(2.0 * v - 1.0) : 1.0 + amp * (2.0 * v - 1.0);
    freq *= 2.0;
    amp *= 0.5;
  }
  return r > 0.2 ? r : 0.2;
}

// The cube-sphere's direction for a lattice point of the cube [0, n]³'s surface: each coordinate
// mapped to [-1, 1] and through the equal-angle warp, `tan(q pi / 4)`, so the cells are near
// square on the sphere. A function of the lattice point alone, so the two faces that share an edge
// compute the same bits for it and the seams weld by position exactly.
D3 lattice_direction(i64 x, i64 y, i64 z, u32 n) {
  const auto warp = [n](i64 c) {
    if (c == 0) return -1.0;
    if (c == static_cast<i64>(n)) return 1.0;
    const f64 q = (2.0 * static_cast<f64>(c) / static_cast<f64>(n) - 1.0) * (k_pi * 0.25);
    return det::sin(q) / det::cos(q);
  };
  return normalized(D3{warp(x), warp(y), warp(z)});
}

// Emits one body: its six charts' grids, outward-wound, with area-weighted normals shared across
// the charts' seams, box-projected UVs (rocks.md, "UVs and materials"), and the burial applied.
void emit_body(const Body& b, f64 tile_m, assets::MeshData& out) {
  const u32 n = b.n;
  const u32 side = n + 1;
  const u32 first = out.positions.size();
  // The surface's positions by lattice point first, then the charts index into them.
  Vector<D3> local;
  Vector<u32> lattice_of;  // per emitted vertex, its lattice point's slot in `local`
  Vector<u32> slot(side * side * side, ~0u);
  const f64 cell = (k_pi * 0.5) *
                   (b.half.x > b.half.y ? (b.half.x > b.half.z ? b.half.x : b.half.z)
                                        : (b.half.y > b.half.z ? b.half.y : b.half.z)) /
                   static_cast<f64>(n);
  const f64 spacing =
      b.layers > 0 ? 2.0 *
                         std::fabs(dot(b.half, D3{std::fabs(b.bedding.x), std::fabs(b.bedding.y),
                                                  std::fabs(b.bedding.z)})) /
                         static_cast<f64>(b.layers)
                   : 0.0;
  // A groove narrower than four cells of the rung fades out rather than aliasing.
  const f64 groove = b.layers > 0 ? b.layer_depth * clamp01((spacing / cell - 2.0) * 0.5) : 0.0;
  // The unit body's point along a lattice direction; stretched to the half-extents below.
  const auto position_of = [&](i64 x, i64 y, i64 z) {
    const D3 u = lattice_direction(x, y, z, n);
    return u * radius(b, u);
  };
  // Faces: (axis, sign) with in-face axes (b, c) such that b x c points outwards.
  struct Face {
    u32 axis;
    i64 sign;
    u32 b;
    u32 c;
  };
  constexpr Face k_faces[6] = {{0, 1, 1, 2},  {0, -1, 2, 1}, {1, 1, 2, 0},
                               {1, -1, 0, 2}, {2, 1, 0, 1},  {2, -1, 1, 0}};
  const f64 halves[3] = {b.half.x, b.half.y, b.half.z};
  // The charts in a 3 x 2 grid centred on the origin, each in a cell of a whole, even number of
  // repeats — so every chart starts on a whole repeat (a tiled set samples each from the same
  // phase), no two overlap in the atlas, and the coordinates stay as near zero as the layout
  // allows (the content build warns past 16: `geometry.uv_range`).
  const f64 widest = k_pi * 0.5 *
                     (halves[0] > halves[1] ? (halves[0] > halves[2] ? halves[0] : halves[2])
                                            : (halves[1] > halves[2] ? halves[1] : halves[2])) /
                     tile_m;
  const f64 grid_cell = 2.0 * std::ceil((widest + 1.0) * 0.5);
  for (u32 f = 0; f < 6; ++f) {
    const Face& face = k_faces[f];
    // The chart's extent in repeats: a quarter circumference of each in-face half-extent.
    const f64 extent_b = k_pi * 0.5 * halves[face.b] / tile_m;
    const f64 extent_c = k_pi * 0.5 * halves[face.c] / tile_m;
    const f64 chart_u = (static_cast<f64>(f % 3u) - 1.5) * grid_cell;
    const f64 chart_v = (static_cast<f64>(f / 3u) - 1.0) * grid_cell;
    const u32 face_first = out.positions.size();
    for (u32 j = 0; j <= n; ++j) {
      for (u32 i = 0; i <= n; ++i) {
        i64 c[3];
        c[face.axis] = face.sign > 0 ? static_cast<i64>(n) : 0;
        c[face.b] = static_cast<i64>(i);
        c[face.c] = static_cast<i64>(j);
        const u32 key = static_cast<u32>(c[0]) * side * side + static_cast<u32>(c[1]) * side +
                        static_cast<u32>(c[2]);
        if (slot[key] == ~0u) {
          slot[key] = local.size();
          local.push_back(position_of(c[0], c[1], c[2]));
        }
        lattice_of.push_back(slot[key]);
        out.positions.push_back(Vec3{});
        out.normals.push_back(Vec3{});
        out.uvs.push_back(
            Vec2{static_cast<f32>(chart_u + extent_b * static_cast<f64>(i) / static_cast<f64>(n)),
                 static_cast<f32>(chart_v + extent_c * static_cast<f64>(j) / static_cast<f64>(n))});
      }
    }
    for (u32 j = 0; j < n; ++j) {
      for (u32 i = 0; i < n; ++i) {
        const u32 a = face_first + j * side + i;
        const u32 bq = a + 1;
        const u32 cq = a + side + 1;
        const u32 d = a + side;
        // Alternate the diagonal in a checkerboard, so no direction across a face is favoured.
        if (((i + j) & 1u) == 0u) {
          out.indices.push_back(a);
          out.indices.push_back(bq);
          out.indices.push_back(cq);
          out.indices.push_back(a);
          out.indices.push_back(cq);
          out.indices.push_back(d);
        } else {
          out.indices.push_back(a);
          out.indices.push_back(bq);
          out.indices.push_back(d);
          out.indices.push_back(bq);
          out.indices.push_back(cq);
          out.indices.push_back(d);
        }
      }
    }
  }
  // The stretch: the cut body is smaller than the ball it was cut from, so each axis is scaled to
  // make the body's box the half-extents asked (an affine map, so a cut stays a plane), centred.
  {
    D3 lo_q{1e30, 1e30, 1e30};
    D3 hi_q{-1e30, -1e30, -1e30};
    for (const D3& q : local) {
      lo_q =
          D3{q.x < lo_q.x ? q.x : lo_q.x, q.y < lo_q.y ? q.y : lo_q.y, q.z < lo_q.z ? q.z : lo_q.z};
      hi_q =
          D3{q.x > hi_q.x ? q.x : hi_q.x, q.y > hi_q.y ? q.y : hi_q.y, q.z > hi_q.z ? q.z : hi_q.z};
    }
    const D3 mid = (lo_q + hi_q) * 0.5;
    const D3 scale{2.0 * b.half.x / (hi_q.x - lo_q.x), 2.0 * b.half.y / (hi_q.y - lo_q.y),
                   2.0 * b.half.z / (hi_q.z - lo_q.z)};
    for (D3& q : local) {
      D3 p{(q.x - mid.x) * scale.x, (q.y - mid.y) * scale.y, (q.z - mid.z) * scale.z};
      if (groove > 0.0) {
        // A groove at every bedding plane, its depth differing bed by bed (the beds weather at
        // their own rates) and the planes a little wavy, as real beds are.
        const f64 t = dot(p, b.bedding) / spacing + b.layer_phase +
                      0.3 * (noise3(b.noise_seed + 0x5EDu, p * (0.4 / spacing)) - 0.5);
        const f64 bed = std::floor(t);
        const f64 f = t - bed;
        f64 g = (2.0 * f - 1.0) * (2.0 * f - 1.0);
        g = g * g;
        g = g * g;
        const f64 depth = 0.35 + 0.65 * lattice_value(b.noise_seed, static_cast<i64>(bed), 17, 29);
        const f64 l = length(p);
        const f64 s = l > 0.0 ? 1.0 - groove * depth * g / l : 1.0;
        p = p * (s > 0.3 ? s : 0.3);
      }
      q = p;
    }
  }
  // The burial: the share `buried` of the body's height under y = 0.
  f64 lo = 1e30;
  f64 hi = -1e30;
  for (const D3& p : local) {
    lo = p.y < lo ? p.y : lo;
    hi = p.y > hi ? p.y : hi;
  }
  const f64 lift = -(lo + b.buried * (hi - lo));
  const f64 cy = det::cos(b.yaw);
  const f64 sy = det::sin(b.yaw);
  Vector<D3> placed(local.size());
  for (u32 v = 0; v < local.size(); ++v) {
    const D3 p = local[v];
    placed[v] = D3{b.at.x + p.x * cy + p.z * sy, p.y + lift, b.at.z - p.x * sy + p.z * cy};
  }
  // Area-weighted normals over the welded surface, so a seam is invisible in the shading.
  Vector<D3> normal(local.size());
  const u32 index_first = out.indices.size() - 36u * n * n;
  for (u32 t = index_first; t < out.indices.size(); t += 3) {
    const u32 ia = lattice_of[out.indices[t] - first];
    const u32 ib = lattice_of[out.indices[t + 1] - first];
    const u32 ic = lattice_of[out.indices[t + 2] - first];
    const D3 fn = cross(placed[ib] - placed[ia], placed[ic] - placed[ia]);
    normal[ia] = normal[ia] + fn;
    normal[ib] = normal[ib] + fn;
    normal[ic] = normal[ic] + fn;
  }
  for (u32 v = first; v < out.positions.size(); ++v) {
    const u32 s = lattice_of[v - first];
    const D3 p = placed[s];
    const D3 nn = normalized(normal[s]);
    out.positions[v] = Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
    out.normals[v] = Vec3{static_cast<f32>(nn.x), static_cast<f32>(nn.y), static_cast<f32>(nn.z)};
  }
}

// The common weathering of a body from the file's type and overrides.
struct Weather {
  TypeModel model;
  f64 rounding;
  u32 layers;
};

Weather weather_of(RockType type, const std::optional<f32>& rounding,
                   const std::optional<u32>& layers) {
  Weather w{model_of(type), 0.0, 0};
  w.rounding = rounding.has_value() ? static_cast<f64>(*rounding) : w.model.rounding;
  w.layers = layers.has_value() ? *layers : w.model.layers;
  return w;
}

void weather_body(Body& b, const Weather& w, u64 seed, u32 part) {
  b.smooth = w.rounding * 0.25;
  b.column_ends = w.model.column_ends;
  b.warp = w.model.warp;
  b.grain = w.model.grain;
  b.grain_freq = w.model.grain_freq;
  b.pitted = w.model.pitted;
  b.layers = w.layers;
  const f64 smallest = b.half.x < b.half.y ? (b.half.x < b.half.z ? b.half.x : b.half.z)
                                           : (b.half.y < b.half.z ? b.half.y : b.half.z);
  b.layer_depth = 0.04 * w.model.layer_depth * smallest;
  if (b.layers > 0 && b.layer_depth <= 0.0) b.layer_depth = 0.04 * smallest;
  Rng noise = stream(seed, part, Stream::noise);
  b.noise_seed = noise.next();
  b.layer_phase = noise.unit();
}

// Joint cuts (a pair of planes on opposite sides per set) at seeded distances, then the type's
// facets in seeded directions.
void cut_body(Body& b, const D3 joints[4], u32 sets, u32 facets, u64 seed, u32 part, f64 lo,
              f64 hi) {
  Rng rng = stream(seed, part, Stream::cuts);
  for (u32 s = 0; s < sets; ++s) {
    b.cuts.push_back(Cut{joints[s], rng.range(lo, hi)});
    b.cuts.push_back(Cut{joints[s] * -1.0, rng.range(lo, hi)});
  }
  for (u32 f = 0; f < facets; ++f)
    b.cuts.push_back(Cut{rng.direction(), rng.range(lo + 0.08, hi + 0.05)});
  if (b.column_ends) {
    const D3 up = normalized(D3{rng.range(-0.05, 0.05), 1.0, rng.range(-0.05, 0.05)});
    b.cuts.push_back(Cut{up, rng.range(lo, hi)});
    b.cuts.push_back(Cut{up * -1.0, rng.range(lo, hi)});
  }
}

// The finest rung whose triangles fit `budget`; false when not even the coarsest does.
bool rung_for(f64 budget, u32& n, u32& step) {
  for (u32 r = k_grid_rung_count; r-- > 0;) {
    if (static_cast<f64>(triangles_of_grid(k_grid_rungs[r])) <= budget) {
      n = k_grid_rungs[r];
      step = k_grid_rung_count - 1 - r;
      return true;
    }
  }
  return false;
}

f64 surface_weight(D3 h) { return h.x * h.y + h.y * h.z + h.x * h.z; }

assets::Material material_of(RockType type) {
  const TypeModel m = model_of(type);
  assets::Material out;
  out.name = rock_type_name(type);
  out.base_color = m.colour;
  out.metallic = 0.0f;
  out.roughness = m.roughness;
  out.double_sided = false;
  return out;
}

// Spends the budget over the parts: every part its coarsest rung first, so a small fragment is
// never priced out by a large one, and what is left by surface, each part then on the finest rung
// its share holds. The shares sum to the budget and each rung fits its share, so the whole does.
bool spend(Vector<Body>& bodies, u32 budget, RockReport& report, std::string& error,
           const char* field) {
  const u32 floor_triangles = triangles_of_grid(k_grid_rungs[0]);
  if (static_cast<u64>(floor_triangles) * bodies.size() > budget) {
    error = std::string(field) + " " + std::to_string(budget) + " cannot hold " +
            std::to_string(bodies.size()) + " part" + (bodies.size() == 1 ? "" : "s") +
            " at the coarsest rung: that needs " +
            std::to_string(static_cast<u64>(floor_triangles) * bodies.size()) + " triangles";
    return false;
  }
  f64 total = 0.0;
  for (const Body& b : bodies)
    total += surface_weight(b.half);
  const f64 rest = static_cast<f64>(budget - floor_triangles * bodies.size());
  report.finest_grid = 0;
  report.coarsest_grid = ~0u;
  report.steps = 0;
  for (Body& b : bodies) {
    u32 step = 0;
    const f64 share = static_cast<f64>(floor_triangles) + rest * surface_weight(b.half) / total;
    if (!rung_for(share, b.n, step)) {
      b.n = k_grid_rungs[0];  // rounding at the floor: the coarsest rung is what was reserved
      step = k_grid_rung_count - 1;
    }
    if (b.n > report.finest_grid) {
      report.finest_grid = b.n;
      report.steps = step;
    }
    if (b.n < report.coarsest_grid) report.coarsest_grid = b.n;
  }
  return true;
}

}  // namespace

assets::Material detail::rock_material(RockType type) { return material_of(type); }

// A fallen block is a scree fragment (above, `solve_rock`'s scree): the type's weathering
// sharpened to 40% of its rounding, unbedded, three joint sets and two facets of its own cut at
// 0.42-0.75, on the rung the cliff's budget gave it, then moved onto the skirt.
void detail::emit_fragment(RockType type, D3 half, D3 at, f64 yaw, f64 buried, u64 seed, u32 n,
                           f64 tile_m, assets::MeshData& out) {
  Weather weather = weather_of(type, std::nullopt, std::optional<u32>(0u));
  weather.rounding *= 0.4;
  Body b;
  b.half = half;
  b.yaw = yaw;
  b.buried = buried;
  b.n = n;
  weather_body(b, weather, seed, 1);
  Rng joint_rng = stream(seed, 1, Stream::joints);
  D3 joints[4];
  joint_directions(type, 3, joint_rng, joints);
  cut_body(b, joints, 3, weather.model.facets + 2, seed, 1, 0.42, 0.75);
  const u32 first = out.positions.size();
  emit_body(b, tile_m, out);
  for (u32 v = first; v < out.positions.size(); ++v) {
    const Vec3 p = out.positions[v];
    out.positions[v] = Vec3{static_cast<f32>(wide(p.x) + at.x), static_cast<f32>(wide(p.y) + at.y),
                            static_cast<f32>(wide(p.z) + at.z)};
  }
}

const char* rock_type_name(RockType type) noexcept {
  switch (type) {
    case RockType::Granite: return "Granite";
    case RockType::Sandstone: return "Sandstone";
    case RockType::Basalt: return "Basalt";
    case RockType::Limestone: return "Limestone";
  }
  return "Granite";
}

const char* rock_shape_name(RockShape shape) noexcept {
  switch (shape) {
    case RockShape::Boulder: return "Boulder";
    case RockShape::Outcrop: return "Outcrop";
    case RockShape::Scree: return "Scree";
    case RockShape::Cliff: return "Cliff";
  }
  return "Boulder";
}

bool solve_rock(const RockFile& file, assets::MeshData& out, RockReport* report_out,
                std::string* error_out) {
  out = assets::MeshData{};
  std::string error;
  if (!rock_file_in_range(file, error)) {
    if (error_out != nullptr) *error_out = error;
    return false;
  }
  // A cliff is a face along the ground, not a radial body (rocks.md, "Cliffs"): cliff.cpp.
  if (file.shape == RockShape::Cliff) {
    CliffReport cliff_report;
    const bool solved = solve_cliff(file.cliff.value_or(Cliff{}), out, &cliff_report, error_out);
    if (solved && report_out != nullptr) {
      *report_out = RockReport{};
      report_out->triangles = cliff_report.triangles;
      report_out->vertices = cliff_report.vertices;
      report_out->parts = cliff_report.parts;
      report_out->steps = cliff_report.steps;
      report_out->cuts = cliff_report.joint_sets;
      report_out->layers = cliff_report.beds;
      report_out->rounding = cliff_report.rounding;
    }
    return solved;
  }
  RockReport report;
  Vector<Body> bodies;
  RockType type = RockType::Granite;
  f64 tile_m = 1.0;
  u32 budget = 0;
  const char* budget_field = "";
  Weather weather{};

  if (file.shape == RockShape::Boulder) {
    const Boulder p = file.boulder.value_or(Boulder{});
    type = p.type;
    tile_m = wide(p.material.tile_m);
    budget = p.triangle_budget;
    budget_field = "engine.rocks.Boulder.triangle_budget";
    weather = weather_of(p.type, p.rounding, p.layers);
    Rng joint_rng = stream(p.seed, 0, Stream::joints);
    D3 joints[4];
    joint_directions(p.type, p.joint_sets, joint_rng, joints);
    Body b;
    b.half = D3{wide(p.size_x), wide(p.size_y), wide(p.size_z)};
    b.buried = wide(p.buried);
    weather_body(b, weather, p.seed, 0);
    cut_body(b, joints, p.joint_sets, weather.model.facets, p.seed, 0, 0.5, 0.75);
    Rng bed = stream(p.seed, 0, Stream::bedding);
    b.bedding = normalized(D3{bed.range(-0.15, 0.15), 1.0, bed.range(-0.15, 0.15)});
    bodies.push_back(std::move(b));
  } else if (file.shape == RockShape::Outcrop) {
    const Outcrop p = file.outcrop.value_or(Outcrop{});
    type = p.type;
    tile_m = wide(p.material.tile_m);
    budget = p.triangle_budget;
    budget_field = "engine.rocks.Outcrop.triangle_budget";
    weather = weather_of(p.type, p.rounding, p.layers);
    // One joint set and one bedding direction for every boulder: one rock mass, broken.
    Rng joint_rng = stream(p.seed, 0, Stream::joints);
    D3 joints[4];
    joint_directions(p.type, p.joint_sets, joint_rng, joints);
    Rng bed = stream(p.seed, 0, Stream::bedding);
    const D3 bedding = normalized(D3{bed.range(-0.12, 0.12), 1.0, bed.range(-0.12, 0.12)});
    const f64 phase = bed.unit();
    Rng layout = stream(p.seed, 0, Stream::layout);
    const f64 up = 1.0 / (2.0 * (1.0 - wide(p.buried)));
    for (u32 i = 0; i < p.count; ++i) {
      // The largest in the middle, the rest smaller the further down the list, round it.
      const f64 t = p.count > 1 ? static_cast<f64>(i) / static_cast<f64>(p.count - 1) : 0.0;
      const f64 s = 1.0 - 0.55 * t * layout.range(0.7, 1.0);
      Body b;
      b.half = D3{wide(p.size_x) * 0.5 * s * layout.range(0.8, 1.1),
                  wide(p.size_y) * s * up * layout.range(0.85, 1.1),
                  wide(p.size_z) * 0.5 * s * layout.range(0.8, 1.1)};
      if (i > 0) {
        const f64 a = layout.range(0.0, 2.0 * k_pi);
        const f64 r = layout.range(0.45, 0.95);
        b.at = D3{det::cos(a) * r * (wide(p.size_x) - b.half.x * 0.6), 0.0,
                  det::sin(a) * r * (wide(p.size_z) - b.half.z * 0.6)};
      }
      b.buried = wide(p.buried);
      weather_body(b, weather, p.seed, i + 1);
      b.bedding = bedding;
      b.layer_phase = phase + dot(b.at, bedding);  // the beds run on through the boulders
      cut_body(b, joints, p.joint_sets, weather.model.facets, p.seed, i + 1, 0.48, 0.72);
      bodies.push_back(std::move(b));
    }
  } else {
    const Scree p = file.scree.value_or(Scree{});
    type = p.type;
    tile_m = wide(p.material.tile_m);
    budget = p.triangle_budget;
    budget_field = "engine.rocks.Scree.triangle_budget";
    // Fragments are freshly broken: sharper than the type's weathered boulders, unbedded.
    weather = weather_of(p.type, p.rounding, std::optional<u32>(0u));
    if (!p.rounding.has_value()) weather.rounding *= 0.4;
    Rng layout = stream(p.seed, 0, Stream::layout);
    Vector<D3> centres;
    Vector<f64> radii;
    for (u32 i = 0; i < p.count; ++i) {
      const f64 size = wide(p.fragment_m) * (i == 0 ? 1.0 : layout.range(0.35, 0.95));
      Body b;
      b.half = D3{size * layout.range(0.7, 1.0), size * layout.range(0.35, 0.6),
                  size * layout.range(0.6, 1.0)};
      // Spread over the footprint, a few tries apart from those already placed.
      D3 at{};
      for (u32 attempt = 0; attempt < 8; ++attempt) {
        at = D3{layout.range(-1.0, 1.0) * (wide(p.size_x) - size * 0.5), 0.0,
                layout.range(-1.0, 1.0) * (wide(p.size_z) - size * 0.5)};
        bool clear = true;
        for (u32 k = 0; k < centres.size() && clear; ++k) {
          const D3 d = at - centres[k];
          clear = length(d) >= 0.8 * (size + radii[k]);
        }
        if (clear) break;
      }
      centres.push_back(at);
      radii.push_back(size);
      b.at = at;
      b.yaw = layout.range(0.0, 2.0 * k_pi);
      b.buried = wide(p.buried);
      weather_body(b, weather, p.seed, i + 1);
      Rng joint_rng = stream(p.seed, i + 1, Stream::joints);
      D3 joints[4];
      joint_directions(p.type, 3, joint_rng, joints);
      cut_body(b, joints, 3, weather.model.facets + 2, p.seed, i + 1, 0.42, 0.75);
      bodies.push_back(std::move(b));
    }
  }

  if (!spend(bodies, budget, report, error, budget_field)) {
    if (error_out != nullptr) *error_out = error;
    return false;
  }
  out.materials.push_back(material_of(type));
  for (const Body& b : bodies) {
    emit_body(b, tile_m, out);
    report.cuts += b.cuts.size();
  }
  assets::Primitive part;
  part.first_index = 0;
  part.index_count = out.indices.size();
  part.material = 0;
  out.primitives.push_back(part);

  report.triangles = out.indices.size() / 3;
  report.vertices = out.positions.size();
  report.parts = bodies.size();
  report.layers = weather.layers;
  report.rounding = static_cast<f32>(weather.rounding);
  if (report_out != nullptr) *report_out = report;
  return true;
}

}  // namespace engine::rocks
