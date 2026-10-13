// engine-editor (docs/subsystems/apps.md, "engine-editor"; ADR-0054). With no window and no
// device: the pointer's maths (display scaling, a pixel's ray, the gizmo's handles, hover and
// drag). In process and offscreen: a fixture document opened through the protocol's session, its
// panels' models built with no window, a pixel of an offscreen capture picked back to its record, a
// transaction moving that record's translation moving its instance in the next frame, a gizmo drag
// that previews without writing and commits one transaction on release, Escape, undo, redo and
// delete through the keys, and the panels composited by the renderer's overlay pass. Then the
// executable itself in a small window, throttled by vsync and the editor's own ceiling, picking
// through the window's frame and dragging a gizmo handle by a scripted pointer.

#include "../editor_document.h"
#include "../editor_input.h"
#include "../editor_methods.h"
#include "../editor_ui.h"
#include "../editor_viewport.h"
#include "../viewport_math.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/platform/process.h>
#include <domain/gfx/device.h>
#include <domain/protocol/listener.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <foundation/io/vfs.h>
#include <foundation/net/websocket.h>
#include <systems/renderer/capture.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
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

// A JSON number as a double, whichever way it was written; NaN when it is not one.
f64 number(const JsonValue* v) {
  f64 out = std::nan("");
  if (v != nullptr) (void)v->get_f64(out);
  return out;
}

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

// A record's `translation` as three doubles.
bool translation_of(const doc::Document& d, Id128 id, f64 out[3]) {
  const JsonValue* t = d.property(id, "translation");
  if (t == nullptr || !t->is_array() || t->size() != 3) return false;
  for (u32 i = 0; i < 3; ++i) {
    if (!(*t)[i].get_f64(out[i])) return false;
  }
  return true;
}

window::Event pointer(window::EventKind kind, f32 x, f32 y) {
  window::Event e;
  e.kind = kind;
  e.x = x;
  e.y = y;
  e.button = 1;
  return e;
}

window::Event key(window::EventKind kind, window::Key k) {
  window::Event e;
  e.kind = kind;
  e.key = k;
  return e;
}

// Runs a test program and returns its summary line, or skips (false) where it has no window.
bool run_editor(const std::vector<std::string_view>& argv, JsonValue& summary,
                std::string& output) {
  platform::Process p;
  std::string error;
  REQUIRE_MESSAGE(p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
                          /*merge_stderr=*/true),
                  error);
  p.close_stdin();
  p.read_all(output);
  const i32 code = p.wait();
  if (code == 3) {
    MESSAGE("engine-editor has no window here: " << output);
    return false;
  }
  REQUIRE_MESSAGE(code == 0, output);
  // The summary is the last line the run prints, one JSON object (its keys sorted).
  const usize line = output.rfind("\n{");
  const usize brace = line != std::string::npos ? line + 1 : (output.rfind('{', 0) == 0 ? 0 : line);
  REQUIRE_MESSAGE(brace != std::string::npos, output);
  REQUIRE_MESSAGE(parse_json(output.substr(brace), summary).ok, output);
  return true;
}

}  // namespace

TEST_CASE("engine-editor: the pointer's maths, with no window and no device") {
  using namespace editor;
  // **Display scaling.** A 1280 x 720 window at 150 % draws a 1920 x 1080 picture, and its pointer
  // reports window coordinates: unscaled, a click at the window's (640, 360) would read pixel
  // (640, 360) of the picture, a third of the way from its middle to its corner.
  const PointerScale scaled = pointer_scale(1280, 720, 1920, 1080);
  CHECK(scaled.x == doctest::Approx(1.5f));
  CHECK(scaled.y == doctest::Approx(1.5f));
  CHECK(640.0f * scaled.x == doctest::Approx(960.0f));
  CHECK(360.0f * scaled.y == doctest::Approx(540.0f));
  const PointerScale uneven = pointer_scale(1000, 500, 1250, 1000);
  CHECK(uneven.x == doctest::Approx(1.25f));
  CHECK(uneven.y == doctest::Approx(2.0f));
  const PointerScale same = pointer_scale(320, 240, 320, 240);
  CHECK(same.x == 1.0f);
  CHECK(same.y == 1.0f);
  const PointerScale unknown = pointer_scale(0, 0, 320, 240);  // no window: offscreen
  CHECK(unknown.x == 1.0f);
  CHECK(unknown.y == 1.0f);

  // **The eye's ray through a pixel.** A frame's view-projection with the eye at the origin, as
  // the renderer builds it: a point projects to a pixel, and the ray through that pixel — reached
  // from the window's coordinates through the 150 % scale — passes through the point.
  const u32 w = 1920;
  const u32 h = 1080;
  const f32 fov = radians(55.0f);
  const Mat4 vp = perspective_reversed_z(fov, static_cast<f32>(w) / static_cast<f32>(h), 0.05f) *
                  look_at(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -0.3f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
  const Vec3 p{1.2f, -0.4f, -6.0f};
  f32 px = 0.0f;
  f32 py = 0.0f;
  REQUIRE(project_point(vp, p, w, h, px, py));
  const f32 window_x = px / scaled.x;
  const f32 window_y = py / scaled.y;
  Ray ray;
  REQUIRE(pixel_ray(vp, window_x * scaled.x, window_y * scaled.y, w, h, ray));
  CHECK(length(ray.direction) == doctest::Approx(1.0f));
  const Vec3 to = p - ray.origin;
  CHECK(length(to - ray.direction * dot(to, ray.direction)) < 1.0e-3f);
  // Unscaled, the same click misses the point by metres.
  Ray wrong;
  REQUIRE(pixel_ray(vp, window_x, window_y, w, h, wrong));
  const Vec3 to_wrong = p - wrong.origin;
  CHECK(length(to_wrong - wrong.direction * dot(to_wrong, wrong.direction)) > 0.5f);
  CHECK_FALSE(project_point(vp, Vec3{0.0f, 0.0f, 4.0f}, w, h, px, py));  // behind the eye
  CHECK_FALSE(pixel_ray(vp, 1.0f, 1.0f, 0, h, ray));

  // **The nearest point of an axis to a ray**, and the two ways there is none.
  const Ray down{Vec3{2.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  f32 t = 0.0f;
  REQUIRE(closest_on_axis(down, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, t));
  CHECK(t == doctest::Approx(2.0f));
  CHECK_FALSE(
      closest_on_axis(down, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, t));  // at the eye
  const Ray away{Vec3{2.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 1.0f}};
  CHECK_FALSE(closest_on_axis(away, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, t));  // behind

  // **The handles**: `pixels` long across the view at the centre's depth, x to the right and y up
  // the picture; the pointer hovers a handle near its segment, none where all three meet, none
  // between them.
  const Vec3 centre{0.0f, -1.0f, -8.0f};
  const GizmoHandles g = gizmo_handles(vp, centre, fov, w, h, 120.0f);
  REQUIRE(g.visible);
  REQUIRE(g.shown[0]);
  REQUIRE(g.shown[1]);
  REQUIRE(g.shown[2]);
  CHECK(std::hypot(g.ex[0] - g.cx, g.ey[0] - g.cy) == doctest::Approx(120.0f).epsilon(0.01));
  CHECK(g.ex[0] > g.cx);
  CHECK(g.ey[1] < g.cy);
  CHECK(hover_axis(g, g.cx + 90.0f, g.cy + 2.0f, k_hover_pixels) == 0u);
  CHECK(hover_axis(g, (g.cx + g.ex[1]) * 0.5f + 3.0f, (g.cy + g.ey[1]) * 0.5f, k_hover_pixels) ==
        1u);
  CHECK(hover_axis(g, (g.cx + g.ex[2]) * 0.5f, (g.cy + g.ey[2]) * 0.5f, k_hover_pixels) == 2u);
  CHECK(hover_axis(g, g.cx + 3.0f, g.cy - 3.0f, k_hover_pixels) == k_no_axis);
  CHECK(hover_axis(g, g.cx + 60.0f, g.cy - 60.0f, k_hover_pixels) == k_no_axis);
  CHECK(hover_axis(g, g.cx + 140.0f, g.cy, k_hover_pixels) == k_no_axis);  // past the end
  const GizmoHandles behind = gizmo_handles(vp, Vec3{0.0f, 0.0f, 3.0f}, fov, w, h, 120.0f);
  CHECK_FALSE(behind.visible);
  CHECK(hover_axis(behind, g.cx + 90.0f, g.cy, k_hover_pixels) == k_no_axis);

  // **A drag along y**, far from the world's origin: begun on the handle a quarter of a metre up,
  // its delta is a metre when the pointer is over the point a metre and a quarter up — and still a
  // metre after the eye has flown three metres sideways with the pointer over the same point.
  const WorldPos eye{100000.0, 20.0, -30.0};
  const WorldPos anchor = eye + DVec3{centre};
  REQUIRE(project_point(vp, centre + Vec3{0.0f, 0.25f, 0.0f}, w, h, px, py));
  REQUIRE(pixel_ray(vp, px, py, w, h, ray));
  AxisDrag drag;
  REQUIRE(begin_axis_drag(1, anchor, eye, ray, drag));
  CHECK(drag.start == doctest::Approx(0.25f).epsilon(0.002));
  REQUIRE(project_point(vp, centre + Vec3{0.0f, 1.25f, 0.0f}, w, h, px, py));
  REQUIRE(pixel_ray(vp, px, py, w, h, ray));
  REQUIRE(update_axis_drag(drag, eye, ray));
  CHECK(drag.delta == doctest::Approx(1.0f).epsilon(0.002));
  const WorldPos flown = eye + DVec3{3.0, 0.0, 0.0};
  REQUIRE(project_point(vp, relative(anchor + DVec3{0.0, 1.25, 0.0}, flown), w, h, px, py));
  REQUIRE(pixel_ray(vp, px, py, w, h, ray));
  REQUIRE(update_axis_drag(drag, flown, ray));
  CHECK(drag.delta == doctest::Approx(1.0f).epsilon(0.002));
  // Along the eye's own line of sight there is nothing to read: the last delta stands.
  const Ray along{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}};
  CHECK_FALSE(update_axis_drag(drag, eye, along));
  CHECK(drag.delta == doctest::Approx(1.0f).epsilon(0.002));
  // The translation moves along y alone; x and z keep their bits.
  const WorldPos start{7.25, -3.5, 1000000.125};
  const WorldPos moved = dragged_translation(start, drag);
  CHECK(moved.x == start.x);
  CHECK(moved.z == start.z);
  CHECK(moved.y == doctest::Approx(-2.5).epsilon(0.002));
  CHECK(std::string(axis_name(1)) == "y");
  AxisDrag none;
  CHECK(dragged_translation(start, none) == start);
  CHECK_FALSE(begin_axis_drag(k_no_axis, anchor, eye, ray, none));
}

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

// The editor's host as main.cpp makes it — a dispatcher over the document's session with the
// editor's methods, and the listener over it with the selection publishing through its
// subscriptions — and one WebSocket client subscribed to `selection`.
struct SelectionWatch {
  editor::EditorState state;
  protocol::Dispatcher dispatcher;
  std::mutex lock;
  protocol::Listener listener;
  editor::EditorDocument* document;
  net::WebSocket client;

  explicit SelectionWatch(editor::EditorDocument& doc)
      : dispatcher(protocol::Context{&doc.sessions(), nullptr, nullptr, &state, nullptr}),
        listener(dispatcher, lock),
        document(&doc) {
    state.document = &doc;
    protocol::add_builtin_methods(dispatcher);
    editor::add_editor_methods(dispatcher);
    doc.set_events(&listener.events());
    std::string error;
    REQUIRE_MESSAGE(listener.start(0, "", doc.session().id(), error), error);
    const std::pair<std::string, std::string> token[] = {{"X-Engine-Token", listener.token()}};
    REQUIRE_MESSAGE(net::websocket_connect(listener.port(), "/", token, client, error), error);
    const JsonValue subscribed = call("session.subscribe", R"({"session":")" + doc.session().id() +
                                                               R"(","kinds":["selection"]})");
    REQUIRE_MESSAGE(subscribed.find("result") != nullptr, write_json(subscribed));
  }
  ~SelectionWatch() {
    client.close(net::close_code::k_normal, "done");
    listener.stop();
    document->set_events(nullptr);
  }
  JsonValue receive() {
    net::Message m;
    REQUIRE_MESSAGE(client.receive(m, 60000), client.close_reason());
    JsonValue message;
    REQUIRE_MESSAGE(parse_json(m.data, message).ok, m.data);
    return message;
  }
  // A request's answer; the selection's events in between are kept in `events`.
  JsonValue call(std::string_view method, const std::string& params) {
    REQUIRE(client.send_text(R"({"jsonrpc":"2.0","id":1,"method":")" + std::string(method) +
                             R"(","params":)" + params + "}"));
    for (;;) {
      JsonValue message = receive();
      if (message.find("id") != nullptr) return message;
      events.push_back(std::move(message));
    }
  }
  // The next `event.selection`: its `selected`, or "" for none.
  std::string next_selected() {
    JsonValue event;
    if (!events.empty()) {
      event = std::move(events.front());
      events.erase(events.begin());
    } else {
      event = receive();
    }
    CHECK(std::string(event.find("method")->as_string()) == "event.selection");
    const JsonValue& p = *event.find("params");
    CHECK(std::string(p.find("session")->as_string()) == document->session().id());
    const JsonValue* selected = p.find("selected");
    return selected != nullptr && selected->is_string() ? std::string(selected->as_string())
                                                        : std::string();
  }
  std::vector<JsonValue> events;
};

TEST_CASE("engine-editor: a selection change reaches a subscriber as event.selection") {
  const test::TempDir tmp("engine_editor_selection_events");
  editor::EditorDocument document;
  make_fixture(document, tmp.path());
  SelectionWatch watch(document);
  // editor.select over the socket, as the shell's outliner sends it.
  const JsonValue chosen =
      watch.call("editor.select", std::string(R"({"id":")") + editor::hex(k_right) + "\"}");
  REQUIRE_MESSAGE(chosen.find("result") != nullptr, write_json(chosen));
  CHECK(watch.next_selected() == editor::hex(k_right));
  // The same selection again changes nothing and says nothing; null clears it, and says so.
  (void)watch.call("editor.select", std::string(R"({"id":")") + editor::hex(k_right) + "\"}");
  (void)watch.call("editor.select", R"({"id":null})");
  CHECK(watch.next_selected().empty());
  CHECK(watch.events.empty());
  // The viewport's own path (a click, under the dispatch lock as main.cpp takes it), and a click
  // on nothing — the scripted pick's off-picture case — which clears.
  {
    std::lock_guard<std::mutex> guard(watch.lock);
    document.select(k_left);
  }
  CHECK(watch.next_selected() == editor::hex(k_left));
  editor::EditorInput input;
  editor::Viewport unbuilt;
  std::string error;
  {
    std::lock_guard<std::mutex> guard(watch.lock);
    REQUIRE_MESSAGE(input.pick(-1.0f, -1.0f, document, unbuilt, error), error);
  }
  CHECK(watch.next_selected().empty());
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

    // The scripted pick's path (`--pick`, main.cpp's between_frames): the input's pick selects
    // what is under the pixel, and a subscriber hears it as `event.selection`.
    {
      SelectionWatch watch(document);
      editor::EditorInput input;
      {
        std::lock_guard<std::mutex> guard(watch.lock);
        REQUIRE_MESSAGE(
            input.pick(static_cast<f32>(x), static_cast<f32>(y), document, viewport, error), error);
      }
      CHECK(watch.next_selected() == editor::hex(k_left));
      {
        std::lock_guard<std::mutex> guard(watch.lock);
        REQUIRE(input.pick(static_cast<f32>(rx), static_cast<f32>(ry), document, viewport, error));
      }
      CHECK(watch.next_selected() == editor::hex(k_right));
    }

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
    editor::EditorInput input;
    renderer::OverlayDrawData overlay;
    // Two frames, as the window draws them: the first makes the font atlas.
    for (u32 f = 0; f < 2; ++f) {
      input.set_view(document, viewport, camera, 320, 200, 1.0f);
      ui.frame(1.0f / 60.0f, 320, 200, document, viewport, camera, input);
      REQUIRE_MESSAGE(ui.render(viewport.renderer(), overlay, error), error);
    }
    // No panels (they are the web shell's now): the selection's box and the gizmo, and ImGui
    // takes neither the pointer nor the keys from the viewport.
    CHECK_FALSE(overlay.commands.empty());
    CHECK(viewport.renderer().overlay().texture_count() >= 1);
    CHECK_FALSE(ui.wants_mouse());
    CHECK_FALSE(ui.wants_keyboard());

    frame.overlay = &overlay;
    renderer::CapturedFrame drawn;
    REQUIRE_MESSAGE(viewport.renderer().capture(frame, color, drawn, &error), error);
    REQUIRE(drawn.color.size() == bare.color.size());
    // The box and the handles are drawn over the picture, and nowhere is a panel: the left
    // quarter, where the outliner docked, is the scene's own.
    u32 differing = 0;
    u32 left = 0;
    for (u32 yy = 0; yy < 200; ++yy) {
      for (u32 xx = 0; xx < 320; ++xx) {
        const u32 p = (yy * 320 + xx) * 4;
        const bool d = std::memcmp(&drawn.color[p], &bare.color[p], 3) != 0;
        differing += d ? 1u : 0u;
        left += d && xx < 60 && yy > 40 && yy < 160 ? 1u : 0u;
      }
    }
    CHECK(differing > 50);
    CHECK(left < 200);
    // The overlay draws only colour: the ids under it are still the scene's.
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
  // The viewport alone (`--no-shell`): the web view beside it is the embedded case's.
  const std::vector<std::string_view> argv = {
      exe,        "--doc", doc_dir,  "--width", "320",   "--height", "240",
      "--frames", "6",     "--pick", "160,110", "--ddc", ddc,        "--no-shell"};
  JsonValue summary;
  std::string output;
  if (!run_editor(argv, summary, output)) return;
  const JsonValue* frames = summary.find("frames");
  REQUIRE(frames != nullptr);
  CHECK(write_json(*frames) == "6");
  const JsonValue* selected = summary.find("selected");
  REQUIRE(selected != nullptr);
  REQUIRE_MESSAGE(selected->is_string(), output);
  CHECK(std::string(selected->as_string()) == editor::hex(k_left));
  CHECK(write_json(*summary.find("embedded")) == "false");
  CHECK(summary.find("listen")->is_null());
}

TEST_CASE(
    "engine-editor: a gizmo drag previews, then commits one transaction; keys undo, redo, delete") {
  const test::TempDir tmp("engine_editor_gizmo");
  gfx::Device device;
  std::string why;
  if (!device.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("no Vulkan device here: " << why);
    return;
  }
  {
    using window::EventKind;
    using window::Key;
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
    REQUIRE_MESSAGE(viewport.renderer().render_offscreen(frame, &error), error);
    const u32 left = viewport.scene().instance_of(k_left);
    REQUIRE(left != ~0u);

    // A click on the left cube picks it, and says what the read cost.
    editor::EditorInput input;
    u32 x = 0;
    u32 y = 0;
    REQUIRE(project(viewport, camera, WorldPos{-1.5, 0.0, 0.0}, x, y));
    const f32 fx = static_cast<f32>(x);
    const f32 fy = static_cast<f32>(y);
    input.set_view(document, viewport, camera, 320, 200, 1.0f);
    CHECK_FALSE(input.handles().visible);  // nothing selected: no handles
    REQUIRE(input.handle(pointer(EventKind::MouseButtonDown, fx, fy), false, true, document,
                         viewport, error));
    REQUIRE(input.handle(pointer(EventKind::MouseButtonUp, fx, fy), false, true, document, viewport,
                         error));
    CHECK(document.selection() == k_left);
    CHECK(input.stats().picks == 1);
    CHECK(input.stats().pick_ms >= 0.0);

    // The handles stand at its box's centre; the pointer over the y handle hovers it.
    input.set_view(document, viewport, camera, 320, 200, 1.0f);
    const editor::GizmoHandles& h = input.handles();
    REQUIRE(h.visible);
    REQUIRE(h.shown[1]);
    CHECK(std::fabs(h.cx - fx) < 2.0f);
    CHECK(std::fabs(h.cy - fy) < 2.0f);
    const f32 gx = h.cx + (h.ex[1] - h.cx) * 0.6f;
    const f32 gy = h.cy + (h.ey[1] - h.cy) * 0.6f;
    REQUIRE(input.handle(pointer(EventKind::MouseMove, gx, gy), false, true, document, viewport,
                         error));
    CHECK(input.hovered() == 1u);
    // Pressing on it starts a drag rather than a pick, and the selection stays.
    REQUIRE(input.handle(pointer(EventKind::MouseButtonDown, gx, gy), false, true, document,
                         viewport, error));
    CHECK(input.dragging());
    CHECK(input.active() == 1u);
    CHECK(document.selection() == k_left);
    CHECK(input.stats().picks == 1);

    // While the button is held the instance moves in the viewport and the document does not.
    REQUIRE(input.handle(pointer(EventKind::MouseMove, gx, gy - 20.0f), false, true, document,
                         viewport, error));
    REQUIRE(input.handle(pointer(EventKind::MouseMove, gx, gy - 40.0f), false, true, document,
                         viewport, error));
    REQUIRE_MESSAGE(input.flush(viewport, error), error);
    CHECK(viewport.previewing());
    const f64 previewed = viewport.placement(left)->origin.y;
    CHECK(previewed > 0.5);
    CHECK(viewport.placement(left)->origin.x == -1.5);
    CHECK(document.document().journal().size() == 1);
    f64 t[3] = {};
    REQUIRE(translation_of(document.document(), k_left, t));
    CHECK(t[1] == 0.0);
    REQUIRE_MESSAGE(viewport.renderer().render_offscreen(frame, &error), error);
    Id128 picked;
    REQUIRE(viewport.pick(x, y, picked, error));
    CHECK(picked != k_left);  // the preview took it from where it stood
    u32 px = 0;
    u32 py = 0;
    REQUIRE(project(viewport, camera, WorldPos{-1.5, previewed, 0.0}, px, py));
    REQUIRE(viewport.pick(px, py, picked, error));
    CHECK(picked == k_left);

    // Release: one transaction of one command, named for the record and the axis; x and z are
    // exactly where they were, and the viewport follows it as it follows anyone's edit.
    REQUIRE(input.handle(pointer(EventKind::MouseButtonUp, gx, gy - 40.0f), false, true, document,
                         viewport, error));
    CHECK_FALSE(input.dragging());
    Vector<editor::JournalRow> journal;
    document.journal(journal, 10);
    REQUIRE(journal.size() == 2);
    CHECK(journal[0].commands == 1);
    CHECK(journal[0].rationale == "move left along y");
    CHECK(journal[0].role == "director");
    REQUIRE(translation_of(document.document(), k_left, t));
    CHECK(t[0] == -1.5);
    CHECK(t[2] == 0.0);
    CHECK(t[1] == doctest::Approx(previewed).epsilon(0.05));
    const f64 committed = t[1];
    CHECK(input.stats().drags == 1);
    CHECK(input.stats().last_axis == 1u);
    CHECK(input.stats().last_transaction == 1);
    bool rebuilt = true;
    REQUIRE_MESSAGE(viewport.sync(document.document(), rebuilt, error), error);
    CHECK_FALSE(rebuilt);
    CHECK_FALSE(viewport.previewing());
    CHECK(viewport.placement(left)->origin.y == committed);

    // Escape cancels a drag: the preview goes back and nothing is written.
    input.set_view(document, viewport, camera, 320, 200, 1.0f);
    const editor::GizmoHandles& moved = input.handles();
    REQUIRE(moved.visible);
    const f32 hx = moved.cx + (moved.ex[1] - moved.cx) * 0.6f;
    const f32 hy = moved.cy + (moved.ey[1] - moved.cy) * 0.6f;
    REQUIRE(input.handle(pointer(EventKind::MouseMove, hx, hy), false, true, document, viewport,
                         error));
    REQUIRE(input.handle(pointer(EventKind::MouseButtonDown, hx, hy), false, true, document,
                         viewport, error));
    REQUIRE(input.dragging());
    REQUIRE(input.handle(pointer(EventKind::MouseMove, hx, hy + 30.0f), false, true, document,
                         viewport, error));
    REQUIRE(input.flush(viewport, error));
    CHECK(viewport.placement(left)->origin.y < committed);
    REQUIRE(
        input.handle(key(EventKind::KeyDown, Key::Escape), false, true, document, viewport, error));
    CHECK_FALSE(input.dragging());
    CHECK_FALSE(viewport.previewing());
    CHECK(viewport.placement(left)->origin.y == committed);
    CHECK(document.document().journal().size() == 2);
    // A press and a release on a handle that move nothing write nothing either.
    REQUIRE(input.handle(pointer(EventKind::MouseButtonDown, hx, hy), false, true, document,
                         viewport, error));
    REQUIRE(input.handle(pointer(EventKind::MouseButtonUp, hx, hy), false, true, document, viewport,
                         error));
    CHECK(document.document().journal().size() == 2);

    // Ctrl+Z undoes the drag: the session steps back, and the viewport follows with a move.
    REQUIRE(input.handle(key(EventKind::KeyDown, Key::Control), false, true, document, viewport,
                         error));
    REQUIRE(input.handle(key(EventKind::KeyDown, Key::Z), false, true, document, viewport, error));
    REQUIRE(input.handle(key(EventKind::KeyUp, Key::Z), false, true, document, viewport, error));
    CHECK(input.stats().undos == 1);
    CHECK(document.session().undo_position() == 1);
    REQUIRE(translation_of(document.document(), k_left, t));
    CHECK(t[1] == 0.0);
    REQUIRE(viewport.sync(document.document(), rebuilt, error));
    CHECK_FALSE(rebuilt);
    CHECK(viewport.placement(left)->origin.y == 0.0);
    document.journal(journal, 10);
    CHECK(journal[0].undone);
    // Ctrl+Y redoes it.
    REQUIRE(input.handle(key(EventKind::KeyDown, Key::Y), false, true, document, viewport, error));
    REQUIRE(
        input.handle(key(EventKind::KeyUp, Key::Control), false, true, document, viewport, error));
    CHECK(input.stats().redos == 1);
    REQUIRE(translation_of(document.document(), k_left, t));
    CHECK(t[1] == committed);
    REQUIRE(viewport.sync(document.document(), rebuilt, error));
    CHECK_FALSE(rebuilt);
    // Without Ctrl, Z is the camera's and nothing is undone.
    REQUIRE(input.handle(key(EventKind::KeyDown, Key::Z), false, true, document, viewport, error));
    CHECK(input.stats().undos == 1);
    // Nor does Delete delete while the panels have the keyboard.
    REQUIRE(input.handle(key(EventKind::KeyDown, Key::Delete), false, false, document, viewport,
                         error));
    CHECK(document.document().exists(k_left));

    // Delete removes the selection through one transaction; the viewport rebuilds without it, and
    // undoing the delete is a rebuild that brings it back.
    REQUIRE(
        input.handle(key(EventKind::KeyDown, Key::Delete), false, true, document, viewport, error));
    CHECK(input.stats().deletes == 1);
    CHECK_FALSE(document.document().exists(k_left));
    CHECK(document.selection().is_null());
    document.journal(journal, 10);
    REQUIRE(journal.size() == 3);
    CHECK(journal[0].rationale == "delete left");
    REQUIRE(viewport.sync(document.document(), rebuilt, error));
    CHECK(rebuilt);
    CHECK(viewport.scene().records.size() == 1);
    REQUIRE(input.undo(document, viewport, error));
    CHECK(document.document().exists(k_left));
    REQUIRE(viewport.sync(document.document(), rebuilt, error));
    CHECK(rebuilt);
    CHECK(viewport.scene().records.size() == 2);
    CHECK(input.message().empty());
  }
  device.destroy();
}

TEST_CASE("engine-editor: the window's gizmo drag is one transaction") {
  const test::TempDir tmp("engine_editor_window_drag");
  // One cube at the origin, three metres across: the window's framing camera looks at it from
  // +x, a little above, so its y handle runs straight up the middle of the picture from the
  // centre, and the z handle runs left.
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
  // In window coordinates, which the editor scales to the picture's pixels as it does a real
  // pointer: the handle is 80 of them long whatever the display's scale, so a press 45 above the
  // centre is on the y handle and a release 30 higher drags it up. Small, vsynced and under the
  // editor's 120 Hz ceiling: never an unthrottled window.
  const std::vector<std::string_view> argv = {
      exe,      "--doc",   doc_dir,  "--width", "320",    "--height", "240", "--frames",  "12",
      "--pick", "160,110", "--drag", "160,75",  "160,45", "--ddc",    ddc,   "--no-shell"};
  JsonValue summary;
  std::string output;
  if (!run_editor(argv, summary, output)) return;
  CHECK(write_json(*summary.find("frames")) == "12");
  const JsonValue* selected = summary.find("selected");
  REQUIRE(selected != nullptr);
  REQUIRE_MESSAGE(selected->is_string(), output);
  CHECK(std::string(selected->as_string()) == editor::hex(k_left));
  const JsonValue* pick_ms = summary.find("pick_ms");
  REQUIRE(pick_ms != nullptr);
  CHECK(pick_ms->is_number());
  CHECK_MESSAGE(write_json(*summary.find("drags")) == "1", output);
  const JsonValue* drag = summary.find("drag");
  REQUIRE(drag != nullptr);
  REQUIRE_MESSAGE(drag->is_object(), output);
  CHECK(std::string(drag->find("axis")->as_string()) == "y");
  CHECK(std::string(drag->find("rationale")->as_string()) == "move only along y");
  CHECK(write_json(*drag->find("transaction")) == "1");
  CHECK(write_json(*summary.find("transactions")) == "2");

  // What the run left on disk, read back through a session of our own: the record moved up the y
  // axis and nowhere else, by exactly one transaction of one command after the fixture's.
  editor::EditorDocument document;
  std::string error;
  REQUIRE_MESSAGE(document.open(doc_dir, false, "editor-test", error), error);
  f64 t[3] = {};
  REQUIRE(translation_of(document.document(), k_left, t));
  CHECK(t[0] == 0.0);
  CHECK(t[2] == 0.0);
  CHECK(t[1] > 0.1);
  Vector<editor::JournalRow> journal;
  document.journal(journal, 10);
  REQUIRE(journal.size() == 2);
  CHECK(journal[0].commands == 1);
  CHECK(journal[0].rationale == "move only along y");
  CHECK(journal[0].actor == "editor");
  CHECK_FALSE(journal[0].undone);
}

TEST_CASE("engine-editor: the shell's three methods, through the editor's dispatcher") {
  const test::TempDir tmp("engine_editor_methods");
  editor::EditorDocument document;
  make_fixture(document, tmp.path());
  // The editor's host as main.cpp makes it, with a roles file whose default role may write
  // nothing: the three methods are UI state, registered read-only, and a role that may not write
  // still lays out, selects and reads.
  editor::EditorState state;
  state.document = &document;
  protocol::Policy policy;
  std::string error;
  REQUIRE_MESSAGE(policy.load_roles("roles.json", R"({"default_role":"watcher","roles":[
      {"name":"watcher","doc":"reads","methods":[],"layers":[],"tiles":[],"untiled":false,
       "object_types":[],"review_required":false}]})",
                                    error),
                  error);
  protocol::Dispatcher d(
      protocol::Context{&document.sessions(), nullptr, nullptr, &state, &policy});
  protocol::add_builtin_methods(d);
  editor::add_editor_methods(d);
  const auto call = [&](std::string_view text) {
    JsonValue response;
    const std::string out = d.dispatch_text(text);
    REQUIRE_MESSAGE(parse_json(out, response).ok, out);
    return response;
  };

  // editor.layout as the page sends it — a notification, so nothing comes back — moves the
  // recorded rectangle; as a request it says where the window will put the viewport.
  CHECK(d.dispatch_text(R"({"jsonrpc":"2.0","method":"editor.layout","params":{"x":300.5,)"
                        R"("y":48,"width":400,"height":300,"device_pixel_ratio":1.5}})")
            .empty());
  CHECK(state.layouts == 1);
  CHECK(state.layout.x == 300.5);
  CHECK(state.layout.device_pixel_ratio == 1.5);
  state.pixels_per_unit = 1.0;
  JsonValue placed =
      call(R"({"jsonrpc":"2.0","id":1,"method":"editor.layout","params":)"
           R"({"x":300.5,"y":48,"width":400,"height":300,"device_pixel_ratio":1.5}})");
  const JsonValue* r = placed.find("result");
  REQUIRE_MESSAGE(r != nullptr, write_json(placed));
  // 300.5 x 1.5 = 450.75, floored; (300.5 + 400) x 1.5 = 1050.75, ceiled: the cell is covered.
  CHECK(write_json(*r->find("x")) == "450");
  CHECK(write_json(*r->find("y")) == "72");
  CHECK(write_json(*r->find("width")) == "601");
  CHECK(write_json(*r->find("height")) == "450");
  CHECK(write_json(*r->find("layouts")) == "2");
  // A window whose coordinates are half its pixels (a display that scales) halves the rectangle.
  const editor::ViewportRect half = editor::layout_rect(state.layout, 2.0);
  CHECK(half.x == 225);
  CHECK(half.width == 301);
  // External input is validated: no negative size, no ratio of zero.
  JsonValue bad = call(R"({"jsonrpc":"2.0","id":2,"method":"editor.layout","params":)"
                       R"({"x":0,"y":0,"width":-4,"height":3,"device_pixel_ratio":0}})");
  REQUIRE(bad.find("error") != nullptr);
  CHECK(state.layouts == 2);

  // editor.select changes the selection the viewport outlines and the summary reports, though the
  // role may write nothing; a write by the same role is refused, so the policy is in force.
  JsonValue selected = call(std::string(R"({"jsonrpc":"2.0","id":3,"method":"editor.select",)") +
                            R"("params":{"id":")" + editor::hex(k_right) + R"("}})");
  REQUIRE_MESSAGE(selected.find("result") != nullptr, write_json(selected));
  CHECK(document.selection() == k_right);
  CHECK(std::string(selected.find("result")->find("selected")->as_string()) ==
        editor::hex(k_right));
  JsonValue write = call(std::string(R"({"jsonrpc":"2.0","id":4,"method":"doc.apply","params":)") +
                         R"({"session":")" + document.session().id() +
                         R"(","commands":[],"attribution":{"rationale":"nothing"}}})");
  REQUIRE(write.find("error") != nullptr);
  CHECK(write_json(*write.find("error")->find("code")) == "1008");
  // A record that is not there is refused, and the selection stays; null clears it.
  JsonValue missing = call(R"({"jsonrpc":"2.0","id":5,"method":"editor.select","params":)"
                           R"({"id":"0123456789abcdef0123456789abcdef"}})");
  REQUIRE(missing.find("error") != nullptr);
  CHECK(document.selection() == k_right);
  JsonValue cleared =
      call(R"({"jsonrpc":"2.0","id":6,"method":"editor.select","params":{"id":null}})");
  REQUIRE(cleared.find("result") != nullptr);
  CHECK(document.selection().is_null());

  // editor.state: the selection a click in the viewport made, the camera in render.capture's
  // shape (A32), the last layout and the picture's size.
  document.select(k_left);
  renderer::Camera camera = test_camera();
  state.camera = editor::framed_camera(camera);
  state.width = 640;
  state.height = 360;
  JsonValue st = call(R"({"jsonrpc":"2.0","id":7,"method":"editor.state"})");
  const JsonValue* s = st.find("result");
  REQUIRE_MESSAGE(s != nullptr, write_json(st));
  CHECK(std::string(s->find("selected")->as_string()) == editor::hex(k_left));
  CHECK(std::string(s->find("session")->as_string()) == document.session().id());
  CHECK(write_json(*s->find("layouts")) == "2");
  CHECK(write_json(*s->find("width")) == "640");
  const JsonValue* cam = s->find("camera");
  REQUIRE(cam != nullptr);
  const JsonValue* position = cam->find("position");
  REQUIRE(position != nullptr);
  REQUIRE(position->size() == 3);
  CHECK(number(&(*position)[0]) == 0.0);
  CHECK(number(&(*position)[1]) == 1.0);
  CHECK(number(&(*position)[2]) == 7.0);
  CHECK(std::fabs(number(cam->find("fov_deg")) - 55.0) < 1e-4);
  // Looking from +z, a little above, at the origin: yaw zero, pitch below the horizon.
  CHECK(std::fabs(number(cam->find("yaw_deg"))) < 1e-4);
  CHECK(number(cam->find("pitch_deg")) < 0.0);

  // The three are the editor's: a dispatcher with only the built-in methods (engine-host's) does
  // not have them.
  protocol::Dispatcher plain(
      protocol::Context{&document.sessions(), nullptr, nullptr, nullptr, nullptr});
  protocol::add_builtin_methods(plain);
  JsonValue unknown;
  REQUIRE(parse_json(plain.dispatch_text(R"({"jsonrpc":"2.0","id":8,"method":"editor.state"})"),
                     unknown)
              .ok);
  REQUIRE(unknown.find("error") != nullptr);
  CHECK(write_json(*unknown.find("error")->find("code")) == "-32601");
}

TEST_CASE("engine-editor: --embedded puts the shell beside the viewport, and the page places it") {
  const test::TempDir tmp("engine_editor_embedded");
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
  // Small enough for a shared desktop and wide enough for the shell's three columns; vsynced and
  // under the editor's ceiling. The listener starts, the web view loads the shell from it, the
  // page's first editor.layout places the viewport's child window, and the pick at the middle of
  // the child's picture finds the cube. Without the WebView2 runtime (a hosted runner) the editor
  // exits 3 saying so, and this skips.
  const std::vector<std::string_view> argv = {
      exe,        "--doc", doc_dir,  "--width", "900",   "--height", "560",
      "--frames", "8",     "--pick", "center",  "--ddc", ddc,        "--embedded"};
  JsonValue summary;
  std::string output;
  if (!run_editor(argv, summary, output)) return;
  // The announcement is the first line on stdout (the log's lines on stderr are merged in here):
  // the listener's URL, its token and the shell's address.
  const usize at = output.find("{\"listen\":");
  REQUIRE_MESSAGE(at != std::string::npos, output);
  JsonValue announcement;
  REQUIRE_MESSAGE(parse_json(output.substr(at, output.find('\n', at) - at), announcement).ok,
                  output);
  REQUIRE(announcement.find("shell") != nullptr);
  CHECK(std::string(announcement.find("listen")->as_string()).starts_with("ws://127.0.0.1:"));
  CHECK(write_json(*summary.find("embedded")) == "true");
  CHECK(summary.find("listen")->is_number());
  CHECK_MESSAGE(number(summary.find("layouts")) >= 1.0, output);
  const JsonValue* rect = summary.find("viewport");
  const bool four = rect != nullptr && rect->is_array() && rect->size() == 4;
  REQUIRE_MESSAGE(four, output);
  CHECK(number(&(*rect)[2]) > 16.0);
  CHECK(number(&(*rect)[3]) > 16.0);
  // The picture is the child's, not the window's: narrower than the 900 the window is.
  CHECK(number(summary.find("width")) < 900.0);
  // Eight frames after the page last moved the viewport; the page may move it more than once as
  // it lays itself out, so the run draws at least eight.
  CHECK(number(summary.find("frames")) >= 8.0);
  const JsonValue* selected = summary.find("selected");
  const bool named = selected != nullptr && selected->is_string();
  REQUIRE_MESSAGE(named, output);
  CHECK(std::string(selected->as_string()) == editor::hex(k_left));
}

TEST_CASE("engine-editor: a change of the display's scale moves the child and the pick scale") {
  // No window and no device: the editor's dispatcher fed a synthetic change of the page's ratio,
  // as `ScaleChanged` and the page's answer to `editor.relayout` bring one, and the placement and
  // the pointer's scale computed from what it recorded, as the frame loop computes them.
  const test::TempDir tmp("engine_editor_dpi");
  editor::EditorDocument document;
  make_fixture(document, tmp.path());
  editor::EditorState state;
  state.document = &document;
  protocol::Dispatcher d(
      protocol::Context{&document.sessions(), nullptr, nullptr, &state, nullptr});
  protocol::add_builtin_methods(d);
  editor::add_editor_methods(d);
  const auto layout = [&](f64 ratio, bool narrow) {
    const std::string text =
        std::string(R"({"jsonrpc":"2.0","method":"editor.layout","params":{"x":300,"y":48,)") +
        R"("width":400,"height":300,"device_pixel_ratio":)" + std::to_string(ratio) +
        R"(,"narrow":)" + (narrow ? "true" : "false") + "}}";
    CHECK(d.dispatch_text(text).empty());
  };
  // The child's pointer scale as main.cpp reads it each frame: its size in the window's
  // coordinates against the swapchain's pixels, which are the window's coordinates times its
  // pixels per unit.
  const auto pick_scale = [](const editor::ViewportRect& r, f64 pixels_per_unit) {
    return editor::pointer_scale(r.width, r.height,
                                 static_cast<u32>(std::lround(r.width * pixels_per_unit)),
                                 static_cast<u32>(std::lround(r.height * pixels_per_unit)));
  };

  // **Windows' model**: the window's coordinates are pixels at every scale (pixels per unit 1). At
  // 100 % the cell is where the page says; the display goes to 150 %, the page answers with the
  // same CSS rectangle at ratio 1.5, and the child moves and grows by half.
  layout(1.0, false);
  REQUIRE(state.layouts == 1);
  const editor::Placement before = editor::place_viewport(state.layout, 1.0, 1600, 900);
  CHECK_FALSE(before.clamped);
  CHECK(before.rect == editor::ViewportRect{300, 48, 400, 300});
  // Between ScaleChanged and the page's answer the editor places the child by the scaled ratio,
  // and the page's answer lands on the same rectangle.
  const protocol::EditorLayoutParams interim = editor::rescaled_layout(state.layout, 1.0, 1.5);
  CHECK(interim.device_pixel_ratio == doctest::Approx(1.5));
  layout(1.5, false);
  REQUIRE(state.layouts == 2);
  const editor::Placement after = editor::place_viewport(state.layout, 1.0, 2400, 1350);
  CHECK(after.rect == editor::ViewportRect{450, 72, 600, 450});
  CHECK(editor::place_viewport(interim, 1.0, 2400, 1350).rect == after.rect);
  CHECK(pick_scale(after.rect, 1.0).x == doctest::Approx(1.0f));

  // **A platform whose coordinates are points** (pixels per unit follows the display, as on a
  // Mac): the same change keeps the child's rectangle in the window's coordinates and doubles
  // what one of them is in pixels, so the pick scale moves from 1 to 1.5 and a click still lands
  // on the pixel under it.
  const editor::Placement points_before = editor::place_viewport(state.layout, 1.5, 1600, 900);
  layout(1.0, false);
  const editor::Placement points_at_1 = editor::place_viewport(state.layout, 1.0, 1600, 900);
  CHECK(points_at_1.rect == points_before.rect);
  CHECK(pick_scale(points_at_1.rect, 1.0).x == doctest::Approx(1.0f));
  CHECK(pick_scale(points_before.rect, 1.5).x == doctest::Approx(1.5f));
  CHECK(pick_scale(points_before.rect, 1.5).y == doctest::Approx(1.5f));

  // The narrow layout's flag is recorded with the rectangle.
  layout(1.5, true);
  CHECK(state.layout.narrow);

  // **The clamp.** A cell past the window's client area is cut to it and says so; a cell whose
  // far edge is the window's (853.33 CSS pixels at 1.5 is 1280 pixels, not 1281) is not.
  protocol::EditorLayoutParams wide;
  wide.x = 284.6666666666667;
  wide.width = 568.6666666666666;
  wide.height = 300;
  wide.device_pixel_ratio = 1.5;
  const editor::Placement edge = editor::place_viewport(wide, 1.0, 1280, 720);
  CHECK_FALSE(edge.clamped);
  CHECK(edge.rect.x + static_cast<i32>(edge.rect.width) == 1280);
  // The page's viewport is whole CSS pixels (854 of them in a 1280-pixel window at 1.5), so a
  // cell at its edge reaches a pixel past the window: cut, and not called a clamp.
  wide.width = 569.3333333333333;
  const editor::Placement rounded = editor::place_viewport(wide, 1.0, 1280, 720);
  CHECK_FALSE(rounded.clamped);
  CHECK(rounded.rect.x + static_cast<i32>(rounded.rect.width) == 1280);
  wide.width = 900.0;
  const editor::Placement cut = editor::place_viewport(wide, 1.0, 1280, 720);
  CHECK(cut.clamped);
  CHECK(cut.rect.x + static_cast<i32>(cut.rect.width) == 1280);
  CHECK(editor::place_viewport(wide, 1.0, 0, 0).rect.width > 1280);  // no client size: no clamp

  // `editor.relayout` is a notification (no id) the page answers with a layout.
  JsonValue relayout;
  REQUIRE(parse_json(editor::relayout_notification("display scale"), relayout).ok);
  CHECK(std::string(relayout.find("method")->as_string()) == "editor.relayout");
  CHECK(relayout.find("id") == nullptr);
  CHECK(std::string(relayout.find("params")->find("reason")->as_string()) == "display scale");

  // The test page's channel exists only where --test-page registered it.
  JsonValue refused;
  REQUIRE(parse_json(d.dispatch_text(R"({"jsonrpc":"2.0","id":1,"method":"editor.test_report",)"
                                     R"("params":{"name":"x","ok":true,"detail":""}})"),
                     refused)
              .ok);
  REQUIRE(refused.find("error") != nullptr);
  CHECK(write_json(*refused.find("error")->find("code")) == "-32601");
  editor::add_editor_test_methods(d);
  CHECK(d.dispatch_text(R"({"jsonrpc":"2.0","method":"editor.test_report",)"
                        R"("params":{"name":"enum","ok":true,"detail":"{}"}})")
            .empty());
  REQUIRE(state.reports.size() == 1);
  CHECK(state.reports[0].name == "enum");
  CHECK(state.reports[0].ok);
}

namespace {

// The embedded runs' document: one cube three metres across at the origin, and with `provenance`
// an AssetProvenance record, whose `license_class` is an enum the properties panel lists.
std::string embedded_fixture(const test::TempDir& tmp, bool provenance) {
  editor::EditorDocument document;
  std::string error;
  REQUIRE(write_cube_glb(tmp.path() + "/cube.glb"));
  REQUIRE_MESSAGE(document.open(tmp.path(), true, "editor-test", error), error);
  Vector<doc::Command> commands;
  commands.push_back(doc::cmd_create(
      k_left, k_placement, Id128{},
      json(R"({"name":"only","mesh":"cube.glb","translation":[0,0,0],"scale":[3,3,3]})")));
  if (provenance) {
    commands.push_back(doc::cmd_create(
        k_yard, "engine.content.AssetProvenance", Id128{},
        json(R"({"generator":"hand","license":"CC0-1.0","license_class":"Permissive",)"
             R"("operator_id":"00000000000000200000000000000009"})")));
  }
  REQUIRE_MESSAGE(document.apply(std::span<const doc::Command>(commands.data(), commands.size()),
                                 "the fixture", error),
                  error);
  return tmp.path();
}

const JsonValue* page_report(const JsonValue& summary, std::string_view name) {
  const JsonValue* page = summary.find("page");
  if (page == nullptr || !page->is_array()) return nullptr;
  for (u32 i = 0; i < page->size(); ++i) {
    const JsonValue* n = (*page)[i].find("name");
    if (n != nullptr && n->is_string() && std::string_view(n->as_string()) == name)
      return &(*page)[i];
  }
  return nullptr;
}

bool report_ok(const JsonValue* report) {
  if (report == nullptr) return false;
  const JsonValue* ok = report->find("ok");
  return ok != nullptr && write_json(*ok) == "true";
}

}  // namespace

TEST_CASE("engine-editor: the page's narrow layout keeps the viewport inside a resized window") {
  const test::TempDir tmp("engine_editor_layout");
  const std::string doc_dir = embedded_fixture(tmp, false);
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  const std::string ddc = tmp.file("ddc");
  // 1280 x 720, then 800 x 600 at the fourth frame: whatever the display's scale, every layout
  // the page sends must put the child inside the client area without the editor's clamp, and
  // the narrow layout must be on exactly when the page is under 960 CSS pixels wide. Vsynced and
  // under the editor's ceiling; the profile is inside this test's directory (<doc>/.editor).
  const std::vector<std::string_view> argv = {
      exe,        "--doc", doc_dir, "--width", "1280",        "--height", "720",
      "--frames", "8",     "--ddc", ddc,       "--resize-to", "800,600",  "--embedded"};
  JsonValue summary;
  std::string output;
  if (!run_editor(argv, summary, output)) return;
  const f64 scale = number(summary.find("display_scale"));
  REQUIRE_MESSAGE(scale > 0.0, output);
  const JsonValue* placements = summary.find("placements");
  REQUIRE_MESSAGE((placements != nullptr && placements->is_array()), output);
  bool large = false;
  bool small = false;
  for (u32 i = 0; i < placements->size(); ++i) {
    const JsonValue& p = (*placements)[i];
    const JsonValue* client = p.find("client");
    const JsonValue* rect = p.find("viewport");
    REQUIRE((client != nullptr && rect != nullptr && client->size() == 2 && rect->size() == 4));
    const f64 cw = number(&(*client)[0]);
    const f64 ch = number(&(*client)[1]);
    const f64 x = number(&(*rect)[0]);
    const f64 y = number(&(*rect)[1]);
    // The page's own layouts never need the clamp; the editor's re-placement of the old layout in
    // the resized window, before the page answers, is what the clamp is for.
    const bool from_page = write_json(*p.find("from_page")) == "true";
    if (from_page) CHECK_MESSAGE(write_json(*p.find("clamped")) == "false", output);
    CHECK(x >= 0.0);
    CHECK(y >= 0.0);
    CHECK(x + number(&(*rect)[2]) <= cw);
    CHECK(y + number(&(*rect)[3]) <= ch);
    // The page decides by its own width in CSS pixels: the client's over the display's scale.
    const bool narrow = cw / scale < 960.0;
    if (from_page)
      CHECK_MESSAGE(write_json(*p.find("narrow")) == (narrow ? "true" : "false"), output);
    large = large || (from_page && cw == 1280.0);
    small = small || (from_page && cw == 800.0);
  }
  CHECK_MESSAGE(large, output);
  CHECK_MESSAGE(small, output);
  // The web view's profile is the document's, not %APPDATA%'s, and it ignores itself.
  std::string ignore;
  CHECK(io::read_file(doc_dir + "/.editor/.gitignore", ignore) == io::Status::Ok);
  CHECK(ignore.find('*') != std::string::npos);
  std::error_code ec;
  CHECK(std::filesystem::is_directory(doc_dir + "/.editor/webview", ec));
}

TEST_CASE("engine-editor: the viewport's keys, the page's keys, and the look gesture") {
  const test::TempDir tmp("engine_editor_keys");
  const std::string doc_dir = embedded_fixture(tmp, false);
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  const std::string ddc = tmp.file("ddc");
  // The pick selects the cube and --move commits a transaction; the look gesture (frames 3 to 7)
  // turns the camera through the adopted child's own events; then a click posted to the child
  // through the platform's queue gives it the keyboard, Ctrl+Z posted to whoever has the
  // keyboard undoes the move, the page is given the keyboard, and Ctrl+Z posted again must not
  // reach SDL. Nothing is sent to the desktop's own foreground window.
  const std::vector<std::string_view> argv = {
      exe,        "--doc",       doc_dir,  "--width", "900",       "--height", "560",
      "--frames", "34",          "--pick", "center",  "--move",    "0,0.25,0", "--look",
      "90,-45",   "--test-keys", "--ddc",  ddc,       "--embedded"};
  JsonValue summary;
  std::string output;
  if (!run_editor(argv, summary, output)) return;
  CHECK(write_json(*summary.find("moved")) == "true");
  const JsonValue* look = summary.find("look");
  REQUIRE_MESSAGE((look != nullptr && look->is_object()), output);
  // Pointer right turns the camera right (yaw falls) and pointer up raises it (pitch rises), as
  // engine-view flies; the amounts are the fly camera's sensitivity's.
  CHECK_MESSAGE(number(look->find("yaw_deg")) != 0.0, output);
  CHECK_MESSAGE(number(look->find("pitch_deg")) != 0.0, output);
  // Asked for; in effect only where the editor's window is the platform's foreground window, which
  // a test cannot arrange (window.md, "Relative mode on an adopted child").
  CHECK_MESSAGE(write_json(*look->find("relative_asked")) == "true", output);
  MESSAGE("relative mode in effect: " << write_json(*look->find("relative")));
  const JsonValue* keys = summary.find("keys");
  REQUIRE_MESSAGE((keys != nullptr && keys->is_object()), output);
  CHECK_MESSAGE(write_json(*keys->find("viewport_focus")) == "true", output);
  CHECK_MESSAGE(write_json(*keys->find("posted_to_viewport")) == "true", output);
  // Ctrl+Z in the viewport undid the move: one undo, the editor's.
  CHECK_MESSAGE(write_json(*keys->find("undos_from_viewport")) == "1", output);
  // With the page holding the keyboard SDL saw no key at all, and the editor undid nothing more.
  CHECK_MESSAGE(write_json(*keys->find("page_focus")) == "true", output);
  CHECK_MESSAGE(write_json(*keys->find("sdl_keys_from_page")) == "0", output);
  CHECK_MESSAGE(write_json(*keys->find("undos_after_page")) == "1", output);
  // Whether the page itself acted on the posted keys is the browser's affair; say what happened.
  MESSAGE("page: posted " << write_json(*keys->find("posted_to_page")) << ", undo position "
                          << write_json(*keys->find("undo_position_before_page")) << " -> "
                          << write_json(*keys->find("undo_position_after_page")));
}

TEST_CASE("engine-editor: the look gesture through the platform's queue turns the camera") {
  const test::TempDir tmp("engine_editor_look_os");
  const std::string doc_dir = embedded_fixture(tmp, false);
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  const std::string ddc = tmp.file("ddc");
  // The right button, three moves and the release posted to the adopted child through the
  // platform's queue (window procedure, SDL's translation), never to the desktop's foreground
  // window. Before 2026-10-12 the child carried SDL's relative flag itself, which SDL honours only
  // for its keyboard focus (a top-level window) and which made it drop every absolute motion of
  // the child: the camera did not turn at all, here or under a person's mouse.
  const std::vector<std::string_view> argv = {
      exe,  "--doc", doc_dir, "--width",        "900",  "--height",  "560", "--frames",
      "14", "--ddc", ddc,     "--test-look-os", "90,0", "--embedded"};
  JsonValue summary;
  std::string output;
  if (!run_editor(argv, summary, output)) return;
  const JsonValue* look = summary.find("look_os");
  REQUIRE_MESSAGE((look != nullptr && look->is_object()), output);
  CHECK_MESSAGE(write_json(*look->find("asked")) == "true", output);
  // SDL's own flag never sits on the child: it is asked through the parent.
  CHECK_MESSAGE(write_json(*look->find("child_flag")) == "false", output);
  const f64 expected = number(look->find("expected_yaw_deg"));
  CHECK(expected < -1.0);  // pointer right turns right: yaw falls
  if (write_json(*look->find("in_effect")) == "false") {
    // Not the foreground window: the posted moves are the absolute pointer's, and they turn the
    // camera by exactly the fly camera's sensitivity.
    CHECK_MESSAGE(number(look->find("yaw_deg")) == doctest::Approx(expected).epsilon(1.0e-3),
                  output);
  } else {
    // In effect, SDL reads the device and a posted move is not one; what is checked is that the
    // hidden cursor was held inside the viewport, where a release is the viewport's.
    CHECK_MESSAGE(write_json(*look->find("clip_inside")) == "true", output);
    CHECK_MESSAGE(write_json(*look->find("cursor_inside")) == "true", output);
    MESSAGE(
        "relative mode was in effect (the editor's window was in front): the posted moves "
        "were not read, by design; turned "
        << number(look->find("yaw_deg")) << " degrees");
  }
}

TEST_CASE("engine-editor: the embedded page keeps its popups off the viewport, and reconnects") {
  const test::TempDir tmp("engine_editor_page");
  const std::string doc_dir = embedded_fixture(tmp, true);
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  const std::string ddc = tmp.file("ddc");
  // The page's test hook (?test=1, only with --test-page) opens the command line's completions
  // and the properties panel's enum list by DOM events and reports whether each stays in its panel
  // and off the cell; then the editor sends editor.relayout (the page answers with a layout) and
  // shuts every connection under the page, which must show its pill, reconnect and say so, while
  // the viewport goes on drawing.
  const std::vector<std::string_view> argv = {exe,
                                              "--doc",
                                              doc_dir,
                                              "--width",
                                              "900",
                                              "--height",
                                              "560",
                                              "--frames",
                                              "40",
                                              "--ddc",
                                              ddc,
                                              "--test-page",
                                              "--test-relayout-at",
                                              "10",
                                              "--test-drop-socket-at",
                                              "20",
                                              "--embedded"};
  JsonValue summary;
  std::string output;
  if (!run_editor(argv, summary, output)) return;
  CHECK_MESSAGE(report_ok(page_report(summary, "completions")), output);
  CHECK_MESSAGE(report_ok(page_report(summary, "enum")), output);
  CHECK_MESSAGE(report_ok(page_report(summary, "done")), output);
  // editor.relayout reached the page, which answered with a layout.
  CHECK(write_json(*summary.find("relayouts")) == "1");
  CHECK_MESSAGE(number(summary.find("relayout_reached")) >= 1.0, output);
  CHECK_MESSAGE(number(summary.find("layouts")) > number(summary.find("layouts_at_relayout")),
                output);
  // The drop: the pill said so, the page came back, and frames went on being drawn throughout.
  CHECK_MESSAGE(number(summary.find("drops")) >= 1.0, output);
  CHECK_MESSAGE(report_ok(page_report(summary, "reconnect")), output);
  const f64 dropped_at = number(summary.find("frames_at_drop"));
  const f64 back_at = number(summary.find("frames_at_reconnect"));
  CHECK_MESSAGE(dropped_at > 0.0, output);
  CHECK_MESSAGE(back_at > dropped_at, output);
  CHECK_MESSAGE(number(summary.find("frames")) > back_at, output);
}

TEST_CASE("engine-editor: --version prints the build stamp with no window") {
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  const std::vector<std::string_view> argv = {exe, "--version"};
  platform::Process p;
  std::string error;
  REQUIRE_MESSAGE(p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
                          /*merge_stderr=*/true),
                  error);
  p.close_stdin();
  std::string output;
  p.read_all(output);
  REQUIRE_MESSAGE(p.wait() == 0, output);
  JsonValue stamp;
  REQUIRE_MESSAGE(parse_json(output, stamp).ok, output);
  CHECK(std::string(stamp.find("tool")->as_string()) == "engine-editor");
  CHECK(stamp.find("commit")->is_string());
  CHECK(stamp.find("dirty")->is_bool());
}
