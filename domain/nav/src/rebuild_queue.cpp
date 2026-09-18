// The asynchronous rebuild queue. The interesting part is not the job plumbing but where the
// boundaries are: `request` is the only place geometry is copied, `build_tile` is the only thing
// that runs on a worker, and `apply_ready` is the only place a NavMesh is touched. Everything
// between them is a list under a mutex, held for the length of a push or a pop and never across
// a build.

#include "nav_log.h"

#include <core/base/assert.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/time/time.h>
#include <domain/nav/rebuild_queue.h>
#include <domain/nav/region_graph.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace engine::nav {
namespace {

u64 this_thread_id() noexcept {
  return static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

struct Pending {
  TileCoord coord;
  Vector<Vec3> vertices;
  Vector<u32> indices;
  Vector<u8> areas;
  f32 distance = 0.0f;  // to the nearest observer
  u32 passes = 0;       // dispatch rounds this tile has been passed over
  u64 sequence = 0;     // request order, so equal scores resolve the same way every run
};

struct Ready {
  NavTileData tile;
  TileCoord coord;
  Status status = Status::Ok;
};

}  // namespace

struct RebuildQueue::Impl {
  RebuildQueueOptions options;
  u32 max_in_flight = 1;

  mutable std::mutex mutex;
  std::condition_variable idle;
  Vector<Pending> pending;
  HashMap<u64, u32> pending_index;  // tile key -> index in `pending`
  Vector<Ready> ready;
  Vector<Vec3> observers;
  u32 in_flight = 0;
  u64 sequence = 0;
  bool stopping = false;
  RebuildQueueStats stats;

  // A unit of work handed to a worker. Owned by the job while it runs and deleted by it: the
  // queue never touches a task's geometry once it has been taken.
  struct Task {
    Impl* owner = nullptr;
    TileCoord coord;
    Vector<Vec3> vertices;
    Vector<u32> indices;
    Vector<u8> areas;
  };

  f32 score_of(const Pending& entry) const noexcept {
    return entry.distance - static_cast<f32>(entry.passes) * options.starvation_relief;
  }

  void erase_pending(u32 index);
  Task* take_locked();
  void pump();
  static void build(Task* task);  // the body: build, record, release the slot
  static void run(void* data);    // the job entry point: build, then keep the pipeline full
};

void RebuildQueue::Impl::erase_pending(u32 index) {
  const u32 last = pending.size() - 1;
  pending_index.erase(tile_key(pending[index].coord));
  if (index != last) {
    pending[index] = std::move(pending[last]);
    pending_index.insert_or_assign(tile_key(pending[index].coord), index);
  }
  pending.pop_back();
}

RebuildQueue::Impl::Task* RebuildQueue::Impl::take_locked() {
  if (stopping || in_flight >= max_in_flight || pending.empty()) return nullptr;

  u32 best = 0;
  f32 best_score = score_of(pending[0]);
  for (u32 i = 1; i < pending.size(); ++i) {
    const f32 score = score_of(pending[i]);
    if (score < best_score ||
        (score == best_score && pending[i].sequence < pending[best].sequence)) {
      best = i;
      best_score = score;
    }
  }
  // Everything that was not chosen gets one step closer to the front. Without this a queue kept
  // busy by tiles near an observer would never build a far one at all, and a destroyed bridge on
  // the other side of the map would stay unwalkable forever.
  for (u32 i = 0; i < pending.size(); ++i) {
    if (i != best) pending[i].passes += 1;
  }

  auto* task = new Task();
  task->owner = this;
  task->coord = pending[best].coord;
  task->vertices = std::move(pending[best].vertices);
  task->indices = std::move(pending[best].indices);
  task->areas = std::move(pending[best].areas);
  erase_pending(best);
  ++in_flight;
  return task;
}

void RebuildQueue::Impl::build(Task* task) {
  Impl* self = task->owner;

  TileGeometry geometry;
  geometry.vertices = std::span<const Vec3>(task->vertices.data(), task->vertices.size());
  geometry.indices = std::span<const u32>(task->indices.data(), task->indices.size());
  geometry.areas = std::span<const u8>(task->areas.data(), task->areas.size());

  Ready result;
  result.coord = task->coord;
  const i64 started = time::monotonic_ns();
  result.status = build_tile(self->options.build, task->coord, geometry, result.tile, nullptr);
  const i64 elapsed = time::monotonic_ns() - started;
  const u64 thread = this_thread_id();
  delete task;

  {
    std::lock_guard<std::mutex> guard(self->mutex);
    self->stats.built += 1;
    self->stats.build_ns += static_cast<u64>(elapsed < 0 ? 0 : elapsed);
    self->stats.last_build_thread = thread;
    if (result.status == Status::Ok) {
      self->ready.push_back(std::move(result));
    } else {
      self->stats.failed += 1;
    }
    ENGINE_ASSERT(self->in_flight > 0, "nav: a build finished that was never dispatched");
    self->in_flight -= 1;
  }
  self->idle.notify_all();
}

void RebuildQueue::Impl::run(void* data) {
  auto* task = static_cast<Task*>(data);
  Impl* self = task->owner;
  build(task);
  // Keep the pipeline full from the worker that just freed a slot, rather than waiting for the
  // next request to notice.
  self->pump();
}

void RebuildQueue::Impl::pump() {
  for (;;) {
    Task* task = nullptr;
    {
      std::lock_guard<std::mutex> guard(mutex);
      task = take_locked();
    }
    if (task == nullptr) return;
    if (options.job_system != nullptr) {
      // The efficiency pool: a rebuild is latency-tolerant (the old tile stays walkable until the
      // new one is applied) and a frame is not, so a rebuild must never take a performance
      // worker. The job system runs a job inline on the submitter when its queues are full,
      // which is backpressure rather than a dropped tile.
      options.job_system->schedule(jobs::Pool::Efficiency, jobs::Job{&Impl::run, task, nullptr});
      continue;
    }
    // No job system: build here, in this loop rather than through `run`, so draining a queue of
    // a hundred tiles is a hundred iterations and not a hundred stack frames.
    build(task);
  }
}

// --- the public shell -------------------------------------------------------------------------

RebuildQueue::RebuildQueue() noexcept = default;

RebuildQueue::~RebuildQueue() { shutdown(); }

Status RebuildQueue::init(const RebuildQueueOptions& options) {
  if (impl_ != nullptr) return Status::InvalidArgument;
  if (!validate(options.build)) return Status::InvalidArgument;
  if (options.max_pending == 0) return Status::InvalidArgument;

  auto* impl = new Impl();
  impl->options = options;
  impl->max_in_flight = options.max_in_flight;
  if (impl->max_in_flight == 0) {
    impl->max_in_flight = options.job_system != nullptr
                              ? options.job_system->worker_count(jobs::Pool::Efficiency)
                              : 1;
    if (impl->max_in_flight == 0) impl->max_in_flight = 1;
  }
  impl_ = impl;
  return Status::Ok;
}

void RebuildQueue::shutdown() noexcept {
  if (impl_ == nullptr) return;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->stopping = true;
    impl_->pending.clear();
    impl_->pending_index.clear();
  }
  // In-flight builds hold a Task and will touch `impl_` when they finish, so the destructor waits
  // for them. There is no way to cancel a Recast build mid-pipeline, and a queue that freed
  // itself under a running one would be a use-after-free rather than a fast shutdown.
  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->idle.wait(lock, [this] { return impl_->in_flight == 0; });
  }
  delete impl_;
  impl_ = nullptr;
}

const RebuildQueueOptions& RebuildQueue::options() const noexcept {
  static const RebuildQueueOptions k_empty;
  return impl_ != nullptr ? impl_->options : k_empty;
}

Status RebuildQueue::request(const RebuildRequest& request) {
  if (impl_ == nullptr) return Status::NotFound;
  if (request.indices.size() % 3 != 0) return Status::InvalidArgument;
  if (!request.areas.empty() && request.areas.size() != request.indices.size() / 3)
    return Status::InvalidArgument;

  const u64 key = tile_key(request.coord);
  Status result = Status::Ok;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->stopping) return Status::Unsupported;

    u32* existing = impl_->pending_index.find_value(key);
    if (existing == nullptr) {
      if (impl_->pending.size() >= impl_->options.max_pending) {
        // Drop the least urgent pending tile, which may be this one. Its mesh stays as it was:
        // stale, and walkable, which beats an unbounded queue during a collapse.
        u32 worst = 0;
        f32 worst_score = impl_->score_of(impl_->pending[0]);
        for (u32 i = 1; i < impl_->pending.size(); ++i) {
          const f32 score = impl_->score_of(impl_->pending[i]);
          if (score > worst_score) {
            worst = i;
            worst_score = score;
          }
        }
        impl_->erase_pending(worst);
        impl_->stats.evicted += 1;
        result = Status::LimitReached;
      }
      impl_->pending.push_back(Pending{});
      const u32 index = impl_->pending.size() - 1;
      impl_->pending_index.insert_or_assign(key, index);
      impl_->pending[index].coord = request.coord;
      impl_->pending[index].sequence = impl_->sequence++;
      existing = impl_->pending_index.find_value(key);
      impl_->stats.requested += 1;
    } else {
      // The coalescing rule: a tile requested twice before it builds is built once, with the
      // second request's geometry. Anything else would build a tile whose result is already
      // known to be wrong.
      impl_->stats.coalesced += 1;
    }

    Pending& entry = impl_->pending[*existing];
    entry.vertices.assign(request.vertices.begin(), request.vertices.end());
    entry.indices.assign(request.indices.begin(), request.indices.end());
    entry.areas.assign(request.areas.begin(), request.areas.end());

    // Score it against the observers we have. A tile with no observers scores 0 and is as urgent
    // as anything else, which is the right answer for a level load.
    f32 nearest = 0.0f;
    if (!impl_->observers.empty()) {
      const Aabb3 box = tile_bounds(impl_->options.build, request.coord, false);
      const Vec3 centre = box.center();
      nearest = 3.4e38f;
      for (const Vec3 observer : impl_->observers) {
        const f32 d = distance(Vec3(observer.x, centre.y, observer.z), centre);
        if (d < nearest) nearest = d;
      }
    }
    entry.distance = nearest;
  }
  return result;
}

void RebuildQueue::dispatch() {
  if (impl_ == nullptr) return;
  impl_->pump();
}

void RebuildQueue::set_observers(std::span<const Vec3> observers) {
  if (impl_ == nullptr) return;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->observers.assign(observers.begin(), observers.end());
  for (Pending& entry : impl_->pending) {
    f32 nearest = 0.0f;
    if (!impl_->observers.empty()) {
      const Aabb3 box = tile_bounds(impl_->options.build, entry.coord, false);
      const Vec3 centre = box.center();
      nearest = 3.4e38f;
      for (const Vec3 observer : impl_->observers) {
        const f32 d = distance(Vec3(observer.x, centre.y, observer.z), centre);
        if (d < nearest) nearest = d;
      }
    }
    entry.distance = nearest;
  }
}

u32 RebuildQueue::ready_count() const noexcept {
  if (impl_ == nullptr) return 0;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->ready.size();
}

u32 RebuildQueue::apply_ready(NavMesh& mesh, RegionGraph* graph) {
  if (impl_ == nullptr) return 0;
  // One call per tick starts what is urgent and applies what finished. With no job system this
  // runs the builds too, so a tool's loop is request-then-apply and nothing else.
  impl_->pump();
  Vector<Ready> batch;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->ready.empty()) {
      impl_->stats.last_apply_thread = this_thread_id();
      return 0;
    }
    batch = std::move(impl_->ready);
    impl_->ready.clear();
  }

  // Tile order, not completion order. This is the whole determinism claim of the queue: whatever
  // the workers did, the mesh ends a frame in the state it would have ended in if every build had
  // run on one thread in tile order (ADR-0010, plan 05 §5.15's "ordered by tile, never by
  // completion time").
  std::sort(batch.begin(), batch.end(),
            [](const Ready& a, const Ready& b) { return tile_key(a.coord) < tile_key(b.coord); });

  u32 applied = 0;
  for (Ready& entry : batch) {
    if (entry.tile.walkable()) {
      if (mesh.add_tile(entry.tile) != Status::Ok) continue;
      if (graph != nullptr) graph->set_tile(entry.tile);
    } else {
      mesh.remove_tile(entry.coord);
      if (graph != nullptr) graph->remove_tile(entry.coord);
    }
    ++applied;
  }

  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->stats.applied += applied;
  impl_->stats.last_apply_thread = this_thread_id();
  return applied;
}

void RebuildQueue::wait_idle() {
  if (impl_ == nullptr) return;
  for (;;) {
    impl_->pump();
    {
      std::unique_lock<std::mutex> lock(impl_->mutex);
      impl_->idle.wait(lock, [this] { return impl_->in_flight == 0; });
      if (impl_->pending.empty()) return;
    }
    // A worker freed the last slot while tiles were still pending; go round again rather than
    // returning with work in the queue.
  }
}

RebuildQueueStats RebuildQueue::stats() const noexcept {
  RebuildQueueStats out;
  if (impl_ == nullptr) return out;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  out = impl_->stats;
  out.pending = impl_->pending.size();
  out.in_flight = impl_->in_flight;
  out.ready = impl_->ready.size();
  return out;
}

}  // namespace engine::nav
