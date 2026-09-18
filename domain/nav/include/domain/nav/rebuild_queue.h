#pragma once

// Asynchronous tile rebuilds on core/jobs (plan 05 §5.8's "async Recast tile rebuild for
// affected tiles", §5.11).
//
// **The threading contract, in three sentences.** `request()` copies the geometry and returns
// immediately; it may be called from any thread. Builds run on the job system's **efficiency**
// pool, one tile per job, and touch nothing but their own copy of the input. Finished tiles wait
// in a ready list until `apply_ready()` — which may only be called from the thread that owns the
// NavMesh — installs them, in tile order, so the mesh's contents never depend on which worker
// finished first.
//
// **A request is queued, not started.** `dispatch()` starts what the priorities say should run,
// and `apply_ready()` and `wait_idle()` call it for a caller who only wants one call per tick.
// The separation is what makes the priority mean anything: if a request started a build on the
// spot, the first request of a frame would always take the free slot and a queue that never went
// empty would be served in arrival order, whatever the observers were doing.
//
// The efficiency pool rather than the performance pool, deliberately: a rebuild is latency-
// tolerant (nothing blocks on it; the old tile stays usable until the new one lands) and a frame
// is not, so a rebuild must never take a performance worker away from the frame. On a machine
// with no efficiency cores, core/jobs runs that pool at below-normal priority on performance
// CPUs, which is the same bargain (docs/subsystems/jobs.md).
//
// **A tile requested twice before it builds is built once**, with the input of the second
// request: the pending set is keyed by tile, and a second request replaces the first's geometry
// rather than queueing a build whose result would immediately be thrown away. A tile requested
// again while it is already building is queued afresh, because its in-flight build is reading
// the older geometry and there is no way to tell it otherwise.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/nav/nav_mesh.h>
#include <domain/nav/tile.h>
#include <domain/nav/types.h>

#include <span>

namespace engine::jobs {
class JobSystem;
}

namespace engine::nav {

class RegionGraph;

struct RebuildQueueOptions {
  NavBuildParams build;

  // Jobs run on the engine's efficiency pool. A null job system builds inline inside
  // `request()`, which is the right answer for tools and for tests that want one thread.
  jobs::JobSystem* job_system = nullptr;

  // How many tiles may be in flight at once. 0 means the efficiency pool's worker count. More
  // than that only queues work behind itself and holds more geometry copies alive.
  u32 max_in_flight = 0;

  // The pending cap (ADR-0017). A request past it evicts the lowest-priority pending tile and
  // returns `Status::LimitReached`, rather than growing without bound while a destruction event
  // storm runs; the evicted tile keeps its old mesh, which is stale but walkable.
  u32 max_pending = 256;

  // The starvation guard. A pending tile's score is its distance to the nearest observer minus
  // `starvation_relief` per dispatch round it was passed over, so a tile 500 m away that has
  // been skipped fifty times at the default scores as if it were at 250 m and eventually wins.
  // Without it a queue kept busy by tiles near an observer never builds a far one at all.
  f32 starvation_relief = 5.0f;
};

struct RebuildQueueStats {
  u64 requested = 0;  // calls to request() that were accepted
  u64 coalesced = 0;  // requests that replaced a pending one instead of adding work
  u64 built = 0;      // build_tile() calls that ran
  u64 applied = 0;    // tiles installed into a NavMesh
  u64 evicted = 0;    // pending tiles dropped at the cap
  u64 failed = 0;     // builds the backend refused
  u64 build_ns = 0;   // summed wall time inside build_tile()
  u32 pending = 0;
  u32 in_flight = 0;
  u32 ready = 0;
  // Which thread last ran a build and which last ran apply_ready(), hashed the way
  // std::hash<std::thread::id> does. Telemetry, and what the threading contract above is tested
  // with: the two must differ whenever there is a job system with workers.
  u64 last_build_thread = 0;
  u64 last_apply_thread = 0;
};

// One tile's worth of work. The spans are copied into the queue and not kept.
struct RebuildRequest {
  TileCoord coord;
  std::span<const Vec3> vertices;
  std::span<const u32> indices;
  std::span<const u8> areas;  // per triangle; empty derives them from the slope
};

class RebuildQueue {
 public:
  RebuildQueue() noexcept;
  ~RebuildQueue();
  ENGINE_NON_COPYABLE(RebuildQueue);

  Status init(const RebuildQueueOptions& options);
  // Waits for every in-flight build, then releases everything. Ready tiles that were never
  // applied are dropped.
  void shutdown() noexcept;
  bool initialized() const noexcept { return impl_ != nullptr; }
  const RebuildQueueOptions& options() const noexcept;

  // Copies the geometry and queues the tile. Any thread. Nothing starts building until
  // `dispatch()`, so a frame's worth of destruction events can be collected and then ordered.
  Status request(const RebuildRequest& request);

  // Starts as many builds as there are free slots, highest priority first. With no job system
  // this *is* the build: it returns when the queue is empty.
  void dispatch();

  // The observer positions priorities are measured from (ADR-0010's observer set). Re-scores
  // every pending tile, so it is called once per tick and not per request.
  void set_observers(std::span<const Vec3> observers);

  // Dispatches, then installs finished tiles, in tile order, on the calling thread. Returns how
  // many were installed. `graph`, when given, is updated from the same bytes in the same call,
  // which is what keeps the coarse tier in sync with the detailed one by construction.
  //
  // One call per tick is the intended use: it starts what the observers made urgent and applies
  // what finished since last time.
  u32 apply_ready(NavMesh& mesh, RegionGraph* graph = nullptr);
  u32 ready_count() const noexcept;

  // Blocks until nothing is in flight and nothing is pending. For tests, tools, and a level load
  // that wants the mesh complete before the first frame; never call it from a frame.
  void wait_idle();

  RebuildQueueStats stats() const noexcept;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace engine::nav
