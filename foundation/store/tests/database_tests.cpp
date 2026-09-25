#include <foundation/store/database.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace engine;
using namespace engine::store;

// One unguessable scratch directory per object, so a second copy of this binary — another
// worktree, a release build beside a debug one — cannot delete this one's databases
// (tests/support/test_temp_dir.h).
using TempDir = test::TempDir;

namespace {

i64 scalar(Database& db, const char* sql) {
  Statement stmt;
  REQUIRE(db.prepare(sql, stmt) == Status::Ok);
  bool row = false;
  REQUIRE(stmt.step(row) == Status::Ok);
  REQUIRE(row);
  return stmt.column_i64(0);
}

}  // namespace

TEST_CASE("store: a file opens in WAL mode with the engine's pragmas") {
  TempDir dir("engine_store");
  Database db;
  REQUIRE(db.open(dir.file("world.db")) == Status::Ok);
  CHECK(db.is_open());

  std::string mode;
  REQUIRE(db.journal_mode(mode) == Status::Ok);
  CHECK(mode == "wal");
  CHECK(scalar(db, "PRAGMA synchronous") == 1);  // NORMAL
  CHECK(scalar(db, "PRAGMA page_size") == 8192);
  CHECK(scalar(db, "PRAGMA foreign_keys") == 1);

  // WAL is a property of the file, so a second connection finds it already set.
  Database again;
  REQUIRE(again.open(dir.file("world.db")) == Status::Ok);
  std::string mode_again;
  REQUIRE(again.journal_mode(mode_again) == Status::Ok);
  CHECK(mode_again == "wal");

  // The WAL sidecar exists once something has been written.
  REQUIRE(db.exec("CREATE TABLE t(a INTEGER)") == Status::Ok);
  CHECK(std::filesystem::exists(dir.file("world.db-wal")));
}

TEST_CASE("store: wal off asks for a rollback journal") {
  TempDir dir("engine_store");
  OpenOptions options;
  options.wal = false;
  options.synchronous_normal = false;
  Database db;
  REQUIRE(db.open(dir.file("journal.db"), options) == Status::Ok);
  std::string mode;
  REQUIRE(db.journal_mode(mode) == Status::Ok);
  CHECK(mode == "delete");
  CHECK(scalar(db, "PRAGMA synchronous") == 2);  // FULL
}

TEST_CASE("store: opening a missing file without create is NotFound") {
  TempDir dir("engine_store");
  OpenOptions options;
  options.create = false;
  Database db;
  CHECK(db.open(dir.file("absent.db"), options) == Status::NotFound);
  CHECK(!db.is_open());
}

TEST_CASE("store: a transaction rolls back when its scope exits without a commit") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  REQUIRE(db.exec("CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)") == Status::Ok);

  {
    Transaction transaction;
    REQUIRE(db.begin(transaction) == Status::Ok);
    CHECK(transaction.active());
    REQUIRE(db.exec("INSERT INTO t VALUES(1, 'one')") == Status::Ok);
    CHECK(scalar(db, "SELECT count(*) FROM t") == 1);
    // No commit: the destructor rolls back.
  }
  CHECK(scalar(db, "SELECT count(*) FROM t") == 0);

  {
    Transaction transaction;
    REQUIRE(db.begin(transaction) == Status::Ok);
    REQUIRE(db.exec("INSERT INTO t VALUES(2, 'two')") == Status::Ok);
    REQUIRE(transaction.commit() == Status::Ok);
    CHECK(!transaction.active());
  }
  CHECK(scalar(db, "SELECT count(*) FROM t") == 1);

  {
    Transaction transaction;
    REQUIRE(db.begin(transaction) == Status::Ok);
    REQUIRE(db.exec("INSERT INTO t VALUES(3, 'three')") == Status::Ok);
    transaction.rollback();
    CHECK(!transaction.active());
  }
  CHECK(scalar(db, "SELECT count(*) FROM t") == 1);

  // A second BEGIN while one is open is refused rather than silently nesting.
  Transaction outer;
  REQUIRE(db.begin(outer) == Status::Ok);
  Transaction inner;
  CHECK(db.begin(inner) == Status::InvalidArgument);
}

TEST_CASE("store: binds and columns round-trip every supported type") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  REQUIRE(db.exec("CREATE TABLE t(i INTEGER, d REAL, s TEXT, b BLOB, n INTEGER)") == Status::Ok);

  const u8 blob[] = {0, 1, 2, 250, 255};
  {
    Statement stmt;
    REQUIRE(db.prepare("INSERT INTO t VALUES(?1,?2,?3,?4,?5)", stmt) == Status::Ok);
    stmt.bind(1, i64{-9'007'199'254'740'993})
        .bind(2, 0.5)
        .bind(3, std::string_view{"héllo"})
        .bind_ref(4, std::span<const u8>(blob, 5))
        .bind_null(5);
    REQUIRE(stmt.run() == Status::Ok);
  }
  CHECK(db.changes() == 1);

  Statement stmt;
  REQUIRE(db.prepare("SELECT i,d,s,b,n FROM t", stmt) == Status::Ok);
  bool row = false;
  REQUIRE(stmt.step(row) == Status::Ok);
  REQUIRE(row);
  CHECK(stmt.column_count() == 5);
  CHECK(stmt.column_i64(0) == -9'007'199'254'740'993);
  CHECK(stmt.column_f64(1) == doctest::Approx(0.5));
  CHECK(stmt.column_text(2) == "héllo");
  const std::span<const u8> out = stmt.column_blob(3);
  REQUIRE(out.size() == 5);
  CHECK(out[3] == 250);
  CHECK(stmt.column_is_null(4));
  REQUIRE(stmt.step(row) == Status::Ok);
  CHECK(!row);
}

TEST_CASE("store: the statement cache reuses one prepared statement per SQL text") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  REQUIRE(db.exec("CREATE TABLE t(a INTEGER)") == Status::Ok);

  const char* sql = "INSERT INTO t VALUES(?1)";
  sqlite3_stmt* first = nullptr;
  for (int i = 0; i < 4; ++i) {
    Statement stmt;
    REQUIRE(db.prepare(sql, stmt) == Status::Ok);
    if (first == nullptr) first = stmt.handle();
    CHECK(stmt.handle() == first);  // same cursor every time, so one parse for the connection
    stmt.bind(1, static_cast<i64>(i));
    REQUIRE(stmt.run() == Status::Ok);
  }
  CHECK(scalar(db, "SELECT count(*) FROM t") == 4);

  // Two live borrows of the same SQL get different cursors, so neither walks the other's rows.
  Statement a;
  Statement b;
  REQUIRE(db.prepare("SELECT a FROM t ORDER BY a", a) == Status::Ok);
  REQUIRE(db.prepare("SELECT a FROM t ORDER BY a", b) == Status::Ok);
  CHECK(a.handle() != b.handle());
  bool row = false;
  REQUIRE(a.step(row) == Status::Ok);
  CHECK(a.column_i64(0) == 0);
  REQUIRE(b.step(row) == Status::Ok);
  CHECK(b.column_i64(0) == 0);
}

TEST_CASE("store: migrations run once, in order, and bump user_version") {
  TempDir dir("engine_store");
  const std::string path = dir.file("migrate.db");

  static constexpr Migration k_v1[] = {
      {1, "CREATE TABLE a(x INTEGER); INSERT INTO a VALUES(1)"},
  };
  static constexpr Migration k_v2[] = {
      {1, "CREATE TABLE a(x INTEGER); INSERT INTO a VALUES(1)"},
      {2, "ALTER TABLE a ADD COLUMN y TEXT; UPDATE a SET y='two'"},
  };

  {
    Database db;
    REQUIRE(db.open(path) == Status::Ok);
    i32 version = -1;
    REQUIRE(db.user_version(version) == Status::Ok);
    CHECK(version == 0);
    REQUIRE(db.migrate(std::span<const Migration>(k_v1, 1)) == Status::Ok);
    REQUIRE(db.user_version(version) == Status::Ok);
    CHECK(version == 1);
    // Running the same set again is a no-op, not a second CREATE TABLE.
    REQUIRE(db.migrate(std::span<const Migration>(k_v1, 1)) == Status::Ok);
    CHECK(scalar(db, "SELECT count(*) FROM a") == 1);
  }
  {
    Database db;
    REQUIRE(db.open(path) == Status::Ok);
    REQUIRE(db.migrate(std::span<const Migration>(k_v2, 2)) == Status::Ok);
    i32 version = -1;
    REQUIRE(db.user_version(version) == Status::Ok);
    CHECK(version == 2);
    Statement stmt;
    REQUIRE(db.prepare("SELECT y FROM a", stmt) == Status::Ok);
    bool row = false;
    REQUIRE(stmt.step(row) == Status::Ok);
    REQUIRE(row);
    CHECK(stmt.column_text(0) == "two");
  }
  {
    // A file written by a newer build is refused, not silently read at the wrong shape.
    Database db;
    REQUIRE(db.open(path) == Status::Ok);
    CHECK(db.migrate(std::span<const Migration>(k_v1, 1)) == Status::SchemaTooNew);
    CHECK(!db.last_error().empty());
  }

  // Versions that do not ascend are the caller's mistake and nothing is applied.
  static constexpr Migration k_bad[] = {{2, "CREATE TABLE b(x)"}, {1, "CREATE TABLE c(x)"}};
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  CHECK(db.migrate(std::span<const Migration>(k_bad, 2)) == Status::InvalidArgument);
}

TEST_CASE("store: a failing migration leaves the file at its old version") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  static constexpr Migration k_broken[] = {
      {1, "CREATE TABLE a(x INTEGER)"},
      {2, "CREATE TABLE a(x INTEGER)"},  // already exists: the second migration fails
  };
  CHECK(db.migrate(std::span<const Migration>(k_broken, 2)) == Status::SqlError);
  i32 version = -1;
  REQUIRE(db.user_version(version) == Status::Ok);
  CHECK(version == 1);
  CHECK(scalar(db, "SELECT count(*) FROM sqlite_schema WHERE name='a'") == 1);
}

TEST_CASE("store: errors are statuses, not exceptions") {
  Database db;
  CHECK(db.exec("SELECT 1") == Status::NotOpen);
  REQUIRE(db.open_memory() == Status::Ok);
  CHECK(db.exec("SELECT nonsense FROM nowhere") == Status::SqlError);
  CHECK(!db.last_error().empty());

  Statement stmt;
  CHECK(db.prepare("NOT SQL", stmt) == Status::SqlError);
  CHECK(!stmt.valid());

  REQUIRE(db.exec("CREATE TABLE t(a INTEGER PRIMARY KEY)") == Status::Ok);
  REQUIRE(db.exec("INSERT INTO t VALUES(1)") == Status::Ok);
  CHECK(db.exec("INSERT INTO t VALUES(1)") == Status::Constraint);

  // SQLITE_DQS=0: a double-quoted string is an identifier, so a typo is an error rather than a
  // silently constant column.
  CHECK(db.exec("SELECT \"no_such_column\" FROM t") == Status::SqlError);

  CHECK(std::string_view(status_name(Status::Busy)) == "Busy");
}

TEST_CASE("store: file_size_bytes reports pages times page size") {
  TempDir dir("engine_store");
  Database db;
  REQUIRE(db.open(dir.file("size.db")) == Status::Ok);
  REQUIRE(db.exec("CREATE TABLE t(a INTEGER PRIMARY KEY, b BLOB)") == Status::Ok);
  Transaction transaction;
  REQUIRE(db.begin(transaction) == Status::Ok);
  Statement stmt;
  REQUIRE(db.prepare("INSERT INTO t VALUES(?1, zeroblob(1024))", stmt) == Status::Ok);
  for (i64 i = 0; i < 256; ++i) {
    stmt.reset();
    stmt.bind(1, i);
    REQUIRE(stmt.run() == Status::Ok);
  }
  REQUIRE(transaction.commit() == Status::Ok);
  i64 bytes = 0;
  REQUIRE(db.file_size_bytes(bytes) == Status::Ok);
  CHECK(bytes >= 256 * 1024);
  CHECK(bytes % 8192 == 0);
}

namespace {

std::string file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// Backups (store.md, "Backups"): VACUUM INTO, a consistent snapshot of a WAL database whatever
// is still in its WAL, whose bytes are a function of the content — a backup of the backup is the
// same file — which is what lets a save round-trip byte for byte through a load.
TEST_CASE("store: a backup is a consistent copy whose bytes depend on its content alone") {
  TempDir dir("engine_store_backup");
  const std::string live = dir.file("live.db");
  Database db;
  REQUIRE(db.open(live) == Status::Ok);
  REQUIRE(db.exec("CREATE TABLE t(k INTEGER PRIMARY KEY, v BLOB); CREATE INDEX t_v ON t(v);") ==
          Status::Ok);
  // Rows committed but not checkpointed: they are in the WAL, not yet in the main file.
  for (i64 k = 0; k < 200; ++k) {
    Statement insert;
    REQUIRE(db.prepare("INSERT INTO t(k,v) VALUES(?1,?2)", insert) == Status::Ok);
    const std::string v = "value " + std::to_string(k * 7919 % 211);
    insert.bind(1, k).bind(2, std::string_view(v));
    REQUIRE(insert.run() == Status::Ok);
  }
  // History the content does not show: a table made and dropped, rows deleted.
  REQUIRE(db.exec("CREATE TABLE gone(a); DROP TABLE gone; DELETE FROM t WHERE k % 3 = 0;") ==
          Status::Ok);
  REQUIRE(std::filesystem::exists(live + "-wal"));

  const std::string first = dir.file("first.db");
  REQUIRE(db.backup_to(first) == Status::Ok);
  // A backup is a rollback-journal file with nothing beside it, holding what the WAL held.
  CHECK_FALSE(std::filesystem::exists(first + "-wal"));
  {
    Database copy;
    OpenOptions options;
    options.read_only = true;
    options.wal = false;
    options.create = false;
    REQUIRE(copy.open(first, options) == Status::Ok);
    CHECK(scalar(copy, "SELECT count(*) FROM t") == 133);
    CHECK(scalar(copy, "SELECT sum(k) FROM t") == scalar(db, "SELECT sum(k) FROM t"));
    CHECK(scalar(copy, "PRAGMA page_size") == 8192);
    CHECK(scalar(copy, "PRAGMA schema_version") == 1);
    CHECK(scalar(copy, "PRAGMA freelist_count") == 0);
  }

  // The backup of the backup, through a connection that opened it in WAL mode as a live store
  // would, is the same bytes: nothing of either file's history survives into its copy.
  const std::string again = dir.file("again.db");
  {
    const std::string reopened = dir.file("reopened.db");
    std::filesystem::copy_file(first, reopened);
    Database second;
    REQUIRE(second.open(reopened) == Status::Ok);
    REQUIRE(second.backup_to(again) == Status::Ok);
  }
  const std::string a = file_bytes(first);
  const std::string b = file_bytes(again);
  REQUIRE_FALSE(a.empty());
  CHECK(a.size() == b.size());
  CHECK(a == b);

  // A path that is there is refused, and the file is left alone.
  CHECK(db.backup_to(first) == Status::AlreadyExists);
  CHECK(file_bytes(first) == a);
  // So is a backup inside a transaction, which VACUUM cannot run in.
  Transaction transaction;
  REQUIRE(db.begin(transaction) == Status::Ok);
  CHECK(db.backup_to(dir.file("inside.db")) == Status::InvalidArgument);
}
