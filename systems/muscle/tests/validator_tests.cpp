// The validator (validator.h): the qualified domain is the corpus's, and every refusal is by name.
#include <systems/muscle/validator.h>

#include <core/json/json.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace engine;
using namespace engine::muscle;

namespace {

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

double num(const JsonValue& v, const char* key) {
  double out = 0.0;
  REQUIRE(v.find(key) != nullptr);
  REQUIRE(v.find(key)->get_f64(out));
  return out;
}

const QualifiedSet& first_set() { return qualified_domain().sets[0]; }

SeriesParameters series_of(const QualifiedSet& q) {
  SeriesParameters s{};
  s.tendon_slack_length = q.slack;
  s.tendon_strain_at_unit_force = q.e1;
  s.pennation_at_optimal = q.alpha0;
  s.normalized_damping = q.beta;
  s.minimum_cosine = q.min_cos;
  return s;
}

void check_refusal(const ValidationResult& r, Refusal kind, const char* field, const char* unit,
                   const char* rule) {
  CHECK(r.refusal == kind);
  CHECK(std::strcmp(r.field, field) == 0);
  CHECK(std::strcmp(r.unit, unit) == 0);
  CHECK(std::strcmp(r.rule, rule) == 0);
}

}  // namespace

TEST_CASE("muscle validator: synthetic-study-only version 1 is the corpus's identities") {
  const QualifiedDomain& d = qualified_domain();
  CHECK(std::strcmp(d.id, "synthetic-study-only") == 0);
  CHECK(d.version == 1u);
  CHECK(d.sets.size() == 17u);
  const std::string root = test::data_path(ENGINE_SOURCE_DIR "/content/muscle", "content/muscle");
  if (!test::path_exists(root + "/corpus.json")) {
    MESSAGE("not in this bundle: " << root);
    return;
  }
  JsonValue index;
  REQUIRE(parse_json(read_text(root + "/corpus.json"), index).ok);
  const JsonValue& domain = *index.find("qualified_domain");
  CHECK(domain.find("id")->as_string() == "synthetic-study-only");
  const JsonValue::Array& ids = domain.find("parameter_sha256")->as_array();
  REQUIRE(ids.size() == d.sets.size());
  for (u32 i = 0; i < ids.size(); ++i) {
    Sha256 id{};
    REQUIRE(parse_sha256(ids[i].as_string(), id));
    CHECK(id == d.sets[i].identity);
  }
  // Every case of the corpus validates with its own identity and parameters.
  u32 cases = 0;
  for (const JsonValue& row : index.find("cases")->as_array()) {
    const std::string name(row.find("case")->as_string());
    INFO(name);
    JsonValue doc;
    REQUIRE(parse_json(read_text(root + "/cases/" + name + ".json"), doc).ok);
    const JsonValue& inputs = *doc.find("inputs");
    const JsonValue& f = *inputs.find("parameters")->find("fiber");
    const JsonValue& s = *inputs.find("parameters")->find("series");
    FiberParameters fiber{};
    fiber.force_scale = num(f, "force_scale_N");
    fiber.optimal_fiber_length = num(f, "optimal_fiber_length_m");
    fiber.rigid_tendon_length = num(f, "rigid_tendon_length_m");
    fiber.vmax_lopt_per_s = num(f, "vmax_lopt_per_s");
    fiber.tau_activation = num(f, "tau_activation_s");
    fiber.tau_deactivation = num(f, "tau_deactivation_s");
    fiber.activation_smoothing = num(f, "activation_smoothing");
    fiber.passive_strain_at_unit_force = num(f, "passive_strain_at_unit_force");
    fiber.passive_shape = num(f, "passive_shape");
    fiber.passive_zero_normalized_length = num(f, "passive_zero_normalized_length");
    fiber.min_normalized_length = num(f, "min_normalized_length");
    fiber.max_normalized_length = num(f, "max_normalized_length");
    fiber.reference_area_m2 = num(f, "reference_area_m2");
    fiber.reference_fiber_length = num(f, "reference_fiber_length_m");
    SeriesParameters series{};
    series.tendon_slack_length = num(s, "tendon_slack_length_m");
    series.tendon_strain_at_unit_force = num(s, "tendon_strain_at_unit_force");
    series.pennation_at_optimal = num(s, "pennation_at_optimal_rad");
    series.normalized_damping = num(s, "normalized_damping");
    series.minimum_cosine = num(s, "minimum_cosine");
    Sha256 id{};
    REQUIRE(parse_sha256(inputs.find("parameter_sha256")->as_string(), id));
    const ValidationResult r = validate(k_model_sha256, id, fiber, series);
    INFO(refusal_code(r.refusal) << " " << r.field << " = " << r.value);
    CHECK(r.ok());
    ++cases;
  }
  CHECK(cases == 39u);
}

TEST_CASE("muscle validator: an unknown law, an unqualified set, and a changed parameter are refused") {
  const QualifiedSet& q = first_set();
  const SeriesParameters series = series_of(q);
  CHECK(validate(k_model_sha256, q.identity, FiberParameters{}, series).ok());
  // The reference's identity is the law this one implements, not this law's.
  ValidationResult r = validate(k_reference_model_sha256, q.identity, FiberParameters{}, series);
  CHECK(r.refusal == Refusal::ModelUnrecognized);
  CHECK(std::strcmp(refusal_code(r.refusal), "model_unrecognized") == 0);
  Sha256 unknown = q.identity;
  unknown[31] ^= 1;
  r = validate(k_model_sha256, unknown, FiberParameters{}, series);
  CHECK(r.refusal == Refusal::DomainUnqualified);
  CHECK(std::strcmp(refusal_code(r.refusal), "domain_unqualified") == 0);
  // A qualified identity with parameters it was not qualified with is not qualified by it.
  SeriesParameters other = series;
  other.normalized_damping = 0.15;
  r = validate(k_model_sha256, q.identity, FiberParameters{}, other);
  CHECK(r.refusal == Refusal::DomainUnqualified);
  CHECK(r.value == 0.15);
  FiberParameters stiffer{};
  stiffer.passive_shape = 5.0;
  CHECK(validate(k_model_sha256, q.identity, stiffer, series).refusal == Refusal::DomainUnqualified);
}

TEST_CASE("muscle validator: a value outside the law's domain names field, value, unit and rule") {
  const QualifiedSet& q = first_set();
  const SeriesParameters series = series_of(q);
  FiberParameters f{};
  f.force_scale = -1000.0;
  ValidationResult r = validate(k_model_sha256, q.identity, f, series);
  check_refusal(r, Refusal::ParameterOutsideDomain, "fiber.force_scale", "N", "finite and positive");
  CHECK(r.value == -1000.0);
  CHECK(std::strcmp(refusal_code(r.refusal), "parameter_outside_domain") == 0);
  f = FiberParameters{};
  f.tau_activation = std::numeric_limits<double>::infinity();
  check_refusal(validate(k_model_sha256, q.identity, f, series), Refusal::ParameterOutsideDomain,
                "fiber.tau_activation", "s", "finite and positive");
  f = FiberParameters{};
  f.min_normalized_length = 0.1;  // below the passive zero length, 0.2
  check_refusal(validate(k_model_sha256, q.identity, f, series), Refusal::ParameterOutsideDomain,
                "fiber.min_normalized_length", "ratio", "above passive_zero_normalized_length");
  f = FiberParameters{};
  f.max_normalized_length = 0.4;
  check_refusal(validate(k_model_sha256, q.identity, f, series), Refusal::ParameterOutsideDomain,
                "fiber.max_normalized_length", "ratio", "above min_normalized_length");
  SeriesParameters s = series;
  s.pennation_at_optimal = 0.5;
  r = validate(k_model_sha256, q.identity, FiberParameters{}, s);
  check_refusal(r, Refusal::ParameterOutsideDomain, "series.pennation_at_optimal", "rad", "finite and in [0, 0.4]");
  CHECK(r.value == 0.5);
  s = series;
  s.minimum_cosine = 1.0;
  check_refusal(validate(k_model_sha256, q.identity, FiberParameters{}, s), Refusal::ParameterOutsideDomain,
                "series.minimum_cosine", "ratio", "below 1");
  s = series;
  s.tendon_slack_length = std::numeric_limits<double>::quiet_NaN();
  check_refusal(validate(k_model_sha256, q.identity, FiberParameters{}, s), Refusal::ParameterOutsideDomain,
                "series.tendon_slack_length", "m", "finite and positive");
}

TEST_CASE("muscle validator: an actuator the validator refuses is never added") {
  const QualifiedSet& q = first_set();
  LineStore store;
  store.reserve(2);
  ActuatorSpec spec{};
  spec.parameter_sha256 = q.identity;
  spec.series = series_of(q);
  u32 index = 0;
  ValidationResult r{};
  CHECK(add_validated(store, spec, k_model_sha256, index, r));
  CHECK(r.ok());
  CHECK(store.size() == 1u);
  spec.series.pennation_at_optimal = -0.1;
  CHECK_FALSE(add_validated(store, spec, k_model_sha256, index, r));
  CHECK(r.refusal == Refusal::ParameterOutsideDomain);
  CHECK(store.size() == 1u);
}
