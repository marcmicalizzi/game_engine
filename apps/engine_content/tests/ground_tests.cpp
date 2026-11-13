// engine-content ground sample end to end (G201 stage 2; docs/subsystems/apps.md, "engine-content
// ground sample"): a scene's drawn ground asked at points, through the reader's ground plan — a
// hole on the renderer's own ground, in every configuration; and where the tile-set capability is,
// a pad raised a metre on a flat heightfield, whose edge a tree's rim hangs over: the disc's drop,
// its plane and the seat a skirt needs there, the same ground uncut, and the usage refusals.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>
#include <vector>

#if ENGINE_CONTENT_TILESET
#include <domain/tileset/tileset.h>
#endif

using namespace engine;

namespace {

struct Output {
  i32 exit_code = -1;
  std::string text;
};

Output run(std::vector<std::string> args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Output out;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-content: " << error);
    return out;
  }
  p.close_stdin();
  p.read_all(out.text);
  out.exit_code = p.wait();
  return out;
}

std::vector<JsonValue> lines(const std::string& text) {
  std::vector<JsonValue> out;
  usize at = 0;
  while (at < text.size()) {
    usize end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    if (end > at) {
      JsonValue v;
      REQUIRE_MESSAGE(parse_json(text.substr(at, end - at), v).ok, text);
      out.push_back(v);
    }
    at = end + 1;
  }
  return out;
}

f64 number(const JsonValue& v, const char* key) {
  const JsonValue* f = v.find(key);
  f64 n = std::nan("");
  if (f != nullptr) (void)f->get_f64(n);
  return n;
}

bool flag(const JsonValue& v, const char* key) {
  const JsonValue* f = v.find(key);
  bool b = false;
  if (f != nullptr) (void)f->get_bool(b);
  return b;
}

bool write(const std::string& path, const std::string& text) {
  return io::write_file(path, text) == io::Status::Ok;
}

}  // namespace

TEST_CASE("engine-content ground sample: a hole on the renderer's own ground, and the usage") {
  const test::TempDir tmp("content_ground_hole");
  REQUIRE(tmp.ok());
  const std::string scene = tmp.file("scene.json");
  REQUIRE(write(scene, R"({"format":"engine.scene.v1","name":"hole",
    "terrain":{"size":33,"extent":16,"dune_height":0.5,
               "holes":[{"name":"pit","center":[2,0,0],"radius_m":1.5}]}})"));
  const Output out =
      run({"ground", "sample", scene, "--point", "2,0", "--point", "-6,3,0.5", "--skirt", "0.5"});
  REQUIRE_MESSAGE(out.exit_code == 0, out.text);
  const std::vector<JsonValue> got = lines(out.text);
  REQUIRE(got.size() == 3);
  CHECK(flag(got[0], "uncut") == false);
  CHECK(number(got[0], "rim_samples") == 16.0);
  CHECK(number(got[0], "grid_spacing_m") == doctest::Approx(1.0));
  // The hole's centre: a hole, and named among the kinds that cover it.
  CHECK(flag(got[1], "hole"));
  const JsonValue* kinds = got[1].find("excluded");
  REQUIRE(kinds != nullptr);
  CHECK(write_json(*kinds).find("holes") != std::string::npos);
  // Away from it: ground, a disc of sixteen samples, a seat no lower than the centre less the drop.
  CHECK_FALSE(flag(got[2], "hole"));
  const JsonValue* disc = got[2].find("disc");
  REQUIRE(disc != nullptr);
  CHECK(number(*disc, "samples") == 16.0);
  CHECK(number(*disc, "seat_m") <= number(*disc, "centre_m"));
  CHECK(std::isfinite(number(got[2], "y_m")));
  // The usage: no point, an unknown subcommand, a radius that is not a number.
  CHECK(run({"ground", "sample", scene}).exit_code == 2);
  CHECK(run({"ground", "probe", scene}).exit_code == 2);
  CHECK(run({"ground", "sample", scene, "--point", "1,2,x"}).exit_code == 2);
  CHECK(run({"ground", "sample", tmp.file("missing.json"), "--point", "0,0"}).exit_code == 1);
}

#if ENGINE_CONTENT_TILESET

TEST_CASE("engine-content ground sample: a rim over a raised pad's edge drops, and needs a seat") {
  const test::TempDir tmp("content_ground_pad");
  REQUIRE(tmp.ok());
  std::string error;
  const tileset::Heightfield field =
      tileset::synth_heightfield(tileset::SynthKind::flat, 129, 250, 0, -16000, -16000);
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("flat.json"), field, error), error);
  // A pad a metre high over x and z in [-4, 4], its fill faces near vertical: a bank.
  const std::string scene = tmp.file("scene.json");
  REQUIRE(write(scene, R"({"format":"engine.scene.v1","name":"pad",
    "terrain":{"size":17,"extent":16,"provider":"tiles",
      "tiles":{"heightfield":"flat.json","cells":32,"ddc":"ddc","edits":[
        {"name":"pad","mode":"Fill","outline":[[-4,1,-4],[4,1,-4],[4,1,4],[-4,1,4]],
         "fill_slope":8,"fill_m":2}]}}})"));
  const Output out = run({"ground", "sample", scene, "--point", "3.9,0,0.5", "--point", "0,0,0.5",
                          "--skirt", "0.5", "--ddc", tmp.file("ddc")});
  REQUIRE_MESSAGE(out.exit_code == 0, out.text);
  const std::vector<JsonValue> got = lines(out.text);
  REQUIRE(got.size() == 3);
  CHECK(number(got[0], "edits") == 1.0);
  // At the edge: the centre on the pad, the rim's far side on the ground a metre down.
  CHECK(number(got[1], "y_m") == doctest::Approx(1.0).epsilon(0.01));
  const JsonValue* edge = got[1].find("disc");
  REQUIRE(edge != nullptr);
  CHECK(number(*edge, "drop_m") > 0.8);
  CHECK(number(*edge, "min_m") < 0.2);
  // The seat: the lowest ground plus the skirt less the margin, under the pad's top.
  CHECK(number(*edge, "seat_m") ==
        doctest::Approx(number(*edge, "min_m") + 0.5 - 0.05).epsilon(1e-9));
  CHECK(number(*edge, "lowered_m") > 0.3);
  // In the middle: level, nothing to lower.
  const JsonValue* middle = got[2].find("disc");
  REQUIRE(middle != nullptr);
  CHECK(number(*middle, "drop_m") == doctest::Approx(0.0).epsilon(0.002));
  CHECK(number(*middle, "lowered_m") == 0.0);
  CHECK(std::fabs(number(*middle, "gx")) < 0.01);
  // The same ground before its edits: the flat heightfield, no drop anywhere.
  const Output uncut =
      run({"ground", "sample", scene, "--point", "3.9,0,0.5", "--uncut", "--ddc", tmp.file("ddc")});
  REQUIRE_MESSAGE(uncut.exit_code == 0, uncut.text);
  const std::vector<JsonValue> flat = lines(uncut.text);
  REQUIRE(flat.size() == 2);
  CHECK(flag(flat[0], "uncut"));
  CHECK(number(flat[1], "y_m") == doctest::Approx(0.0).epsilon(0.002));
}

#endif  // ENGINE_CONTENT_TILESET
