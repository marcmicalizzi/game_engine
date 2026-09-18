#include <core/json/json.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <algorithm>
#include <cmath>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

// An (n x n) heightfield over [-extent, extent]^2 in XZ, dunes-and-ridges in Y. The scene that
// needs no content at all: it is what a bring-up on a new machine draws, and what the
// renderer's own tests fall back to when there is no fixture.
void make_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.reserve(n * n);
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f) +
                    0.12f * std::cos(fx * 4.3f - fz * 3.7f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  indices.reserve((n - 1) * (n - 1) * 6);
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

// Writes a mesh that was just imported and clustered into the derived-data cache, byte for byte
// the container `engine-content build` writes from the same source: the DAG, the materials as
// GPU records, a material index per cluster (a primitive that names none gets one appended
// default, shared), the image paths as the glTF gave them, and the source path they are
// relative to, plus the streaming page table the clusters were laid out in. `lod` and `pages`
// are moved into the container and back out again, so neither is ever copied. A cache that
// cannot be written is a warning and nothing more: the picture does not depend on it.
void write_cluster_cache(const std::string& path, const std::string& source,
                         const assets::MeshData& mesh_data, const Vector<i32>& part_material,
                         const Vector<u32>& part_of_cluster, geometry::ClusterLodMesh& lod,
                         geometry::ClusterPages& pages) {
  geometry::ClusterFileData data;
  data.mesh = std::move(lod);
  data.pages = std::move(pages);
  data.source_path = source;
  data.materials.reserve(mesh_data.materials.size() + 1);
  for (const assets::Material& source_material : mesh_data.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = source_material.base_color;
    material.metallic = source_material.metallic;
    material.roughness = source_material.roughness;
    material.base_color_image = source_material.base_color_image;
    material.normal_image = source_material.normal_image;
    material.metallic_roughness_image =
        geometry::encode_optional_image(source_material.metallic_roughness_image);
    material.occlusion_image = geometry::encode_optional_image(source_material.occlusion_image);
    material.emissive_image = geometry::encode_optional_image(source_material.emissive_image);
    material.emissive = source_material.emissive;
    material.normal_scale = source_material.normal_scale;
    material.alpha = geometry::encode_alpha_word(
        source_material.alpha_mode, source_material.double_sided, source_material.alpha_cutoff);
    data.materials.push_back(material);
  }
  constexpr u32 k_no_default = ~u32{0};
  u32 default_material = k_no_default;
  data.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster) {
    const i32 index = part_material[part];
    if (index >= 0 && static_cast<u32>(index) < mesh_data.materials.size()) {
      data.cluster_material.push_back(static_cast<u32>(index));
      continue;
    }
    if (default_material == k_no_default) {
      default_material = data.materials.size();
      data.materials.push_back(geometry::ClusterFileMaterial{});
    }
    data.cluster_material.push_back(default_material);
  }
  // The image paths as the glTF gave them, and beside them the bytes of the images it embedded,
  // so that what this container draws is what the glTF draws. Writing only the (empty) paths is
  // what made a cache hit a different picture from a cache miss ([geometry](geometry.md)).
  data.image_paths.reserve(mesh_data.images.size());
  data.images.reserve(mesh_data.images.size());
  for (const assets::ImageRef& image : mesh_data.images) {
    data.image_paths.push_back(image.uri);
    geometry::ClusterImage carried;
    carried.mime_type = image.mime_type;
    carried.bytes = image.bytes;
    data.images.push_back(std::move(carried));
  }

  std::string error;
  const io::Status status = io::make_directories(io::parent_path(path));
  if (status != io::Status::Ok) {
    error = std::string("cannot create the directory: ") + io::status_name(status);
  } else if (geometry::write_cluster_file(path, data, &error)) {
    ENGINE_LOG_INFO(log_renderer, "cluster cache written", log::field("path", path),
                    log::field("clusters", data.mesh.mesh.clusters.size()));
  }
  if (!error.empty()) {
    ENGINE_LOG_WARN(log_renderer, "cluster cache not written", log::field("path", path),
                    log::field("error", error));
  }
  lod = std::move(data.mesh);
  pages = std::move(data.pages);
}

// Reads one mesh: a `.clusters` container named outright, the derived-data cache entry this
// glTF and these options address, or the glTF itself (imported, welded, clustered per primitive
// so every cluster has a single material, merged, and written into the cache for the next run).
// An empty `path` builds the procedural heightfield instead.
bool load_source_mesh(const std::string& path, const SceneDesc& desc, SourceMesh& out,
                      geometry::ClusterLodMesh& lod, std::string& error) {
  if (path.empty()) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_terrain(desc.heightfield_grid, 10.0f, positions, indices);
    Vector<Vec2> uvs;
    uvs.reserve(positions.size());
    for (const Vec3& p : positions)
      uvs.push_back(Vec2{(p.x + 10.0f) / 20.0f, (p.z + 10.0f) / 20.0f});
    geometry::AttributeSource attribute_source;
    attribute_source.uvs = std::span<const Vec2>(uvs.data(), uvs.size());  // normals: computed
    return geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod,
                                       &error, attribute_source);
  }
  // Where a built mesh may already be: a container named outright, or the cache entry this
  // source and these options address.
  std::string container;
  std::string cache_path;
  if (io::extension(path) == ".clusters") {
    container = path;
    out.cache = "file";
  } else if (desc.cache) {
    u64 source_hash = 0;
    if (!assets::source_mesh_hash(path, source_hash, &error)) return false;
    // The page target is part of the key, so this has to be the one the build below uses — and
    // the one engine-content uses by default, or the two apps would stop sharing entries.
    const u64 key = geometry::cluster_cache_key(source_hash, geometry::ClusterLodOptions{}, true,
                                                geometry::ClusterPagesOptions{}.page_bytes);
    cache_path = geometry::cluster_cache_path(desc.ddc, key);
    out.cache = "miss";
    if (io::exists(cache_path)) container = cache_path;
  }
  geometry::ClusterFileData container_data;
  bool from_container = false;
  if (!container.empty()) {
    std::string read_error;
    if (geometry::read_cluster_file(container, container_data, &read_error)) {
      from_container = true;
      if (!cache_path.empty()) out.cache = "hit";
    } else if (cache_path.empty()) {
      error = read_error;  // a file named on the command line must load
      return false;
    } else {
      // A cache entry this build cannot read is not an error; it is a miss with a warning.
      ENGINE_LOG_WARN(log_renderer, "cluster cache entry ignored", log::field("path", container),
                      log::field("error", read_error));
    }
  }
  if (from_container) {
    lod = std::move(container_data.mesh);
    // The container's materials, images, and material map, in the shape the glTF path leaves
    // behind so that everything below is the same code for both: one part per material, and the
    // file's own map from cluster to part.
    out.data.materials.reserve(container_data.materials.size());
    out.part_material.reserve(container_data.materials.size());
    for (const geometry::ClusterFileMaterial& source : container_data.materials) {
      assets::Material material;
      material.base_color = source.base_color;
      material.metallic = source.metallic;
      material.roughness = source.roughness;
      material.base_color_image = source.base_color_image;
      material.normal_image = source.normal_image;
      material.metallic_roughness_image =
          geometry::decode_optional_image(source.metallic_roughness_image);
      material.occlusion_image = geometry::decode_optional_image(source.occlusion_image);
      material.emissive_image = geometry::decode_optional_image(source.emissive_image);
      material.emissive = source.emissive;
      material.normal_scale = source.normal_scale;
      material.alpha_mode = geometry::alpha_word_mode(source.alpha);
      material.double_sided = geometry::alpha_word_double_sided(source.alpha);
      material.alpha_cutoff = geometry::alpha_word_cutoff(source.alpha);
      out.data.materials.push_back(std::move(material));
      out.part_material.push_back(static_cast<i32>(out.part_material.size()));
    }
    // The images: a path names a file beside the source mesh, and an image the source embedded
    // travels inside the container, so everything below is the same code for both and a container
    // draws what its source draws. A container written before the bytes were carried has the
    // paths alone, and an embedded image of one still draws untextured with a warning.
    out.data.images.reserve(container_data.image_paths.size());
    for (u32 i = 0; i < container_data.image_paths.size(); ++i) {
      assets::ImageRef image;
      image.uri = container_data.image_paths[i];
      if (i < container_data.images.size()) {
        image.mime_type = std::move(container_data.images[i].mime_type);
        image.bytes = std::move(container_data.images[i].bytes);
      }
      out.data.images.push_back(std::move(image));
    }
    out.part_of_cluster = std::move(container_data.cluster_material);
    if (out.part_of_cluster.empty()) {  // a container with no material map: one default for all
      out.part_of_cluster = Vector<u32>(lod.mesh.clusters.size(), out.part_material.size());
      out.part_material.push_back(-1);
    }
    // Relative image paths belong to the mesh the container was built from; a container that
    // does not name one resolves them beside itself.
    out.image_dir = std::string(io::parent_path(
        container_data.source_path.empty() ? std::string_view(container)
                                           : std::string_view(container_data.source_path)));
    out.primitives = 0;  // a container does not record how many were merged into it
    ENGINE_LOG_INFO(
        log_renderer, "mesh loaded", log::field("path", container),
        log::field("from", "cluster file"), log::field("source", container_data.source_path),
        log::field("cache", out.cache), log::field("clusters", lod.mesh.clusters.size()),
        log::field("vertices", lod.mesh.vertices.size()),
        log::field("triangles", lod.leaf_triangle_count),
        log::field("lod_levels", lod.level_cluster_counts.size()),
        log::field("materials", out.data.materials.size()),
        log::field("images", out.data.images.size()));
    return true;
  }
  if (!assets::load_gltf(path, out.data, &error)) return false;
  out.image_dir = std::string(io::parent_path(path));
  // Exporters duplicate vertices freely; welding the identical ones gives the cluster builder
  // shared vertices to fill clusters with and the LOD builder edges to collapse. **The skin
  // bindings are part of the weld key**, so two duplicates that agree on position, normal and UV
  // but disagree on weights stay two vertices; merging them would silently hand one surface the
  // other's deformation, and the weld runs before clustering, so there would be no later point
  // at which the loss could be noticed ([geometry](geometry.md), "Skinned meshes").
  const u32 loaded_vertices = out.data.positions.size();
  const u32 welded_vertices =
      geometry::weld_vertices(out.data.positions, out.data.normals, out.data.uvs,
                              std::span<u32>(out.data.indices.data(), out.data.indices.size()),
                              out.data.skin_bindings.empty() ? nullptr : &out.data.skin_bindings);
  ENGINE_LOG_INFO(
      log_renderer, "mesh loaded", log::field("path", path), log::field("from", "gltf"),
      log::field("cache", out.cache), log::field("vertices", loaded_vertices),
      log::field("welded", welded_vertices), log::field("triangles", out.data.indices.size() / 3),
      log::field("primitives", out.data.primitives.size()),
      log::field("materials", out.data.materials.size()),
      log::field("skins", out.data.skins.size()), log::field("images", out.data.images.size()));
  const geometry::AttributeSource attribute_source = assets::attribute_source(out.data);
  Vector<geometry::ClusterLodMesh> parts;
  for (const assets::Primitive& primitive : out.data.primitives) {
    if (primitive.index_count < 3) continue;
    const std::span<const u32> range(out.data.indices.data() + primitive.first_index,
                                     primitive.index_count);
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(out.data.positions, range, geometry::ClusterLodOptions{}, part,
                                     &error, attribute_source)) {
      return false;
    }
    parts.push_back(std::move(part));
    out.part_material.push_back(primitive.material);
  }
  if (!geometry::merge_cluster_lod(parts, lod, &out.part_of_cluster, &error)) return false;
  out.primitives = parts.size();
  // Laid out in streaming pages, exactly as engine-content lays it out, because the page target
  // is part of the cache key and the entry either app writes has to be the same container. The
  // layout renumbers the clusters, so the map from cluster to primitive comes along.
  geometry::ClusterPages pages;
  Vector<u32> source_of_cluster;
  std::string page_error;
  if (geometry::build_cluster_pages(lod, geometry::ClusterPagesOptions{}, pages, &page_error,
                                    &source_of_cluster)) {
    geometry::permute_cluster_array(source_of_cluster, out.part_of_cluster);
  } else {
    ENGINE_LOG_WARN(log_renderer, "cluster pages not built", log::field("path", path),
                    log::field("error", page_error));
  }
  // Into the cache for the next run, as the same container engine-content build writes: either
  // app fills the cache, either app finds it.
  if (!cache_path.empty()) {
    write_cluster_cache(cache_path, path, out.data, out.part_material, out.part_of_cluster, lod,
                        pages);
  }
  return true;
}

// n x n copies of mesh 0 on a grid, with a rotation and a scale that vary per copy and a
// non-uniform scale on every third, so one flag exercises the uniform and the non-uniform path,
// the cone test's precondition, and the LOD scaling all at once.
void make_instance_grid(u32 n, f32 mesh_radius, u32 joints, f32 bounds_padding,
                        Vector<SceneInstance>& out) {
  const f32 spacing = 3.0f * mesh_radius;
  const f32 center = 0.5f * static_cast<f32>(n - 1);
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const u32 k = z * n + x;
      SceneInstance instance;
      instance.mesh = 0;
      instance.joints = joints;
      instance.bounds_padding = bounds_padding;
      instance.transform.position = Vec3{(static_cast<f32>(x) - center) * spacing, 0.0f,
                                         (static_cast<f32>(z) - center) * spacing};
      instance.transform.rotation = quat_from_euler(radians(static_cast<f32>(k * 37 % 360)),
                                                    radians(static_cast<f32>(k * 11 % 25)), 0.0f);
      const f32 s = 0.6f + 0.2f * static_cast<f32>(k % 4);
      instance.transform.scale =
          k % 3 == 2 ? Vec3{s, s * 1.4f, s * 0.7f} : Vec3{s, s, s};  // every third non-uniform
      out.push_back(instance);
    }
  }
}

}  // namespace

void mesh_bounds(const geometry::ClusterLodMesh& lod, u32 first, u32 count, Vec3& center,
                 f32& radius) {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (u32 i = first; i < first + count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    lo = Vec3{std::min(lo.x, c.center.x - c.radius), std::min(lo.y, c.center.y - c.radius),
              std::min(lo.z, c.center.z - c.radius)};
    hi = Vec3{std::max(hi.x, c.center.x + c.radius), std::max(hi.y, c.center.y + c.radius),
              std::max(hi.z, c.center.z + c.radius)};
  }
  center = (lo + hi) * 0.5f;
  radius = 1e-6f;  // tighter than the box diagonal for elongated meshes
  for (u32 i = first; i < first + count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    radius = std::max(radius, length(c.center - center) + c.radius);
  }
}

bool read_scene_file(const std::string& path, SceneDesc& out, std::string& error) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = std::string("cannot read ") + path + ": " + io::status_name(status);
    return false;
  }
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = std::string(path) + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  const JsonValue* mesh_list = root.is_object() ? root.find("meshes") : nullptr;
  if (mesh_list == nullptr || !mesh_list->is_array() || mesh_list->size() == 0) {
    error = std::string(path) + ": no \"meshes\" array";
    return false;
  }
  const std::string dir(io::parent_path(path));
  for (usize i = 0; i < mesh_list->size(); ++i) {
    const JsonValue& entry = (*mesh_list)[i];
    std::string_view mesh_path;
    const JsonValue* value = entry.is_object() ? entry.find("path") : nullptr;
    if (value == nullptr || !value->get_string(mesh_path) || mesh_path.empty()) {
      error = std::string(path) + ": mesh " + std::to_string(i) + " has no \"path\"";
      return false;
    }
    const std::string relative(mesh_path);
    out.meshes.push_back(
        io::is_absolute_path(relative) || dir.empty() ? relative : io::join_path(dir, relative));
  }
  auto read_vec = [](const JsonValue* value, u32 count, f32* values) {
    if (value == nullptr || !value->is_array() || value->size() != count) return false;
    for (u32 i = 0; i < count; ++i) {
      f64 v = 0.0;
      if (!(*value)[i].get_f64(v)) return false;
      values[i] = static_cast<f32>(v);
    }
    return true;
  };
  const JsonValue* instance_list = root.find("instances");
  if (instance_list == nullptr || !instance_list->is_array() || instance_list->size() == 0) {
    for (u32 i = 0; i < out.meshes.size(); ++i)
      out.instances.push_back(SceneInstance{i, Transform3::identity()});
    return true;
  }
  for (usize i = 0; i < instance_list->size(); ++i) {
    const JsonValue& entry = (*instance_list)[i];
    SceneInstance instance;
    u64 mesh_index = 0;
    const JsonValue* mesh_value = entry.is_object() ? entry.find("mesh") : nullptr;
    if (mesh_value != nullptr && !mesh_value->get_u64(mesh_index)) mesh_index = ~u64{0};
    if (mesh_index >= out.meshes.size()) {
      error = std::string(path) + ": instance " + std::to_string(i) + " names no known mesh";
      return false;
    }
    instance.mesh = static_cast<u32>(mesh_index);
    f32 v[4] = {};
    if (read_vec(entry.find("translation"), 3, v))
      instance.transform.position = Vec3{v[0], v[1], v[2]};
    if (read_vec(entry.find("rotation"), 4, v))
      instance.transform.rotation = normalize(Quat{v[0], v[1], v[2], v[3]});
    if (read_vec(entry.find("scale"), 3, v)) instance.transform.scale = Vec3{v[0], v[1], v[2]};
    // {"animation":{"clip":"Run","speed":1.5,"phase":0.4}} — read whole and handed on unread.
    // An empty object is a legal block and means "the skin's first clip at speed 1"; every field
    // is optional, so a file may say only what it wants to change.
    if (const JsonValue* animation = entry.is_object() ? entry.find("animation") : nullptr;
        animation != nullptr && animation->is_object()) {
      instance.animation.play = true;
      std::string_view clip;
      if (const JsonValue* value = animation->find("clip");
          value != nullptr && value->get_string(clip)) {
        instance.animation.clip = std::string(clip);
      }
      f64 number = 0.0;
      if (const JsonValue* value = animation->find("speed");
          value != nullptr && value->get_f64(number)) {
        instance.animation.speed = static_cast<f32>(number);
      }
      if (const JsonValue* value = animation->find("phase");
          value != nullptr && value->get_f64(number)) {
        instance.animation.phase = static_cast<f32>(number);
      }
    }
    out.instances.push_back(instance);
  }
  return true;
}

bool load_scene(const SceneDesc& desc, SceneData& out, std::string& error) {
  if (desc.meshes.empty()) {
    error = "a scene needs at least one mesh";
    return false;
  }
  SceneDesc resolved = desc;
  // The derived-data cache lives at <repo>/ddc, found by walking up from the executable to the
  // directory that holds AGENTS.md, unless a root was named. With no root there is nowhere to
  // keep a built mesh, so the cache is simply off.
  if (resolved.cache && resolved.ddc.empty()) {
    resolved.ddc = geometry::find_ddc_root(platform::executable_directory());
    if (resolved.ddc.empty()) {
      resolved.cache = false;
      ENGINE_LOG_WARN(log_renderer, "no derived-data root above the executable",
                      log::field("from", platform::executable_directory()));
    }
  }

  const i64 build_start = time::monotonic_ns();
  out.heightfield = resolved.meshes.size() == 1 && resolved.meshes[0].empty();
  out.sources.resize(resolved.meshes.size());
  Vector<geometry::ClusterLodMesh> dags(resolved.meshes.size());
  for (u32 m = 0; m < resolved.meshes.size(); ++m) {
    if (!load_source_mesh(resolved.meshes[m], resolved, out.sources[m], dags[m], error))
      return false;
  }
  out.mesh_primitives = out.sources[0].primitives;
  out.mesh_cache = out.sources[0].cache;
  if (dags.size() == 1) {
    out.lod = std::move(dags[0]);
    geometry::ClusterMeshPart part;
    part.cluster_count = out.lod.mesh.clusters.size();
    part.leaf_cluster_count = out.lod.level_cluster_counts[0];
    part.quant_origin = out.lod.mesh.quant_origin;
    part.quant_scale = out.lod.mesh.quant_scale;
    out.parts.push_back(part);
  } else if (!geometry::merge_cluster_meshes(dags, out.lod, out.parts, &error)) {
    return false;
  }

  // Instances: the caller's, the grid's, or one identity instance of the one mesh.
  Vector<SceneInstance> instances = resolved.instances;
  if (resolved.grid_instances > 1) {
    instances.clear();
    Vec3 mesh_center{};
    f32 mesh_radius = 1.0f;
    mesh_bounds(out.lod, out.parts[0].first_cluster, out.parts[0].leaf_cluster_count, mesh_center,
                mesh_radius);
    make_instance_grid(resolved.grid_instances, mesh_radius, resolved.grid_joints, 0.0f, instances);
  } else if (instances.empty()) {
    instances.push_back(SceneInstance{0, Transform3::identity()});
  }
  u32 pair_count = 0;
  const u32 palette = out.lod.mesh.skin_joint_count;
  for (const SceneInstance& source : instances) {
    if (source.mesh >= out.parts.size()) {
      error = "an instance names mesh " + std::to_string(source.mesh) + ", which the scene has no";
      return false;
    }
    gfx::InstanceDesc instance{};
    gfx::set_instance_transform(instance, mat4_from_transform(source.transform));
    instance.mesh = source.mesh;
    instance.first_pair = pair_count;
    // The cull pass inflates every sphere of a deformed instance by this, in the instance's own
    // space (gfx::InstanceDesc::bounds_padding). It is the caller's number: only the caller knows
    // which clips this instance will play.
    instance.bounds_padding = source.bounds_padding;
    pair_count += out.parts[source.mesh].cluster_count;
    // An instance may only be skinned by a mesh that carries a binding stream, and never by more
    // joints than that stream's palette: a binding names a palette slot with one byte, so a
    // longer array would be addressed by indices that cannot exist. Both are stated rather than
    // silently clamped, because either one means the caller and the content disagree.
    u32 joints = source.joints;
    if (joints > 0 && out.lod.mesh.skin.empty()) {
      error = "an instance is skinned, but its mesh carries no skin binding stream";
      return false;
    }
    if (joints > palette) {
      error = "an instance names " + std::to_string(joints) +
              " joints, but the mesh's palette is " + std::to_string(palette);
      return false;
    }
    out.instance_joints.push_back(joints);
    out.skinned_instances += joints > 0 ? 1u : 0u;
    out.instances.push_back(instance);
  }
  out.pair_count = pair_count;
  out.max_joints = resolved.max_joints;
  if (out.skinned_instances == 0) out.instance_joints.clear();

  update_scene_bounds(out);
  out.build_ns = time::monotonic_ns() - build_start;
  return true;
}

void update_scene_bounds(SceneData& out) {
  // The plain heightfield keeps the half extent it has always used, so a run with no mesh gets
  // exactly the camera it always did.
  if (out.heightfield && out.instances.size() == 1) {
    out.center = Vec3{};
    out.radius = 10.0f;
  } else {
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    Vector<Vec3> centers(out.parts.size());
    Vector<f32> radii(out.parts.size(), 0.0f);
    for (u32 m = 0; m < out.parts.size(); ++m) {
      mesh_bounds(out.lod, out.parts[m].first_cluster, out.parts[m].leaf_cluster_count, centers[m],
                  radii[m]);
    }
    // A skinned instance's bounds are its bind pose's plus the displacement bound, for the same
    // reason the cull pass inflates its spheres: the camera has to frame the character wherever
    // the clip puts it, not only where it rests.
    auto instance_radius = [&](const gfx::InstanceDesc& instance) {
      return (radii[instance.mesh] + instance.bounds_padding) * instance.scale_max;
    };
    for (const gfx::InstanceDesc& instance : out.instances) {
      const Vec3 c = transform_point(instance.world, centers[instance.mesh]);
      const f32 r = instance_radius(instance);
      lo = Vec3{std::min(lo.x, c.x - r), std::min(lo.y, c.y - r), std::min(lo.z, c.z - r)};
      hi = Vec3{std::max(hi.x, c.x + r), std::max(hi.y, c.y + r), std::max(hi.z, c.z + r)};
    }
    out.center = (lo + hi) * 0.5f;
    out.radius = 1e-6f;
    for (const gfx::InstanceDesc& instance : out.instances) {
      const Vec3 c = transform_point(instance.world, centers[instance.mesh]);
      out.radius = std::max(out.radius, length(c - out.center) + instance_radius(instance));
    }
  }
}

}  // namespace engine::renderer
