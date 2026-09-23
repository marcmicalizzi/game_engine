// End to end: engine-image compares two PNGs the test writes itself and reports numbers a
// script can read. The fixtures are generated here, so nothing binary lives in the tree and the
// expected values are the ones the metric definitions give by hand -- a four-level shift of a
// flat field has an analytic PSNR, and the same damage in the middle of the frame and in a
// corner separate only once the center weighting is on.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/image/decode.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
  JsonValue result;
};

// The built executable, or the bundle's copy of it (tests/support/test_paths.h).
const std::string& image_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}

Run image_app(std::vector<std::string> args, bool merge_stderr = false) {
  std::vector<std::string_view> argv;
  argv.push_back(image_exe());
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error, merge_stderr)) {
    FAIL("cannot spawn engine-image: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  if (!run.output.empty()) (void)parse_json(run.output, run.result);
  return run;
}

f64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  f64 out = 0.0;
  return value != nullptr && value->get_f64(out) ? out : -1.0;
}

bool is_null(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->is_null();
}

std::string forward_slashes(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

constexpr u32 k_width = 96;
constexpr u32 k_height = 72;

std::vector<u8> flat_pixels(u8 value) {
  std::vector<u8> pixels(static_cast<usize>(k_width) * k_height * 4, u8{255});
  for (usize i = 0; i < static_cast<usize>(k_width) * k_height; ++i) {
    pixels[i * 4 + 0] = value;
    pixels[i * 4 + 1] = value;
    pixels[i * 4 + 2] = value;
  }
  return pixels;
}

// A four-pixel checker, which is the same pattern wherever a block of it is taken from as long
// as the block starts on a multiple of eight.
std::vector<u8> checker_pixels() {
  std::vector<u8> pixels(static_cast<usize>(k_width) * k_height * 4, u8{255});
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const usize i = (static_cast<usize>(y) * k_width + x) * 4;
      const u8 value = static_cast<u8>(((x / 4 + y / 4) % 2 != 0) ? 220 : 40);
      pixels[i + 0] = value;
      pixels[i + 1] = value;
      pixels[i + 2] = value;
    }
  }
  return pixels;
}

void flatten_block(std::vector<u8>& pixels, u32 x0, u32 y0, u32 size) {
  for (u32 c = 0; c < 3; ++c) {
    u32 sum = 0;
    for (u32 y = y0; y < y0 + size; ++y) {
      for (u32 x = x0; x < x0 + size; ++x)
        sum += pixels[(static_cast<usize>(y) * k_width + x) * 4 + c];
    }
    const u8 mean = static_cast<u8>(sum / (size * size));
    for (u32 y = y0; y < y0 + size; ++y) {
      for (u32 x = x0; x < x0 + size; ++x)
        pixels[(static_cast<usize>(y) * k_width + x) * 4 + c] = mean;
    }
  }
}

std::string write_fixture(const std::filesystem::path& dir, const char* name,
                          const std::vector<u8>& pixels) {
  const std::string path = forward_slashes((dir / name).string());
  REQUIRE(image::write_png(path, k_width, k_height, 4,
                           std::span<const u8>(pixels.data(), pixels.size())) == io::Status::Ok);
  return path;
}

}  // namespace

TEST_CASE("engine-image: usage errors exit 2 and --help exits 0") {
  CHECK(image_app({}).exit_code == 2);
  CHECK(image_app({"nonsense"}).exit_code == 2);
  CHECK(image_app({"compare"}).exit_code == 2);
  CHECK(image_app({"compare", "only-one.png"}).exit_code == 2);
  CHECK(image_app({"compare", "a.png", "b.png", "--ppd"}).exit_code == 2);
  CHECK(image_app({"compare", "a.png", "b.png", "--ppd", "abc"}).exit_code == 2);
  CHECK(image_app({"compare", "a.png", "b.png", "--ppd", "-3"}).exit_code == 2);
  CHECK(image_app({"compare", "a.png", "b.png", "--weights", "sideways"}).exit_code == 2);
  CHECK(image_app({"compare", "a.png", "b.png", "--bogus"}).exit_code == 2);
  CHECK(image_app({"compare", "a.png", "b.png", "c.png"}).exit_code == 2);
  CHECK(image_app({"info"}).exit_code == 2);
  CHECK(image_app({"info", "a.png", "b.png"}).exit_code == 2);
  // A file that is not there and a file that is not an image are both "could not measure".
  CHECK(image_app({"compare", "no-such-file.png", "no-such-file.png"}).exit_code == 2);
  CHECK(image_app({"info", "no-such-file.png"}).exit_code == 2);

  const Run help = image_app({"--help"});
  CHECK(help.exit_code == 0);
  CHECK(help.output.find("usage: engine-image") != std::string::npos);
}

// The build stamp every app carries (cmake/EngineBuildStamp.cmake), which tools/e10-harness.ps1
// compares with the checkout before it measures anything. A build without git to ask (the Linux
// container syncs the tree without .git) says "unknown" rather than inventing a commit.
TEST_CASE("engine-image: --version names the commit it was built from") {
  const Run version = image_app({"--version"});
  CHECK(version.exit_code == 0);
  REQUIRE(version.result.is_object());
  const JsonValue* tool = version.result.find("tool");
  const JsonValue* commit = version.result.find("commit");
  const JsonValue* dirty = version.result.find("dirty");
  REQUIRE(tool != nullptr);
  REQUIRE(commit != nullptr);
  REQUIRE(dirty != nullptr);
  CHECK(tool->as_string() == "engine-image");
  CHECK(dirty->is_bool());
  const std::string_view sha = commit->as_string();
  const bool hex40 =
      sha.size() == 40 && sha.find_first_not_of("0123456789abcdef") == std::string_view::npos;
  CHECK_MESSAGE((hex40 || sha == "unknown"), "commit: " << std::string(sha));
}

TEST_CASE("engine-image: compare reports the numbers the metrics define") {
  const test::TempDir tmp("engine_image_app");
  const std::filesystem::path dir = tmp.native();

  const std::string flat = write_fixture(dir, "flat.png", flat_pixels(120));
  const std::string shifted = write_fixture(dir, "flat_shifted.png", flat_pixels(124));
  const std::string junk = forward_slashes((dir / "junk.png").string());
  REQUIRE(io::write_file(junk, "this is not a PNG") == io::Status::Ok);

  SUBCASE("an image against itself is perfect") {
    const Run run = image_app({"compare", flat, flat, "--json"});
    REQUIRE(run.exit_code == 0);
    REQUIRE(run.result.is_object());
    CHECK(number(run.result, "width") == doctest::Approx(k_width));
    CHECK(number(run.result, "height") == doctest::Approx(k_height));
    // No finite PSNR, which JSON says as null rather than as a very large number.
    CHECK(is_null(run.result, "psnr"));
    CHECK(number(run.result, "ssim") == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(number(run.result, "flip_mean") == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(number(run.result, "flip_p95") == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(number(run.result, "flip_max") == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(number(run.result, "ppd") == doctest::Approx(67.02).epsilon(0.001));
    CHECK(run.result.find("weights") != nullptr);
    CHECK(is_null(run.result, "flip_image"));
  }

  SUBCASE("four levels over a flat field is the analytic PSNR") {
    const Run run = image_app({"compare", flat, shifted, "--json"});
    REQUIRE(run.exit_code == 0);
    const f64 expected = 10.0 * std::log10(255.0 * 255.0 / 16.0);
    CHECK(number(run.result, "psnr") == doctest::Approx(expected).epsilon(1e-4));
    // Four levels around mid gray is 1.55 of L*, which FLIP puts a little under a tenth.
    CHECK(number(run.result, "flip_mean") > 0.02);
    CHECK(number(run.result, "flip_mean") < 0.2);
    CHECK(number(run.result, "flip_max") ==
          doctest::Approx(number(run.result, "flip_mean")).epsilon(1e-4));
    CHECK(number(run.result, "ms") >= 0.0);
  }

  SUBCASE("the readable report names every measure") {
    const Run run = image_app({"compare", flat, shifted});
    REQUIRE(run.exit_code == 0);
    CHECK(run.output.find("psnr") != std::string::npos);
    CHECK(run.output.find("ssim") != std::string::npos);
    CHECK(run.output.find("flip") != std::string::npos);
    CHECK(run.output.find("p95") != std::string::npos);
  }

  SUBCASE("--ppd changes what the filters keep") {
    const Run coarse = image_app({"compare", flat, shifted, "--ppd", "20", "--json"});
    const Run fine = image_app({"compare", flat, shifted, "--ppd", "120", "--json"});
    REQUIRE(coarse.exit_code == 0);
    REQUIRE(fine.exit_code == 0);
    CHECK(number(coarse.result, "ppd") == doctest::Approx(20.0).epsilon(1e-4));
    CHECK(number(fine.result, "ppd") == doctest::Approx(120.0).epsilon(1e-4));
    // A flat difference survives any amount of low-pass filtering, so the two agree here.
    CHECK(number(coarse.result, "flip_mean") ==
          doctest::Approx(number(fine.result, "flip_mean")).epsilon(1e-3));
  }

  SUBCASE("--flip writes a heat map the size of the inputs") {
    const std::string heat = forward_slashes((dir / "flip.png").string());
    std::filesystem::remove(heat);
    const Run run = image_app({"compare", flat, shifted, "--flip", heat, "--json"});
    REQUIRE(run.exit_code == 0);
    REQUIRE(std::filesystem::exists(heat));
    const JsonValue* written = run.result.find("flip_image");
    REQUIRE(written != nullptr);
    CHECK(written->is_string());
    image::Image decoded;
    std::string error;
    REQUIRE(image::read_image(heat, decoded, 0, &error) == io::Status::Ok);
    CHECK(decoded.width == k_width);
    CHECK(decoded.height == k_height);
    CHECK(decoded.channels == 3);
    // An unwritable destination is a failure to measure, not a silent success.
    const Run bad = image_app({"compare", flat, shifted, "--flip",
                               forward_slashes((dir / "no" / "where" / "x.png").string())});
    CHECK(bad.exit_code == 2);
  }

  SUBCASE("--weights center moves the periphery out of the way") {
    std::vector<u8> center = checker_pixels();
    std::vector<u8> corner = checker_pixels();
    flatten_block(center, 40, 28, 16);
    flatten_block(corner, 8, 8, 16);
    const std::string base = write_fixture(dir, "checker.png", checker_pixels());
    const std::string center_path = write_fixture(dir, "checker_center.png", center);
    const std::string corner_path = write_fixture(dir, "checker_corner.png", corner);

    const Run center_run =
        image_app({"compare", base, center_path, "--weights", "center", "--json"});
    const Run corner_run =
        image_app({"compare", base, corner_path, "--weights", "center", "--json"});
    REQUIRE(center_run.exit_code == 0);
    REQUIRE(corner_run.exit_code == 0);
    // The same damage, so the plain means agree; the attention region is what separates them.
    CHECK(number(center_run.result, "flip_mean") ==
          doctest::Approx(number(corner_run.result, "flip_mean")).epsilon(0.1));
    CHECK(number(center_run.result, "flip_weighted_mean") >
          3.0 * number(corner_run.result, "flip_weighted_mean"));

    // Without the weights, the weighted mean is the mean.
    const Run plain = image_app({"compare", base, center_path, "--json"});
    REQUIRE(plain.exit_code == 0);
    CHECK(number(plain.result, "flip_weighted_mean") ==
          doctest::Approx(number(plain.result, "flip_mean")).epsilon(1e-6));
  }

  SUBCASE("images of different sizes and files that are not images exit 2") {
    const std::string small = forward_slashes((dir / "small.png").string());
    std::vector<u8> pixels(16 * 16 * 4, u8{200});
    REQUIRE(image::write_png(small, 16, 16, 4, std::span<const u8>(pixels.data(), pixels.size())) ==
            io::Status::Ok);
    const Run mismatched = image_app({"compare", flat, small}, /*merge_stderr=*/true);
    CHECK(mismatched.exit_code == 2);
    CHECK(mismatched.output.find("same size") != std::string::npos);
    CHECK(image_app({"compare", flat, junk}).exit_code == 2);
  }

  SUBCASE("info reads the header alone") {
    const Run run = image_app({"info", flat, "--json"});
    REQUIRE(run.exit_code == 0);
    CHECK(number(run.result, "width") == doctest::Approx(k_width));
    CHECK(number(run.result, "height") == doctest::Approx(k_height));
    CHECK(number(run.result, "channels") == doctest::Approx(4));
    CHECK(number(run.result, "bytes") > 0.0);
    const Run readable = image_app({"info", flat});
    CHECK(readable.exit_code == 0);
    CHECK(readable.output.find("96x72") != std::string::npos);
    CHECK(image_app({"info", junk}).exit_code == 2);
  }
}
