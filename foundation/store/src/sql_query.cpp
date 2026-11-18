#include <foundation/store/sql_query.h>

#include <sqlite3.h>

#include <chrono>
#include <cstring>

namespace engine::store {

namespace {

// Off the list although they read only their arguments, because one call is one opcode, which
// max_ms cannot stop, and their work is the product of two arguments' lengths rather than the sum
// (store.md, "max_ms is kept between opcodes"): `instr` and `replace` compare a needle at every
// position of a haystack, `trim`, `ltrim`, `rtrim` and `unhex` look each character of one argument
// up in the whole of another, and `json_patch` looks each key of one object up among all of the
// other's. Measured at the value limit with max_ms 1000: instr and replace 19.5 s, json_patch of
// two objects of 40,000 keys 67 s, unhex of 600,000 characters skipping a set of 300,001 889 s.
constexpr std::string_view k_function_names[] = {
    // Scalars over their arguments.
    "abs", "char", "coalesce", "concat", "concat_ws", "format", "glob", "hex", "if", "ifnull",
    "iif", "length", "like", "likelihood", "likely", "lower", "max", "min", "nullif",
    "octet_length", "printf", "quote", "random", "randomblob", "round", "sign", "substr",
    "substring", "typeof", "unicode", "unlikely", "upper", "zeroblob",
    // Aggregates.
    "avg", "count", "group_concat", "string_agg", "sum", "total",
    // Window functions.
    "row_number", "rank", "dense_rank", "percent_rank", "cume_dist", "ntile", "lag", "lead",
    "first_value", "last_value", "nth_value",
    // Dates (the clock is read only for 'now').
    "date", "time", "datetime", "julianday", "strftime", "unixepoch", "timediff",
    // JSON over a value (json_each and json_tree are virtual tables, which the prepare refuses).
    "json", "jsonb", "json_array", "json_array_length", "json_extract", "json_object", "json_quote",
    "json_type", "json_valid", "json_insert", "json_replace", "json_set", "json_remove",
    "json_group_array", "json_group_object", "json_error_position", "json_pretty", "->", "->>"};

constexpr std::string_view k_tables[] = {"events", "projections", "snapshots"};

// How often the progress handler looks at the clock, in virtual-machine instructions. A thousand
// is a few microseconds of work, so `max_ms` is honoured to well under a millisecond.
constexpr int k_progress_period = 1000;

bool equal_nocase(std::string_view a, const char* b) noexcept {
  const usize n = std::strlen(b);
  if (a.size() != n) return false;
  for (usize i = 0; i < n; ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

const char* action_name(int action) noexcept {
  switch (action) {
    case SQLITE_CREATE_INDEX: return "CREATE INDEX";
    case SQLITE_CREATE_TABLE: return "CREATE TABLE";
    case SQLITE_CREATE_TEMP_INDEX: return "CREATE TEMP INDEX";
    case SQLITE_CREATE_TEMP_TABLE: return "CREATE TEMP TABLE";
    case SQLITE_CREATE_TEMP_TRIGGER: return "CREATE TEMP TRIGGER";
    case SQLITE_CREATE_TEMP_VIEW: return "CREATE TEMP VIEW";
    case SQLITE_CREATE_TRIGGER: return "CREATE TRIGGER";
    case SQLITE_CREATE_VIEW: return "CREATE VIEW";
    case SQLITE_DELETE: return "DELETE";
    case SQLITE_DROP_INDEX: return "DROP INDEX";
    case SQLITE_DROP_TABLE: return "DROP TABLE";
    case SQLITE_DROP_TEMP_INDEX: return "DROP TEMP INDEX";
    case SQLITE_DROP_TEMP_TABLE: return "DROP TEMP TABLE";
    case SQLITE_DROP_TEMP_TRIGGER: return "DROP TEMP TRIGGER";
    case SQLITE_DROP_TEMP_VIEW: return "DROP TEMP VIEW";
    case SQLITE_DROP_TRIGGER: return "DROP TRIGGER";
    case SQLITE_DROP_VIEW: return "DROP VIEW";
    case SQLITE_INSERT: return "INSERT";
    case SQLITE_PRAGMA: return "PRAGMA";
    case SQLITE_TRANSACTION: return "a transaction";
    case SQLITE_UPDATE: return "UPDATE";
    case SQLITE_ATTACH: return "ATTACH";
    case SQLITE_DETACH: return "DETACH";
    case SQLITE_ALTER_TABLE: return "ALTER TABLE";
    case SQLITE_REINDEX: return "REINDEX";
    case SQLITE_ANALYZE: return "ANALYZE";
    case SQLITE_CREATE_VTABLE: return "CREATE VIRTUAL TABLE";
    case SQLITE_DROP_VTABLE: return "DROP VIRTUAL TABLE";
    case SQLITE_SAVEPOINT: return "a savepoint";
    case SQLITE_COPY: return "COPY";
    default: return "an operation";
  }
}

struct AuthState {
  // While the text after the first statement is prepared only to learn whether there is one,
  // everything is denied: a prepare is not always free of effects (a flag pragma such as
  // `query_only` or `writable_schema` takes effect as it is compiled, after its authorizer check),
  // so a second statement must fail to compile, and any statement there at all is the refusal.
  bool tail = false;
  std::string why;
  // Every table and view the file's schema names, read before the authorizer is installed.
  Vector<std::string> file_tables;
  // The first refusal is kept, except that the write to the schema table a CREATE or DROP makes
  // on its way gives way to the CREATE or DROP itself, which says more.
  bool weak = false;
  void refuse(std::string reason, bool is_weak) {
    if (!why.empty() && !(weak && !is_weak)) return;
    why = std::move(reason);
    weak = is_weak;
  }
};

int authorize(void* user, int action, const char* a1, const char* a2, const char* database,
              const char* /*inner*/) {
  AuthState& state = *static_cast<AuthState*>(user);
  if (state.tail) return SQLITE_DENY;
  switch (action) {
    case SQLITE_SELECT:
    case SQLITE_RECURSIVE: return SQLITE_OK;
    case SQLITE_READ: {
      const std::string_view table = a1 != nullptr ? a1 : "";
      const bool main = database != nullptr && std::strcmp(database, "main") == 0;
      if (main || database == nullptr) {
        for (const std::string_view known : k_tables)
          if (table == known) return SQLITE_OK;
      }
      // SQLite names no database for a read of a common table expression or a subquery, and for
      // the read a `count(*)` makes of a table it reads no column of. The first two are the
      // statement's own rows; the last is a table of the file, so a name the file's schema
      // holds (or any sqlite_ name) is refused and any other is the statement's own.
      if (database == nullptr && a1 != nullptr && !table.starts_with("sqlite_")) {
        bool real = false;
        for (const std::string& name : state.file_tables)
          if (equal_nocase(name, a1)) real = true;
        if (!real) return SQLITE_OK;
      }
      state.refuse("reads " + std::string(database != nullptr ? database : "main") + "." +
                       std::string(table) +
                       ", which is not one of the store's tables (events, projections, snapshots)",
                   false);
      return SQLITE_DENY;
    }
    case SQLITE_FUNCTION: {
      const char* name = a2 != nullptr ? a2 : "";
      for (const std::string_view known : k_function_names)
        if (equal_nocase(known, name)) return SQLITE_OK;
      state.refuse("calls " + std::string(name) +
                       "(), which is not on the list of functions a query may call",
                   false);
      return SQLITE_DENY;
    }
    default: {
      std::string reason = std::string("is ") + action_name(action);
      if (a1 != nullptr) reason += std::string(" (") + a1 + ")";
      reason += "; a query is one SELECT";
      const bool schema_write =
          (action == SQLITE_INSERT || action == SQLITE_UPDATE || action == SQLITE_DELETE) &&
          a1 != nullptr && std::strncmp(a1, "sqlite_", 7) == 0;
      state.refuse(std::move(reason), schema_write);
      return SQLITE_DENY;
    }
  }
}

using Clock = std::chrono::steady_clock;

struct Progress {
  Clock::time_point start;
  u32 max_ms = 0;
  u64 calls = 0;
  bool timed_out = false;
};

int on_progress(void* user) {
  Progress& p = *static_cast<Progress*>(user);
  ++p.calls;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - p.start);
  if (elapsed.count() >= static_cast<i64>(p.max_ms)) {
    p.timed_out = true;
    return 1;  // interrupts the statement: sqlite3_step returns SQLITE_INTERRUPT
  }
  return 0;
}

// The connection and the statement, closed when the call returns whatever path it takes.
struct Connection {
  sqlite3* db = nullptr;
  sqlite3_stmt* stmt = nullptr;
  Connection() = default;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() {
    if (stmt != nullptr) sqlite3_finalize(stmt);
    if (db != nullptr) sqlite3_close_v2(db);
  }
};

Status status_of(int rc) noexcept {
  switch (rc & 0xFF) {
    case SQLITE_OK: return Status::Ok;
    case SQLITE_CANTOPEN: return Status::NotFound;
    case SQLITE_BUSY:
    case SQLITE_LOCKED: return Status::Busy;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB: return Status::Corrupt;
    case SQLITE_IOERR: return Status::IoError;
    case SQLITE_READONLY: return Status::ReadOnly;
    case SQLITE_AUTH:
    case SQLITE_TOOBIG:
    case SQLITE_RANGE: return Status::InvalidArgument;
    default: return Status::SqlError;
  }
}

// Every setting before the client's text is seen (sqlite.org/security.html, "Untrusted SQL
// Inputs" and "Untrusted SQLite Database Files").
bool harden(sqlite3* db, u32 max_ms, std::string& message) {
  struct Flag {
    int op;
    int value;
  };
  const Flag flags[] = {
      {SQLITE_DBCONFIG_DEFENSIVE, 1},
      {SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0},
      {SQLITE_DBCONFIG_ENABLE_TRIGGER, 0},
      {SQLITE_DBCONFIG_ENABLE_VIEW, 0},
      {SQLITE_DBCONFIG_DQS_DML, 0},
      {SQLITE_DBCONFIG_DQS_DDL, 0},
      {SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0},
#if defined(SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE)
      {SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE, 0},
#endif
#if defined(SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE)
      {SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE, 0},
#endif
  };
  for (const Flag& f : flags) {
    int now = -1;
    if (sqlite3_db_config(db, f.op, f.value, &now) != SQLITE_OK || now != f.value) {
      message = "the store could not set its connection's setting " + std::to_string(f.op);
      return false;
    }
  }
  struct Limit {
    int id;
    u32 value;
  };
  const Limit limits[] = {
      {SQLITE_LIMIT_LENGTH, k_sql_max_value_bytes},
      {SQLITE_LIMIT_SQL_LENGTH, k_sql_max_text_bytes},
      {SQLITE_LIMIT_COLUMN, k_sql_max_columns},
      {SQLITE_LIMIT_EXPR_DEPTH, k_sql_max_expr_depth},
      {SQLITE_LIMIT_PARSER_DEPTH, k_sql_max_parser_depth},
      {SQLITE_LIMIT_COMPOUND_SELECT, k_sql_max_compound},
      {SQLITE_LIMIT_VDBE_OP, k_sql_max_program_ops},
      {SQLITE_LIMIT_FUNCTION_ARG, k_sql_max_function_args},
      {SQLITE_LIMIT_ATTACHED, 0},
      {SQLITE_LIMIT_LIKE_PATTERN_LENGTH, k_sql_max_like_pattern},
      {SQLITE_LIMIT_VARIABLE_NUMBER, k_sql_max_params},
      {SQLITE_LIMIT_TRIGGER_DEPTH, 1},
      {SQLITE_LIMIT_WORKER_THREADS, 0},
  };
  for (const Limit& l : limits) {
    sqlite3_limit(db, l.id, static_cast<int>(l.value));
    if (sqlite3_limit(db, l.id, -1) != static_cast<int>(l.value)) {
      message = "the store could not set its connection's limit " + std::to_string(l.id);
      return false;
    }
  }
  sqlite3_busy_timeout(db, static_cast<int>(max_ms));
  // query_only refuses any write the connection might still make; mmap off is the page's advice
  // for a file read on behalf of someone else; sorts and DISTINCT tables spill to temporary files
  // past the page cache rather than grow the heap without bound.
  char pragmas[160];
  sqlite3_snprintf(sizeof(pragmas), pragmas,
                   "PRAGMA query_only=1; PRAGMA mmap_size=0; PRAGMA temp_store=FILE; "
                   "PRAGMA trusted_schema=0; PRAGMA cache_size=-%u;",
                   k_sql_cache_kib);
  char* error = nullptr;
  if (sqlite3_exec(db, pragmas, nullptr, nullptr, &error) != SQLITE_OK) {
    message = std::string("the store could not configure its connection: ") +
              (error != nullptr ? error : "");
    sqlite3_free(error);
    return false;
  }
  return true;
}

// Opens `path` read-only and hardens the connection; the caller's `Connection` closes it.
Status open_hardened(std::string_view native_path, u32 max_ms, Connection& c,
                     std::string& message) {
  const std::string path(native_path);
  // No SQLITE_OPEN_URI: the path is a file name, never a URI with options in it.
  const int rc =
      sqlite3_open_v2(path.c_str(), &c.db,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE, nullptr);
  if (rc != SQLITE_OK) {
    message = std::string("cannot open the store read-only: ") +
              (c.db != nullptr ? sqlite3_errmsg(c.db) : sqlite3_errstr(rc));
    return status_of(rc);
  }
  if (!harden(c.db, max_ms, message)) return Status::SqlError;
  return Status::Ok;
}

i64 pragma_value(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  i64 value = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW)
    value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

}  // namespace

static constexpr std::span<const std::string_view> k_function_span{k_function_names};
const std::span<const std::string_view> k_sql_functions = k_function_span;

const char* sql_type_name(SqlType type) noexcept {
  switch (type) {
    case SqlType::Null: return "null";
    case SqlType::Integer: return "integer";
    case SqlType::Real: return "real";
    case SqlType::Text: return "text";
    case SqlType::Blob: return "blob";
  }
  return "?";
}

Status run_sql_query(std::string_view native_path, std::string_view sql,
                     std::span<const SqlValue> params, const SqlQueryLimits& limits,
                     SqlQueryResult& out, std::string& message) {
  out = SqlQueryResult{};
  message.clear();
  if (limits.max_rows == 0 || limits.max_bytes == 0 || limits.max_ms == 0) {
    message = "max_rows, max_bytes and max_ms are at least 1";
    return Status::InvalidArgument;
  }
  if (sql.size() > k_sql_max_text_bytes) {
    message = "the statement is " + std::to_string(sql.size()) + " bytes; at most " +
              std::to_string(k_sql_max_text_bytes);
    return Status::InvalidArgument;
  }
  if (sql.find('\0') != std::string_view::npos) {
    message = "the statement's text holds a NUL byte; pass a value with a NUL as a parameter";
    return Status::InvalidArgument;
  }
  if (params.size() > k_sql_max_params) {
    message = "at most " + std::to_string(k_sql_max_params) + " parameters";
    return Status::InvalidArgument;
  }

  Progress progress;
  progress.start = Clock::now();
  progress.max_ms = limits.max_ms;
  Connection c;
  const Status opened = open_hardened(native_path, limits.max_ms, c, message);
  if (opened != Status::Ok) return opened;

  AuthState auth;
  {
    sqlite3_stmt* names = nullptr;
    if (sqlite3_prepare_v2(c.db, "SELECT name FROM sqlite_schema WHERE type IN ('table', 'view')",
                           -1, &names, nullptr) != SQLITE_OK) {
      message = std::string("cannot read the store's schema: ") + sqlite3_errmsg(c.db);
      return status_of(sqlite3_errcode(c.db));
    }
    while (sqlite3_step(names) == SQLITE_ROW) {
      const unsigned char* name = sqlite3_column_text(names, 0);
      if (name != nullptr) auth.file_tables.push_back(reinterpret_cast<const char*>(name));
    }
    sqlite3_finalize(names);
  }
  sqlite3_set_authorizer(c.db, &authorize, &auth);
  sqlite3_progress_handler(c.db, k_progress_period, &on_progress, &progress);

  const char* begin = sql.data();
  const char* end = sql.data() + sql.size();
  const char* tail = nullptr;
  int rc = sqlite3_prepare_v3(c.db, begin, static_cast<int>(sql.size()), SQLITE_PREPARE_NO_VTAB,
                              &c.stmt, &tail);
  if (rc != SQLITE_OK) {
    // A denied function is SQLITE_ERROR, a denied action SQLITE_AUTH; either way the authorizer
    // said why.
    if (!auth.why.empty() || (rc & 0xFF) == SQLITE_AUTH) {
      message = "refused: the statement " +
                (auth.why.empty() ? std::string("is not authorized") : auth.why);
      return Status::InvalidArgument;
    }
    message = std::string("the statement did not compile: ") + sqlite3_errmsg(c.db);
    return (rc & 0xFF) == SQLITE_ERROR ? Status::SqlError : status_of(rc);
  }
  if (c.stmt == nullptr) {
    message = "the text holds no statement";
    return Status::InvalidArgument;
  }
  // One statement: whatever follows it must prepare to nothing (whitespace and comments).
  if (tail != nullptr && tail < end) {
    auth.tail = true;
    sqlite3_stmt* second = nullptr;
    const char* rest = nullptr;
    const int rc2 = sqlite3_prepare_v3(c.db, tail, static_cast<int>(end - tail),
                                       SQLITE_PREPARE_NO_VTAB, &second, &rest);
    auth.tail = false;
    const bool another = rc2 != SQLITE_OK || second != nullptr;
    if (second != nullptr) sqlite3_finalize(second);
    if (another) {
      message = "refused: the text holds more than one statement; a query is one SELECT";
      return Status::InvalidArgument;
    }
  }
  if (sqlite3_stmt_readonly(c.stmt) == 0) {
    message = "refused: the statement writes; a query is one SELECT";
    return Status::InvalidArgument;
  }

  const int wanted = sqlite3_bind_parameter_count(c.stmt);
  if (wanted != static_cast<int>(params.size())) {
    message = "the statement takes " + std::to_string(wanted) + " parameters; " +
              std::to_string(params.size()) + " were given";
    return Status::InvalidArgument;
  }
  for (usize i = 0; i < params.size(); ++i) {
    const SqlValue& v = params[i];
    const int index = static_cast<int>(i) + 1;
    switch (v.type) {
      case SqlType::Null: rc = sqlite3_bind_null(c.stmt, index); break;
      case SqlType::Integer: rc = sqlite3_bind_int64(c.stmt, index, v.integer); break;
      case SqlType::Real: rc = sqlite3_bind_double(c.stmt, index, v.real); break;
      case SqlType::Text:
        rc = sqlite3_bind_text64(c.stmt, index, v.bytes.data(), v.bytes.size(), SQLITE_TRANSIENT,
                                 SQLITE_UTF8);
        break;
      case SqlType::Blob:
        rc = sqlite3_bind_blob64(c.stmt, index, v.bytes.data(), v.bytes.size(), SQLITE_TRANSIENT);
        break;
    }
    if (rc != SQLITE_OK) {
      message = "parameter " + std::to_string(index) + " was refused: " + sqlite3_errstr(rc);
      if ((rc & 0xFF) == SQLITE_TOOBIG)
        message += " (a value is at most " + std::to_string(k_sql_max_value_bytes) + " bytes)";
      return status_of(rc);
    }
  }

  const int columns = sqlite3_column_count(c.stmt);
  for (int i = 0; i < columns; ++i) {
    const char* name = sqlite3_column_name(c.stmt, i);
    out.columns.push_back(name != nullptr ? std::string(name) : std::string());
  }

  Status result = Status::Ok;
  for (;;) {
    rc = sqlite3_step(c.stmt);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      if (progress.timed_out && (rc & 0xFF) == SQLITE_INTERRUPT) {
        out.complete = false;
        out.bound = "max_ms";
      } else if ((rc & 0xFF) == SQLITE_TOOBIG) {
        message = "a value grew past the value limit of " + std::to_string(k_sql_max_value_bytes) +
                  " bytes: " + sqlite3_errmsg(c.db);
        result = Status::InvalidArgument;
      } else {
        message = std::string("the statement failed: ") + sqlite3_errmsg(c.db);
        result = status_of(rc) == Status::Ok ? Status::SqlError : status_of(rc);
        if (result == Status::InvalidArgument) result = Status::SqlError;
      }
      break;
    }
    if (out.rows >= limits.max_rows) {
      out.complete = false;
      out.bound = "max_rows";
      break;
    }
    // The row is counted against max_bytes as its columns are read, and refused at the column
    // that passes it before that column is copied, so a row of 64 values of 1 MiB under a bound of
    // 1 KiB copies at most the 1 KiB. A value's size is known before its bytes are asked for: the
    // column's own storage class is read, so `sqlite3_column_bytes` converts nothing, and a
    // zeroblob is not even expanded.
    const usize first = out.cells.size();
    u64 row_bytes = 0;
    bool over = false;
    for (int i = 0; i < columns; ++i) {
      SqlValue v;
      const int type = sqlite3_column_type(c.stmt, i);
      const u64 size = type == SQLITE_TEXT || type == SQLITE_BLOB
                           ? static_cast<u64>(sqlite3_column_bytes(c.stmt, i))
                       : type == SQLITE_NULL ? 0u
                                             : 8u;
      if (static_cast<u64>(out.bytes) + row_bytes + size > limits.max_bytes) {
        over = true;
        break;
      }
      switch (type) {
        case SQLITE_INTEGER:
          v.type = SqlType::Integer;
          v.integer = sqlite3_column_int64(c.stmt, i);
          break;
        case SQLITE_FLOAT:
          v.type = SqlType::Real;
          v.real = sqlite3_column_double(c.stmt, i);
          break;
        case SQLITE_TEXT: {
          v.type = SqlType::Text;
          const unsigned char* text = sqlite3_column_text(c.stmt, i);
          if (text != nullptr)
            v.bytes.assign(reinterpret_cast<const char*>(text), static_cast<usize>(size));
          break;
        }
        case SQLITE_BLOB: {
          v.type = SqlType::Blob;
          const void* blob = sqlite3_column_blob(c.stmt, i);
          if (blob != nullptr)
            v.bytes.assign(static_cast<const char*>(blob), static_cast<usize>(size));
          break;
        }
        default: break;
      }
      row_bytes += size;
      out.copied_bytes += v.bytes.size();
      out.cells.push_back(std::move(v));
    }
    if (over) {
      out.cells.resize(static_cast<u32>(first));
      out.complete = false;
      out.bound = "max_bytes";
      break;
    }
    out.bytes += static_cast<u32>(row_bytes);  // at most max_bytes, a u32
    ++out.rows;
  }
  out.instructions = progress.calls * static_cast<u64>(k_progress_period);
  out.elapsed_ms = std::chrono::duration<f64, std::milli>(Clock::now() - progress.start).count();
  return result;
}

Status describe_sql_connection(std::string_view native_path, SqlConnectionReport& out,
                               std::string& message) {
  out = SqlConnectionReport{};
  message.clear();
  Connection c;
  const Status opened = open_hardened(native_path, 1000, c, message);
  if (opened != Status::Ok) return opened;
  auto flag = [&](int op) {
    int value = -1;
    sqlite3_db_config(c.db, op, -1, &value);
    return static_cast<i64>(value);
  };
  out.read_only = sqlite3_db_readonly(c.db, "main");
  out.defensive = flag(SQLITE_DBCONFIG_DEFENSIVE);
  out.trusted_schema = flag(SQLITE_DBCONFIG_TRUSTED_SCHEMA);
  out.triggers = flag(SQLITE_DBCONFIG_ENABLE_TRIGGER);
  out.views = flag(SQLITE_DBCONFIG_ENABLE_VIEW);
  out.dqs_dml = flag(SQLITE_DBCONFIG_DQS_DML);
  out.dqs_ddl = flag(SQLITE_DBCONFIG_DQS_DDL);
  out.load_extension = flag(SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION);
  out.query_only = pragma_value(c.db, "PRAGMA query_only");
  out.mmap_size = pragma_value(c.db, "PRAGMA mmap_size");
  out.temp_store = pragma_value(c.db, "PRAGMA temp_store");
  out.max_value_bytes = sqlite3_limit(c.db, SQLITE_LIMIT_LENGTH, -1);
  out.max_text_bytes = sqlite3_limit(c.db, SQLITE_LIMIT_SQL_LENGTH, -1);
  out.max_columns = sqlite3_limit(c.db, SQLITE_LIMIT_COLUMN, -1);
  out.max_expr_depth = sqlite3_limit(c.db, SQLITE_LIMIT_EXPR_DEPTH, -1);
  out.max_compound = sqlite3_limit(c.db, SQLITE_LIMIT_COMPOUND_SELECT, -1);
  out.max_attached = sqlite3_limit(c.db, SQLITE_LIMIT_ATTACHED, -1);
  out.max_params = sqlite3_limit(c.db, SQLITE_LIMIT_VARIABLE_NUMBER, -1);
  return Status::Ok;
}

}  // namespace engine::store
