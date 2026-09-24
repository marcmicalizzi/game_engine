// engine-content normal-cases end to end (docs/subsystems/apps.md, "engine-content normal-cases"):
// a synthetic cases file written here — the sine threshold on both sides of epsilon, a zero
// tangent, a tangent a millionfold shorter than its reference, interpolations that cancel exactly
// and nearly, a dense face collapsed exactly, a facet collapsing to 1e-5 of its area (the D8
// audit's scalar counterexample) and one past the threshold, record 35 of the authoring side's
// isolated face collapse, the same geometry at 1e-3, 1e3, 1e-30 and 1e30, and the non-finite
// tangents the runner builds — with every expectation derived from the rule as documented
// (docs/subsystems/geometry.md, "The footpoint normal"), then the results file read back case by
// case. None of this is the authoring side's file.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
  JsonValue result;
};

Run content(std::vector<std::string> args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-content: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  if (!run.output.empty()) (void)parse_json(run.output, run.result);
  return run;
}

struct P {
  f64 x, y, z;
};

JsonValue vec(P p) {
  JsonValue v = JsonValue::array();
  v.push_back(JsonValue(p.x));
  v.push_back(JsonValue(p.y));
  v.push_back(JsonValue(p.z));
  return v;
}

P scaled(P p, f64 s) { return P{p.x * s, p.y * s, p.z * s}; }

P unit(P p) {
  const f64 l = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
  return P{p.x / l, p.y / l, p.z / l};
}

P cross(P a, P b) { return P{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

P sub(P a, P b) { return P{a.x - b.x, a.y - b.y, a.z - b.z}; }

P f32_exact(P p) {
  return P{static_cast<f64>(static_cast<f32>(p.x)), static_cast<f64>(static_cast<f32>(p.y)),
           static_cast<f64>(static_cast<f32>(p.z))};
}

// The two kinds of case the 2026-09-24 fix separates, named in each case's note: (a) a live state,
// above every threshold on its direct length, which must come back as its own unit normal with no
// fallback — the kind the expansion used to lose; (b) a state that is degenerate on its direct
// length, which must come back as the reference normal with the fallback reported.
constexpr const char* k_live = "(a) live: its own unit normal, no fallback";
constexpr const char* k_degenerate =
    "(b) degenerate on its direct length: the reference, a fallback";

JsonValue tangent_case(P u, P v, P reference, bool valid, P expected, const char* note) {
  JsonValue c = JsonValue::object();
  c.set("u", vec(u));
  c.set("v", vec(v));
  c.set("reference", vec(reference));
  c.set("valid", JsonValue(valid));
  c.set("expected", vec(expected));
  c.set("note", JsonValue(note));
  return c;
}

JsonValue facet_case(const P (&reference)[3], const P (&state)[3], P bary, bool valid, P expected,
                     const char* note) {
  JsonValue c = JsonValue::object();
  JsonValue r = JsonValue::array();
  JsonValue s = JsonValue::array();
  for (u32 k = 0; k < 3; ++k) {
    r.push_back(vec(reference[k]));
    s.push_back(vec(state[k]));
  }
  c.set("reference", std::move(r));
  c.set("state", std::move(s));
  c.set("barycentric", vec(bary));
  c.set("valid", JsonValue(valid));
  c.set("expected", vec(expected));
  c.set("note", JsonValue(note));
  return c;
}

// Record 35 of the authoring side's isolated face collapse (study019's canonical vertex 1180,
// refined triangle 6, corners 457, 2449 and 4419): the refined limit positions at the reference
// and at that state as the engine's level-3 dump of the control set wrote them (f32 values), and
// the record's barycentrics. The state puts corner 2449 on the line through the other two, exactly
// in the authoring side's f64 construction and to 5.5e-5 of the reference's area in f32.
constexpr P k_record35_reference[3] = {
    {0.09384861588478088, -0.11602731049060822, 1.2347452640533447},
    {0.0961017906665802, -0.11456994712352753, 1.234917163848877},
    {0.09509064257144928, -0.11465424299240112, 1.2354576587677002}};
constexpr P k_record35_state[3] = {{0.09384860843420029, -0.11602731049060822, 1.2347451448440552},
                                   {0.09446962922811508, -0.11534078419208527, 1.2351014614105225},
                                   {0.09509064257144928, -0.11465424299240112, 1.2354576587677002}};
constexpr P k_record35_bary = {0.03271524168229645, 0.7054432832036458, 0.2618414751140577};

// Every expectation follows from the rule as geometry.md writes it: normalize each tangent, invalid
// at a zero or non-finite tangent or a unit-tangent sine of at most 1e-6, otherwise the normalized
// cross product; a degenerate state keeps its reference normal; an interpolation of unit normals of
// length at most 1e-6 keeps the reference footpoint normal; a facet (triangle mode) at a state is
// degenerate at most 1e-6 of its reference's length and is otherwise its own normalized cross
// product. Directions do not change under a uniform rescaling.
JsonValue fixture() {
  JsonValue f = JsonValue::object();
  f.set("schema", JsonValue("engine.test.normal_cases.v1"));
  f.set("epsilon", JsonValue(1.0e-6));

  JsonValue tangents = JsonValue::array();
  // 0: sine 4e-7, below epsilon: the reference is kept.
  tangents.push_back(
      tangent_case({0, 0, 2}, {8.0e-7, 0, 2}, {1, 0, 0}, false, {1, 0, 0}, k_degenerate));
  // 1: sine 3e-6, above it: the normal turns 90 degrees from its reference to -x.
  const P above_u{0, 0, 2};
  const P above_v{0, 6.0e-6, 2};
  tangents.push_back(tangent_case(above_u, above_v, {0, 1, 0}, true, {-1, 0, 0}, k_live));
  // 2: a zero tangent.
  tangents.push_back(
      tangent_case({0, 0, 0}, {1, 0, 0}, {0, -1, 0}, false, {0, -1, 0}, k_degenerate));
  // 3: an ordinary turn, from a reference that is on no axis.
  tangents.push_back(tangent_case({0, 1, 0}, {0, 0, 1}, {0.6, 0, 0.8}, true, {1, 0, 0}, k_live));
  // 4: a tangent 2^-24 of its reference's length that keeps a direction: nonzero, so valid.
  const f64 tiny = std::ldexp(1.0, -24);
  const f64 half = std::sqrt(0.5);
  tangents.push_back(
      tangent_case({tiny, tiny, 0}, {0, 0, 1}, {0, 0, 1}, true, {half, -half, 0}, k_live));
  // 5 to 8: case 1 at 1e-3, 1e3, 1e-30 and 1e30 — the same decision and the same normal.
  for (const f64 s : {1.0e-3, 1.0e3, 1.0e-30, 1.0e30})
    tangents.push_back(
        tangent_case(scaled(above_u, s), scaled(above_v, s), {0, 1, 0}, true, {-1, 0, 0}, k_live));
  f.set("tangents", std::move(tangents));

  JsonValue cancel = JsonValue::array();
  {
    // 0: corners' normals that the weights sum to nothing, exactly.
    JsonValue c = JsonValue::object();
    JsonValue n = JsonValue::array();
    n.push_back(vec({0, 1, 0}));
    n.push_back(vec({0, 1, 0}));
    n.push_back(vec({0, -1, 0}));
    c.set("normals", std::move(n));
    c.set("barycentric", vec({0.25, 0.25, 0.5}));
    c.set("reference", vec({1, 0, 0}));
    c.set("valid", JsonValue(false));
    c.set("expected", vec({1, 0, 0}));
    c.set("note", JsonValue(k_degenerate));
    cancel.push_back(std::move(c));
    // 1: nearly: an interpolation of length 1e-5, above the threshold, which turns to +y.
    JsonValue d = JsonValue::object();
    JsonValue m = JsonValue::array();
    m.push_back(vec({0, 1, 0}));
    m.push_back(vec({0, -1, 0}));
    m.push_back(vec({1, 0, 0}));
    d.set("normals", std::move(m));
    d.set("barycentric", vec({0.500005, 0.499995, 0.0}));
    d.set("reference", vec({1, 0, 0}));
    d.set("valid", JsonValue(true));
    d.set("expected", vec({0, 1, 0}));
    d.set("note", JsonValue(k_live));
    cancel.push_back(std::move(d));
  }
  f.set("interpolation_cancellation", std::move(cancel));

  // A fan whose first face is collinear exactly and whose other two tilt the same way: the facet
  // has no normal, and every corner's area-weighted normal is (0, -1, 3) / sqrt(10). At 1, 1e-3
  // and 1e3.
  JsonValue dense = JsonValue::array();
  for (const f64 s : {1.0, 1.0e-3, 1.0e3}) {
    JsonValue d = JsonValue::object();
    JsonValue positions = JsonValue::array();
    for (const P p : {P{0, 0, 0}, P{2, 0, 0}, P{4, 0, 0}, P{2, 3, 1}})
      positions.push_back(vec(scaled(p, s)));
    JsonValue faces = JsonValue::array();
    const u32 fan[3][3] = {{0, 1, 2}, {0, 1, 3}, {1, 2, 3}};
    for (const auto& t : fan) {
      JsonValue face = JsonValue::array();
      for (const u32 i : t)
        face.push_back(JsonValue(i));
      faces.push_back(std::move(face));
    }
    d.set("positions", std::move(positions));
    d.set("faces", std::move(faces));
    d.set("foot_triangle", JsonValue(u32{0}));
    d.set("barycentric", vec({0.2, 0.3, 0.5}));
    d.set("facet_valid", JsonValue(false));
    d.set("area_weighted_normal", vec(unit({0, -1, 3})));
    dense.push_back(std::move(d));
  }
  f.set("exact_dense_face_collapse", std::move(dense));

  JsonValue facets = JsonValue::array();
  const P right[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  const P third = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
  {
    // 0: the audit's scalar counterexample, c = (0, 0, 1) and dc = (0, -1e-5, -1): the third
    // corner swings down to 1e-5 above the first edge, and the facet turns to -y.
    const P state[3] = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1.0e-5}};
    facets.push_back(facet_case(right, state, third, true, {0, -1, 0},
                                "(a) live: the D8 audit's scalar cancellation counterexample"));
    // 1: the same swing to 5e-7, at or under the threshold: the reference normal is kept.
    const P gone[3] = {{0, 0, 0}, {1, 0, 0}, {0, 0, 5.0e-7}};
    facets.push_back(facet_case(right, gone, third, false, {0, 0, 1}, k_degenerate));
    // 2: record 35, live at 5.5e-5 of its reference's area: the state's own facet normal, in
    // double, of the corners the engine reads.
    P s[3];
    for (u32 k = 0; k < 3; ++k)
      s[k] = f32_exact(k_record35_state[k]);
    const P n = unit(cross(sub(s[1], s[0]), sub(s[2], s[0])));
    facets.push_back(facet_case(k_record35_reference, k_record35_state, k_record35_bary, true, n,
                                "(a) live: record 35 of the isolated face collapse"));
    // 3, 4: the counterexample at 1e-3 and 1e3.
    for (const f64 k : {1.0e-3, 1.0e3}) {
      P r[3];
      P q[3];
      for (u32 i = 0; i < 3; ++i) {
        r[i] = scaled(right[i], k);
        q[i] = scaled(state[i], k);
      }
      facets.push_back(
          facet_case(r, q, third, true, {0, -1, 0}, "(a) live: the counterexample, rescaled"));
    }
  }
  f.set("facet_states", std::move(facets));
  f.set("nonfinite_cases", JsonValue("the runner builds NaN and infinite tangents"));
  return f;
}

f64 at(const JsonValue& list, usize k) {
  f64 v = 0.0;
  list[k].get_f64(v);
  return v;
}

bool flag(const JsonValue& object, std::string_view key) {
  bool b = false;
  const JsonValue* v = object.find(key);
  return v != nullptr && v->get_bool(b) && b;
}

u64 count(const JsonValue& object, std::string_view key) {
  u64 n = 0;
  if (const JsonValue* v = object.find(key); v != nullptr) v->get_u64(n);
  return n;
}

f64 number(const JsonValue& object, std::string_view key) {
  f64 x = std::nan("");
  if (const JsonValue* v = object.find(key); v != nullptr) v->get_f64(x);
  return x;
}

const JsonValue& find_case(const JsonValue& results, std::string_view id) {
  const JsonValue& cases = *results.find("cases");
  for (usize i = 0; i < cases.size(); ++i) {
    std::string_view s;
    cases[i].find("id")->get_string(s);
    if (s == id) return cases[i];
  }
  FAIL("no case " << std::string(id));
  return cases[0];
}

// The engine's answer is the expected direction to within the case's own tolerance.
void check_normal(const JsonValue& c, P want) {
  const JsonValue& n = *c.find("engine")->find("normal");
  const P got{at(n, 0), at(n, 1), at(n, 2)};
  const P x = cross(got, want);
  const f64 angle = std::atan2(std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z),
                               got.x * want.x + got.y * want.y + got.z * want.z) *
                    180.0 / 3.14159265358979323846;
  CHECK(angle <= number(c, "tolerance_deg"));
  CHECK(std::fabs(std::sqrt(got.x * got.x + got.y * got.y + got.z * got.z) - 1.0) < 4.0e-6);
}

}  // namespace

TEST_CASE("normal-cases: every synthetic case, and what the engine answered") {
  engine::test::TempDir tmp("content_normal_cases");
  const std::string input = tmp.file("cases.json");
  REQUIRE(io::write_file(input, write_json(fixture())) == io::Status::Ok);
  const std::string out = tmp.file("results.json");
  const Run run = content({"normal-cases", input, "--out", out});
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  CHECK(count(run.result, "cases") == 23);
  CHECK(count(run.result, "matches") == 23);
  CHECK(count(run.result, "mismatches") == 0);

  std::string text;
  REQUIRE(io::read_file(out, text) == io::Status::Ok);
  JsonValue r;
  REQUIRE(parse_json(text, r).ok);
  std::string_view schema;
  r.find("schema")->get_string(schema);
  CHECK(schema == "engine.normal_cases.v1");
  const JsonValue& summary = *r.find("summary");
  const JsonValue& by = *summary.find("by_category");
  CHECK(count(*by.find("tangents"), "cases") == 9);
  CHECK(count(*by.find("interpolation_cancellation"), "cases") == 2);
  CHECK(count(*by.find("exact_dense_face_collapse"), "cases") == 3);
  CHECK(count(*by.find("facet_states"), "cases") == 5);
  CHECK(count(*by.find("nonfinite"), "cases") == 4);
  // Every case but the dense collapse has a reference-rule path; a facet state's expected fallback
  // is relative to its reference, which a reference evaluation cannot express, so it is not
  // counted.
  CHECK(count(*summary.find("rule_path"), "cases") == 19);
  CHECK(count(*summary.find("rule_path"), "mismatches") == 0);
  CHECK(summary.find("mismatched")->size() == 0);
  CHECK(flag(*r.find("engine"), "input_epsilon_is_engine_sine"));
  std::string_view input_schema;
  summary.find("input_schema")->get_string(input_schema);
  CHECK(input_schema == "engine.test.normal_cases.v1");

  // Below epsilon the reference is kept and says so; above it the state turns, which until
  // 2026-09-24 it did not: the expansion lost the 3e-6 sine and kept the reference silently.
  const JsonValue& below = find_case(r, "tangents[0]");
  CHECK_FALSE(flag(*below.find("engine"), "valid"));
  CHECK(flag(*below.find("engine"), "fell_back"));
  check_normal(below, {1, 0, 0});
  const JsonValue& above = find_case(r, "tangents[1]");
  CHECK(flag(*above.find("engine"), "valid"));
  CHECK_FALSE(flag(*above.find("engine"), "fell_back"));
  CHECK(flag(*above.find("engine")->find("rule"), "match"));
  check_normal(above, {-1, 0, 0});
  CHECK(flag(above, "match"));
  CHECK(flag(find_case(r, "tangents[2]"), "match"));
  check_normal(find_case(r, "tangents[3]"), {1, 0, 0});
  // A tangent 2^-24 of its reference's length is not zero (D10), so the vertex is valid.
  CHECK(flag(*find_case(r, "tangents[4]").find("engine"), "valid"));
  // The same pair at four scales, the last two past where an unscaled squared norm underflows or
  // overflows f32: the same decision and the same direction.
  for (const char* id : {"tangents[5]", "tangents[6]", "tangents[7]", "tangents[8]"}) {
    CAPTURE(id);
    const JsonValue& c = find_case(r, id);
    CHECK(flag(*c.find("engine"), "valid"));
    CHECK(flag(*c.find("engine")->find("rule"), "valid"));
    check_normal(c, {-1, 0, 0});
  }

  // An interpolation that cancels keeps the reference footpoint normal; one of length 1e-5 turns.
  const JsonValue& cancelled = find_case(r, "interpolation_cancellation[0]");
  CHECK(flag(*cancelled.find("engine"), "fell_back"));
  check_normal(cancelled, {1, 0, 0});
  const JsonValue& nearly = find_case(r, "interpolation_cancellation[1]");
  CHECK(flag(*nearly.find("engine"), "valid"));
  check_normal(nearly, {0, 1, 0});

  // The dense collapse: no facet normal, the area-weighted one of the neighbours, at every scale.
  for (const char* id : {"exact_dense_face_collapse[0]", "exact_dense_face_collapse[1]",
                         "exact_dense_face_collapse[2]"}) {
    CAPTURE(id);
    const JsonValue& c = find_case(r, id);
    CHECK_FALSE(flag(*c.find("engine"), "facet_valid"));
    CHECK(flag(*c.find("engine"), "area_weighted_valid"));
    check_normal(c, unit({0, -1, 3}));
  }

  // The facet at 1e-5 of its area turns to its own normal, with unit length, and says it did not
  // fall back; at 5e-7 it falls back and says so.
  const JsonValue& counter = find_case(r, "facet_states[0]");
  CHECK(flag(*counter.find("engine"), "valid"));
  CHECK_FALSE(flag(*counter.find("engine"), "fell_back"));
  CHECK(std::fabs(number(*counter.find("engine"), "state_length_ratio") - 1.0e-5) < 1.0e-7);
  check_normal(counter, {0, -1, 0});
  const JsonValue& gone = find_case(r, "facet_states[1]");
  CHECK_FALSE(flag(*gone.find("engine"), "valid"));
  CHECK(flag(*gone.find("engine"), "fell_back"));
  check_normal(gone, {0, 0, 1});
  // Record 35: live (5.5e-5 of the reference's area), unit length — it was 0.45 — and the state
  // facet's own direction to within what f32 can resolve of a corner angle whose sine is 3e-5.
  const JsonValue& record35 = find_case(r, "facet_states[2]");
  CHECK(flag(*record35.find("engine"), "valid"));
  CHECK_FALSE(flag(*record35.find("engine"), "fell_back"));
  const f64 ratio = number(*record35.find("engine"), "state_length_ratio");
  CHECK(ratio > 1.0e-5);
  CHECK(ratio < 1.0e-4);
  CHECK(flag(record35, "match"));
  CHECK(number(*record35.find("engine"), "normal_length") > 1.0 - 4.0e-6);
  CHECK(number(*record35.find("engine"), "normal_length") < 1.0 + 4.0e-6);
  for (const char* id : {"facet_states[3]", "facet_states[4]"}) {
    CAPTURE(id);
    check_normal(find_case(r, id), {0, -1, 0});
  }

  // NaN and infinite tangents, built by the runner: each keeps the reference.
  for (const char* id : {"nonfinite[0]", "nonfinite[1]", "nonfinite[2]", "nonfinite[3]"}) {
    CAPTURE(id);
    const JsonValue& c = find_case(r, id);
    CHECK(flag(*c.find("engine"), "fell_back"));
    CHECK_FALSE(flag(*c.find("engine")->find("rule"), "valid"));
    check_normal(c, {0, 0, 1});
  }

  // The same bytes on a second run.
  const std::string again = tmp.file("again.json");
  REQUIRE(content({"normal-cases", input, "--out", again}).exit_code == 0);
  std::string second;
  REQUIRE(io::read_file(again, second) == io::Status::Ok);
  CHECK(second == text);
}

TEST_CASE("normal-cases: a mismatch is reported and still exits 0; what it refuses") {
  engine::test::TempDir tmp("content_normal_cases_refusals");
  // A case whose expectation is wrong on purpose (the reference kept above the threshold) and an
  // epsilon that is not the engine's: both reported, and the engine's own threshold still used.
  JsonValue wrong = JsonValue::object();
  wrong.set("epsilon", JsonValue(1.0e-5));
  JsonValue tangents = JsonValue::array();
  tangents.push_back(
      tangent_case({0, 0, 2}, {0, 6.0e-6, 2}, {0, 1, 0}, false, {0, 1, 0}, "wrong on purpose"));
  wrong.set("tangents", std::move(tangents));
  const std::string input = tmp.file("wrong.json");
  REQUIRE(io::write_file(input, write_json(wrong)) == io::Status::Ok);
  const Run run = content({"normal-cases", input, "--out", tmp.file("wrong-results.json")});
  INFO(run.output);
  CHECK(run.exit_code == 0);
  CHECK(count(run.result, "mismatches") == 1);
  std::string text;
  REQUIRE(io::read_file(tmp.file("wrong-results.json"), text) == io::Status::Ok);
  JsonValue r;
  REQUIRE(parse_json(text, r).ok);
  CHECK_FALSE(flag(*r.find("engine"), "input_epsilon_is_engine_sine"));
  CHECK(flag(*find_case(r, "tangents[0]").find("engine"), "valid"));

  CHECK(content({"normal-cases", input}).exit_code == 2);  // no --out
  CHECK(content({"normal-cases", input, "--out", tmp.file("x.json"), "--bogus"}).exit_code == 2);
  CHECK(
      content({"normal-cases", tmp.file("missing.json"), "--out", tmp.file("y.json")}).exit_code ==
      1);
  // A case without its tangents cannot be evaluated; nor can a file that names no cases.
  JsonValue broken = JsonValue::object();
  JsonValue bad = JsonValue::array();
  JsonValue c = JsonValue::object();
  c.set("u", vec({1, 0, 0}));
  bad.push_back(std::move(c));
  broken.set("tangents", std::move(bad));
  const std::string broken_path = tmp.file("broken.json");
  REQUIRE(io::write_file(broken_path, write_json(broken)) == io::Status::Ok);
  CHECK(content({"normal-cases", broken_path, "--out", tmp.file("z.json")}).exit_code == 1);
  const std::string empty_path = tmp.file("empty.json");
  REQUIRE(io::write_file(empty_path, "{\"schema\":\"nothing\"}") == io::Status::Ok);
  CHECK(content({"normal-cases", empty_path, "--out", tmp.file("w.json")}).exit_code == 1);
}
