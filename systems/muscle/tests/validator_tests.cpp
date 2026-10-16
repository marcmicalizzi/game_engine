// The validator (validator.h): the qualified domains are the corpus's, and every refusal is by
// name.
#include <core/json/json.h>
#include <systems/muscle/validator.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <bit>
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

bool same_bits(double a, double b) { return std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }

// The nineteen values under the authoring names the corpus and the identity use (r012's
// PARAMETER-FIELDS.json), against the engine's fields.
struct FiberName {
  const char* key;
  double FiberParameters::* member;
};
constexpr FiberName k_fiber_names[] = {
    {"force_scale_N", &FiberParameters::force_scale},
    {"optimal_fiber_length_m", &FiberParameters::optimal_fiber_length},
    {"rigid_tendon_length_m", &FiberParameters::rigid_tendon_length},
    {"vmax_lopt_per_s", &FiberParameters::vmax_lopt_per_s},
    {"tau_activation_s", &FiberParameters::tau_activation},
    {"tau_deactivation_s", &FiberParameters::tau_deactivation},
    {"activation_smoothing", &FiberParameters::activation_smoothing},
    {"passive_strain_at_unit_force", &FiberParameters::passive_strain_at_unit_force},
    {"passive_shape", &FiberParameters::passive_shape},
    {"passive_zero_normalized_length", &FiberParameters::passive_zero_normalized_length},
    {"min_normalized_length", &FiberParameters::min_normalized_length},
    {"max_normalized_length", &FiberParameters::max_normalized_length},
    {"reference_area_m2", &FiberParameters::reference_area_m2},
    {"reference_fiber_length_m", &FiberParameters::reference_fiber_length},
};
struct SeriesName {
  const char* key;
  double SeriesParameters::* member;
};
constexpr SeriesName k_series_names[] = {
    {"tendon_slack_length_m", &SeriesParameters::tendon_slack_length},
    {"tendon_strain_at_unit_force", &SeriesParameters::tendon_strain_at_unit_force},
    {"pennation_at_optimal_rad", &SeriesParameters::pennation_at_optimal},
    {"normalized_damping", &SeriesParameters::normalized_damping},
    {"minimum_cosine", &SeriesParameters::minimum_cosine},
};

FiberParameters fiber_of(const JsonValue& f) {
  FiberParameters out{};
  for (const FiberName& n : k_fiber_names)
    out.*n.member = num(f, n.key);
  return out;
}

SeriesParameters series_of(const JsonValue& s) {
  SeriesParameters out{};
  for (const SeriesName& n : k_series_names)
    out.*n.member = num(s, n.key);
  return out;
}

const QualifiedSet& first_set() { return qualified_domains()[0].sets[0]; }

void check_refusal(const ValidationResult& r, Refusal kind, const char* field, const char* unit,
                   const char* rule) {
  CHECK(r.refusal == kind);
  CHECK(std::strcmp(r.field, field) == 0);
  CHECK(std::strcmp(r.unit, unit) == 0);
  CHECK(std::strcmp(r.rule, rule) == 0);
}

std::string corpus_root() {
  return test::data_path(ENGINE_SOURCE_DIR "/content/muscle", "content/muscle");
}

}  // namespace

TEST_CASE("muscle validator: the compiled table is the corpus's qualified domains, bit for bit") {
  // The table is generated (src/qualified_domains.inc, tools/muscle-corpus.ps1): every value a
  // hexadecimal literal of its source token's binary64. This holds the literals to the tokens as
  // core/json reads them, so the table and the corpus cannot drift and a literal that did not
  // round-trip would fail here.
  const std::span<const QualifiedDomain> domains = qualified_domains();
  REQUIRE(!domains.empty());
  CHECK(std::strcmp(domains[0].id, "synthetic-study-only") == 0);
  CHECK(domains[0].version == 1u);
  CHECK(domains[0].sets.size() == 17u);
  const std::string root = corpus_root();
  if (!test::path_exists(root + "/corpus.json")) {
    MESSAGE("not in this bundle: " << root);
    return;
  }
  JsonValue index;
  REQUIRE(parse_json(read_text(root + "/corpus.json"), index).ok);
  CHECK(num(index, "format_version") == 2.0);
  const JsonValue::Array& listed = index.find("qualified_domains")->as_array();
  REQUIRE(listed.size() == domains.size());
  u32 values = 0;
  for (u32 d = 0; d < listed.size(); ++d) {
    const JsonValue& domain = listed[d];
    INFO(domains[d].id);
    CHECK(domain.find("id")->as_string() == domains[d].id);
    CHECK(num(domain, "version") == static_cast<double>(domains[d].version));
    const JsonValue::Array& ids = domain.find("identities")->as_array();
    REQUIRE(ids.size() == domains[d].sets.size());
    for (u32 i = 0; i < ids.size(); ++i) {
      const QualifiedSet& q = domains[d].sets[i];
      Sha256 id{};
      REQUIRE(parse_sha256(ids[i].find("parameter_sha256")->as_string(), id));
      CHECK(id == q.identity);
      const JsonValue& f = *ids[i].find("fiber");
      const JsonValue& s = *ids[i].find("series");
      CHECK(f.as_object().size() == std::size(k_fiber_names));
      CHECK(s.as_object().size() == std::size(k_series_names));
      for (const FiberName& n : k_fiber_names) {
        INFO(n.key);
        CHECK(same_bits(q.fiber.*n.member, num(f, n.key)));
        ++values;
      }
      for (const SeriesName& n : k_series_names) {
        INFO(n.key);
        CHECK(same_bits(q.series.*n.member, num(s, n.key)));
        ++values;
      }
    }
  }
  CHECK(values == 17u * 19u);
}

TEST_CASE("muscle validator: every corpus case validates in the domain its row names") {
  const std::string root = corpus_root();
  if (!test::path_exists(root + "/corpus.json")) {
    MESSAGE("not in this bundle: " << root);
    return;
  }
  JsonValue index;
  REQUIRE(parse_json(read_text(root + "/corpus.json"), index).ok);
  const JsonValue::Array& rows = index.find("cases")->as_array();
  u32 cases = 0;
  for (const JsonValue& row : rows) {
    const std::string name(row.find("case")->as_string());
    INFO(name);
    JsonValue doc;
    REQUIRE(parse_json(read_text(root + "/cases/" + name + ".json"), doc).ok);
    const JsonValue& inputs = *doc.find("inputs");
    const FiberParameters fiber = fiber_of(*inputs.find("parameters")->find("fiber"));
    const SeriesParameters series = series_of(*inputs.find("parameters")->find("series"));
    Sha256 id{};
    REQUIRE(parse_sha256(inputs.find("parameter_sha256")->as_string(), id));
    CHECK(row.find("parameter_sha256")->as_string() ==
          inputs.find("parameter_sha256")->as_string());
    const ValidationResult r = validate(k_model_sha256, id, fiber, series);
    INFO(refusal_code(r.refusal) << " " << r.field << " = " << r.value);
    CHECK(r.ok());
    CHECK(row.find("domain")->as_string() == r.domain);
    CHECK(num(row, "domain_version") == static_cast<double>(r.domain_version));
    ++cases;
  }
  CHECK(cases == rows.size());
  CHECK(cases >= 39u);
}

TEST_CASE(
    "muscle validator: an unknown law, an unqualified set, and a changed parameter are refused") {
  const QualifiedSet& q = first_set();
  ValidationResult r = validate(k_model_sha256, q.identity, q.fiber, q.series);
  CHECK(r.ok());
  CHECK(std::strcmp(r.domain, "synthetic-study-only") == 0);
  CHECK(r.domain_version == 1u);
  // The reference's identity is the law this one implements, not this law's.
  r = validate(k_reference_model_sha256, q.identity, q.fiber, q.series);
  CHECK(r.refusal == Refusal::ModelUnrecognized);
  CHECK(std::strcmp(refusal_code(r.refusal), "model_unrecognized") == 0);
  Sha256 unknown = q.identity;
  unknown[31] ^= 1;
  r = validate(k_model_sha256, unknown, q.fiber, q.series);
  check_refusal(r, Refusal::DomainUnqualified, "parameter_sha256", "",
                "synthetic-study-only version 1");
  CHECK(std::strcmp(refusal_code(r.refusal), "domain_unqualified") == 0);
  CHECK(std::strcmp(r.domain, "") == 0);  // found in none
  CHECK(r.domain_version == 0u);
  // A qualified identity with a value it was not qualified with is not qualified by it: the
  // refusal names the first differing field, its value and unit, and the domain it was found in.
  SeriesParameters other = q.series;
  other.normalized_damping = 0.15;
  r = validate(k_model_sha256, q.identity, q.fiber, other);
  check_refusal(r, Refusal::DomainUnqualified, "series.normalized_damping", "1",
                "the value the identity was qualified with");
  CHECK(r.value == 0.15);
  CHECK(std::strcmp(r.domain, "synthetic-study-only") == 0);
  CHECK(r.domain_version == 1u);
  // Every fiber value is compared to the identity's own, not to the law's defaults: one ulp off
  // in any of the fourteen is refused by name.
  for (const FiberName& n : k_fiber_names) {
    INFO(n.key);
    FiberParameters f = q.fiber;
    f.*n.member = std::nextafter(f.*n.member, 2.0 * f.*n.member);
    r = validate(k_model_sha256, q.identity, f, q.series);
    CHECK(r.refusal == Refusal::DomainUnqualified);
    CHECK(r.value == f.*n.member);
    CHECK(std::strcmp(r.domain, "synthetic-study-only") == 0);
  }
  FiberParameters stiffer = q.fiber;
  stiffer.passive_shape = 5.0;
  r = validate(k_model_sha256, q.identity, stiffer, q.series);
  check_refusal(r, Refusal::DomainUnqualified, "fiber.passive_shape", "1",
                "the value the identity was qualified with");
  CHECK(r.value == 5.0);
  FiberParameters wider = q.fiber;
  wider.reference_area_m2 = 0.005;  // unused by the line tier, and still part of the identity
  check_refusal(validate(k_model_sha256, q.identity, wider, q.series), Refusal::DomainUnqualified,
                "fiber.reference_area_m2", "m^2", "the value the identity was qualified with");
}

TEST_CASE("muscle validator: a value outside the law's domain names field, value, unit and rule") {
  const QualifiedSet& q = first_set();
  const SeriesParameters series = q.series;
  FiberParameters f = q.fiber;
  f.force_scale = -1000.0;
  ValidationResult r = validate(k_model_sha256, q.identity, f, series);
  check_refusal(r, Refusal::ParameterOutsideDomain, "fiber.force_scale", "N",
                "finite and positive");
  CHECK(r.value == -1000.0);
  CHECK(std::strcmp(refusal_code(r.refusal), "parameter_outside_domain") == 0);
  f = q.fiber;
  f.tau_activation = std::numeric_limits<double>::infinity();
  check_refusal(validate(k_model_sha256, q.identity, f, series), Refusal::ParameterOutsideDomain,
                "fiber.tau_activation", "s", "finite and positive");
  f = q.fiber;
  f.min_normalized_length = 0.1;  // below the passive zero length, 0.2
  check_refusal(validate(k_model_sha256, q.identity, f, series), Refusal::ParameterOutsideDomain,
                "fiber.min_normalized_length", "ratio", "above passive_zero_normalized_length");
  f = q.fiber;
  f.max_normalized_length = 0.4;
  check_refusal(validate(k_model_sha256, q.identity, f, series), Refusal::ParameterOutsideDomain,
                "fiber.max_normalized_length", "ratio", "above min_normalized_length");
  SeriesParameters s = series;
  s.pennation_at_optimal = 0.5;
  r = validate(k_model_sha256, q.identity, q.fiber, s);
  check_refusal(r, Refusal::ParameterOutsideDomain, "series.pennation_at_optimal", "rad",
                "finite and in [0, 0.4]");
  CHECK(r.value == 0.5);
  s = series;
  s.minimum_cosine = 1.0;
  check_refusal(validate(k_model_sha256, q.identity, q.fiber, s), Refusal::ParameterOutsideDomain,
                "series.minimum_cosine", "ratio", "below 1");
  s = series;
  s.tendon_slack_length = std::numeric_limits<double>::quiet_NaN();
  check_refusal(validate(k_model_sha256, q.identity, q.fiber, s), Refusal::ParameterOutsideDomain,
                "series.tendon_slack_length", "m", "finite and positive");
}

TEST_CASE("muscle validator: an actuator the validator refuses is never added") {
  const QualifiedSet& q = first_set();
  LineStore store;
  store.reserve(2);
  ActuatorSpec spec{};
  spec.parameter_sha256 = q.identity;
  spec.fiber = q.fiber;
  spec.series = q.series;
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
