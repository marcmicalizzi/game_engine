// Experiment E6, the ECS half (docs/plan/10-roadmap-risks.md §10.5, docs/experiments/
// e6-ecs-store.md): flecs at 10^5 entities with relationships and a hierarchy, four systems a
// tick, worker scaling on the engine's job pool, query latency, churn, and memory.
//
// Debug builds only smoke-run benchmarks (docs/subsystems/bench.md), so the debug world is small
// enough for CTest; every number that reaches the write-up comes from msvc-release at 100,000.

#include <core/base/macros.h>
#include <core/jobs/job_system.h>
#include <core/platform/topology.h>
#include <domain/ecs/os_api.h>
#include <domain/ecs/sim_world.h>
#include <foundation/bench/bench.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#if ENGINE_PLATFORM_WINDOWS
#define PSAPI_VERSION 2
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <cstdlib>
#endif

using namespace engine;

namespace {

#if ENGINE_DEBUG
constexpr u32 k_entities = 5'000;
#else
constexpr u32 k_entities = 100'000;
#endif
constexpr u32 k_factions = 32;
constexpr u32 k_parents = k_entities / 100;  // 10% of entities are parented, ~10 children each
constexpr u32 k_children = k_entities / 10;
constexpr u32 k_observers = 3;
constexpr f32 k_world_size = 4096.0f;
constexpr u32 k_fine_grid = 32;  // 32x32 cells of 128 units: plan 03 §3.7's tile size
constexpr u32 k_coarse_grid = 8;
constexpr u32 k_churn = k_entities / 100;

// ---- components ------------------------------------------------------------------------------

struct Position {
  f32 x = 0;
  f32 y = 0;
};
struct Velocity {
  f32 x = 0;
  f32 y = 0;
};
struct WorldPos {
  f32 x = 0;
  f32 y = 0;
};
struct Wealth {
  f32 value = 0;
};
struct Importance {
  f32 value = 1;
};
// The LOD tier and the hysteresis state that keeps it from thrashing (plan 05 §5.4).
struct Tier {
  u8 value = 3;
  u8 candidate = 3;
  u8 dwell = 0;
};
// The coarse spatial grid as a plain component: an integer the query can reject on before it
// does any arithmetic. It does not change what flecs iterates — see the query benchmarks.
struct Cell {
  u32 index = 0;
};
struct FactionIndex {
  u32 value = 0;
};
// The relationship. (FactionOf, ironbound) is a pair, so the archetype carries the membership and
// the aggregate reads one table per faction instead of chasing a pointer per entity.
struct FactionOf {};
// The coarse grid as a *relationship*, for the third query benchmark: it puts the cell in the
// archetype, which is what actually narrows a query, at the cost of fragmenting the table set.
struct InCell {};

struct Observer {
  f32 x = 0;
  f32 y = 0;
  f32 weight = 1;
};
struct ObserverSet {
  Observer observers[k_observers];
};

// Per-faction totals. Written by one single-threaded system; see the write-up for why that is
// the interesting part of the measurement rather than an oversight.
struct Aggregate {
  f32 wealth[k_factions] = {};
  u32 count[k_factions] = {};
};

// ---- environment -----------------------------------------------------------------------------

u64 working_set_bytes() {
#if ENGINE_PLATFORM_WINDOWS
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) return 0;
  return static_cast<u64>(counters.WorkingSetSize);
#else
  std::FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) return 0;
  unsigned long long total = 0;
  unsigned long long resident = 0;
  const int read = std::fscanf(file, "%llu %llu", &total, &resident);
  std::fclose(file);
  if (read != 2) return 0;
  return static_cast<u64>(resident) * 4096ull;
#endif
}

const char* build_preset() {
#if ENGINE_DEBUG
  return "debug";
#else
  return "release";
#endif
}

// One line of environment at the top of the run, so a result file says which machine and which
// build produced it. Tool output, which is the documented exception to structured logging.
void report_environment_once() {
  static bool done = false;
  if (done) return;
  done = true;
  const platform::Topology& topo = platform::topology();
  char description[512];
  platform::describe_topology(topo, description, sizeof(description));
  std::printf("# e6 ecs: %s\n", description);
  std::printf("# e6 ecs: build=%s entities=%u factions=%u children=%u flecs=%d.%d.%d\n",
              build_preset(), k_entities, k_factions, k_children, FLECS_VERSION_MAJOR,
              FLECS_VERSION_MINOR, FLECS_VERSION_PATCH);
  std::fflush(stdout);
}

// ---- the scene -------------------------------------------------------------------------------

// A deterministic 32-bit stream, so two runs build the same world.
struct Rng {
  u64 state = 0x9E3779B97F4A7C15ull;
  u32 next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>(state >> 33);
  }
  f32 unit() { return static_cast<f32>(next() % 100'000u) * 1.0e-5f; }
  f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
};

f32 tier_score(const ObserverSet& set, f32 x, f32 y, f32 importance) {
  f32 best = 1.0e30f;
  for (u32 i = 0; i < k_observers; ++i) {
    const f32 dx = x - set.observers[i].x;
    const f32 dy = y - set.observers[i].y;
    const f32 distance = std::sqrt(dx * dx + dy * dy);
    const f32 score = distance / (set.observers[i].weight * importance);
    if (score < best) best = score;
  }
  return best;
}

u8 tier_from_score(f32 score) {
  if (score < 64.0f) return 0;
  if (score < 256.0f) return 1;
  if (score < 1024.0f) return 2;
  return 3;
}

struct Scene {
  Scene(u32 workers, u32 grid_relationship_cells)
      : jobs(make_config(workers)), adapter(jobs), grid_cells(grid_relationship_cells) {
    build();
    ecs::set_workers(sim.world(), workers);
  }

  void report(u32 workers) const {
    std::printf("# e6 ecs: scene workers=%u grid_relationship=%u tables=%d\n", workers, grid_cells,
                static_cast<int>(sim.world().get_info()->table_count));
    std::fflush(stdout);
  }

  static jobs::JobSystemConfig make_config(u32 workers) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    return config;
  }

  void build();
  void build_systems();

  jobs::JobSystem jobs;
  ecs::JobOsApi adapter;
  ecs::SimWorld sim;
  u32 grid_cells;  // 0: no (InCell, cell) relationship on entities
  std::vector<flecs::entity> factions;
  std::vector<flecs::entity> cells;
  Aggregate aggregate;
};

void Scene::build() {
  flecs::world& world = sim.world();
  Rng rng;

  ObserverSet observers;
  for (u32 i = 0; i < k_observers; ++i) {
    observers.observers[i] = Observer{rng.range(0, k_world_size), rng.range(0, k_world_size),
                                      1.0f + static_cast<f32>(i)};
  }
  world.set<ObserverSet>(observers);

  factions.reserve(k_factions);
  for (u32 i = 0; i < k_factions; ++i) {
    factions.push_back(world.entity().set<FactionIndex>(FactionIndex{i}));
  }
  if (grid_cells != 0) {
    cells.reserve(static_cast<usize>(grid_cells) * grid_cells);
    for (u32 i = 0; i < grid_cells * grid_cells; ++i)
      cells.push_back(world.entity());
  }

  const auto place = [&](flecs::entity e, f32 x, f32 y, u32 faction) {
    const u32 cx = static_cast<u32>(x / (k_world_size / static_cast<f32>(k_fine_grid)));
    const u32 cy = static_cast<u32>(y / (k_world_size / static_cast<f32>(k_fine_grid)));
    e.set<Position>(Position{x, y})
        .set<Velocity>(Velocity{rng.range(-2.0f, 2.0f), rng.range(-2.0f, 2.0f)})
        .set<Wealth>(Wealth{rng.range(0.0f, 100.0f)})
        .set<Importance>(Importance{rng.range(0.5f, 4.0f)})
        .set<Cell>(Cell{cy * k_fine_grid + cx})
        .set<Tier>(Tier{})
        .add<FactionOf>(factions[faction]);
    if (grid_cells != 0) {
      const f32 size = k_world_size / static_cast<f32>(grid_cells);
      const u32 gx = static_cast<u32>(x / size);
      const u32 gy = static_cast<u32>(y / size);
      e.add<InCell>(cells[gy * grid_cells + gx]);
    }
  };

  for (u32 i = 0; i < k_entities - k_children - k_parents; ++i) {
    flecs::entity e = world.entity();
    place(e, rng.range(0, k_world_size), rng.range(0, k_world_size), rng.next() % k_factions);
    e.set<WorldPos>(WorldPos{});
  }

  // 10% of the world hangs off k_parents roots, one level deep: a settlement's props under its
  // settlement, which is the shape the transform propagation has to walk.
  //
  // A child takes its parent's faction rather than a random one, because the archetype is the
  // cross product of every pair an entity carries: random factions on children would give
  // k_parents x k_factions tables of one entity each, and the measurement would be about that
  // mistake rather than about flecs. See the write-up — it is measured there.
  std::vector<flecs::entity> parents;
  std::vector<u32> parent_faction;
  parents.reserve(k_parents);
  parent_faction.reserve(k_parents);
  for (u32 i = 0; i < k_parents; ++i) {
    flecs::entity e = world.entity();
    const u32 faction = rng.next() % k_factions;
    place(e, rng.range(0, k_world_size), rng.range(0, k_world_size), faction);
    e.set<WorldPos>(WorldPos{});
    parents.push_back(e);
    parent_faction.push_back(faction);
  }
  for (u32 i = 0; i < k_children; ++i) {
    flecs::entity e = world.entity();
    const u32 parent = i % k_parents;
    place(e, rng.range(-8.0f, 8.0f) + k_world_size * 0.5f,
          rng.range(-8.0f, 8.0f) + k_world_size * 0.5f, parent_faction[parent]);
    e.set<WorldPos>(WorldPos{}).child_of(parents[parent]);
  }

  build_systems();
}

void Scene::build_systems() {
  flecs::world& world = sim.world();

  // 1. Movement: the trivially parallel one, and the floor for what a tick can cost.
  world.system<Position, const Velocity>("move")
      .kind(sim.phase(ecs::TickPhase::Systems))
      .multi_threaded()
      .each([](Position& p, const Velocity& v) {
        p.x += v.x;
        p.y += v.y;
        if (p.x < 0.0f || p.x > k_world_size) p.x = k_world_size - p.x;
        if (p.y < 0.0f || p.y > k_world_size) p.y = k_world_size - p.y;
      });

  // 2. Hierarchy transform propagation. `.parent().cascade()` makes flecs order the tables so a
  // parent is always written before its children, which is why this can be one pass.
  //
  // Deliberately *not* multi_threaded: flecs parallelizes by splitting each table's rows across
  // stages, and it does not synchronize between tables, so a stage could read a parent's WorldPos
  // before the stage that owns that row has written it. Cascade orders tables, not workers.
  world.system<WorldPos, const Position, const WorldPos*>("propagate")
      .kind(sim.phase(ecs::TickPhase::Systems))
      .term_at(2)
      .parent()
      .cascade()
      .each([](WorldPos& w, const Position& p, const WorldPos* parent) {
        w.x = p.x;
        w.y = p.y;
        if (parent != nullptr) {
          w.x += parent->x;
          w.y += parent->y;
        }
      });

  // 3. Faction aggregation through the relationship. One table per (faction, archetype), so the
  // faction is read once per table and the inner loop is a contiguous sum. Single-threaded on
  // purpose: the totals are one array that every entity contributes to, and splitting it needs
  // per-worker partials plus a reduce. What that costs is the point of the measurement.
  Aggregate* totals = &aggregate;
  world.system<const Wealth>("faction_aggregate")
      .with<FactionOf>(flecs::Wildcard)
      .kind(sim.phase(ecs::TickPhase::Systems))
      .run([totals](flecs::iter& it) {
        for (u32 i = 0; i < k_factions; ++i) {
          totals->wealth[i] = 0;
          totals->count[i] = 0;
        }
        while (it.next()) {
          auto wealth = it.field<const Wealth>(0);
          const flecs::entity faction = it.pair(1).second();
          const u32 index = faction.get<FactionIndex>().value;
          f32 sum = 0;
          for (auto row : it)
            sum += wealth[row].value;
          totals->wealth[index] += sum;
          totals->count[index] += static_cast<u32>(it.count());
        }
      });

  // 4. LOD tier assignment with hysteresis: the minimum over the observer set of
  // f(distance, importance, weight), and a tier only changes after it has been the candidate for
  // two consecutive ticks (plan 05 §5.4, ADR-0010).
  world.system<Tier, const Position, const Importance>("lod_assign")
      .kind(sim.phase(ecs::TickPhase::Lod))
      .multi_threaded()
      .run([](flecs::iter& it) {
        // The observer set is read once per invocation, not once per entity: a singleton lookup
        // per entity would dominate a system this cheap.
        const ObserverSet& observers = it.world().get<ObserverSet>();
        while (it.next()) {
          auto tiers = it.field<Tier>(0);
          auto positions = it.field<const Position>(1);
          auto importance = it.field<const Importance>(2);
          for (auto row : it) {
            Tier& tier = tiers[row];
            const u8 wanted = tier_from_score(
                tier_score(observers, positions[row].x, positions[row].y, importance[row].value));
            if (wanted == tier.value) {
              tier.candidate = wanted;
              tier.dwell = 0;
            } else if (wanted != tier.candidate) {
              tier.candidate = wanted;
              tier.dwell = 1;
            } else if (++tier.dwell >= 2) {
              tier.value = wanted;
              tier.dwell = 0;
            }
          }
        }
      });
}

// Scenes are expensive to build and are reused across repeats, so one is kept per configuration
// for the life of the process. Destruction order inside Scene (world, then adapter, then job
// system) is what lets a flecs world with task workers be torn down safely.
Scene& scene_for(u32 workers, u32 grid_cells) {
  static std::vector<std::unique_ptr<Scene>> cache;
  static std::vector<u64> keys;
  const u64 key = (static_cast<u64>(workers) << 32) | grid_cells;
  for (usize i = 0; i < keys.size(); ++i) {
    if (keys[i] == key) return *cache[i];
  }
  report_environment_once();
  cache.push_back(std::make_unique<Scene>(workers, grid_cells));
  keys.push_back(key);
  cache.back()->report(workers);
  return *cache.back();
}

u32 clamp_workers(u32 requested) {
  const u32 cpus = platform::topology().performance_cpus.count();
  return requested <= cpus ? requested : cpus;
}

}  // namespace

// ---- the tick --------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(ecs_tick_workers, "ecs.tick.four_systems", 1, 2, 4, 8, 16) {
  const u32 requested = static_cast<u32>(state.arg());
  const u32 workers = clamp_workers(requested);
  Scene& scene = scene_for(workers, 0);
  while (state.keep_running()) {
    scene.sim.step();
    bench::keep(scene.aggregate.wealth[0]);
  }
  state.set_items(k_entities);
}

// The systems one at a time, so the tick total can be attributed. Each runs on the same scene by
// running the pipeline with only that phase's systems enabled would need a second world; instead
// these measure the same bodies as plain queries, which is the same work without the pipeline.
ENGINE_BENCH(ecs_move_only, "ecs.system.move") {
  Scene& scene = scene_for(1, 0);
  flecs::query<Position, const Velocity> query =
      scene.sim.world().query_builder<Position, const Velocity>().cached().build();
  while (state.keep_running()) {
    query.each([](Position& p, const Velocity& v) {
      p.x += v.x * 0.0001f;
      p.y += v.y * 0.0001f;
    });
  }
  state.set_items(k_entities);
}

ENGINE_BENCH(ecs_aggregate_only, "ecs.system.faction_aggregate") {
  Scene& scene = scene_for(1, 0);
  flecs::query<const Wealth> query = scene.sim.world()
                                         .query_builder<const Wealth>()
                                         .with<FactionOf>(flecs::Wildcard)
                                         .cached()
                                         .build();
  Aggregate totals;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_factions; ++i)
      totals.wealth[i] = 0;
    query.run([&totals](flecs::iter& it) {
      while (it.next()) {
        auto wealth = it.field<const Wealth>(0);
        const u32 index = it.pair(1).second().get<FactionIndex>().value;
        f32 sum = 0;
        for (auto row : it)
          sum += wealth[row].value;
        totals.wealth[index] += sum;
      }
    });
    bench::keep(totals.wealth[0]);
  }
  state.set_items(k_entities);
}

ENGINE_BENCH(ecs_lod_only, "ecs.system.lod_assign") {
  Scene& scene = scene_for(1, 0);
  flecs::query<Tier, const Position, const Importance> query =
      scene.sim.world().query_builder<Tier, const Position, const Importance>().cached().build();
  const ObserverSet& observers = scene.sim.world().get<ObserverSet>();
  while (state.keep_running()) {
    query.each([&observers](Tier& tier, const Position& p, const Importance& imp) {
      tier.candidate = tier_from_score(tier_score(observers, p.x, p.y, imp.value));
    });
    bench::keep(observers.observers[0].x);
  }
  state.set_items(k_entities);
}

// ---- queries ---------------------------------------------------------------------------------

namespace {

// "Entities of faction F within radius r of observer o", the query plan 06's `query_world` has to
// answer. Three implementations of the same answer.
struct RadiusQuery {
  flecs::query<const Position> by_faction;
  f32 x = 0;
  f32 y = 0;
  f32 radius = 0;
};

u32 scan_radius(const RadiusQuery& query) {
  u32 hits = 0;
  const f32 r2 = query.radius * query.radius;
  query.by_faction.each([&](const Position& p) {
    const f32 dx = p.x - query.x;
    const f32 dy = p.y - query.y;
    if (dx * dx + dy * dy <= r2) ++hits;
  });
  return hits;
}

}  // namespace

ENGINE_BENCH(ecs_query_scan, "ecs.query.faction_radius.scan") {
  Scene& scene = scene_for(1, 0);
  const ObserverSet& observers = scene.sim.world().get<ObserverSet>();
  RadiusQuery query{scene.sim.world()
                        .query_builder<const Position>()
                        .with<FactionOf>(scene.factions[3])
                        .cached()
                        .build(),
                    observers.observers[0].x, observers.observers[0].y, 256.0f};
  while (state.keep_running())
    bench::keep(scan_radius(query));
  state.set_items(k_entities / k_factions);
}

ENGINE_BENCH(ecs_query_cell_component, "ecs.query.faction_radius.cell_component") {
  // The grid as a component: the distance test becomes an integer range test on the cell index
  // first. Same entities iterated, cheaper rejection.
  Scene& scene = scene_for(1, 0);
  flecs::world& world = scene.sim.world();
  const ObserverSet& observers = world.get<ObserverSet>();
  flecs::query<const Position, const Cell> query = world.query_builder<const Position, const Cell>()
                                                       .with<FactionOf>(scene.factions[3])
                                                       .cached()
                                                       .build();
  const f32 cell_size = k_world_size / static_cast<f32>(k_fine_grid);
  const f32 radius = 256.0f;
  const f32 ox = observers.observers[0].x;
  const f32 oy = observers.observers[0].y;
  const i32 lo_x = static_cast<i32>((ox - radius) / cell_size);
  const i32 hi_x = static_cast<i32>((ox + radius) / cell_size);
  const i32 lo_y = static_cast<i32>((oy - radius) / cell_size);
  const i32 hi_y = static_cast<i32>((oy + radius) / cell_size);
  const f32 r2 = radius * radius;
  while (state.keep_running()) {
    u32 hits = 0;
    query.each([&](const Position& p, const Cell& cell) {
      const i32 cx = static_cast<i32>(cell.index % k_fine_grid);
      const i32 cy = static_cast<i32>(cell.index / k_fine_grid);
      if (cx < lo_x || cx > hi_x || cy < lo_y || cy > hi_y) return;
      const f32 dx = p.x - ox;
      const f32 dy = p.y - oy;
      if (dx * dx + dy * dy <= r2) ++hits;
    });
    bench::keep(hits);
  }
  state.set_items(k_entities / k_factions);
}

ENGINE_BENCH(ecs_query_cell_relationship, "ecs.query.faction_radius.cell_relationship") {
  // The grid as a relationship: the cell is in the archetype, so flecs iterates only the tables
  // of the cells that intersect the circle. That is a real index — at the cost of multiplying
  // the table count by the cell count, which the tick benchmarks pay for.
  Scene& scene = scene_for(1, k_coarse_grid);
  flecs::world& world = scene.sim.world();
  const ObserverSet& observers = world.get<ObserverSet>();
  const f32 cell_size = k_world_size / static_cast<f32>(k_coarse_grid);
  const f32 radius = 256.0f;
  const f32 ox = observers.observers[0].x;
  const f32 oy = observers.observers[0].y;
  const f32 r2 = radius * radius;

  std::vector<flecs::query<const Position>> per_cell;
  const auto clampi = [](i32 v, i32 hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
  const i32 last = static_cast<i32>(k_coarse_grid) - 1;
  for (i32 cy = clampi(static_cast<i32>((oy - radius) / cell_size), last);
       cy <= clampi(static_cast<i32>((oy + radius) / cell_size), last); ++cy) {
    for (i32 cx = clampi(static_cast<i32>((ox - radius) / cell_size), last);
         cx <= clampi(static_cast<i32>((ox + radius) / cell_size), last); ++cx) {
      const usize index = static_cast<usize>(cy) * k_coarse_grid + static_cast<usize>(cx);
      per_cell.push_back(world.query_builder<const Position>()
                             .with<FactionOf>(scene.factions[3])
                             .with<InCell>(scene.cells[index])
                             .cached()
                             .build());
    }
  }

  while (state.keep_running()) {
    u32 hits = 0;
    for (auto& query : per_cell) {
      query.each([&](const Position& p) {
        const f32 dx = p.x - ox;
        const f32 dy = p.y - oy;
        if (dx * dx + dy * dy <= r2) ++hits;
      });
    }
    bench::keep(hits);
  }
  state.set_items(k_entities / k_factions);
}

// ---- churn -----------------------------------------------------------------------------------

namespace {

void churn_once(Scene& scene, Rng& rng, std::vector<flecs::entity>& previous,
                std::vector<flecs::entity>& current) {
  flecs::world& world = scene.sim.world();
  current.clear();
  for (u32 i = 0; i < k_churn; ++i) {
    flecs::entity e = world.entity();
    e.set<Position>(Position{rng.range(0, k_world_size), rng.range(0, k_world_size)})
        .set<Velocity>(Velocity{1.0f, 1.0f})
        .set<Wealth>(Wealth{1.0f})
        .set<Importance>(Importance{1.0f})
        .set<Cell>(Cell{0})
        .set<Tier>(Tier{})
        .set<WorldPos>(WorldPos{})
        .add<FactionOf>(scene.factions[i % k_factions]);
    current.push_back(e);
  }
  for (flecs::entity& e : previous)
    e.destruct();
  previous.swap(current);
}

}  // namespace

ENGINE_BENCH(ecs_churn, "ecs.churn.create_destroy") {
  // 1% of the world created and destroyed every tick: a streaming boundary crossing, or a
  // settlement materializing. Every iteration creates k_churn and destroys the k_churn made last
  // time, so the world stays at its size and the table set does not grow over the run.
  Scene& scene = scene_for(1, 0);
  std::vector<flecs::entity> previous;
  std::vector<flecs::entity> current;
  previous.reserve(k_churn);
  current.reserve(k_churn);
  Rng rng;
  while (state.keep_running()) {
    churn_once(scene, rng, previous, current);
    bench::keep(previous.size());
  }
  state.set_items(k_churn * 2);
}

ENGINE_BENCH(ecs_churn_deferred, "ecs.churn.create_destroy_deferred") {
  // The same churn inside a deferred scope. Each `set` on a live entity is an archetype move, so
  // building an entity out of eight components moves it eight times; deferring collects the
  // commands and applies them once. This is the difference that makes.
  Scene& scene = scene_for(1, 0);
  std::vector<flecs::entity> previous;
  std::vector<flecs::entity> current;
  previous.reserve(k_churn);
  current.reserve(k_churn);
  Rng rng;
  while (state.keep_running()) {
    scene.sim.world().defer_begin();
    churn_once(scene, rng, previous, current);
    scene.sim.world().defer_end();
    bench::keep(previous.size());
  }
  state.set_items(k_churn * 2);
}

// ---- memory ----------------------------------------------------------------------------------

ENGINE_BENCH(ecs_world_memory, "ecs.world.build_and_memory") {
  // Not really a timing benchmark: it builds a world from nothing and reports what the process
  // grew by, which is the number E6 asks for. One iteration is one full build.
  report_environment_once();
  static bool reported = false;
  while (state.keep_running()) {
    const u64 before = working_set_bytes();
    {
      auto scene = std::make_unique<Scene>(1u, 0u);
      scene->sim.step();
      const u64 after = working_set_bytes();
      if (!reported) {
        reported = true;
        const u64 delta = after > before ? after - before : 0;
        std::printf("# e6 ecs: working set +%llu bytes for %u entities (%.1f B/entity)\n",
                    static_cast<unsigned long long>(delta), k_entities,
                    static_cast<f64>(delta) / static_cast<f64>(k_entities));
        std::printf("# e6 ecs: tables=%d components=%d\n",
                    static_cast<int>(scene->sim.world().get_info()->table_count),
                    static_cast<int>(scene->sim.world().get_info()->component_id_count));
        std::fflush(stdout);
      }
      bench::keep(after);
    }
  }
  state.set_items(k_entities);
}
