#include <domain/geometry/cluster_lod.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/gpu_scene.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags k_args = k_address | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
// The visible list is read back by a capture, which resolves a visibility id into the instance
// and cluster it names; that is the only reason it is a transfer source.
constexpr VkBufferUsageFlags k_readable = k_address | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

}  // namespace

GpuScene::~GpuScene() { destroy(); }

bool GpuScene::create(const gfx::Device& device, const SceneData& data,
                      const ResolvedSettings& resolved, std::string* error) {
  destroy();
  device_ = &device;
  data_ = &data;
  ray_tracing_ = resolved.rt_chain;
  deform_ = resolved.settings.deform;
  cluster_count_ = data.cluster_count();
  leaf_count_ = data.leaf_count();
  instance_count_ = data.instances.size();
  pair_count_ = data.pair_count;
  triangles_per_cluster_ = geometry::ClusterLodOptions{}.max_triangles;
  instance_table_ = data.instances;

  if (!bindless_.create(device, gfx::BindlessConfig{}, error) ||
      !upload_geometry(resolved, error) || !upload_materials(resolved, error) ||
      !create_working_set(resolved, error) || !create_ray_tracing(resolved, error)) {
    destroy();
    return false;
  }

  // One MeshDesc per mesh: its own grid, its range of the shared cluster array, the one
  // quantized stream every mesh of the scene indexes, and where a deformed instance's positions
  // come from instead — the frame's pool, the deform table, and this mesh's cluster templates.
  // It is uploaded last because those three addresses have to exist first.
  Vector<gfx::MeshDesc> mesh_descs;
  for (const geometry::ClusterMeshPart& part : data.parts) {
    gfx::MeshDesc desc{};
    desc.quant = Vec4{part.quant_origin, part.quant_scale};
    desc.quantized = quantized.address;
    desc.first_cluster = part.first_cluster;
    desc.cluster_count = part.cluster_count;
    desc.deform_pool = deform_pool.address;
    desc.deform = deform_table.address;
    desc.templates = clas_templates.addresses.address;
    mesh_descs.push_back(desc);
  }
  if (!gfx::upload_buffer(device, mesh_descs.data(), mesh_descs.size() * sizeof(gfx::MeshDesc),
                          k_storage, meshes, error)) {
    destroy();
    return false;
  }
  return true;
}

bool GpuScene::upload_geometry(const ResolvedSettings& resolved, std::string* error) {
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  // Positions go to the GPU on the mesh-wide 16-bit grid: six bytes a vertex instead of twelve.
  const u64 float_position_bytes = u64{lod.mesh.vertices.size()} * sizeof(Vec3);
  const u64 quantized_position_bytes =
      u64{lod.mesh.quantized.size()} * sizeof(u16) + sizeof(gfx::MeshDesc);
  if (!gfx::upload_buffer(device, lod.mesh.clusters.data(),
                          u64{cluster_count_} * sizeof(geometry::ClusterDesc), k_storage, clusters,
                          error) ||
      !gfx::upload_buffer(device, lod.mesh.quantized.data(),
                          u64{lod.mesh.quantized.size()} * sizeof(u16), k_storage, quantized,
                          error) ||
      !gfx::upload_buffer(device, lod.mesh.triangles.data(),
                          u64{lod.mesh.triangles.size()} * sizeof(u32), k_storage, triangles,
                          error) ||
      !gfx::upload_buffer(device, lod.lod.data(),
                          u64{cluster_count_} * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                          error)) {
    return false;
  }
  // The deformed-vertex pool and its per-instance table. The pool is device-local: nothing reads
  // it back, and the cluster acceleration structure builds take it as a build input. Every
  // instance gets a block as long as its mesh's cluster-ordered vertex range, so a cluster's
  // vertices are contiguous there and the CLAS records can point straight at them.
  // `pool_offset` is added to the **scene-wide** vertex index, so it is the block's base biased
  // by the mesh's first vertex; both are u32 and the bias wraps, which is the arithmetic the
  // shaders do.
  if (deform_) {
    Vector<gfx::DeformDesc> deform_descs;
    const u32 total_vertices = lod.mesh.vertices.size();
    u32 pool_vertices = 0;
    for (u32 i = 0; i < instance_count_; ++i) {
      const geometry::ClusterMeshPart& part = data_->parts[instance_table_[i].mesh];
      const u32 next = instance_table_[i].mesh + 1 < data_->parts.size()
                           ? data_->parts[instance_table_[i].mesh + 1].first_vertex
                           : total_vertices;
      gfx::DeformDesc desc{};
      desc.vertex_count = next - part.first_vertex;
      desc.pool_offset = pool_vertices - part.first_vertex;
      desc.flags = resolved.settings.deform_kind;
      pool_vertices += desc.vertex_count;
      instance_table_[i].deform = i;
      deform_descs.push_back(desc);
    }
    deform_pool_bytes_ = u64{pool_vertices} * 3 * sizeof(f32);
    constexpr VkBufferUsageFlags k_pool_usage =
        k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    bool ok =
        gfx::create_buffer(device, deform_pool_bytes_, k_pool_usage, false, deform_pool, error) &&
        gfx::upload_buffer(device, deform_descs.data(),
                           deform_descs.size() * sizeof(gfx::DeformDesc), k_storage, deform_table,
                           error);
    for (gfx::BufferResource& args : deform_args)
      ok = ok && gfx::create_buffer(device, sizeof(u32) * 3, k_args, false, args, error);
    if (!ok) return false;
    ENGINE_LOG_INFO(
        log_renderer, "deformed-vertex pool", log::field("mode", deform_name(resolved.settings)),
        log::field("instances", instance_count_), log::field("pool_vertices", pool_vertices),
        log::field("pool_bytes", deform_pool_bytes_));
  }
  // The float positions stay only for the frames that build acceleration structures: the
  // cluster structure builds read them.
  if (ray_tracing_ && !gfx::upload_buffer(device, lod.mesh.vertices.data(), float_position_bytes,
                                          k_storage | gfx::k_build_input_usage, vertices, error)) {
    return false;
  }
  ENGINE_LOG_INFO(log_renderer, "positions quantized",
                  log::field("vertices", lod.mesh.vertices.size()),
                  log::field("float_bytes", float_position_bytes),
                  log::field("quantized_bytes", quantized_position_bytes),
                  log::field("grid_step", lod.mesh.quant_scale));
  return gfx::upload_buffer(device, lod.mesh.attributes.data(),
                            u64{lod.mesh.attributes.size()} * sizeof(geometry::VertexAttributes),
                            k_storage, attributes, error);
}

bool GpuScene::upload_materials(const ResolvedSettings&, std::string* error) {
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  if (!gfx::create_sampler(device, VK_FILTER_LINEAR, sampler_, error)) return false;
  const u32 sampler_slot = bindless_.add_sampler(sampler_);
  Vector<gfx::ResolveMaterial> material_table;
  Vector<u32> cluster_material(cluster_count_);
  Vector<u32> mesh_material_base(data_->parts.size(), 0u);
  if (data_->heightfield) {
    // A procedural ripple texture, linear-sampled through the bindless set.
    constexpr u32 k_texture_size = 256;
    Vector<u8> texels(k_texture_size * k_texture_size * 4);
    for (u32 y = 0; y < k_texture_size; ++y) {
      for (u32 x = 0; x < k_texture_size; ++x) {
        const f32 fx = static_cast<f32>(x);
        const f32 fy = static_cast<f32>(y);
        const f32 ripple = 0.5f + 0.5f * std::sin(fx * 0.25f + 2.0f * std::sin(fy * 0.08f));
        const f32 grain =
            0.5f + 0.5f * std::sin(fx * 1.7f + fy * 2.3f) * std::sin(fy * 1.1f - fx * 0.7f);
        const u8 v = static_cast<u8>((0.62f + 0.3f * ripple + 0.08f * grain) * 255.0f);
        u8* t = &texels[(y * k_texture_size + x) * 4];
        t[0] = t[1] = t[2] = v;
        t[3] = 255;
      }
    }
    if (!gfx::upload_image_2d(device, k_texture_size, k_texture_size, VK_FORMAT_R8G8B8A8_UNORM,
                              texels.data(), texels.size(), procedural_texture_, error) ||
        !gfx::create_image_view(device, procedural_texture_, procedural_view_, error)) {
      return false;
    }
    const u32 texture_slot =
        bindless_.add_sampled_image(procedural_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Materials: a flat table indexed per cluster by the height band of the cluster's center.
    material_table.resize(3);
    material_table[0].albedo = Vec4{0.86f, 0.72f, 0.46f, 0.9f};  // sand
    material_table[1].albedo = Vec4{0.42f, 0.40f, 0.38f, 0.7f};  // rock
    material_table[2].albedo = Vec4{0.92f, 0.94f, 0.97f, 0.4f};  // snow
    for (u32 i = 0; i < 2; ++i) {  // sand and rock carry the ripple texture at different scales
      material_table[i].albedo_texture = texture_slot;
      material_table[i].sampler = sampler_slot;
      material_table[i].uv_scale = i == 0 ? 24.0f : 9.0f;
    }
    for (u32 i = 0; i < cluster_count_; ++i) {
      const f32 y = lod.mesh.clusters[i].center.y;
      cluster_material[i] = y < -0.15f ? 0u : y < 0.65f ? 1u : 2u;
    }
  } else {
    // Materials from the files, one mesh's table after the last: every instance adds its mesh's
    // base to the cluster's material index, so the clusters keep mesh-local indices. Images are
    // decoded on the CPU (embedded bytes or a file beside the glTF) and uploaded once each, into
    // one bindless slot every material of that mesh which names the image shares; an image that
    // fails to decode leaves its slot empty with a warning. Base color is color and goes up as
    // sRGB, so that sampling returns linear; metallic-roughness and normal maps are data, not
    // color, and go up UNORM. A glTF never gives one image both roles, so the format an image is
    // first asked for is the one it keeps.
    for (u32 m = 0; m < data_->sources.size(); ++m) {
      const SourceMesh& source_mesh = data_->sources[m];
      const assets::MeshData& mesh_data = source_mesh.data;
      mesh_material_base[m] = material_table.size();
      Vector<u32> image_slot(mesh_data.images.size(), gfx::k_no_texture);
      Vector<bool> image_tried(mesh_data.images.size(), false);
      const std::string& mesh_dir = source_mesh.image_dir;  // the glTF's or the container's
      auto texture_slot_of = [&](i32 image_index, VkFormat format) -> u32 {
        if (image_index < 0 || static_cast<u32>(image_index) >= mesh_data.images.size())
          return gfx::k_no_texture;
        const u32 index = static_cast<u32>(image_index);
        if (image_tried[index]) return image_slot[index];
        image_tried[index] = true;
        const assets::ImageRef& ref = mesh_data.images[index];
        image::Image decoded;
        std::string image_error;
        bool ok = false;
        if (!ref.bytes.empty()) {
          ok = image::decode_image(std::span<const u8>(ref.bytes.data(), ref.bytes.size()), decoded,
                                   4, &image_error);
        } else if (!ref.uri.empty()) {
          const std::string path = mesh_dir.empty() ? ref.uri : io::join_path(mesh_dir, ref.uri);
          ok = image::read_image(path, decoded, 4, &image_error) == io::Status::Ok;
        } else {
          image_error = "image has neither bytes nor a uri";
        }
        gfx::ImageResource uploaded;
        VkImageView view = VK_NULL_HANDLE;
        if (ok && (!gfx::upload_image_2d(device, decoded.width, decoded.height, format,
                                         decoded.pixels.data(), decoded.pixels.size(), uploaded,
                                         &image_error) ||
                   !gfx::create_image_view(device, uploaded, view, &image_error))) {
          if (uploaded.image != VK_NULL_HANDLE) gfx::destroy_image(device, uploaded);
          ok = false;
        }
        if (!ok) {
          ENGINE_LOG_WARN(log_renderer, "texture skipped", log::field("image", index),
                          log::field("name", ref.name), log::field("error", image_error));
          return gfx::k_no_texture;
        }
        textures_.push_back(uploaded);
        texture_views_.push_back(view);
        image_slot[index] =
            bindless_.add_sampled_image(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return image_slot[index];
      };
      for (const assets::Material& source : mesh_data.materials) {
        gfx::ResolveMaterial material;
        material.albedo =
            Vec4{source.base_color.x, source.base_color.y, source.base_color.z, source.roughness};
        // The emissive factor goes through as a constant term. A material that modulates it with
        // an emissive texture is left unlit instead of glowing at full factor everywhere: the
        // resolve has no emissive slot yet, and too dark is a smaller lie than too bright.
        const Vec3 emissive = source.emissive_image < 0 ? source.emissive : Vec3{};
        material.emissive = Vec4{emissive, source.metallic};
        material.albedo_texture = texture_slot_of(source.base_color_image, VK_FORMAT_R8G8B8A8_SRGB);
        material.metallic_roughness_texture =
            texture_slot_of(source.metallic_roughness_image, VK_FORMAT_R8G8B8A8_UNORM);
        material.normal_texture = texture_slot_of(source.normal_image, VK_FORMAT_R8G8B8A8_UNORM);
        material.normal_scale = source.normal_scale;
        material.sampler = sampler_slot;
        material.uv_scale = 1.0f;
        material_table.push_back(material);
      }
      const u32 local_count = material_table.size() - mesh_material_base[m];
      gfx::ResolveMaterial plain;
      plain.albedo = Vec4{0.8f, 0.8f, 0.8f, 0.6f};
      material_table.push_back(plain);  // the default for a primitive that names no material
      const geometry::ClusterMeshPart& part = data_->parts[m];
      for (u32 i = 0; i < part.cluster_count; ++i) {
        const i32 material = source_mesh.part_material[source_mesh.part_of_cluster[i]];
        cluster_material[part.first_cluster + i] =
            material >= 0 && static_cast<u32>(material) < local_count ? static_cast<u32>(material)
                                                                      : local_count;
      }
    }
  }
  // Now that the tables are laid out, every instance knows where its mesh's materials start.
  for (gfx::InstanceDesc& instance : instance_table_)
    instance.material_base = mesh_material_base[instance.mesh];
  material_count_ = material_table.size();
  return gfx::upload_buffer(device, material_table.data(),
                            u64{material_count_} * sizeof(gfx::ResolveMaterial), k_storage,
                            materials, error) &&
         gfx::upload_buffer(device, cluster_material.data(), u64{cluster_count_} * sizeof(u32),
                            k_storage, cluster_materials, error) &&
         gfx::upload_buffer(device, instance_table_.data(),
                            u64{instance_count_} * sizeof(gfx::InstanceDesc), k_storage, instances,
                            error);
}

bool GpuScene::create_working_set(const ResolvedSettings&, std::string* error) {
  const gfx::Device& device = *device_;
  // One visible list for the whole frame, in three runs: the hardware pass 1, the hardware pass
  // 2, and the software rasterizer. A visibility id names an entry of the whole list, so the
  // resolve reads one array however many draws filled it, and each draw's `visible_offset` is
  // where its run starts. Every run is as long as the pair count, which is as many entries as
  // any one draw can produce.
  const u64 visible_entry_bytes = 2 * sizeof(u32);
  visible_run_bytes_ = u64{pair_count_} * visible_entry_bytes;
  bool ok = gfx::create_buffer(device, visible_run_bytes_ * k_visible_runs, k_readable, false,
                               visible, error) &&
            gfx::create_buffer(device, sizeof(u32) * 3, k_args, false, sw_args, error);
  for (u32 i = 0; i < 2; ++i) {
    ok = ok && gfx::create_buffer(device, sizeof(u32) * 4, k_args, false, draw_args[i], error) &&
         gfx::create_buffer(device, u64{pair_count_} * sizeof(u32),
                            k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, flags[i], error);
  }
  return ok;
}

bool GpuScene::create_ray_tracing(const ResolvedSettings& resolved, std::string* error) {
  if (!ray_tracing_) return true;
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  // Every pair may be in some frame's cut, so the cluster acceleration structures are sized for
  // all of them, and a pair's base geometry index is its entry in the visible list, so the
  // largest geometry index is the last pair. One cluster bottom-level structure per instance,
  // sized for that instance's mesh; the top-level structure instances them with the world
  // transforms and a custom index that is the scene instance.
  Vector<u8> packed;
  gfx::pack_cluster_indices(
      std::span<const u32>(lod.mesh.triangles.data(), lod.mesh.triangles.size()), packed);
  gfx::ClusterSetLimits limits;
  limits.max_clusters = pair_count_;
  limits.max_triangles_per_cluster = triangles_per_cluster_;
  limits.max_vertices_per_cluster = geometry::ClusterLodOptions{}.max_vertices;
  limits.max_geometry_index = pair_count_ - 1;
  limits.instantiate = resolved.settings.rt_templates;
  constexpr VkBufferUsageFlags k_record_usage =
      k_address | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  bool ok =
      gfx::upload_buffer(device, packed.data(), packed.size(), gfx::k_build_input_usage, indices8,
                         error) &&
      gfx::create_buffer(device, gfx::k_cluster_build_record_bytes * pair_count_, k_record_usage,
                         false, records, error) &&
      gfx::create_buffer(device, sizeof(u32), k_record_usage, false, record_count, error) &&
      gfx::create_buffer(device, u64{pair_count_} * sizeof(u32), k_address, false, slots, error) &&
      gfx::create_buffer(device, u64{instance_count_} * sizeof(u32),
                         k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, instance_counts,
                         error) &&
      gfx::create_buffer(device, u64{instance_count_} * sizeof(u32), k_address, false,
                         instance_first, error) &&
      gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * instance_count_, k_record_usage,
                         false, blas_records, error) &&
      gfx::create_cluster_set(device, limits, clas_set, error) &&
      gfx::create_tlas(device, instance_count_, gfx::k_build_fast_trace, tlas, error) &&
      gfx::create_buffer(device, gfx::k_instance_record_bytes * instance_count_,
                         gfx::k_build_input_usage, true, rt_instances, error);
  u64 blas_bytes = 0;
  cluster_blas.resize(instance_count_);
  for (u32 i = 0; i < instance_count_ && ok; ++i) {
    ok = gfx::create_cluster_blas(device, data_->parts[instance_table_[i].mesh].cluster_count,
                                  cluster_blas[i], error);
    if (ok) blas_bytes += cluster_blas[i].data.size;
  }
  // One template per cluster of the scene, built from the rest pose, with cluster id and base
  // geometry index zero so an instantiate record's offsets are the visible entry outright. Built
  // once below; instantiated from the pool every frame.
  if (ok && resolved.settings.rt_templates) {
    gfx::ClusterSetLimits template_limits = limits;
    template_limits.max_clusters = cluster_count_;
    template_limits.max_geometry_index = 0;
    template_limits.instantiate = false;
    ok = gfx::create_cluster_templates(device, template_limits, clas_templates, error) &&
         gfx::create_buffer(device, gfx::k_cluster_template_record_bytes * cluster_count_,
                            k_record_usage, true, template_records, error);
    if (ok) {
      Vector<gfx::ClusterBuildInput> template_inputs;
      for (u32 c = 0; c < cluster_count_; ++c) {
        const geometry::ClusterDesc& desc = lod.mesh.clusters[c];
        gfx::ClusterBuildInput in;
        in.cluster_id = 0;  // the instantiate record's offsets carry the visible entry
        in.triangle_count = desc.triangle_count;
        in.vertex_count = desc.vertex_count;
        in.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
        in.indices = indices8.address + u64{desc.triangle_offset} * 3;
        template_inputs.push_back(in);
      }
      gfx::write_cluster_template_records(
          std::span<const gfx::ClusterBuildInput>(template_inputs.data(), template_inputs.size()),
          template_records.mapped);
    }
  }
  if (ok) {
    u64 scratch_bytes = clas_set.build_scratch_bytes;
    scratch_bytes = std::max(scratch_bytes, tlas.build_scratch_bytes);
    scratch_bytes = std::max(scratch_bytes, clas_templates.build_scratch_bytes);
    for (const gfx::ClusterBlas& blas : cluster_blas)
      scratch_bytes = std::max(scratch_bytes, blas.build_scratch_bytes);
    ok = gfx::create_scratch(device, scratch_bytes, rt_scratch, error);
    tlas_slot_ = bindless_.add_acceleration_structure(tlas.handle);
    if (tlas_slot_ == gfx::BindlessSet::k_invalid_slot) {
      ok = false;
      if (error != nullptr) *error = "no bindless slot for the top-level structure";
    }
  }
  // The templates are built once, before any frame: everything after them is per frame.
  if (ok && resolved.settings.rt_templates) {
    ok = gfx::submit_immediate(
        device,
        [&](VkCommandBuffer cb) {
          gfx::build_cluster_templates(cb, clas_templates, template_records.address, 0, rt_scratch);
          gfx::acceleration_build_barrier(
              cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        },
        error);
    if (ok) {
      const auto* sizes = static_cast<const u32*>(clas_templates.sizes.mapped);
      for (u32 c = 0; c < cluster_count_; ++c)
        template_bytes_ += sizes[c];
    }
  }
  if (!ok) return false;
  ENGINE_LOG_INFO(
      log_renderer, "ray tracing ready", log::field("pairs", pair_count_),
      log::field("instances", instance_count_), log::field("clas_bytes", clas_set.data.size),
      log::field("blas_bytes", blas_bytes), log::field("scratch_bytes", rt_scratch.size),
      log::field("templates", resolved.settings.rt_templates ? cluster_count_ : 0u),
      log::field("template_bytes", template_bytes_));
  return true;
}

void GpuScene::destroy() noexcept {
  if (device_ == nullptr) return;
  const gfx::Device& device = *device_;
  gfx::destroy_buffer(device, meshes);
  gfx::destroy_cluster_templates(device, clas_templates);
  gfx::destroy_buffer(device, template_records);
  gfx::destroy_acceleration_structure(device, tlas);
  for (gfx::ClusterBlas& blas : cluster_blas)
    gfx::destroy_cluster_blas(device, blas);
  cluster_blas.clear();
  gfx::destroy_cluster_set(device, clas_set);
  gfx::destroy_buffer(device, rt_scratch);
  gfx::destroy_buffer(device, rt_instances);
  gfx::destroy_buffer(device, blas_records);
  gfx::destroy_buffer(device, instance_first);
  gfx::destroy_buffer(device, instance_counts);
  gfx::destroy_buffer(device, slots);
  gfx::destroy_buffer(device, record_count);
  gfx::destroy_buffer(device, records);
  gfx::destroy_buffer(device, indices8);
  for (gfx::BufferResource& args : deform_args)
    gfx::destroy_buffer(device, args);
  gfx::destroy_buffer(device, deform_table);
  gfx::destroy_buffer(device, deform_pool);
  for (u32 i = 0; i < 2; ++i) {
    gfx::destroy_buffer(device, flags[i]);
    gfx::destroy_buffer(device, draw_args[i]);
  }
  gfx::destroy_buffer(device, sw_args);
  gfx::destroy_buffer(device, visible);
  gfx::destroy_buffer(device, cluster_materials);
  gfx::destroy_buffer(device, materials);
  gfx::destroy_buffer(device, attributes);
  gfx::destroy_buffer(device, lods);
  gfx::destroy_buffer(device, triangles);
  gfx::destroy_buffer(device, vertices);
  gfx::destroy_buffer(device, instances);
  gfx::destroy_buffer(device, quantized);
  gfx::destroy_buffer(device, clusters);
  for (VkImageView view : texture_views_)
    gfx::destroy_image_view(device, view);
  texture_views_.clear();
  for (gfx::ImageResource& image : textures_)
    gfx::destroy_image(device, image);
  textures_.clear();
  gfx::destroy_image_view(device, procedural_view_);
  procedural_view_ = VK_NULL_HANDLE;
  if (procedural_texture_.image != VK_NULL_HANDLE) gfx::destroy_image(device, procedural_texture_);
  procedural_texture_ = gfx::ImageResource{};
  gfx::destroy_sampler(device, sampler_);
  sampler_ = VK_NULL_HANDLE;
  bindless_.destroy();
  instance_table_.clear();
  device_ = nullptr;
  data_ = nullptr;
  cluster_count_ = leaf_count_ = instance_count_ = pair_count_ = material_count_ = 0;
  triangles_per_cluster_ = 0;
  visible_run_bytes_ = deform_pool_bytes_ = template_bytes_ = 0;
  tlas_slot_ = gfx::BindlessSet::k_invalid_slot;
  ray_tracing_ = deform_ = false;
}

}  // namespace engine::renderer
