#pragma once

// Pipelines created in parallel, and what each one cost (docs/subsystems/gfx.md, "Pipelines
// created in parallel"; roadmap R149).
//
// **A cold pipeline is the driver compiling its shaders, and the driver does it on the thread that
// asks.** A small scene's set took 75 to 98 s on the RTX 5090 with every cache cold, CPU seconds
// equal to wall seconds, while thirty-odd threads idled (T117). Vulkan lets several threads create
// pipelines at once against one `VkPipelineCache` unless the cache was made with
// `VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`, which the device's is not (gfx.md, "The
// pipeline cache"), so the set can compile side by side.
//
// **A batch leaves every call site as it reads.** Between `PipelineBatch`'s construction and its
// `end`, a `create_compute_pipeline`, `create_graphics_pipeline` or `create_mesh_pipeline` made *on
// the thread that opened the batch* records its request — the description copied, its entry points
// owned, the caller's out-parameter kept — and returns true at once (a compute pipeline's layout is
// made then and is in the out-parameter already; the pipeline handle stays null until the end).
// `end` compiles the recorded set on a pool of `gfx.pipelines.threads` threads, the largest SPIR-V
// first, fills every out-parameter, and reports the first failure in the order the requests were
// made, naming its shader. Nothing may *use* a pipeline of the batch before its end. Creations on
// other threads, and every creation outside a batch, are made at once, as before. A batch opened
// while one is open on the same thread nests into it, and the outermost end compiles. A batch that
// goes out of scope without `end` (an early return on an error) compiles nothing: its pipelines
// stay null and the layouts already made are the caller's to destroy, as on any failure.
//
// **Every creation is timed**, batched or not, and kept on the device with its module's name (the
// shader library names the modules it makes), its entry points and its SPIR-V size, so a cold start
// says where it went: `pipeline_creations`, the log line "pipeline created", and engine-view's
// summary (`load.pipelines`).

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/gfx/device.h>
#include <domain/gfx/rhi.h>

#include <string>
#include <string_view>

namespace engine::gfx {

enum class PipelineKind : u8 { Compute, Graphics, Mesh };

// One pipeline the device made, and what it cost.
struct PipelineCreation {
  std::string shader;  // the module's name ("" for a module no library named)
  std::string entry;   // its entry points, "vs/fs" for a graphics pipeline
  PipelineKind kind = PipelineKind::Compute;
  u64 spirv_bytes = 0;  // of the modules it was made from
  f64 seconds = 0.0;    // wall time inside the driver's create call
  bool batched = false;
  bool ok = true;
};

// One batch's end: how many it compiled, on how many threads, the wall time of the whole and the
// sum of its pipelines' own times (their ratio is what the threads bought).
struct PipelineBatchRecord {
  u32 count = 0;
  u32 threads = 0;
  f64 wall_seconds = 0.0;
  f64 sum_seconds = 0.0;
  f64 largest_seconds = 0.0;  // the one pipeline no width can go under
};

class PipelineBatch {
 public:
  explicit PipelineBatch(const Device& device) noexcept;
  ~PipelineBatch();
  ENGINE_NON_COPYABLE(PipelineBatch);

  // Compiles what the batch recorded and fills the out-parameters. False with the first failure's
  // shader, entry and result in `error`. A nested batch's end returns true and leaves the work to
  // the outermost one. Calling it twice is calling it once.
  bool end(std::string* error = nullptr);

 private:
  const Device* device_ = nullptr;
  bool open_ = false;
};

// The library's name for a module `create_shader_module` made, for the timings (shader_library.cpp
// names every module it makes; a module nobody names reports as "").
void name_shader_module(const Device& device, ShaderModuleHandle module, std::string_view name);

// Every creation since the device was made, in the order they finished, and every batch.
void pipeline_creations(const Device& device, Vector<PipelineCreation>& out);
void pipeline_batches(const Device& device, Vector<PipelineBatchRecord>& out);

// The summary block (apps.md, "The summary's load block"): the count, the sum of the creations'
// seconds, every batch, and every creation sorted slowest first.
JsonValue pipeline_creations_json(const Device& device);

const char* pipeline_kind_name(PipelineKind kind) noexcept;

}  // namespace engine::gfx
