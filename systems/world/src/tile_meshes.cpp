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
  bool shelved = false;              // built, withheld for room, `data` let go (`withhold`)
  renderer::PlacementMeshData data;  // until admitted or shelved
  std::string error;
  i64 build_ns = 0;
  u32 clusters = 0;
  u32 materials = 0;
  u64 payload = 0;
  u64 unnamed_since = 0;  // the commit its last reference went at
  // The last commit `admit` was asked about it, and the last whose `admit` answered `no_room`
  // (W51-5b: what `withhold` tells a nearer tile's ask from a farther tile's refusal by).
  u64 asked = 0;
  u64 refused = 0;
  // Built again for room the table holds for it until it is in or let go: what it was asked with.
  bool reserved = false;
  u32 reserved_clusters = 0;
  u32 reserved_materials = 0;
  // Its textures (R147): how many of `data.textures` are in the scene, and the references the
  // table holds on them until the mesh is admitted and holds its own.
  u32 textures_in = 0;
  Vector<u64> held;
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
  // A host that draws: a mesh's build also finds, builds and reads its textures, off the
  // committing thread, skipping the read of one the scene says it holds.
  options_.textures = sink.texture != nullptr;
  options_.texture_resident = sink.texture != nullptr ? sink.texture_resident : nullptr;
  options_.texture_context = sink.context;
  texture_refusing_ = false;
  jobs_ = jobs;
  used_clusters_ = 0;
  reserved_clusters_ = 0;
  reserved_materials_ = 0;
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
  start_build(*raw);
  return slot;
}

void TileMeshes::start_build(Entry& entry) {
  ++stats_.building;
  entry.collected = false;
  entry.state.store(State::building, std::memory_order_release);
  if (jobs_ != nullptr) {
    // Off the committing thread (the frame's, in a windowed host): the efficiency pool, where the
    // collision's builds ahead and the page reads run, so a build does not take a performance
    // worker from the frame's own jobs.
    entry.ran.add(1);
    jobs_->schedule(jobs::Pool::Efficiency, jobs::Job{&TileMeshes::run, &entry, &entry.ran});
  } else {
    entry.build();
  }
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
    collect_entry(entry);
  }
}

void TileMeshes::collect_entry(Entry& entry) {
  entry.collected = true;
  --stats_.building;
  ++stats_.builds;
  stats_.build_ns += entry.build_ns;
  stats_.max_build_ns = std::max(stats_.max_build_ns, entry.build_ns);
  if (entry.state.load(std::memory_order_acquire) == State::failed) {
    reserve(entry, false);
    ++stats_.failures;
    ENGINE_LOG_WARN(log_world_meshes, "a tile's mesh was not built",
                    log::field("mesh", entry.mesh.path), log::field("error", entry.error));
    return;
  }
  // A second build of a shelved mesh is the same bytes (its recipe or file, through the same
  // content-addressed cache), so these are the counts it was shelved with.
  entry.clusters = entry.data.lod.mesh.clusters.size();
  entry.materials = entry.data.source.data.materials.size() + 1u;
  entry.payload = entry.data.payload_bytes();
  ++stats_.waiting;
  stats_.host_bytes += entry.payload;
}

// **A withheld mesh is let go and built again** (world.md, the section of that name; W51-5): the
// room's refusal used to keep the built mesh — its merged streams and its textures read for the
// admission — in host memory for as long as the room stayed full, which standing in the forest
// staging was 189 meshes and 2.2 GB of commit that nothing drew. What the table keeps of a
// shelved mesh is its counts: the tile's pairs are counted from its clusters, and the room is
// asked with its clusters and materials before it is built again.
void TileMeshes::withhold(u32 slot) {
  if (slot >= entries_.size() || entries_[slot] == nullptr) return;
  Entry& entry = *entries_[slot];
  if (!entry.collected || entry.shelved) return;
  if (entry.state.load(std::memory_order_acquire) != State::built) return;
  // One no room could ever take is `admit`'s to fail, with its sentence.
  if (entry.clusters == 0 || entry.clusters > config_.max_clusters) return;
  // **Not one a nearer tile is admitting** (W51-5b): once a commit's first refusal has set the
  // room full, every later ticket is withheld without asking, and a mesh is often shared — by the
  // tiles of one group, by a recipe two tiles hand. A farther tile's ticket used to shelve a mesh
  // a nearer one had asked for in the same commit, or one part-way through its textures, which
  // let its textures go; the nearer tile's next commit built it again and started its textures
  // over, and a farther tile shelved it again, so standing with the room full the nearer tile
  // never filled. A mesh whose own admission answered no room this commit is still let go: the
  // room cannot take it now, whoever asked.
  if (entry.refused != commit_) {
    if (entry.asked == commit_) {
      ++stats_.kept_asked;
      return;
    }
    if (entry.textures_in != 0 || !entry.held.empty()) {
      ++stats_.kept_textures;
      return;
    }
  }
  release_textures(entry);  // the scene may let a texture go that only this mesh wanted
  reserve(entry, false);
  entry.textures_in = 0;
  entry.data = renderer::PlacementMeshData{};
  entry.shelved = true;
  --stats_.waiting;
  stats_.host_bytes -= entry.payload;
  ++stats_.shelved;
}

void TileMeshes::begin_commit(bool whole) {
  ++commit_;
  in_commit_ = true;
  whole_ = whole;
  commit_admissions_ = 0;
  commit_bytes_ = 0;
  collect(whole);
}

// The room asked for `entry` beside every other reservation, less its own: a shelved mesh built
// again holds what it was asked for until it is in or let go, and every admission is asked against
// what is left (W51-5b). **The mesh and the reservations go to the sink apart** (R218): the sum
// used to be handed over as one mesh's count, which the renderer held to `max_clusters` and to one
// run, so two large meshes built again (a forest tree is about 1,850 clusters) refused a third in
// an empty room.
bool TileMeshes::room_for(const Entry& entry) const {
  const u64 clusters = reserved_clusters_ - entry.reserved_clusters;
  const u64 materials = reserved_materials_ - entry.reserved_materials;
  if (sink_.fits != nullptr)
    return sink_.fits(sink_.context, entry.clusters, entry.materials, clusters, materials);
  return u64{used_clusters_} + clusters + entry.clusters <= config_.clusters;
}

void TileMeshes::reserve(Entry& entry, bool on) noexcept {
  if (entry.reserved == on) return;
  entry.reserved = on;
  if (on) {
    entry.reserved_clusters = entry.clusters;
    entry.reserved_materials = entry.materials;
    reserved_clusters_ += entry.clusters;
    reserved_materials_ += entry.materials;
  } else {
    reserved_clusters_ -= entry.reserved_clusters;
    reserved_materials_ -= entry.reserved_materials;
    entry.reserved_clusters = 0;
    entry.reserved_materials = 0;
  }
  stats_.reserved_clusters = reserved_clusters_;
}

// **A nearer mesh takes a farther one's reservation** (R218; world.md, "A withheld mesh is let go
// and built again"). A commit asks in the pair budget's order, so a reservation held by a mesh not
// yet asked this commit is a farther tile's (or one no waiting tile asks for). Until R218 such a
// reservation held the nearer mesh out with nothing in the room: a farther mesh built again, once
// its first texture was in, was kept by `withhold` whenever a nearer refusal came first in a
// commit, and never asked again, while the nearer mesh was refused for its reservation — every
// commit after (the owner's walk of the forest, 2026-11-24: the room empty and 240 tiles withheld).
// The caller asks this after the lingering meshes and before it yields a farther tile's resident
// meshes (a reservation is of nothing drawn), so the head of the order is refused only for what
// nearer tiles hold, resident or reserved: nothing farther can starve it. The one let go is shelved
// again (its streams and textures given back), or, still building, keeps its build and loses only
// the room; either way it is asked again in its turn.
bool TileMeshes::release_farther_reservation() {
  u32 chosen = k_none;
  for (u32 slot = 0; slot < entries_.size(); ++slot) {
    const Entry* entry = entries_[slot].get();
    if (entry == nullptr || !entry->reserved || entry->asked == commit_) continue;
    // The largest first: the fewest builds undone for the room asked.
    if (chosen == k_none || entry->reserved_clusters > entries_[chosen]->reserved_clusters)
      chosen = slot;
  }
  if (chosen == k_none) return false;
  Entry& entry = *entries_[chosen];
  reserve(entry, false);
  ++stats_.reservations_taken;
  if (entry.collected && entry.state.load(std::memory_order_acquire) == State::built &&
      !entry.shelved) {
    release_textures(entry);
    entry.textures_in = 0;
    entry.data = renderer::PlacementMeshData{};
    entry.shelved = true;
    --stats_.waiting;
    stats_.host_bytes -= entry.payload;
    ++stats_.shelved;
  }
  return true;
}
void TileMeshes::release_textures(Entry& entry) {
  if (sink_.release_texture != nullptr) {
    for (const u64 key : entry.held)
      sink_.release_texture(sink_.context, key);
  }
  entry.held.clear();
}

// **A mesh waits for its textures** (texture.md, "Textures that arrive after the load"): a
// textured material's constants are its factors, white for most, so a rock drawn before its
// textures is a white rock that turns brown; its tile draws what it drew for the commits this
// takes. One texture at a time, in the mesh's own order, each inside the commit's bytes — one
// larger than all of them goes alone — and the table holds a reference on each until the mesh is
// in. A refusal (no pair, the byte budget, the device) ends the wait: the mesh is admitted with
// its constants, said once per change with the bound's name.
bool TileMeshes::admit_textures(Entry& entry) {
  if (sink_.texture == nullptr) return true;
  while (entry.textures_in < entry.data.textures.size()) {
    renderer::PlacementTexture& texture = entry.data.textures[entry.textures_in];
    const bool there =
        sink_.texture_resident != nullptr && sink_.texture_resident(sink_.context, texture.key);
    if (!there && texture.data.levels.empty()) {
      // The scene held it when the mesh was built and has let it go since: read now, on this
      // thread, which is the one case a commit reads a file.
      std::string why;
      (void)renderer::read_placement_texture(options_, texture.key, texture.data, &why);
    }
    const u64 bytes = there ? 0 : texture.staged_bytes();
    if (!whole_ && bytes != 0 && config_.admission_bytes != 0 && commit_bytes_ != 0 &&
        commit_bytes_ + bytes > config_.admission_bytes) {
      ++stats_.deferred;
      return false;
    }
    std::string why;
    const TileMeshSink::Texture got = sink_.texture(sink_.context, texture, &why);
    if (got != TileMeshSink::Texture::held) {
      if (got == TileMeshSink::Texture::need_bytes) why = "its built texture could not be read";
      release_textures(entry);
      entry.data.textures.clear();
      entry.data.textures_why = why;
      entry.textures_in = 0;
      ++stats_.texture_refusals;
      if (!texture_refusing_) {
        texture_refusing_ = true;
        ENGINE_LOG_WARN(log_world_meshes,
                        "a tile's mesh draws its materials' constants: a texture was refused",
                        log::field("mesh", entry.mesh.path), log::field("bound", why),
                        log::field("refusals", stats_.texture_refusals));
      }
      return true;
    }
    texture_refusing_ = false;
    entry.held.push_back(texture.key);
    if (there) {
      ++stats_.textures_shared;
    } else {
      ++stats_.textures_uploaded;
      stats_.texture_bytes += bytes;
      commit_bytes_ += bytes;
    }
    texture.data = texture::TextureData{};  // the scene has it: the host's copy goes
    ++entry.textures_in;
  }
  return true;
}

TileMeshes::Admit TileMeshes::admit(u32 slot) {
  if (slot >= entries_.size() || entries_[slot] == nullptr) return Admit::failed;
  Entry& entry = *entries_[slot];
  entry.asked = commit_;
  if (!entry.collected) return Admit::not_ready;
  const State now = entry.state.load(std::memory_order_acquire);
  if (now == State::resident) return Admit::done;
  if (now == State::failed) return Admit::failed;
  if (entry.shelved) {
    // Built again only when the room can take it (its counts are the shelved build's), so a
    // standing world whose room stays full builds nothing. The build is off the committing thread,
    // as the first was: the tile draws what it drew until a later commit collects it, which is the
    // admission's whole cost on that thread — a read of the cache on a worker, never a stall.
    // **The room it was asked for is reserved** (W51-5b) until the mesh is in or let go, so room
    // for one mesh starts one build: every shelved mesh used to be asked against the same free
    // room, and all that fitted alone were built again in one commit, the rest shelved again a
    // commit or two later — the host commit W51-5 gave back, taken again for builds wasted.
    if (!room_for(entry)) {
      entry.refused = commit_;
      return Admit::no_room;
    }
    entry.shelved = false;
    --stats_.shelved;
    ++stats_.rebuilds;
    reserve(entry, true);
    start_build(entry);
    if (!whole_) return Admit::not_ready;
    // A whole commit (a restart, a capture's fill) waits for every build, this one included.
    if (jobs_ != nullptr) {
      jobs_->wait(entry.ran);
    } else {
      entry.ran.wait_blocking();
    }
    collect_entry(entry);
    if (entry.state.load(std::memory_order_acquire) == State::failed) return Admit::failed;
  }
  if (entry.clusters != 0 && entry.clusters <= config_.max_clusters && !admit_textures(entry))
    return Admit::deferred;
  // The allowance (a budgeted commit's): a count, and bytes — a mesh larger than all of it goes
  // alone, so a tile whose mesh is large still arrives. The bytes are the commit's, the textures'
  // included, so a mesh comes the commit after a texture that filled it.
  if (!whole_) {
    const bool count_spent = config_.admissions != 0 && commit_admissions_ >= config_.admissions;
    const bool bytes_spent = config_.admission_bytes != 0 && commit_bytes_ != 0 &&
                             commit_bytes_ + entry.payload > config_.admission_bytes;
    if (count_spent || bytes_spent) {
      ++stats_.deferred;
      return Admit::deferred;
    }
  }
  if (entry.clusters == 0 || entry.clusters > config_.max_clusters) {
    // Never admissible, whatever is let go: said once, as a failure.
    reserve(entry, false);
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
  if (!room_for(entry)) {
    entry.refused = commit_;
    return Admit::no_room;
  }
  const i64 start = time::monotonic_ns();
  std::string why;
  reserve(entry, false);  // in, or failed: either way it holds no room of the table's
  if (sink_.admit != nullptr && !sink_.admit(sink_.context, scene_mesh(slot), entry.data, &why)) {
    entry.state.store(State::failed, std::memory_order_release);
    entry.error = why;
    --stats_.waiting;
    stats_.host_bytes -= entry.payload;
    release_textures(entry);
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
  // back out of the cache), and so do the table's references on its textures — the mesh holds its
  // own from here until it is retired.
  release_textures(entry);
  entry.textures_in = 0;
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
    if (entry.shelved) {
      --stats_.shelved;
    } else {
      --stats_.waiting;
      stats_.host_bytes -= entry.payload;
    }
  }
  if (!entry.collected) --stats_.building;
  reserve(entry, false);
  release_textures(entry);  // a mesh let go while it waited for its textures
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
