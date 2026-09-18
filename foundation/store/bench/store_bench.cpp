// Experiment E6, the store half (docs/plan/10-roadmap-risks.md §10.5, docs/experiments/
// e6-ecs-store.md): 10^6 LOD3 projection records in SQLite — insert throughput against batch
// size, point lookup, range by tile, hourly summarization, snapshot write and read, file size,
// WAL against a rollback journal, and the event log's append rate.
//
// Debug builds only smoke-run benchmarks (docs/subsystems/bench.md), so the debug corpus is small
// enough for CTest; every number that reaches the write-up comes from msvc-release at 10^6.

#include <core/base/macros.h>
#include <core/ids/id128.h>
#include <core/log/log.h>
#include <core/platform/topology.h>
#include <core/time/time.h>
#include <foundation/bench/bench.h>
#include <foundation/store/event_log.h>

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::store;

namespace {

#if ENGINE_DEBUG
constexpr u32 k_records = 20'000;
constexpr u32 k_tiles = 64;
#else
constexpr u32 k_records = 1'000'000;
constexpr u32 k_tiles = 1024;  // plan 03 §3.7's grid: one save file, a thousand tiles
#endif
constexpr u32 k_blob_min = 64;
constexpr u32 k_blob_max = 256;
constexpr u32 k_summary_records = k_records / 100;  // the hourly pass touches 1% of the world
constexpr u32 k_snapshot_records = k_records / 100;
constexpr u32 k_snapshot_tile = 7;

struct Rng {
  u64 state = 0x243F6A8885A308D3ull;
  u32 next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>(state >> 33);
  }
};

Id128 entity_for(u32 index) { return Id128::from_seed(0xE6E6, index); }

std::string temp_root() {
  auto path = std::filesystem::temp_directory_path() / "engine_store_bench";
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  std::string text = path.string();
  for (char& c : text) {
    if (c == '\\') c = '/';
  }
  return text;
}

const char* build_preset() {
#if ENGINE_DEBUG
  return "debug";
#else
  return "release";
#endif
}

// The store logs every open and every migration at info; that is right for an engine run and
// noise in a benchmark table, so the bench turns the category down before anything runs.
struct QuietStore {
  QuietStore() { log::apply_level_spec("store=warn"); }
};
const QuietStore g_quiet_store;

void report_environment_once() {
  static bool done = false;
  if (done) return;
  done = true;
  char description[512];
  platform::describe_topology(platform::topology(), description, sizeof(description));
  std::printf("# e6 store: %s\n", description);
  std::printf("# e6 store: build=%s sqlite=%s records=%u tiles=%u blob=%u..%u B\n", build_preset(),
              sqlite3_libversion(), k_records, k_tiles, k_blob_min, k_blob_max);
  std::fflush(stdout);
}

// The page cache a game would give its save file. The default is 2 MiB, which is smaller than the
// working set of a million-row table, so it is the difference between a read that stays in memory
// and a read that goes to the file every time; both are measured.
constexpr u32 k_warm_cache_kib = 65'536;

// Filled once and reused: building the million-row corpus is the expensive part of the run.
// `warm` is a second connection to the same file with a large page cache — which is what WAL
// makes safe — so the cold and the warm read costs come from one corpus.
struct Corpus {
  Corpus();
  ~Corpus();

  std::string path;
  Database db;
  Database warm;
  std::unique_ptr<EventLog> log;
  std::unique_ptr<EventLog> warm_log;
  std::vector<u8> blob;
};

Corpus::Corpus() {
  report_environment_once();
  path = temp_root() + "/corpus.db";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + "-wal", ec);
  std::filesystem::remove(path + "-shm", ec);

  blob.assign(k_blob_max, 0xA5);
  // The build gets the large page cache. It is not a nicety: the by-entity index is keyed on
  // random 128-bit ids, so a million inserts touch a million random index pages, and with
  // SQLite's default 2 MiB cache the build runs about three times slower — measured, see the
  // write-up. The read benchmarks below reopen this file with the default cache so the cold
  // numbers stay cold.
  OpenOptions build_options;
  build_options.cache_kib = k_warm_cache_kib;
  if (db.open(path, build_options) != Status::Ok) return;
  log = std::make_unique<EventLog>(db);
  if (log->open() != Status::Ok) return;

  const time::Stopwatch watch;
  Rng rng;
  // 10,000 rows per transaction: enough that the commit is amortized, small enough that the
  // rollback journal and the WAL both stay bounded. Rows are written tile by tile, which is both
  // what a world generator produces and what the tile-leading primary key wants: see
  // store.projections.insert_order for what the other order costs.
  constexpr u32 k_batch = 10'000;
  constexpr u32 k_per_tile = k_records / k_tiles;
  u32 written = 0;
  while (written < k_records) {
    Transaction transaction;
    if (db.begin(transaction, true) != Status::Ok) return;
    const u32 end = written + k_batch < k_records ? written + k_batch : k_records;
    for (; written < end; ++written) {
      ProjectionRecord record;
      record.entity = entity_for(written);
      record.tile = written / k_per_tile;
      record.kind = written % 4;
      record.version = 1;
      const u32 size = k_blob_min + rng.next() % (k_blob_max - k_blob_min + 1);
      record.blob = std::span<const u8>(blob.data(), size);
      if (log->insert_projection(record) != Status::Ok) return;
    }
    if (transaction.commit() != Status::Ok) return;
  }
  const f64 seconds = watch.elapsed_s();

  i64 bytes = 0;
  db.file_size_bytes(bytes);
  std::error_code size_ec;
  const auto wal_bytes = std::filesystem::file_size(path + "-wal", size_ec);
  std::printf(
      "# e6 store: corpus %u rows in %.2f s (%.0f rows/s, %u KiB cache), db %lld B "
      "(%.1f B/row), wal %llu B\n",
      k_records, seconds, static_cast<f64>(k_records) / seconds, k_warm_cache_kib,
      static_cast<long long>(bytes), static_cast<f64>(bytes) / static_cast<f64>(k_records),
      size_ec ? 0ull : static_cast<unsigned long long>(wal_bytes));
  std::fflush(stdout);

  // Reopen with the default page cache: `db` is the cold reader from here on.
  log.reset();
  db.close();
  if (db.open(path) != Status::Ok) return;
  log = std::make_unique<EventLog>(db);
  if (log->open() != Status::Ok) {
    log.reset();
    return;
  }

  OpenOptions warm_options;
  warm_options.cache_kib = k_warm_cache_kib;
  if (warm.open(path, warm_options) != Status::Ok) return;
  warm_log = std::make_unique<EventLog>(warm);
  if (warm_log->open() != Status::Ok) warm_log.reset();
}

Corpus::~Corpus() {
  warm_log.reset();
  warm.close();
  log.reset();
  db.close();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + "-wal", ec);
  std::filesystem::remove(path + "-shm", ec);
}

Corpus& corpus() {
  static Corpus instance;
  return instance;
}

// A throwaway database for the write benchmarks, so the corpus stays as it was measured. A write
// benchmark that ran for a hundred milliseconds a repeat would otherwise grow its own table past
// the page cache mid-run and measure the growth rather than the write; `reset()` puts it back to
// empty, and the benchmarks call it (untimed) once they have written enough.
struct Scratch {
  Scratch(const char* name, bool use_wal) : path(temp_root() + "/" + name), wal(use_wal) {
    reset();
  }
  ~Scratch() { discard(); }

  void discard() {
    log.reset();
    db.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + "-wal", ec);
    std::filesystem::remove(path + "-shm", ec);
  }

  void reset() {
    discard();
    OpenOptions options;
    options.wal = wal;
    ok = db.open(path, options) == Status::Ok;
    log = std::make_unique<EventLog>(db);
    ok = ok && log->open() == Status::Ok;
  }

  std::string path;
  bool wal = true;
  Database db;
  std::unique_ptr<EventLog> log;
  bool ok = false;
};

// Past this many rows a scratch database is reset, so every repeat measures writes into a table
// of roughly the same size.
constexpr u32 k_scratch_limit = 200'000;

void count_projection(const ProjectionRecord& record, void* user) {
  auto* total = static_cast<u64*>(user);
  *total += record.blob.size();
}

void count_event(const EventRecord& record, void* user) {
  auto* total = static_cast<u64*>(user);
  *total += record.payload.size() + record.sequence;
}

}  // namespace

// ---- writes ----------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(store_insert_batch, "store.projections.insert", 1, 100, 1000, 10000) {
  // Rows per transaction. One commit per row is the shape a naive per-event write has; the
  // engine's persistence phase writes one transaction per tick, which is the thousands column.
  const u32 batch = static_cast<u32>(state.arg());
  Scratch scratch("insert.db", true);
  if (!scratch.ok) return;
  std::vector<u8> blob(k_blob_max, 0x5A);
  constexpr u32 k_per_tile = k_scratch_limit / k_tiles;
  u32 next = 0;
  while (state.keep_running()) {
    if (next >= k_scratch_limit) {
      state.pause_timing();
      scratch.reset();
      next = 0;
      state.resume_timing();
    }
    Transaction transaction;
    if (scratch.db.begin(transaction, true) != Status::Ok) return;
    for (u32 i = 0; i < batch; ++i, ++next) {
      ProjectionRecord record;
      record.entity = entity_for(next);
      record.tile = next / (k_per_tile > 0 ? k_per_tile : 1);
      record.kind = next % 4;
      record.version = 1;
      record.blob = std::span<const u8>(blob.data(), k_blob_min);
      if (scratch.log->insert_projection(record) != Status::Ok) return;
    }
    if (transaction.commit() != Status::Ok) return;
  }
  state.set_items(batch);
}

ENGINE_BENCH_ARGS(store_insert_order, "store.projections.insert_order", 0, 1) {
  // 0 writes tile by tile, 1 round-robins the tiles. Same rows, same transaction size; the only
  // difference is whether consecutive inserts land in the same region of a tile-leading B-tree or
  // in a thousand different ones. This is the cost of not clustering a persistence flush.
  const bool round_robin = state.arg() != 0;
  Scratch scratch("insert_order.db", true);
  if (!scratch.ok) return;
  std::vector<u8> blob(k_blob_max, 0x5A);
  constexpr u32 k_batch = 1000;
  constexpr u32 k_per_tile = k_scratch_limit / k_tiles;
  u32 next = 0;
  while (state.keep_running()) {
    if (next >= k_scratch_limit) {
      state.pause_timing();
      scratch.reset();
      next = 0;
      state.resume_timing();
    }
    Transaction transaction;
    if (scratch.db.begin(transaction, true) != Status::Ok) return;
    for (u32 i = 0; i < k_batch; ++i, ++next) {
      ProjectionRecord record;
      record.entity = entity_for(next);
      record.tile = round_robin ? next % k_tiles : next / (k_per_tile > 0 ? k_per_tile : 1);
      record.kind = next % 4;
      record.version = 1;
      record.blob = std::span<const u8>(blob.data(), k_blob_min);
      if (scratch.log->insert_projection(record) != Status::Ok) return;
    }
    if (transaction.commit() != Status::Ok) return;
  }
  state.set_items(k_batch);
}

ENGINE_BENCH(store_upsert_cost, "store.projections.upsert_vs_insert") {
  // upsert_projection pays one extra seek of the by-entity index so that an entity which has
  // moved between tiles does not end up filed twice. This is what that costs.
  Scratch scratch("upsert.db", true);
  if (!scratch.ok) return;
  std::vector<u8> blob(k_blob_max, 0x5A);
  constexpr u32 k_per_tile = k_scratch_limit / k_tiles;
  u32 next = 0;
  while (state.keep_running()) {
    if (next >= k_scratch_limit) {
      state.pause_timing();
      scratch.reset();
      next = 0;
      state.resume_timing();
    }
    Transaction transaction;
    if (scratch.db.begin(transaction, true) != Status::Ok) return;
    for (u32 i = 0; i < 1000; ++i, ++next) {
      ProjectionRecord record;
      record.entity = entity_for(next);
      record.tile = next / (k_per_tile > 0 ? k_per_tile : 1);
      record.kind = next % 4;
      record.version = 1;
      record.blob = std::span<const u8>(blob.data(), k_blob_min);
      if (scratch.log->upsert_projection(record) != Status::Ok) return;
    }
    if (transaction.commit() != Status::Ok) return;
  }
  state.set_items(1000);
}

ENGINE_BENCH_ARGS(store_journal_mode, "store.journal", 0, 1) {
  // 0 is a rollback journal, 1 is WAL. Same thousand rows, same transaction, the only difference
  // is how the commit reaches the disk.
  const bool wal = state.arg() != 0;
  Scratch scratch(wal ? "journal_wal.db" : "journal_delete.db", wal);
  if (!scratch.ok) return;
  std::vector<u8> blob(k_blob_max, 0x5A);
  constexpr u32 k_per_tile = k_scratch_limit / k_tiles;
  u32 next = 0;
  while (state.keep_running()) {
    if (next >= k_scratch_limit) {
      state.pause_timing();
      scratch.reset();
      next = 0;
      state.resume_timing();
    }
    Transaction transaction;
    if (scratch.db.begin(transaction, true) != Status::Ok) return;
    for (u32 i = 0; i < 1000; ++i, ++next) {
      ProjectionRecord record;
      record.entity = entity_for(next);
      record.tile = next / (k_per_tile > 0 ? k_per_tile : 1);
      record.kind = 0;
      record.version = 1;
      record.blob = std::span<const u8>(blob.data(), k_blob_min);
      if (scratch.log->insert_projection(record) != Status::Ok) return;
    }
    if (transaction.commit() != Status::Ok) return;
  }
  state.set_items(1000);
}

ENGINE_BENCH_ARGS(store_event_append, "store.events.append", 1, 64, 1024) {
  // Events per transaction. Plan 03 §3.5 expects "hundreds to low thousands of events per
  // second, batched per tick"; the 64 column is roughly one tick's worth.
  const u32 batch = static_cast<u32>(state.arg());
  Scratch scratch("events.db", true);
  if (!scratch.ok) return;
  const u8 payload[64] = {};
  std::vector<EventRecord> records(batch);
  u32 tick = 0;
  u32 written = 0;
  while (state.keep_running()) {
    if (written >= k_scratch_limit) {
      state.pause_timing();
      scratch.reset();
      written = 0;
      tick = 0;
      state.resume_timing();
    }
    ++tick;
    written += batch;
    for (u32 i = 0; i < batch; ++i) {
      records[i] = EventRecord{};
      records[i].tile = i % 16;
      records[i].sim_tick = tick;
      records[i].game_time_us = static_cast<i64>(tick) * 16'666;
      records[i].type = i % 8;
      records[i].subject = entity_for(i);
      records[i].payload = std::span<const u8>(payload, sizeof(payload));
    }
    if (scratch.log->append(std::span<EventRecord>(records.data(), records.size())) != Status::Ok) {
      return;
    }
  }
  state.set_items(batch);
}

// ---- reads -----------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(store_point_lookup, "store.projections.point_lookup", 0, 1) {
  // A random entity out of a million, through the by-entity index. 0 is SQLite's default 2 MiB
  // page cache, 1 is the 64 MiB one a game would configure.
  Corpus& c = corpus();
  EventLog* log = state.arg() != 0 ? c.warm_log.get() : c.log.get();
  if (log == nullptr) return;
  Rng rng;
  u64 bytes = 0;
  while (state.keep_running()) {
    ProjectionRecord out;
    const u32 index = rng.next() % k_records;
    if (log->load_projection(entity_for(index), index % 4, out) == Status::Ok) {
      bytes += out.blob.size();
    }
    bench::keep(bytes);
  }
  state.set_items(1);
}

ENGINE_BENCH_ARGS(store_range_by_tile, "store.projections.range_by_tile", 0, 1) {
  // A tile activating (plan 05 §5.5): read every projection the tile owns. The table is keyed
  // tile-first, so this is one contiguous B-tree walk. 0 is the default page cache, 1 the large
  // one.
  Corpus& c = corpus();
  EventLog* log = state.arg() != 0 ? c.warm_log.get() : c.log.get();
  if (log == nullptr) return;
  Rng rng;
  while (state.keep_running()) {
    u64 bytes = 0;
    log->projections_by_tile(rng.next() % k_tiles, &count_projection, &bytes);
    bench::keep(bytes);
  }
  state.set_items(k_records / k_tiles);
}

ENGINE_BENCH(store_summarize_hour, "store.projections.summarize_hour") {
  // The hourly periodic of plan 05 §5.3 reading its slice: an aggregate over 1% of the world,
  // expressed as SQL so the store does the walking rather than a million rows crossing the API.
  Corpus& c = corpus();
  if (c.log == nullptr) return;
  const u32 tiles_per_pass = k_tiles / 100 > 0 ? k_tiles / 100 : 1;
  Rng rng;
  while (state.keep_running()) {
    Statement stmt;
    if (c.db.prepare("SELECT kind, count(*), sum(version), sum(length(blob)) FROM projections "
                     "WHERE tile BETWEEN ?1 AND ?2 GROUP BY kind",
                     stmt) != Status::Ok) {
      return;
    }
    const i64 first = rng.next() % (k_tiles - tiles_per_pass);
    stmt.bind(1, first).bind(2, first + tiles_per_pass - 1);
    i64 total = 0;
    for (;;) {
      bool row = false;
      if (stmt.step(row) != Status::Ok || !row) break;
      total += stmt.column_i64(1) + stmt.column_i64(3);
    }
    bench::keep(total);
  }
  state.set_items(k_summary_records);
}

ENGINE_BENCH(store_summarize_writeback, "store.projections.summarize_writeback") {
  // The other half of a periodic: it writes its result back. One UPDATE over the same slice.
  Corpus& c = corpus();
  if (c.log == nullptr) return;
  const u32 tiles_per_pass = k_tiles / 100 > 0 ? k_tiles / 100 : 1;
  Rng rng;
  while (state.keep_running()) {
    Transaction transaction;
    if (c.db.begin(transaction, true) != Status::Ok) return;
    Statement stmt;
    if (c.db.prepare("UPDATE projections SET version = version + 1 WHERE tile BETWEEN ?1 AND ?2",
                     stmt) != Status::Ok) {
      return;
    }
    const i64 first = rng.next() % (k_tiles - tiles_per_pass);
    stmt.bind(1, first).bind(2, first + tiles_per_pass - 1);
    if (stmt.run() != Status::Ok) return;
    if (transaction.commit() != Status::Ok) return;
  }
  state.set_items(k_summary_records);
}

ENGINE_BENCH(store_replay_tile, "store.events.replay_tile") {
  Scratch scratch("replay.db", true);
  if (!scratch.ok) return;
  const u8 payload[64] = {};
  std::vector<EventRecord> records(10'000);
  for (u32 i = 0; i < records.size(); ++i) {
    records[i].tile = 1;
    records[i].sim_tick = i;
    records[i].type = i % 8;
    records[i].subject = entity_for(i);
    records[i].payload = std::span<const u8>(payload, sizeof(payload));
  }
  if (scratch.log->append(std::span<EventRecord>(records.data(), records.size())) != Status::Ok) {
    return;
  }
  while (state.keep_running()) {
    u64 total = 0;
    scratch.log->replay(1, 1, &count_event, &total);
    bench::keep(total);
  }
  state.set_items(records.size());
}

// ---- snapshots -------------------------------------------------------------------------------

namespace {

struct SnapshotFixture {
  SnapshotFixture() : scratch("snapshot.db", true) {
    if (!scratch.ok) return;
    std::vector<u8> blob(k_blob_max, 0x3C);
    Transaction transaction;
    if (scratch.db.begin(transaction, true) != Status::Ok) return;
    Rng rng;
    for (u32 i = 0; i < k_snapshot_records; ++i) {
      ProjectionRecord record;
      record.entity = entity_for(i);
      record.tile = k_snapshot_tile;
      record.kind = i % 4;
      record.version = 1;
      const u32 size = k_blob_min + rng.next() % (k_blob_max - k_blob_min + 1);
      record.blob = std::span<const u8>(blob.data(), size);
      if (scratch.log->insert_projection(record) != Status::Ok) return;
    }
    ok = transaction.commit() == Status::Ok;
  }
  Scratch scratch;
  bool ok = false;
};

SnapshotFixture& snapshot_fixture() {
  static SnapshotFixture instance;
  return instance;
}

}  // namespace

ENGINE_BENCH(store_snapshot_write, "store.snapshot.write") {
  SnapshotFixture& fixture = snapshot_fixture();
  if (!fixture.ok) return;
  static bool reported = false;
  while (state.keep_running()) {
    SnapshotInfo info;
    if (fixture.scratch.log->snapshot(k_snapshot_tile, 1, 0, info) != Status::Ok) return;
    if (!reported) {
      reported = true;
      std::printf("# e6 store: snapshot of %u records is %u B (%.1f B/record)\n", info.record_count,
                  info.blob_bytes,
                  static_cast<f64>(info.blob_bytes) / static_cast<f64>(info.record_count));
      std::fflush(stdout);
    }
    bench::keep(info.blob_bytes);
  }
  state.set_items(k_snapshot_records);
}

ENGINE_BENCH(store_snapshot_read, "store.snapshot.read") {
  SnapshotFixture& fixture = snapshot_fixture();
  if (!fixture.ok) return;
  SnapshotInfo written;
  if (fixture.scratch.log->snapshot(k_snapshot_tile, 1, 0, written) != Status::Ok) return;
  while (state.keep_running()) {
    SnapshotInfo info;
    u64 bytes = 0;
    if (fixture.scratch.log->load_snapshot(k_snapshot_tile, info, &count_projection, &bytes) !=
        Status::Ok) {
      return;
    }
    bench::keep(bytes);
  }
  state.set_items(k_snapshot_records);
}
