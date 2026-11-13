// The one rule for a generator's derived-data root (derived_root.h; scene_gen.md, "The
// derived-data root"; T119): the host's root wins; with none, the scene's own, then the
// repository's found from the executable; never the working directory; a generator handed no root
// refuses by name; and a file left under another root is named in the sentence, never read.
#include <core/platform/process.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/derived_root.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::scene_gen;

namespace {

// The process's working directory moved into `dir` for one scope and put back: what a host started
// from a scratch directory sees.
struct WorkingDirectory {
  std::filesystem::path before;
  explicit WorkingDirectory(const std::string& dir) : before(std::filesystem::current_path()) {
    std::filesystem::current_path(dir);
  }
  ~WorkingDirectory() {
    std::error_code ec;
    std::filesystem::current_path(before, ec);
  }
};

bool has(const std::string& path) { return std::filesystem::exists(path); }

}  // namespace

TEST_CASE("a host's derived-data root: the named one, the scene's own, the executable's") {
  engine::test::TempDir tmp("scene_gen_roots");
  REQUIRE(tmp.ok());
  const std::string world = tmp.file("world");    // a scene outside any repository
  const std::string checkout = tmp.file("game");  // a repository: it holds AGENTS.md
  const std::string scenes = checkout + "/scenes/a";
  const std::string working = tmp.file("working");  // where the host was started
  std::filesystem::create_directories(world);
  std::filesystem::create_directories(scenes);
  std::filesystem::create_directories(working);
  REQUIRE(io::write_file_atomic(checkout + "/AGENTS.md", "x") == io::Status::Ok);
  REQUIRE(io::write_file_atomic(working + "/AGENTS.md", "x") == io::Status::Ok);
  const WorkingDirectory in(working);

  // The host's root wins, as given, whatever the scene and the working directory say.
  CHECK(host_derived_root("elsewhere/ddc", scenes, true) == "elsewhere/ddc");
  CHECK(host_derived_root("elsewhere/ddc", {}, false) == "elsewhere/ddc");
  // None named: the repository's above the scene, else `ddc/` beside it — never the working
  // directory's repository, which the walk from "." found before T119.
  CHECK(host_derived_root({}, scenes, true) == io::normalize_path(checkout + "/ddc"));
  CHECK(host_derived_root({}, world, true) == io::normalize_path(world + "/ddc"));
  CHECK(scene_derived_root(world) == io::normalize_path(world + "/ddc"));
  // No scene file (a description in memory): the executable's repository, which is where the test
  // executable was built, not the working directory.
  const std::string executable = geometry::find_ddc_root(platform::executable_directory());
  CHECK(host_derived_root({}, {}, false) == executable);
  CHECK(host_derived_root({}, {}, false) != io::normalize_path(working + "/ddc"));
  // A scene named by a path relative to the working directory is the scene's own still.
  CHECK(host_derived_root({}, "", true) == io::normalize_path(working + "/ddc"));
}

// **A document's host** (T104, T119; doc_scene.md, "The ground's root"): never without a root
// while the document has a directory — the named one; else the repository's above the document;
// else the document's own `ddc/`; only a document with no directory looks from the executable.
// Before 2026-11-12's night a document's host with no `--ddc` always looked from the executable,
// and an SDK's host outside any checkout found nothing there.
TEST_CASE("a document's derived-data root: named, its repository's, its own, the executable's") {
  engine::test::TempDir tmp("scene_gen_document_roots");
  REQUIRE(tmp.ok());
  const std::string alone = tmp.file("alone/doc");  // a document outside any repository
  const std::string game = tmp.file("game");        // a game repository: it holds AGENTS.md
  const std::string inside = game + "/worlds/doc";  // a document inside it
  const std::string working = tmp.file("working");  // where the host was started
  std::filesystem::create_directories(alone);
  std::filesystem::create_directories(inside);
  std::filesystem::create_directories(working);
  REQUIRE(io::write_file_atomic(game + "/AGENTS.md", "x") == io::Status::Ok);
  REQUIRE(io::write_file_atomic(working + "/AGENTS.md", "x") == io::Status::Ok);
  const WorkingDirectory in(working);

  CHECK(document_derived_root("host/ddc", inside) == "host/ddc");
  CHECK(document_derived_root({}, inside) == io::normalize_path(game + "/ddc"));
  CHECK(document_derived_root({}, alone) == io::normalize_path(alone + "/ddc"));
  const std::string executable = geometry::find_ddc_root(platform::executable_directory());
  CHECK(document_derived_root({}, {}) == executable);
  // Never the working directory's repository, whichever branch answers.
  CHECK(document_derived_root({}, alone) != io::normalize_path(working + "/ddc"));
}

TEST_CASE("a generator handed no root refuses by name and writes nothing") {
  engine::test::TempDir tmp("scene_gen_no_root");
  REQUIRE(tmp.ok());
  const WorkingDirectory in(tmp.path());
  Context context;
  context.where = "scene.json: placements 3";
  std::string dir, error;
  CHECK_FALSE(generator_derived_dir(context, "routes", "routes", dir, error));
  CHECK(dir.empty());
  CHECK(error.find("scene.json: placements 3: routes: no derived-data root") == 0);
  CHECK_FALSE(has(tmp.file("ddc")));
  CHECK_FALSE(derived_dir_under({}, "deposits", "deposits", dir, error));
  CHECK(error.find("deposits: no derived-data root") == 0);

  // Handed one, it makes `<root>/<sub>` there and only there.
  const std::string root = tmp.file("host-root");
  context.ddc = root;
  REQUIRE_MESSAGE(generator_derived_dir(context, "routes", "routes", dir, error), error);
  CHECK(dir == io::normalize_path(root + "/routes"));
  CHECK(has(root + "/routes"));
  CHECK_FALSE(has(tmp.file("ddc")));
}

TEST_CASE("a file under another root is named in the refusal, never read") {
  engine::test::TempDir tmp("scene_gen_elsewhere");
  REQUIRE(tmp.ok());
  const std::string scene = tmp.file("world");
  const std::string working = tmp.file("working");
  const std::string host = tmp.file("host/ddc");
  std::filesystem::create_directories(scene);
  std::filesystem::create_directories(working + "/ddc/routes");
  std::filesystem::create_directories(host + "/routes");
  REQUIRE(io::write_file_atomic(working + "/ddc/routes/plan-1.route-plan", "x") == io::Status::Ok);
  const WorkingDirectory in(working);
  Context context;
  context.dir = scene;
  context.ddc = host;
  const std::string here = host + "/routes";
  // Only under the working directory's: the sentence says so.
  const std::string where = derived_elsewhere(context, "routes", "plan-1.route-plan", here);
  CHECK(where.find(io::normalize_path(working + "/ddc/routes")) != std::string::npos);
  CHECK(where.find("under the working directory") != std::string::npos);
  // Under the scene's own as well: the scene's own is named first.
  std::filesystem::create_directories(scene + "/ddc/routes");
  REQUIRE(io::write_file_atomic(scene + "/ddc/routes/plan-1.route-plan", "x") == io::Status::Ok);
  const std::string own = derived_elsewhere(context, "routes", "plan-1.route-plan", here);
  CHECK(own.find(io::normalize_path(scene + "/ddc/routes")) != std::string::npos);
  CHECK(own.find("under the scene's own root") != std::string::npos);
  // Nowhere else: nothing to say.
  CHECK(derived_elsewhere(context, "routes", "plan-2.route-plan", here).empty());
}
