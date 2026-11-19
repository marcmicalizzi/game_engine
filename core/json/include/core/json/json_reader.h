#pragma once

// A pull reader over JSON text: one value at a time, never a tree (roadmap D40).
//
// `parse_json` is built on it, so the two read one grammar with one set of refusals: the same
// message at the same offset, line and column for every malformed text, and the same depth budget.
// What it adds is that a caller can take a value as it comes — a schema walker reflecting a 172 MB
// scene file straight into its structs (`schema::read_json_text`), a document store reading a
// layer — instead of holding a tree of every value of the file while it is copied out.
//
// The protocol: `next_value` reads the start of a value. A scalar is then whole (`bool_value`,
// `int_value`, `uint_value`, `float_value`, `string_value`); an array or an object is open, and its
// members are walked with `next_element` or `next_member`, each followed by one value (read with
// `next_value`, `read_value` or `skip_value`). `rest` builds the rest of a value whose start was
// read into a `JsonValue`, and `skip` passes over it — both checking every byte as the parser
// does. After the top-level value, `finish` refuses anything but whitespace after it.
//
// Strings are views: into the text when the string holds no escape, else into a buffer of the
// reader's own that the next string read reuses. A failed read stays failed; `result` says where.
//
// `JsonParseOptions::refuse_duplicate_keys` (C61) is held by `next_member`, which every path walks
// — `parse_json`'s tree, `skip`, and a schema walker reading straight into its structs — so a key
// given twice in one object is refused at the second wherever the text is read from.

#include <core/base/types.h>
#include <core/containers/hash_set.h>
#include <core/containers/vector.h>
#include <core/json/json.h>
#include <core/json/json_value.h>

#include <string>
#include <string_view>

namespace engine {

class JsonReader {
 public:
  // What `next_value` found. The scalar kinds are JsonValue's; the containers are open.
  enum class Event : u8 { Null, Bool, Int, Uint, Float, String, Array, Object };

  explicit JsonReader(std::string_view text, const JsonParseOptions& options = {});

  // The start of the next value: false on a syntax error or past the depth budget.
  bool next_value(Event& out);
  // In an open array: whether another element follows (then read it), or the array closed.
  bool next_element(bool& more);
  // In an open object: whether another member follows, with its key (then read its value), or the
  // object closed. The key is valid until the next string is read.
  bool next_member(bool& more, std::string_view& key);
  // The rest of the value `begun` started: a scalar as itself, a container's members to its end.
  bool rest(Event begun, JsonValue& out);
  bool skip(Event begun);
  // A whole value: next_value, then rest or skip.
  bool read_value(JsonValue& out);
  bool skip_value();
  // Whitespace to the end of the text, or the parser's "unexpected characters" refusal.
  bool finish();

  bool bool_value() const noexcept { return bool_; }
  i64 int_value() const noexcept { return int_; }
  u64 uint_value() const noexcept { return uint_; }
  f64 float_value() const noexcept { return float_; }
  std::string_view string_value() const noexcept { return string_; }
  // The scalar just read as a JsonValue. A string is copied; every other scalar allocates nothing.
  JsonValue scalar(Event e) const;

  bool failed() const noexcept { return failed_; }
  // ok while nothing failed; the first failure otherwise.
  JsonParseResult result() const noexcept;
  // Bytes consumed so far.
  usize offset() const noexcept { return pos_; }
  // Open containers, the depth the next value is read at.
  u32 depth() const noexcept { return static_cast<u32>(frames_.size()); }

 private:
  bool fail(const char* message);
  bool at_end() const noexcept { return pos_ >= text_.size(); }
  char peek() const noexcept { return at_end() ? '\0' : text_[pos_]; }
  void skip_ws() noexcept;
  bool consume_literal(std::string_view literal);
  bool parse_hex4(u32& out);
  // Reads a string whose opening quote is at pos_ into `string_`.
  bool read_string();
  bool read_number(Event& out);

  // The strict mode's refusal (C61) at the key just read, whose quote is at `key_start`.
  bool refuse_duplicate(usize key_start);

  std::string_view text_;
  u32 max_depth_ = 0;
  bool refuse_duplicate_keys_ = false;
  usize pos_ = 0;
  bool failed_ = false;
  JsonParseResult error_;
  // One byte per open container: bit 0 an object, bit 1 a member or element already read.
  Vector<u8> frames_;
  // With `refuse_duplicate_keys` only: the keys of every open object, each prefixed with its
  // object's depth, and how many the set held when each open object began (its keys are the set's
  // tail). One lookup a key (`refuse_duplicate`), whatever an object's size; `probe_` is the
  // prefixed key looked up, reused.
  HashSet<std::string> keys_;
  Vector<u32> key_marks_;
  std::string probe_;
  std::string buffer_;  // a string with escapes, decoded
  std::string_view string_;
  bool bool_ = false;
  i64 int_ = 0;
  u64 uint_ = 0;
  f64 float_ = 0.0;
};

}  // namespace engine
