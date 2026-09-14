#pragma once

// Id128: the engine's stable identity for authored objects (docs/plan/03-data-model.md §3.1).
//
// Layout: the high 48 bits are a millisecond Unix timestamp so ids sort roughly by creation
// time; the remaining 80 bits are random (or, for deterministic generators, a hash of seed and
// counter). Serialized as 32 lowercase hex characters. The all-zero id is null and never
// generated.

#include <core/base/types.h>
#include <core/hash/hash.h>

#include <compare>
#include <string_view>

namespace engine {

struct Id128 {
  u64 hi = 0;
  u64 lo = 0;

  constexpr bool is_null() const noexcept { return hi == 0 && lo == 0; }
  constexpr explicit operator bool() const noexcept { return !is_null(); }
  constexpr auto operator<=>(const Id128&) const noexcept = default;

  // Milliseconds since the Unix epoch embedded in the id (top 48 bits of hi).
  constexpr u64 timestamp_ms() const noexcept { return hi >> 16; }

  static constexpr usize k_hex_length = 32;

  // Writes 32 hex characters plus a terminating NUL into `out` (33 bytes).
  void to_hex(char* out) const noexcept;
  // Parses exactly 32 hex characters (either case). Returns false on any other input.
  static bool from_hex(std::string_view text, Id128& out) noexcept;

  // Fresh id from the wall clock and the process's random source. Thread-safe.
  static Id128 generate() noexcept;
  // Deterministic id from a seed and counter, for procedural generation and tests. The
  // timestamp field is derived from the seed so results never depend on the clock.
  static Id128 from_seed(u64 seed, u64 counter) noexcept;
  // Id whose bits are the given components, unchecked.
  static constexpr Id128 from_parts(u64 hi, u64 lo) noexcept { return Id128{hi, lo}; }
};

// A deterministic stream of ids: same seed, same sequence, on every machine.
class IdGenerator {
 public:
  explicit IdGenerator(u64 seed) noexcept : seed_(seed) {}
  Id128 next() noexcept { return Id128::from_seed(seed_, counter_++); }
  u64 seed() const noexcept { return seed_; }
  u64 counter() const noexcept { return counter_; }

 private:
  u64 seed_;
  u64 counter_ = 0;
};

template <>
struct Hash<Id128> {
  u64 operator()(const Id128& id) const noexcept { return hash_combine(mix64(id.hi), id.lo); }
};

}  // namespace engine
