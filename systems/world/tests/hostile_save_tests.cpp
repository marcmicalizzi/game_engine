// A save someone else made (T191, from T13's and T190's reading, docs/experiments/
// fuzzing-design.md): `session.load_game` restores any save a client names. Its manifest lists and
// hashes its files, and the restore copies exactly those, each read where the manifest names it
// through no link; a file it does not list is left behind. Every file here is bytes a function
// returns, so the fuzz harness can lift them as seeds.
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
