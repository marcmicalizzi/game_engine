// The law against the reference's archived initial states, the law's identity, and the rule that
// the kernel calls no C library transcendental (docs/subsystems/muscle.md, "Tests").
#include <systems/muscle/law.h>
#include <systems/muscle/muscle.h>
#include <systems/muscle/scheme.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include "rows.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

using namespace engine;
using namespace engine::muscle;
using namespace engine::muscle::test_rows;

namespace {

// 1e-9 relative, with a floor of 1e-9 N (or J) for the quantities the reference has at rounding
// level (the damping force and the residual at an equilibrium velocity of 1e-15 m/s).
void check_close(const char* what, double got, double expected) {
  INFO(what << ": got " << got << ", expected " << expected);
  CHECK(std::fabs(got - expected) <= 1e-9 * std::fmax(std::fabs(expected), 1.0));
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("muscle law: forces, energies and residual at the archived initial states") {
  for (const Row& r : k_rows) {
    INFO(r.name);
    const LawRow p = law_of(r);
    Counters n{};
    Failure f{};
    // The reference's initial sample takes the fiber velocity from the equilibrium root.
    double v = 0.0;
    REQUIRE(equilibrium_velocity(p, r.a, r.l, r.length, v, n, f));
    CHECK(std::fabs(v) < 1e-12);
    Forces forces{};
    REQUIRE(components(p, r.a, r.l, v, r.length, forces, f));
    check_close("tendon force", forces.tendon, r.tendon_force);
    check_close("active force", forces.active, r.active_force);
    check_close("passive force", forces.passive, r.passive_force);
    check_close("damping force", forces.damping, r.damping_force);
    check_close("series residual", forces.residual, r.residual);
    double passive = 0.0;
    REQUIRE(passive_energy(p, r.l, passive, f));
    check_close("passive energy", passive, r.passive_energy);
    Geometry g{};
    REQUIRE(geometry(p, r.l, g, f));
    Tendon t{};
    REQUIRE(tendon(p, r.length - g.projected, t, f));
    check_close("tendon energy", t.energy, r.tendon_energy);
  }
}

TEST_CASE("muscle law: each domain test fails by name with the value it read") {
  const LawRow p = law_of(k_rows[0]);
  Failure f{};
  double out = 0.0;
  CHECK_FALSE(activation_rate(p, 1.5, 0.2, out, f));
  CHECK(f.test == ValidationTest::ActivationOutOfRange);
  CHECK(f.value == 1.5);
  CHECK_FALSE(activation_rate(p, 0.5, -0.1, out, f));
  CHECK(f.test == ValidationTest::ExcitationOutOfRange);
  CHECK_FALSE(force_velocity(1.25, out, f));
  CHECK(f.test == ValidationTest::NormalizedVelocityOutOfRange);
  double x = 0.0;
  CHECK_FALSE(normalized_fiber_length(p, 0.2, x, f));  // x = 2 > 1.6
  CHECK(f.test == ValidationTest::NormalizedLengthOutOfRange);
  CHECK(f.value == 2.0);
  // The 4-ulp snap: a dimensional round trip that lands a few ulp outside the domain is the
  // endpoint, not a failure.
  CHECK(normalized_fiber_length(p, std::nextafter(0.05, 0.0), x, f));
  CHECK(x == 0.5);
  Tendon t{};
  CHECK_FALSE(tendon(p, -0.01, t, f));
  CHECK(f.test == ValidationTest::TendonLengthInvalid);
  // k = ln 6 / 0.049 = 36.6: strain 1 is inside the domain, strain 3 (k strain = 110) is not.
  CHECK(tendon(p, 0.1, t, f));
  CHECK_FALSE(tendon(p, 0.2, t, f));
  CHECK(f.test == ValidationTest::TendonExtensionBeyondDomain);
  CHECK(tendon(p, 0.04, t, f));  // slack: zero force and energy
  CHECK(t.force == 0.0);
  CHECK(t.energy == 0.0);
}

TEST_CASE("muscle law: the model identity is the hash of model.txt") {
  const std::string root =
      test::data_path(ENGINE_SOURCE_DIR "/systems/muscle", "systems/muscle");
  const std::string text_path = root + "/tests/model.txt";
  if (!test::path_exists(text_path)) {
    MESSAGE("not in this bundle: " << text_path);
    return;
  }
  const std::string text = read_text(text_path);
  // The constant was computed from these bytes; a changed law text is a new identity, computed
  // again with Get-FileHash and pinned with its byte count.
  CHECK(text.size() == k_model_text_bytes);
  CHECK(text.find("r005 reference law (model sha256 " + to_hex(k_reference_model_sha256) + ")") !=
        std::string::npos);
  const std::string header = read_text(root + "/include/systems/muscle/muscle.h");
  const std::string provenance = "model.txt, " + std::to_string(k_model_text_bytes) +
                                 " bytes, SHA-256 " + to_hex(k_model_sha256);
  CHECK(header.find(provenance) != std::string::npos);
  Sha256 parsed{};
  CHECK(parse_sha256(to_hex(k_model_sha256), parsed));
  CHECK(parsed == k_model_sha256);
  CHECK_FALSE(parse_sha256("xyz", parsed));
}

// The kernel's bytes are the same on every toolchain only if no C library transcendental reaches
// them (core/math/deterministic.h). A grep is cheap and says where.
TEST_CASE("muscle law: no std:: transcendental in the kernel") {
  const std::string root =
      test::data_path(ENGINE_SOURCE_DIR "/systems/muscle", "systems/muscle");
  if (!test::path_exists(root + "/include")) {
    MESSAGE("not in this bundle: " << root);
    return;
  }
  const char* const banned[] = {"std::exp", "std::expm1", "std::log", "std::log1p", "std::pow",
                                "std::tanh", "std::asinh", "std::sinh", "std::cosh", "std::sin(",
                                "std::cos(", "std::tan(", "std::asin", "std::acos", "std::atan"};
  u32 files = 0;
  for (const char* dir : {"/include/systems/muscle", "/src"}) {
    for (const auto& entry : std::filesystem::directory_iterator(root + dir)) {
      const std::string text = read_text(entry.path().string());
      ++files;
      for (const char* b : banned) {
        INFO(entry.path().string() << " names " << b);
        CHECK(text.find(b) == std::string::npos);
      }
    }
  }
  CHECK(files >= 4);
}
