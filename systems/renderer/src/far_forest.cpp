// The far forest (far_forest.h, far_forest_build.h; docs/subsystems/renderer.md, "The far forest";
// ADR-0059): a streamed world's own trees past what its ring draws, merged per world tile into
// one rigid mesh of their coarsest forms and drawn as one prefix instance per tile, handed off
// with the tail by the tail's own block list.
#include "far_forest_build.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/far_forest_handoff.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <renderer_log.h>

namespace engine::renderer {
namespace {

tunables::Float far_min_height{
    "renderer.far_forest.min_height_m", static_cast<f64>(k_far_min_height_m), 0.0, 1000.0,
    "An authored instance shorter than this (its fitted mesh's height times its largest scale) "
    "draws no far form past the world's ring: the understory (renderer.md, \"The far forest\")"};
tunables::Int far_max_triangles{
    "renderer.far_forest.max_triangles", static_cast<i64>(k_far_max_triangles), 1, 1 << 20,
    "A mesh whose coarsest form has more triangles than this draws no far form (renderer.md, "
    "\"The far forest\")"};

constexpr u32 k_entry_magic = 0x31544646u;  // "FFT1"

// ---- one mesh's coarsest form -----------------------------------------------------------------

// What a scene mesh draws from infinitely far — every cluster whose parent error is terminal, the
// roots of its DAG: a tree's whole-tree impostor star where its container has one (flora.md, "The
// impostor chain"), the decimated top of any other mesh — as triangles over vertices of its own,
// with the material each came from. For a star, the **cross**: the planes of two of its four
// directions, a right angle apart, and how far the dropped planes' vertices lie from the kept ones
// (the error the star's own level is held to in flora.md, measured the same way).
struct FarForm {
  bool usable = false;
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  Vector<i32> material;  // per triangle, the source mesh's material (-1: none)
  Vector<u8> cross;      // per triangle, 1 when the cross keeps it
  bool star = false;
  f32 cross_error = 0.0f;  // mesh space
  f32 height = 0.0f;       // the whole mesh's vertical extent, mesh space
  u64 hash = 0;            // over every field above: the tile entry's key takes it
};

Vec3 face_normal(const Vec3& a, const Vec3& b, const Vec3& c) noexcept {
  const Vec3 n = cross(b - a, c - a);
  const f32 len = std::sqrt(dot(n, n));
  return len > 0.0f ? n * (1.0f / len) : Vec3{};
}

void make_form(const geometry::ClusterLodMesh& dag, const SourceMesh& source, FarForm& out) {
  out = FarForm{};
  const geometry::ClusterMesh& mesh = dag.mesh;
  if (mesh.vertices.empty() || dag.lod.size() != mesh.clusters.size()) return;
  f32 lo = mesh.vertices[0].y, hi = lo;
  for (const Vec3& p : mesh.vertices) {
    lo = std::min(lo, p.y);
    hi = std::max(hi, p.y);
  }
  out.height = hi - lo;
  const bool attributes = mesh.attributes.size() == mesh.vertices.size();
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    if (dag.lod[c].parent_error < geometry::k_lod_terminal_error) continue;
    const geometry::ClusterDesc& cluster = mesh.clusters[c];
    i32 material = -1;
    if (c < source.part_of_cluster.size()) {
      const u32 part = source.part_of_cluster[c];
      if (part < source.part_material.size()) material = source.part_material[part];
    }
    const u32 base = out.positions.size();
    for (u32 v = 0; v < cluster.vertex_count; ++v) {
      const u32 at = cluster.vertex_offset + v;
      out.positions.push_back(mesh.vertices[at]);
      out.normals.push_back(attributes ? geometry::decode_normal_oct(mesh.attributes[at].normal_oct)
                                       : Vec3{0.0f, 1.0f, 0.0f});
      out.uvs.push_back(attributes ? geometry::decode_half2(mesh.attributes[at].uv_half2)
                                   : Vec2{0.0f, 0.0f});
    }
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = mesh.triangles[cluster.triangle_offset + t];
      for (u32 k = 0; k < 3; ++k)
        out.indices.push_back(base + geometry::ClusterMesh::unpack(packed, k));
      out.material.push_back(material);
    }
  }
  const u32 triangles = out.indices.size() / 3;
  if (triangles == 0) return;
  out.usable = true;
  // The star: every triangle a vertical plane, in at least three of four directions 45 degrees
  // apart (a plane and its reverse face are one direction).
  constexpr f32 k_quarter = 0.78539816f;
  constexpr f32 k_half_turn = 3.14159265f;
  out.cross.assign(triangles, u8{1});
  bool vertical = true;
  f32 first_azimuth = 0.0f;
  bool have_first = false;
  u32 seen = 0;
  Vector<u32> bin(triangles, 0u);
  for (u32 t = 0; t < triangles && vertical; ++t) {
    const Vec3 n =
        face_normal(out.positions[out.indices[t * 3]], out.positions[out.indices[t * 3 + 1]],
                    out.positions[out.indices[t * 3 + 2]]);
    if (std::fabs(n.y) > 0.2f || (n.x == 0.0f && n.z == 0.0f)) {
      vertical = false;
      break;
    }
    f32 azimuth = std::atan2(n.z, n.x);
    if (azimuth < 0.0f) azimuth += k_half_turn;
    if (azimuth >= k_half_turn) azimuth -= k_half_turn;
    if (!have_first) {
      first_azimuth = azimuth;
      have_first = true;
    }
    const i32 steps = static_cast<i32>(std::lround((azimuth - first_azimuth) / k_quarter));
    bin[t] = static_cast<u32>(((steps % 4) + 4) % 4);
    seen |= 1u << bin[t];
  }
  const u32 directions =
      static_cast<u32>((seen & 1u) + ((seen >> 1) & 1u) + ((seen >> 2) & 1u) + ((seen >> 3) & 1u));
  if (vertical && directions >= 3 && (seen & 5u) != 0) {
    // The kept planes, and how far every vertex of a dropped plane lies from the nearest.
    Vector<Vec4> planes;
    for (u32 t = 0; t < triangles; ++t) {
      out.cross[t] = (bin[t] == 0 || bin[t] == 2) ? u8{1} : u8{0};
      if (out.cross[t] == 0) continue;
      const Vec3 a = out.positions[out.indices[t * 3]];
      const Vec3 n = face_normal(a, out.positions[out.indices[t * 3 + 1]],
                                 out.positions[out.indices[t * 3 + 2]]);
      planes.push_back(Vec4{n.x, n.y, n.z, dot(n, a)});
    }
    f32 error = 0.0f;
    bool dropped = false;
    for (u32 t = 0; t < triangles; ++t) {
      if (out.cross[t] != 0) continue;
      dropped = true;
      for (u32 k = 0; k < 3; ++k) {
        const Vec3 p = out.positions[out.indices[t * 3 + k]];
        f32 nearest = 3.0e38f;
        for (const Vec4& plane : planes)
          nearest = std::min(nearest, std::fabs(dot(Vec3{plane.x, plane.y, plane.z}, p) - plane.w));
        error = std::max(error, nearest);
      }
    }
    if (dropped && !planes.empty() && error > 0.0f && error < 1.0e30f) {
      out.star = true;
      out.cross_error = error;
    }
  }
  if (!out.star) out.cross.assign(triangles, u8{1});
  u64 h = hash_combine(k_far_forest_version, triangles);
  h = hash_bytes(out.positions.data(), out.positions.size() * sizeof(Vec3), h);
  h = hash_bytes(out.normals.data(), out.normals.size() * sizeof(Vec3), h);
  h = hash_bytes(out.uvs.data(), out.uvs.size() * sizeof(Vec2), h);
  h = hash_bytes(out.indices.data(), out.indices.size() * sizeof(u32), h);
  h = hash_bytes(out.material.data(), out.material.size() * sizeof(i32), h);
  h = hash_bytes(out.cross.data(), out.cross.size(), h);
  u32 bits = 0;
  std::memcpy(&bits, &out.cross_error, sizeof(bits));
  out.hash = hash_combine(h, bits);
}

// ---- one far tile ------------------------------------------------------------------------------

struct TileJob {
  // In.
  i32 x = 0;
  i32 z = 0;
  WorldPos corner{};
  Vector<u32> trees;  // instance indices, in the scene's order
  const FarForestInstance* instances = nullptr;
  const FarForestFit* fits = nullptr;
  const FarForm* forms = nullptr;
  const FarForestOptions* options = nullptr;
  // Out.
  geometry::ClusterLodMesh mesh;
  Vector<u32> part_of_cluster;
  Vector<u64> parts;  // (mesh << 32 | material + 1), sorted: the tile's materials
  u64 key = 0;
  u32 triangles = 0;
  bool hit = false;
  bool ok = false;
  std::string error;
};

u64 part_id(u32 mesh, i32 material) noexcept {
  return (u64{mesh} << 32) | static_cast<u32>(material + 1);
}

// The linear part an instance's points go through (its rotation and scale after the mesh's fit)
// and its translation from the tile's corner, in metres, the sum taken in f64.
struct Placement {
  Mat4 linear;
  Vec3 offset;
  Vec3 inverse_scale;
  Quat rotation;
};
Placement placement_of(const FarForestInstance& instance, const FarForestFit& fit,
                       const WorldPos& corner) noexcept {
  Placement out;
  Transform3 t = instance.transform;
  const Vec3 position = t.position;
  t.position = Vec3{};
  out.linear =
      mat4_from_transform(t) * (scaling(Vec3{fit.scale, fit.scale, fit.scale}) *
                                translation(Vec3{-fit.anchor.x, -fit.anchor.y, -fit.anchor.z}));
  out.offset = Vec3{static_cast<f32>(instance.origin.x + static_cast<f64>(position.x) - corner.x),
                    static_cast<f32>(instance.origin.y + static_cast<f64>(position.y) - corner.y),
                    static_cast<f32>(instance.origin.z + static_cast<f64>(position.z) - corner.z)};
  const Vec3 s = instance.transform.scale;
  out.inverse_scale = Vec3{s.x != 0.0f ? 1.0f / s.x : 0.0f, s.y != 0.0f ? 1.0f / s.y : 0.0f,
                           s.z != 0.0f ? 1.0f / s.z : 0.0f};
  out.rotation = instance.transform.rotation;
  return out;
}

f32 largest_scale(const Vec3& s) noexcept {
  return std::max(std::fabs(s.x), std::max(std::fabs(s.y), std::fabs(s.z)));
}

u64 tile_key_of(const TileJob& job) noexcept {
  u64 h = hash_combine(k_far_forest_version, far_tile_key(job.x, job.z));
  u32 bits = 0;
  std::memcpy(&bits, &job.options->tile_size, sizeof(bits));
  h = hash_combine(h, bits);
  for (const u32 i : job.trees) {
    const FarForestInstance& instance = job.instances[i];
    const FarForestFit& fit = job.fits[instance.mesh];
    h = hash_combine(h, job.forms[instance.mesh].hash);
    h = hash_bytes(&fit.scale, sizeof(f32), h);
    h = hash_bytes(&fit.anchor, sizeof(Vec3), h);
    h = hash_bytes(&instance.transform.position, sizeof(Vec3), h);
    h = hash_bytes(&instance.transform.rotation, sizeof(Quat), h);
    h = hash_bytes(&instance.transform.scale, sizeof(Vec3), h);
    h = hash_bytes(&instance.origin, sizeof(WorldPos), h);
  }
  return h;
}

std::string entry_path(const std::string& ddc, u64 key) {
  char name[40];
  std::snprintf(name, sizeof(name), "%016llx.far", static_cast<unsigned long long>(key));
  return io::join_path(io::join_path(ddc, "far_forest"), name);
}

template <typename T>
void put(std::string& out, const T* data, usize count) {
  const u32 n = static_cast<u32>(count);
  out.append(reinterpret_cast<const char*>(&n), sizeof(n));
  if (count > 0) out.append(reinterpret_cast<const char*>(data), count * sizeof(T));
}
template <typename T>
void put_one(std::string& out, const T& value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

struct Reader {
  const std::string& bytes;
  usize at = 0;
  bool ok = true;
  template <typename T>
  void one(T& value) {
    if (!ok || at + sizeof(T) > bytes.size()) {
      ok = false;
      return;
    }
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    at += sizeof(T);
  }
  template <typename T>
  void many(Vector<T>& out) {
    u32 n = 0;
    one(n);
    if (!ok || at + u64{n} * sizeof(T) > bytes.size()) {
      ok = false;
      return;
    }
    out.resize(n);
    if (n > 0) std::memcpy(out.data(), bytes.data() + at, u64{n} * sizeof(T));
    at += u64{n} * sizeof(T);
  }
};

bool read_entry(const std::string& path, u64 key, geometry::ClusterLodMesh& mesh,
                Vector<u32>& part_of_cluster) {
  std::string bytes;
  if (io::read_file(path, bytes) != io::Status::Ok) return false;
  Reader r{bytes};
  u32 magic = 0, version = 0;
  u64 stored = 0;
  r.one(magic);
  r.one(version);
  r.one(stored);
  if (!r.ok || magic != k_entry_magic || version != k_far_forest_version || stored != key)
    return false;
  geometry::ClusterLodMesh m;
  r.many(m.mesh.clusters);
  r.many(m.lod);
  r.many(m.level_cluster_counts);
  r.many(m.mesh.vertices);
  r.many(m.mesh.vertex_source);
  r.many(m.mesh.attributes);
  r.many(m.mesh.triangles);
  r.many(m.mesh.quantized);
  r.one(m.mesh.quant_origin);
  r.one(m.mesh.quant_scale);
  r.one(m.group_count);
  r.one(m.leaf_triangle_count);
  r.one(m.mesh.source_vertex_count);
  r.one(m.mesh.source_triangle_count);
  Vector<u32> parts;
  r.many(parts);
  if (!r.ok || r.at != bytes.size() || parts.size() != m.mesh.clusters.size() ||
      m.lod.size() != m.mesh.clusters.size() || m.mesh.attributes.size() != m.mesh.vertices.size())
    return false;
  mesh = std::move(m);
  part_of_cluster = std::move(parts);
  return true;
}

// One part of a tile — one material of one mesh, over every tree of the tile that has it — as a
// DAG: with a cross level when the mesh's form is a star (an element a tree, the cross held to the
// largest of its trees' measured errors), else one level of clusters that never coarsen, which is
// what the mesh itself draws from that far.
bool build_part(const TileJob& job, u32 mesh_index, i32 material, geometry::ClusterLodMesh& out,
                u32& triangles, std::string& error) {
  const FarForm& form = job.forms[mesh_index];
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  Vector<u32> element;
  Vector<u32> cross_indices;
  Vector<u32> cross_element;
  u32 trees = 0;
  f32 cross_error = 0.0f;
  for (const u32 i : job.trees) {
    const FarForestInstance& instance = job.instances[i];
    if (instance.mesh != mesh_index) continue;
    bool has = false;
    for (const i32 m : form.material)
      has = has || m == material;
    if (!has) continue;
    const Placement p = placement_of(instance, job.fits[mesh_index], job.corner);
    const u32 base = positions.size();
    for (u32 v = 0; v < form.positions.size(); ++v) {
      positions.push_back(transform_point(p.linear, form.positions[v]) + p.offset);
      const Vec3 n = form.normals[v];
      normals.push_back(
          normalize(rotate(p.rotation, Vec3{n.x * p.inverse_scale.x, n.y * p.inverse_scale.y,
                                            n.z * p.inverse_scale.z})));
      uvs.push_back(form.uvs[v]);
    }
    for (u32 t = 0; t < form.material.size(); ++t) {
      if (form.material[t] != material) continue;
      for (u32 k = 0; k < 3; ++k)
        indices.push_back(base + form.indices[t * 3 + k]);
      element.push_back(trees);
      if (form.star && form.cross[t] != 0) {
        for (u32 k = 0; k < 3; ++k)
          cross_indices.push_back(base + form.indices[t * 3 + k]);
        cross_element.push_back(trees);
      }
    }
    cross_error = std::max(cross_error, form.cross_error * job.fits[mesh_index].scale *
                                            largest_scale(instance.transform.scale));
    ++trees;
  }
  triangles = indices.size() / 3;
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(normals.data(), normals.size());
  attributes.uvs = std::span<const Vec2>(uvs.data(), uvs.size());
  geometry::ClusterLodOptions options;
  options.normal_cones = false;  // the planes are two-sided: a cone would cull a seen face
  // Every tree of the part keeps a cross only when every one does (an element of the level is a
  // tree, and the level must hold every element).
  bool cross = form.star && cross_error > 0.0f && !cross_indices.empty();
  if (cross) {
    Vector<u8> has(trees, u8{0});
    for (const u32 e : cross_element)
      has[e] = 1;
    for (const u8 h : has)
      cross = cross && h != 0;
  }
  if (cross) {
    geometry::ClusterLodHierarchyLevel level;
    level.indices = std::span<const u32>(cross_indices.data(), cross_indices.size());
    level.element_of_triangle = std::span<const u32>(cross_element.data(), cross_element.size());
    level.elements = trees;
    level.error = cross_error;
    if (!geometry::build_cluster_lod_hierarchy(
            std::span<const Vec3>(positions.data(), positions.size()),
            std::span<const u32>(indices.data(), indices.size()),
            std::span<const u32>(element.data(), element.size()),
            std::span<const geometry::ClusterLodHierarchyLevel>(&level, 1), options, out, &error,
            attributes)) {
      return false;
    }
    return true;
  }
  geometry::ClusterBuildOptions build;
  build.max_vertices = options.max_vertices;
  build.max_triangles = options.max_triangles & ~3u;
  build.normal_cones = false;
  out = geometry::ClusterLodMesh{};
  if (!geometry::build_clusters(std::span<const Vec3>(positions.data(), positions.size()),
                                std::span<const u32>(indices.data(), indices.size()), build,
                                out.mesh, &error, attributes)) {
    return false;
  }
  for (u32 c = 0; c < out.mesh.clusters.size(); ++c) {
    const geometry::ClusterDesc& cluster = out.mesh.clusters[c];
    geometry::ClusterLodDesc lod;
    lod.own = Vec4{cluster.center.x, cluster.center.y, cluster.center.z, cluster.radius};
    lod.parent = lod.own;
    lod.own_error = 0.0f;
    lod.parent_error = geometry::k_lod_terminal_error;
    lod.level = 0;
    lod.group = c;
    out.lod.push_back(lod);
  }
  out.level_cluster_counts.push_back(out.mesh.clusters.size());
  out.group_count = out.mesh.clusters.size();
  out.leaf_triangle_count = triangles;
  return true;
}

void run_tile(void* data) {
  TileJob& job = *static_cast<TileJob*>(data);
  // The tile's parts, sorted: what the entry's part numbers and the composite materials mean.
  for (const u32 i : job.trees) {
    const FarForestInstance& instance = job.instances[i];
    for (const i32 m : job.forms[instance.mesh].material)
      job.parts.push_back(part_id(instance.mesh, m));
  }
  std::sort(job.parts.begin(), job.parts.end());
  job.parts.erase(std::unique(job.parts.begin(), job.parts.end()), job.parts.end());
  job.key = tile_key_of(job);
  const FarForestOptions& options = *job.options;
  const std::string path =
      options.cache && !options.ddc.empty() ? entry_path(options.ddc, job.key) : std::string();
  if (!path.empty() && read_entry(path, job.key, job.mesh, job.part_of_cluster)) {
    u32 parts_used = 0;
    for (const u32 p : job.part_of_cluster)
      parts_used = std::max(parts_used, p + 1);
    if (parts_used <= job.parts.size()) {
      job.triangles = job.mesh.leaf_triangle_count;
      job.hit = true;
      job.ok = true;
      return;
    }
  }
  Vector<geometry::ClusterLodMesh> built(job.parts.size());
  job.triangles = 0;
  for (u32 p = 0; p < job.parts.size(); ++p) {
    const u32 mesh = static_cast<u32>(job.parts[p] >> 32);
    const i32 material = static_cast<i32>(static_cast<u32>(job.parts[p])) - 1;
    u32 triangles = 0;
    if (!build_part(job, mesh, material, built[p], triangles, job.error)) {
      job.error =
          "far tile (" + std::to_string(job.x) + ", " + std::to_string(job.z) + "): " + job.error;
      return;
    }
    job.triangles += triangles;
  }
  if (built.size() == 1) {
    job.mesh = std::move(built[0]);
    job.part_of_cluster.assign(job.mesh.mesh.clusters.size(), 0u);
  } else if (!geometry::merge_cluster_lod(
                 std::span<const geometry::ClusterLodMesh>(built.data(), built.size()), job.mesh,
                 &job.part_of_cluster, &job.error)) {
    job.error =
        "far tile (" + std::to_string(job.x) + ", " + std::to_string(job.z) + "): " + job.error;
    return;
  }
  job.mesh.leaf_triangle_count = job.triangles;
  geometry::quantize_positions(job.mesh.mesh);  // one grid over the merged parts
  job.ok = true;
  if (path.empty()) return;
  std::string bytes;
  put_one(bytes, k_entry_magic);
  put_one(bytes, k_far_forest_version);
  put_one(bytes, job.key);
  far_tile_bytes(job.mesh,
                 std::span<const u32>(job.part_of_cluster.data(), job.part_of_cluster.size()),
                 bytes);
  // A cache that cannot be written costs the next run a build and nothing else.
  if (io::make_directories(io::parent_path(path)) == io::Status::Ok)
    (void)io::write_file_atomic(path, bytes);
}

// The tile's materials as one source mesh: each part's material copied from its mesh with the
// images it samples, which keep their built textures' records and none of their bytes — the
// species' own mesh uploaded those textures, and the GPU scene finds them again by key
// (renderer.md, "One upload per distinct image"). With no derived-data cache there are no records
// to find them by, and the bytes are copied.
void make_source(const TileJob& job, std::span<const SourceMesh> sources, SourceMesh& out) {
  out = SourceMesh{};
  out.cache = "none";
  for (u32 p = 0; p < job.parts.size(); ++p) {
    const u32 mesh = static_cast<u32>(job.parts[p] >> 32);
    const i32 material = static_cast<i32>(static_cast<u32>(job.parts[p])) - 1;
    out.part_material.push_back(material < 0 ? -1 : static_cast<i32>(out.data.materials.size()));
    if (material < 0) continue;
    const SourceMesh& source = sources[mesh];
    if (out.texture_ddc.empty()) out.texture_ddc = source.texture_ddc;
    assets::Material copy = source.data.materials[static_cast<u32>(material)];
    const bool records = source.textures.size() == source.data.images.size();
    for (i32* image : {&copy.base_color_image, &copy.metallic_roughness_image, &copy.normal_image,
                       &copy.occlusion_image, &copy.emissive_image}) {
      if (*image < 0 || static_cast<u32>(*image) >= source.data.images.size()) {
        *image = -1;
        continue;
      }
      const u32 original = static_cast<u32>(*image);
      const assets::ImageRef& ref = source.data.images[original];
      assets::ImageRef carried;
      carried.name = ref.name;
      carried.mime_type = ref.mime_type;
      // A path is relative to its own mesh's directory, which the composite does not have.
      carried.uri = ref.uri.empty() || source.image_dir.empty() || io::is_absolute_path(ref.uri)
                        ? ref.uri
                        : io::join_path(source.image_dir, ref.uri);
      if (!records || source.texture_ddc.empty()) carried.bytes = ref.bytes;
      *image = static_cast<i32>(out.data.images.size());
      out.data.images.push_back(std::move(carried));
      out.textures.push_back(records ? source.textures[original] : geometry::ClusterFileTexture{});
    }
    out.data.materials.push_back(std::move(copy));
  }
}

}  // namespace

void far_forest_tunables(FarForestOptions& out) noexcept {
  out.min_height_m = static_cast<f32>(far_min_height.get());
  out.max_triangles = static_cast<u32>(far_max_triangles.get());
}

void far_tile_bytes(const geometry::ClusterLodMesh& mesh, std::span<const u32> part_of_cluster,
                    std::string& out) {
  put(out, mesh.mesh.clusters.data(), mesh.mesh.clusters.size());
  put(out, mesh.lod.data(), mesh.lod.size());
  put(out, mesh.level_cluster_counts.data(), mesh.level_cluster_counts.size());
  put(out, mesh.mesh.vertices.data(), mesh.mesh.vertices.size());
  put(out, mesh.mesh.vertex_source.data(), mesh.mesh.vertex_source.size());
  put(out, mesh.mesh.attributes.data(), mesh.mesh.attributes.size());
  put(out, mesh.mesh.triangles.data(), mesh.mesh.triangles.size());
  put(out, mesh.mesh.quantized.data(), mesh.mesh.quantized.size());
  put_one(out, mesh.mesh.quant_origin);
  put_one(out, mesh.mesh.quant_scale);
  put_one(out, mesh.group_count);
  put_one(out, mesh.leaf_triangle_count);
  put_one(out, mesh.mesh.source_vertex_count);
  put_one(out, mesh.mesh.source_triangle_count);
  put(out, part_of_cluster.data(), part_of_cluster.size());
}

bool build_far_forest(std::span<const FarForestInstance> instances,
                      std::span<const FarForestFit> fits, const FarForestOptions& options,
                      FarForestScene scene, FarForestData& out, std::string& error) {
  const i64 started = time::monotonic_ns();
  out = FarForestData{};
  out.tile_size = options.tile_size;
  if (instances.empty() || !(options.tile_size > 0.0f)) return true;
  // Each mesh's coarsest form, once, and whether its instances may take it.
  const u32 mesh_count = scene.dags.size();
  Vector<FarForm> forms(mesh_count);
  Vector<u8> made(mesh_count, u8{0});
  Vector<u8> textured(mesh_count, u8{1});
  struct Taken {
    u64 key = 0;
    u32 instance = 0;
  };
  Vector<Taken> taken;
  for (u32 i = 0; i < instances.size(); ++i) {
    const FarForestInstance& instance = instances[i];
    const u32 m = instance.mesh;
    if (m >= mesh_count || m >= fits.size() || m >= scene.sources.size()) continue;
    if (made[m] == 0) {
      made[m] = 1;
      make_form(scene.dags[m], scene.sources[m], forms[m]);
      // The built textures a composite needs (`make_source`): every image the form's materials
      // sample has a record, or the load reads no cache and the bytes go along.
      const SourceMesh& source = scene.sources[m];
      if (!source.texture_ddc.empty() && source.textures.size() != source.data.images.size()) {
        for (const i32 material : forms[m].material) {
          if (material >= 0) textured[m] = 0;
        }
      }
    }
    const FarForm& form = forms[m];
    if (!form.usable) continue;
    const f32 height = form.height * fits[m].scale * largest_scale(instance.transform.scale);
    if (height < options.min_height_m) {
      ++out.stats.skipped_small;
      continue;
    }
    if (form.indices.size() / 3 > options.max_triangles) {
      ++out.stats.skipped_heavy;
      continue;
    }
    if (textured[m] == 0) {
      ++out.stats.skipped_textures;
      continue;
    }
    const f64 x = instance.origin.x + static_cast<f64>(instance.transform.position.x);
    const f64 z = instance.origin.z + static_cast<f64>(instance.transform.position.z);
    const f64 tile = static_cast<f64>(options.tile_size);
    const i32 tx = static_cast<i32>(std::floor(x / tile));
    const i32 tz = static_cast<i32>(std::floor(z / tile));
    taken.push_back(Taken{far_tile_key(tx, tz), i});
  }
  if (taken.empty()) return true;
  // By tile, in the scene's order within one: a tile's mesh is a function of its trees in order.
  std::stable_sort(taken.begin(), taken.end(),
                   [](const Taken& a, const Taken& b) { return a.key < b.key; });
  Vector<TileJob> jobs;
  for (u32 i = 0; i < taken.size();) {
    TileJob job;
    const u64 key = taken[i].key;
    job.x = static_cast<i32>(static_cast<u32>(key >> 32) ^ 0x80000000u);
    job.z = static_cast<i32>(static_cast<u32>(key) ^ 0x80000000u);
    const f64 tile = static_cast<f64>(options.tile_size);
    job.corner = WorldPos{static_cast<f64>(job.x) * tile, 0.0, static_cast<f64>(job.z) * tile};
    job.instances = instances.data();
    job.fits = fits.data();
    job.forms = forms.data();
    job.options = &options;
    for (; i < taken.size() && taken[i].key == key; ++i)
      job.trees.push_back(taken[i].instance);
    jobs.push_back(std::move(job));
  }
  if (options.pool != nullptr && jobs.size() > 1) {
    Vector<jobs::Job> list;
    list.reserve(jobs.size());
    jobs::Counter counter;
    for (TileJob& job : jobs)
      list.push_back(jobs::Job{run_tile, &job, &counter});
    options.pool->schedule(jobs::Pool::Performance,
                           std::span<const jobs::Job>(list.data(), list.size()), counter);
    options.pool->wait(counter);
  } else {
    for (TileJob& job : jobs)
      run_tile(&job);
  }
  for (TileJob& job : jobs) {
    if (!job.ok) {
      error = job.error;
      return false;
    }
  }
  // Appended in tile order: a mesh, its description and its source, and its instance at its corner.
  Vector<u8> used(mesh_count, u8{0});
  for (TileJob& job : jobs) {
    FarForestTile tile;
    tile.key = far_tile_key(job.x, job.z);
    tile.x = job.x;
    tile.z = job.z;
    tile.mesh = scene.dags.size();
    tile.instance = scene.prefix.size();
    tile.trees = job.trees.size();
    tile.triangles = job.triangles;
    tile.corner = job.corner;
    SourceMesh source;
    make_source(job, std::span<const SourceMesh>(scene.sources.data(), scene.sources.size()),
                source);
    source.part_of_cluster = std::move(job.part_of_cluster);
    source.path = "far forest (" + std::to_string(job.x) + ", " + std::to_string(job.z) + ")";
    SceneMeshInfo info;
    info.name = source.path;
    info.origin = "far forest";
    out.stats.clusters += job.mesh.mesh.clusters.size();
    out.stats.triangles += job.triangles;
    out.stats.trees += tile.trees;
    out.stats.cache_hits += job.hit ? 1u : 0u;
    out.stats.cache_misses += job.hit ? 0u : 1u;
    for (const u32 i : job.trees)
      used[instances[i].mesh] = 1;
    scene.meshes.push_back(source.path);
    scene.mesh_info.push_back(std::move(info));
    scene.sources.push_back(std::move(source));
    scene.dags.push_back(std::move(job.mesh));
    if (!scene.form_source.empty()) {
      scene.form_source.push_back(tile.mesh);
      scene.form_rigid.push_back(1);
    }
    SceneInstance instance;
    instance.mesh = tile.mesh;
    instance.origin = tile.corner;
    scene.prefix.push_back(instance);
    out.tiles.push_back(tile);
  }
  for (const u8 u : used)
    out.stats.meshes += u;
  out.stats.tiles = out.tiles.size();
  out.stats.build_ms = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  ENGINE_LOG_INFO(
      log_renderer, "far forest built", log::field("tiles", out.stats.tiles),
      log::field("trees", out.stats.trees), log::field("meshes", out.stats.meshes),
      log::field("triangles", out.stats.triangles), log::field("clusters", out.stats.clusters),
      log::field("skipped_small", out.stats.skipped_small),
      log::field("skipped_heavy", out.stats.skipped_heavy),
      log::field("skipped_textures", out.stats.skipped_textures),
      log::field("cache_hits", out.stats.cache_hits),
      log::field("cache_misses", out.stats.cache_misses), log::field("ms", out.stats.build_ms));
  return true;
}

// ---- the hand-off ------------------------------------------------------------------------------

SceneInstance far_tile_placement(const FarForestTile& tile, bool drawn) noexcept {
  SceneInstance out;
  out.mesh = tile.mesh;
  out.origin = drawn ? tile.corner : WorldPos{tile.corner.x, k_far_park_depth_m, tile.corner.z};
  return out;
}

void FarForestHandOff::reset(const FarForestData* data) {
  data_ = data;
  const u32 n = data != nullptr ? data->tiles.size() : 0u;
  drawn_.assign(n, u8{1});
  wanted_.assign(n, u8{1});
  indices_.clear();
  indices_.reserve(n);
  placements_.clear();
  placements_.reserve(n);
  drawn_count_ = n;
}

u32 FarForestHandOff::hand_over(std::span<const DynamicBlock> blocks) {
  indices_.clear();
  placements_.clear();
  if (!active()) return 0;
  const Vector<FarForestTile>& tiles = data_->tiles;
  for (u32 t = 0; t < tiles.size(); ++t)
    wanted_[t] = 1;
  for (const DynamicBlock& block : blocks) {
    if (block.count == 0) continue;
    // The tiles are sorted by key: a block names at most one.
    u32 lo = 0, hi = tiles.size();
    while (lo < hi) {
      const u32 mid = lo + (hi - lo) / 2;
      if (tiles[mid].key < block.key)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < tiles.size() && tiles[lo].key == block.key) wanted_[lo] = 0;
  }
  for (u32 t = 0; t < tiles.size(); ++t) {
    if (wanted_[t] == drawn_[t]) continue;
    indices_.push_back(tiles[t].instance);
    placements_.push_back(far_tile_placement(tiles[t], wanted_[t] != 0));
  }
  return indices_.size();
}

void FarForestHandOff::commit() noexcept {
  drawn_count_ = 0;
  for (u32 t = 0; t < drawn_.size(); ++t) {
    drawn_[t] = wanted_[t];
    drawn_count_ += drawn_[t];
  }
}

}  // namespace engine::renderer
