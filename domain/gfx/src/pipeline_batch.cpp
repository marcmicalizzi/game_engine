// Pipelines created in parallel, and every creation's time (pipeline_batch.h; gfx.md, "Pipelines
// created in parallel"). The Vulkan calls themselves are device.cpp's (`make_pipeline`).
#include "pipeline_recorder.h"

#include <core/jobs/job_system.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/gfx/pipeline_batch.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <atomic>
#include <utility>

namespace engine::gfx {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_pipelines, "gfx.pipelines");

// The batch's width (gfx.md, "Pipelines created in parallel"): read once at each batch's end.
tunables::Int pipeline_threads{
    "gfx.pipelines.threads", 0, 0, 256,
    "Threads a pipeline batch compiles on, read at the batch's end; 0 is automatic (one per "
    "performance CPU but the caller's, capped at gfx.pipelines.max_threads), 1 compiles the batch "
    "on the calling thread in the order it was recorded"};
tunables::Int pipeline_max_threads{
    "gfx.pipelines.max_threads", 8, 1, 256,
    "The automatic width's ceiling: each concurrent compile of a large shader holds the driver's "
    "own working memory, and the measured gain flattens past it (R149)"};

detail::PipelineRecorder* recorder_of(const Device& device) noexcept {
  return device.valid() ? device.handles().pipelines : nullptr;
}

f64 seconds_of(const detail::PipelineRequest& r) noexcept {
  return static_cast<f64>(r.end_ns - r.begin_ns) * 1.0e-9;
}

// "name" for one module, "a+b" for two different ones; the bytes summed over distinct modules.
void add_module(detail::PipelineRecorder& recorder, ShaderModuleHandle module,
                ShaderModuleHandle& seen, std::string& names, u64& bytes) {
  if (!module || module == seen) return;
  seen = module;
  for (const detail::ModuleName& m : recorder.modules) {
    if (m.module != module.bits()) continue;
    if (!names.empty()) names += '+';
    names += m.name.empty() ? std::string("?") : m.name;
    bytes += m.spirv_bytes;
    return;
  }
  if (!names.empty()) names += '+';
  names += '?';
}

void append_entry(std::string& out, const std::string& entry, bool present) {
  if (!present) return;
  if (!out.empty()) out += '/';
  out += entry;
}

}  // namespace

const char* pipeline_kind_name(PipelineKind kind) noexcept {
  switch (kind) {
    case PipelineKind::Compute: return "compute";
    case PipelineKind::Graphics: return "graphics";
    case PipelineKind::Mesh: return "mesh";
  }
  return "?";
}

namespace detail {

void register_module(PipelineRecorder& recorder, ShaderModuleHandle module, u64 spirv_bytes) {
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  for (ModuleName& m : recorder.modules) {
    if (m.module != module.bits()) continue;
    m.spirv_bytes = spirv_bytes;
    m.name.clear();
    return;
  }
  ModuleName m;
  m.module = module.bits();
  m.spirv_bytes = spirv_bytes;
  recorder.modules.push_back(std::move(m));
}

void forget_module(PipelineRecorder& recorder, ShaderModuleHandle module) {
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  for (u32 i = 0; i < recorder.modules.size(); ++i) {
    if (recorder.modules[i].module != module.bits()) continue;
    recorder.modules[i] = std::move(recorder.modules[recorder.modules.size() - 1]);
    recorder.modules.pop_back();
    return;
  }
}

void describe_request(PipelineRecorder& recorder, PipelineRequest& request) {
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  ShaderModuleHandle seen;
  request.shader.clear();
  request.entry.clear();
  request.spirv_bytes = 0;
  switch (request.kind) {
    case PipelineKind::Compute:
      add_module(recorder, request.module, seen, request.shader, request.spirv_bytes);
      append_entry(request.entry, request.entries[0], true);
      break;
    case PipelineKind::Graphics:
      add_module(recorder, request.graphics.vertex, seen, request.shader, request.spirv_bytes);
      add_module(recorder, request.graphics.fragment, seen, request.shader, request.spirv_bytes);
      append_entry(request.entry, request.entries[0], true);
      append_entry(request.entry, request.entries[1], request.graphics.fragment.valid());
      break;
    case PipelineKind::Mesh:
      add_module(recorder, request.mesh.task, seen, request.shader, request.spirv_bytes);
      add_module(recorder, request.mesh.mesh, seen, request.shader, request.spirv_bytes);
      add_module(recorder, request.mesh.fragment, seen, request.shader, request.spirv_bytes);
      append_entry(request.entry, request.entries[0], request.mesh.task.valid());
      append_entry(request.entry, request.entries[1], true);
      append_entry(request.entry, request.entries[2], request.mesh.fragment.valid());
      break;
  }
}

bool record_if_batched(PipelineRecorder& recorder, PipelineRequest& request) {
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  if (recorder.depth == 0 || recorder.owner != std::this_thread::get_id()) return false;
  // The handle is null until the batch's end fills it; a compute layout is the caller's already.
  if (request.out != nullptr) *request.out = {};
  if (request.compute_out != nullptr) request.compute_out->pipeline = {};
  recorder.requests.push_back(std::move(request));
  return true;
}

void record_creation(PipelineRecorder& recorder, const PipelineRequest& made, bool batched) {
  PipelineCreation c;
  c.shader = made.shader;
  c.entry = made.entry;
  c.kind = made.kind;
  c.spirv_bytes = made.spirv_bytes;
  c.seconds = seconds_of(made);
  c.batched = batched;
  c.ok = made.result == VK_SUCCESS;
  ENGINE_LOG_INFO(log_pipelines, "pipeline created", log::field("shader", c.shader),
                  log::field("entry", c.entry), log::field("kind", pipeline_kind_name(c.kind)),
                  log::field("spirv_bytes", c.spirv_bytes), log::field("ms", c.seconds * 1.0e3),
                  log::field("batched", batched), log::field("ok", c.ok));
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  recorder.creations.push_back(std::move(c));
}

}  // namespace detail

PipelineBatch::PipelineBatch(const Device& device) noexcept : device_(&device) {
  detail::PipelineRecorder* recorder = recorder_of(device);
  if (recorder == nullptr) return;
  const std::lock_guard<std::mutex> lock(recorder->mutex);
  const std::thread::id self = std::this_thread::get_id();
  // Another thread's batch is open: this one records nothing, and its creations are made at once.
  if (recorder->depth > 0 && recorder->owner != self) return;
  recorder->owner = self;
  ++recorder->depth;
  open_ = true;
}

// Without `end`, nothing is compiled: the caller is returning an error, and every out-parameter
// already reads as not made (a compute layout made at record time is the caller's to destroy).
PipelineBatch::~PipelineBatch() {
  if (!open_) return;
  detail::PipelineRecorder* recorder = recorder_of(*device_);
  if (recorder == nullptr) return;
  const std::lock_guard<std::mutex> lock(recorder->mutex);
  if (--recorder->depth == 0) {
    recorder->requests.clear();
    recorder->owner = std::thread::id();
  }
}

bool PipelineBatch::end(std::string* error) {
  if (!open_) return true;
  open_ = false;
  detail::PipelineRecorder* recorder = recorder_of(*device_);
  if (recorder == nullptr) return true;
  Vector<detail::PipelineRequest> requests;
  {
    const std::lock_guard<std::mutex> lock(recorder->mutex);
    if (--recorder->depth > 0) return true;  // nested: the outermost end compiles
    requests = std::move(recorder->requests);
    recorder->requests = Vector<detail::PipelineRequest>();
    recorder->owner = std::thread::id();
  }
  const u32 count = requests.size();
  if (count == 0) return true;
  // Largest first, so the longest compile starts at once and the rest fill the other threads
  // around it; the record order breaks ties, so the order is a function of the batch alone.
  Vector<u32> order(count);
  for (u32 i = 0; i < count; ++i)
    order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](u32 a, u32 b) { return requests[a].spirv_bytes > requests[b].spirv_bytes; });
  const u32 asked = static_cast<u32>(pipeline_threads.get());
  const u32 ceiling = static_cast<u32>(pipeline_max_threads.get());
  const Device& device = *device_;
  auto compile = [&](detail::PipelineRequest& r) {
    r.begin_ns = time::monotonic_ns();
    r.result = detail::make_pipeline(device, r);
    r.end_ns = time::monotonic_ns();
    detail::record_creation(*recorder, r, true);
  };
  const i64 start = time::monotonic_ns();
  // Automatic: the CPUs but the caller's, at most the ceiling; never more threads than requests.
  const u32 cpus = std::thread::hardware_concurrency();
  u32 threads = asked != 0 ? asked : std::min(ceiling, cpus > 1 ? cpus - 1 : 1u);
  if (threads > count) threads = count;
  if (threads <= 1) {
    threads = 1;
    for (u32 i = 0; i < count; ++i)
      compile(requests[i]);
  } else {
    jobs::JobSystemConfig config;
    config.performance_workers = threads;
    config.efficiency_workers = 1;
    config.pin_threads = false;
    jobs::JobSystem pool(config);
    std::atomic<u32> next{0};
    // One lane a thread, each taking the next request in size order until none is left.
    pool.parallel_for(jobs::Pool::Performance, threads, 1, [&](u32, u32) {
      for (;;) {
        const u32 i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= count) return;
        compile(requests[order[i]]);
      }
    });
  }
  PipelineBatchRecord record;
  record.count = count;
  record.threads = threads;
  record.wall_seconds = static_cast<f64>(time::monotonic_ns() - start) * 1.0e-9;
  for (const detail::PipelineRequest& r : requests) {
    const f64 s = seconds_of(r);
    record.sum_seconds += s;
    if (s > record.largest_seconds) record.largest_seconds = s;
  }
  ENGINE_LOG_INFO(log_pipelines, "pipeline batch", log::field("count", count),
                  log::field("threads", threads),
                  log::field("wall_ms", record.wall_seconds * 1.0e3),
                  log::field("sum_ms", record.sum_seconds * 1.0e3),
                  log::field("largest_ms", record.largest_seconds * 1.0e3));
  {
    const std::lock_guard<std::mutex> lock(recorder->mutex);
    recorder->batches.push_back(record);
  }
  // Every out-parameter filled in record order, and the first failure in that order reported, so
  // what a caller sees does not depend on which thread finished first.
  bool ok = true;
  for (detail::PipelineRequest& r : requests) {
    std::string why;
    if (detail::finish_request(device, r, ok ? &why : nullptr) || !ok) continue;
    ok = false;
    if (error != nullptr) *error = r.shader + " (" + r.entry + "): " + why;
  }
  return ok;
}

void name_shader_module(const Device& device, ShaderModuleHandle module, std::string_view name) {
  detail::PipelineRecorder* recorder = recorder_of(device);
  if (recorder == nullptr || !module) return;
  const std::lock_guard<std::mutex> lock(recorder->mutex);
  for (detail::ModuleName& m : recorder->modules) {
    if (m.module != module.bits()) continue;
    m.name.assign(name);
    return;
  }
}

void pipeline_creations(const Device& device, Vector<PipelineCreation>& out) {
  out.clear();
  detail::PipelineRecorder* recorder = recorder_of(device);
  if (recorder == nullptr) return;
  const std::lock_guard<std::mutex> lock(recorder->mutex);
  out = recorder->creations;
}

void pipeline_batches(const Device& device, Vector<PipelineBatchRecord>& out) {
  out.clear();
  detail::PipelineRecorder* recorder = recorder_of(device);
  if (recorder == nullptr) return;
  const std::lock_guard<std::mutex> lock(recorder->mutex);
  out = recorder->batches;
}

JsonValue pipeline_creations_json(const Device& device) {
  Vector<PipelineCreation> all;
  Vector<PipelineBatchRecord> batches;
  pipeline_creations(device, all);
  pipeline_batches(device, batches);
  f64 sum = 0.0;
  for (const PipelineCreation& c : all)
    sum += c.seconds;
  JsonValue out = JsonValue::object();
  out.set("count", JsonValue(all.size()));
  out.set("ms", JsonValue(sum * 1.0e3));  // the creations' own times summed, not wall time
  JsonValue batch_list = JsonValue::array();
  for (const PipelineBatchRecord& b : batches) {
    JsonValue one = JsonValue::object();
    one.set("count", JsonValue(b.count));
    one.set("threads", JsonValue(b.threads));
    one.set("wall_ms", JsonValue(b.wall_seconds * 1.0e3));
    one.set("sum_ms", JsonValue(b.sum_seconds * 1.0e3));
    one.set("largest_ms", JsonValue(b.largest_seconds * 1.0e3));
    batch_list.push_back(std::move(one));
  }
  out.set("batches", std::move(batch_list));
  Vector<u32> order(all.size());
  for (u32 i = 0; i < all.size(); ++i)
    order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](u32 a, u32 b) { return all[a].seconds > all[b].seconds; });
  JsonValue slowest = JsonValue::array();
  for (const u32 i : order) {
    const PipelineCreation& c = all[i];
    JsonValue one = JsonValue::object();
    one.set("shader", JsonValue(c.shader));
    one.set("entry", JsonValue(c.entry));
    one.set("kind", JsonValue(pipeline_kind_name(c.kind)));
    one.set("spirv_bytes", JsonValue(c.spirv_bytes));
    one.set("ms", JsonValue(c.seconds * 1.0e3));
    one.set("batched", JsonValue(c.batched));
    one.set("ok", JsonValue(c.ok));
    slowest.push_back(std::move(one));
  }
  out.set("slowest", std::move(slowest));
  return out;
}

}  // namespace engine::gfx
