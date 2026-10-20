// End to end: the HUD on an HDR target, and a HUD of the game's own images and type (roadmap P49;
// docs/subsystems/apps.md "The HUD, provisional"; renderer.md "The overlay"; hotload ABI 6).
//
// The fixture's content directory — beside this test's scene — holds `hud/panel.png`, a 64 x 64
// stand-in this test writes (a 16-pixel opaque white border round a blue at half alpha), and
// `hud/font.ttf`, Roboto from the ImGui checkout the build fetched. With them the fixture draws the
// panel nine-sliced behind its `[E] Pick up` prompt and a `FIND THE WATER` title in that font with
// a shadow and letter spacing. The same replay is captured in `sdr8`, `hdr10` and `scrgb`, twice
// each: with that panel, and with a panel of the same size that is wholly transparent, so the
// second is the picture under the first. In every encoding:
//
// - the HUD is there: the overlay laid out, and the panel, the prompt and the title have bounds;
// - **UI white is paper white**: the panel's border and the title's glyphs are light 1.0 of paper
//   white — code 255 on SDR, PQ's code for 200 nits (which the capture's code histogram counts) and
//   scRGB's 2.5 — and nothing the HUD draws is brighter;
// - **the panel's alpha composites in the space the target says**: on SDR a blend of the codes, as
//   the overlay always blended; on HDR a blend of the light, the picture under it times one minus
//   the alpha plus the blue's decoded light times the alpha;
// - the prompt's outline edge reaches black.
//
// And with `ENGINE_VIEW_BASELINE` naming another build's engine-view (main's, with its walk_poc at
// `game/walk_poc.dll` beside it), the SDR capture of the same replay with no HUD art — the prompt
// and the top-left stack — is that build's to the byte. Every run is offscreen (view_process.h).

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <domain/gfx/display.h>
#include <foundation/image/decode.h>
#include <foundation/image/exr.h>
#include <foundation/image/png.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

namespace engine::view_test {
bool write_box_glb(const std::string& path);  // game_props_tests.cpp
}

#if ENGINE_VIEW_GAME && defined(WALK_POC_PATH)

namespace {

constexpr u32 k_width = 800;
constexpr u32 k_height = 450;
constexpr f64 k_paper_white = 200.0;
constexpr f64 k_alpha = 128.0 / 255.0;
constexpr u8 k_blue[3] = {64, 128, 255};

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run run_exe(const std::string& exe, const std::vector<std::string>& args) {
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

bool summary_of(const Run& run, JsonValue& out) {
  usize end = run.output.size();
  while (end > 0) {
    usize begin = run.output.find_last_of('\n', end - 1);
    begin = begin == std::string::npos ? 0 : begin + 1;
    const std::string line = run.output.substr(begin, end - begin);
    if (!line.empty() && line[0] == '{' && parse_json(line, out).ok && out.is_object()) return true;
    if (begin == 0) break;
    end = begin - 1;
  }
  return false;
}

const JsonValue* at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = &v;
  for (const char* key : path)
    p = p != nullptr && p->is_object() ? p->find(key) : nullptr;
  return p;
}

void copy(const std::string& from, const std::string& to) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(to).parent_path(), ec);
  std::filesystem::copy_file(std::filesystem::path(from), std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << from << ": " << ec.message());
}

// The stand-in panel: a 16-pixel border of opaque white round the blue at half alpha — or, with
// `clear`, nothing at all, the same size.
void write_panel(const std::string& path, bool clear) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::vector<u8> pixels(64 * 64 * 4, 0);
  for (u32 y = 0; y < 64; ++y) {
    for (u32 x = 0; x < 64; ++x) {
      u8* p = &pixels[(y * 64 + x) * 4];
      if (clear) continue;
      const bool border = x < 16 || y < 16 || x >= 48 || y >= 48;
      p[0] = border ? 255 : k_blue[0];
      p[1] = border ? 255 : k_blue[1];
      p[2] = border ? 255 : k_blue[2];
      p[3] = border ? 255 : static_cast<u8>(128);
    }
  }
  REQUIRE(image::write_png(path, 64, 64, 4, std::span<const u8>(pixels.data(), pixels.size())) ==
          io::Status::Ok);
}

// One scene directory: the waves, the box as the props `can` and `marker`, the fixture's art, a
// state file standing the can a metre in front of the walker, and a 40-tick walk towards it.
struct SceneDir {
  std::string scene;
  std::string state;
  std::string session;
};

SceneDir scene_dir(const test::TempDir& tmp, const std::string& name, int art) {
  const std::string dir = tmp.file(name);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  SceneDir out;
  REQUIRE(view_test::write_box_glb(dir + "/box.glb"));
  out.scene = dir + "/props.json";
  REQUIRE(io::write_file(out.scene, R"({"format":"engine.scene.v1","name":"props",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30},
      "props":[{"name":"can","mesh":"box.glb","fit":{"height":1}},
               {"name":"marker","mesh":"box.glb","fit":{"height":0.5}}]})") == io::Status::Ok);
  out.state = dir + "/state.json";
  REQUIRE(
      io::write_file(out.state, R"({"format":"engine.game.state","version":1,"module":"walk_poc",
      "type":"engine.walk_poc.Survivor","state":{"can":1024,"can_placed":true},
      "props":[{"id":1024,"prop":"can","position":[0,0.3,9],"yaw":0,"scale":1}]})") ==
      io::Status::Ok);
  // 0: no art; 1: the panel and the font; 2: a clear panel and the font.
  if (art != 0) {
    write_panel(dir + "/hud/panel.png", art == 2);
#if defined(ENGINE_VIEW_TEST_FONT)
    if (std::filesystem::exists(ENGINE_VIEW_TEST_FONT))
      copy(ENGINE_VIEW_TEST_FONT, dir + "/hud/font.ttf");
#endif
  }
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  keys.record(input::RawEvent{SimTick{10}, input::Source::Key, 26, 1.0f, 0});  // W
  view::SessionHeader header;
  header.ticks = 40;
  header.scene = out.scene;
  header.start.position = WorldPos{0.0, 3.0, 10.0};
  header.start.yaw = 0.0f;
  header.start.pitch = -0.15f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  out.session = dir + "/walk.jsonl";
  REQUIRE(keys.save(out.session) == io::Status::Ok);
  return out;
}

// A capture as light relative to the picture's white, three channels a pixel: an SDR PNG's codes
// through the 1/2.2 power, an HDR EXR's halves as they are (the capture wrote them so). `codes`
// keeps the SDR picture's own codes.
struct Picture {
  u32 width = 0;
  u32 height = 0;
  std::vector<f64> light;
  std::vector<u8> codes;
  f64 at(u32 x, u32 y, u32 c) const { return light[(usize{y} * width + x) * 3 + c]; }
  u8 code(u32 x, u32 y, u32 c) const { return codes[(usize{y} * width + x) * 4 + c]; }
};

Picture read_picture(const std::string& path, bool hdr) {
  Picture out;
  if (!hdr) {
    image::Image png;
    REQUIRE(image::read_image(path, png) == io::Status::Ok);
    out.width = png.width;
    out.height = png.height;
    out.codes.assign(png.pixels.begin(), png.pixels.end());
    out.light.resize(usize{png.width} * png.height * 3);
    for (usize i = 0; i < usize{png.width} * png.height; ++i) {
      for (u32 c = 0; c < 3; ++c)
        out.light[i * 3 + c] =
            std::pow(static_cast<f64>(png.pixels[static_cast<u32>(i * 4 + c)]) / 255.0, 2.2);
    }
    return out;
  }
  image::ExrImage exr;
  std::string error;
  REQUIRE_MESSAGE(image::read_exr(path, exr, &error), error);
  REQUIRE(exr.channels >= 3);
  out.width = exr.width;
  out.height = exr.height;
  out.light.resize(usize{exr.width} * exr.height * 3);
  for (usize i = 0; i < usize{exr.width} * exr.height; ++i) {
    for (u32 c = 0; c < 3; ++c)
      out.light[i * 3 + c] = static_cast<f64>(
          image::f32_from_half(exr.halves[static_cast<u32>(i * exr.channels + c)]));
  }
  return out;
}

struct Box {
  i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

Box bounds_of(const JsonValue& report, u32 id, std::string* kind = nullptr) {
  const JsonValue* elements = at(report, {"game", "hud_elements"});
  REQUIRE(elements != nullptr);
  for (usize i = 0; i < elements->size(); ++i) {
    const JsonValue& e = (*elements)[i];
    u64 eid = 0;
    if (e.find("id") == nullptr || !e.find("id")->get_u64(eid) || eid != id) continue;
    if (kind != nullptr) *kind = e.find("kind")->as_string();
    const JsonValue& b = *e.find("bounds_px");
    f64 v[4] = {0, 0, 0, 0};
    for (usize k = 0; k < 4; ++k)
      REQUIRE(b[k].get_f64(v[k]));
    return Box{static_cast<i32>(v[0]), static_cast<i32>(v[1]), static_cast<i32>(v[2]),
               static_cast<i32>(v[3])};
  }
  return Box{};
}

struct Shot {
  Picture picture;
  JsonValue report;
  JsonValue summary;
  std::string codes;  // the HDR capture's histogram file
};

// One capture of `dir`'s replay in `format` ("sdr8", "hdr10", "scrgb") by `exe` with `module`.
bool shoot(const std::string& exe, const std::string& module, const test::TempDir& tmp,
           const SceneDir& dir, const std::string& format, const std::string& stem, Shot& out) {
  const bool hdr = format != "sdr8";
  const std::string capture = tmp.file(stem + (hdr ? ".exr" : ".png"));
  const std::string report = tmp.file(stem + ".report.json");
  // A copy of the state file for each run: the run saves over the one it was given.
  const std::string state = tmp.file(stem + ".state.json");
  copy(dir.state, state);
  std::vector<std::string> args = {
      "--replay-input", dir.session, "--scene",   dir.scene,      "--offscreen",
      "--width",        "800",       "--height",  "450",          "--ddc",
      tmp.file("ddc"),  "--game",    module,      "--game-state", state,
      "--report",       report,      "--capture", capture};
  if (hdr) {
    args.insert(args.end(),
                {"--present-format", format, "--paper-white-nits", "200", "--peak-nits", "1000"});
  }
  const Run run = run_exe(exe, args);
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return false;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  std::string text;
  REQUIRE(io::read_file(report, text) == io::Status::Ok);
  REQUIRE(parse_json(text, out.report).ok);
  REQUIRE(summary_of(run, out.summary));
  out.picture = read_picture(capture, hdr);
  REQUIRE(out.picture.width == k_width);
  REQUIRE(out.picture.height == k_height);
  if (hdr) out.codes = tmp.file(stem + ".codes.json");
  return true;
}

}  // namespace

TEST_CASE(
    "engine-view: the HUD's panel image and the game's font in sdr8, hdr10 and scrgb: present, "
    "white at paper white, alpha in the target's space") {
  const test::TempDir tmp("engine_view_hud_hdr");
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  const std::string module =
      tmp.file(std::string("walk_poc") + std::filesystem::path(WALK_POC_PATH).extension().string());
  copy(WALK_POC_PATH, module);  // beside the scenes' directories, not in them: the art is found
                                // beside the scene
  const SceneDir art = scene_dir(tmp, "art", 1);
  const SceneDir clear = scene_dir(tmp, "clear", 2);
#if defined(ENGINE_VIEW_TEST_FONT)
  const bool font = std::filesystem::exists(ENGINE_VIEW_TEST_FONT);
#else
  const bool font = false;
#endif
  if (!font) MESSAGE("no font to hand the fixture here; the title line is not checked");

  for (const char* format : {"sdr8", "hdr10", "scrgb"}) {
    INFO(format);
    const bool hdr = std::strcmp(format, "sdr8") != 0;
    Shot a;
    Shot b;
    if (!shoot(exe, module, tmp, art, format, std::string(format) + "-art", a)) return;
    REQUIRE(shoot(exe, module, tmp, clear, format, std::string(format) + "-clear", b));

    // The HUD is there, the panel an image, and everything the fixture drew has bounds.
    f64 layouts = 0.0;
    REQUIRE(at(a.summary, {"interactive", "game", "overlay", "layouts"}) != nullptr);
    CHECK(at(a.summary, {"interactive", "game", "overlay", "layouts"})->get_f64(layouts));
    CHECK(layouts > 0.0);
    std::string kind;
    const Box panel = bounds_of(a.report, 2, &kind);
    const Box prompt = bounds_of(a.report, 1);
    const Box title = bounds_of(a.report, 3);
    MESSAGE(std::string(format) << ": panel [" << panel.x0 << ", " << panel.y0 << ", " << panel.x1
                                << ", " << panel.y1 << "], prompt [" << prompt.x0 << ", "
                                << prompt.y0 << ", " << prompt.x1 << ", " << prompt.y1
                                << "], title [" << title.x0 << ", " << title.y0 << ", " << title.x1
                                << ", " << title.y1 << "]");
    CHECK(kind == "image");
    REQUIRE_FALSE(panel.empty());
    REQUIRE_FALSE(prompt.empty());
    CHECK(panel.x0 < prompt.x0);
    CHECK(panel.x1 > prompt.x1);
    if (font) REQUIRE_FALSE(title.empty());

    // **UI white is paper white**: the panel's left border, inset a pixel from its edges.
    const i32 border = static_cast<i32>(std::lround(16.0 * k_height / 1080.0));  // 7 px at 450
    f64 white_low = 1.0e9;
    f64 white_high = 0.0;
    for (i32 y = panel.y0 + border; y < panel.y1 - border; ++y) {
      for (i32 x = panel.x0 + 1; x < panel.x0 + border - 1; ++x) {
        for (u32 c = 0; c < 3; ++c) {
          const f64 v = a.picture.at(static_cast<u32>(x), static_cast<u32>(y), c);
          white_low = std::min(white_low, v);
          white_high = std::max(white_high, v);
        }
      }
    }
    // A PQ code at 200 nits is 0.8 % of the light, and the dither moves a channel a code.
    const f64 tolerance = std::strcmp(format, "hdr10") == 0 ? 0.02 : 0.005;
    MESSAGE(std::string(format) << ": the panel's white border is " << white_low << ".."
                                << white_high << " of paper white");
    CHECK(white_low >= 1.0 - tolerance);
    CHECK(white_high <= 1.0 + tolerance);
    // Nothing the HUD draws is past paper white: the panel, the prompt, the title.
    f64 hud_high = 0.0;
    for (const Box& box : {panel, prompt, title}) {
      for (i32 y = std::max(box.y0, 0); y < std::min(box.y1, static_cast<i32>(k_height)); ++y) {
        for (i32 x = std::max(box.x0, 0); x < std::min(box.x1, static_cast<i32>(k_width)); ++x) {
          for (u32 c = 0; c < 3; ++c)
            hud_high =
                std::max(hud_high, a.picture.at(static_cast<u32>(x), static_cast<u32>(y), c));
        }
      }
    }
    MESSAGE(std::string(format) << ": the brightest of the HUD's boxes is " << hud_high);
    CHECK(hud_high <= 1.0 + tolerance);
    // The title's glyphs, white, reach paper white.
    if (font) {
      u32 white_glyph = 0;
      for (i32 y = title.y0; y < title.y1; ++y) {
        for (i32 x = title.x0; x < title.x1; ++x) {
          if (a.picture.at(static_cast<u32>(x), static_cast<u32>(y), 1) >= 1.0 - tolerance)
            ++white_glyph;
        }
      }
      MESSAGE(std::string(format) << ": " << white_glyph
                                  << " of the title's pixels at paper white");
      CHECK(white_glyph >= 50);
    }
    // The prompt's black outline reaches black at its edge.
    f64 prompt_low = 1.0e9;
    for (i32 y = prompt.y0; y < prompt.y1; ++y) {
      for (i32 x = prompt.x0; x < prompt.x1; ++x)
        prompt_low =
            std::min(prompt_low, a.picture.at(static_cast<u32>(x), static_cast<u32>(y), 1));
    }
    CHECK(prompt_low <= 0.02);

    // **The panel's alpha**: the strip of its middle left of the prompt, against the same strip
    // under a clear panel. On SDR in codes, on HDR in light.
    f64 blend_worst = 0.0;
    u32 samples = 0;
    for (i32 y = panel.y0 + border + 2; y < panel.y1 - border - 2; ++y) {
      for (i32 x = panel.x0 + border + 2; x < prompt.x0 - 2; ++x) {
        for (u32 c = 0; c < 3; ++c) {
          const u32 ux = static_cast<u32>(x);
          const u32 uy = static_cast<u32>(y);
          if (!hdr) {
            const f64 expect =
                static_cast<f64>(b.picture.code(ux, uy, c)) * (1.0 - k_alpha) + k_blue[c] * k_alpha;
            blend_worst = std::max(blend_worst,
                                   std::fabs(static_cast<f64>(a.picture.code(ux, uy, c)) - expect));
          } else {
            const f64 blue = std::pow(static_cast<f64>(k_blue[c]) / 255.0, 2.2);
            const f64 expect = b.picture.at(ux, uy, c) * (1.0 - k_alpha) + blue * k_alpha;
            blend_worst = std::max(
                blend_worst, std::fabs(a.picture.at(ux, uy, c) - expect) / std::max(expect, 0.05));
          }
          ++samples;
        }
      }
    }
    MESSAGE(std::string(format) << ": the panel's half alpha over " << samples
                                << " channels, within " << blend_worst
                                << std::string(hdr ? " of the light" : " codes"));
    CHECK(samples >= 30);
    CHECK(blend_worst <= (hdr ? 0.03 : 1.0));

    // **The codes**: on hdr10 the capture's histogram counts PQ's code for paper white.
    if (std::strcmp(format, "hdr10") == 0) {
      std::string text;
      REQUIRE(io::read_file(a.codes, text) == io::Status::Ok);
      JsonValue codes;
      REQUIRE(parse_json(text, codes).ok);
      const JsonValue* counts = codes.find("counts");
      REQUIRE(counts != nullptr);
      const u32 white_code = static_cast<u32>(
          std::lround(static_cast<f64>(gfx::pq_encode(static_cast<f32>(k_paper_white))) * 1023.0));
      u64 near_white = 0;
      for (u32 k = white_code - 1; k <= white_code + 1; ++k) {
        u64 n = 0;
        if ((*counts)[usize{1}][usize{k}].get_u64(n)) near_white += n;
      }
      MESSAGE("hdr10: paper white is PQ code " << white_code << "; " << near_white
                                               << " green channels within a code of it");
      CHECK(near_white >= 100);
    }
    if (blend_worst > (hdr ? 0.03 : 1.0) || white_low < 1.0 - tolerance) tmp.keep();
  }

  // **SDR to the byte against another build**, when one is named: the replay with no HUD art.
  const std::string baseline = test::detail::environment("ENGINE_VIEW_BASELINE");
  if (!baseline.empty()) {
    const SceneDir plain = scene_dir(tmp, "plain", 0);
    const std::string base_module =
        (std::filesystem::path(baseline).parent_path() / "game" /
         (std::string("walk_poc") + std::filesystem::path(WALK_POC_PATH).extension().string()))
            .string();
    Shot ours;
    Shot theirs;
    REQUIRE(shoot(exe, module, tmp, plain, "sdr8", "plain-ours", ours));
    REQUIRE(shoot(baseline, base_module, tmp, plain, "sdr8", "plain-theirs", theirs));
    u64 differ = 0;
    for (usize i = 0; i < ours.picture.codes.size(); ++i) {
      if (ours.picture.codes[i] != theirs.picture.codes[i]) ++differ;
    }
    MESSAGE("sdr8 against " << baseline << ": " << differ << " bytes differ");
    CHECK(differ == 0);
    CHECK(bounds_of(ours.report, 2).empty());  // no art: no panel
  }
}

#endif
