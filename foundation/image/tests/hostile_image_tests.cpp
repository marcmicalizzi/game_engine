// Images whose header states more than the decoder takes (T187, from T13's reading,
// docs/experiments/fuzzing-design.md): a client names an image to `render.compare`, and a scene's
// images and masks are read the same way. Each input is built by a function that returns its
// bytes, so the fuzz harness can lift it as a seed.

#include <foundation/image/decode.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::image;

namespace hostile_image {

// A real PNG one pixel tall and `width` wide: small to hold, and past the bound by its width alone.
Vector<u8> png_of_width(u32 width) {
  Vector<u8> pixels(width * 3, u8{128});
  Vector<u8> png;
  REQUIRE(encode_png(width, 1, 3, std::span<const u8>(pixels.data(), pixels.size()), png));
  return png;
}

// One pixel wider than the bound.
Vector<u8> png_one_past_bound() { return png_of_width(k_max_image_side + 1); }

// The PNG signature, an IHDR claiming `width` by `height` RGB, an IDAT holding an empty zlib
// stream, and IEND: 68 bytes, from which stb sized its inflate buffer by the header (width times
// height times three) before finding no pixels. The chunks' CRCs are zero, which stb does not check.
Vector<u8> png_header_claiming(u32 width, u32 height) {
  Vector<u8> out;
  const u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  for (const u8 b : signature)
    out.push_back(b);
  const auto be32 = [&](u32 v) {
    for (u32 shift = 24;; shift -= 8) {
      out.push_back(static_cast<u8>((v >> shift) & 0xffu));
      if (shift == 0) break;
    }
  };
  be32(13);
  for (const char c : {'I', 'H', 'D', 'R'})
    out.push_back(static_cast<u8>(c));
  be32(width);
  be32(height);
  for (const u8 b : {u8{8}, u8{2}, u8{0}, u8{0}, u8{0}})  // 8 bits, RGB, deflate, no filter, no interlace
    out.push_back(b);
  be32(0);
  // zlib: header 78 01, one final stored block of no bytes, the Adler-32 of nothing.
  const u8 zlib[11] = {0x78, 0x01, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01};
  be32(sizeof(zlib));
  for (const char c : {'I', 'D', 'A', 'T'})
    out.push_back(static_cast<u8>(c));
  for (const u8 b : zlib)
    out.push_back(b);
  be32(0);
  be32(0);
  for (const char c : {'I', 'E', 'N', 'D'})
    out.push_back(static_cast<u8>(c));
  be32(0);
  return out;
}

// Past the bound and inside stb's own guard (2^30 bytes of RGB), so before the bound stb took the
// 807 MB this states.
Vector<u8> png_header_past_bound() { return png_header_claiming(16400, 16400); }

}  // namespace hostile_image

TEST_CASE("decode hostile: an image wider or taller than the bound is refused with the bound") {
  const std::string bound = std::to_string(k_max_image_side);
  Image image;
  std::string error;

  const Vector<u8> at_bound = hostile_image::png_of_width(k_max_image_side);
  REQUIRE_MESSAGE(decode_image(std::span<const u8>(at_bound.data(), at_bound.size()), image, 3,
                               &error),
                  error);
  CHECK(image.width == k_max_image_side);

  const Vector<u8> past = hostile_image::png_one_past_bound();
  CHECK_FALSE(decode_image(std::span<const u8>(past.data(), past.size()), image, 3, &error));
  CHECK(error.find(bound) != std::string::npos);
  CHECK(image.pixels.empty());

  ImageInfo info;
  CHECK_FALSE(probe_image(std::span<const u8>(past.data(), past.size()), info, &error));
  CHECK(error.find(bound) != std::string::npos);

  const Vector<u8> header = hostile_image::png_header_past_bound();
  CHECK_FALSE(decode_image(std::span<const u8>(header.data(), header.size()), image, 3, &error));
  CHECK(error.find(bound) != std::string::npos);
}
