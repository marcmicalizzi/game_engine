// End to end: engine-cli drives engine-host over stdio. This is the Phase 0 exit criterion in
// docs/plan/10-roadmap-risks.md: open a session, create objects, commit, diff, and roll back
// through engine-cli, with every step a separate process.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct TempDir {
  std::string path;
  TempDir() {
    const auto p = std::filesystem::temp_directory_path() / "engine_cli_tests";
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    path = p.string();
    for (char& c : path) {
      if (c == '\\') c = '/';
    }
  }
  ~TempDir() { std::filesystem::remove_all(std::filesystem::path(path)); }
};

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
  TempDir tmp;
  const std::string dir = tmp.path + "/world";

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
