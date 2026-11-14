// The far tile (far_tile.h; renderer.md, "The far tile"; G198).
#include <systems/renderer/far_tile.h>

#include <core/hash/hash.h>
#include <core/time/time.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/far_form.h>
#include <domain/geometry/material_sampling.h>
#include <domain/scene_gen/far_tile.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <renderer_log.h>

namespace engine::renderer {

// scene.cpp's: a tile mesh's textures, off the frame (texture.md, "Textures that arrive after the
// load"), the load's own pass run over one mesh.
void load_placement_textures(const SceneDesc& desc, const PlacementMeshOptions& options,
                             PlacementMeshData& out);

namespace {

u64 material_key(const assets::Material& m, const i32 images[5]) {
  const geometry::ClusterFileMaterialSampling sampling =
      geometry::encode_material_sampling(m.sampling, m.occlusion_strength);
  u64 h = hash_bytes(&sampling, sizeof(sampling));
  const f32 words[12] = {m.base_color.x, m.base_color.y, m.base_color.z, m.base_color.w,
                         m.emissive.x,   m.emissive.y,   m.emissive.z,   m.metallic,
                         m.roughness,    m.normal_scale, m.alpha_cutoff, m.occlusion_strength};
  h = hash_combine(h, hash_bytes(words, sizeof(words)));
  h = hash_combine(h, hash_bytes(images, 5 * sizeof(i32)));
  const u32 flags = static_cast<u32>(m.alpha_mode) | (m.double_sided ? 0x100u : 0u);
  return hash_combine(h, flags);
}

// One distinct material of the far tile: its record, and its triangles over the tile's vertices.
struct Part {
  u64 key = 0;
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
};

}  // namespace

bool load_far_tile(const scene_gen::PlacementMesh& mesh, const PlacementMeshOptions& options,
                   PlacementMeshData& out, std::string& error, FarTileStats* stats) {
  out = PlacementMeshData{};
  FarTileStats local;
  FarTileStats& s = stats != nullptr ? *stats : local;
  s = FarTileStats{};
  scene_gen::FarTile tile;
  if (!scene_gen::read_far_tile(mesh.recipe, tile, &error)) {
    error = mesh.path + ": " + error;
    return false;
  }
  const i64 start_ns = time::monotonic_ns();
  // Every member as a near tile builds it, with no textures: the far tile's own pass reads them.
  PlacementMeshOptions member_options = options;
  member_options.textures = false;
  s.members = tile.meshes.size();
  s.placements = tile.placements.size();
  Vector<PlacementMeshData> members(tile.meshes.size());
  Vector<geometry::FarForm> forms(tile.meshes.size());
  for (u32 k = 0; k < tile.meshes.size(); ++k) {
    const scene_gen::PlacementMesh& member = tile.meshes[k];
    if (scene_gen::is_far_tile(member)) {
      error = mesh.path + ": a far tile names a far tile";
      return false;
    }
    std::string why;
    if (!load_placement_mesh(member, member_options, members[k], why)) {
      error = mesh.path + ": its member " + member.name + ": " + why;
      return false;
    }
    // The far form the member's container carried, or the same bake from its finest level: the
    // material it names is the member's part.
    forms[k] = std::move(members[k].source.far_form);
    if (forms[k].empty()) {
      const Vector<u32>& parts = members[k].source.part_of_cluster;
      const std::span<const u32> material =
          parts.size() == members[k].lod.mesh.clusters.size()
              ? std::span<const u32>(parts.data(), parts.size())
              : std::span<const u32>();
      if (!geometry::bake_far_form(members[k].lod, material, forms[k], &why)) {
        error = mesh.path + ": its member " + member.name + ": " + why;
        return false;
      }
      ++s.baked;
    }
  }
  const i64 members_ns = time::monotonic_ns();
  s.members_ms = static_cast<f64>(members_ns - start_ns) / 1.0e6;

  // The far tile's materials and images: each distinct one once, in the order the placements
  // first name them, an image's relative path made absolute against its member's directory.
  SourceMesh& source = out.source;
  Vector<u64> image_keys;
  bool textures_whole = true;
  // Per member: its parts' far tile part (~0u: not drawn), its images' far tile images.
  Vector<Vector<u32>> part_of(tile.meshes.size());
  Vector<Part> parts;
  auto image_of = [&](u32 k, i32 image) -> i32 {
    const SourceMesh& m = members[k].source;
    if (image < 0 || static_cast<u32>(image) >= m.data.images.size()) return -1;
    assets::ImageRef ref = m.data.images[static_cast<u32>(image)];
    if (!ref.uri.empty() && !std::filesystem::path(ref.uri).is_absolute() && !m.image_dir.empty())
      ref.uri = io::join_path(m.image_dir, ref.uri);
    const u64 key = hash_combine(hash_bytes(ref.uri.data(), ref.uri.size()),
                                 hash_bytes(ref.bytes.data(), ref.bytes.size()));
    for (u32 i = 0; i < image_keys.size(); ++i)
      if (image_keys[i] == key) return static_cast<i32>(i);
    image_keys.push_back(key);
    source.data.images.push_back(std::move(ref));
    if (m.textures.size() == m.data.images.size())
      source.textures.push_back(m.textures[static_cast<u32>(image)]);
    else
      textures_whole = false;
    return static_cast<i32>(source.data.images.size() - 1);
  };
  auto part_for = [&](u32 k, u32 member_part) -> u32 {
    Vector<u32>& map = part_of[k];
    if (map.empty()) map.assign(members[k].source.part_material.size() + 1, ~0u);
    if (member_part >= map.size()) return ~0u;
    if (map[member_part] != ~0u) return map[member_part];
    const SourceMesh& m = members[k].source;
    const i32 index = member_part < m.part_material.size() ? m.part_material[member_part] : -1;
    assets::Material material;
    u32 flags = 0;
    if (index >= 0 && static_cast<u32>(index) < m.data.materials.size()) {
      material = m.data.materials[static_cast<u32>(index)];
      if (static_cast<u32>(index) < m.material_sampling_flags.size())
        flags = m.material_sampling_flags[static_cast<u32>(index)];
    }
    i32 images[5] = {image_of(k, material.base_color_image),
                     image_of(k, material.metallic_roughness_image),
                     image_of(k, material.normal_image), image_of(k, material.occlusion_image),
                     image_of(k, material.emissive_image)};
    material.base_color_image = images[0];
    material.metallic_roughness_image = images[1];
    material.normal_image = images[2];
    material.occlusion_image = images[3];
    material.emissive_image = images[4];
    const u64 key = hash_combine(material_key(material, images), flags);
    for (u32 p = 0; p < parts.size(); ++p) {
      if (parts[p].key == key) return map[member_part] = p;
    }
    Part part;
    part.key = key;
    parts.push_back(std::move(part));
    source.data.materials.push_back(std::move(material));
    source.material_sampling_flags.push_back(flags);
    return map[member_part] = parts.size() - 1;
  };

  // Every placement's far form, turned, scaled and moved to its place from the tile's origin.
  for (const scene_gen::FarTilePlacement& p : tile.placements) {
    const geometry::FarForm& form = forms[p.mesh];
    const u32 level = std::min(tile.level, static_cast<u32>(form.levels.size() - 1));
    const geometry::FarFormLevel& record = form.levels[level];
    const Vec3 offset{static_cast<f32>(static_cast<f64>(p.x_mm) / 1000.0),
                      static_cast<f32>(static_cast<f64>(p.y_mm) / 1000.0),
                      static_cast<f32>(static_cast<f64>(p.z_mm) / 1000.0)};
    const f32 scale_max = std::max(std::fabs(p.scale.x), std::max(std::fabs(p.scale.y),
                                                                   std::fabs(p.scale.z)));
    s.error_m = std::max(s.error_m, geometry::FarForm::level_error(record) * scale_max);
    s.source_triangles += form.header.source_triangles;
    for (u32 q = record.first_part; q < record.first_part + record.part_count; ++q) {
      const geometry::FarFormPart& fp = form.parts[q];
      const u32 to = part_for(p.mesh, fp.material);
      if (to == ~0u) continue;
      Part& part = parts[to];
      // The part's own copy of each vertex this placement's triangles name.
      Vector<u32> local(form.vertices.size(), ~0u);
      for (u32 i = fp.first_index; i < fp.first_index + fp.index_count; ++i) {
        const u32 v = form.indices[i];
        if (local[v] == ~0u) {
          local[v] = part.positions.size();
          const geometry::FarFormVertex& fv = form.vertices[v];
          const Vec3 scaled{fv.position.x * p.scale.x, fv.position.y * p.scale.y,
                            fv.position.z * p.scale.z};
          part.positions.push_back(rotate(p.rotation, scaled) + offset);
          const Vec3 n = geometry::decode_normal_oct(fv.normal_oct);
          const Vec3 bent{n.x / p.scale.x, n.y / p.scale.y, n.z / p.scale.z};
          part.normals.push_back(normalize(rotate(p.rotation, bent)));
          part.uvs.push_back(geometry::decode_half2(fv.uv_half2));
        }
        part.indices.push_back(local[v]);
      }
    }
  }
  if (!textures_whole) source.textures.clear();

  // One DAG a part, merged: the far tile's clusters, one material each.
  Vector<geometry::ClusterLodMesh> dags;
  dags.reserve(parts.size());
  for (Part& part : parts) {
    if (part.indices.empty()) continue;
    geometry::AttributeSource attributes;
    attributes.normals = std::span<const Vec3>(part.normals.data(), part.normals.size());
    attributes.uvs = std::span<const Vec2>(part.uvs.data(), part.uvs.size());
    geometry::ClusterLodMesh dag;
    if (!geometry::build_cluster_lod(
            std::span<const Vec3>(part.positions.data(), part.positions.size()),
            std::span<const u32>(part.indices.data(), part.indices.size()), options.lod, dag,
            &error, attributes)) {
      error = mesh.path + ": " + error;
      return false;
    }
    s.triangles += dag.leaf_triangle_count;
    dags.push_back(std::move(dag));
  }
  if (dags.empty()) {
    error = mesh.path + ": its members' far forms draw nothing";
    return false;
  }
  geometry::ClusterLodMesh merged;
  Vector<u32> part_of_cluster;
  if (!geometry::merge_cluster_lod(std::span<const geometry::ClusterLodMesh>(dags.data(), dags.size()),
                                   merged, &part_of_cluster, &error)) {
    error = mesh.path + ": " + error;
    return false;
  }
  // The parts with triangles, in order, are the DAGs merged; their materials are the parts'.
  {
    u32 drawn = 0;
    for (u32 p = 0; p < parts.size(); ++p) {
      if (!parts[p].indices.empty()) source.part_material.push_back(static_cast<i32>(p));
      drawn += parts[p].indices.empty() ? 0u : 1u;
    }
    s.materials = drawn;
  }
  source.part_of_cluster = std::move(part_of_cluster);
  // As `load_placement_mesh` lays out a tile's mesh: leaves first, its streams with it.
  Vector<geometry::ClusterLodMesh> one(1);
  one[0] = std::move(merged);
  Vector<geometry::ClusterMeshPart> layout;
  Vector<u32> source_of_cluster;
  if (!geometry::merge_cluster_meshes(
          std::span<const geometry::ClusterLodMesh>(one.data(), one.size()), out.lod, layout,
          &error, geometry::ClusterOrder::leaves_first, &source_of_cluster)) {
    error = mesh.path + ": " + error;
    return false;
  }
  out.part = layout[0];
  geometry::permute_cluster_array(
      std::span<const u32>(source_of_cluster.data() + out.part.first_cluster,
                           out.part.cluster_count),
      source.part_of_cluster);
  source.path = mesh.path;
  source.cache = "miss";
  out.reach = ground_radius(out.lod, out.part.first_cluster, out.part.cluster_count);
  s.clusters = out.lod.mesh.clusters.size();
  s.build_ms = static_cast<f64>(time::monotonic_ns() - members_ns) / 1.0e6;
  if (options.textures) {
    SceneDesc desc;
    desc.lod = options.lod;
    desc.ddc = options.ddc;
    desc.cache = options.cache && !options.ddc.empty();
    desc.materials = options.materials;
    source.texture_ddc = desc.cache ? desc.ddc : std::string();
    load_placement_textures(desc, options, out);
  }
  ENGINE_LOG_INFO(log_renderer, "far tile built", log::field("path", mesh.path),
                  log::field("members", s.members), log::field("placements", s.placements),
                  log::field("baked", s.baked), log::field("triangles", s.triangles),
                  log::field("source_triangles", s.source_triangles),
                  log::field("clusters", s.clusters), log::field("error_m", s.error_m),
                  log::field("members_ms", s.members_ms), log::field("build_ms", s.build_ms));
  return true;
}

}  // namespace engine::renderer
