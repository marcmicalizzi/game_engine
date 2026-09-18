#pragma once

// Perceptual and signal image metrics on the CPU (docs/plan/04-renderer.md §4.8): FLIP as the
// primary gate for a comparison against the reference renderer, PSNR and SSIM as the secondary
// numbers, and per-region pooling so that a regression in the periphery weighs less than one in
// the middle of the picture.
//
// clang-format off
//     image::Image reference, test;                      // 8-bit RGB or RGBA, same size
//     image::FloatImage error;
//     image::flip(reference, test, error);               // per-pixel LDR-FLIP in [0, 1]
//     const f32 mean = image::pool_mean(error);
//     const f32 p95  = image::pool_percentile(error);    // the tail, which the mean hides
//     image::FloatImage weights;
//     image::center_weight_map(error.width, error.height, {}, weights);
//     const f32 attention = image::pool_weighted_mean(error, weights);
//     image::Image heat;  image::flip_heat_map(error, heat);  // for write_png
// clang-format on
//
// FLIP is the LDR evaluator of Andersson, Nilsson, Akenine-Möller, Oskarsson, Åström, and
// Fairchild, "FLIP: A Difference Evaluator for Alternating Images", HPG 2020: the two images
// are taken to linear sRGB, then to YCxCz, filtered by the spatial contrast sensitivity of the
// three opponent channels at a given viewing density, compared with the Hunt-adjusted HyAB
// color difference, and combined with a feature difference from Gaussian derivative filters
// that catches an edge or a point the color difference alone would call small. The result is
// one number per pixel in [0, 1], where about 0.1 is the threshold a person starts to notice.
//
// Everything here is f32 and single-threaded, with separable filters and no allocation inside
// the loops; the working set is about a dozen float planes, so a 3840x2160 pair costs a few
// hundred megabytes and a couple of seconds. Tiling and the job system are the next step if
// that ever matters (docs/subsystems/image.md).
//
// Not here: HDR-FLIP (which needs exposure sweeps and a float image type this module does not
// have yet), the temporal-stability metric of §4.8, and any GPU implementation.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/image/decode.h>

#include <string>

namespace engine::image {

// One f32 per pixel, rows top first: a FLIP error map, an SSIM map, or a weight map.
struct FloatImage {
  u32 width = 0;
  u32 height = 0;
  Vector<f32> values;

  usize pixel_count() const noexcept {
    return static_cast<usize>(width) * static_cast<usize>(height);
  }
  bool empty() const noexcept { return values.empty(); }
  f32 at(u32 x, u32 y) const noexcept { return values[y * width + x]; }
};

// ---- viewing conditions ------------------------------------------------------------------

// FLIP's reference viewing density: 3840 pixels across a 0.7 m wide screen seen from 0.7 m,
// which is 3840 * pi / 180 pixels per degree. Roughly a 4K 31" monitor at arm's length; a 4K
// 27" monitor at the same distance is nearer 78, which is what `pixels_per_degree` returns for
// it. The default is the paper's so that numbers printed here compare with numbers printed
// anywhere else, and the parameter is there for the case where the display is known.
inline constexpr f32 k_default_pixels_per_degree = 67.0206f;

// Pixels per degree of visual angle for a display of `width_px` by `height_px` pixels whose
// diagonal measures `diagonal_inches` seen from `distance_meters`. Zero for nonsense input.
f32 pixels_per_degree(u32 width_px, u32 height_px, f32 diagonal_inches, f32 distance_meters);

// ---- signal metrics ----------------------------------------------------------------------

// Peak signal-to-noise ratio in decibels over the R, G, and B channels of the stored (sRGB)
// values, alpha ignored. Identical images give an infinity; images that disagree in size or
// carry no pixels give a NaN.
f32 psnr_rgb(const Image& a, const Image& b);

// Structural similarity (Wang, Bovik, Sheikh, Simoncelli 2004) on Rec.601 luma of the stored
// values, with the paper's 11x11 Gaussian window (sigma 1.5) and constants (K1 0.01,
// K2 0.03), averaged over the windows that fit entirely inside the image. Identical images
// give exactly 1, and the measure is symmetric. An image narrower or shorter than the window
// is measured as a single window over the whole image. `map`, when given, receives the
// per-window SSIM, which is (width - 10) by (height - 10). A NaN for mismatched sizes.
f32 ssim_luma(const Image& a, const Image& b, FloatImage* map = nullptr);

// ---- FLIP --------------------------------------------------------------------------------

struct FlipOptions {
  f32 pixels_per_degree = k_default_pixels_per_degree;
};

// Per-pixel LDR-FLIP error in [0, 1], same size as the inputs. False with `error` when the
// two images disagree in size, carry no pixels, or have no color channels.
bool flip(const Image& reference, const Image& test, FloatImage& out,
          const FlipOptions& options = FlipOptions{}, std::string* error = nullptr);

// ---- pooling -----------------------------------------------------------------------------

// The arithmetic mean, which is the number FLIP is usually quoted as.
f32 pool_mean(const FloatImage& error);
// The mean weighted per pixel, for the per-region rule of §4.8: sum(w*e) / sum(w). Zero when
// the weights do not match the error map or add up to nothing.
f32 pool_weighted_mean(const FloatImage& error, const FloatImage& weights);
// The value at `fraction` of the sorted errors (0.95: the worst 5% start here). The mean says
// whether a picture changed; this says whether some corner of it changed a lot.
f32 pool_percentile(const FloatImage& error, f32 fraction = 0.95f);
f32 pool_max(const FloatImage& error);

// A Gaussian falloff from the center of the frame, in normalized screen coordinates, so the
// weighting is an ellipse in pixels and a 48:9 surround frame weighs its side monitors down
// the way §4.6 wants: the attention region is the middle of the picture.
struct CenterWeightOptions {
  f32 sigma = 0.35f;  // falloff, in units of half the frame (1.0 reaches the edge)
  f32 floor = 0.05f;  // what a pixel in the far corner still counts for
};
void center_weight_map(u32 width, u32 height, const CenterWeightOptions& options, FloatImage& out);

// An error map as an RGB8 picture through a perceptually uniform ramp (black through purple
// and red to pale yellow), which is what `--flip out.png` writes. Errors are read as [0, 1].
// False when the map is empty.
bool flip_heat_map(const FloatImage& error, Image& out);

// ---- everything at once ------------------------------------------------------------------

struct MetricsOptions {
  f32 pixels_per_degree = k_default_pixels_per_degree;
  f32 percentile = 0.95f;
  const FloatImage* weights = nullptr;  // null: the weighted mean equals the mean
};

struct ImageMetrics {
  f32 psnr = 0.0f;
  f32 ssim = 0.0f;
  f32 flip_mean = 0.0f;
  f32 flip_weighted_mean = 0.0f;
  f32 flip_percentile = 0.0f;
  f32 flip_max = 0.0f;
};

// PSNR, SSIM, and the pooled FLIP numbers in one pass over the pair. `flip_map`, when given,
// receives the per-pixel error so a caller can write a heat map without running FLIP twice.
bool compare_images(const Image& a, const Image& b, const MetricsOptions& options,
                    ImageMetrics& out, FloatImage* flip_map = nullptr,
                    std::string* error = nullptr);

}  // namespace engine::image
