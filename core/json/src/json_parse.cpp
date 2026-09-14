#include <core/json/json.h>

#include <charconv>
#include <cstring>
#include <string>

namespace engine {

namespace {

class Parser {
 public:
  Parser(std::string_view text, const JsonParseOptions& options) : text_(text), options_(options) {}

  JsonParseResult run(JsonValue& out) {
    skip_ws();
    if (!parse_value(out, 0)) return fail_result();
    skip_ws();
    if (pos_ != text_.size()) {
      out = JsonValue();
      return error("unexpected characters after the JSON value");
    }
    JsonParseResult r;
    r.ok = true;
    return r;
  }

 private:
  JsonParseResult fail_result() {
    JsonParseResult r = error_;
    return r;
  }

  JsonParseResult error(const char* message) {
    if (!failed_) {
      failed_ = true;
      error_.ok = false;
      error_.offset = pos_;
      error_.message = message;
      error_.line = 1;
      error_.column = 1;
      for (usize i = 0; i < pos_ && i < text_.size(); ++i) {
        if (text_[i] == '\n') {
          ++error_.line;
          error_.column = 1;
        } else {
          ++error_.column;
        }
      }
    }
    return error_;
  }

  bool fail(const char* message) {
    error(message);
    return false;
  }

  bool at_end() const noexcept { return pos_ >= text_.size(); }
  char peek() const noexcept { return at_end() ? '\0' : text_[pos_]; }

  void skip_ws() noexcept {
    while (!at_end()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume_literal(const char* lit) {
    const usize n = std::strlen(lit);
    if (text_.substr(pos_, n) != std::string_view(lit, n)) return false;
    pos_ += n;
    return true;
  }

  bool parse_value(JsonValue& out, u32 depth) {
    if (depth > options_.max_depth) return fail("nesting deeper than max_depth");
    switch (peek()) {
      case '{': return parse_object(out, depth);
      case '[': return parse_array(out, depth);
      case '"': {
        std::string s;
        if (!parse_string(s)) return false;
        out = JsonValue(std::string_view(s));
        return true;
      }
      case 't':
        if (!consume_literal("true")) return fail("invalid literal");
        out = JsonValue(true);
        return true;
      case 'f':
        if (!consume_literal("false")) return fail("invalid literal");
        out = JsonValue(false);
        return true;
      case 'n':
        if (!consume_literal("null")) return fail("invalid literal");
        out = JsonValue();
        return true;
      default:
        if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return parse_number(out);
        return fail(at_end() ? "unexpected end of input" : "unexpected character");
    }
  }

  bool parse_object(JsonValue& out, u32 depth) {
    ++pos_;  // '{'
    JsonValue::Object members;
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      out = JsonValue(std::move(members));
      return true;
    }
    while (true) {
      skip_ws();
      if (peek() != '"') return fail("expected a string key");
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (peek() != ':') return fail("expected ':' after key");
      ++pos_;
      skip_ws();
      JsonValue value;
      if (!parse_value(value, depth + 1)) return false;
      members.insert_or_assign(std::move(key), std::move(value));  // duplicate keys: last wins
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        break;
      }
      return fail("expected ',' or '}' in object");
    }
    out = JsonValue(std::move(members));
    return true;
  }

  bool parse_array(JsonValue& out, u32 depth) {
    ++pos_;  // '['
    JsonValue::Array items;
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      out = JsonValue(std::move(items));
      return true;
    }
    while (true) {
      skip_ws();
      JsonValue value;
      if (!parse_value(value, depth + 1)) return false;
      items.push_back(std::move(value));
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        break;
      }
      return fail("expected ',' or ']' in array");
    }
    out = JsonValue(std::move(items));
    return true;
  }

  static void append_utf8(std::string& s, u32 cp) {
    if (cp < 0x80) {
      s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_hex4(u32& out) {
    if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
    u32 v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<u32>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<u32>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<u32>(c - 'A' + 10);
      else return fail("invalid hex digit in \\u escape");
    }
    out = v;
    return true;
  }

  bool parse_string(std::string& out) {
    ++pos_;  // opening quote
    while (true) {
      if (at_end()) return fail("unterminated string");
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (at_end()) return fail("unterminated escape");
      const char e = text_[pos_++];
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
          u32 cp;
          if (!parse_hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: a low surrogate must follow.
            if (text_.substr(pos_, 2) != "\\u") return fail("high surrogate without low surrogate");
            pos_ += 2;
            u32 low;
            if (!parse_hex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return fail("invalid low surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("unexpected low surrogate");
          }
          append_utf8(out, cp);
          break;
        }
        default: return fail("invalid escape sequence");
      }
    }
  }

  bool parse_number(JsonValue& out) {
    const usize start = pos_;
    bool is_float = false;
    if (peek() == '-') ++pos_;
    if (at_end()) return fail("truncated number");
    if (peek() == '0') {
      ++pos_;
    } else if (peek() >= '1' && peek() <= '9') {
      while (peek() >= '0' && peek() <= '9') ++pos_;
    } else {
      return fail("invalid number");
    }
    if (peek() == '.') {
      is_float = true;
      ++pos_;
      if (!(peek() >= '0' && peek() <= '9')) return fail("digits required after decimal point");
      while (peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      is_float = true;
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!(peek() >= '0' && peek() <= '9')) return fail("digits required in exponent");
      while (peek() >= '0' && peek() <= '9') ++pos_;
    }
    const std::string_view token = text_.substr(start, pos_ - start);
    const char* first = token.data();
    const char* last = first + token.size();
    if (!is_float) {
      if (token[0] == '-') {
        i64 v;
        const auto r = std::from_chars(first, last, v);
        if (r.ec == std::errc{} && r.ptr == last) {
          out = JsonValue(v);
          return true;
        }
      } else {
        u64 v;
        const auto r = std::from_chars(first, last, v);
        if (r.ec == std::errc{} && r.ptr == last) {
          if (v <= static_cast<u64>(INT64_MAX)) {
            out = JsonValue(static_cast<i64>(v));
          } else {
            out = JsonValue(v);
          }
          return true;
        }
      }
      // Integer out of 64-bit range: fall through to double.
    }
    f64 d;
    const auto r = std::from_chars(first, last, d);
    if (r.ec != std::errc{} || r.ptr != last) return fail("invalid number");
    out = JsonValue(d);
    return true;
  }

  std::string_view text_;
  const JsonParseOptions& options_;
  usize pos_ = 0;
  bool failed_ = false;
  JsonParseResult error_;
};

}  // namespace

JsonParseResult parse_json(std::string_view text, JsonValue& out, const JsonParseOptions& options) {
  out = JsonValue();
  Parser parser(text, options);
  JsonParseResult r = parser.run(out);
  if (!r.ok) out = JsonValue();
  return r;
}

}  // namespace engine
