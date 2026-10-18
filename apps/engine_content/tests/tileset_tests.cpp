// engine-content terrain-synth and terrain-tiles end to end (docs/subsystems/tileset.md, apps.md):
// a synthetic heightfield written and read back, its tile set built into a directory and into a
// derived-data cache, a second build a hit, the set the library reads, and the refusals.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <string>
#include <vector>

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

JsonValue line(const Output& out) {
  JsonValue v;
  REQUIRE_MESSAGE(parse_json(out.text, v).ok, out.text);
  return v;
}

bool flag(const JsonValue& v, const char* key) {
  const JsonValue* f = v.find(key);
  bool b = false;
  if (f != nullptr) (void)f->get_bool(b);
  return b;
}

std::string text(const JsonValue& v, const char* key) {
  const JsonValue* f = v.find(key);
  std::string_view s;
  if (f != nullptr) (void)f->get_string(s);
  return std::string(s);
}

}  // namespace

TEST_CASE("engine-content terrain-synth: each shape is the library's") {
  const test::TempDir tmp("engine_content_terrain_synth");
  for (const char* kind : {"ridge", "basin", "steps", "flat"}) {
    CAPTURE(kind);
    const std::string out = tmp.file(std::string(kind) + ".json");
    const Output o = run({"terrain-synth", out, "--kind", kind, "--size", "33", "--spacing-mm",
                          "500", "--seed", "4", "--origin-mm", "-8000,1000"});
    REQUIRE_MESSAGE(o.exit_code == 0, o.text);
    tileset::Heightfield field;
    u64 hash = 0;
    std::string error;
    REQUIRE_MESSAGE(tileset::read_heightfield(out, field, hash, error), error);
    tileset::SynthKind k{};
    REQUIRE(tileset::parse_synth_kind(kind, k));
    const tileset::Heightfield want = tileset::synth_heightfield(k, 33, 500, 4, -8000, 1000);
    CHECK(field.origin_x_mm == -8000);
    CHECK(field.origin_z_mm == 1000);
    CHECK(field.data.size() == want.data.size());
    CHECK(std::equal(field.data.begin(), field.data.end(), want.data.begin()));
  }
  CHECK(run({"terrain-synth"}).exit_code == 2);
  CHECK(run({"terrain-synth", tmp.file("x.json"), "--kind", "volcano", "--size", "9",
             "--spacing-mm", "1000"})
            .exit_code == 2);
  CHECK(run({"terrain-synth", tmp.file("x.json"), "--kind", "flat", "--size", "1", "--spacing-mm",
             "1000"})
            .exit_code == 2);
  CHECK(run({"terrain-synth", tmp.file("x.json"), "--kind", "flat", "--size", "9"}).exit_code == 2);
  // An origin off the lattice is the library's refusal.
  CHECK(run({"terrain-synth", tmp.file("x.json"), "--kind", "flat", "--size", "9", "--spacing-mm",
             "1000", "--origin-mm", "1,0"})
            .exit_code == 1);
}

TEST_CASE("engine-content terrain-tiles: a set built, a hit the second time, through the cache") {
  const test::TempDir tmp("engine_content_terrain_tiles");
  const std::string source = tmp.file("ridge.json");
  REQUIRE(run({"terrain-synth", source, "--kind", "ridge", "--size", "65", "--spacing-mm", "1000",
               "--seed", "1"})
              .exit_code == 0);
  // Into a directory.
  const std::string dir = tmp.file("set");
  const Output first = run({"terrain-tiles", source, dir, "--cells", "16", "--far-levels", "3"});
  REQUIRE_MESSAGE(first.exit_code == 0, first.text);
  const JsonValue a = line(first);
  CHECK_FALSE(flag(a, "hit"));
  CHECK(text(a, "manifest") == io::join_path(dir, "manifest.json"));
  tileset::TileSet set;
  std::string error;
  REQUIRE_MESSAGE(set.open(text(a, "manifest"), error), error);
  CHECK(set.manifest().levels.size() == 4);
  CHECK(set.manifest().chain.size() == 3);
  CHECK(set.manifest().chain[0] == 8000);
  CHECK(text(a, "hash") == set.manifest().hash);
  CHECK(text(a, "key") == set.manifest().key);
  const Output second = run({"terrain-tiles", source, dir, "--cells", "16", "--far-levels", "3"});
  REQUIRE(second.exit_code == 0);
  const JsonValue b = line(second);
  CHECK(flag(b, "hit"));
  CHECK(text(b, "hash") == text(a, "hash"));
  // Into the cache: the same key, under tilesets/<key>/, and a hit the second time.
  const std::string ddc = tmp.file("ddc");
  const Output cached =
      run({"terrain-tiles", source, "--ddc", ddc, "--cells", "16", "--far-levels", "3"});
  REQUIRE_MESSAGE(cached.exit_code == 0, cached.text);
  const JsonValue c = line(cached);
  CHECK_FALSE(flag(c, "hit"));
  CHECK(text(c, "key") == text(a, "key"));
  CHECK(text(c, "manifest") == ddc + "/tilesets/" + text(a, "key") + "/manifest.json");
  CHECK(
      flag(line(run({"terrain-tiles", source, "--ddc", ddc, "--cells", "16", "--far-levels", "3"})),
           "hit"));
  CHECK_FALSE(flag(line(run({"terrain-tiles", source, "--ddc", ddc, "--cells", "16", "--far-levels",
                             "3", "--force"})),
                   "hit"));
  // Another option, another key.
  const JsonValue lifted = line(run({"terrain-tiles", source, "--ddc", ddc, "--cells", "16",
                                     "--far-levels", "3", "--offset-mm", "64000,500,0"}));
  CHECK_FALSE(flag(lifted, "hit"));
  CHECK(text(lifted, "key") != text(a, "key"));
  // Refusals: usage, a missing source, an offset between points, a chain the points cannot carry.
  CHECK(run({"terrain-tiles"}).exit_code == 2);
  CHECK(run({"terrain-tiles", source, dir, "--cells", "0"}).exit_code == 2);
  CHECK(run({"terrain-tiles", source, dir, "--offset-mm", "1,2"}).exit_code == 2);
  CHECK(run({"terrain-tiles", tmp.file("missing.json"), dir}).exit_code == 1);
  CHECK(run({"terrain-tiles", source, tmp.file("o"), "--offset-mm", "500,0,0"}).exit_code == 1);
  CHECK(
      run({"terrain-tiles", source, tmp.file("o"), "--ring-spacing-mm", "1500", "--far-ratio", "1"})
          .exit_code == 1);
}
