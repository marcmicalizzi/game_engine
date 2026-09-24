// A built texture is a function of its source and its options on every toolchain and for every
// thread count (ADR-0035, docs/subsystems/texture.md "Determinism"). Integer-made images go through
// every format the builder writes — the mip filter in all three modes, both encoder libraries,
// BC7 with perceptual and linear weights, a PNG through the decoder with the identity recorded —
// and the hash of each file and of its block payload is compared with the table committed here.
//
// **The table was taken on MSVC** and is what every other toolchain has to reproduce. A mismatch is
// a bug until shown otherwise: something contracting a multiply-add, a C library call in a
// decision, an uninitialized byte, a sort that is not a total order. Only a deliberate change to
// the builder — the filter, an encoder setting, the encoders' pin, the container — replaces it, and
// then together with a bump of `k_texture_cache_version`; the failure prints the replacement in the
// form the source wants.
#include "fixtures.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

using namespace engine;
using namespace engine::texture;

namespace {

struct Case {
  const char* name;
  u32 width;
  u32 height;
  u32 image;  // 0 colour (alpha), 1 colour (opaque), 2 normal, 3 grey
  FormatChoice format;
  ColorSpace space;
  bool normal;
  bool mips;
  bool through_png;
};

constexpr Case k_cases[] = {
    {"bc7_srgb", 97, 61, 0, FormatChoice::bc7, ColorSpace::srgb, false, true, false},
    {"bc7_linear", 64, 32, 1, FormatChoice::bc7, ColorSpace::linear, false, true, false},
    {"bc1_srgb", 97, 61, 1, FormatChoice::bc1, ColorSpace::srgb, false, true, false},
    {"bc3_srgb", 61, 97, 0, FormatChoice::bc3, ColorSpace::srgb, false, true, false},
    {"bc5_normal", 64, 64, 2, FormatChoice::automatic, ColorSpace::linear, true, true, false},
    {"bc4_grey", 50, 30, 3, FormatChoice::bc4, ColorSpace::linear, false, true, false},
    {"rgba8_nomips", 33, 17, 0, FormatChoice::rgba8, ColorSpace::linear, false, false, false},
    {"bc7_png", 40, 24, 1, FormatChoice::automatic, ColorSpace::srgb, false, true, true},
};

struct Golden {
  u64 content;  // the file's content hash (the header's, every byte after the header)
  u64 blocks;   // the `data` section's payload alone
};

// Taken on MSVC 14.51 (msvc-debug), 2026-09-24, at k_texture_cache_version 1.
constexpr Golden k_golden[] = {
    {0xb44fc7fe3316033aull, 0xa95d66a7f41d6a92ull},  // bc7_srgb
    {0xa5e0149565818232ull, 0x5f56dd427532c4c2ull},  // bc7_linear
    {0x276aacf49ff8a935ull, 0xaa429f1812cb6bc5ull},  // bc1_srgb
    {0xb4fc689f3761ef7cull, 0x5f451d1e04c69cadull},  // bc3_srgb
    {0x3667aee86889668cull, 0x956f656a27da7005ull},  // bc5_normal
    {0x838478878e8b21d2ull, 0xc681662078f75353ull},  // bc4_grey
    {0x9ffbe791f64659f9ull, 0x72eecca79d48a509ull},  // rgba8_nomips
    {0x630c069169f3816aull, 0xfd256e6c797136c7ull},  // bc7_png
};
static_assert(std::size(k_golden) == std::size(k_cases));

void source_pixels(const Case& c, Vector<u8>& out) {
  switch (c.image) {
    case 0: test_fixtures::colour_image(c.width, c.height, true, out); break;
    case 1: test_fixtures::colour_image(c.width, c.height, false, out); break;
    case 2: test_fixtures::normal_image(c.width, c.height, out); break;
    default: test_fixtures::gray_image(c.width, c.height, out); break;
  }
}

bool build(const Case& c, jobs::JobSystem* pool, TextureData& out, std::string& error) {
  TextureBuildOptions options;
  options.format = c.format;
  options.color_space = c.space;
  options.normal_map = c.normal;
  options.mips = c.mips;
  Vector<u8> pixels;
  source_pixels(c, pixels);
  if (c.through_png) {
    Vector<u8> png;
    if (!image::encode_png(c.width, c.height, 4, std::span<const u8>(pixels.data(), pixels.size()),
                           png)) {
      error = "encode_png failed";
      return false;
    }
    return build_texture_from_encoded(std::span<const u8>(png.data(), png.size()), options, out,
                                      pool, &error);
  }
  return build_texture(std::span<const u8>(pixels.data(), pixels.size()), c.width, c.height,
                       c.image == 3 ? 1u : 4u, options, out, pool, &error);
}

}  // namespace

TEST_CASE("texture determinism: every format builds the committed bytes, serial and parallel") {
  const test::TempDir tmp("texture_determinism");
  jobs::JobSystemConfig config;
  config.performance_workers = 3;
  config.efficiency_workers = 1;
  config.pin_threads = false;
  jobs::JobSystem pool(config);

  std::string table = "this build's table:\nconstexpr Golden k_golden[] = {\n";
  bool same = true;
  for (usize i = 0; i < std::size(k_cases); ++i) {
    const Case& c = k_cases[i];
    TextureData serial;
    TextureData parallel;
    std::string error;
    REQUIRE_MESSAGE(build(c, nullptr, serial, error), std::string(c.name) << ": " << error);
    REQUIRE_MESSAGE(build(c, &pool, parallel, error), std::string(c.name) << ": " << error);
    const u64 content = texture_file_hash(serial);
    CHECK_MESSAGE(content == texture_file_hash(parallel), std::string(c.name)
                                                              << ": the thread count moved it");

    // Through the file, so the hash is of what a reader gets and not only of what was built.
    const std::string path = tmp.file(std::string(c.name) + ".tex");
    REQUIRE_MESSAGE(write_texture_file(path, serial, &error), error);
    std::string bytes;
    REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
    TextureFileHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    CHECK(header.content_hash == content);
    const u64 blocks = hash_bytes(serial.data.data(), serial.data.size());

    char line[160];
    std::snprintf(line, sizeof(line), "    {0x%016llxull, 0x%016llxull},  // %s\n",
                  static_cast<unsigned long long>(content), static_cast<unsigned long long>(blocks),
                  c.name);
    table += line;
    const bool match = content == k_golden[i].content && blocks == k_golden[i].blocks;
    if (!match) table += "    // ^ differs\n";
    same = same && match;
    MESSAGE(std::string(c.name) << ": " << std::string(texture_format_name(serial.format)) << " "
                                << std::string(color_space_name(serial.color_space)) << ", "
                                << serial.levels.size() << " levels, " << serial.data.size()
                                << " bytes of blocks");
  }
  table += "};\n";
  if (!same) FAIL_CHECK("the textures do not match the committed hashes; " << table);
}
