#pragma once

// The smallest scene the shaders read: one mesh on its own 16-bit position grid and one identity
// instance of it. Every test that draws needs it, so the upload lives here rather than eight
// times over. `pair_count()` is what a cull dispatch covers and how long the visible list,
// `flags`, and `prev_flags` are.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/geometry/cluster.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/vulkan.h>

#include <string>

namespace engine::gfx_test {

struct SingleInstance {
  gfx::BufferResource quantized;  // three u16 per vertex
  gfx::BufferResource meshes;     // one MeshDesc
  gfx::BufferResource instances;  // one identity InstanceDesc
  u32 cluster_count = 0;

  bool create(const gfx::Device& device, const geometry::ClusterMesh& mesh, u32 clusters,
              std::string* error) {
    cluster_count = clusters;
    constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    gfx::MeshDesc mesh_desc{};
    mesh_desc.quant = Vec4{mesh.quant_origin, mesh.quant_scale};
    mesh_desc.first_cluster = 0;
    mesh_desc.cluster_count = clusters;
    gfx::InstanceDesc instance{};
    gfx::set_instance_transform(instance, Mat4::identity());
    if (!gfx::upload_buffer(device, mesh.quantized.data(), mesh.quantized.size() * sizeof(u16),
                            k_storage, quantized, error)) {
      return false;
    }
    mesh_desc.quantized = quantized.address;
    return gfx::upload_buffer(device, &mesh_desc, sizeof(mesh_desc), k_storage, meshes, error) &&
           gfx::upload_buffer(device, &instance, sizeof(instance), k_storage, instances, error);
  }

  u32 pair_count() const noexcept { return cluster_count; }

  void destroy(const gfx::Device& device) noexcept {
    gfx::destroy_buffer(device, instances);
    gfx::destroy_buffer(device, meshes);
    gfx::destroy_buffer(device, quantized);
  }
};

}  // namespace engine::gfx_test
