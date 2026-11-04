// A glTF file as an editable document (gltf_document.h): an order-keeping JSON model, Python's
// `json.dumps(indent=2)` as the writer, and the one buffer read and appended to.

#include <domain/assets/gltf_document.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace engine::assets {

namespace {

bool set_error(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// --- parsing ------------------------------------------------------------------------------

constexpr u32 k_max_depth = 256;

struct Parser {
  std::string_view s;
  usize i = 0;
  std::string message;

  bool fail(const char* what) {
    message = std::string(what) + " at byte " + std::to_string(i);
    return false;
  }

  void skip_ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
      ++i;
  }

  bool literal(std::string_view word) {
    if (s.substr(i, word.size()) != word) return fail("invalid literal");
    i += word.size();
    return true;
  }

  static void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(u32& out) {
    if (i + 4 > s.size()) return fail("truncated \\u escape");
    out = 0;
    for (u32 k = 0; k < 4; ++k) {
      const char c = s[i + k];
      u32 d = 0;
      if (c >= '0' && c <= '9') {
        d = static_cast<u32>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<u32>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<u32>(c - 'A' + 10);
      } else {
        return fail("invalid \\u escape");
      }
      out = out * 16 + d;
    }
    i += 4;
    return true;
  }

  bool string(std::string& out) {
    ++i;  // the opening quote
    while (true) {
      if (i >= s.size()) return fail("unterminated string");
      const char c = s[i];
      if (c == '"') {
        ++i;
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
      if (c != '\\') {
        out.push_back(c);
        ++i;
        continue;
      }
      ++i;
      if (i >= s.size()) return fail("unterminated escape");
      const char e = s[i++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          u32 cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u')
              return fail("lone high surrogate");
            i += 2;
            u32 low = 0;
            if (!hex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return fail("lone high surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          append_utf8(out, cp);
          break;
        }
        default: return fail("invalid escape");
      }
    }
  }

  bool number(DocValue& out) {
    const usize start = i;
    bool integral = true;
    if (s[i] == '-') ++i;
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("invalid number");
    if (s[i] == '0') {
      ++i;
    } else {
      while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
      integral = false;
      ++i;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("invalid number");
      while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      integral = false;
      ++i;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("invalid number");
      while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
    }
    const std::string_view token = s.substr(start, i - start);
    if (integral) {
      out.kind = DocValue::Kind::integer;
      // Python reads "-0" as the integer 0 and writes it back as "0".
      out.text = token == "-0" ? std::string("0") : std::string(token);
      return true;
    }
    out.kind = DocValue::Kind::real;
    const auto r = std::from_chars(token.data(), token.data() + token.size(), out.real);
    if (r.ec == std::errc::result_out_of_range) {
      // Python's float() gives an infinity or a zero; JSON written by anything sane has neither.
      return fail("number out of the double range");
    }
    if (r.ec != std::errc{} || r.ptr != token.data() + token.size()) return fail("invalid number");
    return true;
  }

  bool value(DocValue& out, u32 depth) {
    skip_ws();
    if (i >= s.size()) return fail("unexpected end of input");
    if (depth > k_max_depth) return fail("nesting too deep");
    const char c = s[i];
    if (c == '{') {
      out.kind = DocValue::Kind::object;
      ++i;
      skip_ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
        return true;
      }
      while (true) {
        skip_ws();
        if (i >= s.size() || s[i] != '"') return fail("expected a key");
        std::string key;
        if (!string(key)) return false;
        skip_ws();
        if (i >= s.size() || s[i] != ':') return fail("expected ':'");
        ++i;
        DocValue member;
        if (!value(member, depth + 1)) return false;
        // A repeated key keeps its first position and its last value, as a Python dict does.
        out.set(key, std::move(member));
        skip_ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == '}') {
          ++i;
          return true;
        }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      out.kind = DocValue::Kind::array;
      ++i;
      skip_ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
        return true;
      }
      while (true) {
        DocValue item;
        if (!value(item, depth + 1)) return false;
        out.items.push_back(std::move(item));
        skip_ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == ']') {
          ++i;
          return true;
        }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      out.kind = DocValue::Kind::string;
      return string(out.text);
    }
    if (c == 't') {
      out.kind = DocValue::Kind::boolean;
      out.boolean = true;
      return literal("true");
    }
    if (c == 'f') {
      out.kind = DocValue::Kind::boolean;
      return literal("false");
    }
    if (c == 'n') {
      out.kind = DocValue::Kind::null;
      return literal("null");
    }
    return number(out);
  }
};

// --- writing ------------------------------------------------------------------------------

void append_escaped(std::string& out, std::string_view text) {
  static const char k_hex[] = "0123456789abcdef";
  const auto u_escape = [&](u32 unit) {
    out += "\\u";
    out.push_back(k_hex[(unit >> 12) & 0xF]);
    out.push_back(k_hex[(unit >> 8) & 0xF]);
    out.push_back(k_hex[(unit >> 4) & 0xF]);
    out.push_back(k_hex[unit & 0xF]);
  };
  out.push_back('"');
  usize i = 0;
  while (i < text.size()) {
    const u8 b = static_cast<u8>(text[i]);
    if (b < 0x80) {
      ++i;
      switch (b) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
          // Python's ensure_ascii escapes everything outside ' '..'~', DEL included.
          if (b < 0x20 || b == 0x7F) {
            u_escape(b);
          } else {
            out.push_back(static_cast<char>(b));
          }
      }
      continue;
    }
    // A UTF-8 sequence: decode it and escape the code point (a surrogate pair past the BMP). A
    // byte that does not start a valid sequence is escaped as itself; the parser never makes one.
    u32 cp = 0;
    u32 extra = 0;
    if ((b & 0xE0) == 0xC0) {
      cp = b & 0x1Fu;
      extra = 1;
    } else if ((b & 0xF0) == 0xE0) {
      cp = b & 0x0Fu;
      extra = 2;
    } else if ((b & 0xF8) == 0xF0) {
      cp = b & 0x07u;
      extra = 3;
    }
    bool valid = extra > 0 && i + extra < text.size();
    for (u32 k = 1; valid && k <= extra; ++k) {
      const u8 c = static_cast<u8>(text[i + k]);
      if ((c & 0xC0) != 0x80) valid = false;
      cp = (cp << 6) | (c & 0x3Fu);
    }
    if (!valid) {
      u_escape(b);
      ++i;
      continue;
    }
    i += extra + 1;
    if (cp >= 0x10000) {
      const u32 v = cp - 0x10000;
      u_escape(0xD800 + (v >> 10));
      u_escape(0xDC00 + (v & 0x3FF));
    } else {
      u_escape(cp);
    }
  }
  out.push_back('"');
}

bool write_value(const DocValue& v, std::string& out, u32 level, std::string_view newline) {
  const auto indent = [&](u32 n) { out.append(static_cast<usize>(n) * 2, ' '); };
  switch (v.kind) {
    case DocValue::Kind::null: out += "null"; return true;
    case DocValue::Kind::boolean: out += v.boolean ? "true" : "false"; return true;
    case DocValue::Kind::integer: out += v.text; return true;
    case DocValue::Kind::real:
      if (!std::isfinite(v.real)) {
        out += "null";
        return false;
      }
      out += python_float_repr(v.real);
      return true;
    case DocValue::Kind::string: append_escaped(out, v.text); return true;
    case DocValue::Kind::array: {
      if (v.items.empty()) {
        out += "[]";
        return true;
      }
      bool ok = true;
      out.push_back('[');
      for (u32 k = 0; k < v.items.size(); ++k) {
        if (k > 0) out.push_back(',');
        out += newline;
        indent(level + 1);
        ok = write_value(v.items[k], out, level + 1, newline) && ok;
      }
      out += newline;
      indent(level);
      out.push_back(']');
      return ok;
    }
    case DocValue::Kind::object: {
      if (v.items.empty()) {
        out += "{}";
        return true;
      }
      bool ok = true;
      out.push_back('{');
      for (u32 k = 0; k < v.items.size(); ++k) {
        if (k > 0) out.push_back(',');
        out += newline;
        indent(level + 1);
        append_escaped(out, v.keys[k]);
        out += ": ";
        ok = write_value(v.items[k], out, level + 1, newline) && ok;
      }
      out += newline;
      indent(level);
      out.push_back('}');
      return ok;
    }
  }
  return true;
}

bool read_file(const std::filesystem::path& path, Vector<u8>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0 || static_cast<u64>(size) > 0xFFFFFFFFull) return false;
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<u32>(size));
  if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
  return static_cast<bool>(in);
}

u32 read_u32_le(const u8* p) {
  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
         (static_cast<u32>(p[3]) << 24);
}

std::string percent_decode(std::string_view uri) {
  std::string out;
  for (usize i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size()) {
      u32 v = 0;
      const auto r = std::from_chars(uri.data() + i + 1, uri.data() + i + 3, v, 16);
      if (r.ec == std::errc{} && r.ptr == uri.data() + i + 3) {
        out.push_back(static_cast<char>(v));
        i += 2;
        continue;
      }
    }
    out.push_back(uri[i]);
  }
  return out;
}

bool member_u64(const DocValue& object, std::string_view key, u64& out, u64 fallback) {
  const DocValue* v = object.find(key);
  if (v == nullptr) {
    out = fallback;
    return true;
  }
  i64 n = 0;
  if (!v->get_i64(n) || n < 0) return false;
  out = static_cast<u64>(n);
  return true;
}

}  // namespace

// --- DocValue -----------------------------------------------------------------------------

DocValue DocValue::make_bool(bool v) {
  DocValue out;
  out.kind = Kind::boolean;
  out.boolean = v;
  return out;
}

DocValue DocValue::make_int(i64 v) {
  DocValue out;
  out.kind = Kind::integer;
  out.text = std::to_string(v);
  return out;
}

DocValue DocValue::make_real(f64 v) {
  DocValue out;
  out.kind = Kind::real;
  out.real = v;
  return out;
}

DocValue DocValue::make_string(std::string_view v) {
  DocValue out;
  out.kind = Kind::string;
  out.text = std::string(v);
  return out;
}

DocValue DocValue::make_array() {
  DocValue out;
  out.kind = Kind::array;
  return out;
}

DocValue DocValue::make_object() {
  DocValue out;
  out.kind = Kind::object;
  return out;
}

const DocValue* DocValue::find(std::string_view key) const noexcept {
  if (kind != Kind::object) return nullptr;
  for (u32 k = 0; k < keys.size(); ++k)
    if (keys[k] == key) return &items[k];
  return nullptr;
}

DocValue* DocValue::find(std::string_view key) noexcept {
  return const_cast<DocValue*>(static_cast<const DocValue*>(this)->find(key));
}

DocValue& DocValue::set(std::string_view key, DocValue value) {
  if (DocValue* existing = find(key)) {
    *existing = std::move(value);
    return *existing;
  }
  keys.push_back(std::string(key));
  return items.push_back(std::move(value));
}

DocValue& DocValue::push_back(DocValue value) { return items.push_back(std::move(value)); }

bool DocValue::get_f64(f64& out) const noexcept {
  if (kind == Kind::real) {
    out = real;
    return true;
  }
  if (kind != Kind::integer) return false;
  const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
  return r.ec == std::errc{};
}

bool DocValue::get_i64(i64& out) const noexcept {
  if (kind == Kind::integer) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
  }
  if (kind == Kind::real && std::isfinite(real) && std::floor(real) == real &&
      std::fabs(real) < 9.0e18) {
    out = static_cast<i64>(real);
    return true;
  }
  return false;
}

// --- text -----------------------------------------------------------------------------------

bool parse_document_json(std::string_view text, DocValue& out, std::string* error) {
  out = DocValue{};
  if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
      static_cast<u8>(text[2]) == 0xBF)
    text.remove_prefix(3);
  Parser p{text};
  DocValue value;
  if (!p.value(value, 0)) {
    return set_error(error, p.message);
  }
  p.skip_ws();
  if (p.i != text.size()) {
    p.fail("trailing characters");
    return set_error(error, p.message);
  }
  out = std::move(value);
  return true;
}

bool write_document_json(const DocValue& value, std::string& out, std::string_view newline) {
  return write_value(value, out, 0, newline);
}

std::string python_float_repr(f64 value) {
  if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
  // The shortest round-trip digits, as `d.ddde±x`; Python's repr picks the same digits (both
  // take the shortest string that reads back to the value, and the nearest one on a tie).
  char buffer[64];
  const auto r = std::to_chars(buffer, buffer + sizeof(buffer), std::fabs(value),
                               std::chars_format::scientific);
  const std::string_view sci(buffer, static_cast<usize>(r.ptr - buffer));
  const usize e_at = sci.find('e');
  std::string digits;
  for (usize k = 0; k < e_at; ++k)
    if (sci[k] != '.') digits.push_back(sci[k]);
  int exponent = 0;
  std::string_view exp_text = sci.substr(e_at + 1);
  if (!exp_text.empty() && exp_text[0] == '+') exp_text.remove_prefix(1);
  std::from_chars(exp_text.data(), exp_text.data() + exp_text.size(), exponent);
  const int decpt = exponent + 1;  // value = 0.digits * 10^decpt
  const int n = static_cast<int>(digits.size());

  std::string out;
  if (value < 0) out.push_back('-');
  if (decpt <= -4 || decpt > 16) {
    out.push_back(digits[0]);
    if (n > 1) {
      out.push_back('.');
      out.append(digits, 1, std::string::npos);
    }
    out.push_back('e');
    const int e = decpt - 1;
    out.push_back(e < 0 ? '-' : '+');
    const std::string mag = std::to_string(e < 0 ? -e : e);
    if (mag.size() < 2) out.push_back('0');
    out += mag;
    return out;
  }
  if (decpt <= 0) {
    out += "0.";
    out.append(static_cast<usize>(-decpt), '0');
    out += digits;
  } else if (decpt < n) {
    out.append(digits, 0, static_cast<usize>(decpt));
    out.push_back('.');
    out.append(digits, static_cast<usize>(decpt), std::string::npos);
  } else {
    out += digits;
    out.append(static_cast<usize>(decpt - n), '0');
    out += ".0";
  }
  return out;
}

// --- the document ---------------------------------------------------------------------------

bool read_gltf_document(std::string_view path, GltfDocument& out, std::string* error) {
  namespace fs = std::filesystem;
  out = GltfDocument{};
  const fs::path file{std::string(path)};
  Vector<u8> bytes;
  if (!read_file(file, bytes)) return set_error(error, "cannot read " + std::string(path));

  if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
    // GLB: a 12-byte header, a JSON chunk, then an optional BIN chunk.
    const u32 version = read_u32_le(bytes.data() + 4);
    const u32 length = read_u32_le(bytes.data() + 8);
    if (version != 2 || length > bytes.size())
      return set_error(error, "not a version 2 GLB of the length its header says");
    u32 at = 12;
    bool have_json = false;
    while (at + 8 <= length) {
      const u32 chunk_length = read_u32_le(bytes.data() + at);
      const u32 chunk_type = read_u32_le(bytes.data() + at + 4);
      at += 8;
      if (chunk_length > length - at) return set_error(error, "a GLB chunk runs past the file");
      if (chunk_type == 0x4E4F534Au) {  // "JSON"
        const std::string_view text(reinterpret_cast<const char*>(bytes.data() + at), chunk_length);
        if (!parse_document_json(text, out.json, error)) return false;
        have_json = true;
      } else if (chunk_type == 0x004E4942u && have_json && out.bin.empty()) {  // "BIN"
        out.bin.resize(chunk_length);
        if (chunk_length > 0) std::memcpy(out.bin.data(), bytes.data() + at, chunk_length);
      }
      at += chunk_length;
    }
    if (!have_json) return set_error(error, "the GLB has no JSON chunk");
    out.from_glb = true;
  } else {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!parse_document_json(text, out.json, error)) return false;
  }
  if (!out.json.is_object()) return set_error(error, "the glTF document is not a JSON object");

  const DocValue* buffers = out.json.find("buffers");
  if (buffers == nullptr || !buffers->is_array() || buffers->items.empty()) {
    if (!out.from_glb) return set_error(error, "the document has no buffer");
    return true;
  }
  if (buffers->items.size() != 1)
    return set_error(error, "the document has more than one buffer, which is not supported");
  const DocValue* uri = buffers->items[0].find("uri");
  if (out.from_glb) {
    if (uri != nullptr) return set_error(error, "a GLB whose buffer names a URI is not supported");
    return true;
  }
  if (uri == nullptr || !uri->is_string()) return set_error(error, "the buffer has no URI");
  if (uri->text.rfind("data:", 0) == 0)
    return set_error(error, "a buffer held in a data URI is not supported");
  const fs::path bin_path = file.parent_path() / fs::path(percent_decode(uri->text));
  if (!read_file(bin_path, out.bin)) return set_error(error, "cannot read " + bin_path.string());
  return true;
}

bool read_accessor(const GltfDocument& doc, u32 accessor, Vector<f64>& out, u32& components,
                   std::string* error) {
  out.clear();
  const DocValue* accessors = doc.json.find("accessors");
  if (accessors == nullptr || !accessors->is_array() || accessor >= accessors->items.size())
    return set_error(error, "accessor " + std::to_string(accessor) + " does not exist");
  const DocValue& a = accessors->items[accessor];
  const std::string name = "accessor " + std::to_string(accessor);
  if (a.find("sparse") != nullptr) return set_error(error, name + " is sparse");
  if (const DocValue* normalized = a.find("normalized");
      normalized != nullptr && normalized->kind == DocValue::Kind::boolean && normalized->boolean)
    return set_error(error, name + " is normalized");
  u64 view_index = 0, component_type = 0, count = 0, accessor_offset = 0;
  if (a.find("bufferView") == nullptr) return set_error(error, name + " has no buffer view");
  if (!member_u64(a, "bufferView", view_index, 0) ||
      !member_u64(a, "componentType", component_type, 0) || !member_u64(a, "count", count, 0) ||
      !member_u64(a, "byteOffset", accessor_offset, 0))
    return set_error(error, name + " has a malformed field");
  const DocValue* type = a.find("type");
  if (type == nullptr || !type->is_string()) return set_error(error, name + " has no type");
  if (type->text == "SCALAR") {
    components = 1;
  } else if (type->text == "VEC2") {
    components = 2;
  } else if (type->text == "VEC3") {
    components = 3;
  } else if (type->text == "VEC4") {
    components = 4;
  } else if (type->text == "MAT4") {
    components = 16;
  } else {
    return set_error(error, name + " has an unsupported type " + type->text);
  }
  u32 size = 0;
  switch (component_type) {
    case 5126: size = 4; break;  // FLOAT
    case 5125: size = 4; break;  // UNSIGNED_INT
    case 5123: size = 2; break;  // UNSIGNED_SHORT
    case 5121: size = 1; break;  // UNSIGNED_BYTE
    default: return set_error(error, name + " has an unsupported component type");
  }
  const DocValue* views = doc.json.find("bufferViews");
  if (views == nullptr || !views->is_array() || view_index >= views->items.size())
    return set_error(error, name + " names a buffer view that does not exist");
  const DocValue& view = views->items[static_cast<u32>(view_index)];
  u64 view_offset = 0, view_length = 0, stride = 0, buffer = 0;
  if (!member_u64(view, "byteOffset", view_offset, 0) ||
      !member_u64(view, "byteLength", view_length, 0) ||
      !member_u64(view, "byteStride", stride, 0) || !member_u64(view, "buffer", buffer, 0))
    return set_error(error, name + "'s buffer view has a malformed field");
  if (buffer != 0) return set_error(error, name + " is not in buffer 0");
  const u64 element = static_cast<u64>(size) * components;
  if (stride == 0) stride = element;
  const u64 base = view_offset + accessor_offset;
  if (count > 0 && (base + (count - 1) * stride + element > doc.bin.size() ||
                    accessor_offset + (count - 1) * stride + element > view_length))
    return set_error(error, name + " runs past its buffer view");
  if (count * components > 0xFFFFFFFFull) return set_error(error, name + " is too large");
  out.resize(static_cast<u32>(count * components));
  for (u64 e = 0; e < count; ++e) {
    const u8* p = doc.bin.data() + base + e * stride;
    for (u32 c = 0; c < components; ++c) {
      f64 v = 0.0;
      switch (component_type) {
        case 5126: {
          f32 f = 0.0f;
          std::memcpy(&f, p + c * 4u, 4);
          v = static_cast<f64>(f);
          break;
        }
        case 5125: v = static_cast<f64>(read_u32_le(p + c * 4u)); break;
        case 5123:
          v = static_cast<f64>(static_cast<u32>(p[c * 2u]) |
                               (static_cast<u32>(p[c * 2u + 1]) << 8));
          break;
        default: v = static_cast<f64>(p[c]); break;
      }
      out[static_cast<u32>(e * components + c)] = v;
    }
  }
  return true;
}

u32 append_float_accessor(GltfDocument& doc, std::span<const f32> values, u32 components) {
  while (doc.bin.size() % 4 != 0)
    doc.bin.push_back(0);
  const u32 offset = doc.bin.size();
  const u32 bytes = static_cast<u32>(values.size() * sizeof(f32));
  doc.bin.resize(offset + bytes);
  if (bytes > 0) std::memcpy(doc.bin.data() + offset, values.data(), bytes);

  if (doc.json.find("bufferViews") == nullptr) doc.json.set("bufferViews", DocValue::make_array());
  if (doc.json.find("accessors") == nullptr) doc.json.set("accessors", DocValue::make_array());
  DocValue& views = *doc.json.find("bufferViews");
  DocValue view = DocValue::make_object();
  view.set("buffer", DocValue::make_int(0));
  view.set("byteOffset", DocValue::make_int(offset));
  view.set("byteLength", DocValue::make_int(bytes));
  const u32 view_index = views.items.size();
  views.push_back(std::move(view));

  const u32 count = components == 0 ? 0 : static_cast<u32>(values.size() / components);
  DocValue mins = DocValue::make_array();
  DocValue maxs = DocValue::make_array();
  for (u32 c = 0; c < components && count > 0; ++c) {
    f32 lo = values[c];
    f32 hi = values[c];
    for (u32 e = 1; e < count; ++e) {
      const f32 v = values[static_cast<usize>(e) * components + c];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    mins.push_back(DocValue::make_real(static_cast<f64>(lo)));
    maxs.push_back(DocValue::make_real(static_cast<f64>(hi)));
  }
  DocValue& accessors = *doc.json.find("accessors");
  DocValue a = DocValue::make_object();
  a.set("bufferView", DocValue::make_int(view_index));
  a.set("componentType", DocValue::make_int(5126));
  a.set("count", DocValue::make_int(count));
  a.set("type", DocValue::make_string(components == 3 ? "VEC3" : "SCALAR"));
  a.set("min", std::move(mins));
  a.set("max", std::move(maxs));
  const u32 index = accessors.items.size();
  accessors.push_back(std::move(a));
  return index;
}

bool write_gltf_document(GltfDocument& doc, std::string_view dir, std::string_view stem,
                         std::string_view newline, std::string* error) {
  namespace fs = std::filesystem;
  const std::string bin_name = std::string(stem) + ".bin";
  DocValue buffer = DocValue::make_object();
  buffer.set("byteLength", DocValue::make_int(doc.bin.size()));
  buffer.set("uri", DocValue::make_string(bin_name));
  DocValue buffers = DocValue::make_array();
  buffers.push_back(std::move(buffer));
  doc.json.set("buffers", std::move(buffers));

  std::string text;
  if (!write_document_json(doc.json, text, newline))
    return set_error(error, "the document holds a NaN or an infinity");
  text += newline;
  const fs::path base{std::string(dir)};
  const fs::path bin_path = base / bin_name;
  const fs::path gltf_path = base / (std::string(stem) + ".gltf");
  {
    std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
    if (bin && doc.bin.size() > 0)
      bin.write(reinterpret_cast<const char*>(doc.bin.data()), doc.bin.size());
    if (!bin) return set_error(error, "cannot write " + bin_path.string());
  }
  std::ofstream gltf(gltf_path, std::ios::binary | std::ios::trunc);
  gltf.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!gltf) return set_error(error, "cannot write " + gltf_path.string());
  return true;
}

}  // namespace engine::assets
