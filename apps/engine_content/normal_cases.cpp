// engine-content normal-cases: the footpoint-normal rule's edge cases, evaluated one by one against
// what the authoring side expects (docs/subsystems/geometry.md, "The conformance exchange";
// docs/subsystems/apps.md, "engine-content normal-cases").
//
//   engine-content normal-cases <cases.json> --out <results.json>
//
// **Why it exists.** `limit-dump` compares whole surfaces, where a threshold case is one vertex
// among thousands and may never occur at all. So the authoring side also publishes the rule's edge
// cases on their own (`astra.normal_conformance.function_cases.v1`): tangents either side of the
// sine threshold, a zero tangent, an interpolation that cancels, a dense face collapsed exactly,
// and non-finite tangents for the harness to construct. This evaluates each one through the
// engine's own functions — the ones the binding frame and `limit-dump` call, never a copy of them —
// and writes, per case, the inputs, what the engine computed, what the case expects and the angle
// between the two, so the implementations can be compared case by case (the D8 comparison). **A
// mismatch is a finding**: it is written down, never hidden, and never answered by changing the
// engine to fit a case. The command exits 0 whenever it read and evaluated the file, whatever it
// found, 1 when it could not, and 2 on usage.
//
// How each kind of case reaches the engine:
//
// - `tangents`, two paths. The **rule** path is the tangents as a reference evaluation takes them,
//   `limit_reference_normals`: a degenerate pair is invalid and its normal the zero vector, never
//   invented. The **state** path is the only one that falls back, and it is the case's verdict: a
//   reference frame whose limit normal is the case's `reference` (orthonormal tangents, built here
//   in double and rounded), the case's tangents as the state, and `limit_normal_changes` — the
//   reference normal plus its change, exactly as `limit-dump` and `evaluate_binding_frame` compute
//   a state's vertex normals. Both are written; the summary counts both.
// - `interpolation_cancellation`: a one-triangle surface whose corners' reference normals are the
//   case's `reference` and whose state normals are the case's `normals`, through `footpoint_normal`
//   plus `footpoint_normal_change` under `limit-interpolated` (the interpolation stage is the same
//   code in the area-weighted mode). The rule path is `footpoint_normal` with the case's normals
//   taken as the reference's.
// - `exact_dense_face_collapse`: the given dense triangles as a reference surface — the facet by
//   `footpoint_normal` in `triangle` mode, the area-weighted vertex normals by
//   `area_weighted_normal_vectors` and `area_weighted_reference_normals` and their interpolation by
//   `footpoint_normal`. It is not a Loop control mesh and is never refined.
// - `facet_states`, the engine's own addition to the schema: a footpoint triangle's corners at the
//   reference and at a state, through `footpoint_normal` plus `footpoint_normal_change` in
//   `triangle` mode, which is the transfer's facet code. The rule path is `footpoint_normal` on the
//   state's corners.
// - `nonfinite_cases`: when the file names them, tangents with a NaN or an infinity, built here as
//   the authoring side's harness builds them, each on the tangents paths with reference +z.
//
// Every input number is rounded to f32 on the way in, as the engine takes it, and the rounding is
// reported; the engine evaluates in f32; the comparison is in double. **The tolerance is 2^-18 rad
// (2.2e-4 degrees)**, 64 f32 unit roundoffs: an input direction rounded to f32 is within a few
// roundoffs of the one written, the rule is a normalize, a cross product and a normalize (about ten
// roundings of numbers of order one), and a state adds up to four `unit_change`s (the two unit
// tangents, their cross product and the normal), each a handful more. It is tight enough that any
// disagreement about a definition shows, and 4.6 times below the 0.0010 degrees the first surface
// exchange agreed footpoint normals to. **Near a threshold it widens by the rule's own
// conditioning** (`tolerance_rad`): a normal normalized out of a short vector — a sine of 2e-6,
// a facet whose corner angle has a sine of 3e-5 — carries that vector's f32 rounding divided by
// its length, and no implementation in f32 does better. A valid normal must also have unit length
// to 2^-18, which is what a silent loss of length fails whatever its direction. Validity is
// compared exactly. The output's bytes are a function of the input's: the JSON writer is canonical
// and floating-point contraction is off tree-wide (ADR-0035).
#include "content_commands.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/math/math.h>
#include <domain/geometry/surface_binding.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_tolerance_rad = 0x1.0p-18;
constexpr f64 k_tolerance_deg = k_tolerance_rad * 180.0 / k_pi;
constexpr f64 k_nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 k_inf = std::numeric_limits<f64>::infinity();

const char* k_cases_usage =
    "usage: engine-content normal-cases <cases.json> --out <results.json>\n"
    "  evaluates the footpoint-normal rule's edge cases\n"
    "  (astra.normal_conformance.function_cases.v1) through the engine's own functions and\n"
    "  writes, per case, what the engine computed, what the case expects and the angle between\n"
    "  them; a mismatch is reported, and the exit code is 0 whenever the file was evaluated\n";

int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content normal-cases: %s\n", message);
  std::fputs(k_cases_usage, stderr);
  return k_exit_usage;
}

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content normal-cases: %s\n", message.c_str());
  return k_exit_error;
}

// ---- small double-precision vectors: the inputs as written and the comparison ------------------

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

D3 d3(Vec3 v) noexcept {
  return D3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}
f64 dot3(D3 a, D3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross3(D3 a, D3 b) noexcept {
  return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
D3 scale3(D3 a, f64 s) noexcept { return D3{a.x * s, a.y * s, a.z * s}; }
D3 sub3(D3 a, D3 b) noexcept { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
bool finite3(D3 a) noexcept {
  return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

// The angle between two directions in degrees, atan2(|a x b|, a . b) in double — the authoring
// side's own `angle_degrees`, which stays accurate at the small angles acos loses. NaN when either
// is zero or not finite: a zero normal has no direction to compare.
f64 angle_deg(D3 a, D3 b) noexcept {
  if (!finite3(a) || !finite3(b) || dot3(a, a) == 0.0 || dot3(b, b) == 0.0) return k_nan;
  const D3 c = cross3(a, b);
  return std::atan2(std::sqrt(dot3(c, c)), dot3(a, b)) * (180.0 / k_pi);
}

// Input numbers are rounded to f32, as the engine takes them; what that costs is reported. NaN and
// the infinities pass through as themselves, and a finite number past f32's range becomes an
// infinity (with an infinite rounding) rather than an undefined conversion.
struct Rounding {
  f64 worst = 0.0;
  bool exact = true;
  f32 take(f64 value) {
    if (!std::isfinite(value)) return static_cast<f32>(value);
    constexpr f64 k_f32_max = static_cast<f64>(std::numeric_limits<f32>::max());
    if (std::fabs(value) > k_f32_max) {
      exact = false;
      worst = k_inf;
      return value > 0.0 ? std::numeric_limits<f32>::infinity()
                         : -std::numeric_limits<f32>::infinity();
    }
    const f32 rounded = static_cast<f32>(value);
    const f64 error = std::fabs(static_cast<f64>(rounded) - value);
    worst = std::max(worst, error);
    exact = exact && error == 0.0;
    return rounded;
  }
  Vec3 take(D3 v) { return Vec3{take(v.x), take(v.y), take(v.z)}; }
  void merge(const Rounding& other) {
    worst = std::max(worst, other.worst);
    exact = exact && other.exact;
  }
};

// A number, or NaN and the infinities as the strings JSON has no number for ("NaN", "Infinity",
// "-Infinity", JavaScript's spellings), which the reader accepts back.
JsonValue number_json(f64 v) {
  if (std::isnan(v)) return JsonValue("NaN");
  if (std::isinf(v)) return JsonValue(v > 0.0 ? "Infinity" : "-Infinity");
  return JsonValue(v);
}

JsonValue d3_json(D3 v) {
  JsonValue out = JsonValue::array();
  out.push_back(number_json(v.x));
  out.push_back(number_json(v.y));
  out.push_back(number_json(v.z));
  return out;
}

JsonValue vec3_json(Vec3 v) { return d3_json(d3(v)); }

// An angle, or null when there is none to report.
JsonValue angle_json(f64 degrees) { return std::isnan(degrees) ? JsonValue() : JsonValue(degrees); }

bool read_number(const JsonValue& value, f64& out) {
  if (value.get_f64(out)) return true;
  std::string_view s;
  if (!value.get_string(s)) return false;
  if (s == "NaN") {
    out = k_nan;
  } else if (s == "Infinity") {
    out = k_inf;
  } else if (s == "-Infinity") {
    out = -k_inf;
  } else {
    return false;
  }
  return true;
}

bool read_d3(const JsonValue* value, D3& out) {
  if (value == nullptr || !value->is_array() || value->size() != 3) return false;
  return read_number((*value)[0], out.x) && read_number((*value)[1], out.y) &&
         read_number((*value)[2], out.z);
}

// Whether a vector computed as reference + change is the vector the case wrote, bit for bit (or
// NaN where it wrote NaN): the state path's own tangents, echoed so that a case whose state the
// frame could not reproduce exactly says so.
bool same_bits(Vec3 a, Vec3 b) noexcept {
  const f32 x[3] = {a.x, a.y, a.z};
  const f32 y[3] = {b.x, b.y, b.z};
  for (u32 k = 0; k < 3; ++k) {
    if (std::isnan(x[k]) && std::isnan(y[k])) continue;
    if (std::memcmp(&x[k], &y[k], sizeof(f32)) != 0) return false;
  }
  return true;
}

// The power of two the state path's reference frame is built at: the largest finite component of
// the case's tangents, rounded down to a power of two, so that the reference and the state are of
// one size (a uniform rescaling of a case rescales its frame with it, exactly) and reference plus
// change reproduces the state's tangents; 1 when a tangent is zero or not finite.
f64 frame_scale(D3 u, D3 v) noexcept {
  if (!finite3(u) || !finite3(v)) return 1.0;
  const f64 m = std::max({std::fabs(u.x), std::fabs(u.y), std::fabs(u.z), std::fabs(v.x),
                          std::fabs(v.y), std::fabs(v.z)});
  if (!(m > 0.0)) return 1.0;
  int e = 0;
  std::frexp(m, &e);
  return std::ldexp(1.0, e - 1);
}

// Orthogonal tangents of length `scale` whose cross product is along the unit `normal`, in double
// and then rounded: the coordinate axis least aligned with it (the first on a tie), made
// perpendicular, and the normal crossed with that. An axis-aligned normal gets exact axis
// tangents. False, with zero tangents (an invalid reference frame, which the engine never invents
// a normal for), when the normal is zero or not finite.
bool reference_frame(D3 normal, f64 scale, Vec3& tu, Vec3& tv) {
  tu = Vec3{};
  tv = Vec3{};
  const f64 length = std::sqrt(dot3(normal, normal));
  if (!finite3(normal) || !(length > 0.0) || !std::isfinite(length)) return false;
  const D3 n = scale3(normal, 1.0 / length);
  u32 axis = 0;
  if (std::fabs(n.y) < std::fabs(n.x)) axis = 1;
  if (std::fabs(n.z) < std::fabs(axis == 0 ? n.x : n.y)) axis = 2;
  const D3 e{axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0};
  const D3 t = sub3(e, scale3(n, dot3(e, n)));
  const D3 t1 = scale3(t, 1.0 / std::sqrt(dot3(t, t)));
  const D3 t2 = cross3(n, t1);
  Rounding unused;
  tu = unused.take(scale3(t1, scale));
  tv = unused.take(scale3(t2, scale));
  return true;
}

// **The tolerance.** 2^-18 rad — 64 f32 unit roundoffs — for a normal normalized out of a vector
// of length of order one. A normal normalized out of a short dimensionless vector, of length sigma
// (the sine between two unit tangents or between a facet's edges, or the length of an
// interpolation of unit normals), carries that vector's own f32 rounding, about 2 roundoffs a
// component, divided by sigma; the tolerance allows four times that, 2^-21 / sigma, where it is
// the larger. It is the rule's conditioning, not a loosening: a near-threshold normal is only as
// good in f32 as that, whichever implementation computes it.
f64 tolerance_rad(f64 sigma) noexcept {
  if (!(sigma > 0.0) || !std::isfinite(sigma)) return k_tolerance_rad;
  return std::max(k_tolerance_rad, 0x1.0p-21 / sigma);
}

// A valid normal must also be one: |n| within 2^-18 of 1. What a silent loss of length (record 35's
// 0.45, before 2026-09-24) fails, whatever its direction.
bool unit_length(Vec3 n) noexcept {
  const D3 d = d3(n);
  return std::fabs(std::sqrt(dot3(d, d)) - 1.0) <= k_tolerance_rad;
}

// The sine between two directions, in double from the values the engine read.
f64 sine_between(D3 a, D3 b) noexcept {
  const f64 la = std::sqrt(dot3(a, a));
  const f64 lb = std::sqrt(dot3(b, b));
  if (!(la > 0.0) || !(lb > 0.0) || !std::isfinite(la) || !std::isfinite(lb)) return 0.0;
  const D3 c = cross3(a, b);
  return std::sqrt(dot3(c, c)) / (la * lb);
}

f64 degrees(f64 radians) noexcept { return radians * (180.0 / k_pi); }

// The verdict on one engine answer against the expectation: validity exactly, and a valid normal
// unit length and within the case's tolerance of the expected direction; an invalid one (the
// reference kept) within the tolerance of the expected reference.
bool matches(bool valid, Vec3 normal, bool expected_valid, f64 angle, f64 tolerance_deg) noexcept {
  if (valid != expected_valid) return false;
  if (std::isnan(angle) || angle > tolerance_deg) return false;
  return !valid || unit_length(normal);
}

D3 unit3(D3 v) noexcept {
  const f64 length = std::sqrt(dot3(v, v));
  return length > 0.0 && std::isfinite(length) ? scale3(v, 1.0 / length) : D3{};
}

// ---- the tally ---------------------------------------------------------------------------------

enum Category : u32 {
  k_tangents = 0,
  k_cancellation = 1,
  k_collapse = 2,
  k_nonfinite = 3,
  k_facet = 4,
  k_category_count = 5
};
const char* const k_category_names[k_category_count] = {"tangents", "interpolation_cancellation",
                                                        "exact_dense_face_collapse", "nonfinite",
                                                        "facet_states"};

struct Tally {
  u32 cases = 0;
  u32 matches = 0;
};

struct Results {
  JsonValue cases = JsonValue::array();
  JsonValue mismatched = JsonValue::array();
  Tally by_category[k_category_count];
  Tally rule;  // the reference-rule path, where a case has one
  Rounding rounding;
};

void record(Results& results, Category category, const std::string& id, bool match, f64 angle,
            bool valid_matches, JsonValue&& entry) {
  Tally& t = results.by_category[category];
  ++t.cases;
  if (match) {
    ++t.matches;
  } else {
    JsonValue m = JsonValue::object();
    m.set("id", JsonValue(id));
    m.set("category", JsonValue(k_category_names[category]));
    m.set("angle_deg", angle_json(angle));
    m.set("valid_matches", JsonValue(valid_matches));
    results.mismatched.push_back(std::move(m));
  }
  results.cases.push_back(std::move(entry));
}

JsonValue tally_json(const Tally& t) {
  JsonValue out = JsonValue::object();
  out.set("cases", JsonValue(t.cases));
  out.set("matches", JsonValue(t.matches));
  out.set("mismatches", JsonValue(t.cases - t.matches));
  return out;
}

// ---- tangents: the vertex stage of the limit-interpolated rule ---------------------------------

struct TangentCase {
  D3 u;
  D3 v;
  D3 reference;
  D3 expected;
  bool valid = false;
  JsonValue note;  // echoed when the case carries one
};

void evaluate_tangents(Results& results, Category category, const std::string& id,
                       const TangentCase& c, const char* note) {
  Rounding rounding;
  const Vec3 u = rounding.take(c.u);
  const Vec3 v = rounding.take(c.v);
  const Vec3 reference = rounding.take(c.reference);
  results.rounding.merge(rounding);
  // The conditioning of a valid expectation: the sine between the tangents the engine read. An
  // expected fallback is the reference itself, of order one.
  const f64 sigma = c.valid ? sine_between(d3(u), d3(v)) : 1.0;
  const f64 tolerance = degrees(tolerance_rad(sigma));

  // The rule on the case's tangents, as a reference evaluation takes them.
  Vec3 rule_normal{};
  Vector<u32> invalid;
  geometry::limit_reference_normals(std::span<const Vec3>(&u, 1), std::span<const Vec3>(&v, 1),
                                    std::span<Vec3>(&rule_normal, 1), &invalid);
  const bool rule_valid = invalid.empty();
  const f64 rule_angle = angle_deg(d3(rule_normal), c.expected);
  const bool rule_match = rule_valid == c.valid &&
                          (!c.valid || matches(true, rule_normal, true, rule_angle, tolerance));
  ++results.rule.cases;
  if (rule_match) ++results.rule.matches;

  // The state: a reference frame whose normal is the case's reference, at the case's own scale,
  // and the case's tangents as the state, through the engine's state evaluation.
  Vec3 tu{};
  Vec3 tv{};
  const f64 scale = frame_scale(d3(u), d3(v));
  reference_frame(d3(reference), scale, tu, tv);
  Vec3 reference_normal{};
  Vector<u32> reference_invalid;
  geometry::limit_reference_normals(std::span<const Vec3>(&tu, 1), std::span<const Vec3>(&tv, 1),
                                    std::span<Vec3>(&reference_normal, 1), &reference_invalid);
  const bool reference_valid = reference_invalid.empty();
  const Vec3 du = u - tu;
  const Vec3 dv = v - tv;
  Vec3 change{};
  Vector<u32> fallback;
  geometry::limit_normal_changes(std::span<const Vec3>(&tu, 1), std::span<const Vec3>(&tv, 1),
                                 std::span<const Vec3>(&du, 1), std::span<const Vec3>(&dv, 1),
                                 std::span<Vec3>(&change, 1), &fallback);
  const Vec3 normal = reference_normal + change;
  const bool fell_back = !fallback.empty();
  const bool valid = reference_valid && !fell_back;
  const f64 angle = angle_deg(d3(normal), c.expected);
  const bool valid_matches = valid == c.valid;
  const bool match = matches(valid, normal, c.valid, angle, tolerance);

  JsonValue e = JsonValue::object();
  e.set("id", JsonValue(id));
  e.set("category", JsonValue(k_category_names[category]));
  e.set("conditioning", JsonValue(sigma));
  e.set("tolerance_deg", JsonValue(tolerance));
  if (note != nullptr) e.set("constructed", JsonValue(note));
  JsonValue input = JsonValue::object();
  input.set("u", d3_json(c.u));
  input.set("v", d3_json(c.v));
  input.set("reference", d3_json(c.reference));
  input.set("f32_exact", JsonValue(rounding.exact));
  if (!c.note.is_null()) input.set("note", c.note);
  e.set("input", std::move(input));
  JsonValue expected = JsonValue::object();
  expected.set("valid", JsonValue(c.valid));
  expected.set("normal", d3_json(c.expected));
  e.set("expected", std::move(expected));
  JsonValue engine = JsonValue::object();
  engine.set("path", JsonValue("limit_normal_changes"));
  engine.set("valid", JsonValue(valid));
  engine.set("normal", vec3_json(normal));
  engine.set("normal_length", JsonValue(std::sqrt(dot3(d3(normal), d3(normal)))));
  engine.set("fell_back", JsonValue(fell_back));
  JsonValue frame = JsonValue::object();
  frame.set("scale", JsonValue(scale));
  frame.set("tangent_u", vec3_json(tu));
  frame.set("tangent_v", vec3_json(tv));
  frame.set("normal", vec3_json(reference_normal));
  frame.set("valid", JsonValue(reference_valid));
  frame.set("state_tangents_exact", JsonValue(same_bits(tu + du, u) && same_bits(tv + dv, v)));
  engine.set("reference_frame", std::move(frame));
  JsonValue rule = JsonValue::object();
  rule.set("path", JsonValue("limit_reference_normals"));
  rule.set("valid", JsonValue(rule_valid));
  rule.set("normal", vec3_json(rule_normal));
  rule.set("angle_deg", angle_json(rule_angle));
  rule.set("match", JsonValue(rule_match));
  engine.set("rule", std::move(rule));
  e.set("engine", std::move(engine));
  e.set("angle_deg", angle_json(angle));
  e.set("valid_matches", JsonValue(valid_matches));
  e.set("match", JsonValue(match));
  record(results, category, id, match, angle, valid_matches, std::move(e));
}

bool read_tangent_case(const JsonValue& value, const std::string& id, TangentCase& out,
                       std::string& error) {
  if (!value.is_object()) {
    error = id + " is not an object";
    return false;
  }
  const JsonValue* valid = value.find("valid");
  if (!read_d3(value.find("u"), out.u) || !read_d3(value.find("v"), out.v) ||
      !read_d3(value.find("reference"), out.reference) ||
      !read_d3(value.find("expected"), out.expected) || valid == nullptr ||
      !valid->get_bool(out.valid)) {
    error = id + " needs u, v, reference and expected as three numbers each, and valid";
    return false;
  }
  if (const JsonValue* note = value.find("note"); note != nullptr) out.note = *note;
  return true;
}

// ---- an interpolation of unit vertex normals ---------------------------------------------------

bool evaluate_cancellation(Results& results, const std::string& id, const JsonValue& value,
                           std::string& error) {
  const JsonValue* normals = value.is_object() ? value.find("normals") : nullptr;
  const JsonValue* valid_json = value.is_object() ? value.find("valid") : nullptr;
  D3 n[3];
  D3 bary;
  D3 reference;
  D3 expected;
  bool expected_valid = false;
  if (normals == nullptr || !normals->is_array() || normals->size() != 3 ||
      !read_d3(&(*normals)[0], n[0]) || !read_d3(&(*normals)[1], n[1]) ||
      !read_d3(&(*normals)[2], n[2]) || !read_d3(value.find("barycentric"), bary) ||
      !read_d3(value.find("reference"), reference) || !read_d3(value.find("expected"), expected) ||
      valid_json == nullptr || !valid_json->get_bool(expected_valid)) {
    error = id +
            " needs three normals, barycentric, reference and expected as three numbers each, "
            "and valid";
    return false;
  }
  Rounding rounding;
  const Vec3 state[3] = {rounding.take(n[0]), rounding.take(n[1]), rounding.take(n[2])};
  const Vec3 b = rounding.take(bary);
  rounding.take(reference);
  results.rounding.merge(rounding);
  // The reference vertex normals are unit vectors, as the engine's normal fields hold them.
  Rounding unused;
  const Vec3 unit_reference = unused.take(unit3(reference));
  // The conditioning of a valid expectation: the length of the interpolation the engine read.
  D3 m{};
  for (u32 k = 0; k < 3; ++k) {
    const f64 w =
        k == 0 ? static_cast<f64>(b.x) : (k == 1 ? static_cast<f64>(b.y) : static_cast<f64>(b.z));
    m = D3{m.x + w * static_cast<f64>(state[k].x), m.y + w * static_cast<f64>(state[k].y),
           m.z + w * static_cast<f64>(state[k].z)};
  }
  const f64 sigma = expected_valid ? std::sqrt(dot3(m, m)) : 1.0;
  const f64 tolerance = degrees(tolerance_rad(sigma));

  const geometry::NormalMode mode = geometry::NormalMode::limit_interpolated;
  const u32 faces[3] = {0, 1, 2};
  const Vec3 zero[3] = {};
  // The rule: the case's normals as the reference's own vertex normals.
  const geometry::BindingSurface rule_surface{faces, zero, zero, state, zero};
  const Vec3 rule_normal = geometry::footpoint_normal(mode, rule_surface, 0, b);
  const bool rule_valid = !(rule_normal == Vec3{});
  const f64 rule_angle = angle_deg(d3(rule_normal), expected);
  const bool rule_match =
      rule_valid == expected_valid &&
      (!expected_valid || matches(true, rule_normal, true, rule_angle, tolerance));
  ++results.rule.cases;
  if (rule_match) ++results.rule.matches;

  // The state: every corner's reference normal is the case's reference, and its change is what
  // turns it into the case's normal.
  const Vec3 corner_reference[3] = {unit_reference, unit_reference, unit_reference};
  const Vec3 corner_change[3] = {state[0] - unit_reference, state[1] - unit_reference,
                                 state[2] - unit_reference};
  const geometry::BindingSurface surface{faces, zero, zero, corner_reference, corner_change};
  const Vec3 foot_reference = geometry::footpoint_normal(mode, surface, 0, b);
  bool fell_back = false;
  const Vec3 change = geometry::footpoint_normal_change(mode, surface, 0, b, &fell_back);
  const Vec3 normal = foot_reference + change;
  const bool reference_valid = !(foot_reference == Vec3{});
  const bool valid = reference_valid && !fell_back;
  const f64 angle = angle_deg(d3(normal), expected);
  const bool valid_matches = valid == expected_valid;
  const bool match = matches(valid, normal, expected_valid, angle, tolerance);

  JsonValue e = JsonValue::object();
  e.set("id", JsonValue(id));
  e.set("category", JsonValue(k_category_names[k_cancellation]));
  e.set("conditioning", JsonValue(sigma));
  e.set("tolerance_deg", JsonValue(tolerance));
  JsonValue input = JsonValue::object();
  JsonValue normals_json = JsonValue::array();
  for (const D3& v : n)
    normals_json.push_back(d3_json(v));
  input.set("normals", std::move(normals_json));
  input.set("barycentric", d3_json(bary));
  input.set("reference", d3_json(reference));
  input.set("f32_exact", JsonValue(rounding.exact));
  if (const JsonValue* note = value.find("note"); note != nullptr) input.set("note", *note);
  e.set("input", std::move(input));
  JsonValue expected_json = JsonValue::object();
  expected_json.set("valid", JsonValue(expected_valid));
  expected_json.set("normal", d3_json(expected));
  e.set("expected", std::move(expected_json));
  JsonValue engine = JsonValue::object();
  engine.set("path", JsonValue("footpoint_normal + footpoint_normal_change"));
  engine.set("mode", JsonValue(geometry::normal_mode_name(mode)));
  engine.set("valid", JsonValue(valid));
  engine.set("normal", vec3_json(normal));
  engine.set("normal_length", JsonValue(std::sqrt(dot3(d3(normal), d3(normal)))));
  engine.set("fell_back", JsonValue(fell_back));
  engine.set("reference_normal", vec3_json(foot_reference));
  engine.set("reference_valid", JsonValue(reference_valid));
  JsonValue rule = JsonValue::object();
  rule.set("path", JsonValue("footpoint_normal"));
  rule.set("valid", JsonValue(rule_valid));
  rule.set("normal", vec3_json(rule_normal));
  rule.set("angle_deg", angle_json(rule_angle));
  rule.set("match", JsonValue(rule_match));
  engine.set("rule", std::move(rule));
  e.set("engine", std::move(engine));
  e.set("angle_deg", angle_json(angle));
  e.set("valid_matches", JsonValue(valid_matches));
  e.set("match", JsonValue(match));
  record(results, k_cancellation, id, match, angle, valid_matches, std::move(e));
  return true;
}

// ---- a dense face collapsed exactly ------------------------------------------------------------

bool evaluate_collapse(Results& results, const std::string& id, const JsonValue& value,
                       std::string& error) {
  if (!value.is_object()) {
    error = id + " is not an object";
    return false;
  }
  const JsonValue* positions = value.find("positions");
  const JsonValue* face_list = value.find("faces");
  if (positions == nullptr || !positions->is_array() || positions->size() == 0 ||
      face_list == nullptr || !face_list->is_array() || face_list->size() == 0) {
    error = id + " needs positions and faces";
    return false;
  }
  Rounding rounding;
  Vector<D3> written;
  Vector<Vec3> points;
  for (usize i = 0; i < positions->size(); ++i) {
    D3 p;
    if (!read_d3(&(*positions)[i], p) || !finite3(p)) {
      error = id + ".positions[" + std::to_string(i) + "] is not three finite numbers";
      return false;
    }
    written.push_back(p);
    points.push_back(rounding.take(p));
  }
  Vector<u32> faces;
  for (usize i = 0; i < face_list->size(); ++i) {
    const JsonValue& f = (*face_list)[i];
    for (usize k = 0; k < 3; ++k) {
      u64 index = 0;
      if (!f.is_array() || f.size() != 3 || !f[k].get_u64(index) || index >= points.size()) {
        error = id + ".faces[" + std::to_string(i) + "] is not three position indices";
        return false;
      }
      faces.push_back(static_cast<u32>(index));
    }
  }
  const u32 triangle_count = static_cast<u32>(faces.size() / 3);
  u64 foot = 0;
  D3 bary;
  D3 expected_area;
  bool expected_facet_valid = false;
  const JsonValue* foot_json = value.find("foot_triangle");
  const JsonValue* facet_json = value.find("facet_valid");
  if (foot_json == nullptr || !foot_json->get_u64(foot) || foot >= triangle_count ||
      !read_d3(value.find("barycentric"), bary) || facet_json == nullptr ||
      !facet_json->get_bool(expected_facet_valid) ||
      !read_d3(value.find("area_weighted_normal"), expected_area)) {
    error = id +
            " needs foot_triangle (one of its faces), barycentric, facet_valid and "
            "area_weighted_normal";
    return false;
  }
  const Vec3 b = rounding.take(bary);
  results.rounding.merge(rounding);
  const bool expected_area_valid =
      finite3(expected_area) && dot3(expected_area, expected_area) > 0.0;

  const u32 count = static_cast<u32>(points.size());
  Vector<Vec3> zero(count, Vec3{});
  Vector<Vec3> vectors(count);
  Vector<Vec3> normals(count);
  Vector<u32> invalid;
  geometry::area_weighted_normal_vectors(faces, points, std::span<Vec3>(vectors.data(), count));
  geometry::area_weighted_reference_normals(vectors, std::span<Vec3>(normals.data(), count),
                                            &invalid);
  const geometry::BindingSurface surface{faces, points, zero, normals, zero};
  const u32 t = static_cast<u32>(foot);
  const Vec3 facet = geometry::footpoint_normal(geometry::NormalMode::triangle, surface, t, b);
  const Vec3 area = geometry::footpoint_normal(
      geometry::NormalMode::interpolated_vertex_area_weighted, surface, t, b);
  const bool facet_valid = !(facet == Vec3{});
  const bool area_valid = !(area == Vec3{});
  const f64 angle = angle_deg(d3(area), expected_area);
  // The dense case is exact integer geometry whose vertex normals do not cancel: of order one.
  const f64 tolerance = degrees(tolerance_rad(1.0));
  const bool valid_matches =
      facet_valid == expected_facet_valid && area_valid == expected_area_valid;
  const bool match = facet_valid == expected_facet_valid &&
                     matches(area_valid, area, expected_area_valid, angle, tolerance);
  // A diagnostic of the runner's own, not the engine's rule: the sine of the facet's corner angle
  // at its first vertex, in double from the written coordinates, which is where a threshold on unit
  // edges (the authoring side's facet test) would judge the facet.
  const D3 p0 = written[faces[3 * t]];
  const D3 e1 = sub3(written[faces[3 * t + 1]], p0);
  const D3 e2 = sub3(written[faces[3 * t + 2]], p0);
  const f64 edges = std::sqrt(dot3(e1, e1) * dot3(e2, e2));
  const D3 c = cross3(e1, e2);
  const f64 sine = edges > 0.0 ? std::sqrt(dot3(c, c)) / edges : 0.0;

  JsonValue e = JsonValue::object();
  e.set("id", JsonValue(id));
  e.set("category", JsonValue(k_category_names[k_collapse]));
  e.set("conditioning", JsonValue(1.0));
  e.set("tolerance_deg", JsonValue(tolerance));
  JsonValue input = JsonValue::object();
  input.set("positions", *positions);
  input.set("faces", *face_list);
  input.set("foot_triangle", JsonValue(foot));
  input.set("barycentric", d3_json(bary));
  input.set("f32_exact", JsonValue(rounding.exact));
  if (const JsonValue* note = value.find("note"); note != nullptr) input.set("note", *note);
  e.set("input", std::move(input));
  JsonValue expected_json = JsonValue::object();
  expected_json.set("facet_valid", JsonValue(expected_facet_valid));
  expected_json.set("area_weighted_valid", JsonValue(expected_area_valid));
  expected_json.set("normal", d3_json(expected_area));
  e.set("expected", std::move(expected_json));
  JsonValue engine = JsonValue::object();
  engine.set("path",
             JsonValue("footpoint_normal (triangle, interpolated-vertex-area-weighted) at the "
                       "reference"));
  engine.set("facet_valid", JsonValue(facet_valid));
  engine.set("facet_normal", vec3_json(facet));
  engine.set("facet_sine", JsonValue(sine));
  engine.set("area_weighted_valid", JsonValue(area_valid));
  engine.set("normal", vec3_json(area));
  JsonValue vertex_normals = JsonValue::array();
  for (const Vec3& n : normals)
    vertex_normals.push_back(vec3_json(n));
  engine.set("vertex_normals", std::move(vertex_normals));
  JsonValue invalid_json = JsonValue::array();
  for (const u32 i : invalid)
    invalid_json.push_back(JsonValue(i));
  engine.set("invalid_vertices", std::move(invalid_json));
  e.set("engine", std::move(engine));
  e.set("angle_deg", angle_json(angle));
  e.set("valid_matches", JsonValue(valid_matches));
  e.set("match", JsonValue(match));
  record(results, k_collapse, id, match, angle, valid_matches, std::move(e));
  return true;
}

// ---- a facet at a state: the triangle mode's footpoint normal ----------------------------------

// An engine-side case kind (the authoring side's file has none): a footpoint triangle's corners at
// the reference and at a state, through the transfer's own facet code — `footpoint_normal` plus
// `footpoint_normal_change` in `triangle` mode, the displacement being the state's corners minus
// the reference's. It carries the two defects the D8 audit found in that code (the scalar
// cancellation counterexample and record 35 of the isolated face collapse), and their rescalings.
bool evaluate_facet(Results& results, const std::string& id, const JsonValue& value,
                    std::string& error) {
  const JsonValue* reference_json = value.is_object() ? value.find("reference") : nullptr;
  const JsonValue* state_json = value.is_object() ? value.find("state") : nullptr;
  const JsonValue* valid_json = value.is_object() ? value.find("valid") : nullptr;
  D3 reference[3];
  D3 state[3];
  D3 bary{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
  D3 expected;
  bool expected_valid = false;
  bool ok = reference_json != nullptr && reference_json->is_array() &&
            reference_json->size() == 3 && state_json != nullptr && state_json->is_array() &&
            state_json->size() == 3 && read_d3(value.find("expected"), expected) &&
            valid_json != nullptr && valid_json->get_bool(expected_valid);
  for (usize k = 0; ok && k < 3; ++k)
    ok = read_d3(&(*reference_json)[k], reference[k]) && read_d3(&(*state_json)[k], state[k]) &&
         finite3(reference[k]) && finite3(state[k]);
  if (ok && value.find("barycentric") != nullptr) ok = read_d3(value.find("barycentric"), bary);
  if (!ok) {
    error = id +
            " needs reference and state as three finite corners each, expected as three "
            "numbers, and valid";
    return false;
  }
  Rounding rounding;
  Vec3 corners[3];
  Vec3 displacement[3];
  bool state_exact = true;
  for (u32 k = 0; k < 3; ++k) {
    corners[k] = rounding.take(reference[k]);
    const Vec3 s = rounding.take(state[k]);
    displacement[k] = s - corners[k];
    state_exact = state_exact && same_bits(corners[k] + displacement[k], s);
  }
  const Vec3 b = rounding.take(bary);
  results.rounding.merge(rounding);
  Vec3 state_corners[3];
  for (u32 k = 0; k < 3; ++k)
    state_corners[k] = corners[k] + displacement[k];

  // Diagnostics of the runner's own, in double from the f32 corners the engine read: the two cross
  // products, the state's length as a fraction of the reference's (what the state is judged on,
  // k_normal_degenerate_ratio, and where the expansion's accuracy used to run out), and the sine
  // of the state's corner angle, which is the conditioning of its normal.
  const D3 r0 = d3(corners[0]);
  const D3 s0 = d3(state_corners[0]);
  const D3 c = cross3(sub3(d3(corners[1]), r0), sub3(d3(corners[2]), r0));
  const D3 se1 = sub3(d3(state_corners[1]), s0);
  const D3 se2 = sub3(d3(state_corners[2]), s0);
  const D3 sc = cross3(se1, se2);
  const f64 c_length = std::sqrt(dot3(c, c));
  const f64 ratio = c_length > 0.0 ? std::sqrt(dot3(sc, sc)) / c_length : k_nan;
  const f64 sigma = expected_valid ? sine_between(se1, se2) : 1.0;
  const f64 tolerance = degrees(tolerance_rad(sigma));

  const geometry::NormalMode mode = geometry::NormalMode::triangle;
  const u32 faces[3] = {0, 1, 2};
  const Vec3 zero[3] = {};
  const geometry::BindingSurface surface{faces, corners, displacement, zero, zero};
  const Vec3 facet_reference = geometry::footpoint_normal(mode, surface, 0, b);
  bool fell_back = false;
  const Vec3 change = geometry::footpoint_normal_change(mode, surface, 0, b, &fell_back);
  const Vec3 normal = facet_reference + change;
  const bool reference_valid = !(facet_reference == Vec3{});
  const bool valid = reference_valid && !fell_back;
  const f64 angle = angle_deg(d3(normal), expected);
  const bool valid_matches = valid == expected_valid;
  const bool match = matches(valid, normal, expected_valid, angle, tolerance);

  // The rule: the state's corners as a reference evaluation takes them. It has a direction to
  // compare only where the case expects one: a facet state's fallback is relative to its reference
  // (k_normal_degenerate_ratio), and a reference evaluation, which has no reference of its own,
  // calls a facet invalid only when its cross product is exactly zero — so for an expected
  // fallback the rule path is reported and not counted.
  const geometry::BindingSurface state_surface{faces, state_corners, zero, zero, zero};
  const Vec3 rule_normal = geometry::footpoint_normal(mode, state_surface, 0, b);
  const bool rule_valid = !(rule_normal == Vec3{});
  const f64 rule_angle = angle_deg(d3(rule_normal), expected);
  const bool rule_counted = expected_valid;
  const bool rule_match =
      rule_counted && matches(rule_valid, rule_normal, true, rule_angle, tolerance);
  if (rule_counted) {
    ++results.rule.cases;
    if (rule_match) ++results.rule.matches;
  }

  JsonValue e = JsonValue::object();
  e.set("id", JsonValue(id));
  e.set("category", JsonValue(k_category_names[k_facet]));
  e.set("conditioning", JsonValue(sigma));
  e.set("tolerance_deg", JsonValue(tolerance));
  JsonValue input = JsonValue::object();
  JsonValue reference_out = JsonValue::array();
  JsonValue state_out = JsonValue::array();
  for (u32 k = 0; k < 3; ++k) {
    reference_out.push_back(d3_json(reference[k]));
    state_out.push_back(d3_json(state[k]));
  }
  input.set("reference", std::move(reference_out));
  input.set("state", std::move(state_out));
  input.set("barycentric", d3_json(bary));
  input.set("f32_exact", JsonValue(rounding.exact));
  if (const JsonValue* note = value.find("note"); note != nullptr) input.set("note", *note);
  e.set("input", std::move(input));
  JsonValue expected_json = JsonValue::object();
  expected_json.set("valid", JsonValue(expected_valid));
  expected_json.set("normal", d3_json(expected));
  e.set("expected", std::move(expected_json));
  JsonValue engine = JsonValue::object();
  engine.set("path", JsonValue("footpoint_normal + footpoint_normal_change"));
  engine.set("mode", JsonValue(geometry::normal_mode_name(mode)));
  engine.set("valid", JsonValue(valid));
  engine.set("normal", vec3_json(normal));
  engine.set("normal_length", JsonValue(std::sqrt(dot3(d3(normal), d3(normal)))));
  engine.set("fell_back", JsonValue(fell_back));
  engine.set("reference_normal", vec3_json(facet_reference));
  engine.set("reference_valid", JsonValue(reference_valid));
  engine.set("state_exact", JsonValue(state_exact));
  engine.set("reference_cross", d3_json(c));
  engine.set("state_cross", d3_json(sc));
  engine.set("state_length_ratio", number_json(ratio));
  JsonValue rule = JsonValue::object();
  rule.set("path", JsonValue("footpoint_normal on the state's corners"));
  rule.set("valid", JsonValue(rule_valid));
  rule.set("normal", vec3_json(rule_normal));
  rule.set("angle_deg", angle_json(rule_angle));
  rule.set("match", rule_counted ? JsonValue(rule_match) : JsonValue());
  engine.set("rule", std::move(rule));
  e.set("engine", std::move(engine));
  e.set("angle_deg", angle_json(angle));
  e.set("valid_matches", JsonValue(valid_matches));
  e.set("match", JsonValue(match));
  record(results, k_facet, id, match, angle, valid_matches, std::move(e));
  return true;
}

// Calls `evaluate` on a case written as one object or as an array of them, naming each.
template <typename Evaluate>
bool each_case(const JsonValue& value, const char* name, Evaluate&& evaluate, std::string& error) {
  if (value.is_array()) {
    for (usize i = 0; i < value.size(); ++i)
      if (!evaluate(std::string(name) + "[" + std::to_string(i) + "]", value[i], error))
        return false;
    return true;
  }
  return evaluate(std::string(name), value, error);
}

}  // namespace

int normal_cases_command(int argc, char** argv) {
  std::string input;
  std::string output;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      std::fputs(k_cases_usage, stdout);
      return k_exit_ok;
    }
    if (a == "--out") {
      if (i + 1 >= argc) return usage("--out needs a value");
      output = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (input.empty()) {
      input = std::string(a);
    } else {
      return usage("one cases file at a time");
    }
  }
  if (input.empty() || output.empty()) return usage("a cases file and --out are required");

  std::string text;
  const io::Status status = io::read_file(input, text);
  if (status != io::Status::Ok)
    return failed("cannot read '" + input + "': " + io::status_name(status));
  JsonValue json;
  const JsonParseResult parsed = parse_json(text, json);
  if (!parsed.ok || !json.is_object())
    return failed(input + " is not a JSON object: " + parsed.message + " at line " +
                  std::to_string(parsed.line));

  Results results;
  std::string error;
  bool any = false;
  if (const JsonValue* tangents = json.find("tangents"); tangents != nullptr) {
    any = true;
    const bool ok = each_case(
        *tangents, "tangents",
        [&](const std::string& id, const JsonValue& value, std::string& why) {
          TangentCase c;
          if (!read_tangent_case(value, id, c, why)) return false;
          evaluate_tangents(results, k_tangents, id, c, nullptr);
          return true;
        },
        error);
    if (!ok) return failed(error);
  }
  if (const JsonValue* cancel = json.find("interpolation_cancellation"); cancel != nullptr) {
    any = true;
    const bool ok = each_case(
        *cancel, "interpolation_cancellation",
        [&](const std::string& id, const JsonValue& value, std::string& why) {
          return evaluate_cancellation(results, id, value, why);
        },
        error);
    if (!ok) return failed(error);
  }
  if (const JsonValue* collapse = json.find("exact_dense_face_collapse"); collapse != nullptr) {
    any = true;
    const bool ok = each_case(
        *collapse, "exact_dense_face_collapse",
        [&](const std::string& id, const JsonValue& value, std::string& why) {
          return evaluate_collapse(results, id, value, why);
        },
        error);
    if (!ok) return failed(error);
  }
  if (const JsonValue* facets = json.find("facet_states"); facets != nullptr) {
    any = true;
    const bool ok = each_case(
        *facets, "facet_states",
        [&](const std::string& id, const JsonValue& value, std::string& why) {
          return evaluate_facet(results, id, value, why);
        },
        error);
    if (!ok) return failed(error);
  }
  // The file asks the harness for NaN and infinite tangents rather than writing them, since JSON
  // has no number for either: one of each in u, and in v, with reference +z, which the rule must
  // answer with the reference (the authoring side's harness builds the first two the same way).
  const JsonValue* nonfinite = json.find("nonfinite_cases");
  if (nonfinite != nullptr) {
    any = true;
    const D3 x{1.0, 0.0, 0.0};
    const D3 y{0.0, 1.0, 0.0};
    const D3 z{0.0, 0.0, 1.0};
    const TangentCase built[4] = {
        {D3{k_nan, 0.0, 0.0}, y, z, z, false, JsonValue()},
        {D3{k_inf, 0.0, 0.0}, y, z, z, false, JsonValue()},
        {x, D3{0.0, k_nan, 0.0}, z, z, false, JsonValue()},
        {x, D3{0.0, -k_inf, 0.0}, z, z, false, JsonValue()},
    };
    for (u32 i = 0; i < 4; ++i)
      evaluate_tangents(results, k_nonfinite, "nonfinite[" + std::to_string(i) + "]", built[i],
                        "built by the runner, as nonfinite_cases asks");
  }
  if (!any)
    return failed(input +
                  " names no cases: tangents, interpolation_cancellation, "
                  "exact_dense_face_collapse, facet_states or nonfinite_cases");

  // ---- the file ---------------------------------------------------------------------------------
  char hash[32];
  std::snprintf(hash, sizeof(hash), "%016llx",
                static_cast<unsigned long long>(hash_bytes(text.data(), text.size())));
  const JsonValue* schema = json.find("schema");
  JsonValue ignored = JsonValue::array();
  for (auto [key, value] : json.as_object()) {
    (void)value;
    if (key != "schema" && key != "epsilon" && key != "tangents" &&
        key != "interpolation_cancellation" && key != "exact_dense_face_collapse" &&
        key != "facet_states" && key != "nonfinite_cases")
      ignored.push_back(JsonValue(std::string(key)));
  }

  JsonValue out = JsonValue::object();
  out.set("schema", JsonValue("engine.normal_cases.v1"));
  JsonValue source = JsonValue::object();
  source.set("path", JsonValue(input));
  source.set("bytes", JsonValue(static_cast<u64>(text.size())));
  source.set("hash_bytes", JsonValue(hash));
  if (schema != nullptr) source.set("schema", *schema);
  source.set("ignored_keys", std::move(ignored));
  if (nonfinite != nullptr) source.set("nonfinite_cases", *nonfinite);
  out.set("input", std::move(source));
  JsonValue rule = JsonValue::object();
  rule.set("mode", JsonValue(geometry::normal_mode_name(geometry::NormalMode::limit_interpolated)));
  rule.set("arithmetic", JsonValue("f32"));
  rule.set("sine", JsonValue(geometry::k_normal_degenerate_sine));
  rule.set("interpolated_length", JsonValue(geometry::k_normal_interpolated_length));
  rule.set("degenerate_ratio", JsonValue(geometry::k_normal_degenerate_ratio));
  f64 epsilon = 0.0;
  const JsonValue* epsilon_json = json.find("epsilon");
  const bool have_epsilon = epsilon_json != nullptr && epsilon_json->get_f64(epsilon);
  rule.set("input_epsilon", have_epsilon ? JsonValue(epsilon) : JsonValue());
  // The engine's thresholds are its own and are never taken from the input: a different epsilon
  // is reported here, and the cases are still evaluated against the engine's.
  rule.set(
      "input_epsilon_is_engine_sine",
      JsonValue(have_epsilon && static_cast<f32>(epsilon) == geometry::k_normal_degenerate_sine));
  out.set("engine", std::move(rule));
  JsonValue tolerance = JsonValue::object();
  tolerance.set("angle_rad", JsonValue(k_tolerance_rad));
  tolerance.set("angle_deg", JsonValue(k_tolerance_deg));
  tolerance.set("near_degenerate_rad", JsonValue("2^-21 / conditioning, where larger"));
  tolerance.set("unit_length", JsonValue(k_tolerance_rad));
  tolerance.set("valid", JsonValue("exact"));
  tolerance.set("why",
                JsonValue("64 f32 unit roundoffs (2^-18 rad): f32 inputs, the rule's normalize, "
                          "cross product and normalize, and a state's unit_change steps, all in "
                          "f32; a normal normalized out of a vector of length conditioning < 1/8 "
                          "(a sine, or an interpolation's length) carries about 2 roundoffs over "
                          "that length, and is allowed four times it; a valid normal is also unit "
                          "length to 2^-18"));
  out.set("tolerance", std::move(tolerance));
  JsonValue conversion = JsonValue::object();
  conversion.set("f32_exact", JsonValue(results.rounding.exact));
  conversion.set("max_rounding", number_json(results.rounding.worst));
  out.set("input_conversion", std::move(conversion));

  Tally total;
  JsonValue by_category = JsonValue::object();
  for (u32 k = 0; k < k_category_count; ++k) {
    const Tally& t = results.by_category[k];
    total.cases += t.cases;
    total.matches += t.matches;
    if (t.cases > 0) by_category.set(k_category_names[k], tally_json(t));
  }
  JsonValue summary = JsonValue::object();
  summary.set("cases", JsonValue(total.cases));
  summary.set("matches", JsonValue(total.matches));
  summary.set("mismatches", JsonValue(total.cases - total.matches));
  summary.set("by_category", std::move(by_category));
  summary.set("mismatched", results.mismatched);
  summary.set("rule_path", tally_json(results.rule));
  summary.set("input_schema", schema != nullptr ? *schema : JsonValue());
  summary.set("input_hash_bytes", JsonValue(hash));
  out.set("summary", summary);
  out.set("cases", std::move(results.cases));

  std::string written;
  if (!write_json(out, written, JsonWriteOptions{.pretty = false}))
    return failed("an evaluation produced a number JSON cannot hold");
  written.push_back('\n');
  const io::Status wrote = io::write_file_atomic(output, written);
  if (wrote != io::Status::Ok)
    return failed("cannot write '" + output + "': " + io::status_name(wrote));
  JsonValue line_json = JsonValue::object();
  line_json.set("input", JsonValue(input));
  line_json.set("output", JsonValue(output));
  line_json.set("bytes", JsonValue(static_cast<u64>(written.size())));
  line_json.set("input_schema", schema != nullptr ? *schema : JsonValue());
  line_json.set("input_hash_bytes", JsonValue(hash));
  line_json.set("cases", JsonValue(total.cases));
  line_json.set("matches", JsonValue(total.matches));
  line_json.set("mismatches", JsonValue(total.cases - total.matches));
  JsonValue ids = JsonValue::array();
  for (usize i = 0; i < results.mismatched.size(); ++i)
    ids.push_back(*results.mismatched[i].find("id"));
  line_json.set("mismatched", std::move(ids));
  std::string line = write_json(line_json, JsonWriteOptions{.pretty = false});
  line.push_back('\n');
  std::fwrite(line.data(), 1, line.size(), stdout);
  return k_exit_ok;
}

}  // namespace engine::content
