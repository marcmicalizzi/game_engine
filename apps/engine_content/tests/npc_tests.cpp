// engine-content npcs end to end (docs/subsystems/npc.md, "The generator"; apps.md): a params file
// written into the test's scratch directory, the document written beside it and read back, the same
// layer at one worker and at four, and the refusals.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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

}  // namespace

#if ENGINE_CONTENT_NPC

namespace {

u64 number(const JsonValue& object, const char* key) {
  u64 v = 0;
  const JsonValue* value = object.find(key);
  return value != nullptr && value->get_u64(v) ? v : ~u64{0};
}

std::string file_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every file under a document directory, relative path and bytes, in path order.
std::vector<std::pair<std::string, std::string>> tree(const std::string& root) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    out.emplace_back(std::filesystem::relative(e.path(), root).generic_string(),
                     file_text(e.path().string()));
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST_CASE("engine-content npcs: a seeded layer of residents, the same at any worker count") {
  const test::TempDir tmp("engine_content_npcs");
  REQUIRE(tmp.ok());
  const std::string params = tmp.file("params.json");
  {
    std::ofstream out(params, std::ios::binary);
    out << R"({"seed":7,"residents":2000,"extent":[-128,-128,128,128],"time_us":25200000000})";
  }
  const Output one = run({"npcs", params, "--out", tmp.file("one"), "--jobs", "1"});
  REQUIRE_MESSAGE(one.exit_code == 0, one.text);
  JsonValue line;
  REQUIRE(parse_json(one.text, line).ok);
  CHECK(number(line, "residents") == 2000);
  CHECK(number(line, "places") == 500 + 50 + 40 + 20);
  CHECK(number(line, "tiles") > 16);
  CHECK(std::filesystem::exists(tmp.file("one") + "/layers/residents/index.json"));

  const Output four = run({"npcs", params, "--out", tmp.file("four"), "--jobs", "4"});
  REQUIRE_MESSAGE(four.exit_code == 0, four.text);
  // The layer's files, byte for byte; the manifest names the document, which is the same too.
  const auto a = tree(tmp.file("one") + "/layers/residents");
  const auto b = tree(tmp.file("four") + "/layers/residents");
  REQUIRE(a.size() == b.size());
  CHECK(a == b);

  // A second layer of the name is refused (the sentence goes to stderr, and the library's test
  // checks it); so are a bad params file and no --out.
  const Output again = run({"npcs", params, "--out", tmp.file("one")});
  CHECK(again.exit_code == 1);
  {
    std::ofstream out(tmp.file("bad.json"), std::ios::binary);
    out << R"({"residents":10,"colour":"blue"})";
  }
  const Output bad = run({"npcs", tmp.file("bad.json"), "--out", tmp.file("bad")});
  CHECK(bad.exit_code == 1);
  CHECK(run({"npcs", params}).exit_code == 2);
}

#else

TEST_CASE("engine-content npcs: without the npc capability the command says so") {
  const Output out = run({"npcs", "params.json", "--out", "x"});
  CHECK(out.exit_code == 1);
}

#endif
