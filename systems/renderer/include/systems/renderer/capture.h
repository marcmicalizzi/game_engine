#pragma once

// What a frame can be read back as (docs/subsystems/renderer.md, docs/plan/04-renderer.md §4.8
// and 06 §6.9). A capture is how an agent looks at what the renderer drew: `color` is the
// picture, `ids` is the plan's entity-ID buffer, and `depth` and `normals` are the two channels
// a review most often needs beside it.
//
// **The id channel.** The visibility buffer holds `depth << 32 | (visible_index << 8 |
// triangle)` per pixel, and `visible_index` names an entry of *this frame's* visible list, not
// anything stable across frames. A capture therefore resolves it: it reads the visible list
// back with the visibility buffer and writes the pair the id led to — the **scene instance**,
// the **cluster**, and the **triangle within the cluster** — as three `u32` per pixel. That is
// the number an agent can act on ("which instance is under this pixel"), and it stays the same
// whichever rasterizer or LOD cut produced it, which a raw visibility id does not.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/display.h>

#include <string>
#include <string_view>

namespace engine::renderer {

// An id word for a pixel the scene did not cover.
inline constexpr u32 k_no_id = ~u32{0};
// Words per pixel in the id buffer: instance, cluster, triangle.
inline constexpr u32 k_id_words = 3;

struct CaptureChannels {
  bool color = true;
  // The picture's linear light (`capture_light`): what an HDR picture, or the linear radiance, is
  // captured as — an 8-bit `color` cannot hold either — and an SDR one's light beside its PNG.
  bool light = false;
  bool ids = false;
  bool depth = false;
  bool normals = false;

  bool any_visibility() const noexcept { return ids || depth; }
};

// **The codes a picture was stored in, counted** (E39's histogram): per colour channel, how many
// pixels hold each code. `pq` is 10-bit PQ codes over BT.2020 — an HDR10 picture's own, and for a
// scRGB or linear picture the codes an HDR10 signal of the same light would carry (scRGB at 80 nits
// a unit, the linear radiance at paper white, both clipped at PQ's 10,000 nits); `sdr` is an SDR
// target's own 8- or 10-bit codes.
struct CodeHistogram {
  const char* domain = "sdr";  // "sdr" or "pq"
  u32 bits = 0;                // 8 or 10; 0 for none
  Vector<u32> counts;          // 3 << bits: red's 2^bits counts, then green's, then blue's
};

// One frame read back. Empty vectors are channels that were not asked for.
struct CapturedFrame {
  u32 width = 0;
  u32 height = 0;
  Vector<u8> color;      // RGBA8, rows tightly packed, top row first
  Vector<u8> normals;    // RGB8, the world normal encoded n * 0.5 + 0.5
  Vector<u32> ids;       // k_id_words per pixel: instance, cluster, triangle
  Vector<f32> depth;     // reversed-Z clip depth; 0 where the scene did not cover the pixel
  f32 depth_min = 0.0f;  // over the covered pixels; the range the depth PNG is normalized over
  f32 depth_max = 0.0f;
  u32 covered = 0;  // pixels the scene covered
  // The picture's light (`capture_light`): RGB per pixel, linear Rec. 709, **1.0 the picture's
  // white** — paper white for an HDR picture, the SDR picture's own white, a white Lambertian
  // surface under the metered light for the linear radiance — so the three compare directly, and
  // below the curve's knee an HDR picture's light is the SDR picture's. Written as `<stem>.exr`.
  Vector<f32> light;
  CodeHistogram codes;  // with `light`: `<stem>.codes.json`
  gfx::DisplayEncoding encoding = gfx::DisplayEncoding::Sdr;
  f32 white_nits =
      0.0f;  // the nits of `light`'s 1.0 for an HDR picture (paper white); 0 if unknown
};

// Where `write_capture` put each channel. Paths are native; `uri` forms are the caller's job
// (engine-host makes `file://` URIs of them).
struct CaptureFiles {
  std::string color;
  std::string light;       // the half-float EXR
  std::string codes;       // its code histogram
  std::string ids;         // the raw u32 array
  std::string ids_header;  // the JSON that describes it
  std::string depth;
  std::string normals;
};

// Decodes a capture of the picture into `frame.light` and `frame.codes` (and sets `encoding`,
// `white_nits`, `width`, `height`): an SDR target's codes through the 1/2.2 power; HDR10's PQ codes
// to nits, back from BT.2020 to Rec. 709 (`gfx::bt2020_to_bt709`) and over paper white; scRGB's
// halves times 80 nits over paper white; the linear radiance as it is. `paper_white_nits` is the
// frame's (`display_levels`). False, with `error`, for a format the encoding is never drawn into.
bool capture_light(const gfx::Capture& picture, gfx::DisplayEncoding encoding, f32 paper_white_nits,
                   CapturedFrame& frame, std::string* error = nullptr);

// Writes every channel `frame` carries into `dir`, named `<base>.<channel>.<ext>` (the color
// channel is `<base>.png`, so a capture of one channel is one obvious file). The id buffer is
// a raw little-endian `u32` array plus a JSON header, because it is data to be indexed rather
// than a picture to be looked at, and any lossy or reordered image form would make a pixel's
// instance number unreadable. Depth is an 8-bit gray PNG normalized over the frame's own depth
// range, which is in the header and in the result, so it can be looked at; the exact values are
// in the stats, not in the pixels.
bool write_capture(std::string_view dir, std::string_view base, const CapturedFrame& frame,
                   CaptureFiles& out, std::string& error);

// The JSON header the id buffer is written with, also returned by engine-host so a client that
// reads the bytes over a protocol rather than off disk needs no second file.
std::string id_buffer_header(const CapturedFrame& frame, std::string_view ids_file);

}  // namespace engine::renderer
