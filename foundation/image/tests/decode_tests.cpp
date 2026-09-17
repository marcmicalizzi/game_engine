#include <foundation/image/decode.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

// stb_image_write is a test-only dependency: it writes the baseline JPEG the decoder is checked
// against, so the JPEG path is exercised without a binary fixture in the tree. The module never
// links it. Same warning-suppression pattern as the module's stb_image translation unit.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::image;

namespace {

// A deterministic test image: channel c of pixel i is a different ramp, so a wrong channel
// count or a transposed row shows up immediately.
Vector<u8> make_image(u32 width, u32 height, u32 channels) {
  Vector<u8> pixels(width * height * channels);
  for (u32 i = 0; i < width * height; ++i) {
    for (u32 c = 0; c < channels; ++c)
      pixels[i * channels + c] = static_cast<u8>((i * 37 + c * 61 + 11) & 0xFF);
  }
  return pixels;
}

bool close_enough(u8 a, u8 b, int tolerance) {
  return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tolerance;
}

bool is_empty(const Image& image) {
  return image.width == 0 && image.height == 0 && image.channels == 0 && image.pixels.empty();
}

Image make_dirty() {
  Image image;
  image.width = 7;
  image.height = 7;
  image.channels = 4;
  image.pixels.resize(7 * 7 * 4);
  return image;
}

void put_u32_be(Vector<u8>& out, u32 v) {
  out.push_back(static_cast<u8>(v >> 24));
  out.push_back(static_cast<u8>(v >> 16));
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

void put_chunk(Vector<u8>& out, const char type[4], std::span<const u8> data) {
  put_u32_be(out, static_cast<u32>(data.size()));
  const u32 type_offset = out.size();
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(type[i]));
  for (const u8 b : data)
    out.push_back(b);
  put_u32_be(out, crc32(std::span<const u8>(out.data() + type_offset, 4 + data.size())));
}

// A 16-bit PNG, which encode_png cannot write: the same stored-deflate container with bit
// depth 16 and big-endian samples, small enough to fit one stored block.
void encode_png16(u32 width, u32 height, u32 channels, std::span<const u16> samples,
                  Vector<u8>& out) {
  const u32 row_samples = width * channels;
  Vector<u8> raw;
  for (u32 y = 0; y < height; ++y) {
    raw.push_back(0);  // filter: none
    for (u32 x = 0; x < row_samples; ++x) {
      const u16 sample = samples[y * row_samples + x];
      raw.push_back(static_cast<u8>(sample >> 8));
      raw.push_back(static_cast<u8>(sample));
    }
  }
  REQUIRE(raw.size() <= 65535u);

  Vector<u8> zlib;
  zlib.push_back(0x78);
  zlib.push_back(0x01);
  zlib.push_back(1);  // BFINAL, BTYPE = 00 (stored)
  zlib.push_back(static_cast<u8>(raw.size()));
  zlib.push_back(static_cast<u8>(raw.size() >> 8));
  zlib.push_back(static_cast<u8>(~raw.size()));
  zlib.push_back(static_cast<u8>(~raw.size() >> 8));
  for (const u8 b : raw)
    zlib.push_back(b);
  put_u32_be(zlib, adler32(std::span<const u8>(raw.data(), raw.size())));

  static constexpr u8 k_signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  out.clear();
  for (const u8 b : k_signature)
    out.push_back(b);
  Vector<u8> ihdr;
  put_u32_be(ihdr, width);
  put_u32_be(ihdr, height);
  ihdr.push_back(16);  // bit depth
  static constexpr u8 k_color_type[5] = {0, 0, 4, 2, 6};
  ihdr.push_back(k_color_type[channels]);
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  put_chunk(out, "IHDR", std::span<const u8>(ihdr.data(), ihdr.size()));
  put_chunk(out, "IDAT", std::span<const u8>(zlib.data(), zlib.size()));
  put_chunk(out, "IEND", {});
}

void append_to_vector(void* context, void* data, int size) {
  auto* out = static_cast<Vector<u8>*>(context);
  const auto* bytes = static_cast<const u8*>(data);
  for (int i = 0; i < size; ++i)
    out->push_back(bytes[i]);
}

}  // namespace

TEST_CASE("decode: encoded PNGs decode back to the pixels they were made from") {
  constexpr u32 k_w = 9;
  constexpr u32 k_h = 5;
  for (const u32 channels : {u32{1}, u32{3}, u32{4}}) {
    CAPTURE(channels);
    const Vector<u8> source = make_image(k_w, k_h, channels);
    Vector<u8> png;
    REQUIRE(encode_png(k_w, k_h, channels, std::span<const u8>(source.data(), source.size()), png));
    const auto bytes = std::span<const u8>(png.data(), png.size());

    // desired_channels 0: exactly what the file holds.
    Image as_stored;
    std::string error = "unchanged";
    REQUIRE(decode_image(bytes, as_stored, 0, &error));
    CHECK(error == "unchanged");  // success never writes an error
    CHECK(as_stored.width == k_w);
    CHECK(as_stored.height == k_h);
    CHECK(as_stored.channels == channels);
    REQUIRE(as_stored.pixels.size() == source.size());
    CHECK(std::memcmp(as_stored.pixels.data(), source.data(), source.size()) == 0);

    // desired_channels 4: gray replicates, a missing alpha becomes opaque.
    Image rgba;
    REQUIRE(decode_image(bytes, rgba, 4, &error));
    CHECK(rgba.channels == 4);
    REQUIRE(rgba.pixels.size() == k_w * k_h * 4);
    for (u32 i = 0; i < k_w * k_h; ++i) {
      const u8* want = source.data() + i * channels;
      const u8* got = rgba.pixels.data() + i * 4;
      if (channels == 1) {
        CHECK(got[0] == want[0]);
        CHECK(got[1] == want[0]);
        CHECK(got[2] == want[0]);
        CHECK(got[3] == 255);
      } else {
        CHECK(got[0] == want[0]);
        CHECK(got[1] == want[1]);
        CHECK(got[2] == want[2]);
        CHECK(got[3] == (channels == 4 ? want[3] : 255));
      }
    }
  }
}

TEST_CASE("decode: RGBA reduced to one channel is the luminance of the color") {
  constexpr u32 k_w = 4;
  constexpr u32 k_h = 4;
  const Vector<u8> source = make_image(k_w, k_h, 4);
  Vector<u8> png;
  REQUIRE(encode_png(k_w, k_h, 4, std::span<const u8>(source.data(), source.size()), png));
  Image gray;
  REQUIRE(decode_image(std::span<const u8>(png.data(), png.size()), gray, 1));
  CHECK(gray.channels == 1);
  REQUIRE(gray.pixels.size() == k_w * k_h);
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const int luma = (77 * source[i * 4] + 150 * source[i * 4 + 1] + 29 * source[i * 4 + 2]) >> 8;
    CHECK(close_enough(gray.pixels[i], static_cast<u8>(luma), 2));
  }
}

TEST_CASE("decode: probe_image reads the header without decoding") {
  constexpr u32 k_w = 13;
  constexpr u32 k_h = 7;
  const Vector<u8> source = make_image(k_w, k_h, 3);
  Vector<u8> png;
  REQUIRE(encode_png(k_w, k_h, 3, std::span<const u8>(source.data(), source.size()), png));

  ImageInfo info;
  REQUIRE(probe_image(std::span<const u8>(png.data(), png.size()), info));
  CHECK(info.width == k_w);
  CHECK(info.height == k_h);
  CHECK(info.channels == 3);  // as stored, whatever a decode is later asked for

  // A one-channel PNG probes as one channel even though decode_image defaults to four.
  const Vector<u8> gray = make_image(k_w, k_h, 1);
  REQUIRE(encode_png(k_w, k_h, 1, std::span<const u8>(gray.data(), gray.size()), png));
  REQUIRE(probe_image(std::span<const u8>(png.data(), png.size()), info));
  CHECK(info.channels == 1);
  Image decoded;
  REQUIRE(decode_image(std::span<const u8>(png.data(), png.size()), decoded));
  CHECK(decoded.channels == 4);
}

TEST_CASE("decode: a baseline JPEG decodes to the colors it was written from") {
  // Four flat quadrants on 8x8 block boundaries at quality 95 (no chroma subsampling), so the
  // DCT round trip is close to exact away from the seams.
  constexpr u32 k_w = 32;
  constexpr u32 k_h = 32;
  static constexpr u8 k_colors[4][3] = {
      {200, 30, 40}, {30, 200, 60}, {40, 60, 200}, {220, 210, 60}};
  Vector<u8> source(k_w * k_h * 3);
  for (u32 y = 0; y < k_h; ++y) {
    for (u32 x = 0; x < k_w; ++x) {
      const u32 quadrant = (y < k_h / 2 ? 0u : 2u) + (x < k_w / 2 ? 0u : 1u);
      for (u32 c = 0; c < 3; ++c)
        source[(y * k_w + x) * 3 + c] = k_colors[quadrant][c];
    }
  }
  Vector<u8> jpeg;
  REQUIRE(stbi_write_jpg_to_func(&append_to_vector, &jpeg, static_cast<int>(k_w),
                                 static_cast<int>(k_h), 3, source.data(), 95) != 0);
  REQUIRE(jpeg.size() > 2);

  ImageInfo info;
  REQUIRE(probe_image(std::span<const u8>(jpeg.data(), jpeg.size()), info));
  CHECK(info.width == k_w);
  CHECK(info.height == k_h);
  CHECK(info.channels == 3);

  Image decoded;
  REQUIRE(decode_image(std::span<const u8>(jpeg.data(), jpeg.size()), decoded, 0));
  CHECK(decoded.width == k_w);
  CHECK(decoded.height == k_h);
  CHECK(decoded.channels == 3);
  REQUIRE(decoded.pixels.size() == source.size());
  // Sample the middle of each quadrant, four pixels clear of every seam.
  static constexpr u32 k_probe[4][2] = {{8, 8}, {24, 8}, {8, 24}, {24, 24}};
  for (u32 q = 0; q < 4; ++q) {
    const u32 at = (k_probe[q][1] * k_w + k_probe[q][0]) * 3;
    CAPTURE(q);
    CHECK(close_enough(decoded.pixels[at + 0], k_colors[q][0], 8));
    CHECK(close_enough(decoded.pixels[at + 1], k_colors[q][1], 8));
    CHECK(close_enough(decoded.pixels[at + 2], k_colors[q][2], 8));
  }

  // An alpha channel is invented for a format that has none.
  Image rgba;
  REQUIRE(decode_image(std::span<const u8>(jpeg.data(), jpeg.size()), rgba, 4));
  CHECK(rgba.channels == 4);
  REQUIRE(rgba.pixels.size() == k_w * k_h * 4);
  CHECK(rgba.pixels[3] == 255);
}

TEST_CASE("decode: a 16-bit PNG decodes to 8-bit samples") {
  constexpr u32 k_w = 2;
  constexpr u32 k_h = 2;
  static constexpr u16 k_samples[k_w * k_h * 3] = {
      0xABCD, 0x1234, 0xFFFF, 0x0000, 0x8080, 0x00FF,
      0x7F00, 0xC0DE, 0x0102, 0xFEDC, 0x5678, 0x9A00,
  };
  Vector<u8> png;
  encode_png16(k_w, k_h, 3, std::span<const u16>(k_samples, k_w * k_h * 3), png);

  ImageInfo info;
  REQUIRE(probe_image(std::span<const u8>(png.data(), png.size()), info));
  CHECK(info.width == k_w);
  CHECK(info.height == k_h);
  CHECK(info.channels == 3);

  Image decoded;
  REQUIRE(decode_image(std::span<const u8>(png.data(), png.size()), decoded, 0));
  CHECK(decoded.channels == 3);
  REQUIRE(decoded.pixels.size() == k_w * k_h * 3);
  // 16 bits reduce to their high byte: 0xABCD -> 0xAB, 0x00FF -> 0x00.
  for (u32 i = 0; i < k_w * k_h * 3; ++i) {
    CAPTURE(i);
    CHECK(decoded.pixels[i] == static_cast<u8>(k_samples[i] >> 8));
  }
}

TEST_CASE("decode: read_image round-trips a file written with write_png") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_image_decode_tests";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "round_trip.png").string();

  constexpr u32 k_w = 6;
  constexpr u32 k_h = 4;
  const Vector<u8> source = make_image(k_w, k_h, 4);
  REQUIRE(write_png(path, k_w, k_h, 4, std::span<const u8>(source.data(), source.size())) ==
          io::Status::Ok);

  Image image;
  std::string error;
  REQUIRE(read_image(path, image, 4, &error) == io::Status::Ok);
  CHECK(image.width == k_w);
  CHECK(image.height == k_h);
  CHECK(image.channels == 4);
  REQUIRE(image.pixels.size() == source.size());
  CHECK(std::memcmp(image.pixels.data(), source.data(), source.size()) == 0);

  // A missing file reports the read's own status and leaves the image empty.
  Image missing = make_dirty();
  error.clear();
  const std::string absent = (dir / "not_here.png").string();
  CHECK(read_image(absent, missing, 4, &error) == io::Status::NotFound);
  CHECK_FALSE(error.empty());
  CHECK(is_empty(missing));

  // A file that exists but is not an image fails in the decoder.
  const std::string text = (dir / "not_an_image.png").string();
  REQUIRE(io::write_file(text, "this is not a PNG, it is a sentence.") == io::Status::Ok);
  Image garbage = make_dirty();
  error.clear();
  CHECK(read_image(text, garbage, 4, &error) == io::Status::IoError);
  CHECK_FALSE(error.empty());
  CHECK(is_empty(garbage));

  std::filesystem::remove_all(dir);
}

TEST_CASE("decode: bad input fails with a message and leaves the image empty") {
  Image image = make_dirty();
  std::string error;

  CHECK_FALSE(decode_image({}, image, 4, &error));
  CHECK_FALSE(error.empty());
  CHECK(is_empty(image));

  ImageInfo info;
  info.width = 3;
  error.clear();
  CHECK_FALSE(probe_image({}, info, &error));
  CHECK_FALSE(error.empty());
  CHECK(info.width == 0);

  // Bytes that are not an image in any supported format.
  Vector<u8> junk(64);
  for (u32 i = 0; i < junk.size(); ++i)
    junk[i] = static_cast<u8>(i * 17 + 3);
  const auto junk_bytes = std::span<const u8>(junk.data(), junk.size());
  image = make_dirty();
  error.clear();
  CHECK_FALSE(decode_image(junk_bytes, image, 4, &error));
  CHECK_FALSE(error.empty());
  CHECK(is_empty(image));
  error.clear();
  CHECK_FALSE(probe_image(junk_bytes, info, &error));
  CHECK_FALSE(error.empty());

  const Vector<u8> source = make_image(8, 8, 4);
  Vector<u8> png;
  REQUIRE(encode_png(8, 8, 4, std::span<const u8>(source.data(), source.size()), png));

  // A PNG cut in half.
  image = make_dirty();
  error.clear();
  CHECK_FALSE(decode_image(std::span<const u8>(png.data(), png.size() / 2), image, 4, &error));
  CHECK_FALSE(error.empty());
  CHECK(is_empty(image));

  // A PNG whose IHDR claims a bit depth that does not exist (byte 24 of the container).
  Vector<u8> corrupt = png;
  corrupt[24] = 7;
  image = make_dirty();
  error.clear();
  CHECK_FALSE(decode_image(std::span<const u8>(corrupt.data(), corrupt.size()), image, 4, &error));
  CHECK_FALSE(error.empty());
  CHECK(is_empty(image));

  // The caller asking for an impossible channel count is caught before stb sees anything.
  image = make_dirty();
  error.clear();
  CHECK_FALSE(decode_image(std::span<const u8>(png.data(), png.size()), image, 5, &error));
  CHECK_FALSE(error.empty());
  CHECK(is_empty(image));
  CHECK(read_image("ignored.png", image, 5, &error) == io::Status::InvalidArgument);
}
