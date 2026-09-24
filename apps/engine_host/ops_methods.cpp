// engine-host's operation methods (ops_methods.h; docs/subsystems/protocol.md, "The day-one
// operations"): content.build, session.events, engine.budgets, session.run_headless and
// engine.run_tests.
#include "ops_methods.h"

#include "host_state.h"

#include <core/json/json_value.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <core/platform/process.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/content_build/container_stats.h>
#include <domain/content_build/content_build.h>
#include <domain/doc/document.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/gfx/adapter.h>
#include <domain/gfx/device.h>
#include <domain/protocol/session.h>
#include <domain/sim/scheduler.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/rt_capacity.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The capabilities these methods reach, each compiled in only when this configuration has it
// (CMakeLists.txt defines the switch beside the link). `ENGINE_HOST_ANIMATION` and
// `ENGINE_HOST_AUDIO_SYSTEM` imply `ENGINE_HOST_ECS`, because both capabilities require it.
#if defined(ENGINE_HOST_ECS)
#include <domain/ecs/identity.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/systems.h>
#endif
#if defined(ENGINE_HOST_ANIMATION)
#include <systems/animation/animation.h>
#include <systems/animation/library.h>
#endif
#if defined(ENGINE_HOST_AUDIO)
#include <domain/audio/audio.h>
#endif
#if defined(ENGINE_HOST_AUDIO_SYSTEM)
#include <domain/audio/clip_store.h>
#include <domain/audio/mixer.h>
#include <systems/audio_system/audio_system.h>
#endif
#if defined(ENGINE_HOST_STORE)
#include <foundation/store/database.h>
#include <foundation/store/event_log.h>
#endif
#if defined(ENGINE_HOST_TISSUE)
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>
#endif

namespace engine::host {

// ---- the runtime world -------------------------------------------------------------------------

// One session's world (ops_methods.h). What it is, is what this build can make: a flecs world
// (`ecs::SimWorld`) with every ticking capability the host links installed into it, or — in a
// build without the ECS capability — the engine's own scheduler with nothing registered, which
// still has the fixed step, the tick counter and the game clock `run_headless` reports.
//
// **What it is not is a materialization of the document** (docs/plan/03-data-model.md §3.4). No
// document record becomes an entity: nothing maps an authored record's type to the components it
// would materialize into, `sim::MaterializationHooks` rows exist (the animation capability has one)
// but no driver walks a document's records through them, and nothing writes runtime state back to
// the document. So the world ticks the host's systems over no entities, and the predicate's
// document terms read a document the run does not change. docs/subsystems/protocol.md says so
// under "session.run_headless and the materialization gap".
class RuntimeWorld {
 public:
  explicit RuntimeWorld(std::string session_id);
  ~RuntimeWorld() = default;
  ENGINE_NON_COPYABLE(RuntimeWorld);

  const std::string& session() const noexcept { return session_; }
  void step();
  u64 tick() const noexcept;
  i64 game_time_us() const noexcept;
  i64 us_per_tick() noexcept;
  static constexpr u32 hz() noexcept { return k_hz; }
  const char* kind() const noexcept;
  u32 entities() const noexcept;
  const Vector<std::string>& systems() const noexcept { return systems_; }

#if defined(ENGINE_HOST_AUDIO_SYSTEM)
  const audio::ClipStore& clips() const noexcept { return clips_; }
  const audio::Mixer& mixer() const noexcept { return mixer_; }
#endif

 private:
  // The rate every other fixed step in the engine runs at (engine-view's animated world, the
  // deformation phase), so a second of `run_headless` is the same sixty ticks it is everywhere.
  static constexpr u32 k_hz = 60;

  std::string session_;
  Vector<std::string> systems_;
#if defined(ENGINE_HOST_ECS)
  ecs::SimWorld sim_;
#if defined(ENGINE_HOST_ANIMATION)
  // An empty library: the capability is installed, so its three systems tick, over no instances
  // until something attaches one — which a materialization layer would do.
  animation::Library library_;
  animation::AnimationSystem animation_;
#endif
#if defined(ENGINE_HOST_AUDIO_SYSTEM)
  // The mixer on the null backend: nothing plays, and `step()` pulls zero frames after every tick
  // so the commands the emitter system sends are applied and the ring never fills.
  audio::ClipStore clips_;
  audio::Mixer mixer_;
  audio::AudioSystem audio_;
#endif
#else
  sim::SimScheduler scheduler_;
#endif
};

namespace {

#if defined(ENGINE_HOST_ECS)
ecs::SimWorldConfig world_config(u32 hz) {
  ecs::SimWorldConfig config;
  config.hz = hz;
  return config;
}
#else
sim::SimSchedulerConfig scheduler_config(u32 hz) {
  sim::SimSchedulerConfig config;
  config.hz = hz;
  return config;
}
#endif

}  // namespace

RuntimeWorld::RuntimeWorld(std::string session_id)
    : session_(std::move(session_id))
#if defined(ENGINE_HOST_ECS)
      ,
      sim_(world_config(k_hz))
#if defined(ENGINE_HOST_ANIMATION)
      ,
      animation_(library_)
#endif
#if defined(ENGINE_HOST_AUDIO_SYSTEM)
      ,
      mixer_(clips_),
      audio_(mixer_)
#endif
#else
      ,
      scheduler_(scheduler_config(k_hz))
#endif
{
#if defined(ENGINE_HOST_ECS)
#if defined(ENGINE_HOST_ANIMATION)
  animation_.install(sim_);
#endif
#if defined(ENGINE_HOST_AUDIO_SYSTEM)
  audio_.install(sim_);
#endif
  for (const ecs::RegisteredSystem& system : ecs::systems(sim_.world()).all())
    systems_.push_back(system.desc.name != nullptr ? system.desc.name : "?");
#else
  for (u16 i = 0; i < scheduler_.system_count(); ++i) {
    const char* name = scheduler_.system(i).name;
    systems_.push_back(name != nullptr ? name : "?");
  }
#endif
}

void RuntimeWorld::step() {
#if defined(ENGINE_HOST_ECS)
  sim_.step();
#if defined(ENGINE_HOST_AUDIO_SYSTEM)
  mixer_.render(nullptr, 0);
#endif
#else
  scheduler_.step();
#endif
}

u64 RuntimeWorld::tick() const noexcept {
#if defined(ENGINE_HOST_ECS)
  return sim_.tick().value;
#else
  return scheduler_.tick().value;
#endif
}

i64 RuntimeWorld::game_time_us() const noexcept {
#if defined(ENGINE_HOST_ECS)
  return sim_.game_time().us;
#else
  return scheduler_.game_time().us;
#endif
}

i64 RuntimeWorld::us_per_tick() noexcept {
#if defined(ENGINE_HOST_ECS)
  return sim_.game_clock().us_per_tick();
#else
  return scheduler_.step_size().us;
#endif
}

const char* RuntimeWorld::kind() const noexcept {
#if defined(ENGINE_HOST_ECS)
  return "ecs";
#else
  return "scheduler";
#endif
}

u32 RuntimeWorld::entities() const noexcept {
#if defined(ENGINE_HOST_ECS)
  const ecs::IdentityMap* map = ecs::identity_map_if_present(sim_.world());
  return map != nullptr ? map->size() : 0u;
#else
  return 0;
#endif
}

OpsHost::OpsHost() noexcept = default;
OpsHost::~OpsHost() = default;

RuntimeWorld* OpsHost::world(std::string_view session, const protocol::SessionManager& sessions,
                             bool create) {
  for (u32 i = 0; i < worlds_.size();) {
    bool open = false;
    for (const std::unique_ptr<protocol::Session>& s : sessions.sessions()) {
      if (s->id() == worlds_[i]->session()) open = true;
    }
    if (open) {
      ++i;
    } else {
      worlds_.erase_at(i);
    }
  }
  for (const std::unique_ptr<RuntimeWorld>& w : worlds_) {
    if (w->session() == session) return w.get();
  }
  if (!create) return nullptr;
  worlds_.push_back(std::make_unique<RuntimeWorld>(std::string(session)));
  return worlds_.back().get();
}

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_ops, "host.ops");

HostState* state_of(protocol::Context& ctx) { return static_cast<HostState*>(ctx.app); }

protocol::RpcError invalid(std::string message) {
  return protocol::make_error(protocol::codes::k_invalid_argument, std::move(message));
}

protocol::RpcError io_error(std::string message) {
  return protocol::make_error(protocol::codes::k_io_error, std::move(message));
}

// A capability this build does not have (ADR-0027). 1006 rather than a code of its own: to a
// client it is exactly "this host does not offer that service", which is what 1006 already says,
// and the message names the switch that would.
[[maybe_unused]] protocol::RpcError not_built(std::string message) {
  return protocol::make_error(protocol::codes::k_unavailable, std::move(message));
}

f64 ms_since(i64 start_ns) { return static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6; }

// ---- content.build -----------------------------------------------------------------------------

bool read_mesh_options(const protocol::ContentBuildOptions& in, content_build::MeshOptions& out,
                       protocol::RpcError& error) {
  out = content_build::MeshOptions{};
  out.max_triangles = in.max_triangles;
  out.max_vertices = in.max_vertices;
  if (!content_build::options_in_range(out)) {
    error = invalid("options.max_triangles is 4..256 and options.max_vertices is 1..255");
    return false;
  }
  out.page_bytes = in.page_bytes;
  out.weld = in.weld;
  out.morph = in.morph;
  if (!content_build::read_seam_rule(in.uv_seams, out.uv_seams)) {
    error = invalid("options.uv_seams is none, protect or lock; got '" + in.uv_seams + "'");
    return false;
  }
  if (!content_build::read_seam_rule(in.normal_seams, out.normal_seams)) {
    error = invalid("options.normal_seams is none, protect or lock; got '" + in.normal_seams + "'");
    return false;
  }
  if (!std::isfinite(in.uv_weight) || in.uv_weight < 0.0f || !std::isfinite(in.normal_weight) ||
      in.normal_weight < 0.0f) {
    error = invalid("options.uv_weight and options.normal_weight are finite and not negative");
    return false;
  }
  out.uv_weight = in.uv_weight;
  out.normal_weight = in.normal_weight;
  if (!content_build::read_atlas_mode(in.atlas, out.repack)) {
    error = invalid("options.atlas is keep or repack; got '" + in.atlas + "'");
    return false;
  }
  if (!content_build::read_normal_map_mode(in.atlas_normal_maps, out.atlas.normal_maps)) {
    error = invalid("options.atlas_normal_maps is convert or resample; got '" +
                    in.atlas_normal_maps + "'");
    return false;
  }
  out.atlas.proxy_triangles = in.atlas_proxy;
  if (in.atlas_chart_cost == 0) {
    error = invalid("options.atlas_chart_cost is a positive number of thousandths");
    return false;
  }
  out.atlas.max_chart_cost_milli = in.atlas_chart_cost;
  if (in.atlas_supersample != 1 && in.atlas_supersample != 2 && in.atlas_supersample != 4) {
    error = invalid("options.atlas_supersample is 1, 2 or 4");
    return false;
  }
  out.atlas.supersample = in.atlas_supersample;
  return true;
}

void fill_output(const content_build::MeshTask& task, bool want_stats,
                 protocol::ContentBuildOutput& out) {
  const content_build::BuildResult& r = task.result;
  out.source = task.entry->source;
  out.path = r.path;
  out.cached = r.cached;
  out.source_hash = r.source_hash;
  out.build_key = r.build_key;
  switch (task.state) {
    case content_build::TaskState::Built: {
      out.status = "built";
      out.bytes = r.bytes;
      out.build_ms = r.build_ms;
      out.clusters = r.clusters;
      out.leaf_clusters = r.leaf_clusters;
      out.triangles = r.triangles;
      out.lod_levels = r.lod_levels;
      out.vertices = r.vertices;
      out.pages = r.pages;
      out.materials = r.materials;
      out.images = r.images;
      out.embedded_images = r.embedded_images;
      out.hash = r.hash;
      for (const content_build::Diagnostic& w : r.warnings) {
        protocol::Diagnostic d;
        d.path = w.rule;
        d.message = w.message;
        out.warnings.push_back(std::move(d));
      }
      out.repairs = content_build::repairs_json(r);
      out.atlas = content_build::atlas_json(r, task.entry->options);
      break;
    }
    case content_build::TaskState::Skipped: {
      out.status = "skipped";
      out.bytes = r.bytes;
      break;
    }
    case content_build::TaskState::Failed: {
      out.status = "failed";
      out.rule = task.error.rule;
      out.error = task.error.message;
      return;
    }
  }
  if (!want_stats) return;
  JsonValue summary;
  std::string human;  // the tables engine-content puts on stderr; a protocol client has the JSON
  std::string error;
  if (content_build::container_stats(r.path, summary, human, error)) {
    out.stats = std::move(summary);
  } else {
    protocol::Diagnostic d;
    d.path = "content.stats";
    d.message = std::move(error);
    out.warnings.push_back(std::move(d));
  }
}

bool content_build_method(protocol::Context&, const protocol::ContentBuildParams& params,
                          protocol::ContentBuildResult& out, protocol::RpcError& error) {
  const i64 start_ns = time::monotonic_ns();
  if (params.source.empty() == params.manifest.empty()) {
    error = invalid("give a source mesh or a manifest, and not both");
    return false;
  }
  if (!params.manifest.empty() && !params.output.empty()) {
    error = invalid(
        "output names where a single source's container goes; a manifest's entries "
        "name their own");
    return false;
  }
  // One destination for one build, as engine-content refuses `--cache` with an output file.
  if (!params.source.empty() && params.output.empty() == !params.cache) {
    error = invalid(params.cache ? "a source goes to the cache or to output, not both"
                                 : "a source needs an output path, or cache: true");
    return false;
  }
  if (params.jobs > 4096) {
    error = invalid("jobs is 1..4096, or 0 for one per performance CPU");
    return false;
  }
  content_build::MeshOptions options;
  if (!read_mesh_options(params.options, options, error)) return false;

  Vector<content_build::ManifestEntry> entries;
  if (!params.manifest.empty()) {
    if (!io::exists(params.manifest)) {
      error = io_error("no manifest at '" + params.manifest + "'");
      return false;
    }
    std::string message;
    if (!content_build::read_manifest(params.manifest, options, entries, message)) {
      error = invalid(std::move(message));
      return false;
    }
    for (const content_build::ManifestEntry& entry : entries) {
      if (entry.output.empty() && !params.cache) {
        error = invalid("manifest '" + params.manifest + "' entry '" + entry.source +
                        "' has no \"output\" and cache was not asked for");
        return false;
      }
    }
  } else {
    content_build::ManifestEntry entry;
    entry.source = params.source;
    entry.output = params.output;
    entry.options = options;
    entries.push_back(std::move(entry));
  }

  // The derived-data root: the cache's, and the texture step's. A named `ddc` is one even with no
  // output in the cache, as engine-content's `--ddc` is; `cache` with none named finds the
  // repository's. With neither, the call touches nothing but the outputs it names.
  bool to_cache = false;
  for (const content_build::ManifestEntry& entry : entries)
    to_cache = to_cache || entry.output.empty();
  std::string ddc = params.ddc;
  if (to_cache && ddc.empty()) {
    ddc = geometry::find_ddc_root(platform::executable_directory());
    if (ddc.empty()) {
      error = invalid("cache found no repository root above the host executable; pass ddc");
      return false;
    }
  }
  if (to_cache) {
    const std::string dir = io::join_path(ddc, "clusters");
    const io::Status status = io::make_directories(dir);
    if (status != io::Status::Ok) {
      error =
          io_error("cannot create the cache directory '" + dir + "': " + io::status_name(status));
      return false;
    }
  }
  const bool textures = params.textures && !ddc.empty();

  Vector<content_build::MeshTask> tasks;
  tasks.reserve(entries.size());
  for (const content_build::ManifestEntry& entry : entries) {
    content_build::MeshTask task;
    task.entry = &entry;
    task.ddc = &ddc;
    task.strict = params.strict;
    task.force = params.force;
    tasks.push_back(std::move(task));
  }
  Vector<content_build::TextureResult> texture_results;
  {
    jobs::JobSystem pool(content_build::job_config(params.jobs));
    if (params.manifest.empty()) {
      // One mesh: its primitives are the parallelism, as in `engine-content build`.
      tasks[0].pool = &pool;
      content_build::run_mesh_task(&tasks[0]);
    } else {
      // A manifest: one job per mesh, as in `build-all`.
      content_build::run_mesh_tasks(std::span<content_build::MeshTask>(tasks.data(), tasks.size()),
                                    pool);
    }
    // The texture step once every container is settled, as `build-all` runs it: in manifest order
    // and then image order, a texture two meshes share built once, and a mesh that was skipped
    // still has its textures checked, from its container.
    if (textures) {
      Vector<Vector<content_build::TextureSource>> from_containers(tasks.size());
      Vector<const content_build::TextureSource*> sources;
      Vector<u32> mesh_of;
      for (u32 m = 0; m < tasks.size(); ++m) {
        const content_build::MeshTask& task = tasks[m];
        const Vector<content_build::TextureSource>* list = &task.result.texture_sources;
        if (task.state == content_build::TaskState::Skipped) {
          content_build::texture_sources_of_container(task.result.path, task.entry->source,
                                                      from_containers[m]);
          list = &from_containers[m];
        } else if (task.state != content_build::TaskState::Built) {
          continue;
        }
        for (const content_build::TextureSource& source : *list) {
          sources.push_back(&source);
          mesh_of.push_back(m);
        }
      }
      content_build::run_texture_step(
          std::span<const content_build::TextureSource* const>(sources.data(), sources.size()),
          std::span<const u32>(mesh_of.data(), mesh_of.size()), ddc, pool, texture_results);
    }
  }

  for (u32 m = 0; m < tasks.size(); ++m) {
    const content_build::MeshTask& task = tasks[m];
    for (const content_build::Diagnostic& w : task.result.warnings) {
      ENGINE_LOG_WARN(log_ops, "validation warning", log::field("source", task.entry->source),
                      log::field("rule", w.rule), log::field("detail", w.message));
    }
    protocol::ContentBuildOutput output;
    fill_output(task, params.stats, output);
    output.textures = content_build::textures_json(texture_results, m, textures, true);
    switch (task.state) {
      case content_build::TaskState::Built: ++out.built; break;
      case content_build::TaskState::Skipped: ++out.skipped; break;
      case content_build::TaskState::Failed: ++out.failed; break;
    }
    out.outputs.push_back(std::move(output));
  }
  for (const content_build::TextureResult& r : texture_results) {
    if (r.state == content_build::TextureState::Built ||
        r.state == content_build::TextureState::Skipped ||
        r.state == content_build::TextureState::Shared) {
      continue;
    }
    ENGINE_LOG_WARN(log_ops, "texture not built", log::field("image", r.image),
                    log::field("state", content_build::texture_state_name(r.state)),
                    log::field("detail", r.error));
  }
  out.textures = content_build::textures_json(texture_results, ~u32{0}, textures, false);
  out.ddc = ddc;
  out.seconds = ms_since(start_ns) / 1000.0;
  ENGINE_LOG_INFO(log_ops, "content.build", log::field("built", out.built),
                  log::field("skipped", out.skipped), log::field("failed", out.failed),
                  log::field("seconds", out.seconds));
  return true;
}
// ---- session.events ----------------------------------------------------------------------------

constexpr u32 k_events_max_limit = 10000;

// Where a read resumes: the journal's next patch index, and the event log's last consumed place.
// Spelled `j=<index>;s=<tick>:<tile>:<sequence>`, either half absent when that log has not been
// read. Opaque to a client, and parsed strictly here, since a cursor is only ever one this method
// wrote.
struct EventsCursor {
  bool journal = false;
  u64 journal_next = 0;
  bool store = false;
  u64 store_tick = 0;
  u64 store_tile = 0;
  u64 store_sequence = 0;
};

bool parse_u64(std::string_view text, u64& out) {
  if (text.empty() || text.size() > 20) return false;
  u64 value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const u64 digit = static_cast<u64>(c - '0');
    if (value > (~u64{0} - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

bool parse_cursor(std::string_view text, EventsCursor& out) {
  out = EventsCursor{};
  while (!text.empty()) {
    const usize end = text.find(';');
    const std::string_view part = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (part.starts_with("j=")) {
      if (out.journal || !parse_u64(part.substr(2), out.journal_next)) return false;
      out.journal = true;
    } else if (part.starts_with("s=")) {
      if (out.store) return false;
      std::string_view rest = part.substr(2);
      u64* fields[3] = {&out.store_tick, &out.store_tile, &out.store_sequence};
      for (u32 i = 0; i < 3; ++i) {
        const usize colon = rest.find(':');
        if ((i < 2) != (colon != std::string_view::npos)) return false;
        if (!parse_u64(rest.substr(0, colon), *fields[i])) return false;
        rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
      }
      out.store = true;
    } else {
      return false;
    }
  }
  return true;
}

std::string format_cursor(const EventsCursor& c) {
  std::string out;
  if (c.journal) out = "j=" + std::to_string(c.journal_next);
  if (c.store) {
    if (!out.empty()) out.push_back(';');
    out += "s=" + std::to_string(c.store_tick) + ":" + std::to_string(c.store_tile) + ":" +
           std::to_string(c.store_sequence);
  }
  return out;
}

#if defined(ENGINE_HOST_STORE)
// What one scan of the event log feeds the page with. `limit` is the page's room; the first
// matching event past it sets `more` and is not consumed, so the cursor stops in front of it.
struct StoreRead {
  const protocol::SessionEventsParams* params = nullptr;
  Vector<protocol::SessionEvent>* events = nullptr;
  u32 limit = 0;
  u32 rows = 0;
  bool more = false;
  bool consumed_any = false;
  store::LogPosition last;
};

void read_store_event(const store::EventRecord& record, void* user) {
  StoreRead& page = *static_cast<StoreRead*>(user);
  ++page.rows;
  if (page.more) return;
  const store::LogPosition at{record.sim_tick, record.tile, record.sequence};
  const std::string kind = std::to_string(record.type);
  const char* actor = store::origin_name(record.origin);
  const bool matches = (page.params->kind.empty() || page.params->kind == kind) &&
                       (page.params->actor.empty() || page.params->actor == actor);
  if (matches && page.events->size() >= page.limit) {
    page.more = true;
    return;
  }
  page.last = at;
  page.consumed_any = true;
  if (!matches) return;
  protocol::SessionEvent event;
  event.source = "store";
  event.sequence = record.sequence;
  event.tile = record.tile;
  event.tick = record.sim_tick;
  event.game_time_us = record.game_time_us;
  event.kind = kind;
  event.actor = actor;
  event.subject = record.subject;
  event.cause = record.cause;
  event.depth = record.depth;
  event.payload_bytes = static_cast<u32>(record.payload.size());
  page.events->push_back(std::move(event));
}
#endif

bool session_events(protocol::Context& ctx, const protocol::SessionEventsParams& params,
                    protocol::SessionEventsResult& out, protocol::RpcError& error) {
  protocol::Session* session = ctx.sessions->require(params.session, error);
  if (session == nullptr) return false;
  if (params.source != "" && params.source != "journal" && params.source != "store") {
    error = invalid("source is journal, store, or empty for both; got '" + params.source + "'");
    return false;
  }
  if (params.limit == 0 || params.limit > k_events_max_limit) {
    error = invalid("limit is 1..10000");
    return false;
  }
  EventsCursor cursor;
  if (!parse_cursor(params.cursor, cursor)) {
    error = invalid("cursor '" + params.cursor + "' is not one session.events returned");
    return false;
  }
  const bool want_journal = params.source != "store";
  const bool want_store = params.source != "journal";

  // The journal: every committed patch, applied or in the redo tail.
  const doc::Document& document = session->document();
  const Vector<doc::Patch>& journal = document.journal();
  out.journal_total = journal.size();
  out.journal_position = session->undo_position();
  EventsCursor next = cursor;
  if (want_journal) {
    u64 j = cursor.journal ? cursor.journal_next : params.since;
    for (; j < journal.size(); ++j) {
      const doc::Patch& patch = journal[static_cast<u32>(j)];
      const bool matches = (params.kind.empty() || params.kind == "commit") &&
                           (params.actor.empty() || params.actor == patch.attribution.actor);
      if (!matches) continue;
      if (out.events.size() >= params.limit) {
        out.more = true;
        break;
      }
      protocol::SessionEvent event;
      event.source = "journal";
      event.sequence = j;
      event.kind = "commit";
      event.actor = patch.attribution.actor;
      event.attribution = patch.attribution;
      event.layer = patch.layer;
      event.commands = patch.forward.size();
      event.applied = j < session->undo_position();
      out.events.push_back(std::move(event));
    }
    next.journal = true;
    next.journal_next = j;
  }

  // The event log, when there is one to page.
  if (!want_store) {
    out.store_status = "not asked";
  } else {
#if defined(ENGINE_HOST_STORE)
    std::string native;
    const std::string wanted =
        params.store.empty() ? io::join_path(session->dir(), "world.db") : params.store;
    const io::Status resolved = ctx.sessions->vfs().resolve(wanted, native);
    if (resolved != io::Status::Ok) {
      error = io_error("cannot resolve the event log path '" + wanted +
                       "': " + io::status_name(resolved));
      return false;
    }
    if (!io::exists(native)) {
      if (!params.store.empty()) {
        error = io_error("no event log at '" + params.store + "'");
        return false;
      }
      out.store_status = "absent";
    } else {
      out.store_path = native;
      store::Database db;
      store::OpenOptions options;
      options.read_only = true;  // session.events is read-only, and says so to SQLite as well
      options.create = false;
      options.wal = false;  // a read-only connection cannot set a journal mode, and need not
      store::Status status = db.open(native, options);
      store::EventLog event_log(db);
      if (status == store::Status::Ok) status = event_log.open();
      if (status != store::Status::Ok) {
        error = io_error("cannot read the event log '" + native +
                         "': " + store::status_name(status) + " " + std::string(db.last_error()));
        return false;
      }
      out.store_status = "read";
      if (!out.more) {
        StoreRead page;
        page.params = &params;
        page.events = &out.events;
        page.limit = params.limit;
        page.last = store::LogPosition{cursor.store_tick, cursor.store_tile, cursor.store_sequence};
        bool have_last = cursor.store;
        // Filtered-out rows are consumed as they are passed, so a narrow filter over a long log
        // reads it in slices rather than returning a short page that claims to be the end.
        for (;;) {
          const u32 room = params.limit - out.events.size();
          const u32 batch = room + 1 < 256 ? 256 : room + 1;
          page.rows = 0;
          status = event_log.scan(have_last ? &page.last : nullptr, params.since, batch,
                                  &read_store_event, &page);
          if (status != store::Status::Ok) {
            error = io_error("cannot read the event log '" + native +
                             "': " + store::status_name(status));
            return false;
          }
          have_last = have_last || page.consumed_any;
          if (page.more || page.rows < batch) break;
        }
        out.more = out.more || page.more;
        if (have_last) {
          next.store = true;
          next.store_tick = page.last.sim_tick;
          next.store_tile = page.last.tile;
          next.store_sequence = page.last.sequence;
        }
      }
    }
#else
    if (params.source == "store" || !params.store.empty()) {
      error = not_built(
          "this engine-host was built without the store capability (ENGINE_WITH_STORE=OFF or "
          "ENGINE_MINIMAL=ON), so it has no event log to read; the journal is source "
          "\"journal\"");
      return false;
    }
    out.store_status = "not built";
#endif
  }
  out.next = format_cursor(next);
  return true;
}

// ---- engine.budgets ----------------------------------------------------------------------------

constexpr f64 k_mib = 1024.0 * 1024.0;

void add_budget(protocol::BudgetsResult& out, std::string_view filter, const char* name,
                std::string doc, std::optional<f64> limit, std::optional<f64> used,
                std::optional<f64> peak, const char* unit, const char* source, std::string key,
                std::string scope) {
  if (!filter.empty() && !std::string_view(scope).starts_with(filter)) return;
  protocol::Budget b;
  b.name = name;
  b.doc = std::move(doc);
  b.limit = limit;
  b.used = used;
  b.peak = peak;
  b.unit = unit;
  b.source = source;
  b.key = std::move(key);
  b.scope = std::move(scope);
  out.budgets.push_back(std::move(b));
}

bool engine_budgets(protocol::Context& ctx, const protocol::BudgetsParams& params,
                    protocol::BudgetsResult& out, protocol::RpcError&) {
  HostState* state = state_of(ctx);
  const std::string_view filter = params.scope;
  const std::string process = "process";

  // What every scene starts from. The renderer's budgets are per scene, sized when a scene's GPU
  // buffers are built; these are the numbers a scene gets when its settings name none.
  add_budget(out, filter, "renderer.deform_pool",
             "The deformed-vertex pool a scene's deforming instances write their cut into each "
             "frame; a cut past it draws its rest pose.",
             static_cast<f64>(renderer::k_default_deform_pool_kib) * 1024.0, std::nullopt,
             std::nullopt, "bytes", "default", "RenderSettings.deform_pool_kib", process);
  add_budget(out, filter, "renderer.page_budget",
             "Streaming residency: the page bytes a streamed scene keeps on the GPU. No limit by "
             "default, which keeps every page.",
             std::nullopt, std::nullopt, std::nullopt, "bytes", "setting",
             "render.load settings.page_budget_mib", process);
  add_budget(out, filter, "renderer.upload_budget",
             "Streaming uploads: the page payload one frame may copy into a streamed scene's pool.",
             static_cast<f64>(renderer::k_default_upload_budget), std::nullopt, std::nullopt,
             "bytes/frame", "default", "render.load settings.upload_budget_kib", process);
  add_budget(out, filter, "renderer.rt",
             "The device memory the per-frame cluster acceleration structures may take; past it a "
             "frame drops whole instances' structures, shadow casters first.",
             static_cast<f64>(renderer::rt_budget_mib_tunable()) * k_mib, std::nullopt,
             std::nullopt, "bytes", "tunable", "renderer.rt.budget_mib", process);
#if defined(ENGINE_HOST_AUDIO)
  add_budget(out, filter, "audio.clips",
             "Decoded clip memory a clip store may hold; a clip past it is refused, not paged.",
             static_cast<f64>(audio::tunable_clip_budget_bytes()), std::nullopt, std::nullopt,
             "bytes", "tunable", "audio.clip_budget_mb", process);
  add_budget(out, filter, "audio.voices",
             "Voices in a mixer's pool; past it a play steals the lowest priority or is refused.",
             static_cast<f64>(audio::tunable_voices()), std::nullopt, std::nullopt, "voices",
             "tunable", "audio.voices", process);
#endif
  // The allocation counter is a count and not a budget: its rule is "none per frame in steady
  // state", which is measured where a frame loop runs, not in a host between requests.
  const mem::Stats heap = mem::total_stats();
  add_budget(out, filter, "memory.allocations",
             "Heap allocations since the host started. Counted, not bounded here: the frame "
             "loop's steady state allows none, measured where a frame loop runs.",
             std::nullopt, static_cast<f64>(mem::allocation_counter()), std::nullopt, "allocations",
             "counter", "core/memory", process);
  add_budget(out, filter, "memory.heap",
             "Heap bytes the host holds through the engine's allocator.", std::nullopt,
             static_cast<f64>(heap.bytes_current), static_cast<f64>(heap.bytes_peak), "bytes",
             "counter", "core/memory", process);

  // Every loaded scene, with the budgets its buffers were built with and what the last frames used.
  if (state != nullptr) {
    for (const std::unique_ptr<RenderHost::Scene>& scene : state->render.scenes()) {
      if (scene->gpu == nullptr || scene->view == nullptr) continue;
      const std::string scope = "scene:" + scene->id;
      const renderer::Stats& stats = scene->view->stats();
      const renderer::GpuScene& gpu = *scene->gpu;
      if (gpu.deform_pool_bytes() != 0) {
        add_budget(out, filter, "renderer.deform_pool",
                   "This scene's deformed-vertex pool, and the vertices its last frame's cut "
                   "deformed (12 bytes each).",
                   static_cast<f64>(gpu.deform_pool_bytes()),
                   static_cast<f64>(stats.deform_vertices) * 12.0,
                   static_cast<f64>(stats.deform_peak_vertices) * 12.0, "bytes", "setting",
                   "RenderSettings.deform_pool_kib", scope);
      }
      if (gpu.streamed()) {
        const u64 budget =
            gpu.page_budget_bytes() != 0 ? gpu.page_budget_bytes() : stats.stream.page_bytes;
        add_budget(out, filter, "renderer.page_budget",
                   "This scene's residency budget over its page bytes (every page when the "
                   "setting is 0), and the page bytes resident now.",
                   static_cast<f64>(budget), static_cast<f64>(stats.stream.resident_bytes),
                   std::nullopt, "bytes", "setting", "render.load settings.page_budget_mib", scope);
        add_budget(out, filter, "renderer.upload_budget",
                   "The page payload one frame of this scene may copy into its pool.",
                   static_cast<f64>(gpu.upload_budget_bytes()), std::nullopt, std::nullopt,
                   "bytes/frame", "setting", "render.load settings.upload_budget_kib", scope);
      }
      const u32 rt_mib = scene->resolved.settings.rt_budget_mib != 0
                             ? scene->resolved.settings.rt_budget_mib
                             : renderer::rt_budget_mib_tunable();
      add_budget(out, filter, "renderer.rt",
                 "This scene's acceleration-structure budget, and the bytes its per-frame "
                 "structures hold now and at their peak (0 when the frame builds none).",
                 static_cast<f64>(rt_mib) * k_mib, static_cast<f64>(stats.rt.bytes),
                 static_cast<f64>(stats.rt.peak_bytes), "bytes",
                 scene->resolved.settings.rt_budget_mib != 0 ? "setting" : "tunable",
                 scene->resolved.settings.rt_budget_mib != 0 ? "settings.rt_budget_mib"
                                                             : "renderer.rt.budget_mib",
                 scope);
    }

#if defined(ENGINE_HOST_AUDIO_SYSTEM)
    // Every session's runtime world, whose mixer and clip store are the audio budgets in use.
    for (u32 i = 0; i < state->ops.count(); ++i) {
      RuntimeWorld* world = state->ops.at(i);
      const std::string scope = "session:" + world->session();
      add_budget(out, filter, "audio.clips", "This world's clip store and what its clips hold.",
                 static_cast<f64>(world->clips().budget_bytes()),
                 static_cast<f64>(world->clips().resident_bytes()), std::nullopt, "bytes",
                 "tunable", "audio.clip_budget_mb", scope);
      add_budget(out, filter, "audio.voices", "This world's mixer pool and the voices live in it.",
                 static_cast<f64>(world->mixer().voice_count()),
                 static_cast<f64>(world->mixer().live_voices()), std::nullopt, "voices", "tunable",
                 "audio.voices", scope);
    }
#endif
  }

  // Device memory, per GPU. Enumerating the adapters opens no device; the one a render.* call
  // opened also answers VK_EXT_memory_budget's question — what this process may use and is using.
  Vector<gfx::AdapterInfo> adapters;
  if (gfx::enumerate_adapters(adapters, nullptr)) {
    const gfx::Device* device = state != nullptr ? state->render.open_device() : nullptr;
    bool matched = false;
    for (u32 i = 0; i < adapters.size(); ++i) {
      const gfx::AdapterInfo& a = adapters[i];
      gfx::MemoryBudget budget;
      const bool open = device != nullptr && !matched && device->adapter().name == a.name &&
                        device->memory_budget(budget);
      matched = matched || open;
      std::optional<f64> used;
      f64 limit = static_cast<f64>(a.device_local_bytes);
      if (open && budget.valid) {
        limit = static_cast<f64>(budget.budget_bytes);
        used = static_cast<f64>(budget.used_bytes);
      }
      add_budget(out, filter, "gpu.memory",
                 open && budget.valid
                     ? "Device-local memory the driver lets this process use, and what it has "
                       "allocated (VK_EXT_memory_budget)."
                     : "Device-local memory on this GPU; what this process uses is known once a "
                       "render call has opened it on a device with VK_EXT_memory_budget.",
                 limit, used, std::nullopt, "bytes", "device", a.name,
                 "adapter:" + std::to_string(i));
    }
  }
  return true;
}

// ---- session.run_headless ----------------------------------------------------------------------

constexpr f64 k_max_headless_seconds = 3600.0;
constexpr u32 k_max_predicate_depth = 32;
constexpr u32 k_max_predicate_terms = 1024;

// The `until` predicate, parsed once into a flat tree so a tick evaluates nodes rather than
// re-reading JSON.
struct PredicateNode {
  enum class Kind : u8 { All, Any, Not, Equals, Exists, Ticks };
  Kind kind = Kind::All;
  Id128 object;
  std::string property;
  JsonValue value;  // Equals
  bool exists = true;
  u64 ticks = 0;         // Ticks: this call's ticks at least this many
  Vector<u32> children;  // All, Any, Not
};

bool parse_predicate(const JsonValue& json, const std::string& path, u32 depth,
                     Vector<PredicateNode>& nodes, u32& index, std::string& error) {
  if (depth > k_max_predicate_depth) {
    error = path + " nests deeper than " + std::to_string(k_max_predicate_depth) + " terms";
    return false;
  }
  if (nodes.size() >= k_max_predicate_terms) {
    error = "until has more than " + std::to_string(k_max_predicate_terms) + " terms";
    return false;
  }
  if (!json.is_object()) {
    error = path + " is not an object";
    return false;
  }
  index = nodes.size();
  nodes.push_back(PredicateNode{});
  const auto only = [&](std::initializer_list<const char*> keys) {
    for (u32 i = 0; i < json.as_object().size(); ++i) {
      const std::string& key = json.as_object().key_at(i);
      bool known = false;
      for (const char* k : keys)
        known = known || key == k;
      if (!known) {
        error = path + " has '" + key + "', which that term does not take";
        return false;
      }
    }
    return true;
  };
  const auto list = [&](const char* key, PredicateNode::Kind kind) {
    const JsonValue& terms = *json.find(key);
    if (!terms.is_array()) {
      error = path + "." + key + " is not an array of terms";
      return false;
    }
    for (usize i = 0; i < terms.size(); ++i) {
      u32 child = 0;
      if (!parse_predicate(terms[i], path + "." + key + "[" + std::to_string(i) + "]", depth + 1,
                           nodes, child, error)) {
        return false;
      }
      nodes[index].children.push_back(child);
    }
    nodes[index].kind = kind;
    return true;
  };
  if (json.find("all") != nullptr) return only({"all"}) && list("all", PredicateNode::Kind::All);
  if (json.find("any") != nullptr) return only({"any"}) && list("any", PredicateNode::Kind::Any);
  if (const JsonValue* inner = json.find("not"); inner != nullptr) {
    if (!only({"not"})) return false;
    u32 child = 0;
    if (!parse_predicate(*inner, path + ".not", depth + 1, nodes, child, error)) return false;
    nodes[index].kind = PredicateNode::Kind::Not;
    nodes[index].children.push_back(child);
    return true;
  }
  if (const JsonValue* ticks = json.find("ticks_at_least"); ticks != nullptr) {
    if (!only({"ticks_at_least"})) return false;
    u64 n = 0;
    if (!ticks->get_u64(n)) {
      error = path + ".ticks_at_least is not a whole number of ticks";
      return false;
    }
    nodes[index].kind = PredicateNode::Kind::Ticks;
    nodes[index].ticks = n;
    return true;
  }
  if (const JsonValue* seconds = json.find("seconds_at_least"); seconds != nullptr) {
    if (!only({"seconds_at_least"})) return false;
    f64 s = 0.0;
    if (!seconds->get_f64(s) || !std::isfinite(s) || s < 0.0 || s > k_max_headless_seconds) {
      error = path + ".seconds_at_least is a number of seconds, 0..3600";
      return false;
    }
    // Counted in fixed steps, the same way `seconds` is: a second is `hz` ticks exactly.
    nodes[index].kind = PredicateNode::Kind::Ticks;
    nodes[index].ticks = static_cast<u64>(std::llround(s * RuntimeWorld::hz()));
    return true;
  }
  if (const JsonValue* object = json.find("object"); object != nullptr) {
    std::string_view hex;
    if (!object->get_string(hex) || !Id128::from_hex(hex, nodes[index].object)) {
      error = path + ".object is not a 32-digit hex id";
      return false;
    }
    if (const JsonValue* exists = json.find("exists"); exists != nullptr) {
      if (!only({"object", "exists"})) return false;
      if (!exists->get_bool(nodes[index].exists)) {
        error = path + ".exists is not true or false";
        return false;
      }
      nodes[index].kind = PredicateNode::Kind::Exists;
      return true;
    }
    if (!only({"object", "property", "equals"})) return false;
    const JsonValue* property = json.find("property");
    const JsonValue* equals = json.find("equals");
    std::string_view name;
    if (property == nullptr || !property->get_string(name) || name.empty()) {
      error = path + " needs a \"property\" name beside \"object\" (or \"exists\")";
      return false;
    }
    if (equals == nullptr) {
      error = path + " needs \"equals\": the value the property is waited for";
      return false;
    }
    nodes[index].kind = PredicateNode::Kind::Equals;
    nodes[index].property = std::string(name);
    nodes[index].value = *equals;
    return true;
  }
  error = path + " is not a term: all, any, not, object, ticks_at_least or seconds_at_least";
  return false;
}

bool evaluate(const Vector<PredicateNode>& nodes, u32 index, const doc::Document& document,
              u64 ticks) {
  const PredicateNode& node = nodes[index];
  switch (node.kind) {
    case PredicateNode::Kind::All:
      for (const u32 child : node.children) {
        if (!evaluate(nodes, child, document, ticks)) return false;
      }
      return true;
    case PredicateNode::Kind::Any:
      for (const u32 child : node.children) {
        if (evaluate(nodes, child, document, ticks)) return true;
      }
      return false;
    case PredicateNode::Kind::Not: return !evaluate(nodes, node.children[0], document, ticks);
    case PredicateNode::Kind::Exists: return document.exists(node.object) == node.exists;
    case PredicateNode::Kind::Equals: {
      if (!document.exists(node.object)) return false;
      const JsonValue* value = document.property(node.object, node.property);
      return value != nullptr && *value == node.value;
    }
    case PredicateNode::Kind::Ticks: return ticks >= node.ticks;
  }
  return false;
}

bool session_run_headless(protocol::Context& ctx, const protocol::RunHeadlessParams& params,
                          protocol::RunHeadlessResult& out, protocol::RpcError& error) {
  protocol::Session* session = ctx.sessions->require(params.session, error);
  if (session == nullptr) return false;
  HostState* state = state_of(ctx);
  if (state == nullptr) {
    error = not_built("this host has no runtime worlds attached");
    return false;
  }
  if (!std::isfinite(params.seconds) || params.seconds < 0.0 ||
      params.seconds > k_max_headless_seconds) {
    error = invalid(
        "seconds is 0..3600: game seconds at the fixed step, or with until the most "
        "to run before giving up");
    return false;
  }
  Vector<PredicateNode> predicate;
  const bool has_predicate = !params.until.is_null();
  if (has_predicate) {
    u32 root = 0;
    std::string message;
    if (!parse_predicate(params.until, "until", 0, predicate, root, message)) {
      error = invalid(std::move(message));
      return false;
    }
  }

  RuntimeWorld* world = state->ops.world(params.session, *ctx.sessions, true);
  const doc::Document& document = session->document();
  const u64 target = static_cast<u64>(std::llround(params.seconds * RuntimeWorld::hz()));
  u64 ran = 0;
  bool holds = has_predicate && evaluate(predicate, 0, document, 0);
  const i64 start_ns = time::monotonic_ns();
  while (!holds && ran < target) {
    world->step();
    ++ran;
    if (has_predicate) holds = evaluate(predicate, 0, document, ran);
  }
  out.wall_ms = ms_since(start_ns);
  out.ticks = ran;
  out.game_seconds = static_cast<f64>(ran) * static_cast<f64>(world->us_per_tick()) / 1.0e6;
  if (has_predicate) out.predicate = holds;
  out.stopped = holds ? "predicate" : "seconds";
  out.tick = world->tick();
  out.game_time_us = world->game_time_us();
  out.hz = RuntimeWorld::hz();
  out.world = world->kind();
  for (const std::string& name : world->systems())
    out.systems.push_back(name);
  out.entities = world->entities();
  out.materialized = 0;
  ENGINE_LOG_INFO(log_ops, "run_headless", log::field("session", params.session),
                  log::field("ticks", ran), log::field("wall_ms", out.wall_ms),
                  log::field("stopped", out.stopped));
  return true;
}

// ---- engine.run_tests --------------------------------------------------------------------------

void add_check(protocol::RunTestsResult& out, const char* suite, std::string id,
               std::string subject, const char* severity, const char* verdict,
               std::string message) {
  protocol::TestCheck check;
  check.suite = suite;
  check.id = std::move(id);
  check.subject = std::move(subject);
  check.severity = severity;
  check.verdict = verdict;
  check.message = std::move(message);
  out.checks.push_back(std::move(check));
}

void document_checks(protocol::Context& ctx, protocol::Session& session,
                     protocol::RunTestsResult& out) {
  const std::string subject = "session " + session.id() + " (" + session.dir() + ")";
  // doc.validate itself, through the dispatcher, so this check is that method and follows it.
  const protocol::MethodDesc* validate =
      ctx.dispatcher != nullptr ? ctx.dispatcher->find("doc.validate") : nullptr;
  JsonValue params = JsonValue::object();
  params.set("session", JsonValue(session.id()));
  JsonValue result;
  protocol::RpcError rpc_error;
  if (validate == nullptr || !validate->handler(ctx, params, result, rpc_error)) {
    add_check(out, "document", "doc.schema", subject, "error", "fail",
              "doc.validate could not run: " + rpc_error.message);
  } else {
    protocol::ValidateResult typed;
    schema::ReadContext reader;
    (void)schema::from_json(typed, result, reader);
    add_check(out, "document", "doc.schema", subject, "error", typed.ok ? "pass" : "fail",
              typed.ok ? "every record matches the schema registry: types, properties, parents"
                       : std::to_string(typed.diagnostics.size()) +
                             " record(s) do not match the schema registry");
    out.checks.back().diagnostics = std::move(typed.diagnostics);
  }

  // The composed index against a linear recomputation: the invariant every mutation keeps.
  Vector<schema::Diagnostic> index_problems;
  const bool index_ok = session.document().validate_index(&index_problems);
  add_check(out, "document", "doc.index", subject, "error", index_ok ? "pass" : "fail",
            index_ok ? "the composed index agrees with the layers"
                     : std::to_string(index_problems.size()) +
                           " disagreement(s) between the composed index and the layers");
  for (const schema::Diagnostic& d : index_problems) {
    protocol::Diagnostic item;
    item.path = d.path;
    item.message = d.message;
    out.checks.back().diagnostics.push_back(std::move(item));
  }
}

void content_checks(const std::string& path, protocol::RunTestsResult& out) {
  std::string file;
  geometry::ClusterFileHeader header{};
  Vector<geometry::ClusterFileSection> sections;
  geometry::ClusterFileData data;
  std::string error;
  const char* later[] = {"content.cluster_budget", "content.lod", "content.pages",
                         "content.identity", "content.images"};
  if (!content_build::load_container(path, file, header, sections, data, error)) {
    add_check(out, "content", "content.read", path, "error", "fail", error);
    for (const char* id : later)
      add_check(out, "content", id, path, "error", "skipped", "the container could not be read");
    return;
  }
  add_check(out, "content", "content.read", path, "error", "pass",
            "header, section table, content hash and counts as engine-content info reads them");

  const u64 clusters = data.mesh.mesh.clusters.size();
  add_check(out, "content", "content.cluster_budget", path, "error",
            clusters <= content_build::k_max_clusters ? "pass" : "fail",
            std::to_string(clusters) + " clusters; the visibility buffer names at most " +
                std::to_string(content_build::k_max_clusters));

  // The DAG's own invariants. A container does not carry its source's index buffer, so the source
  // triangles are taken back through `vertex_source` from level 0 — which makes "level 0 covers
  // the source once" a check of the mapping rather than of the source, and leaves every other
  // invariant (errors ascending, cuts never empty or growing, spheres, cones, the grid, skin,
  // morph and ids) exactly as the builder's own tests hold it.
  const geometry::ClusterMesh& mesh = data.mesh.mesh;
  Vector<u32> source_indices;
  bool mapped = mesh.vertex_source.size() == mesh.vertices.size() &&
                data.mesh.lod.size() == mesh.clusters.size();
  for (u32 c = 0; mapped && c < mesh.clusters.size(); ++c) {
    if (data.mesh.lod[c].level != 0) continue;
    const geometry::ClusterDesc& desc = mesh.clusters[c];
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const u32 packed = mesh.triangles[desc.triangle_offset + t];
      for (u32 k = 0; k < 3; ++k) {
        const u32 local = geometry::ClusterMesh::unpack(packed, k);
        source_indices.push_back(mesh.vertex_source[desc.vertex_offset + local]);
      }
    }
  }
  std::string lod_error;
  const bool lod_ok =
      mapped && geometry::validate_cluster_lod(data.mesh, source_indices, &lod_error);
  add_check(out, "content", "content.lod", path, "error", lod_ok ? "pass" : "fail",
            lod_ok ? "the LOD DAG's invariants hold"
                   : (mapped ? lod_error : "the vertex source stream does not match the vertices"));

  if (data.pages.pages.empty()) {
    add_check(out, "content", "content.pages", path, "error", "skipped",
              "no page table: built with page_bytes 0");
  } else {
    std::string page_error;
    const bool pages_ok = geometry::validate_cluster_pages(data.mesh, data.pages, &page_error);
    add_check(out, "content", "content.pages", path, "error", pages_ok ? "pass" : "fail",
              pages_ok ? std::to_string(data.pages.pages.size()) +
                             " pages tile the clusters and keep their order"
                       : page_error);
  }

  const bool identity = data.source_hash != 0 && data.build_key != 0;
  add_check(out, "content", "content.identity", path, "warning", identity ? "pass" : "fail",
            identity ? "records the source's hash and the build key"
                     : "records no identity, so an incremental build rebuilds it rather than "
                       "trusting it");

  // Drawable on its own: every image slot carries its bytes or names a file that is there.
  const std::string dir(io::parent_path(data.source_path));
  u32 missing = 0;
  std::string first;
  for (u32 i = 0; i < data.image_paths.size(); ++i) {
    const bool carried = i < data.images.size() && !data.images[i].bytes.empty();
    const std::string& named = data.image_paths[i];
    if (carried || (!named.empty() && io::exists(io::join_path(dir, named)))) continue;
    if (missing++ == 0)
      first = named.empty() ? "image " + std::to_string(i) + " carries no bytes and names no file"
                            : "image '" + named + "' is not there";
  }
  add_check(out, "content", "content.images", path, "warning", missing == 0 ? "pass" : "fail",
            missing == 0
                ? std::to_string(data.image_paths.size()) + " image(s), each carried or found"
                : std::to_string(missing) + " image(s) cannot be drawn: " + first);
}

#if defined(ENGINE_HOST_TISSUE)
bool tissue_checks(const protocol::RunTestsParams& params, protocol::RunTestsResult& out) {
  const std::string& path = params.tissue;
  tissue::TissueFile file;
  std::string error;
  const bool json = path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0;
  const bool loaded = json ? tissue::import_interchange(path, file, &error)
                           : tissue::read_tissue_file(path, file, &error);
  if (!loaded) {
    add_check(out, "tissue", "tissue.read", path, "error", "fail", error);
    return true;
  }
  add_check(out, "tissue", "tissue.read", path, "error", "pass",
            json ? "the interchange's blocks match their sizes and SHA-256s"
                 : "the container's header, sections and blocks read back");
  tissue::ValidateOptions options;
  options.compare_modes = params.tissue_modes;
  tissue::TissueReport report;
  tissue::validate_tissue(file, options, report);
  for (const tissue::ValidationRow& row : report.rows) {
    std::string message = row.note;
    if (!row.threshold.empty())
      message += (message.empty() ? "" : " ") + ("Threshold: " + row.threshold + ".");
    if (!row.witness.empty())
      message += (message.empty() ? "" : " ") + ("Witness: " + row.witness + ".");
    add_check(out, "tissue", row.id, row.subject, tissue::severity_name(row.severity),
              tissue::verdict_name(row.verdict), std::move(message));
    out.checks.back().value = row.value;
  }
  return true;
}
#endif

bool engine_run_tests(protocol::Context& ctx, const protocol::RunTestsParams& params,
                      protocol::RunTestsResult& out, protocol::RpcError& error) {
  protocol::Session* session = nullptr;
  if (!params.session.empty()) {
    session = ctx.sessions->require(params.session, error);
    if (session == nullptr) return false;
  }
#if !defined(ENGINE_HOST_TISSUE)
  if (!params.tissue.empty()) {
    error = not_built(
        "this engine-host was built without the tissue capability (ENGINE_WITH_TISSUE=OFF, "
        "ENGINE_WITH_PHYSICS=OFF, or ENGINE_MINIMAL=ON), so it has no tissue validators");
    return false;
  }
#endif
  if (session != nullptr) document_checks(ctx, *session, out);
#if defined(ENGINE_HOST_TISSUE)
  if (!params.tissue.empty()) tissue_checks(params, out);
#endif
  for (const std::string& container : params.containers)
    content_checks(container, out);

  for (const protocol::TestCheck& check : out.checks) {
    if (check.verdict == "pass") ++out.passed;
    if (check.verdict == "skipped") ++out.skipped;
    if (check.verdict != "fail") continue;
    ++out.failed;
    if (check.severity == "error") ++out.errors;
    if (check.severity == "warning") ++out.warnings;
  }
  out.ok = out.errors == 0;
  return true;
}

}  // namespace

void add_ops_methods(protocol::Dispatcher& d) {
  d.add(protocol::method<protocol::ContentBuildParams, protocol::ContentBuildResult,
                         &content_build_method>(
      "content.build",
      "Build one glTF or GLB file, or a manifest of them, into .clusters containers in the host's "
      "process: engine-content build and build-all, with the derived-data cache, the job pool, "
      "the identity skip, and each output's content-build metrics."));
  d.add(protocol::method<protocol::SessionEventsParams, protocol::SessionEventsResult,
                         &session_events>(
      "session.events",
      "What has happened to a document, a page at a time: its journal's commits with attribution "
      "and, when the host has the store capability and the document a world.db, the world's event "
      "log. Filter by kind and actor; pass next back as cursor to continue or to poll."));
  d.add(protocol::method<protocol::BudgetsParams, protocol::BudgetsResult, &engine_budgets>(
      "engine.budgets",
      "The budgets the engine knows and what uses them: the renderer's deform pool, streaming page "
      "and upload budgets and ray tracing memory (per loaded scene), audio clips and voices, the "
      "allocation counter and heap, and each GPU's memory; name, limit, used, unit and source."));
  d.add(protocol::method<protocol::RunHeadlessParams, protocol::RunHeadlessResult,
                         &session_run_headless>(
      "session.run_headless",
      "Step the session's runtime world at the fixed step with no rendering, for game seconds or "
      "until a predicate over document objects and the run's own clock holds; returns ticks, game "
      "time, wall milliseconds and the predicate's value."));
  d.add(protocol::method<protocol::RunTestsParams, protocol::RunTestsResult, &engine_run_tests>(
      "engine.run_tests",
      "Run the engine's own checks that are safe inside a host — the document validators, the "
      "tissue validators over a tissue file, the content checks over built containers — and "
      "return one structured report. Spawns nothing; ctest is CI's."));
}

}  // namespace engine::host
