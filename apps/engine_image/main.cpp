// engine-image: the perceptual comparison of two rendered frames on the command line
// (docs/plan/04-renderer.md §4.8). `compare` is the gate a renderer change is measured by --
// FLIP first, PSNR and SSIM beside it -- and `--flip out.png` is the picture that says where
// the difference is. The numbers come from foundation/image's metrics; this app is the
// argument parsing, the file reading, and one line of output.
//
// `compare` exits 0 whenever it produced numbers, however large they are: a difference is a
// measurement, not a failure, and it is the caller that decides what threshold means what.
// Anything that stopped it from measuring -- a bad argument, a file that is not there or is
// not an image, two images of different sizes -- exits 2. There is deliberately no exit 1.
#include <core/time/time.h>
#include <foundation/image/decode.h>
#include <foundation/image/metrics.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

using namespace engine;

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_usage = 2;

const char* k_usage =
    "usage: engine-image <command> [options]\n"
    "\n"
    "  compare <a.png> <b.png>                 perceptual difference between two images\n"
    "      --flip <out.png>      write the per-pixel FLIP error as a heat map\n"
    "      --ppd <n>             pixels per degree of visual angle (default 67.02, a 4K\n"
    "                            screen 0.7 m wide seen from 0.7 m)\n"
    "      --weights center|none per-pixel pooling weights (default none); center is a\n"
    "                            Gaussian attention region, so a periphery regression\n"
    "                            weighs less than one in the middle of the frame\n"
    "      --json                one JSON object on stdout instead of a readable report\n"
    "  info <file.png>                         size and channel count of an image\n"
    "      --json                as one JSON object\n"
    "\n"
    "PNG, JPEG, TGA, and BMP are read. Exit codes: 0 measured, 2 could not measure.\n"
    "\n"
    "examples:\n"
    "  engine-image compare before.png after.png\n"
    "  engine-image compare before.png after.png --flip diff.png --weights center --json\n"
    "  engine-image info capture.png\n";

int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-image: %s\n", message);
  std::fputs(k_usage, stderr);
  return k_exit_usage;
}

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-image: %s\n", message.c_str());
  return k_exit_usage;
}

// JSON strings carry Windows paths, so the backslashes have to survive the trip.
std::string json_string(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escape[8] = {};
          std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned int>(c) & 0xffu);
          out += escape;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

// PSNR of two identical images is infinite, and JSON has no infinity: the field is null, which
// a reader can tell apart from a very large number that merely means "almost identical".
std::string json_number(f32 value) {
  if (!std::isfinite(value)) return "null";
  char text[32] = {};
  std::snprintf(text, sizeof(text), "%.6g", static_cast<f64>(value));
  return text;
}

bool next_value(int argc, char** argv, int& i, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "engine-image: %s needs a value\n", argv[i]);
    return false;
  }
  out = argv[++i];
  return true;
}

bool parse_f32(const std::string& text, f32& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(value)) return false;
  out = value;
  return true;
}

// Reads and decodes an image to RGBA8, saying which file and why when it cannot.
bool load(const std::string& path, image::Image& out) {
  std::string error;
  const io::Status status = image::read_image(path, out, 4, &error);
  if (status != io::Status::Ok) {
    std::fprintf(stderr, "engine-image: cannot read %s: %s%s%s\n", path.c_str(),
                 io::status_name(status), error.empty() ? "" : ": ", error.c_str());
    return false;
  }
  return true;
}

struct CompareOptions {
  std::string a;
  std::string b;
  std::string flip_path;
  f32 pixels_per_degree = image::k_default_pixels_per_degree;
  bool center_weights = false;
  bool json = false;
};

int compare_command(int argc, char** argv) {
  CompareOptions options;
  u32 positional = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    std::string value;
    if (a == "--flip") {
      if (!next_value(argc, argv, i, options.flip_path)) return k_exit_usage;
    } else if (a == "--ppd") {
      if (!next_value(argc, argv, i, value)) return k_exit_usage;
      if (!parse_f32(value, options.pixels_per_degree) || options.pixels_per_degree <= 0.0f)
        return usage("--ppd needs a positive number of pixels per degree");
    } else if (a == "--weights") {
      if (!next_value(argc, argv, i, value)) return k_exit_usage;
      if (value == "center") {
        options.center_weights = true;
      } else if (value == "none") {
        options.center_weights = false;
      } else {
        return usage("--weights takes center or none");
      }
    } else if (a == "--json") {
      options.json = true;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (positional == 0) {
      options.a = argv[i];
      ++positional;
    } else if (positional == 1) {
      options.b = argv[i];
      ++positional;
    } else {
      return usage("compare takes two images");
    }
  }
  if (positional != 2) return usage("compare needs two image files");

  image::Image a;
  image::Image b;
  if (!load(options.a, a) || !load(options.b, b)) return k_exit_usage;
  if (a.width != b.width || a.height != b.height) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "the images are %ux%u and %ux%u; a comparison needs the same size", a.width,
                  a.height, b.width, b.height);
    return failed(message);
  }

  image::FloatImage weights;
  image::MetricsOptions metric_options;
  metric_options.pixels_per_degree = options.pixels_per_degree;
  if (options.center_weights) {
    image::center_weight_map(a.width, a.height, image::CenterWeightOptions{}, weights);
    metric_options.weights = &weights;
  }

  image::ImageMetrics metrics;
  image::FloatImage error_map;
  std::string message;
  const i64 started = time::monotonic_ns();
  if (!image::compare_images(a, b, metric_options, metrics, &error_map, &message))
    return failed(message);
  const f64 elapsed_ms = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;

  bool wrote_flip = false;
  if (!options.flip_path.empty()) {
    image::Image heat;
    if (!image::flip_heat_map(error_map, heat)) return failed("cannot build the FLIP heat map");
    const io::Status status =
        image::write_png(options.flip_path, heat.width, heat.height, heat.channels,
                         std::span<const u8>(heat.pixels.data(), heat.pixels.size()));
    if (status != io::Status::Ok) {
      return failed("cannot write " + options.flip_path + ": " + io::status_name(status));
    }
    wrote_flip = true;
  }

  if (options.json) {
    std::printf(
        "{\"a\":%s,\"b\":%s,\"width\":%u,\"height\":%u,\"psnr\":%s,\"ssim\":%s,"
        "\"flip_mean\":%s,\"flip_p95\":%s,\"flip_max\":%s,\"flip_weighted_mean\":%s,"
        "\"ppd\":%s,\"weights\":\"%s\",\"flip_image\":%s,\"ms\":%.3f}\n",
        json_string(options.a).c_str(), json_string(options.b).c_str(), a.width, a.height,
        json_number(metrics.psnr).c_str(), json_number(metrics.ssim).c_str(),
        json_number(metrics.flip_mean).c_str(), json_number(metrics.flip_percentile).c_str(),
        json_number(metrics.flip_max).c_str(), json_number(metrics.flip_weighted_mean).c_str(),
        json_number(options.pixels_per_degree).c_str(), options.center_weights ? "center" : "none",
        wrote_flip ? json_string(options.flip_path).c_str() : "null", elapsed_ms);
  } else {
    std::printf("%ux%u, %.1f pixels per degree\n", a.width, a.height,
                static_cast<f64>(options.pixels_per_degree));
    if (std::isfinite(metrics.psnr)) {
      std::printf("psnr  %.3f dB\n", static_cast<f64>(metrics.psnr));
    } else {
      std::printf("psnr  identical\n");
    }
    std::printf("ssim  %.6f\n", static_cast<f64>(metrics.ssim));
    std::printf("flip  mean %.6f  p95 %.6f  max %.6f\n", static_cast<f64>(metrics.flip_mean),
                static_cast<f64>(metrics.flip_percentile), static_cast<f64>(metrics.flip_max));
    if (options.center_weights) {
      std::printf("flip  center-weighted mean %.6f\n",
                  static_cast<f64>(metrics.flip_weighted_mean));
    }
    if (wrote_flip) std::printf("wrote %s\n", options.flip_path.c_str());
    std::printf("took  %.1f ms\n", elapsed_ms);
  }
  return k_exit_ok;
}

int info_command(int argc, char** argv) {
  std::string path;
  bool json = false;
  u32 positional = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--json") {
      json = true;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (positional == 0) {
      path = argv[i];
      ++positional;
    } else {
      return usage("info takes one image");
    }
  }
  if (positional != 1) return usage("info needs an image file");

  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok)
    return failed("cannot read " + path + ": " + io::status_name(status));
  image::ImageInfo info;
  std::string error;
  if (!image::probe_image(
          std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), info,
          &error)) {
    return failed(path + ": " + error);
  }
  if (json) {
    std::printf("{\"path\":%s,\"width\":%u,\"height\":%u,\"channels\":%u,\"bytes\":%llu}\n",
                json_string(path).c_str(), info.width, info.height, info.channels,
                static_cast<unsigned long long>(bytes.size()));
  } else {
    std::printf("%s: %ux%u, %u channel%s, %llu bytes\n", path.c_str(), info.width, info.height,
                info.channels, info.channels == 1 ? "" : "s",
                static_cast<unsigned long long>(bytes.size()));
  }
  return k_exit_ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage(nullptr);
  const std::string_view command = argv[1];
  if (command == "--help" || command == "-h" || command == "help") {
    std::fputs(k_usage, stdout);
    return k_exit_ok;
  }
  if (command == "compare") return compare_command(argc, argv);
  if (command == "info") return info_command(argc, argv);
  return usage("unknown command");
}
