#include <core/base/assert.h>
#include <core/log/log.h>
#include <foundation/store/database.h>

#include <sqlite3.h>

#include <utility>

namespace engine::store {

ENGINE_LOG_CATEGORY_DEFINE(log_store, "store");

namespace {

constexpr u32 k_uncached = 0xFFFF'FFFFu;

Status status_from_sqlite(int rc) noexcept {
  switch (rc & 0xFF) {
    case SQLITE_OK:
    case SQLITE_ROW:
    case SQLITE_DONE: return Status::Ok;
    case SQLITE_BUSY:
    case SQLITE_LOCKED: return Status::Busy;
    case SQLITE_CONSTRAINT: return Status::Constraint;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB: return Status::Corrupt;
    case SQLITE_READONLY: return Status::ReadOnly;
    case SQLITE_IOERR:
    case SQLITE_FULL:
    case SQLITE_CANTOPEN: return Status::IoError;
    case SQLITE_NOMEM: return Status::IoError;
    case SQLITE_MISUSE: return Status::InvalidArgument;
    default: return Status::SqlError;
  }
}

int text_size(std::string_view text) noexcept { return static_cast<int>(text.size()); }
int blob_size(std::span<const u8> blob) noexcept { return static_cast<int>(blob.size()); }

}  // namespace

const char* status_name(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "Ok";
    case Status::NotFound: return "NotFound";
    case Status::AlreadyExists: return "AlreadyExists";
    case Status::Busy: return "Busy";
    case Status::Constraint: return "Constraint";
    case Status::Corrupt: return "Corrupt";
    case Status::ReadOnly: return "ReadOnly";
    case Status::IoError: return "IoError";
    case Status::NotOpen: return "NotOpen";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::SqlError: return "SqlError";
    case Status::SchemaTooNew: return "SchemaTooNew";
  }
  return "Unknown";
}

// ---- Statement -------------------------------------------------------------------------------

Statement::~Statement() { release(); }

Statement::Statement(Statement&& other) noexcept
    : db_(other.db_), stmt_(other.stmt_), slot_(other.slot_) {
  other.db_ = nullptr;
  other.stmt_ = nullptr;
  other.slot_ = 0;
}

Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    release();
    db_ = other.db_;
    stmt_ = other.stmt_;
    slot_ = other.slot_;
    other.db_ = nullptr;
    other.stmt_ = nullptr;
    other.slot_ = 0;
  }
  return *this;
}

void Statement::release() noexcept {
  if (stmt_ == nullptr) return;
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
  if (slot_ == k_uncached || db_ == nullptr) {
    sqlite3_finalize(stmt_);
  } else {
    db_->give_back(slot_, stmt_);
  }
  db_ = nullptr;
  stmt_ = nullptr;
  slot_ = 0;
}

Statement& Statement::bind(int index, i64 value) noexcept {
  if (stmt_ != nullptr) sqlite3_bind_int64(stmt_, index, value);
  return *this;
}
Statement& Statement::bind(int index, f64 value) noexcept {
  if (stmt_ != nullptr) sqlite3_bind_double(stmt_, index, value);
  return *this;
}
Statement& Statement::bind(int index, std::string_view text) noexcept {
  if (stmt_ != nullptr) {
    sqlite3_bind_text(stmt_, index, text.data(), text_size(text), SQLITE_TRANSIENT);
  }
  return *this;
}
Statement& Statement::bind(int index, std::span<const u8> blob) noexcept {
  if (stmt_ != nullptr) {
    sqlite3_bind_blob(stmt_, index, blob.data(), blob_size(blob), SQLITE_TRANSIENT);
  }
  return *this;
}
Statement& Statement::bind_ref(int index, std::string_view text) noexcept {
  if (stmt_ != nullptr) {
    sqlite3_bind_text(stmt_, index, text.data(), text_size(text), SQLITE_STATIC);
  }
  return *this;
}
Statement& Statement::bind_ref(int index, std::span<const u8> blob) noexcept {
  if (stmt_ != nullptr) {
    sqlite3_bind_blob(stmt_, index, blob.data(), blob_size(blob), SQLITE_STATIC);
  }
  return *this;
}
Statement& Statement::bind_null(int index) noexcept {
  if (stmt_ != nullptr) sqlite3_bind_null(stmt_, index);
  return *this;
}

Status Statement::step(bool& row) noexcept {
  row = false;
  if (stmt_ == nullptr) return Status::InvalidArgument;
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) {
    row = true;
    return Status::Ok;
  }
  if (rc == SQLITE_DONE) return Status::Ok;
  if (db_ != nullptr) db_->record_error("step");
  return status_from_sqlite(rc);
}

Status Statement::run() noexcept {
  bool row = false;
  for (;;) {
    const Status status = step(row);
    if (status != Status::Ok) return status;
    if (!row) return Status::Ok;
  }
}

void Statement::reset() noexcept {
  if (stmt_ == nullptr) return;
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

int Statement::column_count() const noexcept {
  return stmt_ != nullptr ? sqlite3_column_count(stmt_) : 0;
}
bool Statement::column_is_null(int index) const noexcept {
  return stmt_ == nullptr || sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}
i64 Statement::column_i64(int index) const noexcept {
  return stmt_ != nullptr ? sqlite3_column_int64(stmt_, index) : 0;
}
f64 Statement::column_f64(int index) const noexcept {
  return stmt_ != nullptr ? sqlite3_column_double(stmt_, index) : 0.0;
}
std::string_view Statement::column_text(int index) const noexcept {
  if (stmt_ == nullptr) return {};
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
  if (text == nullptr) return {};
  return {text, static_cast<usize>(sqlite3_column_bytes(stmt_, index))};
}
std::span<const u8> Statement::column_blob(int index) const noexcept {
  if (stmt_ == nullptr) return {};
  const auto* bytes = static_cast<const u8*>(sqlite3_column_blob(stmt_, index));
  if (bytes == nullptr) return {};
  return {bytes, static_cast<usize>(sqlite3_column_bytes(stmt_, index))};
}

// ---- Transaction -----------------------------------------------------------------------------

Transaction::~Transaction() { rollback(); }

Transaction::Transaction(Transaction&& other) noexcept : db_(other.db_) { other.db_ = nullptr; }

Transaction& Transaction::operator=(Transaction&& other) noexcept {
  if (this != &other) {
    rollback();
    db_ = other.db_;
    other.db_ = nullptr;
  }
  return *this;
}

Status Transaction::commit() noexcept {
  if (db_ == nullptr) return Status::InvalidArgument;
  Database* db = db_;
  db_ = nullptr;
  const Status status = db->exec("COMMIT");
  db->in_transaction_ = false;
  if (status != Status::Ok) {
    // A failed COMMIT leaves the transaction open in every case SQLite documents except a
    // deferred-write conflict, and rolling back an already-closed transaction is harmless.
    db->exec("ROLLBACK");
  }
  return status;
}

void Transaction::rollback() noexcept {
  if (db_ == nullptr) return;
  Database* db = db_;
  db_ = nullptr;
  db->exec("ROLLBACK");
  db->in_transaction_ = false;
}

// ---- Database --------------------------------------------------------------------------------

Database::~Database() { close(); }

void Database::record_error(std::string_view what) noexcept {
  error_.assign(what);
  if (db_ != nullptr) {
    const char* message = sqlite3_errmsg(db_);
    if (message != nullptr) {
      error_ += ": ";
      error_ += message;
    }
  }
}

Status Database::fail(int rc, std::string_view what) {
  record_error(what);
  const Status status = status_from_sqlite(rc);
  ENGINE_LOG_WARN(log_store, "sqlite call failed", log::field("what", what), log::field("rc", rc),
                  log::field("error", error_));
  return status;
}

Status Database::open(std::string_view native_path) { return open(native_path, OpenOptions{}); }
Status Database::open_memory() { return open_memory(OpenOptions{}); }

Status Database::open_memory(const OpenOptions& options) {
  OpenOptions in_memory = options;
  in_memory.wal = false;  // an in-memory database has no journal to put in WAL mode
  return open(":memory:", in_memory);
}

Status Database::open(std::string_view native_path, const OpenOptions& options) {
  close();
  path_.assign(native_path);
  cache_capacity_ = options.statement_cache_capacity;

  int flags = options.read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
  if (!options.read_only && options.create) flags |= SQLITE_OPEN_CREATE;
  flags |= SQLITE_OPEN_NOMUTEX;  // one connection, one thread at a time (SQLITE_THREADSAFE=2)

  const int rc = sqlite3_open_v2(path_.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    const Status status = fail(rc, "open");
    if (db_ != nullptr) {
      sqlite3_close_v2(db_);
      db_ = nullptr;
    }
    return rc == SQLITE_CANTOPEN && !options.create ? Status::NotFound : status;
  }

  const Status status = apply_pragmas(options);
  if (status != Status::Ok) {
    close();
    return status;
  }
  ENGINE_LOG_INFO(log_store, "database open", log::field("path", path_),
                  log::field("wal", options.wal));
  return Status::Ok;
}

Status Database::apply_pragmas(const OpenOptions& options) {
  char sql[128];

  sqlite3_busy_timeout(db_, static_cast<int>(options.busy_timeout_ms));

  // page_size takes effect only on a database that has no pages yet, and is silently ignored on
  // one that does, so it can be issued unconditionally before anything else touches the file.
  if (!options.read_only) {
    sqlite3_snprintf(sizeof(sql), sql, "PRAGMA page_size=%u", options.page_size);
    const Status status = exec(sql);
    if (status != Status::Ok) return status;
  }

  if (options.wal) {
    // journal_mode returns a row; exec() steps it away. A file on a filesystem that cannot do
    // WAL (some network mounts) silently stays in its old mode, which journal_mode() reports.
    const Status status = exec("PRAGMA journal_mode=WAL");
    if (status != Status::Ok) return status;
  } else if (!options.read_only) {
    const Status status = exec("PRAGMA journal_mode=DELETE");
    if (status != Status::Ok) return status;
  }

  const Status sync =
      exec(options.synchronous_normal ? "PRAGMA synchronous=NORMAL" : "PRAGMA synchronous=FULL");
  if (sync != Status::Ok) return sync;

  if (options.cache_kib != 0) {
    sqlite3_snprintf(sizeof(sql), sql, "PRAGMA cache_size=-%u", options.cache_kib);
    const Status status = exec(sql);
    if (status != Status::Ok) return status;
  }

  // Referential integrity is off by default in SQLite for backwards compatibility; the world
  // store has real foreign keys and wants them enforced.
  return exec("PRAGMA foreign_keys=ON");
}

void Database::close() noexcept {
  for (CacheEntry& entry : cache_) {
    if (entry.stmt != nullptr) sqlite3_finalize(entry.stmt);
    entry.stmt = nullptr;
  }
  cache_.clear();
  cache_index_.clear();
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
  in_transaction_ = false;
  error_.clear();
}

Status Database::exec(std::string_view sql) {
  if (db_ == nullptr) return Status::NotOpen;
  const char* begin = sql.data();
  const char* end = begin + sql.size();
  while (begin < end) {
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v2(db_, begin, static_cast<int>(end - begin), &stmt, &tail);
    if (rc != SQLITE_OK) return fail(rc, "prepare");
    if (stmt == nullptr) break;  // trailing whitespace or a comment
    int step_rc = sqlite3_step(stmt);
    while (step_rc == SQLITE_ROW)
      step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step_rc != SQLITE_DONE) return fail(step_rc, "exec");
    begin = tail;
  }
  return Status::Ok;
}

Status Database::prepare(std::string_view sql, Statement& out) {
  out = Statement{};
  if (db_ == nullptr) return Status::NotOpen;

  if (const u32* slot = cache_index_.find_value(sql); slot != nullptr) {
    CacheEntry& entry = cache_[*slot];
    if (!entry.borrowed) {
      entry.borrowed = true;
      out = Statement(this, entry.stmt, *slot);
      return Status::Ok;
    }
  }

  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v3(db_, sql.data(), static_cast<int>(sql.size()),
                                    SQLITE_PREPARE_PERSISTENT, &stmt, nullptr);
  if (rc != SQLITE_OK) return fail(rc, "prepare");

  // Cache the statement unless the SQL is already cached (and borrowed) or the cache is full;
  // an uncached statement is finalized when its Statement goes away.
  if (!cache_index_.contains(sql) && cache_.size() < cache_capacity_) {
    const u32 slot = static_cast<u32>(cache_.size());
    CacheEntry entry;
    entry.sql.assign(sql);
    entry.stmt = stmt;
    entry.borrowed = true;
    cache_.push_back(std::move(entry));
    cache_index_.insert(std::string(sql), slot);
    out = Statement(this, stmt, slot);
  } else {
    out = Statement(this, stmt, k_uncached);
  }
  return Status::Ok;
}

void Database::give_back(u32 slot, sqlite3_stmt* stmt) noexcept {
  ENGINE_ASSERT(slot < cache_.size(), "store: statement slot out of range");
  ENGINE_ASSERT(cache_[slot].stmt == stmt, "store: statement returned to the wrong slot");
  (void)stmt;
  cache_[slot].borrowed = false;
}

Status Database::begin(Transaction& out) { return begin(out, true); }

Status Database::begin(Transaction& out, bool immediate) {
  out = Transaction{};
  if (db_ == nullptr) return Status::NotOpen;
  if (in_transaction_) return Status::InvalidArgument;  // nested transactions are savepoints
  const Status status = exec(immediate ? "BEGIN IMMEDIATE" : "BEGIN");
  if (status != Status::Ok) return status;
  in_transaction_ = true;
  out = Transaction(this);
  return Status::Ok;
}

Status Database::user_version(i32& out) {
  out = 0;
  Statement stmt;
  const Status status = prepare("PRAGMA user_version", stmt);
  if (status != Status::Ok) return status;
  bool row = false;
  const Status step = stmt.step(row);
  if (step != Status::Ok) return step;
  if (!row) return Status::SqlError;
  out = static_cast<i32>(stmt.column_i64(0));
  return Status::Ok;
}

Status Database::set_user_version(i32 version) {
  // PRAGMA does not take a bound parameter, so the value is formatted in; it is an i32 the
  // caller supplied through a typed field, never text from outside.
  char sql[64];
  sqlite3_snprintf(sizeof(sql), sql, "PRAGMA user_version=%d", version);
  return exec(sql);
}

Status Database::migrate(std::span<const Migration> migrations) {
  if (db_ == nullptr) return Status::NotOpen;
  i32 current = 0;
  Status status = user_version(current);
  if (status != Status::Ok) return status;

  i32 highest = 0;
  for (const Migration& migration : migrations) {
    if (migration.version <= highest) return Status::InvalidArgument;  // must ascend
    highest = migration.version;
  }
  if (current > highest) {
    record_error("user_version is beyond the last known migration");
    return Status::SchemaTooNew;
  }

  for (const Migration& migration : migrations) {
    if (migration.version <= current) continue;
    Transaction transaction;
    status = begin(transaction, true);
    if (status != Status::Ok) return status;
    status = exec(migration.sql);
    if (status != Status::Ok) return status;
    status = set_user_version(migration.version);
    if (status != Status::Ok) return status;
    status = transaction.commit();
    if (status != Status::Ok) return status;
    ENGINE_LOG_INFO(log_store, "schema migrated", log::field("path", path_),
                    log::field("from", current), log::field("to", migration.version));
    current = migration.version;
  }
  return Status::Ok;
}

Status Database::journal_mode(std::string& out) {
  out.clear();
  Statement stmt;
  const Status status = prepare("PRAGMA journal_mode", stmt);
  if (status != Status::Ok) return status;
  bool row = false;
  const Status step = stmt.step(row);
  if (step != Status::Ok) return step;
  if (!row) return Status::SqlError;
  out.assign(stmt.column_text(0));
  return Status::Ok;
}

Status Database::file_size_bytes(i64& out) {
  out = 0;
  Statement stmt;
  const Status status = prepare(
      "SELECT page_count * page_size FROM pragma_page_count(), "
      "pragma_page_size()",
      stmt);
  if (status != Status::Ok) return status;
  bool row = false;
  const Status step = stmt.step(row);
  if (step != Status::Ok) return step;
  if (!row) return Status::SqlError;
  out = stmt.column_i64(0);
  return Status::Ok;
}

i64 Database::last_insert_rowid() const noexcept {
  return db_ != nullptr ? sqlite3_last_insert_rowid(db_) : 0;
}

i64 Database::changes() const noexcept { return db_ != nullptr ? sqlite3_changes64(db_) : 0; }

}  // namespace engine::store
