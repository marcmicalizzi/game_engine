// Deflate throughput and ratio per compression level, on the two inputs a capture path meets:
// a 4K render-like frame (flat areas, gradients, a few hard edges, a band of noise) and bytes
// that cannot be compressed at all. The rate column is input bytes per second; the ratio is
// printed once per benchmark because the harness's result carries rates, not ratios.
#include <core/containers/vector.h>
#include <foundation/bench/bench.h>
#include <foundation/image/deflate.h>
#include <foundation/image/png.h>

#include <cstdio>

using namespace engine;

namespace {

constexpr u32 k_width = 3840;
constexpr u32 k_height = 2160;

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

// What a renderer hands a capture: a sky gradient that only varies down the frame, a flat
// ground, two solid panels with hard edges, a smooth radial highlight, and a noise band.
const Vector<u8>& render_image() {
  static const Vector<u8> pixels = [] {
    Vector<u8> out(k_width * k_height * 4);
    Random rng(20260917);
    const u32 horizon = k_height * 5 / 8;
    for (u32 y = 0; y < k_height; ++y) {
      const u32 sky = 60 + (y * 120) / k_height;
      for (u32 x = 0; x < k_width; ++x) {
        u8* p = out.data() + (static_cast<usize>(y) * k_width + x) * 4;
        u32 r = 0;
        u32 g = 0;
        u32 b = 0;
        if (y < horizon) {
          r = sky / 2;
          g = sky;
          b = 200 - sky / 3;
          // A radial highlight: two-dimensional, so it matches nothing on the row above.
          const i32 dx = static_cast<i32>(x) - static_cast<i32>(k_width / 3);
          const i32 dy = static_cast<i32>(y) - static_cast<i32>(k_height / 5);
          const u32 d2 = static_cast<u32>(dx * dx + dy * dy);
          if (d2 < 400u * 400u) {
            const u32 fall = 255 - (d2 / (400u * 400u / 255u));
            r += fall / 2;
            g += fall / 3;
            b += fall / 8;
          }
        } else {
          r = 70;
          g = 55;
          b = 40;  // flat ground
          if (x > k_width / 4 && x < k_width / 2 && y > horizon + 100 && y < horizon + 500) {
            r = 190;
            g = 30;
            b = 30;  // a solid panel with hard edges
          }
        }
        if (y >= k_height - 64) {
          const u32 n = rng.next();
          r = n & 0xFF;
          g = (n >> 8) & 0xFF;
          b = (n >> 16) & 0xFF;  // a band of noise, so nothing is free
        }
        p[0] = static_cast<u8>(r > 255 ? 255 : r);
        p[1] = static_cast<u8>(g > 255 ? 255 : g);
        p[2] = static_cast<u8>(b > 255 ? 255 : b);
        p[3] = 255;
      }
    }
    return out;
  }();
  return pixels;
}

const Vector<u8>& random_bytes() {
  static const Vector<u8> bytes = [] {
    Vector<u8> out(1024 * 1024);
    Random rng(4242);
    for (u32 i = 0; i < out.size(); ++i)
      out[i] = static_cast<u8>(rng.next());
    return out;
  }();
  return bytes;
}

std::span<const u8> view(const Vector<u8>& v) { return std::span<const u8>(v.data(), v.size()); }

// The bench body runs once per repeat, so the ratio line is printed only the first time.
void report_ratio(const char* name, u32 input, u32 output, bool& printed) {
  if (printed) return;
  printed = true;
  std::printf("# %-40s %10u -> %10u bytes, ratio %6.2fx (%.2f%%)\n", name, input, output,
              output != 0 ? static_cast<f64>(input) / output : 0.0,
              100.0 * static_cast<f64>(output) / input);
  std::fflush(stdout);
}

void deflate_loop(bench::State& state, const Vector<u8>& input, image::Compression level,
                  const char* name, bool& printed) {
  Vector<u8> out;
  image::deflate(view(input), level, out);
  report_ratio(name, input.size(), out.size(), printed);
  while (state.keep_running()) {
    image::deflate(view(input), level, out);
    bench::keep(out);
  }
  state.set_items(input.size());
  state.set_bytes(input.size());
}

}  // namespace

// ---- deflate on a 4K render-like frame ---------------------------------------------------

ENGINE_BENCH(deflate_render_stored, "image.deflate.render_4k.stored") {
  static bool printed = false;
  deflate_loop(state, render_image(), image::Compression::Stored, "deflate render_4k stored",
               printed);
}

ENGINE_BENCH(deflate_render_fast, "image.deflate.render_4k.fast") {
  static bool printed = false;
  deflate_loop(state, render_image(), image::Compression::Fast, "deflate render_4k fast", printed);
}

ENGINE_BENCH(deflate_render_default, "image.deflate.render_4k.default") {
  static bool printed = false;
  deflate_loop(state, render_image(), image::Compression::Default, "deflate render_4k default",
               printed);
}

// ---- deflate on incompressible bytes -----------------------------------------------------

ENGINE_BENCH(deflate_random_stored, "image.deflate.random_1mib.stored") {
  static bool printed = false;
  deflate_loop(state, random_bytes(), image::Compression::Stored, "deflate random_1mib stored",
               printed);
}

ENGINE_BENCH(deflate_random_fast, "image.deflate.random_1mib.fast") {
  static bool printed = false;
  deflate_loop(state, random_bytes(), image::Compression::Fast, "deflate random_1mib fast",
               printed);
}

ENGINE_BENCH(deflate_random_default, "image.deflate.random_1mib.default") {
  static bool printed = false;
  deflate_loop(state, random_bytes(), image::Compression::Default, "deflate random_1mib default",
               printed);
}

// ---- the whole encoder, which is what a capture actually pays ----------------------------

ENGINE_BENCH(encode_png_fast, "image.encode_png.render_4k.fast") {
  const Vector<u8>& pixels = render_image();
  static bool printed = false;
  Vector<u8> png;
  image::encode_png(k_width, k_height, 4, view(pixels), png, image::Compression::Fast);
  report_ratio("encode_png render_4k fast", pixels.size(), png.size(), printed);
  while (state.keep_running()) {
    image::encode_png(k_width, k_height, 4, view(pixels), png, image::Compression::Fast);
    bench::keep(png);
  }
  state.set_items(pixels.size());
  state.set_bytes(pixels.size());
}

ENGINE_BENCH(encode_png_default, "image.encode_png.render_4k.default") {
  const Vector<u8>& pixels = render_image();
  static bool printed = false;
  Vector<u8> png;
  image::encode_png(k_width, k_height, 4, view(pixels), png, image::Compression::Default);
  report_ratio("encode_png render_4k default", pixels.size(), png.size(), printed);
  while (state.keep_running()) {
    image::encode_png(k_width, k_height, 4, view(pixels), png, image::Compression::Default);
    bench::keep(png);
  }
  state.set_items(pixels.size());
  state.set_bytes(pixels.size());
}
