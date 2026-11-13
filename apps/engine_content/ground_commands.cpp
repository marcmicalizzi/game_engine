// engine-content ground sample: what the drawn ground is at a point (G201 stage 2; docs/subsystems/
// apps.md, "engine-content ground sample"; renderer.md, "A tree seated by its footprint").
//
//   ground sample <scene.json> (--point <x_m,z_m[,radius_m]> ... | --points <file.jsonl>)
//                 [--skirt <m>] [--uncut] [--placements] [--ddc <root>]
//
// Reads the scene as a load does, through its ground plan, so the ground asked is the one a
// `ground: true` instance stands on — the terrain with the scene's edits and every edit its
// placement entries plan (a route's treads, a bridge's approaches, a channel's cut) — and answers
// each point with one JSON line from `renderer::SceneGround`: the height, whether it is a hole, the
// exclusion kinds that cover it and the gradient, and with a radius the disc's lowest, highest,
// drop, fitted plane and, given `--skirt`, the seat: the highest an origin there may stand so that
// a skirt that deep is the margin under the lowest ground the rim stands over. A head line first.
// `--uncut` asks the ground before every edit; `--placements` reads the placements too, so their
// shapes join the exclusion. A `--points` file holds one object a line: {"x":..,"z":..,"r":..}.
// Exit 0, 1 for a scene or a points file that does not read, 2 for usage.
#include "content_commands.h"

#include <core/containers/vector.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/ground_sample.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_failed = 1;
constexpr int k_exit_usage = 2;

const char* k_usage =
    "usage: engine-content ground sample <scene.json> (--point <x,z[,radius]> ... | --points "
    "<file.jsonl>)\n"
    "                                    [--skirt <m>] [--uncut] [--placements] [--ddc <root>]\n"
    "  the drawn ground at each point (metres): height, hole, exclusion kinds, gradient; with a\n"
    "  radius the disc's min, max, drop, fitted plane and, with --skirt, the seat; a head first\n";

int usage(const char* why) {
  std::fprintf(stderr, "engine-content ground: %s\n%s", why, k_usage);
  return k_exit_usage;
}

bool parse_number(std::string_view text, f64& out) {
  const std::string s(text);
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end != s.c_str() && *end == '\0';
}

// "x,z" or "x,z,radius".
bool parse_point(std::string_view text, renderer::GroundQuestion& out) {
  f64 values[3] = {0.0, 0.0, 0.0};
  u32 n = 0;
  while (n < 3) {
    const usize comma = text.find(',');
    if (!parse_number(text.substr(0, comma), values[n])) return false;
    ++n;
    if (comma == std::string_view::npos) break;
    text = text.substr(comma + 1);
  }
  if (n < 2) return false;
  out.x_m = values[0];
  out.z_m = values[1];
  out.radius_m = n == 3 ? values[2] : 0.0;
  return out.radius_m >= 0.0;
}

bool member(const JsonValue& line, const char* name, f64& out) {
  const JsonValue* v = line.find(name);
  return v != nullptr && v->get_f64(out);
}

void print(const JsonValue& value) {
  JsonWriteOptions options;
  options.pretty = false;
  const std::string text = write_json(value, options);
  std::fprintf(stdout, "%s\n", text.c_str());
}

}  // namespace

int ground_command(int argc, char** argv) {
  // argv[1] is "ground", argv[2] the subcommand.
  if (argc < 4 || std::string_view(argv[2]) != "sample")
    return usage("expected: ground sample <scene>");
  const std::string scene = argv[3];
  Vector<renderer::GroundQuestion> questions;
  std::string points_file;
  std::string ddc;
  f64 skirt = 0.0;
  bool uncut = false;
  bool placements = false;
  for (int i = 4; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--point" && has_value) {
      renderer::GroundQuestion q;
      if (!parse_point(argv[++i], q)) return usage("--point wants x,z or x,z,radius in metres");
      questions.push_back(q);
    } else if (arg == "--points" && has_value) {
      points_file = argv[++i];
    } else if (arg == "--skirt" && has_value) {
      if (!parse_number(argv[++i], skirt) || !(skirt > 0.0)) return usage("--skirt wants metres");
    } else if (arg == "--ddc" && has_value) {
      ddc = argv[++i];
    } else if (arg == "--uncut") {
      uncut = true;
    } else if (arg == "--placements") {
      placements = true;
    } else {
      return usage("unknown argument");
    }
  }
  if (!points_file.empty()) {
    std::string text;
    if (io::read_file(points_file, text) != io::Status::Ok) {
      std::fprintf(stderr, "engine-content ground: cannot read %s\n", points_file.c_str());
      return k_exit_failed;
    }
    usize start = 0;
    u32 line_no = 0;
    while (start < text.size()) {
      usize end = text.find('\n', start);
      if (end == std::string::npos) end = text.size();
      const std::string_view line(text.data() + start, end - start);
      start = end + 1;
      ++line_no;
      if (line.find_first_not_of(" \t\r") == std::string_view::npos) continue;
      JsonValue value;
      renderer::GroundQuestion q;
      if (!parse_json(line, value).ok || !member(value, "x", q.x_m) || !member(value, "z", q.z_m)) {
        std::fprintf(stderr, "engine-content ground: %s line %u is not {\"x\":..,\"z\":..}\n",
                     points_file.c_str(), line_no);
        return k_exit_failed;
      }
      (void)member(value, "r", q.radius_m);
      questions.push_back(q);
    }
  }
  if (questions.empty()) return usage("no point to ask: --point or --points");
  renderer::SceneGround ground;
  std::string error;
  if (!ground.open(scene, ddc, uncut, placements, error)) {
    std::fprintf(stderr, "engine-content ground: %s\n", error.c_str());
    return k_exit_failed;
  }
  print(ground.head());
  for (const renderer::GroundQuestion& q : questions)
    print(ground.sample(q, skirt));
  return k_exit_ok;
}

}  // namespace engine::content
