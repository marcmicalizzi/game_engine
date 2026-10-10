// engine-editor (docs/subsystems/apps.md, "engine-editor"; ADR-0054). In process and offscreen: a
// fixture document opened through the protocol's session, its panels' models built with no window,
// a pixel of an offscreen capture picked back to its record, a transaction moving that record's
// translation moving its instance in the next frame, and the panels composited by the renderer's
// overlay pass. Then the executable itself in a small window, throttled by vsync and the
// editor's own ceiling, picking through the window's frame.

#include "../editor_document.h"
#include "../editor_ui.h"
#include "../editor_viewport.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/platform/process.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

std::string n(u64 v) { return std::to_string(v); }

// A unit cube as a GLB (systems/renderer/tests/world_translation_tests.cpp's).
bool write_cube_glb(const std::string& path) {
  const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                           Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                            Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<u8> bin;
  for (u32 f = 0; f < 6; ++f) {
    const Vec3 nrm = normals[f];
    const Vec3 t = tangents[f];
    const Vec3 b = cross(nrm, t);
    const Vec3 corners[4] = {nrm * 0.5f - t * 0.5f - b * 0.5f, nrm * 0.5f + t * 0.5f - b * 0.5f,
                             nrm * 0.5f + t * 0.5f + b * 0.5f, nrm * 0.5f - t * 0.5f + b * 0.5f};
    for (const Vec3& c : corners) {
      put_f32(bin, c.x);
      put_f32(bin, c.y);
      put_f32(bin, c.z);
    }
  }
  const u64 normal_offset = bin.size();
  for (u32 f = 0; f < 6; ++f) {
    for (u32 c = 0; c < 4; ++c) {
      put_f32(bin, normals[f].x);
      put_f32(bin, normals[f].y);
      put_f32(bin, normals[f].z);
    }
  }
  const u64 uv_offset = bin.size();
  for (u32 f = 0; f < 6; ++f) {
    const f32 uvs[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (const f32 v : uvs)
      put_f32(bin, v);
  }
  const u64 index_offset = bin.size();
  for (u32 f = 0; f < 6; ++f) {
    const u32 base = f * 4;
    const u32 tris[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    for (const u32 i : tris) {
      bin.push_back(static_cast<u8>(i & 0xffu));
      bin.push_back(static_cast<u8>(i >> 8));
    }
  }
  const u64 index_bytes = bin.size() - index_offset;
  while (bin.size() % 4 != 0)
    bin.push_back(0);
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.5,0.2,1],"
      "\"metallicFactor\":0,\"roughnessFactor\":0.6}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + n(bin.size()) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';
  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return f.good();
}

constexpr const char* k_placement = "engine.scene.MeshPlacement";
const Id128 k_left{0x20, 1};   // a cube left of the origin
const Id128 k_yard{0x20, 2};   // a node, the right cube's parent
const Id128 k_right{0x20, 3};  // a cube right of the origin

JsonValue json(const std::string& text) {
  JsonValue v;
  REQUIRE(parse_json(text, v).ok);
  return v;
}

// The fixture: two cubes and a node, made by one transaction through the session's checked path.
void make_fixture(editor::EditorDocument& document, const std::string& dir) {
  std::string error;
  REQUIRE(write_cube_glb(dir + "/cube.glb"));
  REQUIRE_MESSAGE(document.open(dir, true, "editor-test", error), error);
  Vector<doc::Command> commands;
  commands.push_back(doc::cmd_create(
      k_left, k_placement, Id128{},
      json(R"({"name":"left","mesh":"cube.glb","translation":[-1.5,0,0],"scale":[1.5,1.5,1.5]})")));
  commands.push_back(
      doc::cmd_create(k_yard, "engine.world.Node", Id128{}, json(R"({"name":"yard"})")));
  commands.push_back(doc::cmd_create(
      k_right, k_placement, k_yard,
      json(R"({"name":"right","mesh":"cube.glb","translation":[1.5,0,0],"yaw_deg":30})")));
  REQUIRE_MESSAGE(document.apply(std::span<const doc::Command>(commands.data(), commands.size()),
                                 "the fixture", error),
                  error);
}

// Where a world point lands in the picture: the renderer's own view (what the selection box is
// drawn with), pixels y down.
bool project(editor::Viewport& viewport, const renderer::Camera& camera, WorldPos p, u32& x,
             u32& y) {
  const renderer::ViewSet& views = viewport.renderer().update_views(camera);
  const Vec4 clip = views[0].view_proj * Vec4(relative(p, camera.position), 1.0f);
  if (clip.w <= 0.0f) return false;
  const f32 w = static_cast<f32>(viewport.renderer().width());
  const f32 h = static_cast<f32>(viewport.renderer().height());
  x = static_cast<u32>((clip.x / clip.w * 0.5f + 0.5f) * w);
  y = static_cast<u32>((0.5f - clip.y / clip.w * 0.5f) * h);
  return x < viewport.renderer().width() && y < viewport.renderer().height();
}

renderer::Camera test_camera() {
  renderer::Camera camera;
  camera.position = WorldPos{0.0, 1.0, 7.0};
  camera.target = WorldPos{0.0, 0.0, 0.0};
  return camera;
}

}  // namespace

TEST_CASE("engine-editor: the panels' models come from the session and the schema") {
  const test::TempDir tmp("engine_editor_panels");
  editor::EditorDocument document;
  make_fixture(document, tmp.path());

  Vector<editor::OutlinerRow> rows;
  document.outliner(rows);
  REQUIRE(rows.size() == 3);  // parents before children, siblings by id
  CHECK(rows[0].id == k_left);
  CHECK(rows[0].name == "left");
  CHECK(rows[0].type == k_placement);
  CHECK(rows[0].depth == 0);
  CHECK(rows[1].id == k_yard);
  CHECK(rows[1].children == 1);
  CHECK(rows[2].id == k_right);
  CHECK(rows[2].depth == 1);

  // Every field of the type, in its order, by its kind; unset ones at the type's default.
  Vector<editor::PropertyRow> props;
  REQUIRE(document.properties(k_left, props));
  REQUIRE(props.size() == 6);
  CHECK(props[0].name == "name");
  CHECK(props[0].kind == "string");
  CHECK(props[1].name == "mesh");
  CHECK(props[2].name == "translation");
  CHECK(props[2].kind == "worldpos");
  CHECK(props[2].set);
  CHECK(props[2].value == "[-1.5,0,0]");
  CHECK(props[3].name == "rotation");
  CHECK(props[3].kind == "quat");
  CHECK_FALSE(props[3].set);
  CHECK(props[4].kind == "f32");
  CHECK(props[5].name == "scale");
  CHECK(props[5].kind == "vec3?");
  CHECK(props[5].set);
  CHECK_FALSE(document.properties(Id128{0x20, 99}, props));

  // The journal: the fixture's one transaction, attributed to the editor's caller.
  Vector<editor::JournalRow> journal;
  document.journal(journal, 10);
  REQUIRE(journal.size() == 1);
  CHECK(journal[0].actor == "editor-test");
  CHECK(journal[0].role == "director");
  CHECK(journal[0].rationale == "the fixture");
  CHECK(journal[0].commands == 3);
  CHECK_FALSE(journal[0].undone);

  // Undo is the session's, and the journal says what it undid.
  std::string error;
  REQUIRE_MESSAGE(document.undo(error), error);
  document.journal(journal, 10);
  REQUIRE(journal.size() == 1);
  CHECK(journal[0].undone);
  CHECK_FALSE(document.document().exists(k_left));
  REQUIRE_MESSAGE(document.redo(error), error);
  CHECK(document.document().exists(k_left));
}

TEST_CASE("engine-editor: a pick finds the record, and a moved translation moves it next frame") {
  const test::TempDir tmp("engine_editor_pick");
  gfx::Device device;
  std::string why;
  if (!device.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("no Vulkan device here: " << why);
    return;
  }
  {
    editor::EditorDocument document;
    make_fixture(document, tmp.path());
    editor::Viewport viewport;
    editor::Viewport::Options options;
    options.width = 320;
    options.height = 200;
    options.ddc = tmp.file("ddc");
    std::string error;
    if (!viewport.build(device, document.document(), document.directory(), options, error)) {
      MESSAGE("the viewport cannot draw here: " << error);
      return;
    }
    REQUIRE(viewport.scene().records.size() == 2);
    CHECK(viewport.scene().desc.meshes.size() == 1);  // one cube, shared

    const renderer::Camera camera = test_camera();
    u32 x = 0;
    u32 y = 0;
    REQUIRE(project(viewport, camera, WorldPos{-1.5, 0.0, 0.0}, x, y));
    renderer::FrameDesc frame;
    frame.camera = camera;

    // The capture's ids at that pixel name the left cube's instance, and the instance its record.
    renderer::CaptureChannels channels;
    channels.color = false;
    channels.ids = true;
    renderer::CapturedFrame shot;
    REQUIRE_MESSAGE(viewport.renderer().capture(frame, channels, shot, &error), error);
    const u32 instance = shot.ids[(y * shot.width + x) * renderer::k_id_words];
    REQUIRE(instance < viewport.scene().records.size());
    CHECK(viewport.scene().records[instance] == k_left);
    // The same answer from the frame last drawn, which is what a window's click reads.
    Id128 picked;
    REQUIRE_MESSAGE(viewport.pick(x, y, picked, error), error);
    CHECK(picked == k_left);
    u32 rx = 0;
    u32 ry = 0;
    REQUIRE(project(viewport, camera, WorldPos{1.5, 0.0, 0.0}, rx, ry));
    REQUIRE(viewport.pick(rx, ry, picked, error));
    CHECK(picked == k_right);
    REQUIRE(viewport.pick(2, 2, picked, error));
    CHECK(picked.is_null());  // the sky

    // One transaction through the session moves the left cube up and over; the viewport moves the
    // instance in place (no rebuild), and the next frame draws it there.
    const doc::Command move = doc::cmd_set(k_left, "translation", json("[0,2.2,0]"));
    REQUIRE_MESSAGE(document.apply(std::span<const doc::Command>(&move, 1), "", error), error);
    bool rebuilt = true;
    REQUIRE_MESSAGE(viewport.sync(document.document(), rebuilt, error), error);
    CHECK_FALSE(rebuilt);
    REQUIRE_MESSAGE(viewport.renderer().render_offscreen(frame, &error), error);
    REQUIRE(viewport.pick(x, y, picked, error));
    CHECK(picked != k_left);  // gone from where it stood
    u32 mx = 0;
    u32 my = 0;
    REQUIRE(project(viewport, camera, WorldPos{0.0, 2.2, 0.0}, mx, my));
    REQUIRE(viewport.pick(mx, my, picked, error));
    CHECK(picked == k_left);  // and found where it went

    // A new placement is a rebuild, and the new record picks too.
    const Id128 k_third{0x20, 4};
    const doc::Command add = doc::cmd_create(k_third, k_placement, Id128{},
                                             json(R"({"mesh":"cube.glb","translation":[0,-2,0]})"));
    REQUIRE_MESSAGE(document.apply(std::span<const doc::Command>(&add, 1), "", error), error);
    REQUIRE_MESSAGE(viewport.sync(document.document(), rebuilt, error), error);
    CHECK(rebuilt);
    REQUIRE_MESSAGE(viewport.renderer().render_offscreen(frame, &error), error);
    u32 tx = 0;
    u32 ty = 0;
    REQUIRE(project(viewport, camera, WorldPos{0.0, -2.0, 0.0}, tx, ty));
    REQUIRE(viewport.pick(tx, ty, picked, error));
    CHECK(picked == k_third);
  }
  device.destroy();
}

TEST_CASE("engine-editor: the panels reach the picture through the renderer's overlay pass") {
  const test::TempDir tmp("engine_editor_overlay");
  gfx::Device device;
  std::string why;
  if (!device.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("no Vulkan device here: " << why);
    return;
  }
  {
    editor::EditorDocument document;
    make_fixture(document, tmp.path());
    editor::Viewport viewport;
    editor::Viewport::Options options;
    options.width = 320;
    options.height = 200;
    options.ddc = tmp.file("ddc");
    std::string error;
    if (!viewport.build(device, document.document(), document.directory(), options, error)) {
      MESSAGE("the viewport cannot draw here: " << error);
      return;
    }
    const renderer::Camera camera = test_camera();
    renderer::FrameDesc frame;
    frame.camera = camera;
    renderer::CaptureChannels color;
    renderer::CapturedFrame bare;
    REQUIRE_MESSAGE(viewport.renderer().capture(frame, color, bare, &error), error);

    editor::EditorUi ui;
    REQUIRE(ui.create());
    document.select(k_right);
    renderer::OverlayDrawData overlay;
    // Two frames: the first lays the dock out and makes the font atlas.
    for (u32 f = 0; f < 2; ++f) {
      editor::EditorUi::Actions actions;
      ui.frame(1.0f / 60.0f, 320, 200, document, viewport, camera, actions);
      REQUIRE_MESSAGE(ui.render(viewport.renderer(), overlay, error), error);
    }
    CHECK(ui.outliner_rows() == 3);
    CHECK(ui.property_rows() == 6);
    CHECK(ui.journal_rows() == 1);
    CHECK_FALSE(overlay.commands.empty());
    CHECK(viewport.renderer().overlay().texture_count() >= 1);

    frame.overlay = &overlay;
    renderer::CapturedFrame panels;
    REQUIRE_MESSAGE(viewport.renderer().capture(frame, color, panels, &error), error);
    REQUIRE(panels.color.size() == bare.color.size());
    // The outliner docks on the left: its pixels are the panel's, not the picture's.
    u32 differing = 0;
    for (u32 yy = 40; yy < 160; ++yy) {
      for (u32 xx = 4; xx < 60; ++xx) {
        const u32 p = (yy * 320 + xx) * 4;
        differing += std::memcmp(&panels.color[p], &bare.color[p], 3) != 0 ? 1u : 0u;
      }
    }
    CHECK(differing > 1000);
    // The overlay draws only colour: the ids under the panels are still the scene's.
    renderer::CaptureChannels ids;
    ids.color = false;
    ids.ids = true;
    renderer::CapturedFrame under;
    REQUIRE_MESSAGE(viewport.renderer().capture(frame, ids, under, &error), error);
    frame.overlay = nullptr;
    renderer::CapturedFrame plain;
    REQUIRE_MESSAGE(viewport.renderer().capture(frame, ids, plain, &error), error);
    CHECK(under.ids == plain.ids);
    ui.destroy();
  }
  device.destroy();
}

TEST_CASE("engine-editor: the window picks what is under its pixel") {
  const test::TempDir tmp("engine_editor_window");
  // One cube at the origin, three metres across, so the window's framing camera puts it under the
  // middle of the picture whatever the panels cover.
  {
    editor::EditorDocument document;
    std::string error;
    REQUIRE(write_cube_glb(tmp.path() + "/cube.glb"));
    REQUIRE_MESSAGE(document.open(tmp.path(), true, "editor-test", error), error);
    const doc::Command add = doc::cmd_create(
        k_left, k_placement, Id128{},
        json(R"({"name":"only","mesh":"cube.glb","translation":[0,0,0],"scale":[3,3,3]})"));
    REQUIRE_MESSAGE(document.apply(std::span<const doc::Command>(&add, 1), "", error), error);
  }
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  const std::string doc_dir = tmp.path();
  const std::string ddc = tmp.file("ddc");
  // Small, vsynced and under the editor's 120 Hz ceiling: never an unthrottled window.
  const std::vector<std::string_view> argv = {exe,        "--doc", doc_dir,    "--width", "320",
                                              "--height", "240",   "--frames", "6",       "--pick",
                                              "160,110",  "--ddc", ddc};
  platform::Process p;
  std::string error;
  REQUIRE_MESSAGE(p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
                          /*merge_stderr=*/true),
                  error);
  p.close_stdin();
  std::string output;
  p.read_all(output);
  const i32 code = p.wait();
  if (code == 3) {
    MESSAGE("engine-editor has no window here: " << output);
    return;
  }
  REQUIRE_MESSAGE(code == 0, output);
  const usize brace = output.rfind("{\"frames\"");
  REQUIRE_MESSAGE(brace != std::string::npos, output);
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(output.substr(brace), summary).ok, output);
  const JsonValue* frames = summary.find("frames");
  REQUIRE(frames != nullptr);
  CHECK(write_json(*frames) == "6");
  const JsonValue* selected = summary.find("selected");
  REQUIRE(selected != nullptr);
  REQUIRE_MESSAGE(selected->is_string(), output);
  CHECK(std::string(selected->as_string()) == editor::hex(k_left));
  const JsonValue* panels = summary.find("panels");
  REQUIRE(panels != nullptr);
  CHECK(write_json(*panels->find("properties")) == "6");
}
