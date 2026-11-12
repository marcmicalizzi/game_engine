#pragma once

// A tile's meshes: residency by reference (docs/subsystems/world.md, "A tile's meshes: residency by
// reference"; scene_gen.md, "A tile's meshes"; roadmap R142). A placement generator's tile hands,
// with its placements, the meshes only it names — each a recipe, or a file — and this is the table
// of them a streamed world keeps: one entry per distinct mesh (`renderer::
// placement_mesh_identity`), **built when the first tile that names it activates** (through the
// mesh generator registry and the derived-data cache, on the job system when the host offers one),
// **resident while any tile held names it**, and **let go after the last** — at once when its room
// is wanted, and otherwise after `linger` commits, which is what keeps a walker pacing a ring's
// border from building and uploading the same mesh at every turn (the ring's own hysteresis,
// `WorldDesc::hysteresis`, keeps the *tile* from flipping; this keeps the mesh warm for the turns
// that are wider than that).
//
// **An entry is a mesh slot.** The scene keeps room for a fixed number of such meshes after its
// own (`renderer::GpuScene::MeshRoom`), and an entry takes one of those slots when it is first
// named and keeps it until it goes, so a tile's instance names its scene mesh —
// `first_mesh + slot` — from the moment it is built, before the mesh is, and nothing is renumbered
// when the mesh arrives. Which slot a mesh gets depends on the order tiles came in; nothing drawn
// does, because a visibility id names a pair and a triangle, never a mesh slot, and the mesh's
// bytes are a function of its recipe (renderer.md, "Meshes that come and go", the order rule).
//
// **No device here.** The table hands a built mesh to a sink — the renderer's scene, or nothing
// for a host with none — and asks it whether one fits; the collision builds its tiles' meshes
// through the same `renderer::load_placement_mesh` and keeps its own proxies.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace engine::world {

// Where an admitted mesh goes, and who says whether it fits: the renderer's scene
// (`SceneRenderer::admit_mesh`, `retire_mesh`, `GpuScene::mesh_fits`). All null for a host with no
// renderer, which then counts the room itself (`TileMeshConfig::clusters`).
struct TileMeshSink {
  void* context = nullptr;
  bool (*fits)(void* context, u32 clusters, u32 materials) = nullptr;
  bool (*admit)(void* context, u32 mesh, const renderer::PlacementMeshData& data,
                std::string* error) = nullptr;
  void (*retire)(void* context, u32 mesh) = nullptr;
  // **Its textures** (R147; texture.md, "Textures that arrive after the load"): `texture` takes a
  // reference on the built texture of `texture.key`, uploading it if the scene has none
  // (`SceneRenderer::acquire_texture`), and `release_texture` gives one back; the table holds one
  // per texture of a mesh from the commit that put the texture in until its mesh is admitted (the
  // mesh then holds its own). `texture_resident` is asked from the thread that builds a mesh,
  // before it reads a `.tex`. All null for a host that draws nothing: its builds then leave the
  // textures out.
  enum class Texture : u8 { held, need_bytes, refused };
  Texture (*texture)(void* context, const renderer::PlacementTexture& texture,
                     std::string* why) = nullptr;
  void (*release_texture)(void* context, u64 key) = nullptr;
  bool (*texture_resident)(void* context, u64 key) = nullptr;
};

struct TileMeshConfig {
  // The scene's room (`renderer::GpuScene::MeshRoom`): the scene mesh of slot 0, how many slots,
  // the arena's clusters — the residency budget, in the unit it is refused in — the most one mesh
  // may have, and the device bytes a cluster reserves (what turns clusters into the budget's MiB).
  u32 first_mesh = 0;
  u32 slots = 0;
  u32 clusters = 0;
  u32 max_clusters = 0;
  u64 cluster_bytes = 0;
  // Commits a mesh no held tile names stays resident before it is let go (`world.meshes.linger`).
  u32 linger = 120;
  // What a budgeted commit may admit: meshes, and bytes staged for the device
  // (`world.meshes.admissions`, `world.meshes.admission_kib`). A whole commit admits everything.
  u32 admissions = 2;
  u64 admission_bytes = 4ull * 1024 * 1024;
};

// The tunables' values, for a host filling a `TileMeshConfig`.
u32 mesh_linger_tunable() noexcept;
u32 mesh_admissions_tunable() noexcept;
u64 mesh_admission_bytes_tunable() noexcept;

struct TileMeshStats {
  // Now.
  u32 known = 0;     // entries: named by a held tile, lingering, or still building
  u32 building = 0;  // builds not yet collected
  u32 waiting = 0;   // built, not resident: host memory until admitted
  u32 resident = 0;
  u32 lingering = 0;   // resident and named by no held tile
  u32 clusters = 0;    // the resident meshes' clusters, of `TileMeshConfig::clusters`
  u64 bytes = 0;       // those clusters' reserved device bytes
  u64 host_bytes = 0;  // the waiting meshes' streams in host memory
  // Over the run.
  u64 builds = 0;    // meshes built or found in the cache
  i64 build_ns = 0;  // on whichever thread built them
  i64 max_build_ns = 0;
  u64 admitted = 0;
  i64 admit_ns = 0;  // the sink's admissions, on the committing thread
  i64 max_admit_ns = 0;
  u64 admitted_bytes = 0;
  u64 retired = 0;
  u64 evicted = 0;   // retirements made for room: a lingering mesh, or a farther tile's
  u64 refused = 0;   // admissions the room refused
  u64 deferred = 0;  // admissions a commit's allowance left for the next
  u64 failures = 0;  // meshes that could not be built
  // Their textures (R147), over the run: the ones a commit uploaded and their staged bytes, the
  // ones a mesh found in the scene already, and the meshes that draw their constants because a
  // texture was refused (the bound is in the log) or was never built.
  u64 textures_uploaded = 0;
  u64 texture_bytes = 0;
  u64 textures_shared = 0;
  u64 texture_refusals = 0;
};

class TileMeshes {
 public:
  TileMeshes();
  ~TileMeshes();
  ENGINE_NON_COPYABLE(TileMeshes);

  static constexpr u32 k_none = ~0u;

  // `jobs` null: a mesh is built where it is first named. With one, on its efficiency pool, and a
  // commit collects what has finished. Any entries held are dropped first (their builds waited
  // for, their resident meshes retired).
  void create(const renderer::PlacementMeshOptions& options, const TileMeshConfig& config,
              TileMeshSink sink, jobs::JobSystem* jobs);
  bool enabled() const noexcept { return config_.slots != 0; }
  const TileMeshConfig& config() const noexcept { return config_; }
  const TileMeshStats& stats() const noexcept { return stats_; }

  // A tile names `mesh` (its path resolved against `dir` when relative): the entry's slot, with a
  // reference taken, its build started when it is new. `k_none`, with a sentence, when the table
  // is not enabled or every slot is held by a mesh in use.
  u32 acquire(const scene_gen::PlacementMesh& mesh, std::string_view dir, std::string* error);
  void release(u32 slot) noexcept;
  u32 scene_mesh(u32 slot) const noexcept { return config_.first_mesh + slot; }
  u32 slot_of(u32 scene_mesh) const noexcept {
    return scene_mesh >= config_.first_mesh && scene_mesh - config_.first_mesh < config_.slots
               ? scene_mesh - config_.first_mesh
               : k_none;
  }

  enum class State : u8 { free, building, built, resident, failed };
  State state(u32 slot) const noexcept;
  // Its clusters (a tile's pairs are counted from them) and reach on the ground; 0 until built.
  u32 clusters(u32 slot) const noexcept;
  bool resident(u32 slot) const noexcept { return state(slot) == State::resident; }

  // ---- a commit's half -------------------------------------------------------------------------
  // `whole`: this commit admits everything it can, and waits for every build first (a restart, a
  // capture's fill). Otherwise it collects the builds that have finished and admits within the
  // allowance.
  void begin_commit(bool whole);
  enum class Admit : u8 {
    done,       // resident (now, or already)
    not_ready,  // still building: ask again at a later commit
    deferred,   // built, and this commit's allowance is spent
    no_room,    // built, and the room has no run for it: the caller may make some and ask again
    failed,     // it could not be built, or the sink refused it for another reason
  };
  Admit admit(u32 slot);
  // Retires one resident mesh no held tile names, the longest unnamed first. False: there is none.
  bool evict_lingering();
  // After the tail has been handed on (nothing names a mesh this retires): every mesh unnamed for
  // `linger` commits goes, and every entry nobody names that never became resident.
  void end_commit();
  // Counts of the last `begin_commit`..`end_commit`: admissions made and bytes staged.
  u32 commit_admissions() const noexcept { return commit_admissions_; }

 private:
  struct Entry;
  static void run(void* data) noexcept;
  void collect(bool wait);
  void retire(u32 slot, bool for_room);
  void drop(u32 slot);
  // The entry's textures into the scene, in order, within the commit's bytes: true when every one
  // is in (or the mesh draws its constants), false when the allowance left one for a later commit.
  bool admit_textures(Entry& entry);
  void release_textures(Entry& entry);

  renderer::PlacementMeshOptions options_;
  TileMeshConfig config_;
  TileMeshSink sink_;
  jobs::JobSystem* jobs_ = nullptr;
  Vector<std::unique_ptr<Entry>> entries_;  // by slot; null: free
  Vector<u32> free_;                        // free slots, the lowest last
  FlatMap<u64, u32> by_identity_;
  u32 used_clusters_ = 0;  // a host with no sink counts the room itself
  bool whole_ = true;
  u32 commit_admissions_ = 0;
  u64 commit_bytes_ = 0;
  u64 commit_ = 0;  // commits begun: what an unnamed entry's age is counted in
  bool in_commit_ = false;
  bool texture_refusing_ = false;  // the last texture asked for was refused: said once per change
  TileMeshStats stats_;
};

}  // namespace engine::world
