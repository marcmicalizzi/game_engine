#include <domain/gfx/display.h>
#include <foundation/image/exr.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>

#include <cstdio>
#include <cstring>

namespace engine::renderer {

namespace {

std::string join(std::string_view dir, std::string_view name) {
  return dir.empty() ? std::string(name) : io::join_path(dir, name);
}

bool write_png_file(const std::string& path, u32 width, u32 height, u32 channels,
                    std::span<const u8> pixels, std::string& error) {
  const io::Status status = image::write_png(path, width, height, channels, pixels);
  if (status == io::Status::Ok) return true;
  error = "cannot write " + path + ": " + io::status_name(status);
  return false;
}

// `<stem>.codes.json`: the histogram of a captured picture's codes (`CodeHistogram`), with what a
// reader wants first — how many distinct codes each channel holds and their range — and then every
// count, one array per channel indexed by code.
std::string codes_json(const CapturedFrame& frame, std::string_view light_file) {
  const CodeHistogram& h = frame.codes;
  const u32 levels = 1u << h.bits;
  std::string distinct = "[";
  std::string lowest = "[";
  std::string highest = "[";
  std::string counts = "[";
  for (u32 c = 0; c < 3; ++c) {
    u32 n = 0;
    u32 lo = levels;
    u32 hi = 0;
    std::string row = "[";
    for (u32 k = 0; k < levels; ++k) {
      const u32 count = h.counts[c * levels + k];
      if (count != 0) {
        ++n;
        if (lo == levels) lo = k;
        hi = k;
      }
      row += (k == 0 ? "" : ",") + std::to_string(count);
    }
    const char* sep = c == 0 ? "" : ",";
    distinct += sep + std::to_string(n);
    lowest += sep + std::to_string(lo == levels ? 0u : lo);
    highest += sep + std::to_string(hi);
    counts += sep + row + "]";
  }
  char head[384] = {};
  std::snprintf(head, sizeof(head),
                "{\"format\":\"engine.renderer.codes.v1\",\"file\":\"%.*s\",\"width\":%u,"
                "\"height\":%u,\"encoding\":\"%s\",\"domain\":\"%s\",\"bits\":%u,"
                "\"white_nits\":%.3f,\"channels\":[\"r\",\"g\",\"b\"],",
                static_cast<int>(light_file.size()), light_file.data(), frame.width, frame.height,
                gfx::display_encoding_name(frame.encoding), h.domain, h.bits,
                static_cast<f64>(frame.white_nits));
  return std::string(head) + "\"distinct\":" + distinct + "],\"min\":" + lowest +
         "],\"max\":" + highest + "],\"counts\":" + counts + "]}\n";
}

}  // namespace

std::string id_buffer_header(const CapturedFrame& frame, std::string_view ids_file) {
  char text[512] = {};
  std::snprintf(
      text, sizeof(text),
      "{\"format\":\"engine.renderer.ids.v1\",\"file\":\"%.*s\",\"width\":%u,\"height\":%u,"
      "\"type\":\"u32\",\"byte_order\":\"little\",\"words_per_pixel\":%u,"
      "\"channels\":[\"instance\",\"cluster\",\"triangle\"],\"order\":\"row_major_top_first\","
      "\"empty\":%u,\"covered\":%u}\n",
      static_cast<int>(ids_file.size()), ids_file.data(), frame.width, frame.height, k_id_words,
      k_no_id, frame.covered);
  return text;
}

bool write_capture(std::string_view dir, std::string_view base, const CapturedFrame& frame,
                   CaptureFiles& out, std::string& error) {
  if (!dir.empty()) {
    const io::Status status = io::make_directories(dir);
    if (status != io::Status::Ok) {
      error = "cannot create " + std::string(dir) + ": " + io::status_name(status);
      return false;
    }
  }
  const std::string stem(base);
  if (!frame.color.empty()) {
    out.color = join(dir, stem + ".png");
    if (!write_png_file(out.color, frame.width, frame.height, 4,
                        std::span<const u8>(frame.color.data(), frame.color.size()), error)) {
      return false;
    }
  }
  if (!frame.normals.empty()) {
    out.normals = join(dir, stem + ".normals.png");
    if (!write_png_file(out.normals, frame.width, frame.height, 3,
                        std::span<const u8>(frame.normals.data(), frame.normals.size()), error)) {
      return false;
    }
  }
  if (!frame.ids.empty()) {
    // Three little-endian u32 per pixel. The bytes are written by hand rather than through a
    // writer that could reorder or pad them: an id buffer is only useful if a reader can index
    // it with `(y * width + x) * 3` and get the instance back.
    out.ids = join(dir, stem + ".ids.bin");
    Vector<u8> bytes(frame.ids.size() * 4);
    for (u32 i = 0; i < frame.ids.size(); ++i) {
      const u32 v = frame.ids[i];
      bytes[i * 4 + 0] = static_cast<u8>(v & 0xffu);
      bytes[i * 4 + 1] = static_cast<u8>((v >> 8) & 0xffu);
      bytes[i * 4 + 2] = static_cast<u8>((v >> 16) & 0xffu);
      bytes[i * 4 + 3] = static_cast<u8>((v >> 24) & 0xffu);
    }
    io::Status status = io::write_file(
        out.ids, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (status != io::Status::Ok) {
      error = "cannot write " + out.ids + ": " + io::status_name(status);
      return false;
    }
    out.ids_header = join(dir, stem + ".ids.json");
    status = io::write_file(out.ids_header, id_buffer_header(frame, stem + ".ids.bin"));
    if (status != io::Status::Ok) {
      error = "cannot write " + out.ids_header + ": " + io::status_name(status);
      return false;
    }
  }
  if (!frame.depth.empty()) {
    // Reversed-Z clip depth normalized over the frame's own covered range, so the picture is
    // readable at any scene scale; 0 (black) is both "not covered" and the far end of the range,
    // and the exact numbers are the `depth_min`/`depth_max` the caller reports beside it. An
    // 8-bit gray PNG is what the engine's own encoder writes, and a depth channel is looked at
    // far more often than it is measured — a measurement takes the ids beside it.
    out.depth = join(dir, stem + ".depth.png");
    const f32 span = frame.depth_max - frame.depth_min;
    Vector<u8> gray(frame.depth.size());
    for (u32 i = 0; i < frame.depth.size(); ++i) {
      if (frame.depth[i] == 0.0f || span <= 0.0f) {
        gray[i] = frame.depth[i] == 0.0f ? 0u : 255u;
        continue;
      }
      const f32 t = (frame.depth[i] - frame.depth_min) / span;
      gray[i] = static_cast<u8>(t * 255.0f + 0.5f);
    }
    if (!write_png_file(out.depth, frame.width, frame.height, 1,
                        std::span<const u8>(gray.data(), gray.size()), error)) {
      return false;
    }
  }
  if (!frame.light.empty()) {
    // The light as half floats, the format a GPU's half-float target is, so a scRGB or linear
    // picture's own halves are written back unchanged: every half is a float, and a float made from
    // one rounds back to it (image/exr.h).
    out.light = join(dir, stem + ".exr");
    Vector<u16> halves(frame.light.size());
    for (u32 i = 0; i < frame.light.size(); ++i)
      halves[i] = image::half_from_f32(frame.light[i]);
    image::ExrOptions options;
    options.white_luminance = frame.white_nits;
    const io::Status status =
        image::write_exr(out.light, frame.width, frame.height, 3,
                         std::span<const u16>(halves.data(), halves.size()), options);
    if (status != io::Status::Ok) {
      error = "cannot write " + out.light + ": " + io::status_name(status);
      return false;
    }
    if (frame.codes.bits != 0) {
      out.codes = join(dir, stem + ".codes.json");
      const io::Status codes_status = io::write_file(out.codes, codes_json(frame, stem + ".exr"));
      if (codes_status != io::Status::Ok) {
        error = "cannot write " + out.codes + ": " + io::status_name(codes_status);
        return false;
      }
    }
  }
  return true;
}

bool capture_light(const gfx::Capture& picture, gfx::DisplayEncoding encoding, f32 paper_white_nits,
                   CapturedFrame& frame, std::string* error) {
  const u32 pixels = picture.width * picture.height;
  const auto refuse = [&](const char* why) {
    if (error != nullptr) {
      *error = std::string("capture_light: ") + why + " (" + gfx::format_name(picture.format) +
               " drawn as " + gfx::display_encoding_name(encoding) + ")";
    }
    return false;
  };
  if (pixels == 0 || picture.bytes.size() != pixels * picture.bytes_per_pixel) {
    return refuse("the capture holds no picture");
  }
  const f32 white = paper_white_nits > 0.0f ? paper_white_nits : gfx::k_default_paper_white_nits;
  const bool ten_bit = picture.format == gfx::Format::A2B10G10R10Unorm ||
                       picture.format == gfx::Format::A2R10G10B10Unorm;
  const bool red_low = picture.format == gfx::Format::A2B10G10R10Unorm;
  const bool eight_bit =
      picture.format == gfx::Format::R8G8B8A8Unorm || picture.format == gfx::Format::B8G8R8A8Unorm;
  const bool bgra = picture.format == gfx::Format::B8G8R8A8Unorm;
  const bool half = picture.format == gfx::Format::R16G16B16A16Sfloat;
  const bool sdr = encoding == gfx::DisplayEncoding::Sdr;
  const bool pq = encoding == gfx::DisplayEncoding::Pq;
  const bool scrgb = encoding == gfx::DisplayEncoding::ScRgb;
  if (sdr && !ten_bit && !eight_bit) return refuse("an SDR picture is 8- or 10-bit UNORM");
  if (pq && !ten_bit) return refuse("an HDR10 picture is 10-bit packed UNORM");
  if (!sdr && !pq && !half) return refuse("a scRGB or linear picture is half floats");

  frame.width = picture.width;
  frame.height = picture.height;
  frame.encoding = encoding;
  frame.white_nits = pq || scrgb ? white : 0.0f;
  frame.light.resize(pixels * 3);
  frame.codes = CodeHistogram{};
  frame.codes.domain = sdr ? "sdr" : "pq";
  frame.codes.bits = sdr && eight_bit ? 8u : 10u;
  const u32 levels = 1u << frame.codes.bits;
  frame.codes.counts.resize(3 * levels);
  u32* counts = frame.codes.counts.data();
  const f32 steps = static_cast<f32>(levels - 1);
  const u8* src = picture.bytes.data();
  f32* light = frame.light.data();

  for (u32 i = 0; i < pixels; ++i, light += 3) {
    u32 codes[3] = {};
    if (ten_bit) {
      u32 word = 0;
      std::memcpy(&word, src + static_cast<usize>(i) * 4, sizeof(word));
      const u32 low = word & 1023u;
      const u32 high = (word >> 20) & 1023u;
      codes[0] = red_low ? low : high;
      codes[1] = (word >> 10) & 1023u;
      codes[2] = red_low ? high : low;
    } else if (eight_bit) {
      const u8* p = src + static_cast<usize>(i) * 4;
      codes[0] = bgra ? p[2] : p[0];
      codes[1] = p[1];
      codes[2] = bgra ? p[0] : p[2];
    }
    if (sdr) {
      for (u32 c = 0; c < 3; ++c)
        light[c] = gfx::sdr_decode(static_cast<f32>(codes[c]) / steps);
    } else if (pq) {
      const f32 nits2020[3] = {gfx::pq_decode(static_cast<f32>(codes[0]) / steps),
                               gfx::pq_decode(static_cast<f32>(codes[1]) / steps),
                               gfx::pq_decode(static_cast<f32>(codes[2]) / steps)};
      f32 nits709[3];
      gfx::bt2020_to_bt709(nits2020, nits709);
      for (u32 c = 0; c < 3; ++c)
        light[c] = nits709[c] / white;
    } else {
      u16 h[4];
      std::memcpy(h, src + static_cast<usize>(i) * 8, sizeof(h));
      // scRGB's 1.0 is 80 nits; the linear radiance's 1.0 is already the picture's white.
      f32 nits709[3];
      for (u32 c = 0; c < 3; ++c) {
        const f32 v = image::f32_from_half(h[c]);
        nits709[c] = scrgb ? v * gfx::k_scrgb_unit_nits : v * white;
        light[c] = scrgb ? nits709[c] / white : v;
      }
      // The codes an HDR10 signal of this light would carry (CodeHistogram says why).
      for (u32 r = 0; r < 3; ++r) {
        const f32 n = gfx::k_bt709_to_bt2020[r][0] * nits709[0] +
                      gfx::k_bt709_to_bt2020[r][1] * nits709[1] +
                      gfx::k_bt709_to_bt2020[r][2] * nits709[2];
        codes[r] = static_cast<u32>(gfx::pq_encode(n) * steps + 0.5f);
      }
    }
    for (u32 c = 0; c < 3; ++c) {
      ++counts[c * levels + (codes[c] < levels ? codes[c] : levels - 1u)];
    }
  }
  return true;
}

}  // namespace engine::renderer
