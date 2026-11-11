// `engine-content matgen` end to end (docs/subsystems/matgen.md, "The command"): the sets built
// into a pack directory, read back the way every host reads its default pack, skipped when their
// key is already there, and refused for a role with no default.
#include <core/platform/process.h>
#if ENGINE_CONTENT_MATGEN
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>
#endif

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <string>
#include <string_view>
#include <vector>

using namespace engine;

namespace {

struct Output {
  i32 code = -1;
  std::string text;
};

Output run_matgen(const std::vector<std::string>& args) {
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv{exe};
  for (const auto& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Output result;
  if (!p.spawn(argv, &error, /*merge_stderr=*/true)) {
    FAIL(error);
    return result;
  }
  p.close_stdin();
  p.read_all(result.text);
  result.code = p.wait();
  return result;
}

}  // namespace

TEST_CASE("engine-content matgen builds a pack every host reads, and builds a set once") {
#if ENGINE_CONTENT_MATGEN
  test::TempDir tmp("matgen_command");
  REQUIRE(tmp.ok());
  // A pack directory with no fetched pack: the generated one, in its `generated` folder, is it.
  const std::string pack = tmp.file("pack");
  const std::string out = io::join_path(pack, materials::k_generated_pack_dir);
  const std::vector<std::string> two = {"matgen", "--out",      out,      "--texels",    "64",
                                        "--role", "gravel/bed", "--role", "rock/granite"};
  const Output first = run_matgen(two);
  REQUIRE_MESSAGE(first.code == 0, first.text);
  CHECK(first.text.find("\"built\":true") != std::string::npos);
  CHECK(first.text.find("\"built\":false") == std::string::npos);
  for (const char* file : {"base_color.png", "normal.png", "roughness.png", "height.png",
                           "metallic.png", "set.json"}) {
    CAPTURE(file);
    CHECK(io::exists(io::join_path(io::join_path(out, "gravel/bed"), file)));
    CHECK(io::exists(io::join_path(io::join_path(out, "rock/granite"), file)));
  }

  // Built again, nothing is: each set's sidecar records the key its images were built for.
  const Output second = run_matgen(two);
  REQUIRE_MESSAGE(second.code == 0, second.text);
  CHECK(second.text.find("\"built\":true") == std::string::npos);
  CHECK(second.text.find("\"built\":false") != std::string::npos);
  // A recipe that moved is rebuilt (other texels are another key), and --force rebuilds any.
  const Output other =
      run_matgen({"matgen", "--out", out, "--texels", "128", "--role", "gravel/bed"});
  REQUIRE_MESSAGE(other.code == 0, other.text);
  CHECK(other.text.find("\"built\":true") != std::string::npos);
  // A third role joins the two already in the manifest.
  const Output third = run_matgen({"matgen", "--out", out, "--texels", "64", "--role", "sand/bar"});
  REQUIRE_MESSAGE(third.code == 0, third.text);
  CHECK(third.text.find("\"roles\":3") != std::string::npos);

  // Read as a host reads its default pack: by the pack's directory, the generated sets behind.
  const materials::Pack* read = materials::pack_at(pack);
  REQUIRE(read != nullptr);
  CHECK(read->roles.size() == 3u);
  const materials::MaterialSet* bed = materials::find_role(read, "gravel/bed");
  REQUIRE(bed != nullptr);
  CHECK(bed->tile_m == doctest::Approx(1.0f));
  CHECK_FALSE(bed->stochastic);
  CHECK(io::exists(bed->base_color));
  CHECK(io::exists(bed->normal));
  CHECK(io::exists(bed->roughness));
  const materials::MaterialSet* granite = materials::find_role(read, "rock/granite");
  REQUIRE(granite != nullptr);
  CHECK(granite->stochastic);  // a rock set is laid over faces many times its repeat (R131)
  CHECK(materials::pack_key(read) != 0u);

  // Refusals: a role with no default, a recipe file that is not one, a bad flag.
  const Output unknown = run_matgen({"matgen", "--out", out, "--role", "rock/unobtainium"});
  CHECK(unknown.code == 1);
  CHECK(unknown.text.find("rock/unobtainium") != std::string::npos);
  CHECK(run_matgen({"matgen", "--out", out, "--set", tmp.file("absent.json")}).code == 1);
  CHECK(run_matgen({"matgen", "--texels"}).code == 2);
#else
  const Output result = run_matgen({"matgen"});
  CHECK(result.code != 0);
  CHECK(result.text.find("matgen") != std::string::npos);
#endif
}
