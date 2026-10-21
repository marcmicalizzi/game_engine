#pragma once

// The capability's draws (internal): splitmix64 as the flora kit's (Steele, Lea and Flood 2014),
// integer arithmetic on the seed with each float its top 53 bits scaled by a power of two, so a
// draw is exact on every target; one independent stream per purpose.

#include <core/base/types.h>
#include <domain/cover/host.h>

#include <cmath>

namespace engine::cover {

struct Rng {
  u64 state;
  explicit Rng(u64 seed) : state(seed) {}
  u64 next() {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  f64 unit() { return static_cast<f64>(next() >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)
  f64 symmetric() { return unit() * 2.0 - 1.0; }                                      // [-1, 1)
  // A unit vector, uniform over the sphere: rejection from the cube, at most 16 tries (then +y).
  P3 direction() {
    for (u32 i = 0; i < 16; ++i) {
      const P3 v{symmetric(), symmetric(), symmetric()};
      const f64 l2 = dot(v, v);
      if (l2 > 1e-6 && l2 <= 1.0) return v * (1.0 / std::sqrt(l2));
    }
    return P3{0.0, 1.0, 0.0};
  }
};

inline Rng stream_of(u64 seed, u64 stream) {
  Rng mix(seed ^ (stream * 0xD1B54A32D192ED03ull));
  return Rng(mix.next());
}

}  // namespace engine::cover
