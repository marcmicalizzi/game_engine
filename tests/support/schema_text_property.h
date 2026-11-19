#pragma once

// The property that holds the schema walk over JSON text to the tree path (roadmap D40;
// core/schema/json_text.h), over every struct type registered in the test binary that includes it:
// a generated value written and read both ways to the same object and the same diagnostics, the
// canonical writer and comparison against `write_json(to_json(...))`, and corrupted texts refused
// both ways with the same verdict. `core/schema` runs it over its feature types and `domain/doc`
// over the engine's own schemas, which `core/schema`'s tests cannot link (they are declared later).

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/json_text.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <new>
#include <random>
#include <string>

namespace engine::test::schema_text {

using namespace engine::schema;

// An instance of a registered struct type, constructed in place.
struct Instance {
  const TypeInfo& type;
  void* p;
  explicit Instance(const TypeInfo& t) : type(t) {
    p = ::operator new(t.size, std::align_val_t(t.align));
    t.ops->construct(p);
  }
  ~Instance() {
    type.ops->destroy(p);
    ::operator delete(p, std::align_val_t(type.align));
  }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
};

inline std::string joined(const ReadContext& ctx) {
  std::string out;
  for (const Diagnostic& d : ctx.diagnostics)
    out += d.path + ": " + d.message + "\n";
  return out;
}

// What the tree path does: parse, then map into a default-constructed object.
struct Outcome {
  JsonParseResult parse;
  bool ok = false;
  std::string diagnostics;
};

inline Outcome by_tree(const TypeInfo& type, void* object, std::string_view text,
                       const TextReadOptions& options = {}) {
  Outcome o;
  JsonValue root;
  o.parse = parse_json(text, root, options.parse);
  if (!o.parse.ok) return o;
  ReadContext ctx;
  bool prepared = true;
  for (const TreeField& t : options.tree_fields) {
    JsonValue* member = root.is_object() ? root.find(t.name) : nullptr;
    if (member != nullptr && !t.prepare(*member, ctx, t.user)) prepared = false;
  }
  o.ok = prepared && from_json(type, object, root, ctx);
  o.diagnostics = joined(ctx);
  return o;
}

inline Outcome by_text(const TypeInfo& type, void* object, std::string_view text, bool* streamed,
                       const TextReadOptions& options = {}) {
  Outcome o;
  ReadContext ctx;
  o.ok = read_json_text(type, object, text, ctx, o.parse, options, streamed);
  o.diagnostics = joined(ctx);
  return o;
}

// Both ways on one text, each into a fresh object: the same verdict, refusal, diagnostics, object.
inline void check_same(const TypeInfo& type, std::string_view text, bool expect_streamed = false) {
  Instance a(type);
  Instance b(type);
  const Outcome tree = by_tree(type, a.p, text);
  bool streamed = false;
  const Outcome stream = by_text(type, b.p, text, &streamed);
  INFO(type.qualified_name << " text: " << std::string(text.substr(0, 400)));
  REQUIRE(tree.parse.ok == stream.parse.ok);
  if (!tree.parse.ok) {
    CHECK(std::string(tree.parse.message) == stream.parse.message);
    CHECK(tree.parse.offset == stream.parse.offset);
    CHECK(tree.parse.line == stream.parse.line);
    CHECK(tree.parse.column == stream.parse.column);
    CHECK_FALSE(stream.ok);
    CHECK(stream.diagnostics.empty());
    return;
  }
  CHECK(tree.ok == stream.ok);
  CHECK(tree.diagnostics == stream.diagnostics);
  CHECK(type.ops->equals(a.p, b.p));
  if (expect_streamed) CHECK(streamed);
}

// --- generated values, by type ----------------------------------------------------------------

struct Generator {
  std::mt19937_64 rng;
  explicit Generator(u64 seed) : rng(seed) {}

  u64 next() { return rng(); }
  u32 below(u32 n) { return static_cast<u32>(next() % n); }
  bool coin() { return (next() & 1) != 0; }

  std::string text() {
    static const char* const k_pieces[] = {"a",
                                           "Z",
                                           "0",
                                           " ",
                                           "\"",
                                           "\\",
                                           "\n",
                                           "\t",
                                           "/",
                                           "\x01",
                                           "\xc3\xa9",
                                           "\xe2\x82\xac",
                                           "\xf0\x9f\x8c\xb2",
                                           "key",
                                           "_"};
    std::string s;
    const u32 n = below(6);
    for (u32 i = 0; i < n; ++i)
      s += k_pieces[below(sizeof(k_pieces) / sizeof(k_pieces[0]))];
    return s;
  }

  f64 any_double() {
    switch (below(5)) {
      case 0: return 0.0;
      case 1: return -0.0;
      case 2: return static_cast<f64>(static_cast<i32>(next() % 2001) - 1000);
      case 3:
        return std::ldexp(static_cast<f64>(next() % 1000000) - 500000.0,
                          static_cast<int>(below(80)) - 40);
      default: {
        f64 d;
        do {
          const u64 bits = next();
          std::memcpy(&d, &bits, sizeof(d));
        } while (!std::isfinite(d));
        return d;
      }
    }
  }

  f32 any_float() {
    if (coin())
      return static_cast<f32>(
          std::ldexp(static_cast<f64>(below(2000)) - 1000.0, static_cast<int>(below(60)) - 30));
    f32 f;
    do {
      const u32 bits = static_cast<u32>(next());
      std::memcpy(&f, &bits, sizeof(f));
    } while (!std::isfinite(f));
    return f;
  }

  JsonValue json(u32 depth) {
    switch (below(depth > 2 ? 6 : 8)) {
      case 0: return JsonValue();
      case 1: return JsonValue(coin());
      case 2: return JsonValue(static_cast<i64>(next()));
      case 3: return JsonValue(next() | (u64{1} << 63));
      case 4: return JsonValue(any_double());
      case 5: return JsonValue(text());
      case 6: {
        JsonValue a = JsonValue::array();
        for (u32 i = below(4); i > 0; --i)
          a.push_back(json(depth + 1));
        return a;
      }
      default: {
        JsonValue o = JsonValue::object();
        for (u32 i = below(4); i > 0; --i)
          o.set(text(), json(depth + 1));
        return o;
      }
    }
  }

  JsonValue integer(Kind kind) {
    switch (kind) {
      case Kind::U8: return JsonValue(static_cast<u64>(below(256)));
      case Kind::U16: return JsonValue(static_cast<u64>(below(65536)));
      case Kind::U32: return JsonValue(static_cast<u64>(static_cast<u32>(next())));
      case Kind::U64: return JsonValue(next());
      case Kind::I8: return JsonValue(static_cast<i64>(static_cast<i8>(next())));
      case Kind::I16: return JsonValue(static_cast<i64>(static_cast<i16>(next())));
      case Kind::I32: return JsonValue(static_cast<i64>(static_cast<i32>(next())));
      default: return JsonValue(static_cast<i64>(next()));
    }
  }

  // A JSON value of the shape `to_json` gives `type`.
  JsonValue of(const TypeRef& type, u32 depth) {
    switch (type.kind) {
      case Kind::Bool: return JsonValue(coin());
      case Kind::U8:
      case Kind::U16:
      case Kind::U32:
      case Kind::U64:
      case Kind::I8:
      case Kind::I16:
      case Kind::I32:
      case Kind::I64: return integer(type.kind);
      case Kind::F32: return JsonValue(static_cast<f64>(any_float()));
      case Kind::F64: return JsonValue(any_double());
      case Kind::String: return JsonValue(text());
      case Kind::Bytes: {
        std::string hex;
        for (u32 i = below(5); i > 0; --i) {
          static constexpr char k_hex[] = "0123456789abcdef";
          hex.push_back(k_hex[below(16)]);
          hex.push_back(k_hex[below(16)]);
        }
        return JsonValue(hex);
      }
      case Kind::Id128: {
        std::string hex;
        static constexpr char k_hex[] = "0123456789abcdef";
        for (u32 i = 0; i < 32; ++i)
          hex.push_back(k_hex[below(16)]);
        return JsonValue(hex);
      }
      case Kind::Vec2:
      case Kind::Vec3:
      case Kind::Vec4:
      case Kind::Quat: {
        const u32 n = type.kind == Kind::Vec2 ? 2 : type.kind == Kind::Vec3 ? 3 : 4;
        JsonValue a = JsonValue::array();
        for (u32 i = 0; i < n; ++i)
          a.push_back(JsonValue(static_cast<f64>(any_float())));
        return a;
      }
      case Kind::WorldPos:
      case Kind::DVec3: {
        JsonValue a = JsonValue::array();
        for (u32 i = 0; i < 3; ++i)
          a.push_back(JsonValue(std::ldexp(static_cast<f64>(next() % 2000000) - 1000000.0,
                                           static_cast<int>(below(20)))));
        return a;
      }
      case Kind::Json: return json(depth);
      case Kind::Enum: {
        const TypeInfo& info = *type.type;
        if (!info.values.empty() && below(8) != 0)
          return JsonValue(
              std::string_view(info.values[below(static_cast<u32>(info.values.size()))].name));
        return JsonValue(static_cast<i64>(below(4)));
      }
      case Kind::Struct: {
        JsonValue o = JsonValue::object();
        if (depth > 4) return o;
        for (const FieldInfo& f : type.type->fields) {
          if ((f.flags & FieldFlag::transient) != 0 || below(5) == 0) continue;
          o.set(f.name, of(f.type, depth + 1));
        }
        return o;
      }
      case Kind::Optional:
        if (coin() || depth > 4) return JsonValue();
        return of(*type.element, depth + 1);
      case Kind::Array: {
        JsonValue a = JsonValue::array();
        if (depth > 4) return a;
        for (u32 i = below(4); i > 0; --i)
          a.push_back(of(*type.element, depth + 1));
        return a;
      }
      case Kind::FixedArray: {
        JsonValue a = JsonValue::array();
        for (u32 i = 0; i < type.fixed_count; ++i)
          a.push_back(of(*type.element, depth + 1));
        return a;
      }
      case Kind::Map: {
        if (type.key->kind == Kind::String) {
          JsonValue o = JsonValue::object();
          if (depth > 4) return o;
          for (u32 i = below(4); i > 0; --i)
            o.set(text(), of(*type.element, depth + 1));
          return o;
        }
        JsonValue a = JsonValue::array();
        if (depth > 4) return a;
        for (u32 i = below(4); i > 0; --i) {
          JsonValue pair = JsonValue::array();
          pair.push_back(of(*type.key, depth + 1));
          pair.push_back(of(*type.element, depth + 1));
          a.push_back(std::move(pair));
        }
        return a;
      }
    }
    return JsonValue();
  }

  // A byte of the text changed, cut, or one added: most of these are syntax errors, the rest
  // values the schema refuses or takes.
  std::string corrupt(const std::string& text) {
    static constexpr char k_bytes[] = "{}[],:\" \\0a-.eE1n\x01\xff";
    std::string s = text;
    if (s.empty()) return "{";
    const usize at = next() % s.size();
    switch (below(4)) {
      case 0: s.resize(at); break;
      case 1: s[at] = k_bytes[below(sizeof(k_bytes) - 1)]; break;
      case 2:
        s.insert(s.begin() + static_cast<std::ptrdiff_t>(at), k_bytes[below(sizeof(k_bytes) - 1)]);
        break;
      default: s.erase(at, 1 + below(8)); break;
    }
    return s;
  }
};

inline TypeRef ref_of(const TypeInfo& type) {
  TypeRef ref;
  ref.kind = type.kind;
  ref.type = &type;
  ref.size = type.size;
  ref.align = type.align;
  return ref;
}

// Every registered struct type, `rounds` generated values each, compact and pretty; counts what it
// ran.
inline void run_property(u64 seed, u32 rounds, u32& types, u32& texts) {
  Generator gen(seed);
  for (const TypeInfo* info : Registry::global().all()) {
    if (info->kind != Kind::Struct || info->ops == nullptr) continue;
    ++types;
    for (u32 round = 0; round < rounds; ++round) {
      const JsonValue generated = gen.of(ref_of(*info), 0);
      for (const bool pretty : {true, false}) {
        const std::string text = write_json(generated, JsonWriteOptions{.pretty = pretty});
        check_same(*info, text, /*expect_streamed=*/true);
        ++texts;

        // What the tree path read, written both ways: the same bytes, and the comparison agrees.
        Instance a(*info);
        ReadContext ctx;
        JsonValue parsed;
        REQUIRE(parse_json(text, parsed).ok);
        (void)from_json(*info, a.p, parsed, ctx);
        JsonValue back;
        REQUIRE(to_json(*info, a.p, back));
        const JsonWriteOptions options{.pretty = pretty};
        const std::string canonical = write_json(back, options);
        std::string streamed_text;
        (void)write_json_text(*info, a.p, streamed_text, options);
        CHECK(streamed_text == canonical);
        CHECK(canonical_json_equals(*info, a.p, canonical, {}, options));
        CHECK(canonical_json_equals(*info, a.p, canonical + "\n", "\n", options));
        CHECK_FALSE(canonical_json_equals(*info, a.p, canonical + "\n", {}, options));
        CHECK_FALSE(canonical_json_equals(*info, a.p, canonical.substr(0, canonical.size() - 1), {},
                                          options));
        std::string changed = canonical;
        changed[gen.next() % changed.size()] ^= 0x20;
        CHECK_FALSE(canonical_json_equals(*info, a.p, changed, {}, options));

        for (u32 c = 0; c < 6; ++c) {
          check_same(*info, gen.corrupt(text));
          ++texts;
        }
      }
    }
  }
}

}  // namespace engine::test::schema_text
