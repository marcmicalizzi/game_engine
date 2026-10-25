#include "kit.h"

#include <domain/flora/sward.h>
#include <foundation/image/decode.h>

#include <algorithm>
#include <utility>

namespace engine::flora {
namespace {
using namespace detail;
// Exact source ranges and missing density evidence: docs/experiments/grass-sward.md.
constexpr SwardProfile k_profiles[] = {
    {"tall_fescue", .05f, .70f, .003f, .013f, 3080, 3093, "open panicle"},
    {"perennial_ryegrass", .03f, .20f, .002f, .006f, 5815, 7414, "spike"},
    {"timothy", .04f, .20f, .003f, .009f, 0, 0, "spiciform panicle"},
    {"cocksfoot", .10f, .45f, .002f, .014f, 0, 0, "fascicled panicle"},
    {"wood_melick", .05f, .20f, .003f, .007f, 0, 0, "open panicle"},
    {"wavy_hairgrass", .05f, .20f, .0003f, .0008f, 0, 0, "open panicle"},
    {"bent", .02f, .15f, .001f, .004f, 0, 0, "open panicle"},
    {"red_fescue", .03f, .40f, .0005f, .001f, 0, 0, "panicle"},
};
struct Blade {
  D3 root, heading;
  f64 length, width, lean, twist, angle;
  u32 group;
  u8 hue, age;
};

// A quadratic centre line, normalized to the requested polyline arc length. Both rungs use
// these same fine samples, so lowering tessellation does not redraw the plant's random choices.
Axis blade_axis(const Blade& b) {
  Axis axis;
  constexpr u32 rows = 24;
  f64 arc = 0;
  for (u32 r = 0; r <= rows; ++r) {
    const f64 t = static_cast<f64>(r) / rows;
    const D3 p = k_up * t + b.heading * (b.lean * t * t);
    if (r != 0) arc += length_of(p - axis.point.back());
    axis.point.push_back(p);
    axis.tangent.push_back(normalized(k_up + b.heading * (2 * b.lean * t)));
    axis.normal.push_back(normalized(cross(b.heading, k_up)));
    axis.arc.push_back(arc);
  }
  const f64 scale = b.length / arc;
  for (u32 r = 0; r <= rows; ++r) {
    axis.point[r] = b.root + axis.point[r] * scale;
    axis.arc[r] *= scale;
  }
  axis.length = b.length;
  return axis;
}

void ribbon(const Blade& b, const Axis& fine, u32 segments, f64 width_scale, Emitter& emit) {
  const Axis axis = resample_axis(fine, segments);
  const u32 first = emit.mesh.positions.size();
  for (u32 r = 0; r <= segments; ++r) {
    const f64 t = static_cast<f64>(r) / segments;
    const D3 side = rotate(normalized(cross(b.heading, k_up)), axis.tangent[r], b.twist * t);
    const D3 normal = normalized(cross(side, axis.tangent[r]));
    emit.weight = axis.arc[r] / axis.length;
    if (r == segments) {
      emit.vertex(axis.point[r], normal, .5, 1);
    } else {
      const f64 taper = t <= .65 ? 1 : (1 - t) / .35;
      const D3 half = side * (b.width * width_scale * taper * .5);
      emit.vertex(axis.point[r] - half, normal, 0, t);
      emit.vertex(axis.point[r] + half, normal, 1, t);
    }
  }
  for (u32 r = 0; r < segments; ++r) {
    const u32 a = first + r * 2;
    emit.triangle(a, a + 1, a + 2);
    if (r + 1 < segments) emit.triangle(a + 1, a + 3, a + 2);
  }
}

void card(const BakeView& view, u32 tile, u32 columns, u32 rows, u32 px, Emitter& emit) {
  const u32 first = emit.mesh.positions.size();
  const f64 u0 = (static_cast<f64>((tile % columns) * px) + 1) / (columns * px);
  const f64 v0 = (static_cast<f64>((tile / columns) * px) + 1) / (rows * px);
  for (u32 v = 0; v < 2; ++v) {
    for (u32 u = 0; u < 2; ++u) {
      const f64 a = static_cast<f64>(u ? view.u_max : view.u_min);
      const f64 b = static_cast<f64>(v ? view.v_max : view.v_min);
      const D3 p = from_f32(view.origin) + from_f32(view.u_axis) * a + from_f32(view.v_axis) * b;
      emit.weight = clamp01(p.y / static_cast<f64>(emit.motion.parts[emit.part].length));
      emit.vertex(p, from_f32(view.normal), u0 + static_cast<f64>(u * (px - 2)) / (columns * px),
                  v0 + static_cast<f64>(v * (px - 2)) / (rows * px));
    }
  }
  emit.triangle(first, first + 1, first + 2);
  emit.triangle(first + 1, first + 3, first + 2);
}
}  // namespace

std::span<const SwardProfile> sward_profiles() noexcept { return k_profiles; }
const SwardProfile* sward_profile(std::string_view name) noexcept {
  for (const auto& p : k_profiles)
    if (p.name == name) return &p;
  return nullptr;
}
bool sward_in_range(const Sward& p, std::string& error) {
  const char* field = nullptr;
  if (sward_profile(p.species) == nullptr)
    field = "species";
  else if (p.variant > 15)
    field = "variant (0..15)";
  else if (p.cell_m != 1 && p.cell_m != 2)
    field = "cell_m (1 or 2)";
  else if (p.blades_m2 < 16 || p.blades_m2 > 1024)
    field = "blades_m2 (16..1024)";
  else if (!(p.density >= .0625f && p.density <= 1))
    field = "density (0.0625..1)";
  else if (!(p.growth >= 0 && p.growth <= 1))
    field = "growth (0..1)";
  else if (!(p.straw_share >= 0 && p.straw_share <= 1))
    field = "straw_share (0..1)";
  else if (!(p.card_cutoff >= .05f && p.card_cutoff <= .95f))
    field = "card_cutoff (0.05..0.95)";
  else if (p.segments < 2 || p.segments > 3)
    field = "segments (2 or 3)";
  else if (p.triangle_budget > 100000)
    field = "triangle_budget (0..100000)";
  else if (p.card_px < 32 || p.card_px > 256 || (p.card_px & (p.card_px - 1)) != 0)
    field = "card_px (power of two 32..256)";
  if (field != nullptr) {
    error = std::string("engine.flora.Sward invalid ") + field;
    return false;
  }
  return true;
}

bool solve_sward(const Sward& p, assets::MeshData& out, TreeReport* report, std::string* error,
                 Motion* motion, TreeLevels* levels,
                 Vector<geometry::ClusterFileSwardVertex>* attributes) {
  out = {};
  if (motion != nullptr) *motion = {};
  if (levels != nullptr) *levels = {};
  if (attributes != nullptr) attributes->clear();
  std::string why;
  if (!sward_in_range(p, why)) {
    if (error) *error = why;
    return false;
  }
  const auto& profile = *sward_profile(p.species);
  const u32 area = static_cast<u32>(p.cell_m * p.cell_m);
  const u32 count = std::max(2u, static_cast<u32>(p.blades_m2 * area * p.density));
  const u32 group_side = 4u * static_cast<u32>(p.cell_m);
  const u32 groups = group_side * group_side;
  const u32 triangles = (count + (count + 1) / 2) * blade_triangles(p.segments) + groups * 6;
  const u32 budget = p.triangle_budget != 0 ? p.triangle_budget : 2400u * area;
  if (triangles > budget) {
    if (error)
      *error = "engine.flora.Sward triangle_budget needs " + std::to_string(triangles) +
               " for all levels; lower blades_m2 explicitly";
    return false;
  }
  // Leaf material stays index 0: no bark, no collider, and no dependency on a material pack.
  out.materials.push_back(material("blade", {.09f, .22f, .06f, 1}, .7f, true));
  Motion parts;
  Emitter emit{out, parts};
  Vector<Blade> blades;
  Vector<Axis> axes;
  Vector<BakeRange> ranges;
  u32 side = 1;
  while (side * side < count)
    ++side;
  const f64 cell = static_cast<f64>(p.cell_m);
  const f64 half = cell * .5;
  const u64 seed = p.seed ^ (static_cast<u64>(p.variant) * 0x9e3779b97f4a7c15ull);
  for (u32 i = 0; i < count; ++i) {
    Rng rng = stream_of(seed, i + 1);
    Blade b{};
    b.root = {((i % side + rng.unit()) / side - .5) * cell, 0,
              ((i / side + rng.unit()) / side - .5) * cell};
    const f64 angle = rng.unit() * 2 * k_pi;
    b.angle = angle;
    b.heading = {det::cos(angle), 0, det::sin(angle)};
    b.length = lerp(static_cast<f64>(profile.length_min_m), static_cast<f64>(profile.length_max_m),
                    rng.unit() * static_cast<f64>(p.growth));
    b.width = lerp(static_cast<f64>(profile.width_min_m), static_cast<f64>(profile.width_max_m),
                   rng.unit());
    b.lean = .15 + .4 * rng.unit();
    b.twist = .2 * rng.symmetric();
    b.hue = static_cast<u8>(102 + rng.next() % 52);
    b.age = rng.unit() < static_cast<f64>(p.straw_share) ? 255 : 0;
    Axis axis = blade_axis(b);
    // Leave enough room for the coarse blade too. Translation, not clamping vertices, preserves
    // the source length and avoids collapsed triangles at patch boundaries.
    D3 lo = axis.point[0], hi = lo;
    for (D3 v : axis.point) {
      lo.x = std::min(lo.x, v.x);
      lo.z = std::min(lo.z, v.z);
      hi.x = std::max(hi.x, v.x);
      hi.z = std::max(hi.z, v.z);
    }
    const D3 shift{std::max(0.0, -half + b.width - lo.x) - std::max(0.0, hi.x + b.width - half), 0,
                   std::max(0.0, -half + b.width - lo.z) - std::max(0.0, hi.z + b.width - half)};
    b.root = b.root + shift;
    for (D3& v : axis.point)
      v = v + shift;
    const u32 gx = std::min(group_side - 1, static_cast<u32>((b.root.x + half) * 4));
    const u32 gz = std::min(group_side - 1, static_cast<u32>((b.root.z + half) * 4));
    b.group = gz * group_side + gx;
    begin_structural_part(emit, b.root, axis.tangent[0], axis.length, geometry::k_motion_no_parent,
                          0, 0, 0);
    const u32 first = out.indices.size();
    ribbon(b, axis, p.segments, 1, emit);
    ranges.push_back({first, out.indices.size() - first});
    blades.push_back(b);
    axes.push_back(std::move(axis));
  }
  const u32 near_end = out.indices.size();
  const f32 near_area = static_cast<f32>(triangles_area(out, 0));
  out.primitives.push_back({0, near_end, 0});
  TreeLevels chain;
  chain.leaf_first_index = 0;
  chain.leaf_index_end = near_end;
  TreeLevel coarse;
  coarse.error = profile.width_max_m * 2;
  coarse.feature_m = profile.length_max_m;
  // Empty jobs distinguish a geometric rung from baked impostors in the sward build.
  // Pair adjacent headings so removing a blade does not bias the projected width toward
  // whichever directions happened to occur at even lattice indices. Keep the longer axis.
  Vector<u32> order;
  for (u32 i = 0; i < count; ++i)
    order.push_back(i);
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return blades[a].angle == blades[b].angle ? a < b : blades[a].angle < blades[b].angle;
  });
  for (u32 pair = 0; pair < count; pair += 2) {
    u32 i = order[pair], j = order[std::min(pair + 1, count - 1)];
    if (blades[i].length < blades[j].length) std::swap(i, j);
    emit.part = i;
    const Blade& a = blades[i];
    const Blade& b = blades[j];
    const f64 scale = pair + 1 < count ? 1 + b.width * b.length / (a.width * a.length) : 1;
    Axis coarse_axis = axes[i];
    const f64 margin = a.width * scale * .5;
    D3 lo = coarse_axis.point[0], hi = lo;
    for (D3 v : coarse_axis.point) {
      lo.x = std::min(lo.x, v.x);
      lo.z = std::min(lo.z, v.z);
      hi.x = std::max(hi.x, v.x);
      hi.z = std::max(hi.z, v.z);
    }
    const D3 shift{std::max(0.0, -half + margin - lo.x) - std::max(0.0, hi.x + margin - half), 0,
                   std::max(0.0, -half + margin - lo.z) - std::max(0.0, hi.z + margin - half)};
    for (D3& v : coarse_axis.point)
      v = v + shift;
    ribbon(a, coarse_axis, p.segments, scale, emit);
  }
  for (u32 i = near_end; i < out.indices.size(); ++i)
    coarse.indices.push_back(out.indices[i]);
  out.indices.resize(near_end);
  chain.levels.push_back(std::move(coarse));
  TreeLevel cards;
  cards.error = std::max(.05f, profile.length_max_m * .5f);
  cards.feature_m = profile.length_max_m;
  cards.tile_px = p.card_px;
  cards.alpha_cutoff = p.card_cutoff;
  cards.columns = group_side * 3;
  cards.rows = group_side;
  for (u32 group = 0; group < groups; ++group) {
    Vector<BakeRange> selected;
    D3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (u32 i = 0; i < count; ++i) {
      if (blades[i].group != group) continue;
      selected.push_back(ranges[i]);
      for (u32 j = ranges[i].first_index; j < ranges[i].first_index + ranges[i].index_count; ++j) {
        const Vec3 v = out.positions[out.indices[j]];
        lo = {std::min(lo.x, static_cast<f64>(v.x)), std::min(lo.y, static_cast<f64>(v.y)),
              std::min(lo.z, static_cast<f64>(v.z))};
        hi = {std::max(hi.x, static_cast<f64>(v.x)), std::max(hi.y, static_cast<f64>(v.y)),
              std::max(hi.z, static_cast<f64>(v.z))};
      }
    }
    if (selected.empty()) continue;
    const D3 centre = (lo + hi) * .5;
    begin_structural_part(emit, {centre.x, 0, centre.z}, k_up, hi.y, geometry::k_motion_no_parent,
                          0, 0, 0);
    for (u32 plane = 0; plane < 3; ++plane) {
      BakeJob job;
      job.group = group;
      job.tile = group * 3 + plane;
      job.card_tile = job.tile;
      job.ranges = selected;
      job.view.origin = to_f32(centre);
      if (plane == 0) {
        job.view.u_axis = {1, 0, 0};
        job.view.v_axis = {0, 1, 0};
        job.view.normal = {0, 0, 1};
        job.view.u_min = static_cast<f32>(lo.x - centre.x);
        job.view.u_max = static_cast<f32>(hi.x - centre.x);
        job.view.v_min = static_cast<f32>(lo.y - centre.y);
        job.view.v_max = static_cast<f32>(hi.y - centre.y);
      } else if (plane == 1) {
        job.view.u_axis = {0, 0, 1};
        job.view.v_axis = {0, 1, 0};
        job.view.normal = {-1, 0, 0};
        job.view.u_min = static_cast<f32>(lo.z - centre.z);
        job.view.u_max = static_cast<f32>(hi.z - centre.z);
        job.view.v_min = static_cast<f32>(lo.y - centre.y);
        job.view.v_max = static_cast<f32>(hi.y - centre.y);
      } else {
        job.view.u_axis = {1, 0, 0};
        job.view.v_axis = {0, 0, 1};
        job.view.normal = {0, -1, 0};
        job.view.u_min = static_cast<f32>(lo.x - centre.x);
        job.view.u_max = static_cast<f32>(hi.x - centre.x);
        job.view.v_min = static_cast<f32>(lo.z - centre.z);
        job.view.v_max = static_cast<f32>(hi.z - centre.z);
      }
      job.card = job.view;
      const f32 um = (job.view.u_max - job.view.u_min) / (p.card_px - 2);
      const f32 vm = (job.view.v_max - job.view.v_min) / (p.card_px - 2);
      job.view.u_min -= um;
      job.view.u_max += um;
      job.view.v_min -= vm;
      job.view.v_max += vm;
      card(job.card, job.tile, cards.columns, cards.rows, p.card_px, emit);
      cards.jobs.push_back(std::move(job));
    }
    ++cards.groups;
  }
  for (u32 i = near_end; i < out.indices.size(); ++i)
    cards.indices.push_back(out.indices[i]);
  out.indices.resize(near_end);
  cards.texel_m = profile.length_max_m / (2 * p.card_px);
  cards.parallax_m = cards.error;
  chain.levels.push_back(std::move(cards));
  if (attributes != nullptr) {
    for (u32 v = 0; v < parts.vertex_part.size(); ++v) {
      const u32 part = parts.vertex_part[v];
      const u8 hue = part < blades.size() ? blades[part].hue : 128;
      const u8 age = part < blades.size() ? blades[part].age : 0;
      attributes->push_back({static_cast<u8>(89 + 166 * parts.vertex_weight[v]), 128, hue, age});
    }
  }
  if (report != nullptr) {
    *report = {};
    report->triangles = near_end / 3;
    for (const auto& level : chain.levels)
      report->triangles += level.indices.size() / 3;
    report->leaf_triangles = report->triangles;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->sprays = count;
    report->leaf_area_m2 = near_area;
  }
  if (motion != nullptr) *motion = std::move(parts);
  if (levels != nullptr) *levels = std::move(chain);
  return true;
}

void bake_sward_ground(const assets::MeshData& mesh, const TreeLevels& levels, f32 cell_m,
                       u32 pixels, BakedAtlas& out, Vector<f32>* height) {
  TreeLevel level;
  level.tile_px = pixels;
  BakeJob job;
  job.view.u_axis = {1, 0, 0};
  job.view.v_axis = {0, 0, 1};
  job.view.normal = {0, 1, 0};
  job.view.u_min = job.view.v_min = -.5f * cell_m;
  job.view.u_max = job.view.v_max = .5f * cell_m;
  job.ranges.push_back({0, levels.leaf_index_end});
  job.card = job.view;
  level.jobs.push_back(std::move(job));
  BakeLook look;
  look.leaf = mesh.materials[0].base_color;
  bake_level(mesh, levels, level, look, out, height);
}
SwardCoverage measure_sward_coverage(const assets::MeshData& mesh, const TreeLevels& levels) {
  SwardCoverage result;
  if (levels.levels.size() != 2 || mesh.materials.empty()) return result;
  BakeLook look;
  look.leaf = mesh.materials[0].base_color;
  BakedAtlas cards;
  bake_level(mesh, levels, levels.levels[1], look, cards);
  image::Image card_image;
  card_image.width = cards.width;
  card_image.height = cards.height;
  card_image.channels = 4;
  card_image.pixels = std::move(cards.colour);
  for (u32 view = 0; view < 4; ++view) {
    const f64 angle = view * .5 * k_pi;
    const D3 n{det::cos(angle) * .8660254037844386, .5, det::sin(angle) * .8660254037844386};
    const D3 u = normalized(cross(k_up, n));
    const D3 v = normalized(cross(n, u));
    BakeView frame;
    frame.u_axis = to_f32(u);
    frame.v_axis = to_f32(v);
    frame.normal = to_f32(n);
    f64 u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
    for (const Vec3& p : mesh.positions) {
      const D3 q = from_f32(p);
      const f64 pu = detail::dot(q, u), pv = detail::dot(q, v);
      u0 = std::min(u0, pu);
      u1 = std::max(u1, pu);
      v0 = std::min(v0, pv);
      v1 = std::max(v1, pv);
    }
    frame.u_min = static_cast<f32>(u0);
    frame.u_max = static_cast<f32>(u1);
    frame.v_min = static_cast<f32>(v0);
    frame.v_max = static_cast<f32>(v1);
    for (u32 tier = 0; tier < 3; ++tier) {
      assets::MeshData source = mesh;
      if (tier != 0) source.indices = levels.levels[tier - 1].indices;
      TreeLevels source_levels;
      source_levels.leaf_index_end = source.indices.size();
      TreeLevel probe;
      probe.tile_px = 256;
      BakeJob job;
      job.view = job.card = frame;
      job.ranges.push_back({0, source.indices.size()});
      probe.jobs.push_back(std::move(job));
      BakeLook sample = look;
      if (tier == 2) {
        sample.leaf = {1, 1, 1, 1};
        sample.leaf_image = &card_image;
        sample.leaf_cutoff = levels.levels[1].alpha_cutoff;
      }
      BakedAtlas projected;
      bake_level(source, source_levels, probe, sample, projected);
      u64 coverage = 0;
      for (u32 i = 3; i < projected.colour.size(); i += 4)
        coverage += projected.colour[i];
      result.fraction[tier][view] =
          static_cast<f32>(static_cast<f64>(coverage) / (256 * 256 * 255));
    }
  }
  return result;
}
}  // namespace engine::flora
