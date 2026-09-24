#include <core/base/assert.h>
#include <core/containers/vector.h>
#include <foundation/store/event_log.h>

#include <cstring>

namespace engine::store {

namespace {

// The event log's own schema history. Migrations are append-only: a version is never edited
// once a save file can carry it (plan 03 §3.8).
constexpr Migration k_migrations[] = {
    {1,
     // Events: one contiguous run per tile, so a replay is a single B-tree walk.
     "CREATE TABLE IF NOT EXISTS events ("
     "  tile       INTEGER NOT NULL,"
     "  seq        INTEGER NOT NULL,"
     "  sim_tick   INTEGER NOT NULL,"
     "  game_time  INTEGER NOT NULL,"
     "  type       INTEGER NOT NULL,"
     "  depth      INTEGER NOT NULL,"
     "  origin     INTEGER NOT NULL,"
     "  subject_hi INTEGER NOT NULL,"
     "  subject_lo INTEGER NOT NULL,"
     "  cause      INTEGER NOT NULL,"
     "  payload    BLOB,"
     "  PRIMARY KEY (tile, seq)"
     ") WITHOUT ROWID;"
     // Projections: keyed tile-first for the same reason, with a secondary index for the
     // by-entity point lookup.
     "CREATE TABLE IF NOT EXISTS projections ("
     "  tile       INTEGER NOT NULL,"
     "  entity_hi  INTEGER NOT NULL,"
     "  entity_lo  INTEGER NOT NULL,"
     "  kind       INTEGER NOT NULL,"
     "  version    INTEGER NOT NULL,"
     "  blob       BLOB,"
     "  PRIMARY KEY (tile, entity_hi, entity_lo, kind)"
     ") WITHOUT ROWID;"
     "CREATE INDEX IF NOT EXISTS projections_by_entity"
     "  ON projections(entity_hi, entity_lo, kind);"
     // Snapshots: the rowid is the tile, so a lookup is one descent.
     "CREATE TABLE IF NOT EXISTS snapshots ("
     "  tile         INTEGER PRIMARY KEY,"
     "  seq          INTEGER NOT NULL,"
     "  sim_tick     INTEGER NOT NULL,"
     "  game_time    INTEGER NOT NULL,"
     "  record_count INTEGER NOT NULL,"
     "  blob         BLOB NOT NULL"
     ");"},
};

constexpr u32 k_snapshot_magic = 0x31'50'4E'53u;  // "SNP1" little-endian
constexpr u32 k_snapshot_header_bytes = 16;
constexpr u32 k_snapshot_record_header_bytes = 32;

void put_u32(Vector<u8>& out, u32 value) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((value >> (i * 8)) & 0xFFu));
}
void put_u64(Vector<u8>& out, u64 value) {
  for (u32 i = 0; i < 8; ++i)
    out.push_back(static_cast<u8>((value >> (i * 8)) & 0xFFu));
}
u32 read_u32(const u8* p) noexcept {
  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
         (static_cast<u32>(p[3]) << 24);
}
u64 read_u64(const u8* p) noexcept {
  return static_cast<u64>(read_u32(p)) | (static_cast<u64>(read_u32(p + 4)) << 32);
}

i64 as_i64(u64 value) noexcept { return static_cast<i64>(value); }
u64 as_u64(i64 value) noexcept { return static_cast<u64>(value); }

}  // namespace

const char* origin_name(EventOrigin origin) noexcept {
  switch (origin) {
    case EventOrigin::Deterministic: return "deterministic";
    case EventOrigin::Player: return "player";
    case EventOrigin::Agent: return "agent";
    case EventOrigin::Llm: return "llm";
    case EventOrigin::Debug: return "debug";
  }
  return "unknown";
}

i32 EventLog::schema_version() noexcept {
  return k_migrations[(sizeof(k_migrations) / sizeof(k_migrations[0])) - 1].version;
}

Status EventLog::open() {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  next_sequence_.clear();
  return db_->migrate(
      std::span<const Migration>(k_migrations, sizeof(k_migrations) / sizeof(k_migrations[0])));
}

Status EventLog::highest_sequence(TileId tile, u64& out) {
  out = 0;
  Statement stmt;
  const Status status = db_->prepare("SELECT max(seq) FROM events WHERE tile=?1", stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(tile));
  bool row = false;
  const Status step = stmt.step(row);
  if (step != Status::Ok) return step;
  if (row && !stmt.column_is_null(0)) out = as_u64(stmt.column_i64(0));
  return Status::Ok;
}

Status EventLog::next_sequence(TileId tile, u64& out) {
  out = 0;
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  if (const u64* cached = next_sequence_.find_value(tile); cached != nullptr) {
    out = *cached;
    return Status::Ok;
  }
  u64 highest = 0;
  const Status status = highest_sequence(tile, highest);
  if (status != Status::Ok) return status;
  out = highest + 1;
  next_sequence_.insert(tile, out);
  return Status::Ok;
}

Status EventLog::append_locked(EventRecord& record) {
  if (record.sequence == 0) {
    u64 next = 0;
    const Status status = next_sequence(record.tile, next);
    if (status != Status::Ok) return status;
    record.sequence = next;
  }

  Statement stmt;
  const Status status = db_->prepare(
      "INSERT INTO events(tile,seq,sim_tick,game_time,type,depth,origin,"
      "subject_hi,subject_lo,cause,payload) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
      stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(record.tile))
      .bind(2, as_i64(record.sequence))
      .bind(3, as_i64(record.sim_tick))
      .bind(4, record.game_time_us)
      .bind(5, static_cast<i64>(record.type))
      .bind(6, static_cast<i64>(record.depth))
      .bind(7, static_cast<i64>(record.origin))
      .bind(8, as_i64(record.subject.hi))
      .bind(9, as_i64(record.subject.lo))
      .bind(10, as_i64(record.cause));
  if (record.payload.empty()) {
    stmt.bind_null(11);
  } else {
    stmt.bind_ref(11, record.payload);
  }
  const Status run = stmt.run();
  if (run != Status::Ok) return run;

  u64* next = next_sequence_.find_value(record.tile);
  if (next != nullptr && record.sequence >= *next) *next = record.sequence + 1;
  return Status::Ok;
}

Status EventLog::append(EventRecord& record) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  return append_locked(record);
}

Status EventLog::append(std::span<EventRecord> records) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  if (records.empty()) return Status::Ok;
  Transaction transaction;
  Status status = db_->begin(transaction, true);
  if (status != Status::Ok) return status;
  for (EventRecord& record : records) {
    status = append_locked(record);
    if (status != Status::Ok) {
      // ~Transaction rolls the batch back, which puts the per-tile high-water marks back where
      // they were; the cache cannot know that, so it is dropped and rebuilt from the file.
      next_sequence_.clear();
      return status;
    }
  }
  return transaction.commit();
}

Status EventLog::replay(TileId tile, u64 from_sequence, EventVisitor fn, void* user) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  if (fn == nullptr) return Status::InvalidArgument;
  Statement stmt;
  const Status status = db_->prepare(
      "SELECT seq,sim_tick,game_time,type,depth,origin,subject_hi,subject_lo,cause,payload "
      "FROM events WHERE tile=?1 AND seq>=?2 ORDER BY seq",
      stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(tile)).bind(2, as_i64(from_sequence));

  EventRecord record;
  record.tile = tile;
  for (;;) {
    bool row = false;
    const Status step = stmt.step(row);
    if (step != Status::Ok) return step;
    if (!row) return Status::Ok;
    record.sequence = as_u64(stmt.column_i64(0));
    record.sim_tick = as_u64(stmt.column_i64(1));
    record.game_time_us = stmt.column_i64(2);
    record.type = static_cast<u32>(stmt.column_i64(3));
    record.depth = static_cast<u16>(stmt.column_i64(4));
    record.origin = static_cast<EventOrigin>(stmt.column_i64(5));
    record.subject.hi = as_u64(stmt.column_i64(6));
    record.subject.lo = as_u64(stmt.column_i64(7));
    record.cause = as_u64(stmt.column_i64(8));
    record.payload = stmt.column_blob(9);
    fn(record, user);
  }
}

Status EventLog::scan(const LogPosition* after, u64 since_tick, u32 limit, EventVisitor fn,
                      void* user) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  if (fn == nullptr) return Status::InvalidArgument;
  if (limit == 0) return Status::Ok;
  // Two texts rather than one with a sentinel position: a position at the start of the order is a
  // real place, and the smallest signed tick is not one a caller should have to know about. The
  // row-value comparison is SQLite's own (3.15 and later), so "after" means exactly the ORDER BY's
  // "after".
  Statement stmt;
  const Status status =
      after != nullptr ? db_->prepare(
                             "SELECT tile,seq,sim_tick,game_time,type,depth,origin,subject_hi,"
                             "subject_lo,cause,payload FROM events WHERE sim_tick>=?1 AND "
                             "(sim_tick,tile,seq)>(?2,?3,?4) ORDER BY sim_tick,tile,seq LIMIT ?5",
                             stmt)
                       : db_->prepare(
                             "SELECT tile,seq,sim_tick,game_time,type,depth,origin,subject_hi,"
                             "subject_lo,cause,payload FROM events WHERE sim_tick>=?1 "
                             "ORDER BY sim_tick,tile,seq LIMIT ?2",
                             stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(since_tick));
  if (after != nullptr) {
    stmt.bind(2, as_i64(after->sim_tick))
        .bind(3, as_i64(after->tile))
        .bind(4, as_i64(after->sequence))
        .bind(5, static_cast<i64>(limit));
  } else {
    stmt.bind(2, static_cast<i64>(limit));
  }

  EventRecord record;
  for (;;) {
    bool row = false;
    const Status step = stmt.step(row);
    if (step != Status::Ok) return step;
    if (!row) return Status::Ok;
    record.tile = as_u64(stmt.column_i64(0));
    record.sequence = as_u64(stmt.column_i64(1));
    record.sim_tick = as_u64(stmt.column_i64(2));
    record.game_time_us = stmt.column_i64(3);
    record.type = static_cast<u32>(stmt.column_i64(4));
    record.depth = static_cast<u16>(stmt.column_i64(5));
    record.origin = static_cast<EventOrigin>(stmt.column_i64(6));
    record.subject.hi = as_u64(stmt.column_i64(7));
    record.subject.lo = as_u64(stmt.column_i64(8));
    record.cause = as_u64(stmt.column_i64(9));
    record.payload = stmt.column_blob(10);
    fn(record, user);
  }
}

Status EventLog::count_events(u64& out) {
  out = 0;
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  Statement stmt;
  const Status status = db_->prepare("SELECT count(*) FROM events", stmt);
  if (status != Status::Ok) return status;
  bool row = false;
  const Status step = stmt.step(row);
  if (step != Status::Ok) return step;
  if (row) out = as_u64(stmt.column_i64(0));
  return Status::Ok;
}

Status EventLog::insert_projection(const ProjectionRecord& record) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  Statement stmt;
  const Status status = db_->prepare(
      "INSERT INTO projections(tile,entity_hi,entity_lo,kind,version,blob) "
      "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(tile,entity_hi,entity_lo,kind) DO UPDATE SET "
      "version=excluded.version, blob=excluded.blob",
      stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(record.tile))
      .bind(2, as_i64(record.entity.hi))
      .bind(3, as_i64(record.entity.lo))
      .bind(4, static_cast<i64>(record.kind))
      .bind(5, as_i64(record.version));
  if (record.blob.empty()) {
    stmt.bind_null(6);
  } else {
    stmt.bind_ref(6, record.blob);
  }
  return stmt.run();
}

Status EventLog::erase_projection(Id128 entity, u32 kind) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  Statement stmt;
  const Status status =
      db_->prepare("DELETE FROM projections WHERE entity_hi=?1 AND entity_lo=?2 AND kind=?3", stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(entity.hi)).bind(2, as_i64(entity.lo)).bind(3, static_cast<i64>(kind));
  return stmt.run();
}

Status EventLog::upsert_projection(const ProjectionRecord& record) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  // The tile is part of the key, so a row filed under a different tile has to go first. One
  // seek of projections_by_entity; nothing is written when the entity has not moved.
  Statement stale;
  Status status = db_->prepare(
      "DELETE FROM projections WHERE entity_hi=?1 AND entity_lo=?2 AND kind=?3 AND tile<>?4",
      stale);
  if (status != Status::Ok) return status;
  stale.bind(1, as_i64(record.entity.hi))
      .bind(2, as_i64(record.entity.lo))
      .bind(3, static_cast<i64>(record.kind))
      .bind(4, as_i64(record.tile));
  status = stale.run();
  if (status != Status::Ok) return status;
  stale.reset();
  return insert_projection(record);
}

Status EventLog::upsert_projections(std::span<const ProjectionRecord> records) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  if (records.empty()) return Status::Ok;
  Transaction transaction;
  Status status = db_->begin(transaction, true);
  if (status != Status::Ok) return status;
  for (const ProjectionRecord& record : records) {
    status = upsert_projection(record);
    if (status != Status::Ok) return status;
  }
  return transaction.commit();
}

Status EventLog::load_projection(Id128 entity, u32 kind, ProjectionRecord& out) {
  out = ProjectionRecord{};
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  Statement stmt;
  const Status status = db_->prepare(
      "SELECT tile,version,blob FROM projections WHERE entity_hi=?1 AND entity_lo=?2 AND kind=?3",
      stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(entity.hi)).bind(2, as_i64(entity.lo)).bind(3, static_cast<i64>(kind));
  bool row = false;
  const Status step = stmt.step(row);
  if (step != Status::Ok) return step;
  if (!row) return Status::NotFound;
  out.entity = entity;
  out.kind = kind;
  out.tile = as_u64(stmt.column_i64(0));
  out.version = as_u64(stmt.column_i64(1));
  // The statement goes back to the cache — reset, bindings cleared — the moment `stmt` dies at
  // the end of this function, which invalidates SQLite's own pointer. The bytes are copied into
  // a buffer this log owns so the caller gets a span that outlives the statement.
  const std::span<const u8> blob = stmt.column_blob(2);
  loaded_blob_.assign(blob.begin(), blob.end());
  out.blob = std::span<const u8>(loaded_blob_.data(), loaded_blob_.size());
  return Status::Ok;
}

Status EventLog::projections_by_tile(TileId tile, ProjectionVisitor fn, void* user) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  if (fn == nullptr) return Status::InvalidArgument;
  Statement stmt;
  const Status status = db_->prepare(
      "SELECT entity_hi,entity_lo,kind,version,blob FROM projections WHERE tile=?1 "
      "ORDER BY entity_hi,entity_lo,kind",
      stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(tile));

  ProjectionRecord record;
  record.tile = tile;
  for (;;) {
    bool row = false;
    const Status step = stmt.step(row);
    if (step != Status::Ok) return step;
    if (!row) return Status::Ok;
    record.entity.hi = as_u64(stmt.column_i64(0));
    record.entity.lo = as_u64(stmt.column_i64(1));
    record.kind = static_cast<u32>(stmt.column_i64(2));
    record.version = as_u64(stmt.column_i64(3));
    record.blob = stmt.column_blob(4);
    fn(record, user);
  }
}

Status EventLog::count_projections(TileId tile, u64& out) {
  out = 0;
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  Statement stmt;
  const Status status = db_->prepare("SELECT count(*) FROM projections WHERE tile=?1", stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(tile));
  bool row = false;
  const Status step = stmt.step(row);
  if (step != Status::Ok) return step;
  if (row) out = as_u64(stmt.column_i64(0));
  return Status::Ok;
}

namespace {

struct SnapshotBuilder {
  Vector<u8>* bytes;
  u32 count;
};

void pack_projection(const ProjectionRecord& record, void* user) {
  auto* builder = static_cast<SnapshotBuilder*>(user);
  Vector<u8>& out = *builder->bytes;
  put_u64(out, record.entity.hi);
  put_u64(out, record.entity.lo);
  put_u64(out, record.version);
  put_u32(out, record.kind);
  put_u32(out, static_cast<u32>(record.blob.size()));
  out.append(std::span<const u8>(record.blob.data(), record.blob.size()));
  ++builder->count;
}

}  // namespace

Status EventLog::snapshot(TileId tile, u64 sim_tick, i64 game_time_us, SnapshotInfo& info) {
  info = SnapshotInfo{};
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;

  u64 next = 0;
  Status status = next_sequence(tile, next);
  if (status != Status::Ok) return status;

  Vector<u8> bytes;
  bytes.reserve(64 * 1024);
  put_u32(bytes, k_snapshot_magic);
  put_u32(bytes, 1);  // snapshot blob version
  put_u32(bytes, 0);  // record count, patched below
  put_u32(bytes, 0);  // reserved, keeps the record bodies 8-byte aligned in the blob

  SnapshotBuilder builder{&bytes, 0};
  status = projections_by_tile(tile, &pack_projection, &builder);
  if (status != Status::Ok) return status;

  for (u32 i = 0; i < 4; ++i)
    bytes[8 + i] = static_cast<u8>((builder.count >> (i * 8)) & 0xFFu);

  Statement stmt;
  status = db_->prepare(
      "INSERT INTO snapshots(tile,seq,sim_tick,game_time,record_count,blob) "
      "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(tile) DO UPDATE SET seq=excluded.seq, "
      "sim_tick=excluded.sim_tick, game_time=excluded.game_time, "
      "record_count=excluded.record_count, blob=excluded.blob",
      stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(tile))
      .bind(2, as_i64(next - 1))
      .bind(3, as_i64(sim_tick))
      .bind(4, game_time_us)
      .bind(5, static_cast<i64>(builder.count))
      .bind_ref(6, std::span<const u8>(bytes.data(), bytes.size()));
  status = stmt.run();
  if (status != Status::Ok) return status;

  info.tile = tile;
  info.sequence = next - 1;
  info.sim_tick = sim_tick;
  info.game_time_us = game_time_us;
  info.record_count = builder.count;
  info.blob_bytes = static_cast<u32>(bytes.size());
  return Status::Ok;
}

Status EventLog::load_snapshot(TileId tile, SnapshotInfo& info, ProjectionVisitor fn, void* user) {
  info = SnapshotInfo{};
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  Statement stmt;
  const Status status = db_->prepare(
      "SELECT seq,sim_tick,game_time,record_count,blob FROM snapshots WHERE tile=?1", stmt);
  if (status != Status::Ok) return status;
  stmt.bind(1, as_i64(tile));
  bool row = false;
  const Status step = stmt.step(row);
  if (step != Status::Ok) return step;
  if (!row) return Status::NotFound;

  info.tile = tile;
  info.sequence = as_u64(stmt.column_i64(0));
  info.sim_tick = as_u64(stmt.column_i64(1));
  info.game_time_us = stmt.column_i64(2);
  info.record_count = static_cast<u32>(stmt.column_i64(3));
  const std::span<const u8> blob = stmt.column_blob(4);
  info.blob_bytes = static_cast<u32>(blob.size());

  if (blob.size() < k_snapshot_header_bytes) return Status::Corrupt;
  if (read_u32(blob.data()) != k_snapshot_magic) return Status::Corrupt;
  if (read_u32(blob.data() + 4) != 1) return Status::SchemaTooNew;
  const u32 count = read_u32(blob.data() + 8);
  if (count != info.record_count) return Status::Corrupt;
  if (fn == nullptr) return Status::Ok;

  ProjectionRecord record;
  record.tile = tile;
  usize offset = k_snapshot_header_bytes;
  for (u32 i = 0; i < count; ++i) {
    if (offset + k_snapshot_record_header_bytes > blob.size()) return Status::Corrupt;
    const u8* p = blob.data() + offset;
    record.entity.hi = read_u64(p);
    record.entity.lo = read_u64(p + 8);
    record.version = read_u64(p + 16);
    record.kind = read_u32(p + 24);
    const u32 size = read_u32(p + 28);
    offset += k_snapshot_record_header_bytes;
    if (offset + size > blob.size()) return Status::Corrupt;
    record.blob = std::span<const u8>(blob.data() + offset, size);
    offset += size;
    fn(record, user);
  }
  return Status::Ok;
}

namespace {

struct SnapshotRestore {
  EventLog* log;
  Status status;
};

void restore_projection(const ProjectionRecord& record, void* user) {
  auto* restore = static_cast<SnapshotRestore*>(user);
  if (restore->status != Status::Ok) return;
  restore->status = restore->log->insert_projection(record);
}

}  // namespace

Status EventLog::restore_snapshot(TileId tile, SnapshotInfo& info) {
  if (db_ == nullptr || !db_->is_open()) return Status::NotOpen;
  // The snapshot blob is read through one statement while the restore writes through another;
  // both are on the same connection, which SQLite allows for a read cursor and a writer that do
  // not touch the same table.
  Transaction transaction;
  Status status = db_->begin(transaction, true);
  if (status != Status::Ok) return status;
  SnapshotRestore restore{this, Status::Ok};
  status = load_snapshot(tile, info, &restore_projection, &restore);
  if (status != Status::Ok) return status;
  if (restore.status != Status::Ok) return restore.status;
  return transaction.commit();
}

}  // namespace engine::store
