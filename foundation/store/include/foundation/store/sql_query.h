#pragma once

// SQL a client wrote, run against the store's file without letting it reach past its own statement
// (docs/subsystems/store.md, "SQL a client wrote"; plan 03 §3.5, plan 06 §6.6's `query(sql)`).
//
// The text is not the engine's, so everything about the run is bounded and nothing is trusted:
//
//   - A connection of its own, opened read-only on the file (`SQLITE_OPEN_READONLY`, no URI), with
//     `query_only` on, never the world's connection; it is closed before the call returns, so no
//     cursor, statement or snapshot outlives the call.
//   - SQLite's hardening for untrusted SQL (sqlite.org/security.html): defensive mode, the schema
//     untrusted, triggers and views off, the double-quoted-string misfeature off, extensions off,
//     memory-mapped I/O off, and every limit below set on the connection before the text is seen.
//   - The text is prepared once and refused unless it is one statement (an empty tail) that SQLite
//     says writes nothing (`sqlite3_stmt_readonly`), and prepared with `SQLITE_PREPARE_NO_VTAB`, so
//     no virtual table (`pragma_*`, `json_each`, `dbstat`) is reachable at all.
//   - An authorizer, installed before the prepare, admits `SELECT`, recursive common table
//     expressions, reads of the columns of `events`, `projections` and `snapshots` in the main
//     database, and the functions of `k_sql_functions`; it denies everything else, so `ATTACH`,
//     `PRAGMA`, `sqlite_schema`, a temp table and `load_extension` fail at the prepare.
//   - Values are bound from `params` (`?1` ...), never spliced into the text.
//   - `max_ms` is a progress handler that interrupts the statement; `max_rows` and `max_bytes` are
//     counted as it steps, and the result names the bound that ended it.
//
// One thread at a time, like every `Database`; the function opens and closes its own connection,
// so it may run on any thread beside a writer of the same file.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/store/database.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::store {

// SQLite's five storage classes.
enum class SqlType : u8 { Null, Integer, Real, Text, Blob };
const char* sql_type_name(SqlType type) noexcept;

// One value, bound or returned. `bytes` holds a Text value's UTF-8 (which may hold a NUL: its
// length is the value's) or a Blob's bytes.
struct SqlValue {
  SqlType type = SqlType::Null;
  i64 integer = 0;
  f64 real = 0.0;
  std::string bytes;

  static SqlValue null() { return {}; }
  static SqlValue of(i64 v) {
    SqlValue out;
    out.type = SqlType::Integer;
    out.integer = v;
    return out;
  }
  static SqlValue of(f64 v) {
    SqlValue out;
    out.type = SqlType::Real;
    out.real = v;
    return out;
  }
  static SqlValue text(std::string_view v) {
    SqlValue out;
    out.type = SqlType::Text;
    out.bytes.assign(v);
    return out;
  }
  static SqlValue blob(std::span<const u8> v) {
    SqlValue out;
    out.type = SqlType::Blob;
    out.bytes.assign(reinterpret_cast<const char*>(v.data()), v.size());
    return out;
  }
};

// The connection's limits (`sqlite3_limit`), fixed: a client never chooses them. The values are the
// protocol page's design and, where it was silent, sqlite.org/security.html's high-security column
// or a value a query over the three tables can live with (docs/subsystems/store.md, "SQL a client
// wrote").
inline constexpr u32 k_sql_max_text_bytes = 64u * 1024u;     // SQLITE_LIMIT_SQL_LENGTH
inline constexpr u32 k_sql_max_value_bytes = 1024u * 1024u;  // SQLITE_LIMIT_LENGTH
inline constexpr u32 k_sql_max_columns = 64;                 // SQLITE_LIMIT_COLUMN
inline constexpr u32 k_sql_max_expr_depth = 100;             // SQLITE_LIMIT_EXPR_DEPTH
inline constexpr u32 k_sql_max_parser_depth = 100;           // SQLITE_LIMIT_PARSER_DEPTH
inline constexpr u32 k_sql_max_compound = 8;                 // SQLITE_LIMIT_COMPOUND_SELECT
inline constexpr u32 k_sql_max_program_ops = 25'000;         // SQLITE_LIMIT_VDBE_OP
inline constexpr u32 k_sql_max_function_args = 32;           // SQLITE_LIMIT_FUNCTION_ARG
inline constexpr u32 k_sql_max_like_pattern = 256;           // SQLITE_LIMIT_LIKE_PATTERN_LENGTH
inline constexpr u32 k_sql_max_params = 64;                  // SQLITE_LIMIT_VARIABLE_NUMBER
inline constexpr u32 k_sql_cache_kib = 8u * 1024u;           // the connection's page cache

// The functions the authorizer admits: SQLite's built-ins that read only their arguments (and, for
// `random`, `randomblob` and the date functions' 'now', the clock or a generator), never the
// connection, the file or the process. Everything else, `load_extension` and `fts3_tokenizer`
// among them, is denied by name.
extern const std::span<const std::string_view> k_sql_functions;

struct SqlQueryLimits {
  u32 max_rows = 1000;          // rows returned
  u32 max_bytes = 256u * 1024;  // bytes of values returned: Text and Blob their length, 8 a number
  u32 max_ms = 2000;            // wall milliseconds, prepare to the last step
};

struct SqlQueryResult {
  Vector<std::string> columns;
  Vector<SqlValue> cells;  // row-major, columns.size() a row
  u32 rows = 0;
  u32 bytes = 0;
  // The bytes of text and blob values copied out of the statement, returned or dropped. Never more
  // than `max_bytes`: a row is counted as its columns are read and refused at the column that would
  // pass the bound, before that column is copied, so one wide row is never built whole first.
  u64 copied_bytes = 0;
  // Every row the statement had was returned.
  bool complete = true;
  // Which bound ended it: "" (none), "max_rows", "max_bytes" or "max_ms".
  const char* bound = "";
  // How many SQLite virtual-machine instructions it ran, to the nearest progress-handler period.
  u64 instructions = 0;
  f64 elapsed_ms = 0.0;
};

// Runs `sql` with `params` against the store file at `native_path` under every rule above.
//   Ok               the statement ran to its end or to a bound (`out.complete`, `out.bound`)
//   NotFound         no file at the path
//   InvalidArgument  the text was refused (empty, too long, two statements, writes, not
//                    authorized, a parameter count it does not take) or a value passed the value
//                    limit; `message` says which and why, in SQLite's words where it has them
//   SqlError         the text did not compile or the statement failed for another reason
//   Busy, IoError, Corrupt   the file, as SQLite reports it
Status run_sql_query(std::string_view native_path, std::string_view sql,
                     std::span<const SqlValue> params, const SqlQueryLimits& limits,
                     SqlQueryResult& out, std::string& message);

// What the connection `run_sql_query` opens is set to, read back from SQLite: the proof that the
// hardening took, for the tests and for a reader of the page. -1 where SQLite would not say.
struct SqlConnectionReport {
  i64 read_only = -1;
  i64 defensive = -1;
  i64 trusted_schema = -1;
  i64 triggers = -1;
  i64 views = -1;
  i64 dqs_dml = -1;
  i64 dqs_ddl = -1;
  i64 load_extension = -1;
  i64 query_only = -1;
  i64 mmap_size = -1;
  i64 temp_store = -1;
  i64 max_value_bytes = -1;
  i64 max_text_bytes = -1;
  i64 max_columns = -1;
  i64 max_expr_depth = -1;
  i64 max_compound = -1;
  i64 max_attached = -1;
  i64 max_params = -1;
};
Status describe_sql_connection(std::string_view native_path, SqlConnectionReport& out,
                               std::string& message);

}  // namespace engine::store
