#pragma once

// The line tier's validator (D21 (3), ADR-0055 decision 4): whether an actuator's law, parameter
// set and parameters are ones the evidence qualifies, refused by name when they are not.
//
//   validate(model_id, parameter_identity, fiber, series):
//     model_unrecognized        the model id is not this law's (k_model_sha256)
//     domain_unqualified        the parameter-set identity is not in the qualified domain, or the
//                               parameters are not the ones that identity was qualified with
//     parameter_outside_domain  a value outside the law's own domain, naming the field, the value,
//                               its unit and the rule
//
// The only qualified domain is `synthetic-study-only` version 1: the 17 distinct parameter
// identities of the corpus's 39 rows (content/muscle/corpus.json), each with the parameters it
// was qualified with. Identities are opaque 32-byte values carried as data; the engine recomputes
// none of them (it has no SHA-256 outside the tissue capability). Since it cannot hash, it checks
// the other way round: an identity names one parameter set, and parameters that differ from it are
// not qualified by it. A named-muscle calibration with provenance is what widens the domain, as a
// new domain version.

#include <systems/muscle/muscle.h>
#include <systems/muscle/store.h>

#include <core/base/types.h>

#include <schemas/muscle.h>

#include <span>

namespace engine::muscle {

enum class Refusal : u8 {
  None = 0,
  ModelUnrecognized = 1,       // model_unrecognized
  DomainUnqualified = 2,       // domain_unqualified
  ParameterOutsideDomain = 3,  // parameter_outside_domain
};

/// The refusal's code as the decisions spell it.
const char* refusal_code(Refusal r) noexcept;

struct ValidationResult {
  Refusal refusal = Refusal::None;
  const char* field = "";  // "series.pennation_at_optimal"
  double value = 0.0;
  const char* unit = "";   // "rad"
  const char* rule = "";   // "finite and in [0, 0.4]"

  bool ok() const noexcept { return refusal == Refusal::None; }
};

/// One qualified parameter set: its identity and the series parameters it was qualified with. The
/// fiber parameters of every corpus row are the reference law's defaults (`FiberParameters{}`).
struct QualifiedSet {
  Sha256 identity;
  double slack, e1, alpha0, beta, min_cos;
};

struct QualifiedDomain {
  const char* id;
  u32 version;
  std::span<const QualifiedSet> sets;
};

/// `synthetic-study-only` version 1.
const QualifiedDomain& qualified_domain() noexcept;

/// The law's own domain (the reference's `Parameters` and `SeriesParameters` checks): every fiber
/// value finite and positive, x0 < x_min < x_max; the series lengths, strain, damping and minimum
/// cosine finite and positive, the minimum cosine below one, the pennation in [0, 0.4] rad.
ValidationResult validate_law_domain(const FiberParameters& fiber, const SeriesParameters& series) noexcept;

/// The validator, in the order the decisions list its refusals.
ValidationResult validate(const Sha256& model_id, const Sha256& parameter_identity,
                          const FiberParameters& fiber, const SeriesParameters& series) noexcept;

/// validate() and then LineStore::add(); an actuator the validator refuses is never added. A full
/// store is reported as `false` with an ok result.
bool add_validated(LineStore& store, const ActuatorSpec& spec, const Sha256& model_id, u32& index,
                   ValidationResult& result);

}  // namespace engine::muscle
