// The rock solver (rocks.h; docs/subsystems/rocks.md, "The model by type").
//
// Every body is star-shaped about its centre: a cube-sphere grid of directions, each pushed out to
// the body's radius in that direction. The radius is the smooth minimum of an enclosing hull's and
// every planar cut's (a cut is a plane of unit normal `n` at distance `d`; along a direction `u`
// it is `d / (u . n)` away), so the body before weathering is the convex intersection of enclosing
// half-spaces with the edges rounded; the warp and the surface noise scale that radius, the body
// is stretched to its half-extents, banded along its bedding, and lowered to its burial. A radial
// map of a sphere is closed and cannot fold, which is the whole of the topology argument.
#include "detail.h"
#include "geology.h"
#include "surface_bake.h"

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
    case RockType::Shale:
      return {5,   0.12, 0, 0.0, 0.07, 0.008, 10.0, false, false, Vec4{0.20f, 0.19f, 0.17f, 1.0f},
              0.9f};
    case RockType::Slate:
      return {4,    0.06, 0, 0.0, 0.025, 0.003, 12.0, false, false, Vec4{0.17f, 0.22f, 0.24f, 1.0f},
              0.65f};
    case RockType::Schist:
      return {5,   0.14, 0, 0.0, 0.08, 0.008, 10.0, false, false, Vec4{0.32f, 0.34f, 0.30f, 1.0f},
              0.6f};
    case RockType::Gneiss:
      return {4,    0.22, 0, 0.0, 0.08, 0.008, 8.0, false, false, Vec4{0.39f, 0.37f, 0.35f, 1.0f},
              0.72f};
    case RockType::Marble:
      return {3,    0.35, 0, 0.0, 0.04, 0.004, 8.0, true, false, Vec4{0.72f, 0.69f, 0.63f, 1.0f},
              0.46f};
    case RockType::Obsidian:
      return {6,     0.025, 0,     0.0,   0.025,
              0.001, 8.0,   false, false, Vec4{0.025f, 0.031f, 0.038f, 1.0f},
              0.16f};
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
      case RockType::Shale:
      case RockType::Slate:
      case RockType::Schist:
      case RockType::Gneiss:
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
  D3 n;            // unit normal, in the unit body's frame
  f64 d;           // distance from the centre
  f64 wear = 1.0;  // old joints round; fresh fracture margins remain sharp
};

struct Scar {
  D3 direction;
  f64 width;
  f64 depth;
};

struct Chip {
  Cut planes[4];
  f64 width = 0.0;
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
  RockSurface surface;
  D3 joints[4]{};
  u32 joint_count = 0;
  u64 surface_seed = 0;
  Fabric fabric{};
  Scar scars[6]{};
  Chip chips[16]{};
};

// Physical-scale structure, evaluated in the common formation frame. Joint positions are
// jittered along each set rather than redrawn per vertex or per body. Every feature is filtered
// by its own width (not the distance between cracks), preventing sub-cell razor trenches.
f64 surface_recess(const Body& b, D3 p, f64 cell) {
  const RockSurface& s = b.surface;
  if (s.strength == 0.0f) return 0.0;
  const auto resolved = [cell](f64 width) { return fade(clamp01((width / cell - 2.0) * 0.5)); };
  const f64 spacing = wide(s.joint_spacing_m);
  const f64 width = wide(s.joint_width_m);
  const f64 joint_weight = resolved(width);
  f64 joint = 0.0;
  for (u32 set = 0; set < b.joint_count && joint_weight > 0.0; ++set) {
    const f64 coordinate = dot(p, b.joints[set]) / spacing;
    const i64 nearest = static_cast<i64>(std::floor(coordinate));
    // A plane lies within +/-0.3 spacings of its integer station; these three suffice.
    for (i64 k = nearest - 1; k <= nearest + 1; ++k) {
      const f64 station =
          static_cast<f64>(k) + 0.6 * (lattice_value(b.surface_seed, k, set, 0) - 0.5);
      const f64 wandering =
          0.08 * (noise3(b.surface_seed + set + 0x71u, p * (0.7 / spacing)) - 0.5);
      const f64 distance = std::fabs(coordinate - station + wandering) * spacing;
      const f64 profile = clamp01(1.0 - distance / (0.5 * width));
      // Unequal penetration along a joint leaves intact bridges and chipped intersections.
      const f64 exposure = noise3(b.surface_seed + set, p * (0.9 / spacing));
      const f64 penetration = fade(clamp01((exposure - 0.25) / 0.5));
      const f64 own_depth = 0.35 + 0.65 * lattice_value(b.surface_seed, k, set, 1);
      const f64 recess = wide(s.joint_depth_m) * joint_weight * profile * profile *
                         (3.0 - 2.0 * profile) * penetration * own_depth;
      if (recess > joint) joint = recess;
    }
  }
  f64 relief = 0.0;
  f64 wavelength = wide(s.wavelength_m);
  f64 amplitude = wide(s.relief_m);
  // Five octaves are a representation bound, not a geological smallest feature. The material
  // carries finer scales. The 0.6 amplitude ratio is an explicit authoring approximation.
  for (u32 octave = 0; octave < 5; ++octave) {
    const f64 weight = resolved(wavelength);
    if (weight == 0.0) break;
    const f64 v = noise3(b.surface_seed + 0x91u * (octave + 1), p * (1.0 / wavelength));
    relief += amplitude * weight * (b.pitted ? std::fabs(2.0 * v - 1.0) : 2.0 * v - 1.0);
    wavelength *= 0.5;
    amplitude *= 0.6;
  }
  return wide(s.strength) * (joint + relief);
}

// The body's radius along unit direction `u`, in the unit body, with the noise the rung carries.
// Match smooth_min's ownership blend without perturbing the radius arithmetic. A mask is
// bake-only provenance: support/joint faces are old; the low-wear facets are recent breaks.
f64 cut_blend(f64 a, f64 b, f64 k) {
  return k > 0.0 ? clamp01(0.5 + 0.5 * (b - a) / k) : (a < b ? 1.0 : 0.0);
}

f64 hull_radius(const Body& b, D3 u, f64* fresh = nullptr) {
  f64 r = 1.0e3;
  if (fresh != nullptr) *fresh = 0.0;
  for (const Cut& c : b.cuts) {
    const f64 along = dot(u, c.n);
    const f64 rc = along > 1e-6 ? c.d / along : 1.0e3;
    if (fresh != nullptr) {
      const f64 keep = cut_blend(r, rc, b.smooth * c.wear);
      *fresh = *fresh * keep + (1.0 - c.wear) * (1.0 - keep);
    }
    r = smooth_min(r, rc, b.smooth * c.wear);
  }
  return r;
}

f64 radius(const Body& b, D3 u, f64* fresh = nullptr) {
  f64 r = hull_radius(b, u, fresh);
  // A finite removed patch is the intersection of four outward half-spaces. Along a ray,
  // entering all four takes the greatest plane distance; taking the minimum with the hull
  // removes just that patch. A plane facing away makes the chip absent on this ray.
  const f64 cell = (k_pi * 0.5) / static_cast<f64>(b.n);
  for (const Chip& chip : b.chips) {
    const f64 weight = fade(clamp01((chip.width / cell - 2.0) * 0.5));
    if (weight == 0.0) continue;
    f64 entry = 0.0;
    for (const Cut& plane : chip.planes) {
      const f64 along = dot(u, plane.n);
      const f64 distance = along > 1e-6 ? plane.d / along : 1.0e3;
      if (distance > entry) entry = distance;
    }
    const f64 chipped = smooth_min(r, entry, b.smooth * 0.12);
    if (fresh != nullptr) {
      const f64 keep = cut_blend(r, entry, b.smooth * 0.12);
      *fresh = lerp(*fresh, *fresh * keep + (1.0 - keep), weight);
    }
    r = lerp(r, chipped, weight);
  }
  // Broad asymmetric removal patches break the regular rounded-box silhouette. Scars stay
  // present across quality rungs; a rounded bowl can end at a narrow, freshly broken lip.
  for (const Scar& scar : b.scars) {
    const f64 cap = clamp01((dot(u, scar.direction) - (1.0 - scar.width)) / scar.width);
    const f64 lip = 0.06 + 0.22 * b.fabric.exposure;
    const f64 rim = fade(clamp01(cap / lip));
    const f64 bowl = 0.45 + 0.55 * cap;
    r -= scar.depth * rim * bowl;
    if (fresh != nullptr && scar.depth > 0.0) *fresh = lerp(*fresh, 1.0, rim);
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
// offset by at most a tenth of a cell before the equal-angle warp, `tan(q pi / 4)`. Fixed
// boundary coordinates keep every sample on its chart. A function of the lattice point alone,
// so the two faces that share an edge
// compute the same bits for it and the seams weld by position exactly.
D3 lattice_direction(i64 x, i64 y, i64 z, u32 n, u64 seed) {
  const auto warp = [=](i64 c, u64 axis) {
    if (c == 0) return -1.0;
    if (c == static_cast<i64>(n)) return 1.0;
    const f64 offset = 0.1 * (2.0 * lattice_value(seed + axis, x, y, z) - 1.0);
    const f64 q =
        (2.0 * (static_cast<f64>(c) + offset) / static_cast<f64>(n) - 1.0) * (k_pi * 0.25);
    return det::sin(q) / det::cos(q);
  };
  return normalized(D3{warp(x, 11), warp(y, 23), warp(z, 37)});
}

struct SurfacePlane {
  const Cut* cut = nullptr;
  const Cut* inside = nullptr;  // second max plane inside a finite chip
  f64 distance = 1e3;
};
struct FracturePlanes {
  SurfacePlane first, second;
};

FracturePlanes fracture_planes(const Body& b, D3 u) {
  FracturePlanes result;
  const auto insert = [&](SurfacePlane plane) {
    if (plane.distance < result.first.distance) {
      result.second = result.first;
      result.first = plane;
    } else if (plane.distance < result.second.distance) {
      result.second = plane;
    }
  };
  for (const Cut& cut : b.cuts) {
    const f64 along = dot(u, cut.n);
    if (along <= 1e-6) continue;
    insert({&cut, nullptr, cut.d / along});
  }
  const f64 cell = (k_pi * 0.5) / b.n;
  for (const Chip& chip : b.chips) {
    // A fading chip has no sharp Boolean intersection to fit. Leave it to the sampler.
    if (chip.width < 4.0 * cell) continue;
    SurfacePlane entry{nullptr, nullptr, 0.0};
    f64 runner = 0.0;
    for (const Cut& cut : chip.planes) {
      const f64 along = dot(u, cut.n);
      const f64 distance = along > 1e-6 ? cut.d / along : 1e3;
      if (distance > entry.distance) {
        entry.inside = entry.cut;
        runner = entry.distance;
        entry.cut = &cut;
        entry.distance = distance;
      } else if (distance > runner) {
        entry.inside = &cut;
        runner = distance;
      }
    }
    insert(entry);
  }
  return result;
}

// A boundary of two planes is a great circle: u.(d0*n1-d1*n0)=0. Test both the
// outer minimum and the winning chip's inner maximum; hidden cutting planes never attract.
D3 rim_direction(const Body& b, D3 u) {
  const FracturePlanes planes = fracture_planes(b, u);
  if (planes.first.cut == nullptr) return u;
  const Cut& first = *planes.first.cut;
  const f64 cell = (k_pi * 0.5) / b.n;
  f64 nearest = 0.45 * cell;
  D3 chosen = u;
  for (const Cut* other : {planes.second.cut, planes.first.inside}) {
    if (other == nullptr) continue;
    const f64 fit = clamp01(1.0 - b.smooth * std::min(first.wear, other->wear) / (0.4 * cell));
    if (fit == 0.0) continue;
    const D3 boundary = first.n * other->d - other->n * first.d;
    const f64 square = dot(boundary, boundary);
    if (square < 1e-12) continue;
    const D3 projected = normalized(u - boundary * (dot(u, boundary) / square));
    const f64 distance = length(projected - u);
    if (distance > nearest) continue;
    const f64 along = dot(projected, first.n);
    if (along <= 1e-6) continue;
    const f64 plane_radius = first.d / along;
    const auto visible = fracture_planes(b, projected);
    if (std::fabs(visible.first.distance - plane_radius) > 1e-7) continue;
    chosen = normalized(u + (projected - u) * fit);
    nearest = distance;
  }
  return chosen;
}

// Compact per-welded-vertex triangle adjacency, built in deterministic index order.
struct Incidence {
  Vector<u32> offsets, triangles;
  Incidence(std::span<const u32> indices, std::span<const u32> weld, u32 first, u32 count)
      : offsets(count + 1, 0), triangles(static_cast<u32>(indices.size())) {
    for (u32 index : indices)
      ++offsets[weld[index - first] + 1];
    for (u32 i = 1; i <= count; ++i)
      offsets[i] += offsets[i - 1];
    Vector<u32> cursor = offsets;
    for (u32 i = 0; i < indices.size(); ++i)
      triangles[cursor[weld[indices[i] - first]]++] = i / 3;
  }
};

// Emits one body: its six charts' grids, outward-wound, with area-weighted normals shared across
// the charts' seams, box-projected UVs (rocks.md, "UVs and materials"), and the burial applied.
void emit_body(const Body& b, f64 tile_m, assets::MeshData& out, SurfaceBake* bake = nullptr,
               u32 part = 0, RockType type = RockType::Granite) {
  const u32 n = b.n;
  const u32 side = n + 1;
  const u32 first = out.positions.size();
  // The surface's positions by lattice point first, then the charts index into them.
  Vector<D3> local;
  Vector<D3> directions;
  Vector<D3> samples;
  Vector<f64> fresh;
  Vector<u32> lattice_of;  // per emitted vertex, its lattice point's slot in `local`
  // Only the cube's shell exists. A volume lookup made high detail pay O(n^3) temporary memory.
  Vector<u32> slot(6u * n * n + 2u, ~0u);
  const auto shell_key = [n, side](u32 x, u32 y, u32 z) {
    if (x == 0) return y * side + z;
    if (x == n) return side * side + y * side + z;
    const u32 middle = 2u * side * side;
    if (y == 0) return middle + (x - 1u) * side + z;
    if (y == n) return middle + (n - 1u) * side + (x - 1u) * side + z;
    return middle + 2u * (n - 1u) * side + (z == n ? (n - 1u) * (n - 1u) : 0u) +
           (x - 1u) * (n - 1u) + y - 1u;
  };
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
        const u32 key =
            shell_key(static_cast<u32>(c[0]), static_cast<u32>(c[1]), static_cast<u32>(c[2]));
        if (slot[key] == ~0u) {
          slot[key] = local.size();
          directions.push_back(lattice_direction(c[0], c[1], c[2], n, b.noise_seed + 0x53414Du));
          local.push_back(D3{});
        }
        lattice_of.push_back(slot[key]);
        out.positions.push_back(Vec3{});
        out.normals.push_back(Vec3{});
        out.uvs.push_back(
            Vec2{static_cast<f32>(chart_u + extent_b * static_cast<f64>(i) / static_cast<f64>(n)),
                 static_cast<f32>(chart_v + extent_c * static_cast<f64>(j) / static_cast<f64>(n))});
        if (bake != nullptr)
          out.uvs.back() =
              bake->uv(part * 6u + f, static_cast<f64>(i) / n, static_cast<f64>(j) / n);
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
  const u32 index_first = out.indices.size() - 36u * n * n;
  const auto body_indices = std::span<u32>(out.indices).subspan(index_first);
  {
    const Incidence incident(body_indices, lattice_of, first, local.size());
    Vector<f64> area(static_cast<u32>(body_indices.size() / 3));
    Vector<D3> feature_positions(local.size());
    for (u32 v = 0; v < directions.size(); ++v)
      feature_positions[v] = directions[v] * fracture_planes(b, directions[v]).first.distance;
    // Position accuracy alone does not reject tilted triangles spanning a sharp feature.
    // Judge the piecewise-planar envelope's face normals as well (before warp/weathering).
    const auto normal_error = [&](u32 tri, u32 moved, D3 candidate) {
      D3 p[3];
      for (u32 c = 0; c < 3; ++c) {
        const u32 v = lattice_of[body_indices[tri * 3 + c] - first];
        p[c] = v == moved ? candidate : feature_positions[v];
      }
      const D3 fn = cross(p[1] - p[0], p[2] - p[0]);
      const f64 weight = length(fn);
      const auto target = fracture_planes(b, normalized(p[0] + p[1] + p[2]));
      return weight - dot(fn, target.first.cut->n);
    };
    const auto direction_area = [&](u32 tri, u32 moved, D3 candidate) {
      D3 p[3];
      for (u32 c = 0; c < 3; ++c) {
        const u32 v = lattice_of[body_indices[tri * 3 + c] - first];
        p[c] = v == moved ? candidate : directions[v];
      }
      return dot(p[0], cross(p[1], p[2]));
    };
    for (u32 t = 0; t < area.size(); ++t)
      area[t] = direction_area(t, ~0u, D3{});
    // Sequential moves preserve a positive directional area in every incident triangle.
    // The floor is relative to the starting grid, so later moves cannot accumulate collapse.
    for (u32 v = 0; v < directions.size(); ++v) {
      const D3 candidate = rim_direction(b, directions[v]);
      if (dot(candidate - directions[v], candidate - directions[v]) < 1e-24) continue;
      const auto envelope = fracture_planes(b, directions[v]);
      const bool chip_boundary =
          envelope.first.inside != nullptr || envelope.second.inside != nullptr;
      const D3 position = candidate * fracture_planes(b, candidate).first.distance;
      bool safe = true;
      f64 before = 0.0, after = 0.0;
      for (u32 k = incident.offsets[v]; k < incident.offsets[v + 1]; ++k) {
        const u32 t = incident.triangles[k];
        if (direction_area(t, v, candidate) < area[t] * 0.2) {
          safe = false;
          break;
        }
        if (chip_boundary) {
          before += normal_error(t, ~0u, D3{});
          after += normal_error(t, v, position);
        }
      }
      if (safe && (!chip_boundary || after <= before)) {
        directions[v] = candidate;
        feature_positions[v] = position;
      }
    }
  }
  for (u32 v = 0; v < local.size(); ++v) {
    f64 provenance = 0.0;
    local[v] = directions[v] * radius(b, directions[v], &provenance);
    fresh.push_back(provenance);
  }
  // Pick the diagonal whose midpoint best approximates the generating surface. The bake
  // consumes this same table; choosing connectivity independently would smear its material.
  Vector<u8> diagonals(6u * n * n);
  for (u32 f = 0; f < 6; ++f)
    for (u32 j = 0; j < n; ++j)
      for (u32 i = 0; i < n; ++i) {
        const u32 q = f * side * side + j * side + i;
        const u32 c[4] = {q, q + 1, q + side + 1, q + side};
        D3 d[4], p[4];
        for (u32 k = 0; k < 4; ++k) {
          d[k] = directions[lattice_of[c[k]]];
          p[k] = local[lattice_of[c[k]]];
        }
        const auto error = [&](u32 a, u32 z) {
          const D3 midpoint = (p[a] + p[z]) * 0.5;
          return std::fabs(length(midpoint) - radius(b, normalized(midpoint)));
        };
        const f64 area_floor = 0.05 * (dot(d[0], cross(d[1], d[2])) + dot(d[0], cross(d[2], d[3])));
        const bool ac_safe =
            dot(d[0], cross(d[1], d[2])) > area_floor && dot(d[0], cross(d[2], d[3])) > area_floor;
        const bool bd_safe =
            dot(d[0], cross(d[1], d[3])) > area_floor && dot(d[1], cross(d[2], d[3])) > area_floor;
        const f64 ac = error(0, 2), bd = error(1, 3);
        // If neither alternative meets the stricter diagonal floor, retain the initial
        // connectivity whose positive area the relocation pass already guaranteed.
        const bool initial_main = ((i + j) & 1u) == 0;
        const bool main = !ac_safe && !bd_safe
                              ? initial_main
                              : ac_safe && (!bd_safe || ac < bd || (ac == bd && initial_main));
        const u32 cell_index = (f * n + j) * n + i;
        diagonals[cell_index] = main ? 1 : 0;
        const u32 order_ac[6] = {0, 1, 2, 0, 2, 3}, order_bd[6] = {0, 1, 3, 1, 2, 3};
        for (u32 k = 0; k < 6; ++k)
          body_indices[cell_index * 6 + k] = first + c[main ? order_ac[k] : order_bd[k]];
      }
  // The stretch: the cut hull has seeded dimensions, so each axis is scaled to
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
      // Use the intended resting frame for the formation-wide field, before the final burial
      // correction. Recession remains radial and positive, retaining the closed shell.
      const D3 sample = p + b.at + D3{0.0, b.half.y * (1.0 - 2.0 * b.buried), 0.0};
      if (bake != nullptr) samples.push_back(sample);
      const f64 recess = surface_recess(b, sample, cell) + fabric_recess(b.fabric, sample, cell);
      const f64 len = length(p);
      const f64 scale_detail = len > 0.0 ? 1.0 - recess / len : 1.0;
      p = p * (scale_detail < 0.65 ? 0.65 : (scale_detail > 1.05 ? 1.05 : scale_detail));
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
  if (bake != nullptr) {
    Vector<D3> chart_samples(lattice_of.size());
    Vector<f64> chart_fresh(lattice_of.size());
    for (u32 v = 0; v < lattice_of.size(); ++v) {
      chart_samples[v] = samples[lattice_of[v]];
      chart_fresh[v] = fresh[lattice_of[v]];
    }
    bake->body(part, n, first, b.fabric, type, chart_samples, chart_fresh, diagonals, out);
  }
  const Incidence incident(body_indices, lattice_of, first, local.size());
  Vector<D3> face_normals(static_cast<u32>(body_indices.size() / 3)),
      face_area(static_cast<u32>(body_indices.size() / 3));
  for (u32 t = 0; t < face_normals.size(); ++t) {
    const D3 a = placed[lattice_of[body_indices[t * 3] - first]];
    face_area[t] = cross(placed[lattice_of[body_indices[t * 3 + 1] - first]] - a,
                         placed[lattice_of[body_indices[t * 3 + 2] - first]] - a);
    face_normals[t] = normalized(face_area[t]);
  }
  // Smooth within a face, not across a fracture. Each corner sums compatible incident faces;
  // copies retain exact positions/UVs, so this adds vertex attributes but never triangles.
  for (u32 t = index_first; t < out.indices.size(); t += 3) {
    const D3 fn = face_normals[(t - index_first) / 3];
    for (u32 corner = 0; corner < 3; ++corner) {
      const u32 old = out.indices[t + corner];
      const u32 v = lattice_of[old - first];
      const f64 cosine = lerp(0.8, 0.5, b.fabric.exposure * (1.0 - fresh[v]));
      D3 sum{};
      bool split = false;
      for (u32 k = incident.offsets[v]; k < incident.offsets[v + 1]; ++k) {
        const u32 neighbour = incident.triangles[k];
        if (dot(fn, face_normals[neighbour]) >= cosine)
          sum = sum + face_area[neighbour];
        else
          split = true;
      }
      if (!split) continue;
      const D3 nn = normalized(sum);
      const Vec3 position = out.positions[old];
      const Vec2 uv = out.uvs[old];
      out.indices[t + corner] = out.positions.size();
      out.positions.push_back(position);
      out.uvs.push_back(uv);
      out.normals.push_back(
          Vec3{static_cast<f32>(nn.x), static_cast<f32>(nn.y), static_cast<f32>(nn.z)});
    }
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
  // Explicit legacy layer counts still work; physical fabric spacing is the default now.
  w.layers = layers.value_or(0u);
  return w;
}

void weather_body(Body& b, const Weather& w, u64 seed, u32 part) {
  b.smooth = w.rounding * (0.04 + 0.3 * b.fabric.exposure);
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
  Rng scars(mix64(seed ^ (static_cast<u64>(part) << 32) ^ 0x5343415253ull));
  for (Scar& scar : b.scars) {
    scar.direction = scars.direction();
    scar.width = scars.range(0.18, 0.65);
    scar.depth = scars.range(0.025, 0.13) * b.fabric.irregularity * b.fabric.model.scar;
  }
}

// Joint cuts (a pair of planes on opposite sides per set) at seeded distances, then the type's
// facets in seeded directions.
void cut_body(Body& b, const D3 joints[4], u32 sets, u32 facets, u64 seed, u32 part, f64 lo,
              f64 hi) {
  b.joint_count = sets;
  b.surface_seed = mix64(seed ^ 0x524F434B53555246ull);
  for (u32 s = 0; s < sets; ++s)
    b.joints[s] = joints[s];
  Rng rng = stream(seed, part, Stream::cuts);
  // Strictly enclosing, oblique faces even when the recipe asks for no joint sets. No unit-ball
  // patches remain between fracture planes. A dominant signed axis exceeds both off-axis
  // components together, so at least one plane faces every ray and the hull is bounded.
  Rng hull(mix64(seed ^ (static_cast<u64>(part) << 32) ^ 0x48554C4Cull));
  for (u32 axis = 0; axis < 3; ++axis) {
    for (f64 sign : {-1.0, 1.0}) {
      f64 v[3] = {hull.range(-0.2, 0.2), hull.range(-0.2, 0.2), hull.range(-0.2, 0.2)};
      v[axis] = sign;
      b.cuts.push_back(Cut{normalized(D3{v[0], v[1], v[2]}), hull.range(0.82, 1.08)});
    }
  }
  for (u32 s = 0; s < sets; ++s) {
    b.cuts.push_back(Cut{joints[s], rng.range(lo, hi)});
    b.cuts.push_back(Cut{joints[s] * -1.0, rng.range(lo, hi)});
  }
  for (u32 f = 0; f < facets; ++f)
    b.cuts.push_back(Cut{rng.direction(), rng.range(lo + 0.08, hi + 0.05), rng.range(0.02, 0.18)});
  if (b.column_ends) {
    const D3 up = normalized(D3{rng.range(-0.05, 0.05), 1.0, rng.range(-0.05, 0.05)});
    b.cuts.push_back(Cut{up, rng.range(lo, hi)});
    b.cuts.push_back(Cut{up * -1.0, rng.range(lo, hi)});
  }
  Rng chips(mix64(seed ^ (static_cast<u64>(part) << 32) ^ 0x4348495053ull));
  for (Chip& chip : b.chips) {
    const D3 direction = chips.direction();
    const D3 tangent = normalized(
        cross(direction, std::fabs(direction.y) < 0.8 ? D3{0.0, 1.0, 0.0} : D3{1.0, 0.0, 0.0}));
    const D3 bitangent = cross(direction, tangent);
    const f64 strength = b.fabric.irregularity * (0.35 + b.fabric.model.flaking) *
                         (1.0 - 0.75 * b.fabric.model.scar);
    const f64 depth = chips.range(0.035, 0.13) * strength;
    const f64 width = chips.range(0.12, 0.35);
    chip.width = width;
    const f64 top = hull_radius(b, direction);
    chip.planes[0] = Cut{direction, top - depth, 0.12};
    const f64 phase = chips.range(0.0, 2.0 * k_pi);
    for (u32 side = 0; side < 3; ++side) {
      const f64 angle = phase + static_cast<f64>(side) * (2.0 * k_pi / 3.0);
      const D3 across = tangent * det::cos(angle) + bitangent * det::sin(angle);
      const D3 normal = normalized(direction + across * (2.0 * depth / width));
      chip.planes[side + 1] = Cut{normal, dot(direction * (top - 1.8 * depth), normal), 0.12};
    }
    // Zero irregularity is a real off switch, including the smooth-min lip.
    if (strength == 0.0) chip.width = 0.0;
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
  // Cliff talus keeps its existing coarse profile; its face and block budgets are independent.
  b.surface.strength = 0.0f;
  RockGeology geology;
  geology.weathering = 0.2f;
  b.fabric = make_fabric(type, geology, seed);
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
    case RockType::Shale: return "Shale";
    case RockType::Slate: return "Slate";
    case RockType::Schist: return "Schist";
    case RockType::Gneiss: return "Gneiss";
    case RockType::Marble: return "Marble";
    case RockType::Obsidian: return "Obsidian";
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
  RockMaterial material;
  RockQuality quality = RockQuality::High;

  if (file.shape == RockShape::Boulder) {
    const Boulder p = file.boulder.value_or(Boulder{});
    type = p.type;
    material = p.material;
    quality = p.quality;
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
    b.surface = p.surface;
    b.fabric = make_fabric(p.type, p.geology, p.seed);
    weather_body(b, weather, p.seed, 0);
    cut_body(b, joints, p.joint_sets, weather.model.facets, p.seed, 0, 0.5, 0.75);
    Rng bed = stream(p.seed, 0, Stream::bedding);
    b.bedding = normalized(D3{bed.range(-0.15, 0.15), 1.0, bed.range(-0.15, 0.15)});
    bodies.push_back(std::move(b));
  } else if (file.shape == RockShape::Outcrop) {
    const Outcrop p = file.outcrop.value_or(Outcrop{});
    type = p.type;
    material = p.material;
    quality = p.quality;
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
      b.surface = p.surface;
      b.fabric = make_fabric(p.type, p.geology, p.seed);
      weather_body(b, weather, p.seed, i + 1);
      b.bedding = bedding;
      b.layer_phase = phase + dot(b.at, bedding);  // the beds run on through the boulders
      cut_body(b, joints, p.joint_sets, weather.model.facets, p.seed, i + 1, 0.48, 0.72);
      bodies.push_back(std::move(b));
    }
  } else {
    const Scree p = file.scree.value_or(Scree{});
    type = p.type;
    material = p.material;
    quality = p.quality;
    tile_m = wide(p.material.tile_m);
    budget = p.triangle_budget;
    budget_field = "engine.rocks.Scree.triangle_budget";
    // Fragments are freshly broken: sharper than the type's weathered boulders, with inherited
    // fabric.
    weather = weather_of(p.type, p.rounding, std::optional<u32>(0u));
    if (!p.rounding.has_value()) weather.rounding *= 0.4;
    Rng layout = stream(p.seed, 0, Stream::layout);
    Vector<D3> centres;
    Vector<f64> radii;
    for (u32 i = 0; i < p.count; ++i) {
      const f64 size = wide(p.fragment_m) * (i == 0 ? 1.0 : layout.range(0.35, 0.95));
      Body b;
      b.half = D3{size * layout.range(0.7, 1.0),
                  size * layout.range(0.5, 0.85) * lithology(p.type).plate,
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
      b.surface = p.surface;
      b.fabric = make_fabric(p.type, p.geology, p.seed + i);
      weather_body(b, weather, p.seed, i + 1);
      Rng joint_rng = stream(p.seed, i + 1, Stream::joints);
      D3 joints[4];
      joint_directions(p.type, 3, joint_rng, joints);
      cut_body(b, joints, 3, weather.model.facets + 2, p.seed, i + 1, 0.42, 0.75);
      bodies.push_back(std::move(b));
    }
  }

  budget = quality_budget(budget, quality, bodies.size() * triangles_of_grid(k_grid_rungs[0]));
  if (!spend(bodies, budget, report, error, budget_field)) {
    if (error_out != nullptr) *error_out = error;
    return false;
  }
  out.materials.push_back(material_of(type));
  material_factors(material, out.materials[0].base_color, out.materials[0].roughness);
  SurfaceBake bake;
  if (material.surface_maps && !bake.init(*material.surface_maps, bodies.size(), error)) {
    out = assets::MeshData{};
    if (error_out != nullptr) *error_out = error;
    return false;
  }
  for (u32 i = 0; i < bodies.size(); ++i) {
    const Body& b = bodies[i];
    emit_body(b, tile_m, out, material.surface_maps ? &bake : nullptr, i, type);
    report.cuts += b.cuts.size();
  }
  if (material.surface_maps && !bake.finish(out, material.normal_scale)) {
    out = assets::MeshData{};
    if (error_out != nullptr) *error_out = "rock surface map PNG encoding failed";
    return false;
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
