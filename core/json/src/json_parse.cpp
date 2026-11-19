#include <core/json/json.h>
#include <core/json/json_reader.h>

#include <charconv>
#include <cstring>
#include <string>

namespace engine {

namespace {

constexpr u8 k_frame_object = 1;
constexpr u8 k_frame_started = 2;

void append_utf8(std::string& s, u32 cp) {
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

}  // namespace

JsonReader::JsonReader(std::string_view text, const JsonParseOptions& options)
    : text_(text), max_depth_(options.max_depth) {
  frames_.reserve(16);
}

bool JsonReader::fail(const char* message) {
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
  return false;
}

JsonParseResult JsonReader::result() const noexcept {
  if (failed_) return error_;
  JsonParseResult r;
  r.ok = true;
  return r;
}

void JsonReader::skip_ws() noexcept {
  while (!at_end()) {
    const char c = text_[pos_];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++pos_;
    } else {
      break;
    }
  }
}

bool JsonReader::consume_literal(std::string_view literal) {
  if (text_.substr(pos_, literal.size()) != literal) return false;
  pos_ += literal.size();
  return true;
}

// Every call site of the tree parser's value reader skipped whitespace first, so this does; the
// depth budget is checked where the value starts, as it was.
bool JsonReader::next_value(Event& out) {
  if (failed_) return false;
  skip_ws();
  if (depth() > max_depth_) return fail("nesting deeper than max_depth");
  switch (peek()) {
    case '{':
      ++pos_;
      frames_.push_back(k_frame_object);
      out = Event::Object;
      return true;
    case '[':
      ++pos_;
      frames_.push_back(0);
      out = Event::Array;
      return true;
    case '"':
      if (!read_string()) return false;
      out = Event::String;
      return true;
    case 't':
      if (!consume_literal("true")) return fail("invalid literal");
      bool_ = true;
      out = Event::Bool;
      return true;
    case 'f':
      if (!consume_literal("false")) return fail("invalid literal");
      bool_ = false;
      out = Event::Bool;
      return true;
    case 'n':
      if (!consume_literal("null")) return fail("invalid literal");
      out = Event::Null;
      return true;
    default:
      if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return read_number(out);
      return fail(at_end() ? "unexpected end of input" : "unexpected character");
  }
}

bool JsonReader::next_element(bool& more) {
  more = false;
  if (failed_) return false;
  ENGINE_ASSERT(!frames_.empty() && (frames_.back() & k_frame_object) == 0,
                "JsonReader: next_element outside an array");
  u8& frame = frames_.back();
  skip_ws();
  if ((frame & k_frame_started) == 0) {
    if (peek() == ']') {
      ++pos_;
      frames_.pop_back();
      return true;
    }
    frame |= k_frame_started;
    more = true;
    return true;
  }
  if (peek() == ',') {
    ++pos_;
    more = true;
    return true;
  }
  if (peek() == ']') {
    ++pos_;
    frames_.pop_back();
    return true;
  }
  return fail("expected ',' or ']' in array");
}

bool JsonReader::next_member(bool& more, std::string_view& key) {
  more = false;
  if (failed_) return false;
  ENGINE_ASSERT(!frames_.empty() && (frames_.back() & k_frame_object) != 0,
                "JsonReader: next_member outside an object");
  u8& frame = frames_.back();
  skip_ws();
  if ((frame & k_frame_started) == 0) {
    if (peek() == '}') {
      ++pos_;
      frames_.pop_back();
      return true;
    }
    frame |= k_frame_started;
  } else if (peek() == ',') {
    ++pos_;
  } else if (peek() == '}') {
    ++pos_;
    frames_.pop_back();
    return true;
  } else {
    return fail("expected ',' or '}' in object");
  }
  skip_ws();
  if (peek() != '"') return fail("expected a string key");
  if (!read_string()) return false;
  key = string_;
  skip_ws();
  if (peek() != ':') return fail("expected ':' after key");
  ++pos_;
  skip_ws();
  more = true;
  return true;
}

JsonValue JsonReader::scalar(Event e) const {
  switch (e) {
    case Event::Bool: return JsonValue(bool_);
    case Event::Int: return JsonValue(int_);
    case Event::Uint: return JsonValue(uint_);
    case Event::Float: return JsonValue(float_);
    case Event::String: return JsonValue(string_);
    default: return JsonValue();
  }
}

bool JsonReader::rest(Event begun, JsonValue& out) {
  if (failed_) return false;
  if (begun == Event::Array) {
    JsonValue::Array items;
    bool more = false;
    while (next_element(more) && more) {
      JsonValue value;
      if (!read_value(value)) return false;
      items.push_back(std::move(value));
    }
    if (failed_) return false;
    out = JsonValue(std::move(items));
    return true;
  }
  if (begun == Event::Object) {
    JsonValue::Object members;
    bool more = false;
    std::string_view key;
    while (next_member(more, key) && more) {
      std::string name(key);
      JsonValue value;
      if (!read_value(value)) return false;
      members.insert_or_assign(std::move(name), std::move(value));  // duplicate keys: last wins
    }
    if (failed_) return false;
    out = JsonValue(std::move(members));
    return true;
  }
  out = scalar(begun);
  return true;
}

bool JsonReader::skip(Event begun) {
  if (failed_) return false;
  bool more = false;
  if (begun == Event::Array) {
    while (next_element(more) && more) {
      if (!skip_value()) return false;
    }
    return !failed_;
  }
  if (begun == Event::Object) {
    std::string_view key;
    while (next_member(more, key) && more) {
      if (!skip_value()) return false;
    }
    return !failed_;
  }
  return true;
}

bool JsonReader::read_value(JsonValue& out) {
  Event e;
  return next_value(e) && rest(e, out);
}

bool JsonReader::skip_value() {
  Event e;
  return next_value(e) && skip(e);
}

bool JsonReader::finish() {
  if (failed_) return false;
  skip_ws();
  if (pos_ != text_.size()) return fail("unexpected characters after the JSON value");
  return true;
}

bool JsonReader::parse_hex4(u32& out) {
  if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
  u32 v = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = text_[pos_++];
    v <<= 4;
    if (c >= '0' && c <= '9')
      v |= static_cast<u32>(c - '0');
    else if (c >= 'a' && c <= 'f')
      v |= static_cast<u32>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v |= static_cast<u32>(c - 'A' + 10);
    else
      return fail("invalid hex digit in \\u escape");
  }
  out = v;
  return true;
}

bool JsonReader::read_string() {
  ++pos_;  // opening quote
  bool buffered = false;
  while (true) {
    // The run up to the next quote, escape or control character in one step (roadmap T183: a byte
    // at a time was most of reading a document in a debug build). A string with no escape is a view
    // of the text and copies nothing.
    const char* const base = text_.data();
    const char* p = base + pos_;
    const char* const end = base + text_.size();
    while (p != end && *p != '"' && *p != '\\' && static_cast<unsigned char>(*p) >= 0x20)
      ++p;
    const usize run = static_cast<usize>(p - (base + pos_));
    if (!buffered && p != end && *p == '"') {
      string_ = text_.substr(pos_, run);
      pos_ += run + 1;
      return true;
    }
    if (!buffered) {
      buffer_.clear();
      buffered = true;
    }
    if (run != 0) buffer_.append(base + pos_, run);
    pos_ += run;
    if (at_end()) return fail("unterminated string");
    const char c = text_[pos_++];
    if (c == '"') {
      string_ = buffer_;
      return true;
    }
    if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
    if (at_end()) return fail("unterminated escape");
    const char e = text_[pos_++];
    switch (e) {
      case '"': buffer_.push_back('"'); break;
      case '\\': buffer_.push_back('\\'); break;
      case '/': buffer_.push_back('/'); break;
      case 'b': buffer_.push_back('\b'); break;
      case 'f': buffer_.push_back('\f'); break;
      case 'n': buffer_.push_back('\n'); break;
      case 'r': buffer_.push_back('\r'); break;
      case 't': buffer_.push_back('\t'); break;
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
        append_utf8(buffer_, cp);
        break;
      }
      default: return fail("invalid escape sequence");
    }
  }
}

bool JsonReader::read_number(Event& out) {
  const usize start = pos_;
  bool is_float = false;
  if (peek() == '-') ++pos_;
  if (at_end()) return fail("truncated number");
  if (peek() == '0') {
    ++pos_;
  } else if (peek() >= '1' && peek() <= '9') {
    while (peek() >= '0' && peek() <= '9')
      ++pos_;
  } else {
    return fail("invalid number");
  }
  if (peek() == '.') {
    is_float = true;
    ++pos_;
    if (!(peek() >= '0' && peek() <= '9')) return fail("digits required after decimal point");
    while (peek() >= '0' && peek() <= '9')
      ++pos_;
  }
  if (peek() == 'e' || peek() == 'E') {
    is_float = true;
    ++pos_;
    if (peek() == '+' || peek() == '-') ++pos_;
    if (!(peek() >= '0' && peek() <= '9')) return fail("digits required in exponent");
    while (peek() >= '0' && peek() <= '9')
      ++pos_;
  }
  const std::string_view token = text_.substr(start, pos_ - start);
  const char* first = token.data();
  const char* last = first + token.size();
  if (!is_float) {
    if (token[0] == '-') {
      i64 v;
      const auto r = std::from_chars(first, last, v);
      if (r.ec == std::errc{} && r.ptr == last) {
        int_ = v;
        out = Event::Int;
        return true;
      }
    } else {
      u64 v;
      const auto r = std::from_chars(first, last, v);
      if (r.ec == std::errc{} && r.ptr == last) {
        if (v <= static_cast<u64>(INT64_MAX)) {
          int_ = static_cast<i64>(v);
          out = Event::Int;
        } else {
          uint_ = v;
          out = Event::Uint;
        }
        return true;
      }
    }
    // Integer out of 64-bit range: fall through to double.
  }
  f64 d;
  const auto r = std::from_chars(first, last, d);
  if (r.ec != std::errc{} || r.ptr != last) return fail("invalid number");
  float_ = d;
  out = Event::Float;
  return true;
}

JsonParseResult parse_json(std::string_view text, JsonValue& out, const JsonParseOptions& options) {
  out = JsonValue();
  JsonReader reader(text, options);
  JsonValue value;
  if (!reader.read_value(value) || !reader.finish()) return reader.result();
  out = std::move(value);
  return reader.result();
}

}  // namespace engine
