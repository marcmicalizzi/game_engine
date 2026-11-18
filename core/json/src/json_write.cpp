#include <core/json/json.h>

#include <charconv>
#include <cmath>

namespace engine {

namespace {

void write_indent(std::string& out, u32 depth) {
  out.push_back('\n');
  for (u32 i = 0; i < depth; ++i)
    out.append("  ");
}

// A run of bytes that need no escape goes out in one append: a record's ids, names and paths are
// whole runs, and a byte at a time was a call and a capacity check each, the largest single cost of
// writing a document in a debug build (roadmap T183).
void write_escaped(std::string_view s, std::string& out) {
  out.push_back('"');
  const char* const begin = s.data();
  const char* const end = begin + s.size();
  const char* run = begin;
  for (const char* p = begin; p != end; ++p) {
    const char c = *p;
    if (static_cast<unsigned char>(c) >= 0x20 && c != '"' && c != '\\') continue;
    if (p != run) out.append(run, static_cast<usize>(p - run));
    run = p + 1;
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      default: {  // another control character; UTF-8 passes through in the runs
        static constexpr char k_hex[] = "0123456789abcdef";
        out.append("\\u00");
        out.push_back(k_hex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
        out.push_back(k_hex[static_cast<unsigned char>(c) & 0xF]);
      }
    }
  }
  if (end != run) out.append(run, static_cast<usize>(end - run));
  out.push_back('"');
}

template <class T>
void write_integer(T v, std::string& out) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
}

bool write_float(f64 v, std::string& out) {
  if (!std::isfinite(v)) {
    out.append("null");
    return false;
  }
  if (v == 0.0) {
    out.append(std::signbit(v) ? "-0.0" : "0.0");
    return true;
  }
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);  // shortest round-trip
  std::string_view text(buf, static_cast<usize>(r.ptr - buf));
  out.append(text);
  // Keep floats distinguishable from integers so the parser preserves the Float kind.
  if (text.find_first_of(".eE") == std::string_view::npos) out.append(".0");
  return true;
}

bool write_value(const JsonValue& v, std::string& out, const JsonWriteOptions& opts, u32 depth) {
  bool ok = true;
  switch (v.kind()) {
    case JsonValue::Kind::Null: out.append("null"); break;
    case JsonValue::Kind::Bool: out.append(v.as_bool() ? "true" : "false"); break;
    case JsonValue::Kind::Int: write_integer(v.as_int(), out); break;
    case JsonValue::Kind::Uint: write_integer(v.as_uint(), out); break;
    case JsonValue::Kind::Float: ok = write_float(v.as_float(), out); break;
    case JsonValue::Kind::String: write_escaped(v.as_string(), out); break;
    case JsonValue::Kind::Array: {
      const JsonValue::Array& items = v.as_array();
      if (items.empty()) {
        out.append("[]");
        break;
      }
      out.push_back('[');
      for (u32 i = 0; i < items.size(); ++i) {
        if (i > 0) out.push_back(',');
        if (opts.pretty) write_indent(out, depth + 1);
        ok = write_value(items[i], out, opts, depth + 1) && ok;
      }
      if (opts.pretty) write_indent(out, depth);
      out.push_back(']');
      break;
    }
    case JsonValue::Kind::Object: {
      const JsonValue::Object& members = v.as_object();
      if (members.empty()) {
        out.append("{}");
        break;
      }
      out.push_back('{');
      bool first = true;
      for (auto [key, value] : members) {
        if (!first) out.push_back(',');
        first = false;
        if (opts.pretty) write_indent(out, depth + 1);
        write_escaped(key, out);
        out.append(opts.pretty ? ": " : ":");
        ok = write_value(value, out, opts, depth + 1) && ok;
      }
      if (opts.pretty) write_indent(out, depth);
      out.push_back('}');
      break;
    }
  }
  return ok;
}

}  // namespace

bool write_json(const JsonValue& value, std::string& out, const JsonWriteOptions& options) {
  return write_value(value, out, options, 0);
}

std::string write_json(const JsonValue& value, const JsonWriteOptions& options) {
  std::string out;
  write_value(value, out, options, 0);
  return out;
}

}  // namespace engine
