// The vine solver (vine.h): the growth walk over the host and its mesh.
#include "rng.h"

#include <core/math/deterministic.h>
#include <domain/cover/vine.h>
#include <domain/flora/companion.h>
#include <domain/geometry/cluster_file.h>

#include <cmath>

namespace engine::cover {

namespace {

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_surface_lift = 1e-3;  // a walked point stands this far off the surface
constexpr u32 k_step_cap = 200000;    // steps over every stem: a recipe's lengths stay far below

template <typename T>
bool in(T value, f64 lo, f64 hi) {
  const f64 v = static_cast<f64>(value);
  return v >= lo && v <= hi;
}

P3 rotate(P3 v, P3 axis, f64 angle) {
  const f64 c = det::cos(angle);
  const f64 s = det::sin(angle);
  return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

P3 tangent_of(P3 d, P3 n) { return d - n * dot(d, n); }

// A direction in the plane of `n` close to up, or a seeded one when the plane is level.
P3 initial_direction(P3 n, Rng& rng) {
  const P3 up{0.0, 1.0, 0.0};
  const P3 t = tangent_of(up, n);
  if (length_of(t) > 0.2) return normalized(t);
  const P3 random = rng.direction();
  const P3 r = tangent_of(random, n);
  return length_of(r) > 1e-6 ? normalized(r) : normalized(cross(n, P3{1.0, 0.0, 0.0}));
}

struct Walker {
  P3 p;
  P3 d;
  P3 n;
  f64 budget = 0.0;  // metres it may still grow
  u32 parent = ~0u;
  u32 at = 0;
  u32 depth = 0;
};

}  // namespace

bool vine_in_range(const Vine& v, std::string& error) {
  const auto bad = [&](const char* field) {
    error = std::string("vine.") + field + " is out of range (cover.md, \"The vine\")";
    return false;
  };
  if (!in(v.seeds, 1, 64)) return bad("seeds");
  if (!in(v.seed_band_m, 0.02, 10.0)) return bad("seed_band_m");
  if (!in(v.step_m, 0.01, 0.5)) return bad("step_m");
  if (!in(v.max_length_m, 0.2, 60.0)) return bad("max_length_m");
  if (!in(v.primary_weight, 0.0, 4.0)) return bad("primary_weight");
  if (!in(v.adhesion_weight, 0.0, 4.0)) return bad("adhesion_weight");
  if (!in(v.light_weight, 0.0, 4.0)) return bad("light_weight");
  if (!in(v.gravity_weight, 0.0, 4.0)) return bad("gravity_weight");
  if (!in(v.random_weight, 0.0, 4.0)) return bad("random_weight");
  if (!in(v.adhesion_distance_m, 0.005, 1.0)) return bad("adhesion_distance_m");
  if (!in(v.max_free_m, 0.05, 5.0)) return bad("max_free_m");
  if (!in(v.hold_roughness, 0.0, 1.0)) return bad("hold_roughness");
  if (!in(v.branch_chance, 0.0, 0.5)) return bad("branch_chance");
  if (!in(v.branch_angle_deg, 5.0, 85.0)) return bad("branch_angle_deg");
  if (v.branch_depth > 6) return bad("branch_depth");
  if (!in(v.stem_radius_m, 0.002, 0.2)) return bad("stem_radius_m");
  if (!in(v.tip_radius_m, 0.0005, static_cast<f64>(v.stem_radius_m))) return bad("tip_radius_m");
  if (!in(v.leaf_length_m, 0.01, 0.5)) return bad("leaf_length_m");
  if (!in(v.leaf_width, 0.3, 1.5)) return bad("leaf_width");
  if (!in(v.leaf_spacing_m, 0.01, 1.0)) return bad("leaf_spacing_m");
  if (!in(v.petiole_m, 0.0, 0.3)) return bad("petiole_m");
  if (!in(v.sides, 3, 12)) return bad("sides");
  if (!in(v.row_m, 0.01, 1.0)) return bad("row_m");
  if (!in(v.triangle_budget, 500, 2000000)) return bad("triangle_budget");
  for (const Vec3 a : v.anchors) {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z)) return bad("anchors");
  }
  return true;
}

bool grow_vine(const HostSurface& surface, const HostBvh& bvh, const HostSite& site,
               const Vine& params, Vector<VineStem>& stems, VineReport& report,
               std::string& error) {
  stems.clear();
  report = VineReport{};
  const u32 triangles = surface.triangle_count();
  const f64 ground = static_cast<f64>(site.ground_y_m);
  // The base: the lowest holding point, or the ground where the ground stands above it.
  f64 base = 1e300;
  for (u32 t = 0; t < triangles; ++t) {
    if (surface.holds[t] == 0) continue;
    for (u32 k = 0; k < 3; ++k)
      base = surface.corner(t, k).y < base ? surface.corner(t, k).y : base;
  }
  if (!(base < 1e300)) {
    error = "the host has no surface a climber may hold";
    return false;
  }
  const f64 band_top = (base > ground ? base : ground) + static_cast<f64>(params.seed_band_m);
  Vector<u32> candidates;
  Vector<f64> cumulative;
  f64 total = 0.0;
  for (u32 t = 0; t < triangles; ++t) {
    if (surface.holds[t] == 0 || surface.face_area[t] <= 0.0) continue;
    // Reaching into the band above the ground, and not an underside: a seed sprouts on a face it
    // can climb from (the point itself is drawn inside the band below).
    f64 lowest = 1e300;
    f64 highest = -1e300;
    for (u32 k = 0; k < 3; ++k) {
      const f64 y = surface.corner(t, k).y;
      lowest = y < lowest ? y : lowest;
      highest = y > highest ? y : highest;
    }
    if (lowest > band_top || highest < ground - 0.01 || surface.face_normal[t].y < -0.7) continue;
    total += surface.face_area[t];
    candidates.push_back(t);
    cumulative.push_back(total);
  }
  if (candidates.empty()) {
    error = "the host has no holding surface within seed_band_m of its base";
    return false;
  }
  Rng rng = stream_of(params.seed, 1);
  Vector<Walker> queue;
  const f64 search = length_of(surface.hi - surface.lo) + 1.0;
  for (u32 s = 0; s < params.seeds; ++s) {
    const f64 pick = rng.unit() * total;
    u32 lo = 0;
    u32 hi = cumulative.size() - 1u;
    while (lo < hi) {
      const u32 mid = (lo + hi) / 2u;
      if (cumulative[mid] < pick) {
        lo = mid + 1u;
      } else {
        hi = mid;
      }
    }
    const u32 t = candidates[lo];
    // A point of the triangle inside the band, by rejection (a large triangle reaches past it);
    // after 64 draws, the triangle's lowest corner.
    const P3 a = surface.corner(t, 0);
    P3 point = a;
    for (u32 k = 1; k < 3; ++k)
      point = surface.corner(t, k).y < point.y ? surface.corner(t, k) : point;
    for (u32 attempt = 0; attempt < 64; ++attempt) {
      f64 u = rng.unit();
      f64 v = rng.unit();
      if (u + v > 1.0) {
        u = 1.0 - u;
        v = 1.0 - v;
      }
      const P3 q = a + (surface.corner(t, 1) - a) * u + (surface.corner(t, 2) - a) * v;
      if (q.y <= band_top && q.y >= ground - 0.01) {
        point = q;
        break;
      }
    }
    const P3 n = surface.face_normal[t];
    Walker w;
    w.p = point + n * k_surface_lift;
    w.n = n;
    w.d = initial_direction(n, rng);
    w.budget = static_cast<f64>(params.max_length_m);
    queue.push_back(w);
  }
  for (const Vec3 anchor : params.anchors) {
    HostHit hit;
    const P3 at{static_cast<f64>(anchor.x), static_cast<f64>(anchor.y), static_cast<f64>(anchor.z)};
    if (!bvh.closest(at, search, true, hit)) continue;
    Walker w;
    w.p = hit.point + hit.normal * k_surface_lift;
    w.n = hit.normal;
    w.d = initial_direction(hit.normal, rng);
    w.budget = static_cast<f64>(params.max_length_m);
    queue.push_back(w);
  }
  report.seeds = queue.size();
  for (const Walker& w : queue)
    report.seed_height += w.p.y - ground;
  report.seed_height /= static_cast<f64>(queue.size() > 0 ? queue.size() : 1u);

  const f64 step = static_cast<f64>(params.step_m);
  const f64 reach = static_cast<f64>(params.adhesion_distance_m);
  const f64 contact = static_cast<f64>(params.tip_radius_m) + k_surface_lift;
  const P3 up{0.0, 1.0, 0.0};
  const P3 down{0.0, -1.0, 0.0};
  const f64 branch_angle = static_cast<f64>(params.branch_angle_deg) * (k_pi / 180.0);
  for (u32 q = 0; q < queue.size(); ++q) {
    Walker w = queue[q];
    Rng walk = stream_of(params.seed, 1000u + q);
    VineStem stem;
    stem.parent = w.parent;
    stem.at = w.at;
    stem.depth = w.depth;
    stem.points.push_back(w.p);
    stem.normals.push_back(w.n);
    stem.held.push_back(1u);
    stem.free_run.push_back(0.0);
    f64 grown = 0.0;
    f64 free = 0.0;
    while (grown < w.budget && report.steps < k_step_cap) {
      HostHit hit;
      const bool near = bvh.closest(w.p, reach, true, hit);
      const bool holds = near && surface.roughness[hit.triangle] >= params.hold_roughness;
      P3 dir = w.d * static_cast<f64>(params.primary_weight);
      dir = dir + up * static_cast<f64>(params.light_weight);
      dir = dir + walk.direction() * static_cast<f64>(params.random_weight);
      if (holds) {
        dir = dir + normalized(hit.point - w.p) * static_cast<f64>(params.adhesion_weight);
      } else {
        const f64 sag = free / 0.25 < 1.0 ? free / 0.25 : 1.0;
        dir = dir + down * (static_cast<f64>(params.gravity_weight) * (0.25 + 0.75 * sag));
      }
      if (near) {
        const f64 into = dot(dir, hit.normal);
        // Held, the stem keeps to the surface's plane; leaning, it may leave it but not enter it.
        if (holds || into < 0.0) dir = tangent_of(dir, hit.normal);
      }
      if (length_of(dir) < 1e-9) dir = w.d;
      dir = normalized(dir);
      P3 next = w.p + dir * step;
      P3 normal = near ? hit.normal : w.n;
      bool held = false;
      if (holds) {
        HostHit onto;
        if (bvh.closest(next, step * 2.0 + reach, true, onto)) {
          next = onto.point + onto.normal * k_surface_lift;
          normal = onto.normal;
          held = surface.roughness[onto.triangle] >= params.hold_roughness;
        }
      } else {
        HostHit touch;
        if (bvh.closest(next, contact, false, touch) &&
            dot(next - touch.point, touch.normal) < contact) {
          next = touch.point + touch.normal * contact;
          normal = touch.normal;
        }
      }
      ++report.steps;
      if (held) {
        ++report.held_steps;
        free = 0.0;
      } else if (near) {
        ++report.leaning_steps;
        free += step;
      } else {
        ++report.free_steps;
        free += step;
      }
      if (free > static_cast<f64>(params.max_free_m) || next.y < ground - 0.01) break;
      const P3 moved = next - w.p;
      const f64 travelled = length_of(moved);
      if (travelled < step * 1e-3) break;  // wedged: nowhere left to go
      w.d = normalized(moved);
      w.p = next;
      w.n = normal;
      grown += travelled;
      stem.points.push_back(next);
      stem.normals.push_back(normal);
      stem.held.push_back(held ? 1u : 0u);
      stem.free_run.push_back(free);
      if (held && w.depth < params.branch_depth &&
          walk.unit() < static_cast<f64>(params.branch_chance)) {
        Walker child;
        child.p = next;
        child.n = normal;
        const f64 side = walk.unit() < 0.5 ? -1.0 : 1.0;
        child.d = normalized(rotate(w.d, normal, side * branch_angle));
        child.budget = (w.budget - grown) * 0.7;
        child.parent = stems.size();
        child.at = stem.points.size() - 1u;
        child.depth = w.depth + 1u;
        if (child.budget > step * 4.0) queue.push_back(child);
      }
    }
    report.length_m += grown;
    stems.push_back(std::move(stem));
  }
  // The tips each stem carries, children after their parents.
  for (u32 s = stems.size(); s-- > 0;) {
    if (stems[s].parent != ~0u) stems[stems[s].parent].tips += stems[s].tips;
  }
  report.stems = stems.size();
  f64 tips = 0.0;
  u32 seed_stems = 0;
  for (const VineStem& stem : stems) {
    if (stem.parent != ~0u) continue;
    tips += stem.points.back().y - ground;
    ++seed_stems;
  }
  report.tip_height = tips / static_cast<f64>(seed_stems > 0 ? seed_stems : 1u);
  return true;
}

bool solve_vine(const HostSurface& surface, const HostBvh& bvh, const HostSite& site,
                const Vine& params, assets::MeshData& out, flora::Motion& motion,
                VineReport* report_out, std::string* error_out) {
  out = assets::MeshData{};
  motion = flora::Motion{};
  std::string error;
  VineReport report;
  if (!vine_in_range(params, error)) {
    if (error_out != nullptr) *error_out = error;
    return false;
  }
  Vector<VineStem> stems;
  if (!grow_vine(surface, bvh, site, params, stems, report, error)) {
    if (error_out != nullptr) *error_out = error;
    return false;
  }
  const f64 tip_r = static_cast<f64>(params.tip_radius_m);
  const f64 stem_r = static_cast<f64>(params.stem_radius_m);
  const f64 step = static_cast<f64>(params.step_m);
  const u32 row_points = static_cast<u32>(std::ceil(static_cast<f64>(params.row_m) / step - 1e-9));
  const u32 base_row = row_points < 1 ? 1u : row_points;
  const u32 leaf_points_raw =
      static_cast<u32>(std::ceil(static_cast<f64>(params.leaf_spacing_m) / step - 1e-9));
  const u32 base_leaf = leaf_points_raw < 1 ? 1u : leaf_points_raw;
  // The budget's ladder: coarser rows, then fewer leaves, alternately; the walk never moves.
  u32 row_stride = 1;
  u32 leaf_stride = 1;
  const auto rows_of = [&](const VineStem& stem, u32 stride) {
    const u32 every = base_row * stride;
    const u32 n = stem.points.size();
    return n < 2 ? 0u : (n - 1u + every - 1u) / every + 1u;
  };
  const auto count = [&](u32 rs, u32 ls) {
    u64 triangles = 0;
    for (const VineStem& stem : stems) {
      const u32 rows = rows_of(stem, rs);
      triangles += flora::kit_tube_triangles(rows, params.sides);
      if (stem.points.size() > 1)
        triangles += static_cast<u64>((stem.points.size() - 1u) / (base_leaf * ls)) *
                     flora::k_kit_leaf_triangles;
    }
    return triangles;
  };
  for (u32 rung = 0; count(row_stride, leaf_stride) > params.triangle_budget; ++rung) {
    if (rung >= 16) {
      if (error_out != nullptr)
        *error_out = "the vine does not fit triangle_budget even at its coarsest rung";
      return false;
    }
    if (rung % 2 == 0) {
      row_stride *= 2;
    } else {
      leaf_stride *= 2;
    }
  }
  report.row_stride = row_stride;
  report.leaf_stride = leaf_stride;

  out.materials.push_back(assets::Material{});
  out.materials[0].name = "vine stem";
  out.materials[0].base_color = Vec4{0.33f, 0.27f, 0.18f, 1.0f};
  out.materials[0].roughness = 0.9f;
  out.materials[0].metallic = 0.0f;
  out.materials.push_back(assets::Material{});
  out.materials[1].name = "vine leaf";
  out.materials[1].base_color = Vec4{0.12f, 0.27f, 0.07f, 1.0f};
  out.materials[1].roughness = 0.55f;
  out.materials[1].metallic = 0.0f;
  out.materials[1].double_sided = true;

  flora::KitWriter writer{out, motion};
  // Part 0: the root, a pivot with no geometry, so nothing of the vine is a collider.
  const P3 root = stems.empty() ? P3{} : stems[0].points[0];
  flora::kit_begin_part(writer, flora::KitPoint{root.x, root.y, root.z},
                        flora::KitPoint{0.0, 1.0, 0.0}, 0.0, geometry::k_motion_no_parent, 0.0, 0.0,
                        0u);
  const auto radius_at = [&](const VineStem& stem, f64 s) {
    f64 base = tip_r * std::sqrt(static_cast<f64>(stem.tips));
    base = base > stem_r ? stem_r : (base < tip_r ? tip_r : base);
    return base + (tip_r - base) * s;
  };
  const auto weight_at = [&](const VineStem& stem, u32 i) {
    if (stem.held[i] != 0) return 0.0;
    const f64 w = stem.free_run[i] / 1.0;
    return w > 1.0 ? 1.0 : w;
  };
  Vector<u32> part_of_stem;
  Vector<flora::KitPoint> points;
  Vector<f64> radii;
  Vector<f64> weights;
  const u32 stem_first_index = out.indices.size();
  for (u32 s = 0; s < stems.size(); ++s) {
    const VineStem& stem = stems[s];
    const u32 n = stem.points.size();
    f64 length = 0.0;
    for (u32 i = 1; i < n; ++i)
      length += length_of(stem.points[i] - stem.points[i - 1]);
    const u32 parent = stem.parent == ~0u ? 0u : part_of_stem[stem.parent];
    const P3 start = stem.points[0];
    const P3 heading = n > 1 ? normalized(stem.points[n - 1] - start) : P3{0.0, 1.0, 0.0};
    const u32 part = flora::kit_begin_part(writer, flora::KitPoint{start.x, start.y, start.z},
                                           flora::KitPoint{heading.x, heading.y, heading.z}, length,
                                           parent, radius_at(stem, 0.0), tip_r, 0u);
    part_of_stem.push_back(part);
    if (n < 2) continue;
    points.clear();
    radii.clear();
    weights.clear();
    const u32 every = base_row * row_stride;
    for (u32 i = 0;; i += every) {
      const u32 at = i < n - 1u ? i : n - 1u;
      const f64 s01 = static_cast<f64>(at) / static_cast<f64>(n - 1u);
      const f64 r = radius_at(stem, s01);
      // A held point stands its radius off the surface; a free one is where the walk left it.
      const P3 p = stem.held[at] != 0 ? stem.points[at] + stem.normals[at] * r : stem.points[at];
      points.push_back(flora::KitPoint{p.x, p.y, p.z});
      radii.push_back(r);
      weights.push_back(weight_at(stem, at));
      if (at == n - 1u) break;
    }
    flora::kit_polyline_tube(writer, std::span<const flora::KitPoint>(points.data(), points.size()),
                             std::span<const f64>(radii.data(), radii.size()),
                             std::span<const f64>(weights.data(), weights.size()), params.sides);
  }
  const u32 stem_index_count = out.indices.size() - stem_first_index;
  // The leaves, on petioles off the stems' outer side, alternate, facing out and up.
  const u32 leaf_first_index = out.indices.size();
  const P3 up{0.0, 1.0, 0.0};
  for (u32 s = 0; s < stems.size(); ++s) {
    const VineStem& stem = stems[s];
    const u32 n = stem.points.size();
    if (n < 2) continue;
    Rng leaves = stream_of(params.seed, 500000u + s);
    writer.part = part_of_stem[s];
    const u32 every = base_leaf * leaf_stride;
    u32 alternate = 0;
    for (u32 i = every; i < n; i += every) {
      const f64 s01 = static_cast<f64>(i) / static_cast<f64>(n - 1u);
      const P3 tangent = normalized(stem.points[i < n - 1u ? i + 1u : i] - stem.points[i - 1u]);
      const P3 normal = stem.normals[i];
      P3 side = cross(normal, tangent);
      if (length_of(side) < 1e-6) side = cross(up, tangent);
      side = normalized(side);
      const f64 flip = (alternate++ % 2u == 0u) ? 1.0 : -1.0;
      const f64 r = radius_at(stem, s01);
      const P3 base = stem.points[i] + normal * (r + static_cast<f64>(params.petiole_m) * 0.6) +
                      side * (flip * static_cast<f64>(params.petiole_m) * 0.5);
      const P3 out_dir =
          normalized(normal * 0.55 + up * 0.55 + side * (flip * 0.35) + leaves.direction() * 0.2);
      const f64 length = static_cast<f64>(params.leaf_length_m) * (0.8 + 0.4 * leaves.unit());
      writer.weight = weight_at(stem, i);  // a leaf on a held stem is as fixed as its stem
      flora::kit_leaf_card(writer, flora::KitPoint{base.x, base.y, base.z},
                           flora::KitPoint{out_dir.x, out_dir.y, out_dir.z},
                           flora::KitPoint{side.x, side.y, side.z}, length,
                           length * static_cast<f64>(params.leaf_width), 0.15);
      ++report.leaves;
    }
  }
  const u32 leaf_index_count = out.indices.size() - leaf_first_index;
  if (stem_index_count == 0) {
    if (error_out != nullptr) *error_out = "the vine found nowhere on the host to grow";
    return false;
  }
  out.primitives.push_back(assets::Primitive{stem_first_index, stem_index_count, 0, -1});
  if (leaf_index_count > 0)
    out.primitives.push_back(assets::Primitive{leaf_first_index, leaf_index_count, 1, -1});
  report.triangles = out.indices.size() / 3u;
  if (report_out != nullptr) *report_out = report;
  return true;
}

}  // namespace engine::cover
