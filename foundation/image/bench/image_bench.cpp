// Deflate throughput and ratio per compression level, on the two inputs a capture path meets:
// a 4K render-like frame (flat areas, gradients, a few hard edges, a band of noise) and bytes
// that cannot be compressed at all. The rate column is input bytes per second; the ratio is
// printed once per benchmark because the harness's result carries rates, not ratios.
//
// Beside them, the perceptual metric the optimization loop gates on (docs/plan/04-renderer.md
// §4.8): one LDR-FLIP over a 4K pair. It is here because it is the module's only float-heavy
// kernel — a dozen separable filters over a dozen f32 planes — and therefore the one place in
// `image` where a wider instruction-set baseline could show up at all.
#include <core/containers/vector.h>
#include <foundation/bench/bench.h>
#include <foundation/image/deflate.h>
#include <foundation/image/metrics.h>
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

// ---- the FLIP pair -----------------------------------------------------------------------

// A pair the way the optimization loop meets one: the same frame twice, with the differences a
// cheaper renderer actually produces — a slight exposure shift over the whole picture, a hard
// edge moved by one pixel, and low-amplitude noise where the dither is. FLIP's cost does not
// depend on the content, but a pair that differs nowhere would be a pair somebody later
// "optimized" by short-circuiting, so the content is real.
//
// The smoke run (CTest) drops to a sixth of the width and height: one 4K FLIP is about two
// seconds and buys nothing there, while a 640x360 pair still walks every filter.
u32 flip_width() { return bench::smoke_mode() ? k_width / 6 : k_width; }
u32 flip_height() { return bench::smoke_mode() ? k_height / 6 : k_height; }

image::Image make_flip_image(bool perturbed) {
  const u32 width = flip_width();
  const u32 height = flip_height();
  image::Image out;
  out.width = width;
  out.height = height;
  out.channels = 4;
  out.pixels.resize(static_cast<usize>(width) * height * 4);
  Random rng(perturbed ? 90210u : 31337u);
  const u32 horizon = height * 5 / 8;
  for (u32 y = 0; y < height; ++y) {
    const u32 sky = 60 + (y * 120) / height;
    for (u32 x = 0; x < width; ++x) {
      u8* p = out.pixels.data() + (static_cast<usize>(y) * width + x) * 4;
      i32 r = 0;
      i32 g = 0;
      i32 b = 0;
      if (y < horizon) {
        r = static_cast<i32>(sky / 2);
        g = static_cast<i32>(sky);
        b = static_cast<i32>(200 - sky / 3);
      } else {
        r = 70;
        g = 55;
        b = 40;
        // A hard edge, one pixel further right in the perturbed image: the case FLIP's
        // feature term exists for and a plain per-pixel difference under-reports.
        const u32 edge = width / 4 + (perturbed ? 1u : 0u);
        if (x > edge && x < width / 2 && y > horizon + height / 20 && y < horizon + height / 5) {
          r = 190;
          g = 30;
          b = 30;
        }
      }
      if (perturbed) {
        r = r * 103 / 100;  // 3% exposure
        const u32 n = rng.next();
        r += static_cast<i32>(n & 3u) - 1;
        g += static_cast<i32>((n >> 8) & 3u) - 1;
        b += static_cast<i32>((n >> 16) & 3u) - 1;
      }
      p[0] = static_cast<u8>(r < 0 ? 0 : (r > 255 ? 255 : r));
      p[1] = static_cast<u8>(g < 0 ? 0 : (g > 255 ? 255 : g));
      p[2] = static_cast<u8>(b < 0 ? 0 : (b > 255 ? 255 : b));
      p[3] = 255;
    }
  }
  return out;
}

const image::Image& flip_reference() {
  static const image::Image reference = make_flip_image(false);
  return reference;
}

const image::Image& flip_test() {
  static const image::Image test = make_flip_image(true);
  return test;
}

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

// ---- the perceptual metric, which is what the optimization loop waits on ------------------

ENGINE_BENCH(flip_pair, "image.flip.render_4k") {
  const image::Image& reference = flip_reference();
  const image::Image& test = flip_test();
  image::FloatImage error;
  static bool printed = false;
  if (!printed) {
    printed = true;
    image::flip(reference, test, error);
    std::printf("# %-40s %ux%u, mean FLIP %.4f, p95 %.4f\n", "flip render_4k", reference.width,
                reference.height, static_cast<f64>(image::pool_mean(error)),
                static_cast<f64>(image::pool_percentile(error)));
    std::fflush(stdout);
  }
  while (state.keep_running()) {
    image::flip(reference, test, error);
    bench::keep(error.values);
  }
  state.set_items(static_cast<usize>(reference.width) * reference.height);
}
