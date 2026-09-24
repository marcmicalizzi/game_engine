// engine-content limit-dump end to end (docs/subsystems/apps.md, "engine-content limit-dump"): a
// control mesh in, the operators, the evaluations and the footpoints' bound positions out, the
// reference giving the base back bit for bit, a rigid translation carried exactly, and the same
// bytes on a second run.
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

JsonValue point(f64 x, f64 y, f64 z) {
  JsonValue p = JsonValue::array();
  p.push_back(JsonValue(x));
  p.push_back(JsonValue(y));
  p.push_back(JsonValue(z));
  return p;
}

// A 5 x 5 sheet with a gentle dome, the grid split along one diagonal, facing +z; a state that is
// the same sheet 10 mm higher; three footpoints with offsets and no base positions.
JsonValue control_mesh() {
  JsonValue c = JsonValue::object();
  c.set("schema", JsonValue("engine.test.control.v1"));
  JsonValue positions = JsonValue::array();
  JsonValue lifted = JsonValue::array();
  for (u32 j = 0; j < 5; ++j)
    for (u32 i = 0; i < 5; ++i) {
      const f64 x = 0.01 * static_cast<f64>(i) - 0.02;
      const f64 y = 0.01 * static_cast<f64>(j) - 0.02;
      const f64 z = 0.02 - 10.0 * (x * x + y * y);
      positions.push_back(point(x, y, static_cast<f64>(static_cast<f32>(z))));
      lifted.push_back(point(x, y, static_cast<f64>(static_cast<f32>(z) + 0.01f)));
    }
  JsonValue faces = JsonValue::array();
  for (u32 j = 0; j < 4; ++j)
    for (u32 i = 0; i < 4; ++i) {
      const u32 a = j * 5 + i;
      const u32 tris[2][3] = {{a, a + 1, a + 6}, {a, a + 6, a + 5}};
      for (const auto& t : tris) {
        JsonValue f = JsonValue::array();
        for (const u32 v : t)
          f.push_back(JsonValue(v));
        faces.push_back(std::move(f));
      }
    }
  c.set("positions", std::move(positions));
  c.set("faces", std::move(faces));
  JsonValue states = JsonValue::object();
  states.set("lifted", std::move(lifted));
  c.set("states", std::move(states));
  JsonValue feet = JsonValue::array();
  const f64 bary[3][3] = {{0.2, 0.3, 0.5}, {0.6, 0.2, 0.2}, {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}};
  const u32 triangles[3] = {40, 70, 100};
  for (u32 k = 0; k < 3; ++k) {
    JsonValue f = JsonValue::object();
    f.set("triangle", JsonValue(triangles[k]));
    f.set("barycentric", point(bary[k][0], bary[k][1], bary[k][2]));
    f.set("normal_offset", JsonValue(0.002 + 0.001 * static_cast<f64>(k)));
    f.set("weight", JsonValue(1.0));
    feet.push_back(std::move(f));
  }
  c.set("footpoints", std::move(feet));
  return c;
}

f64 at(const JsonValue& list, usize i, usize k) {
  f64 v = 0.0;
  list[i][k].get_f64(v);
  return v;
}

}  // namespace

TEST_CASE("limit-dump: operators, evaluations and bound positions for a control mesh") {
  engine::test::TempDir tmp("content_limit_dump");
  const std::string input = tmp.file("control.json");
  REQUIRE(io::write_file(input, write_json(control_mesh())) == io::Status::Ok);
  const std::string dump = tmp.file("dump.json");
  const Run run = content({"limit-dump", input, "--level", "2", "--out", dump});
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  std::string text;
  REQUIRE(io::read_file(dump, text) == io::Status::Ok);
  JsonValue d;
  REQUIRE(parse_json(text, d).ok);
  const JsonValue& L = *d.find("operators")->find("L");
  u64 columns = 0;
  L.find("column_count")->get_u64(columns);
  CHECK(columns == 25);
  const usize rows = L.find("row_offsets")->size() - 1;
  CHECK(rows == d.find("states")->find("reference")->find("limit_positions")->size());
  CHECK(d.find("faces")->size() == 32 * 16);  // level 2: sixteen triangles for each of 32
  std::string_view mode;
  d.find("mode")->get_string(mode);
  CHECK(mode == "limit-interpolated");

  const JsonValue& reference = *d.find("states")->find("reference");
  // Unit vertex normals, facing +z on a dome wound counter-clockwise from above.
  const JsonValue& normals = *reference.find("vertex_normals");
  for (usize i = 0; i < normals.size(); ++i) {
    const f64 n =
        std::sqrt(at(normals, i, 0) * at(normals, i, 0) + at(normals, i, 1) * at(normals, i, 1) +
                  at(normals, i, 2) * at(normals, i, 2));
    CHECK(std::fabs(n - 1.0) < 1e-6);
    CHECK(at(normals, i, 2) > 0.5);
  }
  CHECK(reference.find("invalid_vertices")->size() == 0);
  CHECK(reference.find("fallback_vertices")->size() == 0);
  // At the reference the transfer is the base, with and without the record's packing.
  const JsonValue& bound = *reference.find("bound_positions");
  const JsonValue& exact = *reference.find("bound_positions_unquantized");
  REQUIRE(bound.size() == 3);
  for (usize i = 0; i < 3; ++i)
    for (usize k = 0; k < 3; ++k)
      CHECK(at(bound, i, k) == at(exact, i, k));
  // A rigid translation 10 mm up carries every bound position 10 mm up.
  const JsonValue& lifted = *d.find("states")->find("lifted");
  const JsonValue& moved = *lifted.find("bound_positions");
  for (usize i = 0; i < 3; ++i) {
    CHECK(std::fabs(at(moved, i, 0) - at(bound, i, 0)) < 1e-7);
    CHECK(std::fabs(at(moved, i, 1) - at(bound, i, 1)) < 1e-7);
    CHECK(std::fabs(at(moved, i, 2) - at(bound, i, 2) - 0.01) < 1e-7);
  }

  // The same bytes on a second run.
  const std::string again = tmp.file("again.json");
  REQUIRE(content({"limit-dump", input, "--level", "2", "--out", again}).exit_code == 0);
  std::string second;
  REQUIRE(io::read_file(again, second) == io::Status::Ok);
  CHECK(second == text);

  // Another mode is a flag, never a substitution.
  const Run area = content({"limit-dump", input, "--level", "2", "--out", tmp.file("area.json"),
                            "--mode", "interpolated-vertex-area-weighted"});
  CHECK(area.exit_code == 0);
}

TEST_CASE("limit-dump: what it refuses") {
  engine::test::TempDir tmp("content_limit_dump_refusals");
  const std::string input = tmp.file("control.json");
  REQUIRE(io::write_file(input, write_json(control_mesh())) == io::Status::Ok);
  CHECK(content({"limit-dump", input, "--level", "2"}).exit_code == 2);  // no --out
  CHECK(content({"limit-dump", input, "--level", "9", "--out", tmp.file("x.json")}).exit_code == 2);
  CHECK(content({"limit-dump", input, "--level", "2", "--out", tmp.file("x.json"), "--mode",
                 "normalized_limit_vertex_interpolation"})
            .exit_code == 2);
  // A control mesh whose edge three triangles share is not a surface.
  JsonValue broken = control_mesh();
  JsonValue extra = JsonValue::array();
  extra.push_back(JsonValue(u32{0}));
  extra.push_back(JsonValue(u32{1}));
  extra.push_back(JsonValue(u32{12}));
  (*broken.find("faces")).push_back(std::move(extra));
  const std::string bad = tmp.file("bad.json");
  REQUIRE(io::write_file(bad, write_json(broken)) == io::Status::Ok);
  const Run run = content({"limit-dump", bad, "--level", "2", "--out", tmp.file("y.json")});
  CHECK(run.exit_code == 1);
}
