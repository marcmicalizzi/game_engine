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
  return true;
}

}  // namespace engine::renderer
