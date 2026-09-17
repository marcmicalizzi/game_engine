// The compressor is checked against a decoder written here rather than against itself: a
// reference inflater (RFC 1951, stored, fixed, and dynamic blocks) for the raw streams, and
// stb_image through `decode_image` for the PNGs, which is the decoder the rest of the world
// brings. The inflater below doubles as documentation of the format the encoder emits.
#include <foundation/image/decode.h>
#include <foundation/image/deflate.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::image;

namespace {

// ---- a reference inflater (RFC 1951) -----------------------------------------------------

constexpr u32 k_max_bits = 15;
constexpr u16 k_length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 k_length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 k_dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                 33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr u8 k_clen_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// A canonical code as counts per length plus the symbols in length order (Mark Adler's puff).
struct Code {
  u16 count[k_max_bits + 1];
  u16 symbol[288];
};

// Returns false when the lengths do not describe a usable code.
bool construct(Code& code, const u8* lengths, u32 n) {
  for (u32 len = 0; len <= k_max_bits; ++len)
    code.count[len] = 0;
  for (u32 symbol = 0; symbol < n; ++symbol)
    ++code.count[lengths[symbol]];
  if (code.count[0] == n) return false;  // no symbol has a code
  i32 left = 1;
  for (u32 len = 1; len <= k_max_bits; ++len) {
    left <<= 1;
    left -= code.count[len];
    if (left < 0) return false;  // over-subscribed
  }
  u16 offsets[k_max_bits + 2] = {};
  for (u32 len = 1; len < k_max_bits; ++len)
    offsets[len + 1] = static_cast<u16>(offsets[len] + code.count[len]);
  for (u32 symbol = 0; symbol < n; ++symbol) {
    if (lengths[symbol] != 0) code.symbol[offsets[lengths[symbol]]++] = static_cast<u16>(symbol);
  }
  return true;
}

class Inflater {
 public:
  explicit Inflater(std::span<const u8> bytes) noexcept : in_(bytes) {}

  bool run(Vector<u8>& out) {
    bool final = false;
    while (!final) {
      u32 header = 0;
      if (!bits(1, header)) return false;
      final = header != 0;
      u32 type = 0;
      if (!bits(2, type)) return false;
      if (type == 0) {
        if (!stored(out)) return false;
      } else if (type == 1) {
        Code lit;
        Code dist;
        fixed(lit, dist);
        if (!codes(out, lit, dist)) return false;
      } else if (type == 2) {
        Code lit;
        Code dist;
        if (!dynamic_codes(lit, dist)) return false;
        if (!codes(out, lit, dist)) return false;
      } else {
        return false;
      }
    }
    return true;
  }

  usize consumed() const noexcept { return at_; }

 private:
  bool bits(u32 count, u32& value) {
    while (held_ < count) {
      if (at_ >= in_.size()) return false;
      acc_ |= u32{in_[at_++]} << held_;
      held_ += 8;
    }
    value = count == 0 ? 0u : acc_ & ((1u << count) - 1);
    acc_ >>= count;
    held_ -= count;
    return true;
  }

  // Walks the canonical code one bit at a time; slow and obviously correct.
  bool decode(const Code& code, u32& symbol) {
    u32 value = 0;
    u32 first = 0;
    u32 index = 0;
    for (u32 len = 1; len <= k_max_bits; ++len) {
      u32 bit = 0;
      if (!bits(1, bit)) return false;
      value |= bit;
      const u32 count = code.count[len];
      if (value - first < count) {
        symbol = code.symbol[index + (value - first)];
        return true;
      }
      index += count;
      first = (first + count) << 1;
      value <<= 1;
    }
    return false;
  }

  bool stored(Vector<u8>& out) {
    acc_ = 0;
    held_ = 0;  // stored blocks start on a byte boundary
    if (at_ + 4 > in_.size()) return false;
    const u32 len = u32{in_[at_]} | (u32{in_[at_ + 1]} << 8);
    const u32 nlen = u32{in_[at_ + 2]} | (u32{in_[at_ + 3]} << 8);
    at_ += 4;
    if (((len ^ nlen) & 0xFFFFu) != 0xFFFFu) return false;
    if (at_ + len > in_.size()) return false;
    out.append(in_.subspan(at_, len));
    at_ += len;
    return true;
  }

  static void fixed(Code& lit, Code& dist) {
    u8 lengths[288];
    for (u32 i = 0; i < 144; ++i)
      lengths[i] = 8;
    for (u32 i = 144; i < 256; ++i)
      lengths[i] = 9;
    for (u32 i = 256; i < 280; ++i)
      lengths[i] = 7;
    for (u32 i = 280; i < 288; ++i)
      lengths[i] = 8;
    construct(lit, lengths, 288);
    for (u32 i = 0; i < 30; ++i)
      lengths[i] = 5;
    construct(dist, lengths, 30);
  }

  bool dynamic_codes(Code& lit, Code& dist) {
    u32 hlit = 0;
    u32 hdist = 0;
    u32 hclen = 0;
    if (!bits(5, hlit) || !bits(5, hdist) || !bits(4, hclen)) return false;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) return false;

    u8 clen_lengths[19] = {};
    for (u32 i = 0; i < hclen; ++i) {
      u32 value = 0;
      if (!bits(3, value)) return false;
      clen_lengths[k_clen_order[i]] = static_cast<u8>(value);
    }
    Code clen;
    if (!construct(clen, clen_lengths, 19)) return false;

    u8 lengths[286 + 30] = {};
    u32 index = 0;
    while (index < hlit + hdist) {
      u32 symbol = 0;
      if (!decode(clen, symbol)) return false;
      if (symbol < 16) {
        lengths[index++] = static_cast<u8>(symbol);
        continue;
      }
      u32 value = 0;
      u32 repeat = 0;
      if (symbol == 16) {
        if (index == 0) return false;
        value = lengths[index - 1];
        if (!bits(2, repeat)) return false;
        repeat += 3;
      } else if (symbol == 17) {
        if (!bits(3, repeat)) return false;
        repeat += 3;
      } else {
        if (!bits(7, repeat)) return false;
        repeat += 11;
      }
      if (index + repeat > hlit + hdist) return false;
      for (u32 k = 0; k < repeat; ++k)
        lengths[index++] = static_cast<u8>(value);
    }
    if (!construct(lit, lengths, hlit)) return false;
    return construct(dist, lengths + hlit, hdist);
  }

  bool codes(Vector<u8>& out, const Code& lit, const Code& dist) {
    for (;;) {
      u32 symbol = 0;
      if (!decode(lit, symbol)) return false;
      if (symbol < 256) {
        out.push_back(static_cast<u8>(symbol));
      } else if (symbol == 256) {
        return true;
      } else {
        symbol -= 257;
        if (symbol >= 29) return false;
        u32 extra = 0;
        if (!bits(k_length_extra[symbol], extra)) return false;
        const u32 length = k_length_base[symbol] + extra;
        if (!decode(dist, symbol)) return false;
        if (symbol >= 30) return false;
        if (!bits(k_dist_extra[symbol], extra)) return false;
        const u32 distance = k_dist_base[symbol] + extra;
        if (distance > out.size()) return false;
        const u32 from = out.size() - distance;
        for (u32 k = 0; k < length; ++k) {
          const u8 byte = out[from + k];  // by value: push_back may move the buffer
          out.push_back(byte);            // overlapping copies are the point of LZ77
        }
      }
    }
  }

  std::span<const u8> in_;
  usize at_ = 0;
  u32 acc_ = 0;
  u32 held_ = 0;
};

// ---- test data ---------------------------------------------------------------------------

struct Random {
  u64 state;
  explicit Random(u64 seed) noexcept : state(seed) {}
  u32 next() noexcept {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return static_cast<u32>(z >> 32);
  }
};

Vector<u8> random_bytes(u32 count, u64 seed) {
  Vector<u8> out(count);
  Random rng(seed);
  for (u32 i = 0; i < count; ++i)
    out[i] = static_cast<u8>(rng.next());
  return out;
}

Vector<u8> repeated_pattern(u32 count) {
  Vector<u8> out(count);
  static constexpr u8 k_pattern[7] = {1, 2, 3, 4, 5, 200, 7};
  for (u32 i = 0; i < count; ++i)
    out[i] = k_pattern[i % 7];
  return out;
}

Vector<u8> text_bytes(u32 repeats) {
  static const char* k_line =
      "the cluster cull pass drops backfacing clusters, and the visibility buffer keeps one "
      "u64 per pixel. ";
  const u32 line = static_cast<u32>(std::strlen(k_line));
  Vector<u8> out;
  out.reserve(line * repeats);
  for (u32 r = 0; r < repeats; ++r) {
    for (u32 i = 0; i < line; ++i)
      out.push_back(static_cast<u8>(k_line[i]));
  }
  return out;
}

std::span<const u8> view(const Vector<u8>& v) { return std::span<const u8>(v.data(), v.size()); }

// Encodes, decodes through stb_image, and compares byte for byte.
void check_png_round_trip(u32 width, u32 height, u32 channels, const Vector<u8>& pixels,
                          Compression level) {
  Vector<u8> png;
  REQUIRE(encode_png(width, height, channels, view(pixels), png, level));
  Image image;
  std::string error;
  REQUIRE_MESSAGE(decode_image(view(png), image, 0, &error), error);
  CHECK(image.width == width);
  CHECK(image.height == height);
  CHECK(image.channels == channels);
  REQUIRE(image.pixels.size() == pixels.size());
  CHECK(std::memcmp(image.pixels.data(), pixels.data(), pixels.size()) == 0);
}

// A deterministic picture: flat sky, a vertical gradient, a block of solid colour, and an edge.
Vector<u8> make_image(u32 width, u32 height, u32 channels, u64 seed) {
  Vector<u8> pixels(width * height * channels);
  Random rng(seed);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      u8* p = pixels.data() + (static_cast<usize>(y) * width + x) * channels;
      const u8 gradient = static_cast<u8>((y * 255) / (height > 1 ? height - 1 : 1));
      const bool box = x * 3 > width && x * 3 < width * 2 && y * 3 > height;
      for (u32 c = 0; c < channels; ++c) {
        if (box) {
          p[c] = static_cast<u8>(40 + c * 30);
        } else if (x % 64 == 0) {
          p[c] = static_cast<u8>(rng.next());  // a few noisy columns
        } else {
          p[c] = static_cast<u8>(gradient + c * 7);
        }
      }
      if (channels == 4) p[3] = 255;
      if (channels == 2) p[1] = 255;
    }
  }
  return pixels;
}

constexpr Compression k_levels[3] = {Compression::Stored, Compression::Fast, Compression::Default};

}  // namespace

TEST_CASE("deflate: every level round-trips through a reference inflater") {
  Vector<u8> inputs[4] = {random_bytes(70000, 1), Vector<u8>(50000), repeated_pattern(40000),
                          text_bytes(400)};
  for (const Vector<u8>& input : inputs) {
    for (const Compression level : k_levels) {
      Vector<u8> compressed;
      deflate(view(input), level, compressed);
      Vector<u8> back;
      Inflater inflater(view(compressed));
      REQUIRE(inflater.run(back));
      CHECK(inflater.consumed() == compressed.size());
      REQUIRE(back.size() == input.size());
      CHECK(std::memcmp(back.data(), input.data(), input.size()) == 0);
    }
  }
}

TEST_CASE("deflate: empty and very short inputs are still complete streams") {
  Vector<u8> input;
  for (u32 size = 0; size <= 5; ++size) {
    input.clear();
    for (u32 i = 0; i < size; ++i)
      input.push_back(static_cast<u8>(i + 1));
    for (const Compression level : k_levels) {
      Vector<u8> compressed;
      deflate(view(input), level, compressed);
      CHECK(compressed.size() > 0);
      Vector<u8> back;
      Inflater inflater(view(compressed));
      REQUIRE(inflater.run(back));
      REQUIRE(back.size() == size);
      if (size != 0) CHECK(std::memcmp(back.data(), input.data(), size) == 0);
    }
  }
}

TEST_CASE("deflate: input that cannot be compressed falls back to stored blocks") {
  const Vector<u8> input = random_bytes(1024 * 1024, 7);
  for (const Compression level : k_levels) {
    Vector<u8> compressed;
    deflate(view(input), level, compressed);
    CHECK(u64{compressed.size()} * 100 <= u64{input.size()} * 101);
    Vector<u8> back;
    Inflater inflater(view(compressed));
    REQUIRE(inflater.run(back));
    REQUIRE(back.size() == input.size());
    CHECK(std::memcmp(back.data(), input.data(), input.size()) == 0);
  }
}

TEST_CASE("deflate: a block that shrinks is compressed and blocks span the LZ77 window") {
  // Two megabytes of a short pattern: many blocks, every one of them compressible.
  const Vector<u8> input = repeated_pattern(2 * 1024 * 1024);
  Vector<u8> compressed;
  deflate(view(input), Compression::Default, compressed);
  CHECK(compressed.size() < input.size() / 100);
  Vector<u8> back;
  Inflater inflater(view(compressed));
  REQUIRE(inflater.run(back));
  REQUIRE(back.size() == input.size());
  CHECK(std::memcmp(back.data(), input.data(), input.size()) == 0);
}

TEST_CASE("zlib_compress: the container carries a valid header and checksum") {
  const Vector<u8> input = text_bytes(200);
  for (const Compression level : k_levels) {
    Vector<u8> stream;
    zlib_compress(view(input), level, stream);
    REQUIRE(stream.size() > 6);
    CHECK(stream[0] == 0x78);
    CHECK(((u32{stream[0]} << 8) | stream[1]) % 31 == 0);
    const u32 trailer = (u32{stream[stream.size() - 4]} << 24) |
                        (u32{stream[stream.size() - 3]} << 16) |
                        (u32{stream[stream.size() - 2]} << 8) | stream[stream.size() - 1];
    CHECK(trailer == adler32(view(input)));
    Vector<u8> back;
    Inflater inflater(std::span<const u8>(stream.data() + 2, stream.size() - 6));
    REQUIRE(inflater.run(back));
    REQUIRE(back.size() == input.size());
    CHECK(std::memcmp(back.data(), input.data(), input.size()) == 0);
  }
  // `Stored` is what the encoder produced before there was a compressor: header 0x78 0x01,
  // stored blocks of 65535, adler32.
  Vector<u8> stored;
  zlib_compress(view(input), Compression::Stored, stored);
  CHECK(stored[1] == 0x01);
  CHECK(stored.size() == 2 + 5 + input.size() + 4);
  CHECK(stored[2] == 1);  // BFINAL, BTYPE 00
}

TEST_CASE("png: images round-trip through stb_image at every level") {
  struct Case {
    u32 width;
    u32 height;
    u32 channels;
  };
  // 1x1, a width that is not a multiple of four, and one image whose scanlines run past a
  // single deflate block (512 * 2049 bytes).
  const Case cases[6] = {{1, 1, 4},  {1, 1, 1},     {13, 7, 3},
                         {13, 7, 1}, {300, 300, 4}, {512, 512, 4}};
  for (const Case& c : cases) {
    const Vector<u8> pixels = make_image(c.width, c.height, c.channels, c.width * 31u + c.channels);
    for (const Compression level : k_levels)
      check_png_round_trip(c.width, c.height, c.channels, pixels, level);
  }
}

TEST_CASE("png: a flat image compresses to a fraction of its size") {
  constexpr u32 k_side = 1024;
  constexpr u32 k_raw = k_side * k_side * 4;
  Vector<u8> pixels(k_raw);
  for (u32 i = 0; i < k_side * k_side; ++i) {
    pixels[i * 4 + 0] = 30;
    pixels[i * 4 + 1] = 90;
    pixels[i * 4 + 2] = 160;
    pixels[i * 4 + 3] = 255;
  }
  Vector<u8> png;
  REQUIRE(encode_png(k_side, k_side, 4, view(pixels), png, Compression::Default));
  CHECK(png.size() * 100 < k_raw);  // under 1%
  MESSAGE("flat 1024x1024 RGBA at Default: " << png.size() << " bytes");
  REQUIRE(encode_png(k_side, k_side, 4, view(pixels), png, Compression::Fast));
  CHECK(png.size() * 20 < k_raw);  // under 5%
  MESSAGE("flat 1024x1024 RGBA at Fast: " << png.size() << " bytes");

  // Stored is the other end: the raw size plus the container and the block headers.
  REQUIRE(encode_png(k_side, k_side, 4, view(pixels), png, Compression::Stored));
  CHECK(png.size() > k_raw);
}
