#pragma once

// The device a gfx test draws on, and the raster path it draws the cluster geometry through.
//
// ADR-0024 makes the vertex-shader cluster path (`cluster_vertex.slang`) the baseline tier: the
// same clusters, the same visibility buffer and the same cull pass (`CullParams::count_index` =
// 1, `vkCmdDrawIndirect`) as the mesh-shader path, on GPUs without `VK_EXT_mesh_shader` — which
// is both of the project's legacy GPU machines. So a case about the *picture* — the shading
// against `brdf_reference.h`, the cull against its CPU reference, culling never changing the
// picture, the visibility ids — does not need mesh shaders and must not skip without them. It
// draws through `ClusterRaster`, which is the mesh path where the device has one and the vertex
// path where it does not, and asserts the same things either way. Only a case about the mesh
// stage itself needs `VK_EXT_mesh_shader`, and only a case that traces rays needs
// `VK_KHR_ray_query`; those skip, naming the extension.
//
// Every skip in this suite goes through `require` or `part`, so it reads `skipped: <why>` (the
// whole case) or `part skipped: <what>: <why>` (one half of it), and a log counts them with one
// search. doctest has no skip at run time: a skipped case returns early and is counted as passed,
// which is exactly why the message has to be findable.
//
// `ENGINE_GFX_TEST_DEVICE` runs the suite on a weaker device than the GPU it has, through
// `DeviceOptions::overrides` — a device that really lacks what the profile removes, so
// `create_mesh_pipeline` fails on it exactly as on Pascal (docs/subsystems/gfx.md, "What a device
// has to have"). It is how the baseline tier is exercised on a machine that has everything:
//
//   baseline   no VK_EXT_mesh_shader: every picture case draws through the vertex path, and the
//              ray cases still trace wherever the GPU can
//   titanxp    the TITAN Xp's absences as its driver reports them (docs/ci/self-hosted-runners.md,
//              "The first run on the Titan Xp"): no mesh shaders, no ray queries, no cluster
//              acceleration structures, the same profile as the renderer's TITAN-Xp-like case
//   a,b,...    requirement-table rows to report absent: VK_EXT_mesh_shader,VK_KHR_ray_query
//
// A forced profile that yields no device **fails** the case rather than skipping it: a run that
// asked for the baseline tier and got nothing must not come back green. A misspelled row is one
// way to get there (`apply_overrides` refuses a name that matches no row).
//
// The cases that are about a device rather than a picture — `device_tests.cpp` and the profiles
// of `requirements_tests.cpp` — build their own options and ignore the variable.

#include <core/base/types.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <initializer_list>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_vertex.spv.h>
#include <string>

namespace engine::gfx_test {

// ---- the device ---------------------------------------------------------------------------

// The value of ENGINE_GFX_TEST_DEVICE, read once; empty for the GPU as it is.
inline const std::string& device_profile() {
  static const std::string profile = test::detail::environment("ENGINE_GFX_TEST_DEVICE");
  return profile;
}

// Appends the rows `profile` reports absent to `absent`.
inline void profile_absent(const std::string& profile, Vector<std::string>& absent) {
  if (profile.empty()) return;
  if (profile == "baseline") {
    absent.push_back("VK_EXT_mesh_shader");
    return;
  }
  if (profile == "titanxp") {
    for (const char* row :
         {"VK_EXT_mesh_shader", "VK_KHR_ray_query", "VK_NV_cluster_acceleration_structure",
          "VK_EXT_memory_decompression", "VK_KHR_fragment_shading_rate"}) {
      absent.push_back(row);
    }
    return;
  }
  usize start = 0;
  while (start <= profile.size()) {
    usize end = profile.find(',', start);
    if (end == std::string::npos) end = profile.size();
    usize first = start;
    usize last = end;
    while (first < last && profile[first] == ' ')
      ++first;
    while (last > first && profile[last - 1] == ' ')
      --last;
    if (last > first) absent.push_back(profile.substr(first, last - first));
    start = end + 1;
  }
}

// `options` with the forced profile's absences added to whatever overrides it already carries.
inline gfx::DeviceOptions device_options(gfx::DeviceOptions options = {}) {
  profile_absent(device_profile(), options.overrides.absent);
  return options;
}

// Creates the device a case draws on. Without a usable Vulkan device the case records a skip and
// this returns false; under a forced profile the same thing is a failure.
inline bool open_device(gfx::Device& device, const gfx::DeviceOptions& options = {}) {
  std::string error;
  if (device.create(device_options(options), &error)) return true;
  if (!device_profile().empty()) {
    FAIL_CHECK("ENGINE_GFX_TEST_DEVICE=" << device_profile()
                                         << " asked for a device and got none: " << error);
    return false;
  }
  MESSAGE("skipped: device unavailable: " << error);
  return false;
}

// ---- what a case needs --------------------------------------------------------------------

enum class Need : u8 {
  VisibilityBuffer,  // 64-bit buffer atomics: every rasterizer writes the buffer with an atomic max
  MeshShader,
  AccelerationStructure,
  RayQuery,
  ClusterAccelerationStructure,
  GeometryShader,  // SV_PrimitiveID in a vertex pipeline: the vertex path's indexed draw
  FullDrawIndex,   // index values past 2^24 - 1: the same draw's `slot << 8 | local` indices
};

// The requirement-table row that provides it, which is the name a skip gives.
inline const char* need_row(Need need) noexcept {
  switch (need) {
    case Need::VisibilityBuffer: return "shaderBufferInt64Atomics";
    case Need::MeshShader: return "VK_EXT_mesh_shader";
    case Need::AccelerationStructure: return "VK_KHR_acceleration_structure";
    case Need::RayQuery: return "VK_KHR_ray_query";
    case Need::ClusterAccelerationStructure: return "VK_NV_cluster_acceleration_structure";
    case Need::GeometryShader: return "geometryShader";
    case Need::FullDrawIndex: return "fullDrawIndexUint32";
  }
  return "?";
}

inline bool has(const gfx::DeviceFeatures& f, Need need) noexcept {
  switch (need) {
    case Need::VisibilityBuffer: return f.buffer_int64_atomics;
    case Need::MeshShader: return f.mesh_shader;
    case Need::AccelerationStructure: return f.acceleration_structure;
    case Need::RayQuery: return f.ray_query;
    case Need::ClusterAccelerationStructure: return f.cluster_acceleration_structure;
    case Need::GeometryShader: return f.geometry_shader;
    case Need::FullDrawIndex: return f.full_draw_index_uint32;
  }
  return false;
}

// "<adapter> has no <row>, <row>" for what the device lacks of `needs`; empty when it has them.
// Under a forced profile it says so, since the adapter's name is then the GPU's and not the
// device's.
inline std::string lacking(const gfx::Device& device, std::initializer_list<Need> needs) {
  std::string rows;
  for (const Need need : needs) {
    if (has(device.features(), need)) continue;
    if (!rows.empty()) rows += ", ";
    rows += need_row(need);
  }
  if (rows.empty()) return rows;
  std::string why = std::string(device.adapter().name) + " has no " + rows;
  if (!device_profile().empty()) why += " (ENGINE_GFX_TEST_DEVICE=" + device_profile() + ")";
  return why;
}

// True when the device has every one of `needs`. Otherwise the case records `skipped: <adapter>
// has no <rows>`, the device is destroyed, and this returns false for the case to return.
inline bool require(gfx::Device& device, std::initializer_list<Need> needs) {
  const std::string why = lacking(device, needs);
  if (why.empty()) return true;
  MESSAGE("skipped: " << why);
  device.destroy();
  return false;
}

// For a half of a case that needs more than the rest of it: true when the device has `needs`,
// otherwise records `part skipped: <what>: <adapter> has no <rows>` and returns false.
inline bool part(const gfx::Device& device, const char* what, std::initializer_list<Need> needs) {
  const std::string why = lacking(device, needs);
  if (why.empty()) return true;
  MESSAGE("part skipped: " << std::string(what) << ": " << why);
  return false;
}

// ---- the raster path ----------------------------------------------------------------------

enum class RasterPath : u8 { Mesh, Vertex };

// ADR-0024's rule, the one `renderer::resolve_settings` applies to `--raster hw`: the mesh path
// where the device has mesh shaders, the vertex path where it does not.
inline RasterPath raster_path(const gfx::DeviceFeatures& features) noexcept {
  return features.mesh_shader ? RasterPath::Mesh : RasterPath::Vertex;
}

inline const char* raster_path_name(RasterPath path) noexcept {
  return path == RasterPath::Mesh ? "mesh" : "vertex";
}

// The hardware cluster rasterizer, whichever path draws: its pipeline with `fs_visibility` (the
// fragment entry the two shaders share, so both fill the same 64-bit visibility buffer), the
// direct and the indirect draw, and the two things a cull pass feeding it has to know — which
// word of the argument block counts survivors, and what the rest of the block holds.
class ClusterRaster {
 public:
  // The path the device draws with. `triangles_per_cluster` is the cluster build's triangle
  // capacity (`ClusterBuildOptions::max_triangles`, `ClusterLodOptions::max_triangles`): the
  // vertex path draws three vertices of it per cluster and collapses the ones past the cluster's
  // own count; the mesh path ignores it.
  bool create(const gfx::Device& device, gfx::PipelineLayoutHandle layout,
              u32 triangles_per_cluster, std::string* error) {
    return create(device, raster_path(device.features()), layout, triangles_per_cluster, error);
  }

  // A named path, for a case that compares the two. `depth_format` other than undefined builds
  // the same geometry stage **depth-only** — no fragment stage, depth tested and written
  // greater-or-equal (reversed) — which is how the renderer draws a shadow map's cascades.
  bool create(const gfx::Device& device, RasterPath path, gfx::PipelineLayoutHandle layout,
              u32 triangles_per_cluster, std::string* error,
              gfx::Format depth_format = gfx::Format::Undefined) {
    path_ = path;
    triangles_per_cluster_ = triangles_per_cluster;
    const bool depth_only = depth_format != gfx::Format::Undefined;
    if (path == RasterPath::Mesh) {
      module_ = gfx::create_shader_module(device, shaders::k_cluster_mesh_spirv,
                                          shaders::k_cluster_mesh_spirv_size, error);
      if (!module_.valid()) return false;
      gfx::MeshPipelineDesc desc;
      desc.mesh = module_;
      desc.fragment = depth_only ? gfx::ShaderModuleHandle{} : module_;
      desc.fragment_entry = "fs_visibility";
      desc.layout = layout;
      desc.depth_format = depth_format;
      desc.depth_test = depth_only;
      desc.depth_write = depth_only;
      return gfx::create_mesh_pipeline(device, desc, pipeline_, error);
    }
    module_ = gfx::create_shader_module(device, shaders::k_cluster_vertex_spirv,
                                        shaders::k_cluster_vertex_spirv_size, error);
    if (!module_.valid()) return false;
    gfx::GraphicsPipelineDesc desc;
    desc.vertex = module_;
    desc.vertex_entry = "vs_cluster";
    desc.fragment = depth_only ? gfx::ShaderModuleHandle{} : module_;
    desc.fragment_entry = "fs_visibility";
    desc.layout = layout;
    desc.depth_format = depth_format;
    desc.depth_test = depth_only;
    desc.depth_write = depth_only;
    return gfx::create_graphics_pipeline(device, desc, pipeline_, error);
  }

  void destroy(const gfx::Device& device) noexcept {
    if (pipeline_.valid()) gfx::destroy_pipeline(device, pipeline_);
    if (module_.valid()) gfx::destroy_shader_module(device, module_);
    pipeline_ = {};
    module_ = {};
  }

  RasterPath path() const noexcept { return path_; }
  const char* name() const noexcept { return raster_path_name(path_); }

  // The access a raster pass declares on what its geometry stage reads: the visible list, the
  // deformed-vertex pool.
  gfx::Access geometry_read() const noexcept {
    return path_ == RasterPath::Mesh ? gfx::Access::MeshRead : gfx::Access::VertexRead;
  }

  // `CullParams::count_index` for a cull pass whose argument block `draw_indirect` reads.
  u32 count_index() const noexcept { return path_ == RasterPath::Mesh ? 0u : 1u; }

  // The survivor count in an argument block read back to the host.
  u32 survivors(const u32* block) const noexcept { return block[count_index()]; }

  // Records the reset of the argument block at `offset` of `args` to what the cull pass counts
  // into: {0, 1, 1} for vkCmdDrawMeshTasksIndirectEXT, {3 x triangles_per_cluster, 0, 0, 0} for
  // vkCmdDrawIndirect. A block is `gfx::k_draw_args_bytes` (16), which holds either.
  void reset_args(gfx::CommandList commands, gfx::BufferHandle args, u64 offset = 0) const {
    if (path_ == RasterPath::Mesh) {
      commands.fill_buffer(args, offset, sizeof(u32), 0);
      commands.fill_buffer(args, offset + sizeof(u32), sizeof(u32) * 2, 1);
    } else {
      commands.fill_buffer(args, offset, sizeof(u32), triangles_per_cluster_ * 3);
      commands.fill_buffer(args, offset + sizeof(u32), sizeof(u32) * 3, 0);
    }
  }

  // Binds the pipeline and the bindless set, pushes `params`, and draws `entries` entries of
  // `params.visible` (clusters 0..entries-1 when it is null): one mesh workgroup, or one vertex
  // instance, per entry.
  void draw(gfx::CommandList commands, const gfx::BindlessSet& bindless,
            gfx::ClusterDrawParams params, u32 entries) const {
    bind(commands, bindless, params);
    if (path_ == RasterPath::Mesh) {
      commands.draw_mesh_tasks(entries, 1, 1);
    } else {
      commands.draw(triangles_per_cluster_ * 3, entries, 0, 0);
    }
  }

  // The same, with the entry count the cull pass wrote into the block at `offset` of `args`. On
  // the vertex path this is the **capacity** draw, which is what the renderer draws with culling
  // off or without geometryShader; its culled draw elsewhere is indexed (gfx::VertexDrawHeader),
  // and vertex_path_tests.cpp holds the two to one picture, so a case here about the cut or the
  // picture is answered the same by either.
  void draw_indirect(gfx::CommandList commands, const gfx::BindlessSet& bindless,
                     gfx::ClusterDrawParams params, gfx::BufferHandle args, u64 offset = 0) const {
    bind(commands, bindless, params);
    if (path_ == RasterPath::Mesh) {
      commands.draw_mesh_tasks_indirect(args, offset, 1, sizeof(u32) * 3);
    } else {
      commands.draw_indirect(args, offset, 1, sizeof(u32) * 4);
    }
  }

 private:
  void bind(gfx::CommandList commands, const gfx::BindlessSet& bindless,
            gfx::ClusterDrawParams& params) const {
    commands.bind_pipeline(gfx::BindPoint::Graphics, pipeline_);
    bindless.bind(commands, gfx::BindPoint::Graphics);
    commands.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(params),
                            &params);
  }

  gfx::ShaderModuleHandle module_ = {};
  gfx::PipelineHandle pipeline_ = {};
  RasterPath path_ = RasterPath::Vertex;
  u32 triangles_per_cluster_ = 0;
};

}  // namespace engine::gfx_test
