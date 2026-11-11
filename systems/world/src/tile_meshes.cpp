#include <core/log/log.h>
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/world/tile_meshes.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_meshes, "world.meshes");

// **How long a mesh no tile names stays** (world.md, "A tile's meshes: residency by reference").
// The ring keeps a tile through a band of its radius (`WorldRings.hysteresis`, 15 %), which is what
// stops a tile flipping at a border; a walker who turns back from farther than that would rebuild
// and re-upload every mesh the tiles behind them named. 120 commits is two seconds of a moving
// camera at 60 Hz. A lingering mesh never holds room against one a tile wants: it is the first
// thing let go.
tunables::Int meshes_linger{
    "world.meshes.linger", 120, 0, 1 << 20,
    "Commits a tile's mesh stays resident after the last tile that named it went, unless its room "
    "is wanted first (0: it goes with its last tile)"};

// **What a budgeted commit may admit** (renderer.md, "Meshes that come and go", what an admission
// costs): an admission stages the mesh's streams on the committing thread — a copy of its bytes —
// and the next frame copies them on the device. Two meshes and 4 MiB a commit keeps that under a
// millisecond of the frame's thread for the solvers' tile meshes (tens to a few hundred KiB); the
// rest wait a commit, drawn as their tile drew before.
tunables::Int meshes_admissions{
    "world.meshes.admissions", 2, 0, 1 << 16,
    "Meshes that arrive with their tiles a budgeted commit admits, nearest tile first; the rest "
    "wait for a later commit. 0 is unlimited"};

tunables::Int meshes_admission_kib{
    "world.meshes.admission_kib", 4'096, 0, 1 << 22,
    "KiB of meshes a budgeted commit stages for the device, at most (a mesh larger than all of it "
    "goes alone). 0 is unlimited"};

}  // namespace

u32 mesh_linger_tunable() noexcept { return static_cast<u32>(meshes_linger.get()); }
u32 mesh_admissions_tunable() noexcept { return static_cast<u32>(meshes_admissions.get()); }
u64 mesh_admission_bytes_tunable() noexcept {
  return static_cast<u64>(meshes_admission_kib.get()) * 1024;
}

// One mesh some tile names, or named: its slot is its index. The job that builds it touches
// `mesh`, `data`, `error`, `build_ns` and `state`, and nothing else; the committing thread reads
// them only once `ran` is done.
struct TileMeshes::Entry {
  TileMeshes* owner = nullptr;
  u64 identity = 0;
  scene_gen::PlacementMesh mesh;  // its path resolved
  u32 refs = 0;
  std::atomic<State> state{State::building};
  bool collected = false;            // its build's result has been counted
  renderer::PlacementMeshData data;  // until admitted
  std::string error;
  i64 build_ns = 0;
  u32 clusters = 0;
  u32 materials = 0;
  u64 payload = 0;
  u64 unnamed_since = 0;  // the commit its last reference went at
  jobs::Counter ran;

  void build() noexcept {
    const i64 start = time::monotonic_ns();
    const bool ok = renderer::load_placement_mesh(mesh, owner->options_, data, error);
    build_ns = time::monotonic_ns() - start;
    state.store(ok ? State::built : State::failed, std::memory_order_release);
  }
};

TileMeshes::TileMeshes() = default;

TileMeshes::~TileMeshes() {
  for (const std::unique_ptr<Entry>& entry : entries_) {
    if (entry != nullptr) entry->ran.wait_blocking();
  }
}

void TileMeshes::run(void* data) noexcept { static_cast<Entry*>(data)->build(); }

void TileMeshes::create(const renderer::PlacementMeshOptions& options, const TileMeshConfig& config,
                        TileMeshSink sink, jobs::JobSystem* jobs) {
  for (u32 slot = 0; slot < entries_.size(); ++slot) {
    if (entries_[slot] == nullptr) continue;
    entries_[slot]->ran.wait_blocking();
    if (entries_[slot]->state.load(std::memory_order_acquire) == State::resident &&
        sink_.retire != nullptr) {
      sink_.retire(sink_.context, scene_mesh(slot));
    }
  }
  entries_.clear();
  free_.clear();
  by_identity_.clear();
  options_ = options;
  config_ = config;
  sink_ = sink;
  jobs_ = jobs;
  used_clusters_ = 0;
  whole_ = true;
  commit_admissions_ = 0;
  commit_bytes_ = 0;
  commit_ = 0;
  in_commit_ = false;
  stats_ = TileMeshStats{};
  entries_.resize(config.slots);
  free_.reserve(config.slots);
  for (u32 slot = config.slots; slot-- > 0;)
    free_.push_back(slot);  // slot 0 is taken first
  by_identity_.reserve(config.slots);
}

TileMeshes::State TileMeshes::state(u32 slot) const noexcept {
  if (slot >= entries_.size() || entries_[slot] == nullptr) return State::free;
  const Entry& entry = *entries_[slot];
  // A build the commit has not collected is still building to everyone but its job.
  if (!entry.collected) return State::building;
  return entry.state.load(std::memory_order_acquire);
}

u32 TileMeshes::clusters(u32 slot) const noexcept {
  return slot < entries_.size() && entries_[slot] != nullptr ? entries_[slot]->clusters : 0u;
}

u32 TileMeshes::acquire(const scene_gen::PlacementMesh& mesh, std::string_view dir,
                        std::string* error) {
  if (config_.slots == 0) {
    if (error != nullptr) {
      *error = mesh.path +
               ": this host holds no room for a mesh that arrives with a tile (a paged scene, a "
               "zero renderer.meshes.budget_mib, or a generator that did not say its tiles hand "
               "meshes: scene_gen::PlacementGeneratorDesc::tile_meshes)";
    }
    return k_none;
  }
  // The path as the load would resolve it: against the scene's directory when it is relative.
  scene_gen::PlacementMesh resolved = mesh;
  if (!resolved.path.empty() && !dir.empty() &&
      !std::filesystem::path(resolved.path).is_absolute()) {
    resolved.path = std::string(dir) + "/" + resolved.path;
  }
  const u64 identity = renderer::placement_mesh_identity(resolved);
  if (const u32* held = by_identity_.find_value(identity)) {
    Entry& entry = *entries_[*held];
    if (entry.refs == 0 && entry.collected &&
        entry.state.load(std::memory_order_acquire) == State::resident) {
      --stats_.lingering;
    }
    ++entry.refs;
    return *held;
  }
  // A slot: a free one, or a lingering mesh's, the longest unnamed first.
  if (free_.empty() && !evict_lingering()) {
    if (error != nullptr) {
      *error = mesh.path + ": every one of the " + std::to_string(config_.slots) +
               " mesh slots holds a mesh a tile names (renderer.meshes.slots)";
    }
    return k_none;
  }
  const u32 slot = free_.back();
  free_.pop_back();
  auto entry = std::make_unique<Entry>();
  entry->owner = this;
  entry->identity = identity;
  entry->mesh = std::move(resolved);
  entry->refs = 1;
  Entry* raw = entry.get();
  entries_[slot] = std::move(entry);
  by_identity_.insert_or_assign(identity, slot);
  ++stats_.known;
  ++stats_.building;
  if (jobs_ != nullptr) {
    // Off the committing thread (the frame's, in a windowed host): the efficiency pool, where the
    // collision's builds ahead and the page reads run, so a build does not take a performance
    // worker from the frame's own jobs.
    raw->ran.add(1);
    jobs_->schedule(jobs::Pool::Efficiency, jobs::Job{&TileMeshes::run, raw, &raw->ran});
  } else {
    raw->build();
  }
  return slot;
}

void TileMeshes::release(u32 slot) noexcept {
  if (slot >= entries_.size() || entries_[slot] == nullptr) return;
  Entry& entry = *entries_[slot];
  if (entry.refs == 0) return;
  if (--entry.refs != 0) return;
  // The commit that lets its last tile go is the first it is unnamed in: the one running, or the
  // next one when a tile's event let it go between two.
  entry.unnamed_since = in_commit_ ? commit_ : commit_ + 1;
  if (entry.collected && entry.state.load(std::memory_order_acquire) == State::resident)
    ++stats_.lingering;
}

// The builds that have finished since the last look, counted once each.
void TileMeshes::collect(bool wait) {
  for (u32 slot = 0; slot < entries_.size(); ++slot) {
    if (entries_[slot] == nullptr || entries_[slot]->collected) continue;
    Entry& entry = *entries_[slot];
    if (wait) {
      if (jobs_ != nullptr) {
        jobs_->wait(entry.ran);
      } else {
        entry.ran.wait_blocking();
      }
    }
    if (!entry.ran.done()) continue;
    entry.collected = true;
    --stats_.building;
    ++stats_.builds;
    stats_.build_ns += entry.build_ns;
    stats_.max_build_ns = std::max(stats_.max_build_ns, entry.build_ns);
    if (entry.state.load(std::memory_order_acquire) == State::failed) {
      ++stats_.failures;
      ENGINE_LOG_WARN(log_world_meshes, "a tile's mesh was not built",
                      log::field("mesh", entry.mesh.path), log::field("error", entry.error));
      continue;
    }
    entry.clusters = entry.data.lod.mesh.clusters.size();
    entry.materials = entry.data.source.data.materials.size() + 1u;
    entry.payload = entry.data.payload_bytes();
    ++stats_.waiting;
    stats_.host_bytes += entry.payload;
  }
}

void TileMeshes::begin_commit(bool whole) {
  ++commit_;
  in_commit_ = true;
  whole_ = whole;
  commit_admissions_ = 0;
  commit_bytes_ = 0;
  collect(whole);
}

TileMeshes::Admit TileMeshes::admit(u32 slot) {
  if (slot >= entries_.size() || entries_[slot] == nullptr) return Admit::failed;
  Entry& entry = *entries_[slot];
  if (!entry.collected) return Admit::not_ready;
  const State now = entry.state.load(std::memory_order_acquire);
  if (now == State::resident) return Admit::done;
  if (now == State::failed) return Admit::failed;
  // The allowance (a budgeted commit's): a count, and bytes — a mesh larger than all of it goes
  // alone, so a tile whose mesh is large still arrives.
  if (!whole_) {
    const bool count_spent = config_.admissions != 0 && commit_admissions_ >= config_.admissions;
    const bool bytes_spent = config_.admission_bytes != 0 && commit_admissions_ != 0 &&
                             commit_bytes_ + entry.payload > config_.admission_bytes;
    if (count_spent || bytes_spent) {
      ++stats_.deferred;
      return Admit::deferred;
    }
  }
  if (entry.clusters == 0 || entry.clusters > config_.max_clusters) {
    // Never admissible, whatever is let go: said once, as a failure.
    entry.state.store(State::failed, std::memory_order_release);
    entry.error = entry.mesh.path + ": " + std::to_string(entry.clusters) +
                  " clusters, and a mesh that arrives with a tile may have at most " +
                  std::to_string(config_.max_clusters) + " (renderer.meshes.max_clusters)";
    --stats_.waiting;
    stats_.host_bytes -= entry.payload;
    entry.data = renderer::PlacementMeshData{};
    ++stats_.failures;
    ENGINE_LOG_WARN(log_world_meshes, "a tile's mesh was refused",
                    log::field("mesh", entry.mesh.path), log::field("error", entry.error));
    return Admit::failed;
  }
  const bool fits = sink_.fits != nullptr
                        ? sink_.fits(sink_.context, entry.clusters, entry.materials)
                        : u64{used_clusters_} + entry.clusters <= config_.clusters;
  if (!fits) return Admit::no_room;
  const i64 start = time::monotonic_ns();
  std::string why;
  if (sink_.admit != nullptr && !sink_.admit(sink_.context, scene_mesh(slot), entry.data, &why)) {
    entry.state.store(State::failed, std::memory_order_release);
    entry.error = why;
    --stats_.waiting;
    stats_.host_bytes -= entry.payload;
    entry.data = renderer::PlacementMeshData{};
    ++stats_.failures;
    ENGINE_LOG_WARN(log_world_meshes, "a tile's mesh was refused",
                    log::field("mesh", entry.mesh.path), log::field("error", why));
    return Admit::failed;
  }
  const i64 took = time::monotonic_ns() - start;
  stats_.admit_ns += took;
  stats_.max_admit_ns = std::max(stats_.max_admit_ns, took);
  // The scene has it: the host's copy of the streams goes (a mesh let go and wanted again comes
  // back out of the cache).
  entry.data = renderer::PlacementMeshData{};
  entry.state.store(State::resident, std::memory_order_release);
  used_clusters_ += entry.clusters;
  ++commit_admissions_;
  commit_bytes_ += entry.payload;
  --stats_.waiting;
  stats_.host_bytes -= entry.payload;
  ++stats_.resident;
  stats_.clusters += entry.clusters;
  stats_.bytes += u64{entry.clusters} * config_.cluster_bytes;
  ++stats_.admitted;
  stats_.admitted_bytes += entry.payload;
  if (entry.refs == 0) ++stats_.lingering;
  return Admit::done;
}

// The entry's slot back, whatever state it is in (never while its build runs).
void TileMeshes::drop(u32 slot) {
  Entry& entry = *entries_[slot];
  const State now = entry.state.load(std::memory_order_acquire);
  if (entry.collected && now == State::built) {
    --stats_.waiting;
    stats_.host_bytes -= entry.payload;
  }
  if (!entry.collected) --stats_.building;
  by_identity_.erase(entry.identity);
  entries_[slot].reset();
  free_.push_back(slot);
  --stats_.known;
}

void TileMeshes::retire(u32 slot, bool for_room) {
  Entry& entry = *entries_[slot];
  if (sink_.retire != nullptr) sink_.retire(sink_.context, scene_mesh(slot));
  used_clusters_ -= entry.clusters;
  --stats_.resident;
  stats_.clusters -= entry.clusters;
  stats_.bytes -= u64{entry.clusters} * config_.cluster_bytes;
  if (entry.refs == 0) --stats_.lingering;
  ++stats_.retired;
  if (for_room) ++stats_.evicted;
  // Its state leaves `resident` before the slot goes, so `drop` counts it as nothing held.
  entry.state.store(State::failed, std::memory_order_release);
  drop(slot);
}

bool TileMeshes::evict_lingering() {
  u32 oldest = k_none;
  for (u32 slot = 0; slot < entries_.size(); ++slot) {
    if (entries_[slot] == nullptr) continue;
    const Entry& entry = *entries_[slot];
    if (entry.refs != 0 || !entry.collected) continue;
    if (oldest == k_none || entry.unnamed_since < entries_[oldest]->unnamed_since) oldest = slot;
  }
  if (oldest == k_none) return false;
  if (entries_[oldest]->state.load(std::memory_order_acquire) == State::resident) {
    retire(oldest, true);
  } else {
    drop(oldest);
  }
  return true;
}

void TileMeshes::end_commit() {
  in_commit_ = false;
  for (u32 slot = 0; slot < entries_.size(); ++slot) {
    if (entries_[slot] == nullptr) continue;
    Entry& entry = *entries_[slot];
    if (entry.refs != 0 || !entry.collected) continue;  // named, or its build still runs
    const State now = entry.state.load(std::memory_order_acquire);
    if (now == State::resident) {
      if (commit_ >= entry.unnamed_since + config_.linger) retire(slot, false);
    } else {
      // Built or failed and nobody names it: nothing to keep warm that the cache does not.
      drop(slot);
    }
  }
}

}  // namespace engine::world
