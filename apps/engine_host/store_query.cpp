// store.query: SQL a client wrote, over the session's document's store (docs/subsystems/
// protocol.md, "Querying the document"; docs/subsystems/store.md, "SQL a client wrote").
//
// The store capability's half of plan 06 §6.6's `query(sql | structured)`, beside
// `session.events`; `doc.query` is the structured half over the document. Everything that keeps
// the text inside its own statement is `store::run_sql_query`'s: this file turns the protocol's
// JSON into bound values and the rows back into JSON, and finds the store, which is always the
// session's own `world.db` — the method takes no path from a client.

#include "ops_methods.h"

#include <core/hash/base64.h>
#include <core/json/json_value.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <string>
#include <utility>

#if defined(ENGINE_HOST_STORE)
#include <foundation/store/sql_query.h>
#endif

namespace engine::host {

namespace {

constexpr u32 k_max_rows = 10'000;
constexpr u32 k_max_ms = 10'000;
constexpr u32 k_min_bytes = 1024;
constexpr u32 k_max_bytes = 4u * 1024u * 1024u;

protocol::RpcError invalid(std::string message) {
  return protocol::make_error(protocol::codes::k_invalid_argument, std::move(message));
}

#if defined(ENGINE_HOST_STORE)
bool to_value(const JsonValue& json, u32 index, store::SqlValue& out, protocol::RpcError& error) {
  const std::string where = "params[" + std::to_string(index) + "]";
  if (json.is_null()) {
    out = store::SqlValue::null();
  } else if (json.is_bool()) {
    out = store::SqlValue::of(static_cast<i64>(json.as_bool() ? 1 : 0));
  } else if (json.is_int() || json.is_uint()) {
    i64 v = 0;
    if (!json.get_i64(v)) {
      error = invalid(where + " is an integer past 64 bits");
      return false;
    }
    out = store::SqlValue::of(v);
  } else if (json.is_float()) {
    out = store::SqlValue::of(json.as_float());
  } else if (json.is_string()) {
    out = store::SqlValue::text(json.as_string());
  } else if (json.is_object()) {
    const JsonValue* blob = json.find("blob");
    std::string bytes;
    if (blob == nullptr || !blob->is_string() || json.size() != 1 ||
        !base64_decode(blob->as_string(), bytes)) {
      error = invalid(where +
                      " is an object; the one object a parameter may be is "
                      "{\"blob\": \"<base64>\"}");
      return false;
    }
    out.type = store::SqlType::Blob;
    out.bytes = std::move(bytes);
  } else {
    error = invalid(where +
                    " is an array; a parameter is null, a boolean, a number, a string or "
                    "{\"blob\": \"<base64>\"}");
    return false;
  }
  return true;
}

JsonValue to_json(const store::SqlValue& v) {
  switch (v.type) {
    case store::SqlType::Null: return JsonValue::null();
    case store::SqlType::Integer: return JsonValue(v.integer);
    case store::SqlType::Real: {
      if (std::isfinite(v.real)) return JsonValue(v.real);
      JsonValue out = JsonValue::object();
      out.set("real", JsonValue(v.real > 0 ? "inf" : "-inf"));
      return out;
    }
    case store::SqlType::Text: return JsonValue(std::string_view(v.bytes));
    case store::SqlType::Blob: {
      JsonValue out = JsonValue::object();
      out.set("blob", JsonValue(std::string_view(base64_encode(std::string_view(v.bytes)))));
      return out;
    }
  }
  return JsonValue::null();
}
#endif

bool store_query(protocol::Context& ctx, const protocol::StoreQueryParams& params,
                 protocol::StoreQueryResult& out, protocol::RpcError& error) {
  protocol::Session* session = ctx.sessions->require(params.session, error);
  if (session == nullptr) return false;
  if (params.max_rows == 0 || params.max_rows > k_max_rows) {
    error = invalid("max_rows is 1..10000");
    return false;
  }
  if (params.max_ms == 0 || params.max_ms > k_max_ms) {
    error = invalid("max_ms is 1..10000");
    return false;
  }
  if (params.max_bytes < k_min_bytes || params.max_bytes > k_max_bytes) {
    error = invalid("max_bytes is 1024..4194304");
    return false;
  }
  if (!params.params.is_null() && !params.params.is_array()) {
    error = invalid("params is null or an array of values for ?1, ?2, ...");
    return false;
  }
#if defined(ENGINE_HOST_STORE)
  Vector<store::SqlValue> values;
  if (params.params.is_array()) {
    const JsonValue::Array& list = params.params.as_array();
    if (list.size() > store::k_sql_max_params) {
      error = invalid("params holds " + std::to_string(list.size()) + " values; at most " +
                      std::to_string(store::k_sql_max_params));
      return false;
    }
    for (u32 i = 0; i < list.size(); ++i) {
      store::SqlValue v;
      if (!to_value(list[i], i, v, error)) return false;
      values.push_back(std::move(v));
    }
  }
  // The session's own store, `session.events`' convention: `world.db` in the document's
  // directory. Never a path a client names.
  std::string native;
  const std::string wanted = io::join_path(session->dir(), "world.db");
  const io::Status resolved = ctx.sessions->vfs().resolve(wanted, native);
  if (resolved != io::Status::Ok || !io::exists(native)) {
    error = protocol::make_error(
        protocol::codes::k_not_found,
        "the session's document has no store (world.db in its directory): a world writes one "
        "when it runs with the store capability");
    return false;
  }
  store::SqlQueryLimits limits;
  limits.max_rows = params.max_rows;
  limits.max_ms = params.max_ms;
  limits.max_bytes = params.max_bytes;
  store::SqlQueryResult result;
  std::string message;
  const store::Status status = store::run_sql_query(
      native, params.sql, std::span<const store::SqlValue>(values.data(), values.size()), limits,
      result, message);
  if (status != store::Status::Ok) {
    const bool client =
        status == store::Status::InvalidArgument || status == store::Status::SqlError;
    error = protocol::make_error(
        client ? protocol::codes::k_invalid_argument : protocol::codes::k_io_error,
        message.empty() ? std::string(store::status_name(status)) : message);
    return false;
  }
  out.columns.clear();
  for (const std::string& column : result.columns)
    out.columns.push_back(column);
  JsonValue rows = JsonValue::array();
  const u32 width = result.columns.size();
  for (u32 r = 0; r < result.rows; ++r) {
    JsonValue row = JsonValue::array();
    for (u32 c = 0; c < width; ++c)
      row.push_back(to_json(result.cells[r * width + c]));
    rows.push_back(std::move(row));
  }
  out.rows = std::move(rows);
  out.complete = result.complete;
  out.bound = result.bound;
  out.bytes = result.bytes;
  out.elapsed_ms = result.elapsed_ms;
  out.instructions = result.instructions;
  return true;
#else
  (void)out;
  error = protocol::make_error(
      protocol::codes::k_unavailable,
      "this engine-host was built without the store capability (ENGINE_WITH_STORE=OFF or "
      "ENGINE_MINIMAL=ON), so it has no store to query; doc.query reads the document");
  return false;
#endif
}

}  // namespace

void add_store_query_method(protocol::Dispatcher& d) {
  // Read-only by AGENTS.md's rule: it changes no document, lease or host setting, and its
  // connection to the store is opened read-only with query_only on.
  d.add(protocol::read_only(
      protocol::method<protocol::StoreQueryParams, protocol::StoreQueryResult, &store_query>(
          "store.query",
          "One SELECT over the session's document's store (world.db: events, projections, "
          "snapshots), on a read-only connection of its own: one statement that writes nothing, "
          "the three tables' columns and a list of pure functions, values bound from params, "
          "bounded by max_rows, max_bytes and max_ms, the bound that ended it named.")));
}

}  // namespace engine::host
