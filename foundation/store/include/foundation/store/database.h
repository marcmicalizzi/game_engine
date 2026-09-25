#pragma once

// SQLite behind an engine-shaped API (docs/plan/03-data-model.md §3.5, ADR-0003).
//
// The point of SQLite in this engine is that agents and tests can run SQL against world state
// (plan 03 §3.5), so this is a thin, honest wrapper, not an abstraction over storage.
// `Database::handle()` hands out the `sqlite3*` and nothing here pretends the engine could swap
// the store for something else.
//
// What the wrapper is for:
//
//   - Errors as `Status`, in the shape `foundation/io` already uses, because exceptions are off
//     in engine targets and a bare `int` return from SQLite tells a caller nothing.
//   - A prepared-statement cache. Preparing a statement parses and plans the SQL; a save file
//     written every tick runs the same dozen statements forever, so preparing them once is the
//     difference between a store you can call per tick and one you cannot.
//   - RAII transactions that roll back when a scope exits without a commit, so an early return
//     on an error path cannot leave a half-written tick in the file.
//   - `user_version` migrations, which is SQLite's own answer to schema evolution (plan 03 §3.8).
//
// Threading: the library is compiled `SQLITE_THREADSAFE=2` (multi-thread), so one `Database`
// belongs to one thread at a time. Two threads that both want the store open two connections to
// the same file; WAL mode is what makes that concurrent-reader case work.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>

#include <span>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace engine::store {

enum class Status : u8 {
  Ok,
  NotFound,  // no row, or no such file with `create` off
  AlreadyExists,
  Busy,        // another connection holds the lock past the busy timeout
  Constraint,  // a UNIQUE, CHECK, or NOT NULL constraint refused the row
  Corrupt,
  ReadOnly,
  IoError,
  NotOpen,          // the call needs an open database
  InvalidArgument,  // the caller passed inconsistent arguments (nothing was touched)
  SqlError,         // the statement did not compile or did not run; see last_error()
  SchemaTooNew,     // the file's user_version is beyond what this build knows how to read
};
const char* status_name(Status status) noexcept;

struct OpenOptions {
  bool read_only = false;
  bool create = true;
  // WAL lets readers run while a writer commits and turns a commit into an append; the
  // alternative (a rollback journal) blocks readers for the length of every write. A save file
  // that a simulation tick writes to while a streaming thread reads from it wants WAL.
  bool wal = true;
  // With WAL, `synchronous = NORMAL` fsyncs at checkpoints rather than at every commit: a crash
  // may lose the last transactions, and can never corrupt the database. FULL costs an fsync per
  // tick to save transactions the tick loop would replay from the event log anyway.
  bool synchronous_normal = true;
  u32 busy_timeout_ms = 5'000;
  // 8 KiB rather than SQLite's 4 KiB default. A projection blob is 64–256 bytes and the
  // projections table is WITHOUT ROWID, and SQLite spills a WITHOUT ROWID row to an overflow
  // page once it exceeds about 1/20 of a page; at 4 KiB that threshold is ~204 bytes, which a
  // 256-byte blob crosses. 8 KiB puts the whole record inline and halves the depth of the
  // million-row B-tree. Only ever applied to a database being created.
  u32 page_size = 8192;
  // Page cache, in kibibytes. 0 keeps SQLite's default (2 MiB as of 3.53).
  u32 cache_kib = 0;
  u32 statement_cache_capacity = 64;
};

// One step of the schema's history (docs/plan/03-data-model.md §3.8). `sql` may hold several
// statements; the whole migration and the `user_version` bump commit together, so a file is
// never at a half-applied version.
struct Migration {
  i32 version = 0;  // the user_version this migration produces; ascending, starts at 1
  const char* sql = "";
};

class Database;

// A prepared statement borrowed from the database's cache. Destruction resets the statement and
// returns it, so a Statement is a scope rather than an owner and the same SQL text costs one
// parse for the life of the connection.
class Statement {
 public:
  Statement() noexcept = default;
  ~Statement();
  Statement(Statement&& other) noexcept;
  Statement& operator=(Statement&& other) noexcept;
  ENGINE_NON_COPYABLE(Statement);

  bool valid() const noexcept { return stmt_ != nullptr; }
  sqlite3_stmt* handle() const noexcept { return stmt_; }

  // Parameter indices are 1-based, as SQLite numbers them.
  Statement& bind(int index, i64 value) noexcept;
  Statement& bind(int index, f64 value) noexcept;
  // Copied into the statement, so the caller's buffer need not outlive the bind.
  Statement& bind(int index, std::string_view text) noexcept;
  Statement& bind(int index, std::span<const u8> blob) noexcept;
  // Bound by reference: no copy, and the caller keeps the bytes alive until the statement is
  // reset or stepped to completion. This is the bind that a per-tick write should use.
  Statement& bind_ref(int index, std::string_view text) noexcept;
  Statement& bind_ref(int index, std::span<const u8> blob) noexcept;
  Statement& bind_null(int index) noexcept;

  // Advances one row. `row` is true while a row is available, false once the statement is done.
  // A Status other than Ok means the step failed and nothing may be read.
  Status step(bool& row) noexcept;
  // Steps a statement that returns no rows to completion.
  Status run() noexcept;
  // Forgets the cursor and the bindings; the statement can be bound and stepped again.
  void reset() noexcept;

  // Column indices are 0-based, as SQLite numbers them.
  int column_count() const noexcept;
  bool column_is_null(int index) const noexcept;
  i64 column_i64(int index) const noexcept;
  f64 column_f64(int index) const noexcept;
  // Valid until the next step() or reset().
  std::string_view column_text(int index) const noexcept;
  std::span<const u8> column_blob(int index) const noexcept;

 private:
  friend class Database;
  Statement(Database* db, sqlite3_stmt* stmt, u32 slot) noexcept
      : db_(db), stmt_(stmt), slot_(slot) {}
  void release() noexcept;

  Database* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
  u32 slot_ = 0;  // index into the database's cache; k_uncached for a one-off statement
};

// A transaction that rolls back unless it is committed. Obtained from Database::begin().
class Transaction {
 public:
  Transaction() noexcept = default;
  ~Transaction();
  Transaction(Transaction&& other) noexcept;
  Transaction& operator=(Transaction&& other) noexcept;
  ENGINE_NON_COPYABLE(Transaction);

  bool active() const noexcept { return db_ != nullptr; }
  Status commit() noexcept;
  void rollback() noexcept;

 private:
  friend class Database;
  explicit Transaction(Database* db) noexcept : db_(db) {}
  Database* db_ = nullptr;
};

class Database {
 public:
  Database() noexcept = default;
  ~Database();
  ENGINE_NON_COPYABLE(Database);

  // `native_path` is a UTF-8 filesystem path; ":memory:" works but open_memory() says so.
  Status open(std::string_view native_path, const OpenOptions& options);
  Status open(std::string_view native_path);
  Status open_memory(const OpenOptions& options);
  Status open_memory();
  void close() noexcept;
  bool is_open() const noexcept { return db_ != nullptr; }
  sqlite3* handle() const noexcept { return db_; }
  std::string_view path() const noexcept { return path_; }

  // SQL with no parameters, for DDL and pragmas. Several statements are allowed.
  Status exec(std::string_view sql);

  // A statement from the cache, prepared on first use. The returned Statement borrows it and
  // gives it back on destruction; asking for the same SQL twice at once prepares a second,
  // uncached copy rather than handing out the same cursor.
  Status prepare(std::string_view sql, Statement& out);

  // `immediate` takes the write lock now instead of on the first write, which is how a writer
  // avoids SQLITE_BUSY halfway through a transaction it cannot restart.
  Status begin(Transaction& out, bool immediate);
  Status begin(Transaction& out);

  // Applies every migration whose version is above the file's user_version, in order, each in
  // its own transaction, and leaves user_version at the last one. A file already at or beyond
  // the last version is left alone; a file beyond it is SchemaTooNew.
  Status migrate(std::span<const Migration> migrations);
  Status user_version(i32& out);
  Status set_user_version(i32 version);

  // A consistent, compact copy of the database in a new file at `native_path`, which must not
  // exist: `VACUUM INTO`, so the copy is one read transaction's snapshot whatever other
  // connections are writing, the WAL's committed frames are in it, and it has no free pages. The
  // copy is written in rollback-journal mode at the page size this connection was opened with.
  //
  // **Its bytes are a function of the database's content**, not of its history. `VACUUM INTO`
  // lays every table out afresh in key order, and the one header field that still remembers
  // history — the schema cookie, which it sets to the source's plus one, so that a backup of a
  // backup would differ from the backup by one — is set to 1 once the copy is written, through a
  // connection of its own on a file nothing else has open (which is the one case the pragma is safe
  // in). That is what lets a save round-trip byte for byte through a load (docs/subsystems/
  // store.md, "Backups"). AlreadyExists when the path does; the call runs outside a transaction.
  Status backup_to(std::string_view native_path);

  // "wal", "delete", "memory", ... as the connection reports it.
  Status journal_mode(std::string& out);
  // page_count * page_size, which is the file's size on disk for a database with no WAL
  // pending; the WAL file is separate.
  Status file_size_bytes(i64& out);

  i64 last_insert_rowid() const noexcept;
  i64 changes() const noexcept;
  // SQLite's message for the most recent failure. Empty when nothing failed.
  std::string_view last_error() const noexcept { return error_; }

 private:
  friend class Statement;
  friend class Transaction;

  struct CacheEntry {
    std::string sql;
    sqlite3_stmt* stmt = nullptr;
    bool borrowed = false;
  };

  Status apply_pragmas(const OpenOptions& options);
  Status fail(int rc, std::string_view what);
  void record_error(std::string_view what) noexcept;
  void give_back(u32 slot, sqlite3_stmt* stmt) noexcept;

  sqlite3* db_ = nullptr;
  std::string path_;
  std::string error_;
  Vector<CacheEntry> cache_;
  HashMap<std::string, u32> cache_index_;
  u32 cache_capacity_ = 64;
  bool in_transaction_ = false;
};

}  // namespace engine::store
