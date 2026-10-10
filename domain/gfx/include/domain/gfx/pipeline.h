#pragma once

// Shader modules and pipelines (docs/subsystems/gfx.md, "Shaders", "Raster passes", "Mesh
// shaders"). SPIR-V comes from the build (`engine_shaders()`) or the shader library; a pipeline
// is created once and bound in a pass body with `CommandList::bind_pipeline`.

#include <core/base/types.h>
#include <domain/gfx/device.h>
#include <domain/gfx/rhi.h>

#include <span>
#include <string>

namespace engine::gfx {

// How a graphics pipeline's colour output meets what the attachment holds. `None` overwrites, what
// every pass of the frame does. `Alpha` is straight-alpha "over" — colour `src * a + dst * (1 -
// a)`, alpha `src + dst * (1 - src)` — which is what an immediate-mode UI's vertices are written
// for: the renderer's overlay pass (systems/renderer, `overlay.h`; ADR-0054) draws with it.
enum class BlendMode : u8 { None, Alpha };

// SPIR-V bytes (4-byte aligned, as the embedded headers provide). Null on failure.
ShaderModuleHandle create_shader_module(const Device& device, const unsigned char* spirv,
                                        usize bytes, std::string* error = nullptr);
void destroy_shader_module(const Device& device, ShaderModuleHandle module) noexcept;

struct ComputePipeline {
  PipelineHandle pipeline;
  PipelineLayoutHandle layout;
};

// One compute entry point over the given descriptor set layouts and a push-constant block of
// `push_constant_bytes` (0 for none).
bool create_compute_pipeline(const Device& device, ShaderModuleHandle module, const char* entry,
                             std::span<const DescriptorSetLayoutHandle> set_layouts,
                             u32 push_constant_bytes, ComputePipeline& out,
                             std::string* error = nullptr);
void destroy_compute_pipeline(const Device& device, ComputePipeline& pipeline) noexcept;

// A graphics pipeline for dynamic rendering: no vertex input (geometry is pulled through device
// addresses or generated), dynamic viewport and scissor, one color attachment, optional depth.
// A null `fragment` builds a **depth-only** pipeline with no fragment stage at all — what a
// shadow map is drawn with: the rasterizer writes depth and nothing is shaded, so the hardware
// never launches a fragment invocation.
struct GraphicsPipelineDesc {
  ShaderModuleHandle vertex;
  const char* vertex_entry = "vs_main";
  ShaderModuleHandle fragment;  // null: depth only, no fragment stage
  const char* fragment_entry = "fs_main";
  PipelineLayoutHandle layout;
  Format color_format = Format::Undefined;
  Format depth_format = Format::Undefined;
  CullMode cull = CullMode::None;
  bool depth_test = false;
  bool depth_write = false;
  // Reversed-Z: greater-or-equal passes (core/math projections produce near = 1).
  CompareOp depth_compare = CompareOp::GreaterOrEqual;
  BlendMode blend = BlendMode::None;
};
bool create_graphics_pipeline(const Device& device, const GraphicsPipelineDesc& desc,
                              PipelineHandle& out, std::string* error = nullptr);
void destroy_pipeline(const Device& device, PipelineHandle pipeline) noexcept;

// A mesh-shader pipeline: optional task stage, mesh stage, fragment stage; no vertex input or
// input assembly state exists for these. Requires DeviceFeatures::mesh_shader.
struct MeshPipelineDesc {
  ShaderModuleHandle task;  // optional
  const char* task_entry = "task_main";
  ShaderModuleHandle mesh;
  const char* mesh_entry = "mesh_main";
  ShaderModuleHandle fragment;  // null: depth only, as GraphicsPipelineDesc
  const char* fragment_entry = "fs_main";
  PipelineLayoutHandle layout;
  Format color_format = Format::Undefined;
  Format depth_format = Format::Undefined;
  CullMode cull = CullMode::None;
  bool depth_test = false;
  bool depth_write = false;
  CompareOp depth_compare = CompareOp::GreaterOrEqual;
};
bool create_mesh_pipeline(const Device& device, const MeshPipelineDesc& desc, PipelineHandle& out,
                          std::string* error = nullptr);

}  // namespace engine::gfx
