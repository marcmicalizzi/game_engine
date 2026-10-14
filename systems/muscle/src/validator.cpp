#include <systems/muscle/validator.h>

#include <bit>
#include <cmath>

namespace engine::muscle {

namespace {

// The 17 distinct parameter identities of content/muscle/corpus.json's 39 rows, in first-appearance
// order, with the series parameters each was qualified with (from each identity's first case file).
// tests/validator_tests.cpp holds this table to the corpus.
constexpr QualifiedSet k_synthetic_v1[] = {
    {sha256_literal("9665d9f3c53dfae5861a5318536d6625b973eaed432fd67c533d4627cd5c1ed6"), 0.05,
     0.049, 0.2, 0.1, 0.2},  // step ...
    {sha256_literal("d3bf5436c95281683d7abcedbaf49690399f0c302cba3965dfdb2e57aec1a4e4"), 0.05, 0.02,
     0.0, 0.02, 0.2},  // corner_00
    {sha256_literal("edf5f738e577d34beed3900133b41e1cd449bbd2b6ebbbdf47af5d56225af16d"), 0.2, 0.02,
     0.0, 0.02, 0.2},  // corner_01
    {sha256_literal("ea8a3aa84b83e02c2f9d4ccebf3a17a9ef90d895f527e10666f5e6ca5dc589eb"), 0.05, 0.02,
     0.4, 0.02, 0.2},  // corner_02
    {sha256_literal("9d1fc0131a7d452686cbfaf8a96715fc57ff1ca6e69f0828b0e1038428e3d56f"), 0.2, 0.02,
     0.4, 0.02, 0.2},  // corner_03
    {sha256_literal("acabf8c9d2878749daf9353ff2c70bbd52794b3e719ea07fa3cdf13277526d55"), 0.05, 0.02,
     0.0, 0.2, 0.2},  // corner_04
    {sha256_literal("94c1d59f7634eb22d11035119fadad544c9c9d70cf4f644d273217cc297c036a"), 0.2, 0.02,
     0.0, 0.2, 0.2},  // corner_05
    {sha256_literal("e843c9e6568a8fb13f26d432dd19578dd24603b01da2479d652ef6aa931609ef"), 0.05, 0.02,
     0.4, 0.2, 0.2},  // corner_06
    {sha256_literal("fe85c70806b87072b918f9de70df69d660aea559f37bf1c1723bd72c39eadb25"), 0.2, 0.02,
     0.4, 0.2, 0.2},  // corner_07
    {sha256_literal("85eea457fa77401b9d34a2d285d8423212a6a76d836bc764cb81e9114c98a36d"), 0.05, 0.1,
     0.0, 0.02, 0.2},  // corner_08
    {sha256_literal("a73a8fdf12ee1473aa185644444c1c318075bf301c7bb0c90c5037970352399b"), 0.2, 0.1,
     0.0, 0.02, 0.2},  // corner_09
    {sha256_literal("bd1cb2c5fb384d3f15bb30447c24cf174bbcf841da828d0cefd2224552e7fdd9"), 0.05, 0.1,
     0.4, 0.02, 0.2},  // corner_10
    {sha256_literal("4b0c5b646f669eba796592e54b510c1dff8630d278a5b42f1067a92ed0856285"), 0.2, 0.1,
     0.4, 0.02, 0.2},  // corner_11
    {sha256_literal("a32cf7c1d8e31bad7a482e7ef8a77969f1e51c69be144ff78508df8f605fd87c"), 0.05, 0.1,
     0.0, 0.2, 0.2},  // corner_12
    {sha256_literal("2d9e982b69bc00c801889e163828e26630700ac6b52029d7a7adad4bf0040158"), 0.2, 0.1,
     0.0, 0.2, 0.2},  // corner_13
    {sha256_literal("271f4c092287d52427c59811add339ce7252ebfbe4ff2b1acb1dc7c60c0278dc"), 0.05, 0.1,
     0.4, 0.2, 0.2},  // corner_14
    {sha256_literal("c8d784ea9f5e015c845719431dec1bc595d2d1bc603b3cf36ae9be3e5491e564"), 0.2, 0.1,
     0.4, 0.2, 0.2},  // corner_15
};

constexpr QualifiedDomain k_domain{"synthetic-study-only", 1,
                                   std::span<const QualifiedSet>(k_synthetic_v1)};

ValidationResult refuse(Refusal r, const char* field, double value, const char* unit,
                        const char* rule) {
  return ValidationResult{r, field, value, unit, rule};
}

bool same(double a, double b) { return std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }

}  // namespace

const char* refusal_code(Refusal r) noexcept {
  switch (r) {
    case Refusal::None: return "ok";
    case Refusal::ModelUnrecognized: return "model_unrecognized";
    case Refusal::DomainUnqualified: return "domain_unqualified";
    case Refusal::ParameterOutsideDomain: return "parameter_outside_domain";
  }
  return "unknown";
}

const QualifiedDomain& qualified_domain() noexcept { return k_domain; }

ValidationResult validate_law_domain(const FiberParameters& p, const SeriesParameters& s) noexcept {
  struct Positive {
    const char* field;
    double value;
    const char* unit;
  };
  const Positive fiber[] = {
      {"fiber.force_scale", p.force_scale, "N"},
      {"fiber.optimal_fiber_length", p.optimal_fiber_length, "m"},
      {"fiber.rigid_tendon_length", p.rigid_tendon_length, "m"},
      {"fiber.vmax_lopt_per_s", p.vmax_lopt_per_s, "lopt/s"},
      {"fiber.tau_activation", p.tau_activation, "s"},
      {"fiber.tau_deactivation", p.tau_deactivation, "s"},
      {"fiber.activation_smoothing", p.activation_smoothing, "1"},
      {"fiber.passive_strain_at_unit_force", p.passive_strain_at_unit_force, "ratio"},
      {"fiber.passive_shape", p.passive_shape, "1"},
      {"fiber.passive_zero_normalized_length", p.passive_zero_normalized_length, "ratio"},
      {"fiber.min_normalized_length", p.min_normalized_length, "ratio"},
      {"fiber.max_normalized_length", p.max_normalized_length, "ratio"},
      {"fiber.reference_area_m2", p.reference_area_m2, "m^2"},
      {"fiber.reference_fiber_length", p.reference_fiber_length, "m"},
  };
  for (const Positive& x : fiber) {
    if (!std::isfinite(x.value) || x.value <= 0.0)
      return refuse(Refusal::ParameterOutsideDomain, x.field, x.value, x.unit,
                    "finite and positive");
  }
  if (!(p.passive_zero_normalized_length < p.min_normalized_length))
    return refuse(Refusal::ParameterOutsideDomain, "fiber.min_normalized_length",
                  p.min_normalized_length, "ratio", "above passive_zero_normalized_length");
  if (!(p.min_normalized_length < p.max_normalized_length))
    return refuse(Refusal::ParameterOutsideDomain, "fiber.max_normalized_length",
                  p.max_normalized_length, "ratio", "above min_normalized_length");
  const Positive series[] = {
      {"series.tendon_slack_length", s.tendon_slack_length, "m"},
      {"series.tendon_strain_at_unit_force", s.tendon_strain_at_unit_force, "ratio"},
      {"series.normalized_damping", s.normalized_damping, "1"},
      {"series.minimum_cosine", s.minimum_cosine, "ratio"},
  };
  for (const Positive& x : series) {
    if (!std::isfinite(x.value) || x.value <= 0.0)
      return refuse(Refusal::ParameterOutsideDomain, x.field, x.value, x.unit,
                    "finite and positive");
  }
  if (!std::isfinite(s.pennation_at_optimal) || !(s.pennation_at_optimal >= 0.0) ||
      !(s.pennation_at_optimal <= 0.4))
    return refuse(Refusal::ParameterOutsideDomain, "series.pennation_at_optimal",
                  s.pennation_at_optimal, "rad", "finite and in [0, 0.4]");
  if (!(s.minimum_cosine < 1.0))
    return refuse(Refusal::ParameterOutsideDomain, "series.minimum_cosine", s.minimum_cosine,
                  "ratio", "below 1");
  return ValidationResult{};
}

ValidationResult validate(const Sha256& model_id, const Sha256& identity,
                          const FiberParameters& fiber, const SeriesParameters& series) noexcept {
  if (model_id != k_model_sha256)
    return refuse(Refusal::ModelUnrecognized, "model_sha256", 0.0, "",
                  "the engine's line-tier law (k_model_sha256)");
  const QualifiedSet* set = nullptr;
  for (const QualifiedSet& q : k_domain.sets) {
    if (q.identity == identity) {
      set = &q;
      break;
    }
  }
  if (set == nullptr)
    return refuse(Refusal::DomainUnqualified, "parameter_sha256", 0.0, "",
                  "an identity of synthetic-study-only version 1");
  const ValidationResult law = validate_law_domain(fiber, series);
  if (!law.ok()) return law;
  // The identity names one parameter set: the reference's default fiber and these series values.
  const FiberParameters d{};
  const double fiber_values[][2] = {
      {fiber.force_scale, d.force_scale},
      {fiber.optimal_fiber_length, d.optimal_fiber_length},
      {fiber.rigid_tendon_length, d.rigid_tendon_length},
      {fiber.vmax_lopt_per_s, d.vmax_lopt_per_s},
      {fiber.tau_activation, d.tau_activation},
      {fiber.tau_deactivation, d.tau_deactivation},
      {fiber.activation_smoothing, d.activation_smoothing},
      {fiber.passive_strain_at_unit_force, d.passive_strain_at_unit_force},
      {fiber.passive_shape, d.passive_shape},
      {fiber.passive_zero_normalized_length, d.passive_zero_normalized_length},
      {fiber.min_normalized_length, d.min_normalized_length},
      {fiber.max_normalized_length, d.max_normalized_length},
      {fiber.reference_area_m2, d.reference_area_m2},
      {fiber.reference_fiber_length, d.reference_fiber_length},
  };
  for (const auto& v : fiber_values) {
    if (!same(v[0], v[1]))
      return refuse(Refusal::DomainUnqualified, "fiber", v[0], "",
                    "the parameters the identity was qualified with");
  }
  const double series_values[][2] = {
      {series.tendon_slack_length, set->slack},   {series.tendon_strain_at_unit_force, set->e1},
      {series.pennation_at_optimal, set->alpha0}, {series.normalized_damping, set->beta},
      {series.minimum_cosine, set->min_cos},
  };
  for (const auto& v : series_values) {
    if (!same(v[0], v[1]))
      return refuse(Refusal::DomainUnqualified, "series", v[0], "",
                    "the parameters the identity was qualified with");
  }
  return ValidationResult{};
}

bool add_validated(LineStore& store, const ActuatorSpec& spec, const Sha256& model_id, u32& index,
                   ValidationResult& result) {
  result = validate(model_id, spec.parameter_sha256, spec.fiber, spec.series);
  if (!result.ok()) return false;
  return store.add(spec, index);
}

}  // namespace engine::muscle
