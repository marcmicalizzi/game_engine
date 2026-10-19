// The game HUD's sizing (game_hud.h; docs/subsystems/apps.md "The HUD, provisional"; roadmap P44):
// the font from the picture's height and the display's scale, the panel inside the safe region at
// every size, the atlas baked when the size changes and not per frame. The layout cases run ImGui
// with no device; the captures run engine-view offscreen over a window of the erg with the fixture
// and without, and hold the HUD's pixels to the region it is allowed.

#include "../fly_camera.h"
#include "../game_hud.h"
#include "view_process.h"

#include <core/json/json.h>
#include <core/time/time.h>
#include <foundation/image/decode.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

#if ENGINE_VIEW_GAME

TEST_CASE("game hud: the font follows the picture's height and the display's scale") {
  // 18 px on a 1080-pixel-high picture at 100 % (game_hud.h says why), in proportion elsewhere,
  // whole pixels, never under 10 or over 64.
  CHECK(view::hud_font_px(1080, 1.0f) == 18.0f);
  CHECK(view::hud_font_px(2160, 1.0f) == 36.0f);
  CHECK(view::hud_font_px(2160, 1.5f) == 54.0f);  // a 4K display at 150 %
  CHECK(view::hud_font_px(900, 1.0f) == 15.0f);
  CHECK(view::hud_font_px(720, 1.0f) == 12.0f);
  CHECK(view::hud_font_px(450, 1.0f) == 10.0f);  // 7.5, clamped: the test captures' height
  CHECK(view::hud_font_px(180, 1.0f) == 10.0f);
  CHECK(view::hud_font_px(96, 1.0f) == 10.0f);
  CHECK(view::hud_font_px(4320, 2.0f) == 64.0f);  // clamped
  CHECK(view::hud_font_px(1080, 0.0f) == 18.0f);  // no scale known: 100 %
}

namespace {

view::GameHud fixture_hud() {
  view::GameHud hud;
  std::snprintf(hud.meters[0].name, sizeof(hud.meters[0].name), "thirst");
  hud.meters[0].value = 99.75;
  hud.meters[0].max = 100.0;
  hud.meter_count = 1;
  const char* lines[] = {"day 1   2 s survived   3 m walked", "water: the spring, 25 m away",
                         "E pressed 1 times"};
  for (u32 i = 0; i < 3; ++i) {
    hud.lines[i].set = true;
    std::snprintf(hud.lines[i].text, sizeof(hud.lines[i].text), "%s", lines[i]);
  }
  return hud;
}

// ImGui sizes an auto-resizing window from its first frame's content and shows it from the second,
// so a layout is run twice to see where the panel stands.
void lay_out_twice(view::GameHudOverlay& overlay, const view::GameHud& hud, const JsonValue* result,
                   u32 width, u32 height, f32 scale) {
  REQUIRE(overlay.layout(hud, result, width, height, scale));
  REQUIRE(overlay.layout(hud, result, width, height, scale));
}

bool inside(const view::HudRect& r, f32 x0, f32 y0, f32 x1, f32 y1) {
  return !r.empty() && r.x0 >= x0 && r.y0 >= y0 && r.x1 <= x1 && r.y1 <= y1;
}

}  // namespace

TEST_CASE(
    "game hud: the panel stays inside the safe region at every size, and the result panel too") {
  view::GameHudOverlay overlay;
  REQUIRE(overlay.create());
  const view::GameHud hud = fixture_hud();
  JsonValue result;
  REQUIRE(parse_json(R"({"days":0,"seconds":123.456,"distance":45.6})", result).ok);

  // 320 x 180: the font at its floor, the panel from the 16 x 9 margin, nothing past 304 x 171.
  lay_out_twice(overlay, hud, &result, 320, 180, 1.0f);
  CHECK(overlay.font_px() == 10.0f);
  MESSAGE("320x180: HUD panel " << overlay.panel().x0 << "," << overlay.panel().y0 << " to "
                                << overlay.panel().x1 << "," << overlay.panel().y1
                                << "; result panel " << overlay.result_panel().x0 << ","
                                << overlay.result_panel().y0 << " to " << overlay.result_panel().x1
                                << "," << overlay.result_panel().y1);
  CHECK(inside(overlay.panel(), 16.0f, 9.0f, 304.0f, 171.0f));
  CHECK(overlay.panel().x0 == 16.0f);
  CHECK(overlay.panel().y0 == 9.0f);
  CHECK(inside(overlay.result_panel(), 16.0f, 9.0f, 304.0f, 171.0f));
  // 1600 x 900: 15 px, the panel in the top-left quarter from the 80 x 45 margin.
  lay_out_twice(overlay, hud, nullptr, 1600, 900, 1.0f);
  CHECK(overlay.font_px() == 15.0f);
  CHECK(inside(overlay.panel(), 80.0f, 45.0f, 800.0f, 450.0f));
  CHECK(overlay.result_panel().empty());
  // 3840 x 2160 at 150 %: 54 px, still a corner.
  lay_out_twice(overlay, hud, nullptr, 3840, 2160, 1.5f);
  CHECK(overlay.font_px() == 54.0f);
  CHECK(inside(overlay.panel(), 192.0f, 108.0f, 1920.0f, 1080.0f));
  MESSAGE("3840x2160 at 150 %: HUD panel " << overlay.panel().x1 - overlay.panel().x0 << " x "
                                           << overlay.panel().y1 - overlay.panel().y0 << " px");
  // An empty HUD and no result lays nothing out.
  REQUIRE(overlay.layout(view::GameHud{}, nullptr, 1600, 900, 1.0f));
  CHECK(overlay.panel().empty());
}

TEST_CASE("game hud: an element's font is its 1080p size times the HUD's scale, with a floor") {
  CHECK(view::hud_element_px(28.0f, 1080, 1.0f) == 28.0f);
  CHECK(view::hud_element_px(28.0f, 2160, 1.0f) == 56.0f);
  CHECK(view::hud_element_px(28.0f, 2160, 1.5f) == 84.0f);
  CHECK(view::hud_element_px(28.0f, 450, 1.0f) == 12.0f);  // 11.67, whole pixels
  CHECK(view::hud_element_px(28.0f, 180, 1.0f) == 10.0f);  // the floor holds
  CHECK(view::hud_element_px(200.0f, 2160, 2.0f) == view::k_hud_element_max_px);
}

namespace {

hotload::HudElement element(u32 id, hotload::HudAnchor anchor, hotload::HudAlign align,
                            const char* text) {
  hotload::HudElement e;
  e.id = id;
  e.anchor = anchor;
  e.align = align;
  e.text = text;
  e.text_length = std::strlen(text);
  return e;
}

f32 centre_x(const view::HudRect& r) { return (r.x0 + r.x1) * 0.5f; }
f32 centre_y(const view::HudRect& r) { return (r.y0 + r.y1) * 0.5f; }

}  // namespace

TEST_CASE(
    "game hud: elements stand on their anchors, align on them, and the stack keeps its rows") {
  // Nine elements, one an anchor, and the fixture's stack: every element's box on its point of the
  // safe region (5 % in), the stack's rows inside its panel, an element hidden drawing nothing.
  using hotload::HudAlign;
  using hotload::HudAnchor;
  view::GameHud hud = fixture_hud();
  const struct {
    HudAnchor anchor;
    HudAlign align;
  } at[] = {{HudAnchor::TopLeft, HudAlign::Left},     {HudAnchor::Top, HudAlign::Centre},
            {HudAnchor::TopRight, HudAlign::Right},   {HudAnchor::Left, HudAlign::Left},
            {HudAnchor::Centre, HudAlign::Centre},    {HudAnchor::Right, HudAlign::Right},
            {HudAnchor::BottomLeft, HudAlign::Left},  {HudAnchor::Bottom, HudAlign::Centre},
            {HudAnchor::BottomRight, HudAlign::Right}};
  for (u32 i = 0; i < 9; ++i) {
    REQUIRE(hud.set_element(element(10 + i, at[i].anchor, at[i].align, "[E] Pick up"), 1) ==
            hotload::Result::Ok);
  }
  hotload::HudElement hidden = element(30, HudAnchor::Centre, HudAlign::Centre, "hidden");
  hidden.visible = 0;
  REQUIRE(hud.set_element(hidden, 1) == hotload::Result::Ok);
  CHECK(hud.element_count == 10);
  // Said again under the same id, an element is replaced in place, not added.
  REQUIRE(hud.set_element(element(14, HudAnchor::Centre, HudAlign::Centre, "[E] Drink"), 2) ==
          hotload::Result::Ok);
  CHECK(hud.element_count == 10);
  CHECK(std::strcmp(hud.find_element(14)->text, "[E] Drink") == 0);

  view::GameHudOverlay overlay;
  REQUIRE(overlay.create());
  lay_out_twice(overlay, hud, nullptr, 1920, 1080, 1.0f);
  const f32 mx = 96.0f;  // 5 % of 1920 x 1080
  const f32 my = 54.0f;
  const view::HudRect tl = overlay.bounds_of(10);
  CHECK(tl.x0 == mx);
  CHECK(tl.y0 == my);
  const view::HudRect top = overlay.bounds_of(11);
  CHECK(centre_x(top) == doctest::Approx(960.0f).epsilon(0.002));
  CHECK(top.y0 == my);
  const view::HudRect tr = overlay.bounds_of(12);
  CHECK(tr.x1 == doctest::Approx(1920.0f - mx).epsilon(0.001));
  const view::HudRect left = overlay.bounds_of(13);
  CHECK(left.x0 == mx);
  CHECK(centre_y(left) == doctest::Approx(540.0f).epsilon(0.002));
  const view::HudRect centre = overlay.bounds_of(14);
  CHECK(centre_x(centre) == doctest::Approx(960.0f).epsilon(0.002));
  CHECK(centre_y(centre) == doctest::Approx(540.0f).epsilon(0.002));
  const view::HudRect bl = overlay.bounds_of(16);
  CHECK(bl.y1 == doctest::Approx(1080.0f - my).epsilon(0.001));
  const view::HudRect br = overlay.bounds_of(18);
  CHECK(br.x1 == doctest::Approx(1920.0f - mx).epsilon(0.001));
  CHECK(br.y1 == doctest::Approx(1080.0f - my).epsilon(0.001));
  // 18 px at 1080p: the box is a line of text tall, a little more with no outline.
  CHECK(centre.y1 - centre.y0 >= 17.0f);
  CHECK(centre.y1 - centre.y0 <= 24.0f);
  CHECK(overlay.bounds_of(30).empty());  // hidden
  // The stack's rows, under their reserved ids, inside the stack's panel.
  const view::HudRect meter = overlay.bounds_of(hotload::k_hud_meter_id);
  const view::HudRect line0 = overlay.bounds_of(hotload::k_hud_line_id);
  CHECK(inside(meter, overlay.panel().x0, overlay.panel().y0, overlay.panel().x1,
               overlay.panel().y1));
  CHECK(inside(line0, overlay.panel().x0, overlay.panel().y0, overlay.panel().x1,
               overlay.panel().y1));
  CHECK(line0.y0 >= meter.y1);  // the meters first, then the lines
  // Twice the picture, twice the box.
  lay_out_twice(overlay, hud, nullptr, 3840, 2160, 1.0f);
  const view::HudRect big = overlay.bounds_of(14);
  CHECK(big.y1 - big.y0 == doctest::Approx(2.0f * (centre.y1 - centre.y0)).epsilon(0.15));
  CHECK(centre_x(big) == doctest::Approx(1920.0f).epsilon(0.002));

  // The report's list: the stack's rows first under their ids, then the elements in order.
  const JsonValue list = view::hud_elements_json(hud, &overlay);
  REQUIRE(list.is_array());
  CHECK(list.size() == 1 + 3 + 10);
  const JsonValue& first = list[usize{0}];
  CHECK(first.find("kind")->as_string() == "meter");
  u64 id = 0;
  REQUIRE(list[4].find("id")->get_u64(id));
  CHECK(id == 10);
  CHECK(list[8].find("anchor")->as_string() == "centre");
  CHECK(list[8].find("text")->as_string() == "[E] Drink");
  CHECK(list[8].find("bounds_px")->size() == 4);

  // Removal, and the table's limit.
  CHECK(hud.remove_element(14) == hotload::Result::Ok);
  CHECK(hud.remove_element(14) == hotload::Result::NotFound);
  CHECK(hud.find_element(15) != nullptr);
  for (u32 i = 0; hud.element_count < view::GameHud::k_elements; ++i) {
    REQUIRE(hud.set_element(element(100 + i, HudAnchor::Top, HudAlign::Centre, "x"), 3) ==
            hotload::Result::Ok);
  }
  CHECK(hud.set_element(element(9999, HudAnchor::Top, HudAlign::Centre, "x"), 3) ==
        hotload::Result::OutOfRange);
  hud.clear();  // a tick: the lines and meters go, the elements stay
  CHECK(hud.stack_empty());
  CHECK(hud.element_count == view::GameHud::k_elements);
  hud.clear_all();  // a new run
  CHECK(hud.element_count == 0);
  CHECK(hud.empty());
}

TEST_CASE("game hud: a size change bakes the font again; a frame at the same size does not") {
  // What a rebuild costs on the CPU: ImGui rasterizes the glyphs a frame first draws at a size
  // when it renders that frame, so the layout after a size change carries the bake, and the one
  // after it at the same size is the layout alone. The upload to the overlay is `upload`'s, timed
  // in the captures' summary (`overlay.atlas_ms`).
  view::GameHudOverlay overlay;
  REQUIRE(overlay.create());
  const view::GameHud hud = fixture_hud();
  const auto timed = [&](u32 width, u32 height) {
    const i64 start = time::monotonic_ns();
    REQUIRE(overlay.layout(hud, nullptr, width, height, 1.0f));
    return static_cast<f64>(time::monotonic_ns() - start) / 1.0e6;
  };
  const f64 first = timed(1920, 1080);  // the first frame: the atlas and the font at 18 px
  const f64 same = timed(1920, 1080);   // the same size: no bake
  const f64 same2 = timed(1920, 1080);
  const f64 resized = timed(3840, 2160);  // 36 px: a bake
  const f64 after = timed(3840, 2160);
  MESSAGE("layout ms: first " << first << ", same " << same << ", " << same2 << ", resized "
                              << resized << ", after " << after);
  CHECK(overlay.pending_textures() >= 1);  // nothing uploaded them: the atlas is still asked for
  CHECK(overlay.layouts() == 5);
  // A layout at the same size is cheap in any build; the bound is loose because this runs in a
  // Debug build beside other load (the figure for the page comes from the MESSAGE above).
  CHECK(same < 50.0);
  CHECK(after < 50.0);
}

#if defined(WALK_POC_PATH)

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run run_view(const std::vector<std::string>& args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

#ifndef ENGINE_VIEW_TESTS_DUNES
#define ENGINE_VIEW_TESTS_DUNES 0
#endif
bool refused_without_dunes(const Run& run) {
  return ENGINE_VIEW_TESTS_DUNES == 0 && run.exit_code == 1 &&
         run.output.find("which this build does not have") != std::string::npos;
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
  for (const char* key : path) {
    p = p != nullptr && p->is_object() ? p->find(key) : nullptr;
  }
  return p;
}

f64 number_at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = at(v, path);
  f64 out = -1.0;
  return p != nullptr && p->get_f64(out) ? out : -1.0;
}

std::string fixture(const test::TempDir& tmp, const char* built) {
  const std::filesystem::path from(built);
  const std::string to = tmp.file("walk_poc" + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  return to;
}

std::string erg_window(const test::TempDir& tmp) {
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!test::path_exists(committed)) {
    MESSAGE("the erg is not in this bundle");
    return {};
  }
  std::string source;
  REQUIRE(io::read_file(committed, source) == io::Status::Ok);
  JsonValue erg;
  REQUIRE(parse_json(source, erg).ok);
  JsonValue terrain = *erg.find("terrain");
  terrain.set("size", static_cast<u64>(129));
  terrain.set("extent", JsonValue(128.0));
  erg.set("terrain", std::move(terrain));
  const std::string scene = tmp.file("erg.json");
  REQUIRE(io::write_file(scene, write_json(erg)) == io::Status::Ok);
  std::string path_text;
  REQUIRE(io::read_file(committed.substr(0, committed.size() - std::string("scene.json").size()) +
                            "camera-path.json",
                        path_text) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("camera-path.json"), path_text) == io::Status::Ok);
  return scene;
}

// game_view_tests.cpp's walk: W from tick 10, E at tick 100, so the HUD has a meter and three
// lines.
std::string walk_session(const test::TempDir& tmp, const std::string& scene, u64 ticks) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(10, 26, true);  // W
  key(100, 8, true);  // E
  key(101, 8, false);
  view::SessionHeader header;
  header.ticks = ticks;
  header.scene = scene;
  header.start.position = WorldPos{20.0, 40.0, 10.0};
  header.start.yaw = 0.5f;
  header.start.pitch = -0.2f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string path = tmp.file("walk.jsonl");
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

// The same replay captured with the game and without, at `width` x `height`: where the HUD's
// pixels fell. `region` is the HUD's allowed rectangle (x0, y0, x1, y1); pixels that differ
// outside it are `elsewhere`.
struct HudPixels {
  u64 inside = 0;
  u64 elsewhere = 0;
  u32 min_x = ~0u, min_y = ~0u, max_x = 0, max_y = 0;  // the bounding box of what differed
  f64 font_px = 0.0;
  f64 atlas_uploads = 0.0;
  f64 atlas_ms = 0.0;
  bool ran = false;
};

HudPixels capture_hud(const test::TempDir& tmp, u32 width, u32 height, const u32 region[4]) {
  HudPixels out;
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return out;
  const std::string session = walk_session(tmp, scene, 120);
  const std::string with = tmp.file("with.png");
  const std::string without = tmp.file("without.png");
  const std::vector<std::string> common = {"--replay-input",
                                           session,
                                           "--scene",
                                           scene,
                                           "--offscreen",
                                           "--width",
                                           std::to_string(width),
                                           "--height",
                                           std::to_string(height),
                                           "--ddc",
                                           tmp.file("ddc")};
  std::vector<std::string> args = common;
  args.insert(args.end(), {"--game", fixture(tmp, WALK_POC_PATH), "--capture", with});
  const Run game = run_view(args);
  if (game.exit_code == 3 || refused_without_dunes(game)) {
    MESSAGE("engine-view unavailable here: " << game.output);
    return out;
  }
  REQUIRE_MESSAGE(game.exit_code == 0, game.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(game, summary), game.output);
  out.font_px = number_at(summary, {"interactive", "game", "overlay", "font_px"});
  out.atlas_uploads = number_at(summary, {"interactive", "game", "overlay", "atlas_uploads"});
  out.atlas_ms = number_at(summary, {"interactive", "game", "overlay", "atlas_ms"});
  args = common;
  args.insert(args.end(), {"--capture", without});
  const Run plain = run_view(args);
  REQUIRE_MESSAGE(plain.exit_code == 0, plain.output);
  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(with, a) == io::Status::Ok);
  REQUIRE(image::read_image(without, b) == io::Status::Ok);
  REQUIRE(a.width == width);
  REQUIRE(a.height == height);
  REQUIRE(b.width == width);
  REQUIRE(b.height == height);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const u32 i = (y * width + x) * 4;
      if (std::memcmp(&a.pixels[i], &b.pixels[i], 3) == 0) continue;
      const bool in = x >= region[0] && y >= region[1] && x < region[2] && y < region[3];
      (in ? out.inside : out.elsewhere) += 1;
      out.min_x = x < out.min_x ? x : out.min_x;
      out.min_y = y < out.min_y ? y : out.min_y;
      out.max_x = x > out.max_x ? x : out.max_x;
      out.max_y = y > out.max_y ? y : out.max_y;
    }
  }
  out.ran = true;
  MESSAGE(width << "x" << height << ": font " << out.font_px << " px, the HUD changed "
                << out.inside << " pixels inside its region and " << out.elsewhere
                << " outside, from " << out.min_x << "," << out.min_y << " to " << out.max_x << ","
                << out.max_y << "; atlas uploads " << out.atlas_uploads << " in " << out.atlas_ms
                << " ms");
  if (out.elsewhere != 0) tmp.keep();  // the two pictures, for a human to look at
  return out;
}

}  // namespace

TEST_CASE("engine-view: the HUD at 1600x900 is 15 px and stays in the top-left quarter") {
  const test::TempDir tmp("engine_view_game_hud_900");
  const u32 quarter[4] = {80, 45, 800, 450};  // the safe margin to the quarter's edge
  const HudPixels hud = capture_hud(tmp, 1600, 900, quarter);
  if (!hud.ran) return;
  CHECK(hud.font_px == 15.0);
  CHECK(hud.inside > 4000);  // a panel with a meter and three lines, at 15 px
  CHECK(hud.elsewhere == 0);
  CHECK(hud.min_x >= 80);
  CHECK(hud.min_y >= 45);
  CHECK(hud.atlas_uploads >= 1.0);
  CHECK(hud.atlas_uploads <=
        4.0);  // the first frame's, and a glyph or two baked later; not a frame's
}

TEST_CASE("engine-view: the HUD at 320x180 is 10 px, inside the safe region, nothing clipped") {
  const test::TempDir tmp("engine_view_game_hud_180");
  // 5 % of 320 x 180 is 16 x 9: the HUD may touch nothing outside [16, 304) x [9, 171), and a
  // panel the region had cut would reach its edge.
  const u32 safe[4] = {16, 9, 304, 171};
  const HudPixels hud = capture_hud(tmp, 320, 180, safe);
  if (!hud.ran) return;
  CHECK(hud.font_px == 10.0);
  CHECK(hud.inside > 1500);
  CHECK(hud.elsewhere == 0);
  CHECK(hud.min_x >= 16);
  CHECK(hud.min_y >= 9);
  CHECK(hud.max_x < 303);  // short of the region's last column: not cut by it
  CHECK(hud.max_y < 170);
}

#endif  // WALK_POC_PATH
#endif  // ENGINE_VIEW_GAME
