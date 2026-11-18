#pragma once

// The canonical writer's spellings, over any sink (roadmap D40).
//
// `write_json` writes through these with a string as its sink, and so does a writer that walks
// something other than a JsonValue — a schema type's fields (`schema::write_json_text`), or a sink
// that compares what would be written against bytes already in hand
// (`schema::canonical_json_equals`) — so there is one spelling of every value, whichever walk
// produced it. A sink is any type with `append(const char*, usize)` and `put(char)`.

#include <core/base/types.h>
#include <core/json/json_value.h>

#include <charconv>
#include <cmath>
#include <string>
#include <string_view>

namespace engine::json_format {

// A sink appending to a string.
struct StringSink {
  std::string& out;
  void append(const char* p, usize n) { out.append(p, n); }
  void put(char c) { out.push_back(c); }
};

template <class Sink>
void indent(Sink& out, u32 depth) {
  out.put('\n');
  for (u32 i = 0; i < depth; ++i)
    out.append("  ", 2);
}

// A run of bytes that need no escape goes out in one append: a record's ids, names and paths are
// whole runs, and a byte at a time was a call and a capacity check each, the largest single cost of
// writing a document in a debug build (roadmap T183).
template <class Sink>
void string(Sink& out, std::string_view s) {
  out.put('"');
  const char* const begin = s.data();
  const char* const end = begin + s.size();
  const char* run = begin;
  for (const char* p = begin; p != end; ++p) {
    const char c = *p;
    if (static_cast<unsigned char>(c) >= 0x20 && c != '"' && c != '\\') continue;
    if (p != run) out.append(run, static_cast<usize>(p - run));
    run = p + 1;
    switch (c) {
      case '"': out.append("\\\"", 2); break;
      case '\\': out.append("\\\\", 2); break;
      case '\n': out.append("\\n", 2); break;
      case '\r': out.append("\\r", 2); break;
      case '\t': out.append("\\t", 2); break;
      case '\b': out.append("\\b", 2); break;
      case '\f': out.append("\\f", 2); break;
      default: {  // another control character; UTF-8 passes through in the runs
        static constexpr char k_hex[] = "0123456789abcdef";
        out.append("\\u00", 4);
        out.put(k_hex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
        out.put(k_hex[static_cast<unsigned char>(c) & 0xF]);
      }
    }
  }
  if (end != run) out.append(run, static_cast<usize>(end - run));
  out.put('"');
}

template <class Sink, class T>
void integer(Sink& out, T v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
}

// A float in its shortest round-trip form, with ".0" where that form has no fraction or exponent
// so the parser reads it back as a float. False, and "null" written, for a NaN or an infinity.
template <class Sink>
bool number(Sink& out, f64 v) {
  if (!std::isfinite(v)) {
    out.append("null", 4);
    return false;
  }
  if (v == 0.0) {
    if (std::signbit(v))
      out.append("-0.0", 4);
    else
      out.append("0.0", 3);
    return true;
  }
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);  // shortest round-trip
  const std::string_view text(buf, static_cast<usize>(r.ptr - buf));
  out.append(text.data(), text.size());
  if (text.find_first_of(".eE") == std::string_view::npos) out.append(".0", 2);
  return true;
}

template <class Sink>
bool value(Sink& out, const JsonValue& v, bool pretty, u32 depth) {
  bool ok = true;
  switch (v.kind()) {
    case JsonValue::Kind::Null: out.append("null", 4); break;
    case JsonValue::Kind::Bool:
      if (v.as_bool())
        out.append("true", 4);
      else
        out.append("false", 5);
      break;
    case JsonValue::Kind::Int: integer(out, v.as_int()); break;
    case JsonValue::Kind::Uint: integer(out, v.as_uint()); break;
    case JsonValue::Kind::Float: ok = number(out, v.as_float()); break;
    case JsonValue::Kind::String: string(out, v.as_string()); break;
    case JsonValue::Kind::Array: {
      const JsonValue::Array& items = v.as_array();
      if (items.empty()) {
        out.append("[]", 2);
        break;
      }
      out.put('[');
      for (u32 i = 0; i < items.size(); ++i) {
        if (i > 0) out.put(',');
        if (pretty) indent(out, depth + 1);
        ok = value(out, items[i], pretty, depth + 1) && ok;
      }
      if (pretty) indent(out, depth);
      out.put(']');
      break;
    }
    case JsonValue::Kind::Object: {
      const JsonValue::Object& members = v.as_object();
      if (members.empty()) {
        out.append("{}", 2);
        break;
      }
      out.put('{');
      bool first = true;
      for (auto [key, member] : members) {
        if (!first) out.put(',');
        first = false;
        if (pretty) indent(out, depth + 1);
        string(out, key);
        if (pretty)
          out.append(": ", 2);
        else
          out.put(':');
        ok = value(out, member, pretty, depth + 1) && ok;
      }
      if (pretty) indent(out, depth);
      out.put('}');
      break;
    }
  }
  return ok;
}

}  // namespace engine::json_format
