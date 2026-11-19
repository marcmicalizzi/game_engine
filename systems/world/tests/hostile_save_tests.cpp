// A save someone else made (T191, from T13's and T190's reading, docs/experiments/
// fuzzing-design.md): `session.load_game` restores any save a client names. Its manifest lists and
// hashes its files, and the restore copies exactly those, each read where the manifest names it
// through no link; a file it does not list is left behind. Every file here is bytes a function
// returns, so the fuzz harness can lift them as seeds.
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <foundation/io/vfs.h>
#include <systems/world/save_game.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

using namespace engine;
using namespace engine::world;

namespace hostile_save {

std::string document_manifest() {
  return R"({"name":"x","layers":[{"name":"base","role":"Base","file":"base.json"}],)"
         R"("edit_layer":"base","undo_position":0})"
         "\n";
}
std::string base_layer() { return R"({"name":"base","role":"Base","objects":[]})"; }
std::string host_secret() { return "a file of the host's, which no save may carry\n"; }

// A directory link at `link` to `target` (as T190's): a symbolic link where the platform lets this
// process make one, else (Windows without the privilege) a junction, which any user may make.
bool make_directory_link(const std::string& target, const std::string& link) {
  std::error_code ec;
  std::filesystem::create_directory_symlink(target, link, ec);
  if (!ec) return true;
#ifdef _WIN32
  std::string command = "cmd /c mklink /J \"" + link + "\" \"" + target + "\" >nul 2>nul";
  for (char& c : command)
    if (c == '/') c = '\\';
  return std::system(command.c_str()) == 0;
#else
  return false;
#endif
}

void write(const std::string& path, const std::string& text) {
  REQUIRE(io::make_directories(std::string(io::parent_path(path))) == io::Status::Ok);
  REQUIRE(io::write_file(path, text) == io::Status::Ok);
}

// A save of a one-layer document with no store, no ring and no player, written by `write_save`.
std::string a_save(const test::TempDir& tmp, const char* name) {
  const std::string doc_dir = tmp.file(std::string(name) + "-doc");
  write(doc_dir + "/manifest.json", document_manifest());
  write(doc_dir + "/layers/base.json", base_layer());
  SaveInputs inputs;
  inputs.document_dir = doc_dir;
  SaveManifest manifest;
  std::string error;
  const std::string dir = tmp.file(name);
  REQUIRE_MESSAGE(write_save(dir, inputs, manifest, error), error);
  return dir;
}

}  // namespace hostile_save

TEST_CASE("save hostile: a restore copies the files the manifest lists, not what else is there") {
  const test::TempDir tmp("engine_world_hostile_save_unlisted");
  const std::string save = hostile_save::a_save(tmp, "save");
  // Beside the listed files: a stray file, and a directory link to a directory of the host's.
  hostile_save::write(save + "/document/stray.txt", "not in the manifest\n");
  hostile_save::write(tmp.file("outside/secret.txt"), hostile_save::host_secret());
  REQUIRE(hostile_save::make_directory_link(tmp.file("outside"), save + "/document/extra"));

  SaveManifest read;
  SaveCheck check;
  std::string error;
  REQUIRE_MESSAGE(read_save(save, read, check, error), error);
  const std::string restored = tmp.file("restored");
  REQUIRE(std::filesystem::create_directories(restored));
  REQUIRE_MESSAGE(restore_save_files(save, read, restored, error), error);
  CHECK(std::filesystem::exists(restored + "/manifest.json"));
  CHECK(std::filesystem::exists(restored + "/layers/base.json"));
  CHECK_FALSE(std::filesystem::exists(restored + "/stray.txt"));
  CHECK_FALSE(std::filesystem::exists(restored + "/extra"));
  CHECK_FALSE(std::filesystem::exists(restored + "/extra/secret.txt"));
}

TEST_CASE("save hostile: a listed file reached through a link is refused") {
  const test::TempDir tmp("engine_world_hostile_save_link");
  const std::string save = hostile_save::a_save(tmp, "save");
  // `document/layers` moved out of the save, the same bytes, and a link left where it was: every
  // size and hash still matches, so only where the file is read from tells it apart.
  std::error_code ec;
  std::filesystem::rename(save + "/document/layers", tmp.file("elsewhere"), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(hostile_save::make_directory_link(tmp.file("elsewhere"), save + "/document/layers"));

  SaveManifest read;
  SaveCheck check;
  std::string error;
  CHECK_FALSE(read_save(save, read, check, error));
  CHECK(error.find("document/layers/base.json") != std::string::npos);
  CHECK(error.find("through a link") != std::string::npos);
  CHECK(error.find("elsewhere") == std::string::npos);  // where it leads is not told

  // A link to nothing is a link, not a missing file: whether its target exists is not told either.
  std::filesystem::remove(save + "/document/layers", ec);
  REQUIRE_FALSE(ec);
  REQUIRE(hostile_save::make_directory_link(tmp.file("nowhere"), save + "/document/layers"));
  CHECK_FALSE(read_save(save, read, check, error));
  CHECK(error.find("through a link") != std::string::npos);

  // Put back, the save reads and restores.
  std::filesystem::remove(save + "/document/layers", ec);
  REQUIRE_FALSE(ec);
  std::filesystem::rename(tmp.file("elsewhere"), save + "/document/layers", ec);
  REQUIRE_FALSE(ec);
  REQUIRE_MESSAGE(read_save(save, read, check, error), error);
  const std::string restored = tmp.file("restored");
  REQUIRE(std::filesystem::create_directories(restored));
  REQUIRE_MESSAGE(restore_save_files(save, read, restored, error), error);
  CHECK(std::filesystem::exists(restored + "/layers/base.json"));
}

namespace hostile_save {

// A manifest path that passes a check of its text for `..`, a leading '/' and `\` but names a
// file outside the save once joined (the stack's mend of T191): an empty, `.` or `..`-free
// component the walk once skipped, and the absolute path that empty component lets in.
Vector<std::string> outside_paths(const test::TempDir& tmp) {
  Vector<std::string> paths;
  paths.push_back("document//x");
  paths.push_back("document/./x");
  paths.push_back("document/x/");
  paths.push_back("/x");
  // `document//<an absolute path>`: inside the test's scratch, so even the code before the mend
  // could not write outside it; the case asserts on what the rule and the destination return.
  std::string absolute = tmp.file("written-here");
  for (char& c : absolute)
    if (c == '\\') c = '/';
  if (absolute.size() > 1 && absolute[1] == ':') absolute = absolute.substr(2);  // `/dir/...`
  paths.push_back("document/" + absolute);
  return paths;
}

// The save's manifest with `path` appended to its files.
void list_in_manifest(const std::string& save, const std::string& path) {
  std::string text;
  REQUIRE(io::read_file(save + "/save.json", text) == io::Status::Ok);
  JsonValue json;
  REQUIRE(parse_json(text, json).ok);
  JsonValue* files = json.find("files");
  REQUIRE(files != nullptr);
  JsonValue entry{JsonValue::Object{}};
  entry.set("path", JsonValue(std::string_view(path)));
  entry.set("bytes", JsonValue(i64{0}));
  entry.set("hash", JsonValue(std::string_view("0000000000000000")));
  files->push_back(std::move(entry));
  REQUIRE(io::write_file(save + "/save.json", write_json(json)) == io::Status::Ok);
}

}  // namespace hostile_save

TEST_CASE("save hostile: a manifest path with an empty, '.' or absolute part is outside the save") {
  const test::TempDir tmp("engine_world_hostile_save_outside");
  const std::string to = tmp.file("restored");
  for (const std::string& path : hostile_save::outside_paths(tmp)) {
    CAPTURE(path);
    CHECK_FALSE(save_path_admissible(path));
    const std::string_view relative =
        path.rfind("document/", 0) == 0 ? std::string_view(path).substr(9) : std::string_view(path);
    CHECK(restore_destination(to, relative).empty());
  }
  // Before the mend the restore wrote to `io::join_path(to, relative)`, which for the last path
  // is its absolute half, outside `to`: what the destination function now refuses.
  const std::string absolute = hostile_save::outside_paths(tmp).back().substr(9);
  CHECK(io::is_absolute_path(io::join_path(to, absolute)));
  CHECK(io::join_path(to, absolute).rfind(io::normalize_path(to), 0) != 0);
  // What the rule lets through still restores where it should.
  CHECK(save_path_admissible("document/layers/base.json"));
  CHECK(restore_destination(to, "layers/base.json") == io::join_path(to, "layers/base.json"));

  // `read_save` refuses each by name, and so does the restore handed a manifest that lists it.
  u32 n = 0;
  for (const std::string& path : hostile_save::outside_paths(tmp)) {
    CAPTURE(path);
    ++n;
    const std::string save = hostile_save::a_save(tmp, ("save-" + std::to_string(n)).c_str());
    SaveManifest good;
    SaveCheck check;
    std::string error;
    REQUIRE_MESSAGE(read_save(save, good, check, error), error);
    hostile_save::list_in_manifest(save, path);
    SaveManifest read;
    CHECK_FALSE(read_save(save, read, check, error));
    CHECK(error == "the save names a file outside itself: '" + path + "'");
    if (path.rfind("document/", 0) != 0) continue;  // the restore copies the document's files
    SaveFile listed;
    listed.path = path;
    good.files.push_back(listed);
    const std::string restored = tmp.file("restored-" + std::to_string(n));
    REQUIRE(std::filesystem::create_directories(restored));
    CHECK_FALSE(restore_save_files(save, good, restored, error));
    CHECK(error == "the save names a file outside itself: '" + path + "'");
  }
  CHECK_FALSE(std::filesystem::exists(tmp.file("written-here")));
}

TEST_CASE("save hostile: a valid save still restores byte for byte, its store checked again") {
  const test::TempDir tmp("engine_world_hostile_save_whole");
  const std::string save = hostile_save::a_save(tmp, "save");
  SaveManifest read;
  SaveCheck check;
  std::string error;
  REQUIRE_MESSAGE(read_save(save, read, check, error), error);
  const std::string restored = tmp.file("restored");
  REQUIRE(std::filesystem::create_directories(restored));
  REQUIRE_MESSAGE(restore_save_files(save, read, restored, error), error);
  std::string a;
  std::string b;
  REQUIRE(io::read_file(save + "/document/layers/base.json", a) == io::Status::Ok);
  REQUIRE(io::read_file(restored + "/layers/base.json", b) == io::Status::Ok);
  CHECK(a == b);
  // A store the manifest names but whose bytes changed after the check is refused at the copy.
  hostile_save::write(save + "/" + k_save_store_file, "a store\n");
  SaveFile store;
  store.path = k_save_store_file;
  store.bytes = 8;
  store.hash = "0000000000000000";
  read.files.push_back(store);
  read.store = k_save_store_file;
  const std::string again = tmp.file("restored-again");
  REQUIRE(std::filesystem::create_directories(again));
  CHECK_FALSE(restore_save_files(save, read, again, error));
  CHECK(error.find("changed after the save was checked") != std::string::npos);
}
