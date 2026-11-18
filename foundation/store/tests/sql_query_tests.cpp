// SQL a client wrote, run against the store's file (docs/subsystems/store.md, "SQL a client
// wrote"). Every case here is something the plainest implementation — open, prepare, step, return —
// would let through; each says what it would have done.

#include <core/containers/vector.h>
#include <foundation/store/database.h>
#include <foundation/store/event_log.h>
#include <foundation/store/sql_query.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::store;

using TempDir = test::TempDir;

namespace {

std::span<const u8> bytes_of(std::string_view text) {
  return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

// A store with `events` events over four tiles, a projection per event and one snapshot.
std::string make_store(TempDir& dir, u32 events) {
  const std::string path = dir.file("world.db");
  Database db;
  REQUIRE(db.open(path) == Status::Ok);
  EventLog log(db);
  REQUIRE(log.open() == Status::Ok);
  Transaction tx;
  REQUIRE(db.begin(tx, true) == Status::Ok);
  std::string payload = "payload";
  for (u32 i = 0; i < events; ++i) {
    EventRecord record;
    record.tile = i % 4;
    record.type = 10 + i % 3;
    record.sim_tick = i;
    record.subject = Id128::from_seed(1, i);
    record.payload = bytes_of(payload);
    REQUIRE(log.append(record) == Status::Ok);
    u8 blob[4] = {0x00, static_cast<u8>(i & 0xFF), 0xFF, 0x00};
    ProjectionRecord p;
    p.entity = Id128::from_seed(2, i);
    p.tile = i % 4;
    p.kind = 7;
    p.blob = std::span<const u8>(blob, 4);
    REQUIRE(log.upsert_projection(p) == Status::Ok);
  }
  REQUIRE(tx.commit() == Status::Ok);
  SnapshotInfo info;
  REQUIRE(log.snapshot(0, 1, 1, info) == Status::Ok);
  return path;
}

struct Run {
  Status status = Status::Ok;
  SqlQueryResult result;
  std::string message;
};

Run query(const std::string& path, std::string_view sql, std::span<const SqlValue> params = {},
          SqlQueryLimits limits = {}) {
  Run run;
  run.status = run_sql_query(path, sql, params, limits, run.result, run.message);
  return run;
}

i64 scalar(const std::string& path, std::string_view sql) {
  Run run = query(path, sql);
  REQUIRE_MESSAGE(run.status == Status::Ok, run.message);
  REQUIRE(run.result.rows == 1);
  REQUIRE(run.result.cells[0].type == SqlType::Integer);
  return run.result.cells[0].integer;
}

// What the file holds, read through the engine's own connection, so a refusal is checked to
// have left it as it was.
std::string fingerprint(const std::string& path) {
  Database db;
  OpenOptions options;
  options.read_only = true;
  options.create = false;
  options.wal = false;
  REQUIRE(db.open(path, options) == Status::Ok);
  Statement s;
  REQUIRE(db.prepare("SELECT (SELECT count(*) FROM events) || '/' || "
                     "(SELECT count(*) FROM projections) || '/' || "
                     "(SELECT count(*) FROM snapshots) || '/' || "
                     "(SELECT group_concat(name) FROM (SELECT name FROM sqlite_schema ORDER BY "
                     "name))",
                     s) == Status::Ok);
  bool row = false;
  REQUIRE(s.step(row) == Status::Ok);
  REQUIRE(row);
  return std::string(s.column_text(0));
}

bool io_exists_for_test(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

// A statement SQLite cannot compile here at all (a function or virtual table this build lacks or
// the prepare's NO_VTAB refuses): not an authorizer's refusal, but no row and no change.
void require_fails(const std::string& path, std::string_view sql) {
  CAPTURE(sql);
  const std::string before = fingerprint(path);
  Run run = query(path, sql);
  CAPTURE(run.message);
  CHECK(run.status != Status::Ok);
  CHECK(run.result.rows == 0);
  CHECK(fingerprint(path) == before);
}

void require_refused(const std::string& path, std::string_view sql, std::string_view why) {
  CAPTURE(sql);
  const std::string before = fingerprint(path);
  Run run = query(path, sql);
  CAPTURE(run.message);
  CHECK(run.status == Status::InvalidArgument);
  CHECK(run.result.rows == 0);
  CHECK(run.message.find(why) != std::string::npos);
  CHECK(fingerprint(path) == before);
}

}  // namespace

TEST_CASE("sql query: a SELECT over the three tables, with bound values") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 40);

  CHECK(scalar(path, "SELECT count(*) FROM events") == 40);
  CHECK(scalar(path, "SELECT count(*) FROM projections") == 40);
  CHECK(scalar(path, "SELECT count(*) FROM snapshots") == 1);

  const SqlValue params[] = {SqlValue::of(i64{2}), SqlValue::of(i64{11})};
  Run run = query(path,
                  "SELECT tile, seq, type FROM events WHERE tile = ?1 AND type = ?2 "
                  "ORDER BY seq",
                  params);
  REQUIRE_MESSAGE(run.status == Status::Ok, run.message);
  REQUIRE(run.result.columns.size() == 3);
  CHECK(run.result.columns[0] == "tile");
  CHECK(run.result.columns[2] == "type");
  CHECK(run.result.complete);
  CHECK(std::string_view(run.result.bound).empty());
  // tile 2 is i = 2, 6, 10, ...; type 11 is i % 3 == 1: i = 10, 22, 34.
  CHECK(run.result.rows == 3);
  for (u32 r = 0; r < run.result.rows; ++r) {
    CHECK(run.result.cells[r * 3].integer == 2);
    CHECK(run.result.cells[r * 3 + 2].integer == 11);
  }
  CHECK(run.result.bytes == 3 * 3 * 8);

  // A join, an aggregate, a window function, a CTE and JSON all stay inside the statement.
  Run joined = query(path,
                     "WITH t AS (SELECT tile, count(*) AS n FROM events GROUP BY tile) "
                     "SELECT t.tile, t.n, row_number() OVER (ORDER BY t.tile) AS r, "
                     "json_object('n', t.n) AS j, (SELECT count(*) FROM projections p "
                     "WHERE p.tile = t.tile) AS m FROM t ORDER BY t.tile");
  REQUIRE_MESSAGE(joined.status == Status::Ok, joined.message);
  CHECK(joined.result.rows == 4);
  CHECK(joined.result.cells[3].bytes == "{\"n\":10}");

  // A missing file is NotFound, and nothing is created in its place.
  Run missing = query(dir.file("absent.db"), "SELECT 1");
  CHECK(missing.status == Status::NotFound);
  CHECK(fingerprint(path).starts_with("40/40/1/"));
}

TEST_CASE("sql query: one statement, or none at all") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  require_refused(path, "SELECT 1; SELECT 2", "more than one statement");
  // The plainest implementation runs the first and ignores the rest, or runs both.
  require_refused(path, "SELECT 1; DROP TABLE events", "more than one statement");
  require_refused(path, "SELECT 1;DELETE FROM events;", "more than one statement");
  // A flag pragma takes effect as it is compiled, so the tail is prepared with everything denied.
  require_refused(path, "SELECT 1; PRAGMA writable_schema = 1", "more than one statement");
  require_refused(path, "SELECT 1; VACUUM", "more than one statement");
  require_refused(path, "", "no statement");
  require_refused(path, "  -- only a comment\n", "no statement");
  // A trailing semicolon, whitespace or comment is not a second statement.
  CHECK(scalar(path, "SELECT 7;  -- done\n") == 7);
  // A parameter count the statement does not take.
  const SqlValue one[] = {SqlValue::of(i64{1})};
  Run extra = query(path, "SELECT 1", one);
  CHECK(extra.status == Status::InvalidArgument);
  Run missing = query(path, "SELECT ?1 + ?2", one);
  CHECK(missing.status == Status::InvalidArgument);
  // The text's length.
  std::string long_text = "SELECT 1 -- ";
  long_text.append(k_sql_max_text_bytes, 'x');
  require_refused(path, long_text, "bytes; at most");
}

TEST_CASE("sql query: every kind of write is refused and the file is unchanged") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  const char* writes[] = {
      "INSERT INTO events (tile, seq, sim_tick, game_time, type, depth, origin, subject_hi, "
      "subject_lo, cause) VALUES (9, 1, 0, 0, 0, 0, 0, 0, 0, 0)",
      "INSERT OR REPLACE INTO snapshots (tile, seq, sim_tick, game_time, record_count, blob) "
      "VALUES (0, 0, 0, 0, 0, x'00')",
      "REPLACE INTO snapshots (tile, seq, sim_tick, game_time, record_count, blob) "
      "VALUES (0, 0, 0, 0, 0, x'00')",
      "UPDATE events SET type = 0",
      "DELETE FROM projections",
      "WITH x AS (SELECT 1) DELETE FROM events",
      "INSERT INTO events SELECT * FROM events",
      "DROP TABLE events",
      "DROP INDEX projections_by_entity",
      "CREATE TABLE t (x)",
      "CREATE TABLE t AS SELECT * FROM events",
      "CREATE INDEX i ON events(type)",
      "CREATE VIEW v AS SELECT 1",
      "CREATE TRIGGER g AFTER INSERT ON events BEGIN SELECT 1; END",
      "ALTER TABLE events ADD COLUMN x",
      "ALTER TABLE events RENAME TO e",
      "VACUUM",
      "VACUUM INTO 'copy.db'",
      "REINDEX",
      "ANALYZE",
      "BEGIN",
      "BEGIN IMMEDIATE",
      "COMMIT",
      "SAVEPOINT s",
      "RELEASE s",
      "DETACH main",
  };
  for (const char* sql : writes) {
    CAPTURE(sql);
    const std::string before = fingerprint(path);
    Run run = query(path, sql);
    CAPTURE(run.message);
    CHECK(run.status == Status::InvalidArgument);
    CHECK(run.message.starts_with("refused"));
    CHECK(fingerprint(path) == before);
  }
  CHECK_FALSE(io_exists_for_test(dir.file("copy.db")));
}

TEST_CASE("sql query: ATTACH, PRAGMA, the schema table and temp tables are refused") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  // ATTACH would open (and create) any file the host can reach, and read or write it.
  require_refused(path, "ATTACH DATABASE 'other.db' AS other", "ATTACH");
  require_refused(path, "ATTACH ':memory:' AS m", "ATTACH");
  CHECK_FALSE(io_exists_for_test(dir.file("other.db")));
  // PRAGMA reads settings and writes them: writable_schema would let a statement corrupt the file.
  require_refused(path, "PRAGMA user_version", "PRAGMA");
  require_refused(path, "PRAGMA table_info(events)", "PRAGMA");
  require_refused(path, "PRAGMA writable_schema = 1", "PRAGMA");
  require_refused(path, "PRAGMA writable_schema", "PRAGMA");
  require_refused(path, "PRAGMA query_only = 0", "PRAGMA");
  require_refused(path, "PRAGMA journal_mode = DELETE", "PRAGMA");
  require_refused(path, "PRAGMA main.cache_size = 1000000", "PRAGMA");
  require_refused(path, "PRAGMA trusted_schema = 1", "PRAGMA");
  // The pragma table-valued functions are virtual tables, which the prepare refuses outright.
  require_fails(path, "SELECT * FROM pragma_table_info('events')");
  require_fails(path, "SELECT 1 FROM pragma_function_list");
  // The schema table: what the store holds beyond the three tables is not a client's to read.
  require_refused(path, "SELECT sql FROM sqlite_master", "sqlite_master");
  // SQLite names the table sqlite_master or sqlite_schema depending on the statement.
  require_refused(path, "SELECT name FROM sqlite_schema", "sqlite_");
  require_refused(path, "SELECT count(*) FROM sqlite_schema", "sqlite_");
  require_refused(path, "SELECT 1 FROM main.sqlite_schema", "sqlite_");
  require_fails(path, "SELECT * FROM temp.sqlite_master");
  require_fails(path, "SELECT * FROM sqlite_temp_schema");
  // A temp table is a write to a database of the connection's own.
  require_refused(path, "CREATE TEMP TABLE t (x)", "sqlite_temp_master");
  require_refused(path, "CREATE TEMPORARY TABLE t AS SELECT * FROM events", "sqlite_temp_master");
  require_refused(path, "CREATE TEMP VIEW v AS SELECT 1", "sqlite_temp_master");
}

TEST_CASE("sql query: load_extension and every function off the list are refused") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  require_refused(path, "SELECT load_extension('evil')", "load_extension");
  require_refused(path, "SELECT load_extension('evil', 'entry')", "load_extension");
  require_fails(path, "SELECT fts3_tokenizer('simple')");
  require_refused(path, "SELECT sqlite_compileoption_get(0)", "sqlite_compileoption_get");
  require_refused(path, "SELECT sqlite_version()", "sqlite_version");
  require_refused(path, "SELECT changes()", "changes");
  require_refused(path, "SELECT last_insert_rowid()", "last_insert_rowid");
  require_fails(path, "SELECT sqlite_offset(seq) FROM events");
  require_fails(path, "SELECT no_such_function(1)");
  // Virtual tables, eponymous or not.
  require_fails(path, "SELECT * FROM json_each('[1,2]')");
  require_fails(path, "SELECT * FROM json_tree('{}')");
  require_fails(path, "SELECT * FROM dbstat");
  require_fails(path, "SELECT * FROM generate_series(1, 10)");
  // A function on the list in any case of its name.
  CHECK(scalar(path, "SELECT ABS(-3) + Length('ab')") == 5);
}

TEST_CASE("sql query: a recursive query that never ends is stopped by max_ms") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  SqlQueryLimits limits;
  limits.max_ms = 200;
  const auto start = std::chrono::steady_clock::now();
  Run run = query(path,
                  "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) "
                  "SELECT count(*) FROM c",
                  {}, limits);
  const f64 took =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
  REQUIRE_MESSAGE(run.status == Status::Ok, run.message);
  CHECK_FALSE(run.result.complete);
  CHECK(std::string_view(run.result.bound) == "max_ms");
  CHECK(run.result.rows == 0);
  CHECK(run.result.elapsed_ms >= 200.0);
  CHECK(run.result.instructions > 0);
  // Stopped near its bound, not at the end of the world (generous: the machine is shared).
  CHECK(took < 5000.0);
  MESSAGE("max_ms 200 stopped the query after " << took << " ms and " << run.result.instructions
                                                << " instructions");
  // The rows it returned before the clock ran out are kept.
  Run rows = query(path,
                   "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) "
                   "SELECT x FROM c WHERE x % 1000000 = 0",
                   {}, limits);
  REQUIRE(rows.status == Status::Ok);
  CHECK(std::string_view(rows.result.bound) == "max_ms");
}

TEST_CASE("sql query: a product of the tables is cut at max_rows") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 1000);
  SqlQueryLimits limits;
  limits.max_rows = 100;
  // 1000^3 = a billion rows.
  Run run = query(path, "SELECT a.seq, b.seq, c.seq FROM events a, events b, events c", {}, limits);
  REQUIRE_MESSAGE(run.status == Status::Ok, run.message);
  CHECK(run.result.rows == 100);
  CHECK(run.result.cells.size() == 300);
  CHECK_FALSE(run.result.complete);
  CHECK(std::string_view(run.result.bound) == "max_rows");
  // Exactly max_rows rows is complete: the bound is only named when a row was left behind.
  Run exact = query(path, "SELECT seq FROM events LIMIT 100", {}, limits);
  CHECK(exact.result.rows == 100);
  CHECK(exact.result.complete);
}

TEST_CASE("sql query: values grown inside the statement meet the value limit and max_bytes") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  // Past the value limit: refused while it runs, whatever made it.
  const char* grown[] = {
      "SELECT zeroblob(2000000)",
      "SELECT randomblob(2000000)",
      "SELECT printf('%.*c', 2000000, 'x')",
      "SELECT printf('%2000000d', 1)",
      "SELECT hex(zeroblob(600000))",
      ("WITH RECURSIVE s(x, n) AS (SELECT 'ab', 0 UNION ALL "
       "SELECT replace(x, 'a', 'aa') || x, n + 1 FROM s WHERE n < 40) SELECT max(length(x)) FROM "
       "s"),
      "SELECT group_concat(zeroblob(100000)) FROM events a, events b",
  };
  for (const char* sql : grown) {
    CAPTURE(sql);
    Run run = query(path, sql);
    CAPTURE(run.message);
    CHECK(run.status == Status::InvalidArgument);
    CHECK(run.message.find("value limit") != std::string::npos);
  }
  // Under the value limit but past max_bytes over the rows: cut, with the bound named.
  SqlQueryLimits limits;
  limits.max_bytes = 1024 * 1024;
  Run run = query(path,
                  "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 100) "
                  "SELECT zeroblob(500000) FROM c",
                  {}, limits);
  REQUIRE_MESSAGE(run.status == Status::Ok, run.message);
  CHECK(run.result.rows == 2);
  CHECK(run.result.bytes == 1'000'000);
  CHECK_FALSE(run.result.complete);
  CHECK(std::string_view(run.result.bound) == "max_bytes");
  // A bound value past the limit is refused at the bind.
  const SqlValue big[] = {SqlValue::text(std::string(k_sql_max_value_bytes + 1, 'x'))};
  Run bound = query(path, "SELECT length(?1)", big);
  CHECK(bound.status == Status::InvalidArgument);
}

TEST_CASE("sql query: an expression nested past the depth is refused") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  auto nested = [](u32 depth) {
    std::string sql = "SELECT ";
    for (u32 i = 0; i < depth; ++i)
      sql += "(";
    sql += "1";
    for (u32 i = 0; i < depth; ++i)
      sql += ")";
    return sql;
  };
  auto chained = [](u32 terms) {
    std::string sql = "SELECT 1";
    for (u32 i = 0; i < terms; ++i)
      sql += " + abs(1";
    for (u32 i = 0; i < terms; ++i)
      sql += ")";
    return sql;
  };
  CHECK(scalar(path, nested(10)) == 1);
  for (const std::string& sql : {nested(5000), chained(200), chained(5000)}) {
    CAPTURE(sql.size());
    Run run = query(path, sql);
    CAPTURE(run.message);
    CHECK(run.status != Status::Ok);
  }
  // A compound past the limit.
  std::string compound = "SELECT 1";
  for (u32 i = 0; i < 20; ++i)
    compound += " UNION ALL SELECT 1";
  Run run = query(path, compound);
  CHECK(run.status != Status::Ok);
}

TEST_CASE("sql query: values are bound, never spliced, and come back as they went in") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  // A parameter that would end the statement if it were spliced into the text.
  const std::string attack = "'; DROP TABLE events; --";
  const SqlValue params[] = {SqlValue::text(attack)};
  Run run = query(path, "SELECT ?1, typeof(?1), count(*) FROM events WHERE ?1 IS NOT NULL", params);
  REQUIRE_MESSAGE(run.status == Status::Ok, run.message);
  REQUIRE(run.result.rows == 1);
  CHECK(run.result.cells[0].type == SqlType::Text);
  CHECK(run.result.cells[0].bytes == attack);
  CHECK(run.result.cells[1].bytes == "text");
  CHECK(run.result.cells[2].integer == 8);
  CHECK(scalar(path, "SELECT count(*) FROM events") == 8);

  // Text holding a NUL: bound by length, returned by length, all three bytes.
  const std::string with_nul("a\0b", 3);
  const SqlValue nul[] = {SqlValue::text(with_nul)};
  Run back = query(path, "SELECT ?1, octet_length(?1)", nul);
  REQUIRE_MESSAGE(back.status == Status::Ok, back.message);
  CHECK(back.result.cells[0].type == SqlType::Text);
  CHECK(back.result.cells[0].bytes == with_nul);
  CHECK(back.result.cells[1].integer == 3);
  // In the statement's own text it is refused rather than cut short at the NUL.
  require_refused(path, std::string_view("SELECT 1\0; DROP TABLE events", 29), "NUL");

  // Every storage class round-trips, a real's bits included.
  const u8 raw[] = {0x00, 0x01, 0xFE, 0xFF, 0x00};
  const SqlValue all[] = {SqlValue::null(), SqlValue::of(i64{-9'007'199'254'740'993}),
                          SqlValue::of(0.1), SqlValue::text("ü"), SqlValue::blob(raw)};
  Run types = query(path, "SELECT ?1, ?2, ?3, ?4, ?5", all);
  REQUIRE_MESSAGE(types.status == Status::Ok, types.message);
  CHECK(types.result.cells[0].type == SqlType::Null);
  CHECK(types.result.cells[1].integer == -9'007'199'254'740'993);
  CHECK(types.result.cells[2].real == 0.1);
  CHECK(types.result.cells[3].bytes == "ü");
  CHECK(types.result.cells[4].type == SqlType::Blob);
  CHECK(types.result.cells[4].bytes == std::string(reinterpret_cast<const char*>(raw), 5));
}

TEST_CASE("sql query: a blob column comes back as bytes and compares with a bound blob") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  Run run = query(path,
                  "SELECT blob, typeof(blob), length(blob) FROM projections ORDER BY tile, "
                  "entity_hi, entity_lo LIMIT 1");
  REQUIRE_MESSAGE(run.status == Status::Ok, run.message);
  REQUIRE(run.result.rows == 1);
  const SqlValue& blob = run.result.cells[0];
  CHECK(blob.type == SqlType::Blob);
  REQUIRE(blob.bytes.size() == 4);
  CHECK(static_cast<u8>(blob.bytes[0]) == 0x00);
  CHECK(static_cast<u8>(blob.bytes[2]) == 0xFF);
  CHECK(run.result.cells[1].bytes == "blob");
  CHECK(run.result.cells[2].integer == 4);
  const u8 wanted[] = {0x00, 0x05, 0xFF, 0x00};
  const SqlValue params[] = {SqlValue::blob(wanted)};
  Run match = query(path, "SELECT count(*) FROM projections WHERE blob = ?1", params);
  REQUIRE_MESSAGE(match.status == Status::Ok, match.message);
  CHECK(match.result.cells[0].integer == 1);
}

TEST_CASE("sql query: the connection's own settings are the hardened ones") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 8);
  SqlConnectionReport report;
  std::string message;
  REQUIRE_MESSAGE(describe_sql_connection(path, report, message) == Status::Ok, message);
  CHECK(report.defensive == 1);
  CHECK(report.trusted_schema == 0);
  CHECK(report.triggers == 0);
  CHECK(report.views == 0);
  CHECK(report.dqs_dml == 0);
  CHECK(report.dqs_ddl == 0);
  CHECK(report.load_extension == 0);
  CHECK(report.query_only == 1);
  CHECK(report.mmap_size == 0);
  CHECK(report.read_only == 1);
  CHECK(report.max_value_bytes == static_cast<i64>(k_sql_max_value_bytes));
  CHECK(report.max_text_bytes == static_cast<i64>(k_sql_max_text_bytes));
  CHECK(report.max_columns == static_cast<i64>(k_sql_max_columns));
  CHECK(report.max_expr_depth == static_cast<i64>(k_sql_max_expr_depth));
  CHECK(report.max_compound == static_cast<i64>(k_sql_max_compound));
  CHECK(report.max_attached == 0);
  CHECK(report.max_params == static_cast<i64>(k_sql_max_params));
}

TEST_CASE("sql query: a query beside a world appending holds nothing up and leaves nothing open") {
  TempDir dir("engine_store_sql");
  const std::string path = make_store(dir, 64);
  Database writer;
  REQUIRE(writer.open(path) == Status::Ok);
  EventLog log(writer);
  REQUIRE(log.open() == Status::Ok);
  std::string payload(200, 'p');

  auto append_batch = [&](u32 batch, u64 tick) {
    Vector<EventRecord> records;
    for (u32 i = 0; i < batch; ++i) {
      EventRecord record;
      record.tile = i % 4;
      record.type = 1;
      record.sim_tick = tick;
      record.payload = bytes_of(payload);
      records.push_back(record);
    }
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(log.append(std::span<EventRecord>(records.data(), records.size())) == Status::Ok);
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  auto median = [](std::vector<f64> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };

  std::vector<f64> alone;
  for (u32 t = 0; t < 40; ++t)
    alone.push_back(append_batch(64, 1000 + t));

  std::atomic<bool> running{true};
  Run slow;
  std::thread reader([&] {
    SqlQueryLimits limits;
    limits.max_ms = 600;
    slow = query(path,
                 "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) "
                 "SELECT count(*) FROM c, (SELECT count(*) FROM events)",
                 {}, limits);
    running = false;
  });
  std::vector<f64> beside;
  u64 tick = 2000;
  // Appends for as long as the query runs, at least forty.
  while (running.load() || beside.size() < 40)
    beside.push_back(append_batch(64, tick++));
  reader.join();
  REQUIRE_MESSAGE(slow.status == Status::Ok, slow.message);
  CHECK(std::string_view(slow.result.bound) == "max_ms");

  // The query's connection is gone: a checkpoint that must wait for every reader completes and
  // empties the WAL.
  {
    Statement checkpoint;
    REQUIRE(writer.prepare("PRAGMA wal_checkpoint(TRUNCATE)", checkpoint) == Status::Ok);
    bool row = false;
    REQUIRE(checkpoint.step(row) == Status::Ok);
    REQUIRE(row);
    CHECK(checkpoint.column_i64(0) == 0);  // not busy: no reader holds a snapshot
    CHECK(checkpoint.column_i64(1) == 0);  // the WAL is empty
  }
  MESSAGE("append of 64 events: median "
          << median(alone) << " ms alone, " << median(beside) << " ms beside a 600 ms query ("
          << beside.size() << " appends, max " << *std::max_element(beside.begin(), beside.end())
          << " ms)");
}
