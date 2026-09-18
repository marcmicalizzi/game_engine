// End to end: engine-cli drives engine-host over stdio. This is the Phase 0 exit criterion in
// docs/plan/10-roadmap-risks.md: open a session, create objects, commit, diff, and roll back
// through engine-cli, with every step a separate process.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string stdout_text;
  JsonValue result;  // parsed stdout when it was JSON
};

Run cli(std::vector<std::string> args) {
  std::vector<std::string_view> argv;
  argv.push_back(ENGINE_APP_PATH);
  argv.push_back("--host");
  argv.push_back(ENGINE_HOST_PATH);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-cli: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.stdout_text);
  run.exit_code = p.wait();
  if (!run.stdout_text.empty()) (void)parse_json(run.stdout_text, run.result);
  return run;
}

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE(v != nullptr);
  return *v;
}

const char* k_a = "00000000000000100000000000000001";
const char* k_b = "00000000000000100000000000000002";
const char* k_type = "engine.content.AssetProvenance";

std::string apply_params(std::string commands) {
  return "{\"commands\":" + commands +
         ",\"attribution\":{\"actor\":\"cli-test\",\"role\":\"environment\",\"task\":\"t\","
         "\"rationale\":\"exit criterion\"}}";
}

}  // namespace

TEST_CASE("cli: engine.info and engine.methods through a spawned host") {
  Run info = cli({"engine.info"});
  CHECK(info.exit_code == 0);
  CHECK(at(info.result, "name") == JsonValue("game_engine"));
  CHECK(at(info.result, "version").as_string().size() >= 5);
  Run methods = cli({"--compact", "engine.methods"});
  CHECK(methods.exit_code == 0);
  CHECK(methods.stdout_text.find('\n') == methods.stdout_text.size() - 1);
  CHECK(at(methods.result, "methods").size() >= 20);
}

TEST_CASE("cli: errors exit non-zero and print nothing on stdout") {
  Run bad = cli({"no.such.method"});
  CHECK(bad.exit_code == 1);
  CHECK(bad.stdout_text.empty());
  Run bad_params = cli({"session.open", "{\"path\":5}"});
  CHECK(bad_params.exit_code == 1);
  Run not_json = cli({"engine.info", "{oops"});
  CHECK(not_json.exit_code == 2);
  Run no_method = cli({});
  CHECK(no_method.exit_code == 2);
}

TEST_CASE("cli: the Phase 0 exit criterion, one process per step") {
  const test::TempDir tmp("engine_cli");
  const std::string dir = tmp.file("world");

  Run missing = cli({"--doc", dir, "session.info"});
  CHECK(missing.exit_code == 1);

  Run created = cli({"--doc", dir, "--create", "--name", "World", "session.info"});
  REQUIRE(created.exit_code == 0);
  CHECK(at(created.result, "name") == JsonValue("World"));
  CHECK(at(created.result, "journal_length") == JsonValue(u32{0}));
  CHECK(std::filesystem::exists(std::filesystem::path(dir) / "manifest.json"));

  const std::string create_a = std::string("[{\"kind\":\"CreateObject\",\"id\":\"") + k_a +
                               "\",\"type\":\"" + k_type +
                               "\"},{\"kind\":\"SetProperty\",\"id\":\"" + k_a +
                               "\",\"name\":\"generator\",\"value\":\"dune-gen\"}]";
  Run applied = cli({"--doc", dir, "doc.apply", apply_params(create_a)});
  REQUIRE(applied.exit_code == 0);
  CHECK(at(applied.result, "committed") == JsonValue(true));
  CHECK(at(applied.result, "patch_index") == JsonValue(u32{0}));

  Run objects = cli({"--doc", dir, "doc.objects"});
  REQUIRE(objects.exit_code == 0);
  CHECK(at(objects.result, "total") == JsonValue(u32{1}));
  CHECK(at(at(at(objects.result, "objects")[0], "properties"), "generator") ==
        JsonValue("dune-gen"));

  const std::string create_b = std::string("[{\"kind\":\"CreateObject\",\"id\":\"") + k_b +
                               "\",\"type\":\"" + k_type + "\",\"parent\":\"" + k_a + "\"}]";
  Run applied_b = cli({"--doc", dir, "doc.apply", apply_params(create_b)});
  REQUIRE(applied_b.exit_code == 0);
  CHECK(at(applied_b.result, "patch_index") == JsonValue(u32{1}));
  CHECK(at(cli({"--doc", dir, "doc.objects"}).result, "total") == JsonValue(u32{2}));

  // Roll back across processes.
  Run undone = cli({"--doc", dir, "doc.undo"});
  REQUIRE(undone.exit_code == 0);
  CHECK(at(undone.result, "position") == JsonValue(u32{1}));
  CHECK(at(cli({"--doc", dir, "doc.objects"}).result, "total") == JsonValue(u32{1}));
  Run undone_again = cli({"--doc", dir, "doc.undo"});
  CHECK(at(undone_again.result, "position") == JsonValue(u32{0}));
  CHECK(at(cli({"--doc", dir, "doc.objects"}).result, "total") == JsonValue(u32{0}));
  Run redone = cli({"--doc", dir, "doc.redo", "{\"steps\":2}"});
  CHECK(at(redone.result, "position") == JsonValue(u32{2}));
  CHECK(at(cli({"--doc", dir, "doc.objects"}).result, "total") == JsonValue(u32{2}));

  // Layers and diff.
  Run layered = cli({"--doc", dir, "doc.add_layer", "{\"name\":\"quest\"}"});
  REQUIRE(layered.exit_code == 0);
  CHECK(at(layered.result, "layers").size() == 2);
  const std::string override_a = std::string("[{\"kind\":\"SetProperty\",\"id\":\"") + k_a +
                                 "\",\"name\":\"generator\",\"value\":\"quest-gen\"}]";
  REQUIRE(cli({"--doc", dir, "doc.apply", apply_params(override_a)}).exit_code == 0);
  Run diff = cli({"--doc", dir, "doc.diff", "{\"from_layer\":\"base\",\"to_layer\":\"quest\"}"});
  REQUIRE(diff.exit_code == 0);
  CHECK(at(diff.result, "commands").size() >= 1);
  Run got = cli({"--doc", dir, "doc.get", std::string("{\"id\":\"") + k_a + "\"}"});
  REQUIRE(got.exit_code == 0);
  CHECK(at(at(got.result, "properties"), "generator") == JsonValue("quest-gen"));
  CHECK(at(got.result, "defining_layer") == JsonValue("base"));

  Run validated = cli({"--doc", dir, "doc.validate"});
  REQUIRE(validated.exit_code == 0);
  CHECK(at(validated.result, "ok") == JsonValue(true));
  Run journal = cli({"--doc", dir, "doc.journal"});
  REQUIRE(journal.exit_code == 0);
  CHECK(at(journal.result, "total") == JsonValue(u32{3}));
  CHECK(at(at(at(journal.result, "patches")[0], "attribution"), "actor") == JsonValue("cli-test"));
  CHECK(std::filesystem::exists(std::filesystem::path(dir) / "layers" / "quest.json"));
  CHECK(std::filesystem::exists(std::filesystem::path(dir) / "journal.jsonl"));
}

TEST_CASE("cli: a partitioned layer keeps its objects in tile files") {
  const test::TempDir tmp("engine_cli");
  const std::string dir = tmp.file("tiled");
  const std::filesystem::path layer_dir = std::filesystem::path(dir) / "layers" / "places";
  REQUIRE(cli({"--doc", dir, "--create", "--name", "Tiled", "session.info"}).exit_code == 0);

  Run layered = cli({"--doc", dir, "doc.add_layer",
                     R"({"name":"places","partition":{"property":"position","tile_size":64}})"});
  REQUIRE(layered.exit_code == 0);
  const JsonValue& layers = at(layered.result, "layers");
  REQUIRE(layers.size() == 2);
  CHECK(at(layers[1], "name") == JsonValue("places"));
  CHECK(at(layers[1], "tiles") == JsonValue(u32{0}));
  CHECK(at(at(layers[1], "partition"), "tile_size") == JsonValue(64.0));
  CHECK(std::filesystem::exists(layer_dir / "index.json"));

  // Three objects: two in one tile, one two tiles over.
  auto place = [](const char* id, f64 x, f64 z) {
    return std::string("{\"kind\":\"CreateObject\",\"id\":\"") + id + "\",\"type\":\"" + k_type +
           "\",\"value\":{\"position\":[" + std::to_string(x) + ",0.0," + std::to_string(z) + "]}}";
  };
  const char* k_c = "00000000000000100000000000000003";
  Run placed = cli({"--doc", dir, "doc.apply",
                    apply_params("[" + place(k_a, 8, 8) + "," + place(k_b, 20, 20) + "," +
                                 place(k_c, 160, 8) + "]")});
  REQUIRE(placed.exit_code == 0);
  CHECK(at(placed.result, "committed") == JsonValue(true));
  CHECK(std::filesystem::exists(layer_dir / "tiles" / "0_0.json"));
  CHECK(std::filesystem::exists(layer_dir / "tiles" / "2_0.json"));
  CHECK(at(at(cli({"--doc", dir, "doc.layers"}).result, "layers")[1], "tiles") ==
        JsonValue(u32{2}));

  // A second transaction touches one object, so the store rewrites that object's tile and
  // leaves the other one alone: not rewritten with the same bytes, not written at all.
  const auto stamp_of = [&](const char* file) {
    return std::filesystem::last_write_time(layer_dir / "tiles" / file);
  };
  const auto untouched_before = stamp_of("0_0.json");
  const auto touched_before = stamp_of("2_0.json");
  REQUIRE(cli({"--doc", dir, "doc.apply",
               apply_params(std::string("[{\"kind\":\"SetProperty\",\"id\":\"") + k_c +
                            "\",\"name\":\"generator\",\"value\":\"tile-gen\"}]")})
              .exit_code == 0);
  CHECK(stamp_of("0_0.json") == untouched_before);
  CHECK(stamp_of("2_0.json") != touched_before);

  // The tiles are an on-disk detail: every object is there, composed, in the next process.
  CHECK(at(cli({"--doc", dir, "doc.objects"}).result, "total") == JsonValue(u32{3}));
  CHECK(at(at(cli({"--doc", dir, "doc.get", std::string("{\"id\":\"") + k_c + "\"}"}).result,
              "properties"),
           "generator") == JsonValue("tile-gen"));

  // And back to one file, with the tiles gone.
  Run flattened = cli({"--doc", dir, "doc.set_partition", R"({"layer":"places"})"});
  REQUIRE(flattened.exit_code == 0);
  CHECK(at(at(flattened.result, "layers")[1], "tiles") == JsonValue(u32{0}));
  CHECK(std::filesystem::exists(std::filesystem::path(dir) / "layers" / "places.json"));
  CHECK_FALSE(std::filesystem::exists(layer_dir / "index.json"));
  CHECK(at(cli({"--doc", dir, "doc.objects"}).result, "total") == JsonValue(u32{3}));
}

TEST_CASE("cli: two layers that diverged from a common base are merged in one call") {
  const test::TempDir tmp("engine_cli");
  const std::string dir = tmp.file("merged");
  REQUIRE(cli({"--doc", dir, "--create", "--name", "Merged", "session.info"}).exit_code == 0);

  // Two objects in the base layer.
  const std::string world = std::string("[{\"kind\":\"CreateObject\",\"id\":\"") + k_a +
                            "\",\"type\":\"" + k_type + "\"},{\"kind\":\"CreateObject\",\"id\":\"" +
                            k_b + "\",\"type\":\"" + k_type + "\",\"parent\":\"" + k_a + "\"}]";
  REQUIRE(cli({"--doc", dir, "doc.apply", apply_params(world)}).exit_code == 0);

  // A layer of overrides, and two copies of it that diverged: ours renames the first object and
  // drops the second, theirs renames both differently.
  auto record = [](const char* id, const char* properties) {
    return std::string("{\"kind\":\"RestoreRecord\",\"id\":\"") + id + "\",\"record\":{\"id\":\"" +
           id + "\",\"properties\":" + properties + "}}";
  };
  auto layer = [&](const char* name, const std::string& commands) {
    REQUIRE(cli({"--doc", dir, "doc.add_layer", std::string("{\"name\":\"") + name + "\"}"})
                .exit_code == 0);
    REQUIRE(cli({"--doc", dir, "doc.apply", apply_params(commands)}).exit_code == 0);
  };
  layer("shared", "[" + record(k_a, R"({"generator":"shared","license":"MIT"})") + "," +
                      record(k_b, R"({"generator":"b-shared"})") + "]");
  layer("ours", "[" + record(k_a, R"({"generator":"ours","license":"MIT"})") + "]");
  layer("theirs", "[" + record(k_a, R"({"generator":"theirs","license":"MIT"})") + "," +
                      record(k_b, R"({"generator":"b-theirs"})") + "]");

  Run merged = cli({"--doc", dir, "doc.merge",
                    R"({"base_layer":"shared","ours_layer":"ours","theirs_layer":"theirs",)"
                    R"("output_layer":"merged"})"});
  REQUIRE(merged.exit_code == 0);
  CHECK(at(merged.result, "committed") == JsonValue(true));
  CHECK(at(merged.result, "applied_ours") == JsonValue(u32{1}));
  CHECK(at(merged.result, "applied_theirs") == JsonValue(u32{1}));
  const JsonValue& conflicts = at(merged.result, "conflicts");
  REQUIRE(conflicts.size() == 2);
  CHECK(at(conflicts[0], "kind") == JsonValue("PropertyBothChanged"));
  CHECK(at(conflicts[0], "object") == JsonValue(k_a));
  CHECK(at(conflicts[0], "property") == JsonValue("generator"));
  CHECK(at(conflicts[0], "ours") == JsonValue("ours"));
  CHECK(at(conflicts[0], "theirs") == JsonValue("theirs"));
  CHECK(at(conflicts[1], "kind") == JsonValue("DeletedAndModified"));
  CHECK(at(conflicts[1], "object") == JsonValue(k_b));
  CHECK(at(conflicts[1], "property") == JsonValue(""));

  // The merged layer is the strongest: a conflicting property kept ours, the object we deleted
  // and they changed survived with their change.
  Run got_a = cli({"--doc", dir, "doc.get", std::string("{\"id\":\"") + k_a + "\"}"});
  CHECK(at(at(got_a.result, "properties"), "generator") == JsonValue("ours"));
  Run got_b = cli({"--doc", dir, "doc.get", std::string("{\"id\":\"") + k_b + "\"}"});
  CHECK(at(at(got_b.result, "properties"), "generator") == JsonValue("b-theirs"));
  CHECK(at(cli({"--doc", dir, "doc.validate"}).result, "ok") == JsonValue(true));
  CHECK(std::filesystem::exists(std::filesystem::path(dir) / "layers" / "merged.json"));

  // And it is undoable across processes, like every other transaction.
  REQUIRE(cli({"--doc", dir, "doc.undo"}).exit_code == 0);
  Run after_undo = cli({"--doc", dir, "doc.get", std::string("{\"id\":\"") + k_a + "\"}"});
  CHECK(at(at(after_undo.result, "properties"), "generator") == JsonValue("theirs"));
}
