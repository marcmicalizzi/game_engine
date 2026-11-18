#pragma once

// The hostile-input harness: its generator, its mutators and what a target is (roadmap T13, stage
// 0; docs/experiments/fuzzing-design.md, "2. The harness: both, with different jobs"; how to add a
// target: docs/ci/fuzzing.md).
//
// A target is one function, `Verdict one(std::span<const u8>)`, that hands the bytes to a reader's
// memory entry and checks the reader's contract. It runs two ways: under CTest as a doctest case in
// the reader's own test executable, fed by the deterministic mutators below (fuzz_runner.h), and,
// under the `linux-clang-fuzz` preset, wrapped as `LLVMFuzzerTestOneInput` (fuzz_target.h). This
// header is the part both share, so it includes no doctest.
//
// **Every input is a function of (target, seed, iteration) and of nothing else.** The generator is
// SplitMix64, written here, and every choice is made from it by integer arithmetic: no standard
// library distribution (the standard leaves their algorithms to each library, so the same seed
// would give other bytes on another compiler), no clock, no thread, no address. So an input a run
// names by its target, seed and iteration is the same bytes on MSVC, GCC and Clang, and the
// harness's own tests pin a hash of the first thousand.
//
// Header-only, like the rest of tests/support. It needs `engine::json` (the JSON layer parses a
// seed and writes it back with the engine's own writer); `engine_module_fuzz()` links it.

#include <core/base/types.h>
#include <core/json/json.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::test::fuzz {

using Bytes = std::vector<u8>;

// What a target says about one input. `ok` false is a broken contract: the reader refused without
// a message, left a partial output behind a refusal, or accepted bytes it cannot read back from its
// own writing. `why` is one line, a sentence a finding report can quote.
struct Verdict {
  bool ok = true;
  std::string why;

  static Verdict broken(std::string reason) {
    Verdict v;
    v.ok = false;
    v.why = std::move(reason);
    return v;
  }
};

using TargetFn = Verdict (*)(std::span<const u8> input);

// A target and its budget. Counts, never a clock, are what is committed: the in-tree run is
// `byte_iterations + json_iterations` inputs a seed, every one of them a function of its index.
struct TargetSpec {
  const char* name = "";  // the target's name: its doctest case is "fuzz: <name>"
  TargetFn one = nullptr;
  usize max_len = 64 * 1024;  // a mutated input is cut to this many bytes
  u32 byte_iterations = 512;  // inputs a seed from the byte layer; iteration 0 is the seed itself
  u32 json_iterations = 0;    // and then from the JSON layer, for a seed that parses
  // The allocation cap of one input against the engine allocator's counters: `cap_per_byte` times
  // the input's size plus `cap_floor` (fuzz_runner.h, "AllocationWatch").
  u64 cap_per_byte = 64;
  u64 cap_floor = u64{1} << 20;
  // Seconds one input may run before the watchdog names it as hung and ends the process
  // (fuzz_runner.h, "Watchdog"). Only the harness's own planted hang lowers it.
  u32 watchdog_s = 30;
  // Seeds that ride in the source, named `builtin-<n>`: a target's own fixed inputs, and what a run
  // has when its `content/fuzz/<name>/` directory is not there (a test bundle that does not carry
  // it). Read before the seed files.
  std::span<const std::string_view> builtin_seeds{};
  // Whether `content/fuzz/<name>/` holds this target's seeds, regressions and known failures. A
  // target of the harness's own tests (the planted one) has none.
  bool has_content = true;
};

struct Seed {
  std::string name;
  Bytes bytes;
  u64 hash = 0;
};

// --- hashing and the generator --------------------------------------------------------------

inline constexpr u64 k_fnv_offset = 0xcbf29ce484222325ull;

inline u64 fnv1a(std::span<const u8> bytes, u64 h = k_fnv_offset) {
  for (const u8 b : bytes) {
    h ^= b;
    h *= 0x100000001b3ull;
  }
  return h;
}

inline u64 fnv1a(std::string_view text, u64 h = k_fnv_offset) {
  return fnv1a(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), h);
}

// One input's contribution to a hash over a sequence of inputs: its length, then its bytes, so two
// sequences that only regroup the same bytes do not collide.
inline u64 hash_input(std::span<const u8> input, u64 h) {
  u8 length[8];
  const u64 n = input.size();
  for (u32 i = 0; i < 8; ++i)
    length[i] = static_cast<u8>(n >> (8 * i));
  return fnv1a(input, fnv1a(std::span<const u8>(length, 8), h));
}

// SplitMix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number generators", OOPSLA
// 2014; the constants are the reference implementation's). Seeded with 0 its first output is
// 0xe220a8397b1dcdaf, which the harness's tests pin.
inline u64 splitmix64(u64& state) {
  state += 0x9e3779b97f4a7c15ull;
  u64 z = state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

class Rng {
 public:
  explicit Rng(u64 seed) : state_(seed) {}

  u64 next() { return splitmix64(state_); }

  // Uniform in [0, n) for n > 0, by rejection: the draws below 2^64 mod n are thrown away, so no
  // value is likelier than another and nothing depends on a library's distribution.
  u64 below(u64 n) {
    if (n <= 1) return 0;
    const u64 threshold = (u64{0} - n) % n;
    for (;;) {
      const u64 r = next();
      if (r >= threshold) return r % n;
    }
  }
  usize index(usize n) { return static_cast<usize>(below(static_cast<u64>(n))); }
  bool one_in(u64 n) { return below(n) == 0; }

 private:
  u64 state_;
};

// The generator's state for one input: target, seed and iteration, mixed. Nothing else reaches it.
inline u64 input_state(std::string_view target, u64 seed_hash, u64 iteration) {
  u64 s = fnv1a(target);
  s ^= seed_hash;
  (void)splitmix64(s);
  s ^= iteration;
  return splitmix64(s);
}

inline Seed make_seed(std::string name, std::string_view text) {
  Seed seed;
  seed.name = std::move(name);
  seed.bytes.assign(reinterpret_cast<const u8*>(text.data()),
                    reinterpret_cast<const u8*>(text.data()) + text.size());
  seed.hash = fnv1a(seed.bytes);
  return seed;
}

// --- the byte layer -------------------------------------------------------------------------

// The values that turned up every wrap the design's reading found (fuzzing-design.md, "1. The
// inventory"): 0, 1, -1, 2^15, 2^16-1, 2^31, 2^32-1, 2^53, 2^62, 2^63-1.
inline constexpr u64 k_interesting[] = {
    0ull,           1ull,        ~0ull,      u64{1} << 15, 0xffffull, u64{1} << 31, 0xffffffffull,
    u64{1} << 53, u64{1} << 62, 0x7fffffffffffffffull,
};
inline constexpr usize k_interesting_count = sizeof(k_interesting) / sizeof(k_interesting[0]);

namespace detail {

inline void insert_bytes(Bytes& b, usize at, std::span<const u8> what) {
  b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), what.begin(), what.end());
}

inline void erase_bytes(Bytes& b, usize at, usize count) {
  b.erase(b.begin() + static_cast<std::ptrdiff_t>(at),
          b.begin() + static_cast<std::ptrdiff_t>(at + count));
}

}  // namespace detail

// One to three of: flip a bit; insert up to eight random bytes; delete up to sixteen; splice with
// another seed (this one's head, the other's tail); write an interesting integer, little-endian, in
// 1, 2, 4 or 8 bytes at an offset aligned to its width; insert one as decimal text (a text reader's
// way to meet it); duplicate a range of up to 64 bytes somewhere else. Cut to `max_len` at the end.
inline void mutate_bytes(Bytes& b, Rng& rng, std::span<const Seed> seeds, usize max_len) {
  const u64 count = 1 + rng.below(3);
  for (u64 m = 0; m < count; ++m) {
    switch (rng.below(7)) {
      case 0:  // flip a bit
        if (!b.empty()) {
          const usize at = rng.index(b.size());
          b[at] = static_cast<u8>(b[at] ^ (1u << rng.below(8)));
        }
        break;
      case 1: {  // insert random bytes
        u8 fresh[8];
        const usize n = 1 + rng.index(8);
        for (usize i = 0; i < n; ++i)
          fresh[i] = static_cast<u8>(rng.next());
        detail::insert_bytes(b, rng.index(b.size() + 1), std::span<const u8>(fresh, n));
        break;
      }
      case 2:  // delete a range
        if (!b.empty()) {
          const usize at = rng.index(b.size());
          detail::erase_bytes(b, at, 1 + rng.index(std::min<usize>(b.size() - at, 16)));
        }
        break;
      case 3: {  // splice: this input's head, another seed's tail
        if (seeds.empty()) break;
        const Seed& other = seeds[rng.index(seeds.size())];
        const usize cut = rng.index(b.size() + 1);
        const usize from = rng.index(other.bytes.size() + 1);
        b.resize(cut);
        b.insert(b.end(), other.bytes.begin() + static_cast<std::ptrdiff_t>(from),
                 other.bytes.end());
        break;
      }
      case 4: {  // an interesting integer in binary, at an aligned offset
        const usize width = usize{1} << rng.below(4);
        if (b.size() < width) break;
        u64 value = k_interesting[rng.index(k_interesting_count)];
        if (rng.one_in(2)) value = u64{0} - value;  // and its two's-complement negation
        const usize at = rng.index(b.size() / width) * width;
        for (usize i = 0; i < width; ++i)
          b[at + i] = static_cast<u8>(value >> (8 * i));
        break;
      }
      case 5: {  // an interesting integer as decimal text
        char text[24];
        const u64 value = k_interesting[rng.index(k_interesting_count)];
        std::to_chars_result r{};
        if (rng.one_in(2) && value != 0 && value <= (u64{1} << 63)) {
          // The negative of a value up to 2^63, in i64 arithmetic that cannot overflow.
          const i64 negative =
              value == (u64{1} << 63) ? i64{INT64_MIN} : -static_cast<i64>(value);
          r = std::to_chars(text, text + sizeof(text), negative);
        } else {
          r = std::to_chars(text, text + sizeof(text), value);
        }
        const usize n = static_cast<usize>(r.ptr - text);
        detail::insert_bytes(b, rng.index(b.size() + 1),
                             std::span<const u8>(reinterpret_cast<const u8*>(text), n));
        break;
      }
      case 6: {  // duplicate a range
        if (b.empty()) break;
        const usize at = rng.index(b.size());
        const usize n = 1 + rng.index(std::min<usize>(b.size() - at, 64));
        const Bytes chunk(b.begin() + static_cast<std::ptrdiff_t>(at),
                          b.begin() + static_cast<std::ptrdiff_t>(at + n));
        detail::insert_bytes(b, rng.index(b.size() + 1), chunk);
        break;
      }
      default: break;
    }
  }
  if (b.size() > max_len) b.resize(max_len);
}

// --- the JSON layer -------------------------------------------------------------------------

// Strings a reader of names and paths must refuse or survive: invalid UTF-8 (a lone continuation
// byte's lead, 0xFF, an overlong '/', a UTF-16 surrogate written as UTF-8), a NUL, the parent
// directory, an absolute path on each system, a share, Windows device names and a name ending in a
// dot or a space (fuzzing-design.md, "2. The harness: both, with different jobs", item 2).
inline constexpr std::string_view k_hostile_strings[] = {
    std::string_view("\xC3\x28", 2),
    std::string_view("\xFF", 1),
    std::string_view("\xC0\xAF", 2),
    std::string_view("\xED\xA0\x80", 3),
    std::string_view("a\0b", 3),
    "..",
    "../../outside.json",
    "/etc/passwd",
    "C:\\Windows\\win.ini",
    "\\\\server\\share\\file",
    "CON",
    "NUL",
    "aux.txt",
    "name.",
    "name ",
    "%2e%2e%2f",
};
inline constexpr usize k_hostile_string_count =
    sizeof(k_hostile_strings) / sizeof(k_hostile_strings[0]);

// The parser's default depth budget (`JsonParseOptions::max_depth`), which the deepening mutation
// reaches and passes by one.
inline constexpr u32 k_json_depth_cap = JsonParseOptions{}.max_depth;

namespace detail {

struct JsonNode {
  JsonValue* value = nullptr;
  u32 depth = 0;
};

inline void collect_nodes(JsonValue& v, u32 depth, std::vector<JsonNode>& out) {
  out.push_back({&v, depth});
  if (v.is_array()) {
    JsonValue::Array& items = v.as_array();
    for (u32 i = 0; i < items.size(); ++i)
      collect_nodes(items[i], depth + 1, out);
  } else if (v.is_object()) {
    for (auto member : v.as_object())
      collect_nodes(member.second, depth + 1, out);
  }
}

inline u32 json_height(const JsonValue& v) {
  u32 h = 0;
  if (v.is_array()) {
    const JsonValue::Array& items = v.as_array();
    for (u32 i = 0; i < items.size(); ++i)
      h = std::max(h, 1 + json_height(items[i]));
  } else if (v.is_object()) {
    for (auto member : v.as_object())
      h = std::max(h, 1 + json_height(member.second));
  }
  return h;
}

inline JsonValue interesting_number(Rng& rng) {
  switch (rng.below(17)) {
    case 0: return JsonValue(i64{0});
    case 1: return JsonValue(i64{1});
    case 2: return JsonValue(i64{-1});
    case 3: return JsonValue(i64{1} << 15);
    case 4: return JsonValue(i64{0xffff});
    case 5: return JsonValue(i64{1} << 31);
    case 6: return JsonValue(i64{0xffffffff});
    case 7: return JsonValue(i64{1} << 53);
    case 8: return JsonValue(i64{1} << 62);
    case 9: return JsonValue(i64{INT64_MAX});
    case 10: return JsonValue(i64{INT64_MIN});
    case 11: return JsonValue(~u64{0});
    case 12: return JsonValue(1e39);  // past FLT_MAX: what an f32 field must refuse
    case 13: return JsonValue(-1e39);
    case 14: return JsonValue(1.7976931348623157e308);
    case 15: return JsonValue(5e-324);
    default: return JsonValue(-0.0);
  }
}

inline JsonValue other_kind(const JsonValue& v, Rng& rng) {
  for (;;) {
    JsonValue out;
    switch (rng.below(6)) {
      case 0: out = JsonValue(); break;
      case 1: out = JsonValue(true); break;
      case 2: out = JsonValue(i64{0}); break;
      case 3: out = JsonValue(std::string_view("")); break;
      case 4: out = JsonValue::array(); break;
      default: out = JsonValue::object(); break;
    }
    if (out.kind() != v.kind()) return out;
  }
}

inline std::string hostile_string(Rng& rng) {
  const usize pick = rng.index(k_hostile_string_count + 1);
  if (pick == k_hostile_string_count) return std::string(4096, 'a');  // a long one
  return std::string(k_hostile_strings[pick]);
}

// How an object is written when the mutation is about its members' order: the engine's writer
// always sorts keys and cannot repeat one, so the reordering and duplicating mutations write the
// one object they chose themselves, and every scalar and every other container through the
// engine's own writer.
enum class ObjectOrder : u8 { Sorted, Reversed, Duplicated };

inline void write_compact(const JsonValue& v, std::string& out, const JsonValue* special,
                          ObjectOrder order, u64 pick) {
  if (v.is_array()) {
    out.push_back('[');
    const JsonValue::Array& items = v.as_array();
    for (u32 i = 0; i < items.size(); ++i) {
      if (i > 0) out.push_back(',');
      write_compact(items[i], out, special, order, pick);
    }
    out.push_back(']');
    return;
  }
  if (v.is_object()) {
    std::vector<std::pair<std::string_view, const JsonValue*>> members;
    for (auto member : v.as_object())
      members.emplace_back(std::string_view(member.first), &member.second);
    if (&v == special && !members.empty()) {
      if (order == ObjectOrder::Reversed) {
        std::reverse(members.begin(), members.end());
      } else if (order == ObjectOrder::Duplicated) {
        const auto again = members[static_cast<usize>(pick % members.size())];
        const usize at = static_cast<usize>((pick / 7) % (members.size() + 1));
        members.insert(members.begin() + static_cast<std::ptrdiff_t>(at), again);
      }
    }
    out.push_back('{');
    for (usize i = 0; i < members.size(); ++i) {
      if (i > 0) out.push_back(',');
      write_json(JsonValue(members[i].first), out, JsonWriteOptions{false});
      out.push_back(':');
      write_compact(*members[i].second, out, special, order, pick);
    }
    out.push_back('}');
    return;
  }
  write_json(v, out, JsonWriteOptions{false});
}

}  // namespace detail

// One or two of, on the parsed seed: replace a value with an interesting number; change a value's
// kind; drop an object's key or an array's element; replace a string or a key with a hostile one;
// wrap a value in arrays to the parser's depth cap or one past it; repeat an array's element a
// thousand times (fewer when a thousand would pass `max_len`); write an object's keys in reverse
// order (the parser's sorted insert at its slowest) or with one key twice. Written back by the
// engine's writer, pretty or compact, or by `detail::write_compact` when the order is the point.
// False when the seed is not JSON, and the caller falls back to the byte layer.
inline bool mutate_json(const Seed& seed, Rng& rng, usize max_len, Bytes& out) {
  JsonValue doc;
  const std::string_view text(reinterpret_cast<const char*>(seed.bytes.data()), seed.bytes.size());
  if (!parse_json(text, doc).ok) return false;

  const JsonValue* special = nullptr;
  detail::ObjectOrder order = detail::ObjectOrder::Sorted;
  u64 order_pick = 0;
  const u64 count = 1 + rng.below(2);
  for (u64 m = 0; m < count; ++m) {
    std::vector<detail::JsonNode> nodes;
    detail::collect_nodes(doc, 0, nodes);
    const detail::JsonNode node = nodes[rng.index(nodes.size())];
    JsonValue& v = *node.value;
    switch (rng.below(8)) {
      case 0: v = detail::interesting_number(rng); break;
      case 1: v = detail::other_kind(v, rng); break;
      case 2:  // drop a member or an element
        if (v.is_object() && v.size() > 0) {
          const usize drop = rng.index(v.size());
          usize n = 0;
          std::string key;
          for (auto member : v.as_object()) {
            if (n++ == drop) {
              key = member.first;
              break;
            }
          }
          v.as_object().erase(key);
        } else if (v.is_array() && v.size() > 0) {
          const usize drop = rng.index(v.size());
          JsonValue::Array& items = v.as_array();
          JsonValue::Array kept;
          for (u32 i = 0; i < items.size(); ++i) {
            if (i != drop) kept.push_back(std::move(items[i]));
          }
          v = JsonValue(std::move(kept));
        }
        break;
      case 3:  // a hostile string, as a value or as a key
        if (v.is_object() && v.size() > 0 && rng.one_in(2)) {
          const usize rename = rng.index(v.size());
          usize n = 0;
          std::string key;
          for (auto member : v.as_object()) {
            if (n++ == rename) {
              key = member.first;
              break;
            }
          }
          JsonValue moved = std::move(*v.find(key));
          v.as_object().erase(key);
          v.set(detail::hostile_string(rng), std::move(moved));
        } else {
          v = JsonValue(detail::hostile_string(rng));
        }
        break;
      case 4: {  // deepen to the cap, or one past it
        const u32 used = node.depth + detail::json_height(v);
        const u32 target = k_json_depth_cap + static_cast<u32>(rng.below(2));
        for (u32 level = used; level < target; ++level) {
          JsonValue wrapped = JsonValue::array();
          wrapped.push_back(std::move(v));
          v = std::move(wrapped);
        }
        break;
      }
      case 5: {  // repeat an element
        JsonValue element = v.is_array() && v.size() > 0 ? v[rng.index(v.size())] : v;
        const usize each = write_json(element, JsonWriteOptions{false}).size() + 1;
        const usize times = std::min<usize>(1000, max_len / each);
        JsonValue repeated = JsonValue::array();
        for (usize i = 0; i < times; ++i)
          repeated.push_back(element);
        v = std::move(repeated);
        break;
      }
      case 6:  // keys in reverse order
        if (v.is_object()) {
          special = &v;
          order = detail::ObjectOrder::Reversed;
        }
        break;
      default:  // one key twice
        if (v.is_object()) {
          special = &v;
          order = detail::ObjectOrder::Duplicated;
          order_pick = rng.next();
        }
        break;
    }
    // A later mutation may move the object `special` points at; the order mutation is kept only
    // when it is the last one.
    if (m + 1 < count) special = nullptr;
  }

  std::string written;
  if (special != nullptr) {
    detail::write_compact(doc, written, special, order, order_pick);
  } else {
    write_json(doc, written, JsonWriteOptions{rng.one_in(2)});
  }
  if (written.size() > max_len) written.resize(max_len);
  out.assign(written.begin(), written.end());
  return true;
}

// --- one input ------------------------------------------------------------------------------

// The input at `iteration` of seed `seed_index`: iteration 0 is the seed itself; then
// `byte_iterations - 1` from the byte layer; then `json_iterations` from the JSON layer, each
// falling back to the byte layer for a seed that is not JSON.
inline Bytes generate(const TargetSpec& spec, std::span<const Seed> seeds, usize seed_index,
                      u32 iteration) {
  const Seed& seed = seeds[seed_index];
  Bytes input = seed.bytes;
  if (iteration == 0) return input;
  Rng rng(input_state(spec.name, seed.hash, iteration));
  if (iteration >= spec.byte_iterations && mutate_json(seed, rng, spec.max_len, input)) {
    return input;
  }
  mutate_bytes(input, rng, seeds, spec.max_len);
  return input;
}

inline u32 iterations_per_seed(const TargetSpec& spec) {
  return spec.byte_iterations + spec.json_iterations;
}

}  // namespace engine::test::fuzz
