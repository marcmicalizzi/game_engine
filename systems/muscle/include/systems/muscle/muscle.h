#pragma once

// systems/muscle: the line tier of the engine's muscles (ADR-0027, ADR-0055;
// docs/subsystems/muscle.md; docs/plan/05-simulation.md §5.16).
//
// A muscle here is a Hill-type line actuator on a routed path: activation dynamics, a fiber with
// active, passive and damping force, and a compliant tendon in series at a fixed pennation height
// (law.h). Tier 0 integrates it at the scheduler's 60 Hz tick in eight parents of 1/480 s with
// Alexander's SDIRK3 (scheme.h), a located slack/taut crossing per parent, and a whole-tick commit
// or rollback (schedule.h). The authoring track qualified the law and the scheme offline under
// decisions D20-D23; those decisions are the contract, and the qualification corpus under
// content/muscle/ is replayed against the archived oracles by the tests.
//
// Stage 1 (roadmap C53) is the kernel: the law, the scheme, the schedule, the ledger, static
// initialisation, the validator, the schema, a batch API over the capability's own SoA store. It
// owns no entity and runs from no scheduler. Stage 2 (C54) adds the supervisor (tier degradation,
// the rigid and inert tiers, freeze accounting, promotion), the scheduler entry, document records
// and the path's source in the skeleton.
//
// Scaffolded by tools/new-capability.ps1; ENGINE_WITH_MUSCLE=OFF (or ENGINE_MINIMAL=ON) removes
// it. The registration points (ADR-0027 decision 2):
//
//   [x] schema types      schemas/muscle.schema, engine_schema_library
//   [ ] scheduler entry   stage 2: the kernel is a batch API the test drives (see the docs page)
//   [ ] render passes     none: a muscle draws nothing
//   [ ] derived data      none yet; the corpus is test data, not content the build derives
//   [ ] protocol methods  none
//   [ ] tunables          none: the schedule is fixed by D20-D23, not tuned at run time
//   [x] LOD policy        tier 0 only in stage 1; the tiers are the supervisor's (stage 2)
//   [x] determinism       k_determinism below; core/math/deterministic.h for the transcendentals
//   [x] zero cost unused  an empty store runs nothing; no other module names this one
//   [x] docs, tests, size table
//   [x] bench             bench/muscle_bench.cpp

#include <systems/muscle/law.h>
#include <systems/muscle/scheme.h>

#include <core/base/types.h>
#include <core/math/deterministic.h>

#include <schemas/muscle.h>

#include <array>
#include <string>
#include <string_view>

namespace engine::muscle {

/// Tick phase the stage-2 system will run in (plan 05 §5.2).
inline constexpr const char* k_phase = "systems";

/// Determinism stance (ADR-0010): "hashed". The committed state is integration state that the
/// next tick reads back, and it is the same bytes on MSVC, GCC and Clang: the law calls only
/// `engine::det` transcendentals, the tree compiles with contraction off (ADR-0035), and no
/// decision depends on anything but the inputs' bits. The tests pin a hash of the corpus's final
/// states that every toolchain reproduces.
inline constexpr const char* k_determinism = "hashed";

/// LOD tiers integrated, one bit per tier: tier 0 only in stage 1.
inline constexpr u32 k_lod_tiers = 0b0001u;

using Sha256 = std::array<u8, 32>;

constexpr u8 hex_nibble(char c) noexcept {
  return c >= '0' && c <= '9' ? static_cast<u8>(c - '0')
         : c >= 'a' && c <= 'f' ? static_cast<u8>(c - 'a' + 10)
         : c >= 'A' && c <= 'F' ? static_cast<u8>(c - 'A' + 10)
                                : static_cast<u8>(0xff);
}

constexpr Sha256 sha256_literal(const char (&hex)[65]) noexcept {
  Sha256 out{};
  for (usize i = 0; i < 32; ++i)
    out[i] = static_cast<u8>((hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
  return out;
}

/// Parses 64 hex digits; false for anything else.
bool parse_sha256(std::string_view hex, Sha256& out);
/// 64 lowercase hex digits.
std::string to_hex(const Sha256& value);

/// The engine's law identity: the SHA-256 of the canonical text of the law as this module
/// computes it, `systems/muscle/tests/model.txt`, one formula or constant per line. Computed once
/// with `Get-FileHash -Algorithm SHA256` (the engine has no SHA-256 outside the tissue capability,
/// and recomputes no identity in stage 1). Provenance, which tests/law_tests.cpp re-reads:
/// model.txt, 1983 bytes, SHA-256 0238cd3049a25304523d516e6000da980d12180c77549f92b7ef1c6bc4e5cd9c
inline constexpr Sha256 k_model_sha256 =
    sha256_literal("0238cd3049a25304523d516e6000da980d12180c77549f92b7ef1c6bc4e5cd9c");
inline constexpr usize k_model_text_bytes = 1983;

/// The reference law this one implements: the authoring track's r005 model identity, as r010's
/// METHOD.json records it (`model_sha256`).
inline constexpr Sha256 k_reference_model_sha256 =
    sha256_literal("3096e5ff70d2d2ff7156b29ec409ec4db917ffb427d3e4a6f833f86eb5ac5288");

// ---- paths -----------------------------------------------------------------------------------------
//
// The musculotendon path is a template parameter of the tick, never an indirect call per sample.
// A path has `void sample(double t, double tick_start, double& length, double& rate) const`, with
// t absolute and tick_start the tick's first instant, (k - 1)/60.

/// The runtime's input: a cubic Hermite over the tick from (length, rate) at its start to its end.
/// The skeleton supplies the endpoints in stage 2.
struct HermitePath {
  double l0;  // m, at the tick's start
  double v0;  // m/s
  double l1;  // m, at the tick's end
  double v1;  // m/s

  static constexpr double k_duration = 1.0 / 60.0;

  void sample(double t, double tick_start, double& length, double& rate) const noexcept {
    const double s = (t - tick_start) / k_duration;
    const double s2 = s * s;
    const double s3 = s2 * s;
    const double h00 = 2.0 * s3 - 3.0 * s2 + 1.0;
    const double h10 = s3 - 2.0 * s2 + s;
    const double h01 = -2.0 * s3 + 3.0 * s2;
    const double h11 = s3 - s2;
    length = h00 * l0 + h10 * k_duration * v0 + h01 * l1 + h11 * k_duration * v1;
    const double d00 = 6.0 * s2 - 6.0 * s;
    const double d10 = 3.0 * s2 - 4.0 * s + 1.0;
    const double d01 = -6.0 * s2 + 6.0 * s;
    const double d11 = 3.0 * s2 - 2.0 * s;
    rate = (d00 * l0 + d10 * k_duration * v0 + d01 * l1 + d11 * k_duration * v1) / k_duration;
  }
};

/// The corpus's paths, in absolute time (the reference's `path_value`): a constant length, a
/// linear one, or a sine about a centre.
struct AnalyticPath {
  enum class Kind : u8 { Constant, Linear, Sine };
  Kind kind = Kind::Constant;
  double base = 0.0;       // constant: the length; linear: the origin; sine: the centre (m)
  double slope = 0.0;      // linear: the rate (m/s); sine: the amplitude (m)
  double omega = 0.0;      // sine: rad/s
  double rate_amplitude = 0.0;  // sine: the rate's amplitude (m/s), as the corpus states it

  void sample(double t, double, double& length, double& rate) const noexcept {
    switch (kind) {
      case Kind::Constant:
        length = base;
        rate = 0.0;
        return;
      case Kind::Linear:
        length = base + slope * t;
        rate = slope;
        return;
      case Kind::Sine:
        length = base + slope * det::sin(omega * t);
        rate = rate_amplitude * det::cos(omega * t);
        return;
    }
  }
};

}  // namespace engine::muscle
