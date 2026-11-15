// engine-content alpha-merge: a base colour and a separate opacity map made one RGBA image
// (docs/subsystems/apps.md, "engine-content"; docs/experiments/static-asset-world.md, "Bought
// foliage"; roadmap T139).
//
//   alpha-merge <color> <alpha> <out.png>   writes the color image's RGB with the alpha image's
//                                           first channel as A, and prints one JSON line
//
// **Why it exists.** glTF 2.0 has no opacity slot: an alpha-tested material's mask is the base
// colour texture's alpha channel. Bought foliage is very often shipped the other way — Poly Haven's
// plants, for one, come with a JPEG base colour (no alpha channel at all) and the opacity as a
// separate `*_alpha` map that the glTF does not reference, under a material that already says
// `MASK` or `BLEND`. Drawn as shipped, every leaf card is a solid quad. A consumer fixes that in
// their DCC; `tools/fetch-foliage.ps1` fixes it with this command, so the engine's own decoder and
// encoder make the image and the result is the same bytes on every machine (stb's JPEG decoder and
// the engine's PNG writer, no system codec).
//
// The two images must have the same size: resampling one to the other would be a decision this
// command has no business taking. The alpha map's first channel is taken (a grey map's R = G = B).
#include "content_commands.h"

#include <core/base/types.h>
#include <foundation/image/decode.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

int usage(const char* why) {
  std::fprintf(stderr,
               "engine-content alpha-merge: %s\n"
               "usage: engine-content alpha-merge <color> <alpha> <out.png>\n"
               "  the color image's RGB with the alpha image's first channel as A, one PNG\n",
               why);
  return k_exit_usage;
}

}  // namespace

int alpha_merge_command(int argc, char** argv) {
  std::string paths[3];
  int count = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (!a.empty() && a[0] == '-') return usage("unknown option");
    if (count == 3) return usage("alpha-merge takes three paths");
    paths[count++] = std::string(a);
  }
  if (count != 3) return usage("alpha-merge takes three paths");
  image::Image color;
  image::Image alpha;
  std::string error;
  if (image::read_image(paths[0], color, 4, &error) != io::Status::Ok) {
    std::fprintf(stderr, "engine-content alpha-merge: cannot read %s: %s\n", paths[0].c_str(),
                 error.c_str());
    return k_exit_error;
  }
  if (image::read_image(paths[1], alpha, 1, &error) != io::Status::Ok) {
    std::fprintf(stderr, "engine-content alpha-merge: cannot read %s: %s\n", paths[1].c_str(),
                 error.c_str());
    return k_exit_error;
  }
  if (color.width != alpha.width || color.height != alpha.height) {
    std::fprintf(stderr, "engine-content alpha-merge: %s is %ux%u but %s is %ux%u\n",
                 paths[0].c_str(), color.width, color.height, paths[1].c_str(), alpha.width,
                 alpha.height);
    return k_exit_error;
  }
  // The decoder's Vector is indexed by u32, and four bytes a texel of a 32k x 32k image would
  // already overflow it, so the size was bounded before this loop by the decode itself.
  const u32 texels = color.width * color.height;
  u32 kept = 0;  // texels at or over the glTF default cutoff 0.5, the share a card keeps
  for (u32 t = 0; t < texels; ++t) {
    const u8 a = alpha.pixels[t];
    color.pixels[t * 4 + 3] = a;
    if (a >= 128) ++kept;
  }
  if (image::write_png(paths[2], color.width, color.height, 4,
                       std::span<const u8>(color.pixels.data(), color.pixels.size())) !=
      io::Status::Ok) {
    std::fprintf(stderr, "engine-content alpha-merge: cannot write %s\n", paths[2].c_str());
    return k_exit_error;
  }
  std::printf("{\"width\":%u,\"height\":%u,\"coverage\":%.4f}\n", color.width, color.height,
              texels == 0 ? 0.0 : static_cast<f64>(kept) / static_cast<f64>(texels));
  return k_exit_ok;
}

}  // namespace engine::content
