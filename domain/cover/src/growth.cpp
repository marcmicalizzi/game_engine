// The cover solver (growth.h): the mask, the stand-in film and the shaped cover.
#include "rng.h"

#include <core/math/deterministic.h>
#include <domain/cover/growth.h>
#include <domain/flora/companion.h>
#include <domain/geometry/cluster_file.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::cover {

namespace {

constexpr u32 k_sky_rays = 32;
constexpr u32 k_pieces_per_triangle = 64;  // a cap, so one huge triangle cannot take the budget

bool in(f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; }
f64 clamp01(f64 v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
f64 smoothstep(f64 lo, f64 hi, f64 v) {
  const f64 t = clamp01((v - lo) / (hi - lo));
  return t * t * (3.0 - 2.0 * t);
}

assets::Material material(const char* name, Vec4 colour, f32 roughness, bool two_sided) {
  assets::Material m;
  m.name = name;
  m.base_color = colour;
  m.roughness = roughness;
  m.metallic = 0.0f;
  m.double_sided = two_sided;
  return m;
}

// What one rung of the budget emits.
struct Rung {
  f64 threshold = 0.4;
  f64 density = 1.0;
};

// A lattice value in [0, 1) for (x, y, z) and a stream: splitmix of the coordinates.
f64 lattice(i64 x, i64 y, i64 z, u64 seed, u64 stream) {
  u64 h = seed ^ (stream * 0x9E3779B97F4A7C15ull);
  h ^= static_cast<u64>(x) * 0xBF58476D1CE4E5B9ull;
  h ^= static_cast<u64>(y) * 0x94D049BB133111EBull;
  h ^= static_cast<u64>(z) * 0xD1B54A32D192ED03ull;
  return Rng(h).unit();
}

// Value noise over the surface at `scale` metres, two octaves, smoothstep-interpolated, in [0, 1].
f64 patch_noise(P3 p, f64 scale, u64 seed, u64 stream) {
  f64 total = 0.0;
  f64 amplitude = 0.65;
  f64 frequency = 1.0 / scale;
  for (u32 octave = 0; octave < 2; ++octave) {
    const f64 x = p.x * frequency;
    const f64 y = p.y * frequency;
    const f64 z = p.z * frequency;
    const f64 fx = std::floor(x);
    const f64 fy = std::floor(y);
    const f64 fz = std::floor(z);
    const i64 ix = static_cast<i64>(fx);
    const i64 iy = static_cast<i64>(fy);
    const i64 iz = static_cast<i64>(fz);
    const f64 tx = smoothstep(0.0, 1.0, x - fx);
    const f64 ty = smoothstep(0.0, 1.0, y - fy);
    const f64 tz = smoothstep(0.0, 1.0, z - fz);
    const u64 s = stream * 2u + octave;
    f64 corner[2][2][2];
    for (u32 a = 0; a < 2; ++a)
      for (u32 b = 0; b < 2; ++b)
        for (u32 c = 0; c < 2; ++c)
          corner[a][b][c] = lattice(ix + a, iy + b, iz + c, seed, s);
    const auto mix = [](f64 u, f64 v, f64 t) { return u + (v - u) * t; };
    const f64 v = mix(mix(mix(corner[0][0][0], corner[1][0][0], tx),
                          mix(corner[0][1][0], corner[1][1][0], tx), ty),
                      mix(mix(corner[0][0][1], corner[1][0][1], tx),
                          mix(corner[0][1][1], corner[1][1][1], tx), ty),
                      tz);
    total += amplitude * v;
    amplitude = 0.35;
    frequency *= 2.7;
  }
  return clamp01(total);
}

// The host's holding triangles split until no edge is longer than `edge`: each into n x n by its
// longest edge, a point on a shared edge computed from that edge's two corners in the order of
// their weld ids, so two triangles that share the edge make the same point and the weld joins
// them. Refined past 1.5 million triangles, the edge is lengthened until it fits.
void refine_surface(const HostSurface& host, f64 edge, assets::GeneratorHostMesh& out) {
  out = assets::GeneratorHostMesh{};
  const u32 triangles = host.triangle_count();
  const auto splits = [&](u32 t, f64 e) {
    f64 longest = 0.0;
    for (u32 k = 0; k < 3; ++k) {
      const f64 l = length_of(host.corner(t, (k + 1u) % 3u) - host.corner(t, k));
      longest = l > longest ? l : longest;
    }
    const f64 n = std::ceil(longest / e - 1e-9);
    return n < 1.0 ? 1u : (n > 64.0 ? 64u : static_cast<u32>(n));
  };
  for (u32 attempt = 0; attempt < 16; ++attempt) {
    u64 total = 0;
    for (u32 t = 0; t < triangles; ++t)
      if (host.holds[t] != 0) total += static_cast<u64>(splits(t, edge)) * splits(t, edge);
    if (total <= 1500000u) break;
    edge *= 1.5;
  }
  Vector<u32> local;
  for (u32 t = 0; t < triangles; ++t) {
    if (host.holds[t] == 0) continue;
    const u32 n = splits(t, edge);
    const u32 corner[3] = {host.indices[t * 3u], host.indices[t * 3u + 1u],
                           host.indices[t * 3u + 2u]};
    const P3 p[3] = {host.positions[corner[0]], host.positions[corner[1]],
                     host.positions[corner[2]]};
    const u32 w[3] = {host.weld[corner[0]], host.weld[corner[1]], host.weld[corner[2]]};
    // The point of integer barycentrics (b[0], b[1], b[2]) summing to n.
    const auto point = [&](const u32 b[3]) {
      const f64 fn = static_cast<f64>(n);
      for (u32 k = 0; k < 3; ++k) {
        if (b[k] != 0) continue;
        // On the edge opposite corner k: between corners x and y, ordered by weld id.
        u32 x = (k + 1u) % 3u;
        u32 y = (k + 2u) % 3u;
        if (w[y] < w[x] || (w[y] == w[x] && y < x)) std::swap(x, y);
        return p[x] + (p[y] - p[x]) * (static_cast<f64>(b[y]) / fn);
      }
      return p[0] + (p[1] - p[0]) * (static_cast<f64>(b[1]) / fn) +
             (p[2] - p[0]) * (static_cast<f64>(b[2]) / fn);
    };
    local.clear();
    const u32 base = out.positions.size();
    for (u32 j = 0; j <= n; ++j) {
      for (u32 i = 0; i + j <= n; ++i) {
        const u32 b[3] = {n - i - j, i, j};
        const P3 q = point(b);
        out.positions.push_back(
            Vec3{static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z)});
      }
    }
    // Row j starts at sum_{r<j} (n + 1 - r).
    const auto at = [&](u32 i, u32 j) { return base + j * (n + 1u) - (j * (j - 1u)) / 2u + i; };
    for (u32 j = 0; j < n; ++j) {
      for (u32 i = 0; i + j < n; ++i) {
        const u32 tri[3] = {at(i, j), at(i + 1u, j), at(i, j + 1u)};
        for (const u32 v : tri)
          out.indices.push_back(v);
        out.roughness.push_back(host.roughness[t]);
        out.holds.push_back(1u);
        if (i + j + 1u < n) {
          const u32 second[3] = {at(i + 1u, j), at(i + 1u, j + 1u), at(i, j + 1u)};
          for (const u32 v : second)
            out.indices.push_back(v);
          out.roughness.push_back(host.roughness[t]);
          out.holds.push_back(1u);
        }
      }
    }
  }
}

// The fine surface's mask at the host's welded vertices: a host vertex of a holding triangle is a
// corner of the refinement, at the same position; every other host vertex carries no cover.
void host_mask(const HostSurface& host, const HostSurface& fine, const Vector<f32>& mask,
               Vector<f32>& out) {
  out.clear();
  out.resize(host.welded * k_cover_kinds, 0.0f);
  struct Entry {
    P3 p;
    u32 weld;
  };
  Vector<Entry> entries;
  entries.reserve(fine.positions.size());
  for (u32 v = 0; v < fine.positions.size(); ++v)
    entries.push_back(Entry{fine.positions[v], fine.weld[v]});
  const auto less = [](const Entry& a, const Entry& b) {
    if (a.p.x != b.p.x) return a.p.x < b.p.x;
    if (a.p.y != b.p.y) return a.p.y < b.p.y;
    if (a.p.z != b.p.z) return a.p.z < b.p.z;
    return a.weld < b.weld;
  };
  std::sort(entries.begin(), entries.end(), less);
  for (u32 v = 0; v < host.positions.size(); ++v) {
    const Entry key{host.positions[v], 0u};
    const Entry* found = std::lower_bound(entries.begin(), entries.end(), key, less);
    if (found == entries.end() ||
        !(found->p.x == key.p.x && found->p.y == key.p.y && found->p.z == key.p.z))
      continue;
    const u32 w = host.weld[v];
    for (u32 k = 0; k < k_cover_kinds; ++k)
      out[w * k_cover_kinds + k] = mask[found->weld * k_cover_kinds + k];
  }
}

}  // namespace

bool cover_in_range(const CoverGrowth& c, std::string& error) {
  const auto bad = [&](const char* field) {
    error = std::string("cover.") + field + " is out of range (cover.md, \"The cover\")";
    return false;
  };
  if (!in(c.age_years, 0.0, 500.0)) return bad("age_years");
  if (!in(c.moss, 0.0, 2.0)) return bad("moss");
  if (!in(c.lichen, 0.0, 2.0)) return bad("lichen");
  if (!in(c.algae, 0.0, 2.0)) return bad("algae");
  if (!in(c.fungus, 0.0, 2.0)) return bad("fungus");
  if (!in(c.crevice_radius_m, 0.05, 10.0)) return bad("crevice_radius_m");
  if (!in(c.sample_m, 0.02, 5.0)) return bad("sample_m");
  if (!in(c.patchiness, 0.0, 1.0)) return bad("patchiness");
  if (!in(c.patch_m, 0.02, 20.0)) return bad("patch_m");
  if (c.smoothing > 32) return bad("smoothing");
  if (!in(c.film_threshold, 0.05, 1.0)) return bad("film_threshold");
  if (!in(c.film_offset_m, 0.0, 0.05)) return bad("film_offset_m");
  if (!in(c.cushions_per_m2, 0.0, 400.0)) return bad("cushions_per_m2");
  if (!in(c.tufts_per_m2, 0.0, 400.0)) return bad("tufts_per_m2");
  if (!in(c.brackets_per_m2, 0.0, 400.0)) return bad("brackets_per_m2");
  if (!in(c.mushrooms_per_m2, 0.0, 400.0)) return bad("mushrooms_per_m2");
  if (!in(c.cushion_radius_m, 0.005, 0.3)) return bad("cushion_radius_m");
  if (!in(c.bracket_reach_m, 0.01, 0.5)) return bad("bracket_reach_m");
  if (!in(c.cap_radius_m, 0.005, 0.2)) return bad("cap_radius_m");
  if (!in(c.tuft_height_m, 0.005, 0.2)) return bad("tuft_height_m");
  if (!in(c.triangle_budget, 500, 2000000)) return bad("triangle_budget");
  return true;
}

Suitability cover_suitability(const Exposure& e, f64 roughness, const HostSite& site,
                              const CoverGrowth& params) {
  Suitability s;
  const f64 sun = static_cast<f64>(e.sun);
  const f64 sky = static_cast<f64>(e.sky);
  const f64 up = static_cast<f64>(e.up);
  const f64 north = static_cast<f64>(e.north) > 0.0 ? static_cast<f64>(e.north) : 0.0;
  const f64 r = clamp01(roughness);
  // Damp: the climate, the base (splash and the ground's moisture within half a metre), shelter
  // (a crevice holds water and loses less to the wind), the north face (in the northern
  // hemisphere it dries last), the water's spray and humidity within a few metres, less the sun.
  const f64 damp =
      clamp01(0.45 * static_cast<f64>(site.wetness) +
              0.25 * det::exp(-static_cast<f64>(e.height) / 0.5) + 0.2 * (1.0 - sky) +
              0.12 * north + 0.2 * det::exp(-static_cast<f64>(e.water) / 5.0) - 0.25 * sun);
  s.damp = damp;
  // Moss: damp, shade, a face that holds water and debris (a top best, a wall well, an overhang
  // poorly), more on a rough substrate (bark texture is the strongest factor epiphyte surveys
  // find).
  const f64 orient = up >= 0.0 ? 0.7 + 0.3 * up : (0.7 + 0.8 * up > 0.1 ? 0.7 + 0.8 * up : 0.1);
  s.kind[k_moss] = clamp01(1.4 * damp) * (1.0 - 0.75 * sun) * orient * (0.35 + 0.65 * r);
  // Crustose lichen: light and open sky, tolerant of drought and beaten by moss where it is damp;
  // rock and bark alike, less under an overhang.
  const f64 light = sun > 0.6 * sky ? sun : 0.6 * sky;
  s.kind[k_lichen] =
      (0.3 + 0.7 * light) * (1.0 - 0.6 * damp) * (0.5 + 0.5 * r) * (up > -0.5 ? 1.0 : 0.4);
  // Algae: a green film where it is damp and shaded, on any substrate, smooth included.
  s.kind[k_algae] = clamp01(1.2 * damp) * (1.0 - 0.9 * sun);
  // Fungi fruit only on dead or dying wood, where it is damp and shaded.
  s.kind[k_fungus] = params.dead_wood ? clamp01(1.3 * damp) * (1.0 - 0.8 * sun) : 0.0;
  return s;
}

void solve_cover_mask(const HostSurface& surface, const Vector<Exposure>& exposure,
                      const HostSite& site, const CoverGrowth& params, Vector<f32>& out) {
  const u32 triangles = surface.triangle_count();
  const f64 age = static_cast<f64>(params.age_years);
  const f64 weight[k_cover_kinds] = {static_cast<f64>(params.moss), static_cast<f64>(params.lichen),
                                     static_cast<f64>(params.algae),
                                     static_cast<f64>(params.fungus)};
  f64 grown[k_cover_kinds];
  for (u32 k = 0; k < k_cover_kinds; ++k)
    grown[k] = 1.0 - det::exp(-age / k_tau_years[k]);
  const f64 patchiness = static_cast<f64>(params.patchiness);
  const f64 patch_m = static_cast<f64>(params.patch_m);
  // Per triangle, then to the welded vertices by area.
  Vector<f64> sum;
  sum.resize(surface.welded * k_cover_kinds, 0.0);
  Vector<f64> area;
  area.resize(surface.welded, 0.0);
  for (u32 t = 0; t < triangles; ++t) {
    if (surface.holds[t] == 0 || surface.face_area[t] <= 0.0) continue;
    const Suitability s =
        cover_suitability(exposure[t], static_cast<f64>(surface.roughness[t]), site, params);
    // Colonies: each kind establishes in patches, a seeded value noise over the surface at
    // `patch_m` scaling its share by 1 - patchiness + patchiness * noise.
    const P3 at = surface.centroid(t);
    f64 patch[k_cover_kinds];
    for (u32 k = 0; k < k_cover_kinds; ++k)
      patch[k] = 1.0 - patchiness +
                 patchiness * smoothstep(0.3, 0.7, patch_noise(at, patch_m, params.seed, k));
    f64 c[k_cover_kinds];
    c[k_moss] = clamp01(weight[k_moss] * s.kind[k_moss] * grown[k_moss] * patch[k_moss]);
    c[k_lichen] = clamp01(weight[k_lichen] * s.kind[k_lichen] * grown[k_lichen] * patch[k_lichen]) *
                  (1.0 - c[k_moss]);
    c[k_algae] = clamp01(weight[k_algae] * s.kind[k_algae] * grown[k_algae] * patch[k_algae]) *
                 (1.0 - c[k_moss]) * (1.0 - c[k_lichen]);
    c[k_fungus] = clamp01(weight[k_fungus] * s.kind[k_fungus] * grown[k_fungus] * patch[k_fungus]);
    const f64 a = surface.face_area[t];
    for (u32 j = 0; j < 3; ++j) {
      const u32 w = surface.weld[surface.indices[t * 3u + j]];
      area[w] += a;
      for (u32 k = 0; k < k_cover_kinds; ++k)
        sum[w * k_cover_kinds + k] += c[k] * a;
    }
  }
  Vector<f64> value;
  value.resize(surface.welded * k_cover_kinds, 0.0);
  for (u32 w = 0; w < surface.welded; ++w) {
    if (area[w] <= 0.0) continue;
    for (u32 k = 0; k < k_cover_kinds; ++k)
      value[w * k_cover_kinds + k] = sum[w * k_cover_kinds + k] / area[w];
  }
  // Smoothing along the surface (Jacobi: every vertex from the last pass's neighbours), over the
  // vertices that carry cover, so a leaf's or an empty part's zero does not bleed in.
  Vector<f64> next;
  next.resize(value.size(), 0.0);
  for (u32 pass = 0; pass < params.smoothing; ++pass) {
    for (u32 w = 0; w < surface.welded; ++w) {
      for (u32 k = 0; k < k_cover_kinds; ++k)
        next[w * k_cover_kinds + k] = value[w * k_cover_kinds + k];
      if (area[w] <= 0.0) continue;
      f64 total[k_cover_kinds] = {0.0, 0.0, 0.0, 0.0};
      u32 count = 0;
      for (u32 i = surface.adjacency_offset[w]; i < surface.adjacency_offset[w + 1u]; ++i) {
        const u32 v = surface.adjacency[i];
        if (area[v] <= 0.0) continue;
        ++count;
        for (u32 k = 0; k < k_cover_kinds; ++k)
          total[k] += value[v * k_cover_kinds + k];
      }
      if (count == 0) continue;
      for (u32 k = 0; k < k_cover_kinds; ++k)
        next[w * k_cover_kinds + k] =
            0.5 * value[w * k_cover_kinds + k] + 0.5 * total[k] / static_cast<f64>(count);
    }
    value.swap(next);
  }
  // Sharpened, so a patch has an edge rather than a fog: smoothstep(0.1, 0.8).
  out.clear();
  out.resize(value.size(), 0.0f);
  for (u32 i = 0; i < value.size(); ++i)
    out[i] = static_cast<f32>(smoothstep(0.1, 0.8, value[i]));
}

void pack_cover_mask(const HostSurface& surface, const Vector<f32>& welded, Vector<u32>& out) {
  out.clear();
  out.resize(surface.positions.size(), 0u);
  for (u32 v = 0; v < surface.positions.size(); ++v) {
    const u32 w = surface.weld[v];
    u32 word = 0;
    for (u32 k = 0; k < k_cover_kinds; ++k) {
      const f64 c = clamp01(static_cast<f64>(welded[w * k_cover_kinds + k]));
      word |= static_cast<u32>(std::floor(c * 255.0 + 0.5)) << (8u * k);
    }
    out[v] = word;
  }
}

bool solve_cover(const HostSurface& host, const HostBvh& bvh, const HostSite& site,
                 const CoverGrowth& params, assets::MeshData& out, flora::Motion& motion,
                 Vector<u32>* mask_out, CoverReport* report_out, std::string* error_out) {
  out = assets::MeshData{};
  motion = flora::Motion{};
  std::string error;
  if (!cover_in_range(params, error)) {
    if (error_out != nullptr) *error_out = error;
    return false;
  }
  // The mask is solved over the host's holding surface split to `sample_m` (a wall of a dozen
  // triangles still gets a mask with a shape), its terms' rays against the host itself.
  assets::GeneratorHostMesh refined;
  refine_surface(host, static_cast<f64>(params.sample_m), refined);
  HostSurface fine;
  if (!make_host_surface(refined, fine, error)) {
    if (error_out != nullptr) *error_out = "nothing on the host holds cover: " + error;
    return false;
  }
  const HostSurface& surface = fine;
  Vector<Exposure> exposure;
  host_exposure(surface, bvh, site, static_cast<f64>(params.crevice_radius_m), k_sky_rays,
                exposure);
  Vector<f32> mask;
  solve_cover_mask(surface, exposure, site, params, mask);
  if (mask_out != nullptr) {
    Vector<f32> at_host;
    host_mask(host, surface, mask, at_host);
    pack_cover_mask(host, at_host, *mask_out);
  }
  CoverReport report;
  const u32 triangles = surface.triangle_count();
  for (u32 t = 0; t < triangles; ++t) {
    if (surface.holds[t] == 0) continue;
    const f64 a = surface.face_area[t];
    report.holding_area_m2 += a;
    for (u32 k = 0; k < k_cover_kinds; ++k) {
      f64 c = 0.0;
      for (u32 j = 0; j < 3; ++j)
        c += static_cast<f64>(mask[surface.weld[surface.indices[t * 3u + j]] * k_cover_kinds + k]);
      report.mean[k] += a * c / 3.0;
    }
  }
  for (u32 k = 0; k < k_cover_kinds; ++k)
    report.mean[k] = report.holding_area_m2 > 0.0 ? report.mean[k] / report.holding_area_m2 : 0.0;
  const auto corner_mean = [&](u32 t, u32 k) {
    f64 c = 0.0;
    for (u32 j = 0; j < 3; ++j)
      c += static_cast<f64>(mask[surface.weld[surface.indices[t * 3u + j]] * k_cover_kinds + k]);
    return c / 3.0;
  };

  // One emission at a rung; the ladder raises the film's threshold, then thins the pieces.
  Vector<u32> film_vertex;
  const auto emit = [&](const Rung& rung, bool count_only, u64& triangles_out) {
    out = assets::MeshData{};
    motion = flora::Motion{};
    CoverReport counts;
    out.materials.push_back(material("moss mat", Vec4{0.19f, 0.30f, 0.07f, 1.0f}, 0.95f, false));
    out.materials.push_back(material("lichen crust", Vec4{0.56f, 0.60f, 0.48f, 1.0f}, 0.9f, true));
    out.materials.push_back(material("algae film", Vec4{0.15f, 0.29f, 0.11f, 1.0f}, 0.8f, false));
    out.materials.push_back(material("fungus cap", Vec4{0.52f, 0.38f, 0.24f, 1.0f}, 0.7f, false));
    flora::KitWriter writer{out, motion};
    const P3 root = surface.lo;
    flora::kit_begin_part(writer, flora::KitPoint{root.x, root.y, root.z},
                          flora::KitPoint{0.0, 1.0, 0.0}, 0.0, geometry::k_motion_no_parent, 0.0,
                          0.0, 0u);
    flora::kit_begin_part(writer, flora::KitPoint{root.x, root.y, root.z},
                          flora::KitPoint{0.0, 1.0, 0.0}, 0.0, 0u, 0.0, 0.0, 0u);
    writer.weight = 0.0;
    const f64 offset = static_cast<f64>(params.film_offset_m);
    for (u32 kind = 0; kind < k_cover_kinds; ++kind) {
      const u32 first_index = out.indices.size();
      // (c) The film of this kind: the triangles whose strongest smooth kind it is.
      if (kind != k_fungus) {
        film_vertex.clear();
        film_vertex.resize(surface.welded, ~0u);
        for (u32 t = 0; t < triangles; ++t) {
          if (surface.holds[t] == 0) continue;
          u32 strongest = k_moss;
          f64 best = corner_mean(t, k_moss);
          for (u32 k = k_lichen; k <= k_algae; ++k) {
            const f64 c = corner_mean(t, k);
            if (c > best) {
              best = c;
              strongest = k;
            }
          }
          if (strongest != kind || best < rung.threshold) continue;
          ++counts.film_triangles;
          if (count_only) continue;
          u32 corner[3];
          for (u32 j = 0; j < 3; ++j) {
            const u32 v = surface.indices[t * 3u + j];
            const u32 w = surface.weld[v];
            if (film_vertex[w] == ~0u) {
              const P3 n = surface.welded_normal[w];
              const P3 p = surface.positions[v] + n * offset;
              const u32 index = out.positions.size();
              out.positions.push_back(
                  Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)});
              out.normals.push_back(
                  Vec3{static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z)});
              // UVs in metres over the host's plan, so a tiled set lies on it at its repeat.
              out.uvs.push_back(Vec2{static_cast<f32>(p.x + p.y), static_cast<f32>(p.z + p.y)});
              motion.vertex_part.push_back(writer.part);
              motion.vertex_weight.push_back(0.0f);
              film_vertex[w] = index;
            }
            corner[j] = film_vertex[w];
          }
          out.indices.push_back(corner[0]);
          out.indices.push_back(corner[1]);
          out.indices.push_back(corner[2]);
        }
      }
      // (b) The shaped cover of this kind.
      for (u32 t = 0; t < triangles; ++t) {
        if (surface.holds[t] == 0) continue;
        const f64 c = corner_mean(t, kind);
        if (c <= 0.0) continue;
        const P3 n = surface.face_normal[t];
        f64 density = 0.0;
        if (kind == k_moss) density = static_cast<f64>(params.cushions_per_m2);
        if (kind == k_lichen) density = static_cast<f64>(params.tufts_per_m2);
        if (kind == k_algae) continue;  // algae is a film and nothing else
        bool bracket = false;
        if (kind == k_fungus) {
          // A bracket on a steep face, a mushroom on a face that looks up.
          bracket = n.y < 0.6;
          density = static_cast<f64>(bracket ? params.brackets_per_m2 : params.mushrooms_per_m2);
          if (!bracket && n.y < 0.6) continue;
        }
        Rng rng = stream_of(params.seed, (static_cast<u64>(t) << 2) | kind);
        const f64 expected = surface.face_area[t] * density * c * rung.density;
        u32 pieces = static_cast<u32>(std::floor(expected + rng.unit()));
        pieces = pieces > k_pieces_per_triangle ? k_pieces_per_triangle : pieces;
        for (u32 i = 0; i < pieces; ++i) {
          f64 u = rng.unit();
          f64 v = rng.unit();
          if (u + v > 1.0) {
            u = 1.0 - u;
            v = 1.0 - v;
          }
          const P3 a = surface.corner(t, 0);
          const P3 p = a + (surface.corner(t, 1) - a) * u + (surface.corner(t, 2) - a) * v;
          const P3 along = normalized(cross(n, rng.direction()));
          const u32 variant = static_cast<u32>(rng.next() % 3u);
          const f64 size = 0.7 + 0.3 * static_cast<f64>(variant) + 0.2 * rng.unit();
          if (kind == k_moss) {
            ++counts.cushions;
            if (count_only) continue;
            const f64 r = static_cast<f64>(params.cushion_radius_m) * size;
            const P3 c0 = p - n * (0.15 * r);
            flora::kit_dome(writer, flora::KitPoint{c0.x, c0.y, c0.z},
                            flora::KitPoint{n.x, n.y, n.z},
                            flora::KitPoint{along.x, along.y, along.z}, r, 0.55 * r,
                            0.75 + 0.1 * static_cast<f64>(variant), 6u + variant, 3u);
          } else if (kind == k_lichen) {
            ++counts.tufts;
            if (count_only) continue;
            // Fruticose lichen: three small strap cards standing off the surface.
            const f64 h = static_cast<f64>(params.tuft_height_m) * size;
            for (u32 j = 0; j < 3; ++j) {
              const f64 turn = 2.0943951023931953 * static_cast<f64>(j);
              const P3 side = normalized(along * det::cos(turn) + cross(n, along) * det::sin(turn));
              const P3 dir = normalized(n + side * 0.45);
              flora::kit_leaf_card(writer, flora::KitPoint{p.x, p.y, p.z},
                                   flora::KitPoint{dir.x, dir.y, dir.z},
                                   flora::KitPoint{side.x, side.y, side.z}, h, 0.35 * h, 0.1);
            }
          } else if (bracket) {
            ++counts.brackets;
            if (count_only) continue;
            // A shelf out of a steep face: a flat half-dome, its top level, reaching out along n.
            const f64 reach = static_cast<f64>(params.bracket_reach_m) * size;
            const P3 level = normalized(n - P3{0.0, 1.0, 0.0} * n.y);
            const P3 out_dir = length_of(level) > 0.0 ? level : along;
            flora::kit_dome(writer, flora::KitPoint{p.x, p.y, p.z}, flora::KitPoint{0.0, 1.0, 0.0},
                            flora::KitPoint{out_dir.x, out_dir.y, out_dir.z}, reach, 0.22 * reach,
                            0.65, 8u, 2u);
          } else {
            ++counts.mushrooms;
            if (count_only) continue;
            // A stalk and a cap.
            const f64 r = static_cast<f64>(params.cap_radius_m) * size;
            const f64 h = 2.2 * r;
            const flora::KitPoint stalk[2] = {{p.x, p.y, p.z},
                                              {p.x + n.x * h, p.y + n.y * h, p.z + n.z * h}};
            const f64 radii[2] = {0.22 * r, 0.18 * r};
            const f64 weights[2] = {0.0, 0.0};
            flora::kit_polyline_tube(writer, std::span<const flora::KitPoint>(stalk, 2),
                                     std::span<const f64>(radii, 2),
                                     std::span<const f64>(weights, 2), 5u);
            flora::kit_dome(writer, stalk[1], flora::KitPoint{n.x, n.y, n.z},
                            flora::KitPoint{along.x, along.y, along.z}, r, 0.6 * r, 1.0, 8u, 3u);
          }
        }
      }
      if (!count_only && out.indices.size() > first_index) {
        out.primitives.push_back(assets::Primitive{first_index, out.indices.size() - first_index,
                                                   static_cast<i32>(kind), -1});
      }
    }
    if (count_only) {
      // The pieces' triangles, by the kit's shapes (counted, not built).
      triangles_out = counts.film_triangles + static_cast<u64>(counts.cushions) * 42u +
                      static_cast<u64>(counts.tufts) * 6u +
                      static_cast<u64>(counts.brackets) * 24u +
                      static_cast<u64>(counts.mushrooms) * 55u;
    } else {
      triangles_out = out.indices.size() / 3u;
    }
    report.film_triangles = counts.film_triangles;
    report.cushions = counts.cushions;
    report.tufts = counts.tufts;
    report.brackets = counts.brackets;
    report.mushrooms = counts.mushrooms;
  };
  Rung rung;
  rung.threshold = static_cast<f64>(params.film_threshold);
  u64 count = 0;
  for (u32 step = 0;; ++step) {
    emit(rung, true, count);
    if (count <= params.triangle_budget) break;
    if (step >= 24) {
      if (error_out != nullptr) *error_out = "the cover does not fit triangle_budget";
      return false;
    }
    if (rung.threshold < 1.0) {
      rung.threshold = rung.threshold + 0.1 < 1.0 ? rung.threshold + 0.1 : 1.0;
    } else {
      rung.density *= 0.5;
    }
  }
  emit(rung, false, count);
  report.film_threshold = static_cast<f32>(rung.threshold);
  report.density_scale = rung.density;
  report.triangles = out.indices.size() / 3u;
  if (out.indices.empty()) {
    if (error_out != nullptr) *error_out = "nothing grows on this host at this age and site";
    return false;
  }
  if (report_out != nullptr) *report_out = report;
  return true;
}

}  // namespace engine::cover
