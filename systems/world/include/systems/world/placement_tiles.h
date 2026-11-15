#pragma once

// The placements consumer (docs/subsystems/world.md, "The consumers"; scene_gen.md; ADR-0046; E34,
// E35, E37): a tile that activates gets what the scene's placement entries put on it — each entry's
// generator, found in the scene-generator registry by the name the scene gave it, asked for the
// tile (`scene_gen::PlacementGeneratorDesc::tile`) in the representation the tile's ring draws,
// which is exactly what the scene reader's whole expansion would have put there — as instances of
// the meshes the reader left resident; a tile that goes inactive drops them; a tile that moves
// between rings is built again when a generator draws the two rings differently. Once an update,
// the consumer hands the renderer every active tile's instances, **in tile order**, as the scene's
// tail (`renderer::SceneRenderer::set_dynamic_instances`). It names no generator: the ruins are the
// placement generator "ruins" (E34's blocks near and sections far is theirs), the city "city".
//
// **One consumer for every entry, in the scene's order.** A tile's placements are every entry's,
// entry by entry in the order the reader walks them (the `ruins` entries, then `placements`), so a
// tile streamed in is the whole read restricted to the tile; and the renderer takes one tail, so
// the pair budget is decided over all of it at once. Ground is not here: a world that holds its
// tiles' ground registers that consumer before this one (world.md, "The consumers"), and the
// placements stand on the terrain's ground function (the provider's floor), which is the same at
// any activation.
//
// **Nothing cross-fades a representation change**, so a building **pops** from sections to blocks
// when its tile crosses the inner radius, and E35 measures by how much; each placement's tag (the
// ruins': rubble) is kept beside the tail for engine-view's handover pass to split the pop by.
//
// **The pair budget is enforced here too.** A tile whose instances would take the scene past its
// budget — the visibility id's 2^24 pairs by default — is withheld at the commit, over every tile
// held: by ring, innermost first, then nearest first, then in tile order, and none after the first
// that does not fit. A withheld tile stays active and draws nothing until the budget has room.
// **It does not trade at the cap** (T131; world.md, "No trading at the pair cap"): a drawn tile
// keeps its place against one less than `admit_bias` nearer, a tile that would grow the drawn set
// past its farthest member waits for `admit_spare_pairs` of room beyond itself, and a tile the
// budget let go waits `admit_hold_off` commits before it may grow the set again.
//
// **None of a tile's building is on the frame's thread** (T131; world.md, "Builds off the frame's
// thread"): with a job system (`set_build_jobs`) and a budgeted update, the due tiles are built by
// one job at a time on the job system's efficiency pool, `budget` tiles a job, and a commit hands
// over what the last job finished — its instances copied into the tile, its meshes named — which is
// the frame's whole share. A whole update (budget 0) waits for the job and builds the rest where it
// is, so a capture or a restart is the same tail either way.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/world/tile_meshes.h>
#include <systems/world/tile_ring.h>
#include <systems/world/world.h>

#include <memory>
#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::world {

// What a ring draws, the scene's per-ring word (`engine.scene.RingRuins`, a streamed world's
// `rings[r].ruins`): what the ruins generator draws a tile in, and what engine-view's handover pass
// and summary name a ring by.
enum class RingRuins : u8 { Blocks = 0, Sections = 1, Walls = 2 };
const char* ring_ruins_name(RingRuins ruins) noexcept;

// How many representations a generator may number its tiles' `kind` by, which the stats count
// (the ruins' are the scene's `RingRuins`: 0 blocks, 1 sections, 2 walls).
inline constexpr u32 k_placement_kinds = 4;

struct PlacementTilesConfig {
  f32 tile_size = 32.0f;  // the ring's; every streamed entry's grid must be the same
  u32 ring_count = 3;
  RingRuins ruins[k_max_rings] = {RingRuins::Blocks, RingRuins::Sections, RingRuins::Walls,
                                  RingRuins::Walls,  RingRuins::Walls,    RingRuins::Walls,
                                  RingRuins::Walls};
  // The scene's pair budget, its load's instances included.
  u32 max_pairs = renderer::k_max_pairs;
  // The renderer's prefix plus all reserved terrain slots. Zero uses the file prefix.
  u32 fixed_pairs = 0;
  // **No trading at the cap** (T131; world.md, "No trading at the pair cap"). A drawn tile ranks as
  // if it were `admit_bias` tiles nearer than it is, so a tile must be that much nearer to take its
  // place; a tile ranked past every drawn tile is admitted only with `admit_spare_pairs` of the
  // budget still free after it (zero: a sixty-fourth of what the tiles may hold, `max_pairs` less
  // the fixed pairs); and a tile the budget let go
  // does not come back past the drawn set for `admit_hold_off` commits. The defaults are the
  // tunables' (`admission_tunables`). None of it moves `max_pairs`.
  f32 admit_bias = 1.0f;
  u32 admit_spare_pairs = 0;
  u32 admit_hold_off = 120;
};

// The tunables' values (`world.placements.admit_bias`, `admit_spare_pairs`, `admit_hold_off`),
// written into a host's config.
void admission_tunables(PlacementTilesConfig& config) noexcept;

// Where the tail goes once an update. False with `error` is logged and counted, and the tail is
// offered again on the next change.
using InstanceSink = bool (*)(void* context, std::span<const renderer::SceneInstance> tail,
                              std::string* error);

struct PlacementTilesStats {
  // A generator's tile calls that put something on a tile, by the representation it drew (its own
  // numbering, `scene_gen::Placements::kind`), and their wall time.
  u64 built[k_placement_kinds] = {};
  i64 build_ns[k_placement_kinds] = {};
  // Every tile built for the ring (activated, moved between rings that draw it differently, or
  // re-stood), whatever its generators put on it, and the whole build's wall time: the generators'
  // calls above and the tile's pair count after them (W82: the frame record's `world.tiles_ms`).
  // `tiles_ns` is the frame's share: the generators' calls where the build ran on the committing
  // thread, and the hand-over of what a job built (`collect_ns`) where it did not.
  u64 tiles_built = 0;
  i64 tiles_ns = 0;
  // **Builds off the frame's thread** (T131, `set_build_jobs`): the jobs started, the generators'
  // wall time on the worker, the tiles a commit took from a job (and the time it took, the most of
  // one commit), and the results let go because their tile changed or went while it was built.
  u64 jobs = 0;
  i64 worker_ns = 0;
  u64 collected = 0;
  i64 collect_ns = 0;
  i64 max_collect_ns = 0;
  u64 discarded = 0;
  // **Trades at the pair cap** (T131): drawn tiles the budget let go, and of those the ones drawn
  // again while they were still held — a tile that went out and came back, which is a trade.
  u64 released = 0;
  u64 returned = 0;
  // **Seating by the footprint** (G201 stage 2; world.md, "A tile's trees are seated"): placements
  // standing on the ground whose mesh has a seat, over the run (17 ground queries each, inside
  // `tiles_ns`), the ones of those lowered, and the seating's own wall time.
  u64 seated = 0;
  u64 lowered = 0;
  i64 seat_ns = 0;
  // With a build budget (`set_build_budget`): the activations and redrawing ring changes whose
  // build waited for a later commit, over the run; and now, the held tiles still waiting.
  u64 deferred = 0;
  u32 pending = 0;
  u64 dropped = 0;   // tiles whose instances were dropped
  u64 refused = 0;   // tiles the pair budget withheld: each time one new or drawn is not drawn
  u64 failures = 0;  // generator or sink failures
  u64 handed = 0;    // tails handed to the sink
  i64 sink_ns = 0;   // the renderer taking the tail: the device wait and the tables' upload
  i64 max_sink_ns = 0;
  // A commit's own work before the sink: the pair budget's ranking over every tile held (`admit`)
  // and the tail written out of the drawn tiles.
  i64 admit_ns = 0;
  i64 tail_ns = 0;
  // Tiles built again for a moving ground (`restand`), and their wall time.
  u64 restood = 0;
  i64 restand_ns = 0;
  // Tiles generated for queries (`query_tile`: cache misses), and their wall time.
  u64 queried = 0;
  i64 query_ns = 0;
  // Now: the things (buildings) the drawn tiles hold, and the tail's instances and pairs; the tiles
  // held but not drawn, for the budget.
  u32 buildings = 0;
  u32 withheld = 0;
  u32 instances = 0;
  u32 pairs = 0;
  // Now: the tiles whose last build waits for its meshes (drawn as they were before it), and of
  // those the ones the mesh room refused at the last commit — the residency budget's withheld
  // tiles (world.md, "A tile's meshes: residency by reference"). Over the run: the tiles that gave
  // their meshes up to a nearer one.
  u32 mesh_waiting = 0;
  u32 mesh_starved = 0;
  u64 mesh_yielded = 0;
  // Now, by the ring each drawn tile is in: its tiles, instances and pairs, and the tiles withheld
  // there — what a streamed world's accounting reports (renderer.md, "The pair accounting").
  u32 ring_tiles[k_max_rings] = {};
  u32 ring_instances[k_max_rings] = {};
  u32 ring_pairs[k_max_rings] = {};
  u32 ring_withheld[k_max_rings] = {};
};

class PlacementTiles {
 public:
  PlacementTiles();
  ~PlacementTiles();
  ENGINE_NON_COPYABLE(PlacementTiles);

  // Opens every streamed placement entry's generator (`SceneData::streamed`) against the ring's
  // grid and the scene's world block, standing on the scene's terrain. `scene` must outlive the
  // consumer: its terrain is the ground the placements stand on and its parts are what a tile's
  // pairs are counted from. False, with `error`, for a generator this executable does not carry, or
  // an entry its generator refuses (a kit that will not read, an entry on another grid).
  bool create(const renderer::SceneData& scene, const PlacementTilesConfig& config,
              std::string* error = nullptr);
  void set_sink(InstanceSink sink, void* context) noexcept {
    sink_ = sink;
    sink_context_ = context;
  }
  // The row to register, after the ground's: every ring (a ring's representation is the config's).
  TileConsumer consumer() noexcept;
  // The ring whose scores order the tiles of one ring when the pair budget cannot draw them all:
  // the nearest first. Without one, tile order.
  void set_ring(const TileRing* ring) noexcept { ring_ = ring; }

  // **A tile's meshes** (world.md, "A tile's meshes: residency by reference"; tile_meshes.h): the
  // room they arrive into (the renderer's `GpuScene::mesh_room()`, as a `TileMeshConfig`), where an
  // admitted one goes, and the job system their builds run on (null: where the tile is built).
  // Called after `create` and before the first tile; without it a tile that hands a mesh is
  // refused with a sentence, and the rest of the tile is drawn. A tile whose build names a mesh
  // that is not resident **waits**: it draws what it drew before (nothing, for a new tile) until
  // every mesh it names is in, so no frame draws an instance of a mesh that is not there and no
  // ring change opens a hole. When the room has no run for a mesh, the farthest drawn tile that
  // names one gives its meshes up first, so every tile drawn outranks every tile the room
  // withholds (`PlacementTilesStats::mesh_starved`), as the pair budget's does.
  void set_meshes(const TileMeshConfig& config, TileMeshSink sink, jobs::JobSystem* jobs = nullptr);
  const TileMeshes& meshes() const noexcept { return meshes_; }
  // **Where a budgeted update's tiles are built** (T131; world.md, "Builds off the frame's
  // thread"): with `jobs`, on its efficiency pool, one job at a time; null (the default), on the
  // committing thread, as before. The entries' generators are asked for one tile at a time either
  // way, since an entry's state is its own scratch; their `occupies` and `representation` may be
  // asked on the committing thread while a job runs `tile`, and read only what `open` made.
  void set_build_jobs(jobs::JobSystem* jobs) noexcept;
  // Waits for a job in flight and takes what it built. Called before anything that asks a
  // generator on this thread (a whole commit, a query's tile, `restand` without a job system).
  void settle();

  // The tail as it was last handed on, in tile order. With a run sink (`set_run_sink`) a commit
  // writes no tail, and this writes it out of the drawn tiles when first asked after a commit.
  std::span<const renderer::SceneInstance> tail() const;
  // Parallel to `tail()`: each instance's tag, as its generator gave it (the ruins' 1 is rubble — a
  // debris member of a building in sections, a fallen block of one in blocks — and 0 wall). What
  // engine-view's handover pass splits a pop by.
  std::span<const u8> tail_tags() const;
  const PlacementTilesStats& stats() const noexcept { return stats_; }
  const PlacementTilesConfig& config() const noexcept { return config_; }
  // Whether some streamed entry puts anything on `tile`.
  bool occupied(TileCoord tile) const noexcept;
  // Whether some entry on `tile` draws rings `from` and `to` differently (its generator's
  // `representation`): a ring change it builds the tile again for, which is a handover a host can
  // measure. A generator with no `representation` draws every ring its own.
  bool redraws(TileCoord tile, u8 from, u8 to) const noexcept;
  // The bytes of instance and tag room the tile blocks hold, wherever a block is at the moment: the
  // finite entries' exact per-tile room made at load, the active tiles' and the free list's. It is
  // about the authored instance count times (sizeof(SceneInstance) + 1), the world over, resident
  // for the whole run (world.md, "Finite placement storage"). Excludes the tail and its scratch.
  u64 room_bytes() const noexcept;
  // The instances one active tile holds; empty for another tile.
  std::span<const renderer::SceneInstance> tile_instances(TileCoord tile) const noexcept;
  // Where each active tile's instances sit in the tail, in tile order: what a caller swapping one
  // tile's instances for others needs (engine-view's handover pass).
  struct TileRange {
    u64 key = 0;
    u32 first = 0;
    u32 count = 0;
    // The ring the tile is drawn in: what the renderer's ray tracing chain drops by, farthest first
    // (renderer.md, "The ray tracing chain's memory").
    u8 ring = 0;
    // The tile's own instances, `count` of them, as the consumer holds them; and their version: a
    // number that changes whenever the tile's instances are built again, never reused (W81). What
    // a run sink hands the renderer as a `renderer::DynamicBlock`, so a tile no update changed is
    // neither copied nor compared.
    const renderer::SceneInstance* instances = nullptr;
    u64 version = 0;
  };
  void tile_ranges(Vector<TileRange>& out) const;
  // **The hand-over that costs the change** (W81; world.md, "The consumers"): with a run sink set,
  // a commit hands it the drawn tiles' runs — key, place, count, ring, own instances and version —
  // and writes no tail; the instance sink above is not called. The runs of the last commit stay
  // readable (`runs`) until the next.
  using RunSink = bool (*)(void* context, std::span<const TileRange> runs, std::string* error);
  // **The tail's room goes with it** (T131, the memory row's finding): `create` reserves the tail
  // for every authored instance of every entry, for a consumer that writes it each commit; with a
  // run sink it is written only when `tail()` is asked (a handover pass, a game's props) and grows
  // to what the drawn tiles hold. Kept, the reserve committed 64.6 MiB on the forest staging and
  // touched 0.1.
  void set_run_sink(RunSink sink, void* context) noexcept {
    run_sink_ = sink;
    run_sink_context_ = context;
    if (sink != nullptr) {
      Vector<renderer::SceneInstance>().swap(tail_);
      Vector<u8>().swap(tail_tags_);
    }
  }
  std::span<const TileRange> runs() const noexcept { return {runs_.data(), runs_.size()}; }
  // Whether the pair budget left an active tile's placements out of the tail at the last commit.
  bool withheld(TileCoord tile) const noexcept;
  // **A moving ground** (world.md, "The consumers"; scatter.md, "A moving ground"): the time the
  // host's surface stands at. When it is not the one the held tiles were stood on, up to `budget`
  // of them — innermost ring first, then nearest, then in tile order — are built again with the
  // surface at that time as `scene_gen::Context::stand`, the same places, turns and scales standing
  // on the sand where it now is, and the tail is handed on. Returns how many were built.
  u32 restand(f64 surface_time_s, u32 budget);
  f64 stand_time() const noexcept { return stand_time_; }

  // **The build budget** (W82; world.md, "The consumers"): with `budget` above zero, a tile that
  // activates — or moves to a ring that draws it differently — is not built in the event: it is
  // held as due, and each commit builds at most `budget` due tiles, by ring, innermost first, then
  // nearest, then in tile order (the pair budget's order), before it admits the tail. A due tile
  // draws what it held (a ring change's old representation) or nothing (a new tile) until its
  // build. Zero, the default, builds every tile in its event, as before. Which tiles a commit
  // builds is a function of the tiles held, their rings, the observers and the budget, never of
  // timing, so the tail is too. A host brackets each update of the world with `begin_update`, the
  // budget for it — zero for an update that must be whole (a restart, a capture's complete fill),
  // the tunable's otherwise — and `end_update`, which commits the next due tiles when the world
  // committed nothing (it commits only a consumer an event touched, and a camera that stopped
  // after a burst touches none). Returns the tiles `end_update` built.
  void begin_update(u32 budget) noexcept {
    build_budget_ = budget;
    committed_ = false;
  }
  u32 end_update();
  u32 build_budget() const noexcept { return build_budget_; }

  // **A query's tile** (world.md "The consumers", "Placements answered from the scene"; apps.md
  // "The game contract"; roadmap P44): the tile's placements as a function of the scene alone — the
  // first `entries` streamed entries in the scene's order, in the **innermost ring's**
  // representation, standing on the scene's own ground at its own time — generated on the first ask
  // into a cache keyed by the tile and kept until `trim_query_tiles` drops it; a tile cached with
  // fewer entries than asked is generated again with more. **Never the streamed block**: a held
  // tile is in whatever ring it is in and re-stood on the moving sand a few tiles an update, so
  // what it holds depends on how the ring got there and when; this does not, so a module's
  // `nearest_placement` answers the same in a recording, its replay and a capture. Empty for a tile
  // no entry occupies. `entries_for` is how many entries a kind needs: through the last entry of
  // that generator, since a later entry is handed the earlier ones' footprints and none after.
  std::span<const renderer::SceneInstance> query_tile(TileCoord tile, u32 entries);
  u32 entries_for(std::string_view generator) const noexcept;
  // Drops the cached query tiles farther than `reach` tiles (Chebyshev) from `centre`, which is
  // what bounds the cache: (2 · reach + 1)² tiles at most.
  void trim_query_tiles(TileCoord centre, i32 reach);
  u32 query_tile_count() const noexcept { return query_tiles_.size(); }

 private:
  struct Entry;
  struct TileBlock {
    u8 ring = k_inactive;
    bool withheld = false;  // held, and left out of the tail by the pair budget
    // With a build budget: its instances are not its ring's yet (`due`), and it was never built
    // since it activated, so it holds none (`fresh`) and is not drawn.
    bool due = false;
    bool fresh = false;
    // **Its last build waits for its meshes** (world.md, "A tile's meshes: residency by
    // reference"): `instances` name a mesh that is not resident yet, so the tile draws what it
    // drew before the build — `held_*`, nothing for a tile that had nothing — until every mesh it
    // names is, and then the build is what it draws. Never set for a tile whose generators hand no
    // mesh, which draws its build at once as it always did.
    bool waiting = false;
    bool starved = false;  // waiting, and the mesh room refused it at the last commit
    // The pair budget's memory of it (T131): drawn at the last commit; let go by the budget once
    // since it was held, and the commit it was let go at.
    bool admitted = false;
    bool released = false;
    u64 released_at = 0;
    // Changed whenever it is made due, so a job's result for an older ask is known and let go.
    u64 serial = 0;
    u32 pairs = 0;                              // of what it draws
    u32 buildings = 0;                          // of its last build
    Vector<renderer::SceneInstance> instances;  // its last build
    Vector<u8> tags;                            // parallel to `instances`
    Vector<u32> tickets;  // the mesh slots `instances` name, a reference each (`TileMeshes`)
    Vector<renderer::SceneInstance> held_instances;  // drawn while `waiting`
    Vector<u8> held_tags;
    Vector<u32> held_tickets;
    u32 held_buildings = 0;
    f64 stood = 0.0;  // the surface time it was built standing on
    u64 version = 0;  // of what it draws; changed whenever that changes (`TileRange::version`)
    std::span<const renderer::SceneInstance> drawn() const noexcept {
      return waiting ? std::span<const renderer::SceneInstance>(held_instances.data(),
                                                                held_instances.size())
                     : std::span<const renderer::SceneInstance>(instances.data(), instances.size());
    }
    u32 drawn_buildings() const noexcept { return waiting ? held_buildings : buildings; }
  };
  // The tile's meshes (`set_meshes`), and a commit's pass over the tiles that wait for them.
  TileMeshes meshes_;
  Vector<u32> handed_;  // build's scratch: the slot of each mesh the entry's tile call handed
  void stage(TileBlock& block);     // before a build: what it draws now is kept as `held_*`
  void finalize(TileBlock& block);  // its build is what it draws from here
  u32 count_pairs(const TileBlock& block) const noexcept;
  void admit_meshes();
  void release_tickets(Vector<u32>& tickets) noexcept;

  static bool activate(void* context, const TileEvent& event);
  static bool change_ring(void* context, const TileEvent& event);
  static void deactivate(void* context, const TileEvent& event);
  static void commit(void* context);
  // `scene_ground`: stand on the scene's own ground at its own time, never the re-stood surface
  // (a query's tile). `entries`: how many of the entries to run, in the scene's order.
  scene_gen::Context context_for(const Entry& entry, u8 ring,
                                 bool scene_ground = false) const noexcept;
  // The same, standing on `stand` (null: the scene's ground) with `earlier` the footprints of the
  // entries before this one on the tile: what a job asks its generators with.
  scene_gen::Context context_in(const Entry& entry, u8 ring, const renderer::TerrainSampler* stand,
                                std::span<const scene_gen::Footprint> earlier) const noexcept;

  // ---- one tile's build, in two halves (T131) --------------------------------------------------
  // The generators' half (`generate`) reads only what `create` made and the entries' own state, so
  // it runs on the committing thread or on a job; the hand-over (`install`) names the tile's meshes
  // in the table and decides what the tile draws, on the committing thread.
  struct MeshRef {
    u32 entry = 0;  // whose `placed.meshes`
    u32 index = 0;
  };
  struct Generated {
    bool ok = true;
    u32 buildings = 0;
    f64 stood = 0.0;
    // The meshes the tile's entries handed, in the order they handed them: an instance of one names
    // `k_handed | k` until `install` gives it the mesh's slot. A job copies them (`copies`), since
    // its entries' scratch moves on to the next tile.
    Vector<MeshRef> meshes;
    Vector<scene_gen::PlacementMesh> copies;
    u32 copied = 0;  // of `copies`, the ones that are this build's
    u64 built[k_placement_kinds] = {};
    i64 build_ns[k_placement_kinds] = {};
    u64 seated = 0;
    u64 lowered = 0;
    i64 seat_ns = 0;
    i64 ns = 0;  // the whole half's wall time
    void reset() noexcept;
  };
  static constexpr u32 k_handed = 0x80000000u;
  // Seats a placement standing on the ground by its mesh's seat (G201 stage 2; `ground_`, the
  // scene's own ground at its own time, as the load's): nothing for a mesh with none or a fit.
  void seat(renderer::SceneInstance& instance, Generated& counts) const;
  bool generate(TileCoord tile, u8 ring, u32 entries, bool for_query,
                const renderer::TerrainSampler* stand, Vector<scene_gen::Footprint>& earlier,
                Vector<renderer::SceneInstance>& instances, Vector<u8>& tags, Generated& out) const;
  // Its meshes named, its handed instances given their slots, and the build is what it draws (or
  // waits for its meshes); the generators' counts added to the stats.
  bool install(TileBlock& block, Generated& built);
  bool build(TileCoord tile, u8 ring, TileBlock& out, u32 entries = ~0u, bool for_query = false);
  Generated inline_;  // the committing thread's half

  // ---- the job (T131) --------------------------------------------------------------------------
  struct Job;
  std::unique_ptr<Job> job_;
  jobs::JobSystem* build_jobs_ = nullptr;
  static void run_job(void* data) noexcept;
  bool on_jobs() const noexcept { return build_jobs_ != nullptr && build_budget_ != 0; }
  void collect(bool wait);  // takes a finished job's tiles (waiting for it with `wait`)
  void start_job();         // the next due tiles, in the pair budget's order
  void mark_due(TileBlock& block) noexcept;
  u64 next_serial_ = 0;
  u64 commits_ = 0;  // admissions made: what a hold-off is counted in
  // The surface a job in flight stands its tiles on, kept until the job is collected when
  // `restand` moves on (`stand_` is the newest).
  std::unique_ptr<renderer::TerrainSampler> job_stand_;
  bool take(const TileEvent& event);
  void recycle(u64 key, TileBlock&& block);
  void build_due();
  void commit_tail();  // a commit past `build_due`: the admission, the runs, the sink
  void admit();
  u32 build_budget_ = 0;
  bool committed_ = false;  // this update's world committed (`begin_update`, `end_update`)
  Vector<u64> emptied_;     // build_due's: tiles whose build came out empty
  // The query cache: a tile's instances and how many entries made them.
  struct QueryTile {
    u32 entries = 0;
    Vector<renderer::SceneInstance> instances;
  };
  FlatMap<u64, QueryTile> query_tiles_;
  TileBlock query_scratch_;

  const renderer::SceneData* scene_ = nullptr;
  PlacementTilesConfig config_;
  // The world block the generators see (`scene_gen::Context::world`): the config's rings, with
  // the scene's radii.
  scene::WorldRings world_rings_;
  std::unique_ptr<renderer::TerrainSampler> ground_;
  Vector<std::unique_ptr<Entry>> entries_;
  FlatMap<u64, TileBlock> tiles_;
  // Finite entries' exact per-tile room, made at load and exchanged with the active map. The
  // empty record stays in the pool while its tile is active. Other generators reuse free blocks.
  FlatMap<u64, TileBlock> spare_tiles_;
  Vector<TileBlock> free_blocks_;
  // Written by a commit with no run sink, and otherwise on the first ask after one (	ail).
  mutable Vector<renderer::SceneInstance> tail_;
  mutable Vector<u8> tail_tags_;
  mutable bool tail_stale_ = false;
  void write_tail() const;
  Vector<TileRange> runs_;  // the last commit's, for a run sink
  u64 next_version_ = 0;
  RunSink run_sink_ = nullptr;
  void* run_sink_context_ = nullptr;
  u32 tail_pairs_ = 0;
  const TileRing* ring_ = nullptr;
  struct Rank {
    u8 ring = 0;
    f32 score = 0.0f;
    u64 key = 0;
    u32 index = 0;
  };
  Vector<Rank> ranks_;    // commit's scratch
  Vector<Rank> waiting_;  // admit_meshes' scratch: the tiles that wait, nearest first
  // The farthest drawn tile past `wanting` that names a mesh gives its meshes up (it is built
  // again, and waits). False: there is none, and `wanting` is the one withheld.
  bool yield_farthest(const Rank& wanting);
  u32 mesh_refusals_logged_ = 0;
  InstanceSink sink_ = nullptr;
  void* sink_context_ = nullptr;
  PlacementTilesStats stats_;
  // Each scene mesh's reach on the ground (`renderer::ground_radius`), for the footprints a later
  // entry is handed; and the footprints of the entries before the one being built, one tile's.
  Vector<f32> radius_of_;
  Vector<scene_gen::Footprint> earlier_;
  // The surface the tiles stand on now, when the host's ground has moved on from the scene's time.
  std::unique_ptr<renderer::TerrainSampler> stand_;
  f64 stand_time_ = 0.0;
  // The ground before its cuts, for a generator that plans them (`scene_gen::Context::uncut`).
  std::unique_ptr<renderer::UncutGround> uncut_;
  scene_gen::Context::UncutGround uncut_hook_;
};

}  // namespace engine::world
