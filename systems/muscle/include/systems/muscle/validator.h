#pragma once

// The line tier's validator (D21 (3), ADR-0055 decision 4): whether an actuator's law, parameter
// set and parameters are ones the evidence qualifies, refused by name when they are not.
//
//   validate(model_id, parameter_identity, fiber, series):
//     model_unrecognized        the model id is not this law's (k_model_sha256)
//     domain_unqualified        the parameter-set identity is in no qualified domain, or the
//                               parameters are not the nineteen that identity was qualified with
//                               (naming the first field that differs, its value and unit, and the
//                               domain and version the identity was found in)
//     parameter_outside_domain  a value outside the law's own domain, naming the field, the value,
//                               its unit and the rule
//
// The qualified domains are data: content/muscle/corpus.json's `qualified_domains`, written by
// tools/muscle-corpus.ps1 from the source packets content/muscle/sources.json names, and compiled
// in from the table the same script generates (src/qualified_domains.inc, held to the corpus by
// tests/validator_tests.cpp). Today there is one, `synthetic-study-only` version 1: the 17
// distinct parameter identities of the corpus's 39 rows, each with the nineteen values it was
// qualified with. Identities are opaque 32-byte values carried as data; the engine recomputes none
// of them (it has no SHA-256 outside the tissue capability). Since it cannot hash, it checks the
// other way round: an identity names one parameter set, and parameters that differ from it in any
// of the nineteen values are not qualified by it. A named-muscle calibration with provenance
// widens the domains as a new packet in the sources and a regenerated table, not as code.

#include <core/base/types.h>
#include <systems/muscle/muscle.h>
#include <systems/muscle/store.h>

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
  const char* unit = "";  // "rad"
  const char* rule = "";  // "finite and in [0, 0.4]"
  // The qualified domain the identity was found in, for a `domain_unqualified` refusal of its
  // values (and an accepted set); empty and zero when the identity is in none.
  const char* domain = "";  // "synthetic-study-only"
  u32 domain_version = 0;

  bool ok() const noexcept { return refusal == Refusal::None; }
};

/// One qualified parameter set: its identity and the nineteen values it was qualified with.
struct QualifiedSet {
  Sha256 identity;
  FiberParameters fiber;
  SeriesParameters series;
};

struct QualifiedDomain {
  const char* id;
  u32 version;
  std::span<const QualifiedSet> sets;
};

/// Every qualified domain, in content/muscle/corpus.json's order.
std::span<const QualifiedDomain> qualified_domains() noexcept;

/// The domain and set an identity names, or false. An identity is in at most one set of a domain;
/// the first domain that holds it answers.
bool find_qualified(const Sha256& identity, const QualifiedDomain*& domain,
                    const QualifiedSet*& set) noexcept;

/// The law's own domain (the reference's `Parameters` and `SeriesParameters` checks): every fiber
/// value finite and positive, x0 < x_min < x_max; the series lengths, strain, damping and minimum
/// cosine finite and positive, the minimum cosine below one, the pennation in [0, 0.4] rad.
ValidationResult validate_law_domain(const FiberParameters& fiber,
                                     const SeriesParameters& series) noexcept;

/// The validator, in the order the decisions list its refusals.
ValidationResult validate(const Sha256& model_id, const Sha256& parameter_identity,
                          const FiberParameters& fiber, const SeriesParameters& series) noexcept;

/// validate() and then LineStore::add(); an actuator the validator refuses is never added. A full
/// store is reported as `false` with an ok result.
bool add_validated(LineStore& store, const ActuatorSpec& spec, const Sha256& model_id, u32& index,
                   ValidationResult& result);

}  // namespace engine::muscle
