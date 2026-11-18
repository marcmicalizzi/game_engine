// Plan 06 §6.6's `query(structured)` over the document (docs/subsystems/protocol.md, "Querying the
// document"): the live records that pass a filter a client writes, in id order, a page at a time.
// The filter is input from outside, so it is data this file walks and never text it runs: it is
// compiled once into a tree of terms, bounded in depth and size, and every page is bounded in the
// rows it returns, the candidates it examines, the milliseconds it spends and the bytes it builds,
// with the bound that ended it and where the next page starts. The candidates come from the
// document's indices (`children`, `ids_in_tile`, `objects`), never a scan of the layers.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/document.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>

#include <algorithm>
#include <string>

namespace engine::protocol {

namespace {

constexpr u32 k_max_depth = 32;
constexpr u32 k_max_terms = 1024;
constexpr u32 k_max_rows = 10000;
constexpr u32 k_max_examined = 1000000;
constexpr u32 k_max_ms = 10000;
constexpr u32 k_min_bytes = 1024;
constexpr u32 k_max_bytes = 4u << 20;
constexpr u64 k_max_region_tiles = 65536;
// The clock is read once per this many candidates, not per candidate.
constexpr u32 k_clock_every = 256;

enum class TermKind : u8 { All, Any, Not, Type, Property, Parent };
enum class Test : u8 { Equals, AtLeast, AtMost, Prefix, Exists };

struct Term {
  TermKind kind = TermKind::All;
  Test test = Test::Equals;
  std::string text;  // a type pattern, a property name, or a prefix
  i64 element = -1;  // `name[i]`
  std::string prefix;
  JsonValue value;
  f64 number = 0;
  bool exists = true;
  Id128 id;
  Vector<u32> kids;
};

struct Filter {
  Vector<Term> terms;  // terms[0] is the root when there is one
};

bool fail_at(const std::string& path, const std::string& why, RpcError& error) {
  error = make_error(codes::k_invalid_argument, "filter" + path + ": " + why);
  return false;
}

// `name` or `name[i]`.
bool split_element(std::string_view name, std::string& base, i64& element) {
  element = -1;
  base = std::string(name);
  if (name.empty()) return false;
  if (name.back() != ']') return true;
  const usize open = name.rfind('[');
  if (open == std::string_view::npos || open == 0 || open + 2 == name.size()) return false;
  i64 n = 0;
  for (usize i = open + 1; i + 1 < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9' || n > 100000000) return false;
    n = n * 10 + (name[i] - '0');
  }
  base = std::string(name.substr(0, open));
  element = n;
  return true;
}

bool compile(const JsonValue& v, const std::string& path, u32 depth, Filter& f, u32& index,
             RpcError& error) {
  if (depth > k_max_depth) return fail_at(path, "nested deeper than 32", error);
  if (f.terms.size() >= k_max_terms) return fail_at(path, "more than 1,024 terms", error);
  if (!v.is_object()) return fail_at(path, "a term is an object", error);
  index = f.terms.size();
  f.terms.push_back(Term{});
  const auto set_kind = [&](TermKind k) { f.terms[index].kind = k; };
  for (const char* list : {"all", "any"}) {
    const JsonValue* items = v.find(list);
    if (items == nullptr) continue;
    if (v.size() != 1)
      return fail_at(path, std::string("\"") + list + "\" takes no other key", error);
    if (!items->is_array()) return fail_at(path + "." + list, "a list of terms", error);
    set_kind(list[1] == 'l' ? TermKind::All : TermKind::Any);
    for (u32 i = 0; i < items->size(); ++i) {
      u32 kid = 0;
      if (!compile((*items)[i], path + "." + list + "[" + std::to_string(i) + "]", depth + 1, f,
                   kid, error))
        return false;
      f.terms[index].kids.push_back(kid);
    }
    return true;
  }
  if (const JsonValue* inner = v.find("not"); inner != nullptr) {
    if (v.size() != 1) return fail_at(path, "\"not\" takes no other key", error);
    set_kind(TermKind::Not);
    u32 kid = 0;
    if (!compile(*inner, path + ".not", depth + 1, f, kid, error)) return false;
    f.terms[index].kids.push_back(kid);
    return true;
  }
  if (const JsonValue* t = v.find("type"); t != nullptr) {
    if (v.size() != 1 || !t->is_string())
      return fail_at(path, "\"type\" is a pattern string and takes no other key", error);
    set_kind(TermKind::Type);
    f.terms[index].text = std::string(t->as_string());
    return true;
  }
  if (const JsonValue* p = v.find("parent"); p != nullptr) {
    Id128 id;
    if (v.size() != 1 || !p->is_string() || !Id128::from_hex(p->as_string(), id))
      return fail_at(path, "\"parent\" is an id of 32 hex digits and takes no other key", error);
    set_kind(TermKind::Parent);
    f.terms[index].id = id;
    return true;
  }
  const JsonValue* name = v.find("property");
  if (name == nullptr || !name->is_string())
    return fail_at(path, "a term is all, any, not, type, parent or property", error);
  Term& term = f.terms[index];
  term.kind = TermKind::Property;
  if (!split_element(name->as_string(), term.text, term.element))
    return fail_at(path + ".property", "a name, or a name and [index]", error);
  if (v.size() != 2) return fail_at(path, "a property term takes exactly one test", error);
  if (const JsonValue* e = v.find("equals"); e != nullptr) {
    term.test = Test::Equals;
    term.value = *e;
  } else if (const JsonValue* lo = v.find("at_least"); lo != nullptr) {
    term.test = Test::AtLeast;
    if (!lo->get_f64(term.number)) return fail_at(path + ".at_least", "a number", error);
  } else if (const JsonValue* hi = v.find("at_most"); hi != nullptr) {
    term.test = Test::AtMost;
    if (!hi->get_f64(term.number)) return fail_at(path + ".at_most", "a number", error);
  } else if (const JsonValue* pre = v.find("prefix"); pre != nullptr) {
    term.test = Test::Prefix;
    if (!pre->is_string()) return fail_at(path + ".prefix", "a string", error);
    term.prefix = std::string(pre->as_string());
  } else if (const JsonValue* ex = v.find("exists"); ex != nullptr) {
    term.test = Test::Exists;
    if (!ex->get_bool(term.exists)) return fail_at(path + ".exists", "true or false", error);
  } else {
    return fail_at(path, "a property term's test is equals, at_least, at_most, prefix or exists",
                   error);
  }
  return true;
}

bool same_json(const JsonValue& a, const JsonValue& b) {
  f64 x = 0, y = 0;
  if (a.is_number() && b.is_number() && a.get_f64(x) && b.get_f64(y)) return x == y;
  return a == b;
}

bool eval(const Filter& f, u32 index, const doc::Document& d, Id128 id) {
  const Term& t = f.terms[index];
  switch (t.kind) {
    case TermKind::All:
      for (const u32 k : t.kids)
        if (!eval(f, k, d, id)) return false;
      return true;
    case TermKind::Any:
      for (const u32 k : t.kids)
        if (eval(f, k, d, id)) return true;
      return false;
    case TermKind::Not: return !eval(f, t.kids[0], d, id);
    case TermKind::Type: return glob_match(t.text, d.type_of(id));
    case TermKind::Parent: return d.parent_of(id) == t.id;
    case TermKind::Property: break;
  }
  const JsonValue* v = d.property(id, t.text);
  if (v != nullptr && t.element >= 0)
    v = v->is_array() && static_cast<u64>(t.element) < v->size()
            ? &(*v)[static_cast<u32>(t.element)]
            : nullptr;
  if (t.test == Test::Exists) return (v != nullptr && !v->is_null()) == t.exists;
  if (v == nullptr) return false;
  f64 x = 0;
  switch (t.test) {
    case Test::Equals: return same_json(*v, t.value);
    case Test::AtLeast: return v->get_f64(x) && x >= t.number;
    case Test::AtMost: return v->get_f64(x) && x <= t.number;
    case Test::Prefix:
      return v->is_string() && v->as_string().substr(0, t.prefix.size()) == t.prefix;
    case Test::Exists: break;
  }
  return false;
}

template <class T>
bool in_range(T v, T lo, T hi) {
  return v >= lo && v <= hi;
}

bool doc_query(Context& ctx, const QueryParams& params, QueryResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  const auto bad = [&](const std::string& why) {
    error = make_error(codes::k_invalid_argument, why);
    return false;
  };
  if (!in_range(params.max_rows, 1u, k_max_rows)) return bad("max_rows must be 1 to 10,000");
  if (!in_range(params.max_examined, 1u, k_max_examined))
    return bad("max_examined must be 1 to 1,000,000");
  if (!in_range(params.max_ms, 1u, k_max_ms)) return bad("max_ms must be 1 to 10,000");
  if (!in_range(params.max_bytes, k_min_bytes, k_max_bytes))
    return bad("max_bytes must be 1,024 to 4,194,304");
  if (params.parent.has_value() && params.region.has_value())
    return bad("query narrows by parent or by region, not both");
  Filter filter;
  u32 root = 0;
  const bool filtered = !params.filter.is_null();
  if (filtered && !compile(params.filter, "", 1, filter, root, error)) return false;
  const doc::Document& d = s->document();

  // The candidates, in id order, through the document's indices.
  Vector<Id128> candidates;
  if (params.parent.has_value()) {
    candidates = d.children(*params.parent);
  } else if (params.region.has_value()) {
    const TileRange& r = *params.region;
    if (r.x1 < r.x0 || r.y1 < r.y0) return bad("region must have x0 <= x1 and y0 <= y1");
    const u64 tiles = static_cast<u64>(static_cast<i64>(r.x1) - r.x0 + 1) *
                      static_cast<u64>(static_cast<i64>(r.y1) - r.y0 + 1);
    if (tiles > k_max_region_tiles) return bad("region covers more than 65,536 tiles");
    if (tiles <= d.occupied_tiles()) {
      for (i32 y = r.y0; y <= r.y1; ++y)
        for (i32 x = r.x0; x <= r.x1; ++x)
          for (const Id128 id : d.ids_in_tile({x, y}))
            candidates.push_back(id);
      std::sort(candidates.begin(), candidates.end());
    } else {
      for (const Id128 id : d.objects()) {
        doc::TileCoord t;
        if (d.object_tile(id, t) && in_range(t.x, r.x0, r.x1) && in_range(t.y, r.y0, r.y1))
          candidates.push_back(id);
      }
    }
  } else {
    candidates = d.objects();
  }
  auto first = candidates.begin();
  if (!params.after.is_null())
    first = std::upper_bound(candidates.begin(), candidates.end(), params.after);
  out.candidates = static_cast<u32>(candidates.end() - first);

  const i64 deadline = time::monotonic_ns() + static_cast<i64>(params.max_ms) * 1000000;
  Id128 last;
  for (auto it = first; it != candidates.end(); ++it) {
    if (out.examined >= params.max_examined) {
      out.bound = "max_examined";
      break;
    }
    if (out.examined % k_clock_every == 0 && out.examined > 0 && time::monotonic_ns() > deadline) {
      out.bound = "max_ms";
      break;
    }
    const Id128 id = *it;
    if (filtered && !eval(filter, root, d, id)) {
      ++out.examined;
      last = id;
      continue;
    }
    if (out.rows.size() >= params.max_rows) {
      out.bound = "max_rows";
      break;
    }
    QueryRow row;
    row.id = id;
    row.type = std::string(d.type_of(id));
    row.parent = d.parent_of(id);
    for (const std::string& field : params.fields)
      if (const JsonValue* v = d.property(id, field); v != nullptr)
        row.fields.insert_or_assign(field, *v);
    const u32 bytes = static_cast<u32>(write_json(schema::to_json(row)).size());
    if (out.bytes + bytes > params.max_bytes) {
      // A row that alone passes the bound would stop every page here: it is the caller's to
      // raise max_bytes or ask for fewer fields.
      out.bound = "max_bytes";
      break;
    }
    ++out.examined;
    last = id;
    out.bytes += bytes;
    out.rows.push_back(std::move(row));
  }
  out.complete = out.bound.empty();
  out.next = out.complete ? Id128{} : last.is_null() ? params.after : last;
  return true;
}

}  // namespace

void add_query_methods(Dispatcher& d) {
  d.add(read_only(method<QueryParams, QueryResult, &doc_query>(
      "doc.query",
      "The live records that pass a structured filter (type patterns, property tests, parent; "
      "all, any, not), narrowed by parent or region through the document's indices, in id order "
      "a page at a time: bounded by max_rows, max_examined, max_ms and max_bytes, with the bound "
      "that ended the page and the id to resume after.")));
}

}  // namespace engine::protocol
