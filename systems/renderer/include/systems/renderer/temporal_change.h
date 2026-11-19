#pragma once

// **What changes on the screen from one frame to the next** (renderer.md, "The change between
// frames, as the eye sees it"; roadmap R171, leg 4). The owner's fault with the bounce indoors was
// not its grain but that "it changes every single frame": a measure of that has to be taken where
// the eye takes it — on the displayed picture, after the exposure, the curve, the dither and the
// 8-bit store — and not on the light before them. So this reads the pictures a run captures
// (`CapturedFrame::color`, RGBA8 as displayed) one after another and counts, for every pixel of
// every consecutive pair:
//
// - **the change of its displayed luma** in code steps, Y' = 0.2126 R' + 0.7152 G' + 0.0722 B' of
//   the codes (Rec. 709's luma weights on the encoded values), in sixteenths of a step;
// - **the change of its light** as the display makes it, relative to its level (a Weber fraction,
//   |L1 - L0| over their mean), each channel taken through the 2.2 power the encode inverted and
//   weighed by Rec. 709's luminance weights; a pixel black in both frames has none.
//
// Both by **the pixel's displayed luma** in six bands — 0-7, 8-15, 16-31, 32-63, 64-127 and 128-255
// codes — because an eye's threshold for a change depends on the light it is seen at (the page
// cites what it is). It keeps no picture but the last, and one histogram per band and measure, so a
// 4K run of hundreds of frames costs the last picture's 33 MB and a few hundred kilobytes.
//
// Beside the histograms, each frame's mean luma and mean light over the whole picture and over its
// centre (the middle half in each direction): the series a moving case (the sun at the day's rate,
// a walk through a doorway) is read from — how many frames the picture's level takes to settle, and
// whether it overshoots or trails.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <string>

namespace engine::renderer {

class TemporalChange {
 public:
  static constexpr u32 k_bands = 6;
  // The luma bands' lower edges, in codes; the last ends at 256.
  static constexpr u32 k_band_low[k_bands] = {0, 8, 16, 32, 64, 128};
  // Luma change: sixteenths of a code step, up to 64 steps (the last bin holds everything past).
  static constexpr u32 k_code_bins = 1024;
  static constexpr f64 k_code_bin = 1.0 / 16.0;
  // Light change: tenths of a percent, up to 100 % (the last bin holds everything past).
  static constexpr u32 k_light_bins = 1000;
  static constexpr f64 k_light_bin = 0.001;

  // One displayed picture, RGBA8 rows top first. The first one only sets what the next is compared
  // with; a picture of another size starts again (and says so by returning false).
  bool add(const u8* rgba, u32 width, u32 height);

  struct Stats {
    u64 pairs = 0;         // pixel changes counted
    f64 mean_codes = 0.0;  // luma change, code steps
    f64 median_codes = 0.0;
    f64 p95_codes = 0.0;
    f64 p99_codes = 0.0;
    f64 p999_codes = 0.0;
    f64 at_least_one = 0.0;  // the share whose luma moved a whole step or more
    f64 at_least_two = 0.0;
    // The forest session's measure, so the engine and the forest quote one number (R171's sixth
    // leg): the share of pixels of which any one channel moved a whole code or more, and three or
    // more. A grey step of one code moves all three channels by one and the luma by one; a step
    // of one channel alone moves the luma by 0.07 to 0.72 of a code, so this share is the larger.
    f64 any_channel_one = 0.0;
    f64 any_channel_three = 0.0;
    u64 light_pairs = 0;   // those with light in either frame
    f64 mean_light = 0.0;  // relative change of the light, a fraction
    f64 median_light = 0.0;
    f64 p95_light = 0.0;
    f64 p99_light = 0.0;
    f64 p999_light = 0.0;
  };
  // `band` < k_bands, or k_bands for every pixel.
  Stats stats(u32 band) const;
  u32 frames() const noexcept { return frames_; }
  // Per frame added: the mean luma (codes) and mean light (1.0 the display's white) over the whole
  // picture and over its centre.
  const Vector<f64>& mean_luma() const noexcept { return mean_luma_; }
  const Vector<f64>& mean_light() const noexcept { return mean_light_; }
  const Vector<f64>& centre_luma() const noexcept { return centre_luma_; }
  const Vector<f64>& centre_light() const noexcept { return centre_light_; }

  // The whole report as one JSON object: frames, size, `all` and `bands` (each band's edges and
  // Stats), and the four series.
  std::string json() const;

 private:
  void count(u32 band, f64 codes, f64 light, bool lit, u32 channel_codes);

  u32 width_ = 0;
  u32 height_ = 0;
  u32 frames_ = 0;
  Vector<u8> previous_;
  Vector<u64> code_hist_;      // (k_bands + 1) * k_code_bins
  Vector<u64> light_hist_;     // (k_bands + 1) * k_light_bins
  Vector<f64> code_sum_;       // k_bands + 1
  Vector<f64> light_sum_;      // k_bands + 1
  Vector<u64> lit_pairs_;      // k_bands + 1
  Vector<u64> channel_one_;    // k_bands + 1: pairs with a channel a code or more apart
  Vector<u64> channel_three_;  // k_bands + 1: ... three or more
  Vector<f64> mean_luma_;
  Vector<f64> mean_light_;
  Vector<f64> centre_luma_;
  Vector<f64> centre_light_;
};

}  // namespace engine::renderer
