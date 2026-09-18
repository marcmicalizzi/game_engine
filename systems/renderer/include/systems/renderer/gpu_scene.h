#pragma once

// The scene on the device (docs/subsystems/renderer.md). Everything whose size is a function of
// the *scene* lives here: the global geometry buffers behind device addresses, the material
// table and the bindless textures, the instance table, the deformed-vertex pool, the frame's
// visible list and the indirect argument blocks it is counted into, and — when the settings ask
// for ray tracing — the cluster acceleration structures, the per-instance bottom-level
// structures, and the top-level structure over them.
//
// The line between this and `SceneRenderer` is size: a `GpuScene` is sized by the scene and a
// `SceneRenderer` by the screen. That is also the order they have to be built in, because the
// `MeshDesc` array names the deformed-vertex pool and this scene's cluster templates, so it is
// uploaded last, after every address it carries exists.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/acceleration.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/vulkan.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <string>

namespace engine::renderer {

// The frame's visible list is one array in three runs — the hardware pass 1, the hardware pass
// 2, and the software rasterizer — so a visibility id names an entry of the whole list however
// many draws filled it.
inline constexpr u32 k_visible_runs = 3;

class GpuScene {
 public:
  GpuScene() noexcept = default;
  ~GpuScene();
  ENGINE_NON_COPYABLE(GpuScene);

  // Uploads `data` with the buffers `resolved` calls for. The device must outlive the scene.
  bool create(const gfx::Device& device, const SceneData& data, const ResolvedSettings& resolved,
              std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  const SceneData& data() const noexcept { return *data_; }
  gfx::BindlessSet& bindless() noexcept { return bindless_; }
  const gfx::BindlessSet& bindless() const noexcept { return bindless_; }

  u32 cluster_count() const noexcept { return cluster_count_; }
  u32 leaf_count() const noexcept { return leaf_count_; }
  u32 instance_count() const noexcept { return instance_count_; }
  u32 pair_count() const noexcept { return pair_count_; }
  u32 material_count() const noexcept { return material_count_; }
  u32 triangles_per_cluster() const noexcept { return triangles_per_cluster_; }
  u64 deform_pool_bytes() const noexcept { return deform_pool_bytes_; }
  u64 template_bytes() const noexcept { return template_bytes_; }
  u64 visible_run_bytes() const noexcept { return visible_run_bytes_; }
  u32 tlas_slot() const noexcept { return tlas_slot_; }

  // ---- the global buffers, read through device addresses -------------------------------------
  gfx::BufferResource clusters;           // geometry::ClusterDesc[]
  gfx::BufferResource quantized;          // three u16 per vertex on each mesh's own grid
  gfx::BufferResource vertices;           // float positions; only the acceleration builds read them
  gfx::BufferResource triangles;          // packed local indices
  gfx::BufferResource lods;               // geometry::ClusterLodDesc[]
  gfx::BufferResource attributes;         // geometry::VertexAttributes[]
  gfx::BufferResource meshes;             // gfx::MeshDesc[], uploaded last
  gfx::BufferResource instances;          // gfx::InstanceDesc[]
  gfx::BufferResource materials;          // gfx::ResolveMaterial[]
  gfx::BufferResource cluster_materials;  // u32 per cluster

  // ---- the frame's working set, sized by the scene --------------------------------------------
  gfx::BufferResource visible;       // u32x2[3 * pair_count]: {instance, cluster} per entry
  gfx::BufferResource draw_args[2];  // occlusion pass 1 and pass 2 indirect blocks
  gfx::BufferResource sw_args;       // the software rasterizer's indirect dispatch block
  gfx::BufferResource flags[2];      // drawn last frame / this frame, ping-pong, by pair

  // ---- the deformed-vertex pool ----------------------------------------------------------------
  gfx::BufferResource deform_pool;   // f32[3 * pool_vertices]
  gfx::BufferResource deform_table;  // gfx::DeformDesc[] indexed by InstanceDesc::deform
  gfx::BufferResource deform_args[k_visible_runs];

  // ---- ray tracing ------------------------------------------------------------------------------
  gfx::BufferResource indices8;         // 8-bit packed cluster indices for the CLAS builds
  gfx::BufferResource records;          // CLAS build records written from the cull output
  gfx::BufferResource record_count;     // u32: how many
  gfx::BufferResource slots;            // u32 per pair: the records pass's bucketing scratch
  gfx::BufferResource instance_counts;  // u32 per instance: its surviving clusters
  gfx::BufferResource instance_first;   // u32 per instance: its dense record base
  gfx::BufferResource blas_records;     // one 16-byte bottom-level record per instance
  gfx::BufferResource rt_instances;     // one top-level instance record per instance
  gfx::BufferResource rt_scratch;
  gfx::BufferResource template_records;
  gfx::ClusterSet clas_set;
  gfx::ClusterTemplateSet clas_templates;
  Vector<gfx::ClusterBlas> cluster_blas;  // one per scene instance
  gfx::AccelerationStructure tlas;

 private:
  bool upload_geometry(const ResolvedSettings& resolved, std::string* error);
  bool upload_materials(const ResolvedSettings& resolved, std::string* error);
  bool create_working_set(const ResolvedSettings& resolved, std::string* error);
  bool create_ray_tracing(const ResolvedSettings& resolved, std::string* error);

  const gfx::Device* device_ = nullptr;
  const SceneData* data_ = nullptr;
  gfx::BindlessSet bindless_;
  gfx::ImageResource procedural_texture_;
  VkImageView procedural_view_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  Vector<gfx::ImageResource> textures_;  // decoded from the meshes' images
  Vector<VkImageView> texture_views_;
  Vector<gfx::InstanceDesc> instance_table_;  // the scene's, with material_base and deform filled
  u32 cluster_count_ = 0;
  u32 leaf_count_ = 0;
  u32 instance_count_ = 0;
  u32 pair_count_ = 0;
  u32 material_count_ = 0;
  u32 triangles_per_cluster_ = 0;
  u64 visible_run_bytes_ = 0;
  u64 deform_pool_bytes_ = 0;
  u64 template_bytes_ = 0;
  u32 tlas_slot_ = gfx::BindlessSet::k_invalid_slot;
  bool ray_tracing_ = false;
  bool deform_ = false;
};

}  // namespace engine::renderer
