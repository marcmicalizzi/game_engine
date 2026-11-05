#include "detail.h"
#include "geology.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/rocks/deposits.h>
#include <domain/texture/material_textures.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::rocks {
namespace {
using namespace detail;
constexpr i32 k_dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr i32 k_dz[] = {-1, -1, -1, 0, 0, 1, 1, 1};
constexpr f64 k_diag = 1.4142135623730951;
constexpr f32 k_min_depth = 0.001f;
bool fail(std::string& error, const char* field) {
  error = std::string("engine.rocks deposits: invalid ") + field;
  return false;
}
bool in(f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; }
f64 tangent(f32 degrees) {
  const f64 a = wide(degrees) * (k_pi / 180.0);
  return det::sin(a) / det::cos(a);
}
u32 neighbour(u32 i, u32 w, u32 h, u32 d) {
  const i32 x = static_cast<i32>(i % w) + k_dx[d];
  const i32 z = static_cast<i32>(i / w) + k_dz[d];
  return x < 0 || z < 0 || x >= static_cast<i32>(w) || z >= static_cast<i32>(h)
             ? ~0u
             : static_cast<u32>(z) * w + static_cast<u32>(x);
}
f64 step(u32 d, f64 spacing) { return (k_dx[d] != 0 && k_dz[d] != 0) ? spacing * k_diag : spacing; }
std::string canonical(const DepositPatch& p) {
  JsonWriteOptions o;
  o.pretty = false;
  JsonValue json = schema::to_json(p);
  drop_absent_sampling(json);
  return write_json(json, o);
}
struct Front {
  f64 head;
  f64 distance;
  u32 cell;
};
bool less_front(const Front& a, const Front& b) {
  if (a.head != b.head) return a.head < b.head;
  if (a.distance != b.distance) return a.distance > b.distance;
  return a.cell > b.cell;
}
// Same diagonal as the surface mesh, not bilinear interpolation through a different surface.
f64 sample(const DepositPatch& p, const Vector<f32>& values, f64 x, f64 z) {
  const f64 fx = std::clamp(x / wide(p.spacing_m), 0.0, static_cast<f64>(p.width - 1));
  const f64 fz = std::clamp(z / wide(p.spacing_m), 0.0, static_cast<f64>(p.depth - 1));
  const u32 ix = std::min(static_cast<u32>(fx), p.width - 2);
  const u32 iz = std::min(static_cast<u32>(fz), p.depth - 2);
  const f64 u = fx - ix, v = fz - iz;
  const u32 a = iz * p.width + ix;
  if (u + v <= 1.0)
    return wide(values[a]) * (1.0 - u - v) + wide(values[a + 1]) * u +
           wide(values[a + p.width]) * v;
  return wide(values[a + 1]) * (1.0 - v) + wide(values[a + p.width]) * (1.0 - u) +
         wide(values[a + p.width + 1]) * (u + v - 1.0);
}
}  // namespace

bool plan_deposits(const DepositLayer& p, const scene_gen::Ground& ground, DepositPlan& out,
                   std::string& error) {
  out = {};
  if (!ground.present()) return fail(error, "ground (required)");
  if (!in(wide(p.spacing_m), .25, 8)) return fail(error, "spacing_m");
  const i64 spacing = scene_gen::nearest_mm(wide(p.spacing_m));
  if (static_cast<f32>(static_cast<f64>(spacing) / 1000.0) != p.spacing_m)
    return fail(error, "spacing_m (whole millimetres)");
  for (u32 a = 0; a < 2; ++a) {
    if (!in(p.region_min[a], -1e8, 1e8) || !in(p.region_max[a], -1e8, 1e8) ||
        p.region_max[a] <= p.region_min[a])
      return fail(error, "region_min/max");
    if (scene_gen::nearest_mm(p.region_min[a]) % spacing != 0 ||
        scene_gen::nearest_mm(p.region_max[a]) % spacing != 0)
      return fail(error, "region alignment");
  }
  if (p.max_samples < 4 || p.max_samples > 1048576) return fail(error, "max_samples");
  if (!in(wide(p.source_deg), 40, 85) || !in(wide(p.repose_deg), 25, 40) ||
      p.source_deg <= p.repose_deg)
    return fail(error, "source_deg/repose_deg");
  if (!in(wide(p.talus_depth_m), 0, 20) || !in(wide(p.runout_m), 1, 1000) ||
      !in(wide(p.channel_area_m2), 1, 1e6) || !in(wide(p.channel_deg), 1, 30) ||
      !in(wide(p.sediment_depth_m), 0, 2) || !in(wide(p.clear_m), 0, 100))
    return fail(error, "deposition controls");
  DepositPlan plan;
  plan.x_mm = scene_gen::nearest_mm(p.region_min[0]);
  plan.z_mm = scene_gen::nearest_mm(p.region_min[1]);
  plan.spacing_mm = spacing;
  const u64 w =
      static_cast<u64>((scene_gen::nearest_mm(p.region_max[0]) - plan.x_mm) / spacing) + 1;
  const u64 h =
      static_cast<u64>((scene_gen::nearest_mm(p.region_max[1]) - plan.z_mm) / spacing) + 1;
  if (w > p.max_samples || h > p.max_samples || w * h > p.max_samples)
    return fail(error, "max_samples (region exceeds grid budget)");
  plan.width = static_cast<u32>(w);
  plan.depth = static_cast<u32>(h);
  const u32 n = plan.width * plan.depth;
  plan.ground.resize(n);
  plan.thickness.resize(n, 0.0f);
  plan.coarse.resize(n, 0.0f);
  plan.drainage_m2.resize(n, wide(p.spacing_m) * wide(p.spacing_m));
  Vector<u32> order(n), next(n, ~0u);
  Vector<f64> slopes(n, 0.0), heads(n, -std::numeric_limits<f64>::infinity()), distances(n, 0.0);
  for (u32 i = 0; i < n; ++i) {
    plan.ground[i] = ground.surface_mm(plan.x_mm + static_cast<i64>(i % plan.width) * spacing,
                                       plan.z_mm + static_cast<i64>(i / plan.width) * spacing);
    if (!in(plan.ground[i], -1e6, 1e6)) return fail(error, "ground height (finite +/-1e6 m)");
    order[i] = i;
  }
  for (u32 i = 0; i < n; ++i)
    for (u32 d = 0; d < 8; ++d) {
      const u32 j = neighbour(i, plan.width, plan.depth, d);
      if (j == ~0u) continue;
      const f64 slope = (plan.ground[i] - plan.ground[j]) / step(d, wide(p.spacing_m));
      if (slope > slopes[i]) {
        slopes[i] = slope;
        next[i] = j;
      }
    }
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return plan.ground[a] != plan.ground[b] ? plan.ground[a] > plan.ground[b] : a < b;
  });
  for (u32 i : order)
    if (next[i] != ~0u) plan.drainage_m2[next[i]] += plan.drainage_m2[i];
  Vector<Front> queue;
  const f64 source = tangent(p.source_deg), repose = tangent(p.repose_deg);
  if (p.talus_depth_m > 0)
    for (u32 i = 0; i < n; ++i)
      if (slopes[i] >= source) {
        heads[i] = plan.ground[i];
        queue.push_back({heads[i], 0.0, i});
      }
  std::make_heap(queue.begin(), queue.end(), less_front);
  while (!queue.empty()) {
    std::pop_heap(queue.begin(), queue.end(), less_front);
    const Front f = queue.back();
    queue.pop_back();
    if (f.head != heads[f.cell] || f.distance != distances[f.cell]) continue;
    for (u32 d = 0; d < 8; ++d) {
      const u32 j = neighbour(f.cell, plan.width, plan.depth, d);
      if (j == ~0u || plan.ground[j] > plan.ground[f.cell]) continue;
      const f64 travel = f.distance + step(d, wide(p.spacing_m));
      const f64 carried = slopes[f.cell] < source
                              ? std::min(f.head, plan.ground[f.cell] + wide(p.talus_depth_m))
                              : f.head;
      const f64 head = carried - repose * step(d, wide(p.spacing_m));
      if (travel > wide(p.runout_m) || head <= plan.ground[j] || head < heads[j] ||
          (head == heads[j] && travel >= distances[j]))
        continue;
      heads[j] = head;
      distances[j] = travel;
      queue.push_back({head, travel, j});
      std::push_heap(queue.begin(), queue.end(), less_front);
    }
  }
  const f64 channel = tangent(p.channel_deg);
  for (u32 i = 0; i < n; ++i) {
    // Talus occupies reachable ground below the source threshold, including the foot transition.
    const f64 talus = slopes[i] < source
                          ? std::clamp(heads[i] - plan.ground[i], 0.0, wide(p.talus_depth_m))
                          : 0.0;
    const f64 sediment = plan.drainage_m2[i] >= wide(p.channel_area_m2)
                             ? wide(p.sediment_depth_m) * clamp01(1.0 - slopes[i] / channel)
                             : 0.0;
    plan.thickness[i] = static_cast<f32>(std::max(talus, sediment));
    plan.coarse[i] = talus > sediment
                         ? static_cast<f32>(.2 + .8 * clamp01(distances[i] / wide(p.runout_m)))
                         : 0.0f;
  }
  out = std::move(plan);
  return true;
}

bool deposit_patch_in_range(const DepositPatch& p, std::string& error) {
  if (p.width < 2 || p.width > 65 || p.depth < 2 || p.depth > 65)
    return fail(error, "patch dimensions");
  if (!in(wide(p.spacing_m), .25, 8) || wide(p.spacing_m) * (p.width - 1) > 32 ||
      wide(p.spacing_m) * (p.depth - 1) > 32)
    return fail(error, "patch spacing/span");
  const u32 n = p.width * p.depth;
  if (p.ground.size() != n || p.thickness.size() != n || p.coarse.size() != n)
    return fail(error, "patch array sizes");
  for (u32 i = 0; i < n; ++i)
    if (!in(wide(p.ground[i]), -10000, 10000) || !in(wide(p.thickness[i]), 0, 20) ||
        !in(wide(p.coarse[i]), 0, 1))
      return fail(error, "patch samples");
  if (p.fragments > 4096 || p.triangle_budget < 500 || p.triangle_budget > 1000000 ||
      !in(wide(p.fragment_m), .02, std::min(2.0, wide(p.spacing_m) * .5)))
    return fail(error, "patch fragment/budget");
  // Reuse the existing rock/material validator, but this patch has repeated UVs, not a baked atlas.
  RockFile rock;
  rock.boulder = Boulder{};
  rock.boulder->type = p.type;
  rock.boulder->material = p.material;
  if (!rock_file_in_range(rock, error)) return false;
  if (p.material.surface_maps) return fail(error, "patch material.surface_maps (unsupported)");
  return true;
}

bool solve_deposit(const DepositPatch& p, assets::MeshData& out, std::string& error) {
  out = {};
  if (!deposit_patch_in_range(p, error)) return false;
  const u32 w = p.width, h = p.depth, n = w * h;
  Vector<u8> active((w - 1) * (h - 1), 0);
  Vector<u32> cells;
  for (u32 z = 0; z + 1 < h; ++z)
    for (u32 x = 0; x + 1 < w; ++x) {
      const u32 a = z * w + x;
      if (std::max({p.thickness[a], p.thickness[a + 1], p.thickness[a + w],
                    p.thickness[a + w + 1]}) > k_min_depth) {
        active[z * (w - 1) + x] = 1;
      }
    }
  // A pair of cells meeting only at a corner would share a vertical boundary edge four ways.
  // Bridge that ambiguous corner with a zero-depth substrate cell. Its top stays buried, but
  // the closed bed remains two-manifold for simplification and collision. Each cell is added
  // at most once; only its four corners need another look.
  Vector<u32> corners;
  for (u32 z = 1; z + 1 < h; ++z)
    for (u32 x = 1; x + 1 < w; ++x)
      corners.push_back(z * w + x);
  while (!corners.empty()) {
    const u32 vertex = corners.back();
    corners.pop_back();
    const u32 x = vertex % w, z = vertex / w;
    const u32 nw = (z - 1) * (w - 1) + x - 1, ne = nw + 1, sw = nw + w - 1, se = sw + 1;
    u32 fill = ~0u;
    if (active[nw] && active[se] && !active[ne] && !active[sw]) fill = ne;
    if (active[ne] && active[sw] && !active[nw] && !active[se]) fill = nw;
    if (fill == ~0u) continue;
    active[fill] = 1;
    const u32 cx = fill % (w - 1), cz = fill / (w - 1);
    for (u32 dz = 0; dz < 2; ++dz)
      for (u32 dx = 0; dx < 2; ++dx)
        if (cx + dx > 0 && cx + dx + 1 < w && cz + dz > 0 && cz + dz + 1 < h)
          corners.push_back((cz + dz) * w + cx + dx);
  }
  for (u32 z = 0; z + 1 < h; ++z)
    for (u32 x = 0; x + 1 < w; ++x)
      if (active[z * (w - 1) + x]) cells.push_back(z * w + x);
  if (cells.empty()) return fail(error, "patch (no deposit)");
  auto present = [&](i32 x, i32 z) {
    return x >= 0 && z >= 0 && x < static_cast<i32>(w - 1) && z < static_cast<i32>(h - 1) &&
           active[static_cast<u32>(z) * (w - 1) + static_cast<u32>(x)] != 0;
  };
  u32 surface_triangles = 4 * cells.size();
  for (u32 a : cells) {
    const i32 x = static_cast<i32>(a % w), z = static_cast<i32>(a / w);
    surface_triangles += 2u * static_cast<u32>(!present(x - 1, z) + !present(x + 1, z) +
                                               !present(x, z - 1) + !present(x, z + 1));
  }
  if (static_cast<u64>(surface_triangles) + 192ull * p.fragments > p.triangle_budget)
    return fail(error, "patch triangle_budget (surface plus every fragment at minimum rung)");
  u32 rung = 4;
  const u32 each = p.fragments > 0 ? (p.triangle_budget - surface_triangles) / p.fragments : 0;
  for (u32 r : k_grid_rungs)
    if (r <= 20 && triangles_of_grid(r) <= each) rung = r;
  assets::MeshData mesh;
  assets::Material mat = rock_material(p.type);
  material_factors(p.material, mat.base_color, mat.roughness);
  mesh.materials.push_back(mat);
  Vector<f32> surface(n);
  for (u32 i = 0; i < n; ++i)
    surface[i] = p.ground[i] + p.thickness[i] - k_min_depth;
  for (u32 bottom = 0; bottom < 2; ++bottom)
    for (u32 i = 0; i < n; ++i) {
      const f32 x = static_cast<f32>(i % w) * p.spacing_m,
                z = static_cast<f32>(i / w) * p.spacing_m;
      mesh.positions.push_back({x, bottom ? p.ground[i] - .05f : surface[i], z});
      mesh.normals.push_back({0, 0, 0});
      mesh.uvs.push_back({x / p.material.tile_m, z / p.material.tile_m});
    }
  auto tri = [&](u32 a, u32 b, u32 c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
  };
  auto edge = [&](u32 a, u32 b) {
    // Boundary walls have their own normals; their buried vertical faces must not bend the bed
    // shading.
    const u32 first = mesh.positions.size();
    for (u32 v : {a, b, b + n, a + n}) {
      const Vec3 position = mesh.positions[v];
      const Vec2 uv = mesh.uvs[v];
      mesh.positions.push_back(position);
      mesh.uvs.push_back(uv);
      mesh.normals.push_back({0, 0, 0});
    }
    tri(first, first + 1, first + 2);
    tri(first, first + 2, first + 3);
  };
  for (u32 a : cells) {
    const u32 b = a + 1, c = a + w, d = c + 1;
    tri(a, c, b);
    tri(b, c, d);
    tri(a + n, b + n, c + n);
    tri(b + n, d + n, c + n);
    const i32 x = static_cast<i32>(a % w), z = static_cast<i32>(a / w);
    if (!present(x, z - 1)) edge(a, b);
    if (!present(x + 1, z)) edge(b, d);
    if (!present(x, z + 1)) edge(d, c);
    if (!present(x - 1, z)) edge(c, a);
  }
  // Area-weighted normals for the connected bed; fragment normals stay their own.
  for (u32 t = 0; t < mesh.indices.size(); t += 3) {
    const u32 a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
    const Vec3 normal =
        cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
    mesh.normals[a] = mesh.normals[a] + normal;
    mesh.normals[b] = mesh.normals[b] + normal;
    mesh.normals[c] = mesh.normals[c] + normal;
  }
  for (Vec3& normal : mesh.normals)
    normal = length(normal) > 0 ? normalize(normal) : Vec3{0, 1, 0};
  for (u32 f = 0; f < p.fragments; ++f) {
    Rng rng(mix64(p.seed ^ mix64(static_cast<u64>(f) + 1)));
    const u32 a = cells[static_cast<u32>(static_cast<u64>(f) * cells.size() / p.fragments)];
    const f64 x = (static_cast<f64>(a % w) + rng.range(.25, .75)) * wide(p.spacing_m);
    const f64 z = (static_cast<f64>(a / w) + rng.range(.25, .75)) * wide(p.spacing_m);
    const f64 coarse = sample(p, p.coarse, x, z), pick = rng.unit();
    const f64 size = pick < .65 ? rng.range(.004, .032)
                                : (pick < .92 ? wide(p.fragment_m) * rng.range(.08, .3)
                                              : wide(p.fragment_m) * rng.range(.4, 1.0));
    const f64 radius =
        std::min(size * (coarse > 0 ? .35 + .65 * coarse : .08), wide(p.spacing_m) * .2);
    const f64 r = coarse > 0 ? std::max(.004, radius) : rng.range(.004, .032);
    const f64 y = sample(p, surface, x, z);
    const u32 first = mesh.positions.size();
    const D3 half{r, r * rng.range(.35, .7), r * rng.range(.6, 1)};
    const f64 yaw = rng.range(0, 2 * k_pi);
    const u64 seed = rng.next();
    emit_fragment(p.type, half, {x, y, z}, yaw, .3, seed, rung, wide(p.material.tile_m), mesh);
    // Support shear is local to each fragment; its existing angular fabric is retained.
    const f64 eps = wide(p.spacing_m) * .01;
    const f64 sx = (sample(p, surface, x + eps, z) - sample(p, surface, x - eps, z)) / (2 * eps);
    const f64 sz = (sample(p, surface, x, z + eps) - sample(p, surface, x, z - eps)) / (2 * eps);
    for (u32 v = first; v < mesh.positions.size(); ++v) {
      Vec3& pos = mesh.positions[v];
      pos.y = static_cast<f32>(wide(pos.y) + sample(p, surface, wide(pos.x), wide(pos.z)) - y);
      const Vec3 q = mesh.normals[v];
      const D3 nn = normalized({wide(q.x) - sx * wide(q.y), wide(q.y), wide(q.z) - sz * wide(q.y)});
      mesh.normals[v] = {static_cast<f32>(nn.x), static_cast<f32>(nn.y), static_cast<f32>(nn.z)};
    }
  }
  Vector<u32> remap(mesh.positions.size(), ~0u);
  for (u32 index : mesh.indices)
    remap[index] = 0;
  u32 used = 0;
  for (u32 i = 0; i < remap.size(); ++i)
    if (remap[i] != ~0u) {
      remap[i] = used;
      mesh.positions[used] = mesh.positions[i];
      mesh.normals[used] = mesh.normals[i];
      mesh.uvs[used] = mesh.uvs[i];
      ++used;
    }
  for (u32& index : mesh.indices)
    index = remap[index];
  mesh.positions.resize(used);
  mesh.normals.resize(used);
  mesh.uvs.resize(used);
  assets::Primitive part;
  part.index_count = mesh.indices.size();
  part.material = 0;
  mesh.primitives.push_back(part);
  out = std::move(mesh);
  return true;
}

u64 deposit_source_hash(const DepositPatch& p) {
  const std::string text = canonical(p);
  return hash_combine(hash_bytes(text.data(), text.size()),
                      hash_combine(k_deposit_version, k_rocks_version));
}

bool build_deposit_container(const DepositPatch& input, const geometry::ClusterLodOptions& lod,
                             u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                             const materials::Pack* pack) {
  out = {};
  DepositPatch p = input;
  materials::MaterialSet set;
  set.base_color = p.material.base_color.value_or("");
  set.normal = p.material.normal.value_or("");
  set.roughness = p.material.roughness.value_or("");
  set.metallic = p.material.metallic.value_or("");
  set.height = p.material.height.value_or("");
  set.tile_m = p.material.tile_m;
  set.normal_scale = p.material.normal_scale;
  bool from_pack = false;
  const auto* chosen = materials::resolve_set(&set, pack, rock_role(p.type), &from_pack);
  if (from_pack) p.material.tile_m = chosen->tile_m;
  assets::MeshData mesh;
  if (!solve_deposit(p, mesh, error)) return false;
  geometry::ClusterLodOptions options = lod;
  options.normal_cones = true;
  if (!geometry::build_cluster_lod(mesh.positions, {mesh.indices.data(), mesh.indices.size()},
                                   options, out.mesh, &error, assets::attribute_source(mesh)))
    return false;
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions pages;
    pages.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(out.mesh, pages, out.pages, &error)) return false;
  }
  out.cluster_material.assign(out.mesh.mesh.clusters.size(), 0u);
  geometry::ClusterFileMaterial material;
  material.base_color = mesh.materials[0].base_color;
  material.metallic = 0;
  material.roughness = mesh.materials[0].roughness;
  out.materials.push_back(material);
  if (chosen) {
    // apply_set requires a sampling record beside the material. Keep the optional section
    // absent for untextured deposits so their existing container bytes remain unchanged.
    out.material_sampling.push_back(geometry::encode_material_sampling(
        mesh.materials[0].sampling, mesh.materials[0].occlusion_strength));
    // The patch's `sampling`, when it names one, places the chosen set's texels (R131).
    materials::MaterialSet applied = *chosen;
    if (p.material.sampling.has_value())
      applied.stochastic = *p.material.sampling == RockSampling::Stochastic;
    materials::apply_set(applied, materials::UvLayout::repeats, out, 0);
    material_factors(p.material, out.materials[0].base_color, out.materials[0].roughness);
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  out.source_hash = deposit_source_hash(input);
  out.build_key = assets::generated_mesh_build_key(
      out.source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  out.generator = "deposit";
  out.generator_kind = "sediment";
  out.generator_params = canonical(input);
  return true;
}
namespace {
bool read_patch(std::span<const u8> bytes, DepositPatch& out, std::string& error) {
  JsonValue json;
  const auto parsed = parse_json({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, json);
  if (!parsed.ok) {
    error = parsed.message;
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, json, ctx) || !ctx.ok()) {
    error = "engine.rocks.DepositPatch";
    for (const auto& d : ctx.diagnostics)
      error += "; " + d.path + ": " + d.message;
    return false;
  }
  return deposit_patch_in_range(out, error);
}
bool source_hash(std::span<const u8> bytes, u64& out, std::string& error) {
  DepositPatch p;
  if (!read_patch(bytes, p, error)) return false;
  out = deposit_source_hash(p);
  return true;
}
bool build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
           const geometry::ClusterLodOptions& lod, u32 pages, geometry::ClusterFileData& out,
           std::string& error) {
  DepositPatch p;
  return read_patch(bytes, p, error) &&
         build_deposit_container(p, lod, pages, out, error, inputs.materials);
}
constexpr assets::MeshGeneratorDesc k_mesh{
    .name = "deposit", .source_hash = &source_hash, .build = &build};
const assets::MeshGeneratorRegistrar k_registrar{k_mesh};
}  // namespace
}  // namespace engine::rocks
