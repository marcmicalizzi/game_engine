#pragma once

// The device's record of its pipeline creations and its open batch (pipeline_batch.h; gfx.md,
// "Pipelines created in parallel"). Private to gfx: device.cpp's three `create_*_pipeline`
// functions ask it whether to make a pipeline now or record it for the batch, and report every
// creation's time to it; pipeline_batch.cpp compiles a batch and reads the record back.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/pipeline_batch.h>

#include <mutex>
#include <string>
#include <thread>

namespace engine::gfx::detail {

// One pipeline to make: the description copied, the entry points owned (a caller's `const char*`
// need not outlive a batch), and where the result goes.
struct PipelineRequest {
  PipelineKind kind = PipelineKind::Compute;
  ShaderModuleHandle module;      // compute
  PipelineLayoutHandle layout;    // compute: made before the request, already in `compute_out`
  GraphicsPipelineDesc graphics;  // graphics
  MeshPipelineDesc mesh;          // mesh
  // compute: [0]; graphics: vertex, fragment; mesh: task, mesh, fragment
  std::string entries[3];
  ComputePipeline* compute_out = nullptr;  // compute
  PipelineHandle* out = nullptr;           // graphics and mesh
  std::string shader;                      // the modules' names, for the report
  std::string entry;                       // the entry points, for the report
  u64 spirv_bytes = 0;
  VkResult result = VK_SUCCESS;
  VkPipeline pipeline = VK_NULL_HANDLE;
  i64 begin_ns = 0;
  i64 end_ns = 0;
};

struct ModuleName {
  u64 module = 0;  // the handle's bits
  u64 spirv_bytes = 0;
  std::string name;
};

struct PipelineRecorder {
  // Guards everything below: a pipeline may be created on any thread, a batch is one thread's.
  std::mutex mutex;
  Vector<ModuleName> modules;
  Vector<PipelineCreation> creations;
  Vector<PipelineBatchRecord> batches;
  // The open batch: the thread that opened it, how deep it is nested there, what it holds.
  std::thread::id owner;
  u32 depth = 0;
  Vector<PipelineRequest> requests;
};

// pipeline_batch.cpp
void register_module(PipelineRecorder& recorder, ShaderModuleHandle module, u64 spirv_bytes);
void forget_module(PipelineRecorder& recorder, ShaderModuleHandle module);
// Fills the request's `shader`, `entry` and `spirv_bytes` from its modules and entry points.
void describe_request(PipelineRecorder& recorder, PipelineRequest& request);
// Whether the calling thread holds `recorder`'s open batch, and if so records the request.
bool record_if_batched(PipelineRecorder& recorder, PipelineRequest& request);
// One pipeline made, its time added to the record and the log.
void record_creation(PipelineRecorder& recorder, const PipelineRequest& made, bool batched);

// device.cpp: the `vkCreate*Pipelines` call behind each kind, made from a request's copies, and
// the result into the caller's out-parameter (on failure: the error as the unbatched call gives
// it, the out-parameter cleared, a compute pipeline's layout destroyed).
VkResult make_pipeline(const Device& device, PipelineRequest& request);
bool finish_request(const Device& device, PipelineRequest& request, std::string* error);

}  // namespace engine::gfx::detail
