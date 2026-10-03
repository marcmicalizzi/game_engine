// One request, two hosts (docs/subsystems/renderer.md, "One request, two hosts"; request.h).
//
// The point of this file is the first case: every field of `renderer::RenderSettings` and of
// `renderer::FrameDesc` is named here, with the protocol field that reaches it or the reason none
// does, and the structured bindings below stop compiling the day either struct grows a field. So
// a setting that lands with an engine-view flag and nothing else fails the build here, and whoever
// adds it reads why: the sky's hour, the exposure, the ground's time and the ground's layout all
// landed in one host only before this test existed. Then every field of the wire's
// `engine.protocol.RenderSettings` (by the schema's own reflection) is set to something that is
// not its default and read through `read_protocol_settings`, and the renderer field it names has
// to change — a field decoded into a struct nobody reads fails here too. No device.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <systems/renderer/request.h>

#include <doctest/doctest.h>

#include <cstring>
#include <iterator>
#include <schemas/protocol.h>
#include <string>
#include <string_view>

using namespace engine;

namespace {

using renderer::RenderRequest;

// Where one renderer field comes from on the wire: the `engine.protocol.RenderSettings` field that
// reaches it, or null and the reason none does. `differs` compares the field in two requests.
struct Row {
  const char* field;
  const char* wire;
  const char* reason;
  bool (*differs)(const RenderRequest& a, const RenderRequest& b);
};

#define ENGINE_ROW(name, wire, reason)                                        \
  Row {                                                                       \
    #name, wire, reason, [](const RenderRequest& a, const RenderRequest& b) { \
      return !(a.settings.name == b.settings.name);                           \
    }                                                                         \
  }

// Every field of `RenderSettings`, in declaration order.
const Row k_settings_rows[] = {
    ENGINE_ROW(raster, "raster", ""),
    ENGINE_ROW(shadows, "shadows", ""),
    ENGINE_ROW(view_mode, "view", ""),
    ENGINE_ROW(lod_px, "lod_px", ""),
    ENGINE_ROW(sw_px, "sw_px", ""),
    ENGINE_ROW(cull, "cull", ""),
    ENGINE_ROW(occlusion, "occlusion", ""),
    ENGINE_ROW(cone, "cone", ""),
    ENGINE_ROW(shadow_casters, "shadow_casters", ""),
    ENGINE_ROW(shadow_cascades, "shadow_cascades", ""),
    ENGINE_ROW(shadow_map, "shadow_map", ""),
    ENGINE_ROW(shadow_distance, "shadow_distance", ""),
    ENGINE_ROW(shadow_frustum, nullptr,
               "the switch for the maps' 'culling never changes the picture' test; no flag"),
    ENGINE_ROW(shadow_lod_scale, nullptr,
               "a measurement's knob for the maps' own LOD question; no flag"),
    ENGINE_ROW(shadow_normal_offset, nullptr,
               "the maps' lookup offset, a tests' knob; no flag (k_shadow_normal_offset_texels)"),
    ENGINE_ROW(lights, "lights", ""),
    ENGINE_ROW(orbit_lights, "orbit_lights", ""),
    ENGINE_ROW(sun_azimuth_deg, "sun_azimuth_deg", ""),
    ENGINE_ROW(sun_elevation_deg, "sun_elevation_deg", ""),
    ENGINE_ROW(exposure_ev, "exposure_ev", ""),
    ENGINE_ROW(exposure_ev100, "exposure_ev100", ""),
    ENGINE_ROW(deform, "deform", ""),
    ENGINE_ROW(deform_kind, "deform", ""),
    ENGINE_ROW(deform_amplitude, "deform_amplitude", ""),
    ENGINE_ROW(deform_pool_kib, "deform_pool_mib", ""),
    // Resolved against the scene's channels (`settings_for`); the wire carries the request.
    Row{"morph_static_weights", "morph", "",
        [](const RenderRequest& a, const RenderRequest& b) { return !(a.morph == b.morph); }},
    ENGINE_ROW(static_shape_kib, "static_shape_kib", ""),
    ENGINE_ROW(rt_templates, "rt_templates", ""),
    ENGINE_ROW(rt_budget_mib, "rt_budget_mib", ""),
    ENGINE_ROW(share_textures, "share_textures", ""),
    ENGINE_ROW(time_rate, "time_rate", ""),
    ENGINE_ROW(time_rate_live, nullptr,
               "a window's: the levels exist so the , and . keys can set the rate going; a call "
               "has no keys, and its rate is time_rate"),
    ENGINE_ROW(terrain_rings, "terrain_rings", ""),
    ENGINE_ROW(terrain_tiles, "terrain_tiles", ""),
    ENGINE_ROW(terrain_far_levels, "terrain_far_levels", ""),
    ENGINE_ROW(stream, "stream", ""),
    ENGINE_ROW(page_budget_bytes, "page_budget_mib", ""),
    ENGINE_ROW(upload_budget_bytes, "upload_budget_kib", ""),
    ENGINE_ROW(views, "views", ""),
    ENGINE_ROW(side_yaw, "side_yaw_deg", ""),
    ENGINE_ROW(panini_d, "panini_d", ""),
    ENGINE_ROW(peripheral_lod, "peripheral_lod", ""),
};
#undef ENGINE_ROW

// The wire's fields that reach the request beside its settings.
const Row k_request_rows[] = {
    Row{"page_budget_pct", "page_budget_pct", "",
        [](const RenderRequest& a, const RenderRequest& b) {
          return a.page_budget_pct != b.page_budget_pct;
        }},
};

// A value that is not the default, for every field of the wire's `RenderSettings`. A field the
// schema grows without one here fails the reverse check below.
struct WireValue {
  const char* field;
  const char* json;  // the object the field is set by, with a companion where one is required
};
const WireValue k_wire_values[] = {
    {"raster", R"({"raster":"rt"})"},
    {"shadows", R"({"shadows":"csm"})"},
    {"view", R"({"view":"normals"})"},
    {"lod_px", R"({"lod_px":2})"},
    {"sw_px", R"({"sw_px":16})"},
    {"cull", R"({"cull":false})"},
    {"occlusion", R"({"occlusion":false})"},
    {"cone", R"({"cone":false})"},
    {"lights", R"({"lights":false})"},
    {"orbit_lights", R"({"orbit_lights":true})"},
    {"sun_azimuth_deg", R"({"sun_azimuth_deg":10,"sun_elevation_deg":20})"},
    {"sun_elevation_deg", R"({"sun_azimuth_deg":10,"sun_elevation_deg":20})"},
    {"deform", R"({"deform":"wave"})"},
    {"deform_amplitude", R"({"deform_amplitude":0.5})"},
    {"rt_templates", R"({"rt_templates":true})"},
    {"views", R"({"views":"surround3"})"},
    {"side_yaw_deg", R"({"side_yaw_deg":10})"},
    {"panini_d", R"({"panini_d":0.5})"},
    {"peripheral_lod", R"({"peripheral_lod":2})"},
    {"stream", R"({"stream":true})"},
    {"page_budget_mib", R"({"page_budget_mib":64})"},
    {"upload_budget_kib", R"({"upload_budget_kib":512})"},
    {"shadow_casters", R"({"shadow_casters":false})"},
    {"rt_budget_mib", R"({"rt_budget_mib":256})"},
    {"shadow_cascades", R"({"shadow_cascades":2})"},
    {"shadow_map", R"({"shadow_map":1024})"},
    {"shadow_distance", R"({"shadow_distance":100})"},
    {"share_textures", R"({"share_textures":false})"},
    {"time_rate", R"({"time_rate":86400})"},
    {"exposure_ev", R"({"exposure_ev":1.5})"},
    {"exposure_ev100", R"({"exposure_ev100":12})"},
    {"terrain_rings", R"({"terrain_rings":true})"},
    {"terrain_tiles", R"({"terrain_tiles":true})"},
    {"terrain_far_levels", R"({"terrain_far_levels":2})"},
    {"deform_pool_mib", R"({"deform_pool_mib":16})"},
    {"static_shape_kib", R"({"static_shape_kib":1024})"},
    {"morph", R"({"morph":["0=0.5"]})"},
    {"page_budget_pct", R"({"page_budget_pct":25})"},
};

bool read_wire(std::string_view text, protocol::RenderSettings& out) {
  JsonValue json;
  if (!parse_json(text, json).ok) return false;
  schema::ReadContext ctx;
  return schema::from_json(out, json, ctx) && ctx.ok();
}

// Every field of `FrameDesc`, in declaration order: where a call says it, or why it does not.
struct FrameRow {
  const char* field;
  const char* source;
};
const FrameRow k_frame_rows[] = {
    {"camera", "camera, orbit, or a camera_path's frame"},
    {"frame_index", "frame (capture, evaluate), the frame's number (benchmark)"},
    {"sun_time_s", "clock.time_of_day and clock.sun_rate, through renderer::frame_clock"},
    {"exposure",
     "a window's - = and 0 keys; a call asks through settings.exposure_ev and "
     "settings.exposure_ev100, which the frame's request adds to"},
    {"view_mode", "the renderer's own override (the normals channel); a call's is settings.view"},
    {"lod_px",
     "the reference renderer's own override (its finest cut); a call's is settings.lod_px"},
    {"rt_complete", "the reference renderer's own"},
    {"color", "a presenting host's swapchain image; render.* draws into the renderer's own target"},
    {"final_layout", "a presenting host's"},
    {"wait", "a presenting host's"},
    {"signal", "a presenting host's"},
    {"joints", "the animation capability's (engine-view --animate); render.* draws the bind pose"},
    {"instance_joints", "the animation capability's, beside joints"},
    {"morph_weights",
     "the pose stage's (engine-view --morph-animate, the animation capability's); "
     "a call's static weights are settings.morph"},
};

}  // namespace

TEST_CASE("request: every RenderSettings and FrameDesc field says how a request reaches it") {
  // **Adding a field to `RenderSettings` stops this line compiling.** Name it in
  // `k_settings_rows` with the `engine.protocol.RenderSettings` field that reaches it — add that
  // field to schemas/protocol.schema and read it in `read_protocol_settings` — or with the reason a
  // call has no business asking for it; then put the binding's count up by one.
  renderer::RenderSettings settings;
  [[maybe_unused]] auto& [s00, s01, s02, s03, s04, s05, s06, s07, s08, s09, s10, s11, s12, s13, s14,
                          s15, s16, s17, s18, s19, s20, s21, s22, s23, s24, s25, s26, s27, s28, s29,
                          s30, s31, s32, s33, s34, s35, s36, s37, s38, s39, s40, s41] = settings;
  static_assert(std::size(k_settings_rows) == 42, "one row per RenderSettings field");
  // The same for `FrameDesc`: a new per-frame input is a call's parameter or a host's own, and
  // the row says which.
  renderer::FrameDesc frame;
  [[maybe_unused]] auto& [f00, f01, f02, f03, f04, f05, f06, f07, f08, f09, f10, f11, f12, f13] =
      frame;
  static_assert(std::size(k_frame_rows) == 14, "one row per FrameDesc field");
  for (const FrameRow& row : k_frame_rows) {
    CHECK_MESSAGE(std::strlen(row.source) > 0, row.field);
  }

  const schema::TypeInfo& wire = schema::type_of<protocol::RenderSettings>();
  for (const Row& row : k_settings_rows) {
    INFO("renderer field " << row.field);
    if (row.wire == nullptr) {
      CHECK(std::strlen(row.reason) > 0);
    } else {
      CHECK(wire.find_field(row.wire) != nullptr);
    }
  }

  // Every wire field, set to something that is not its default, changes what it names.
  const RenderRequest base = [] {
    RenderRequest r;
    std::string error;
    REQUIRE(renderer::read_protocol_settings(protocol::RenderSettings{}, r, error));
    return r;
  }();
  for (const schema::FieldInfo& field : wire.fields) {
    INFO("wire field " << field.name);
    const WireValue* value = nullptr;
    for (const WireValue& v : k_wire_values)
      if (std::string_view(v.field) == field.name) value = &v;
    REQUIRE_MESSAGE(value != nullptr, "give the new wire field a value in k_wire_values");
    protocol::RenderSettings in;
    REQUIRE(read_wire(value->json, in));
    RenderRequest read;
    std::string error;
    REQUIRE_MESSAGE(renderer::read_protocol_settings(in, read, error), error);
    bool named = false;
    bool reached = false;
    for (const Row& row : k_settings_rows) {
      if (row.wire == nullptr || std::string_view(row.wire) != field.name) continue;
      named = true;
      reached = reached || row.differs(read, base);
    }
    for (const Row& row : k_request_rows) {
      if (std::string_view(row.wire) != field.name) continue;
      named = true;
      reached = reached || row.differs(read, base);
    }
    CHECK_MESSAGE(named, "no renderer field names this wire field");
    CHECK_MESSAGE(reached, "the field is read into nothing");
  }
}

TEST_CASE("request: a wire request that names nothing is the renderer's defaults, exactly") {
  // What keeps a call that names none of the new fields drawing the bytes it drew: the defaults
  // of the wire's settings are `RenderSettings{}` field for field, and the clock is still.
  RenderRequest request;
  std::string error;
  REQUIRE(renderer::read_protocol_settings(protocol::RenderSettings{}, request, error));
  CHECK(request.settings == renderer::RenderSettings{});
  CHECK(request.page_budget_pct == 0);
  CHECK(request.morph.empty());
  REQUIRE(renderer::read_protocol_clock(protocol::RenderClock{}, request.clock, error));
  CHECK(!request.clock.time_of_day_h.has_value());
  CHECK(!request.clock.sun_rate.has_value());
  // And an empty JSON object reads as the defaults: what an agent's `{}` is.
  protocol::RenderSettings empty;
  REQUIRE(read_wire("{}", empty));
  RenderRequest from_empty;
  REQUIRE(renderer::read_protocol_settings(empty, from_empty, error));
  CHECK(from_empty.settings == renderer::RenderSettings{});
}

TEST_CASE("request: the wire's refusals") {
  const auto refused = [](std::string_view json) {
    protocol::RenderSettings in;
    REQUIRE(read_wire(json, in));
    RenderRequest out;
    std::string error;
    const bool ok = renderer::read_protocol_settings(in, out, error);
    CHECK(!error.empty() == !ok);
    return !ok;
  };
  CHECK(refused(R"({"raster":"fast"})"));
  CHECK(refused(R"({"exposure_ev":31})"));
  CHECK(refused(R"({"exposure_ev100":-31})"));
  CHECK(refused(R"({"page_budget_pct":101})"));
  CHECK(refused(R"({"deform_pool_mib":70000})"));
  CHECK(refused(R"({"morph":["smile"]})"));
  CHECK(refused(R"({"sun_azimuth_deg":10})"));
  CHECK(refused(R"({"terrain_far_levels":9})"));
  CHECK(!refused(R"({"terrain_far_levels":0})"));
  CHECK(!refused(R"({"exposure_ev":-2.5,"exposure_ev100":-3})"));

  const auto clock_refused = [](std::optional<f64> hour, std::optional<f64> rate) {
    protocol::RenderClock in;
    in.time_of_day = hour;
    in.sun_rate = rate;
    renderer::ClockRequest out;
    std::string error;
    return !renderer::read_protocol_clock(in, out, error);
  };
  CHECK(clock_refused(24.0, std::nullopt));
  CHECK(clock_refused(-0.5, std::nullopt));
  CHECK(clock_refused(std::nullopt, -1.0));
  CHECK(clock_refused(std::nullopt, 2.0e7));
  CHECK(!clock_refused(23.99, 86400.0));
}

TEST_CASE("request: the frame's clock is the one engine-view's --time-of-day and --sun-rate set") {
  renderer::SceneData scene;
  renderer::ClockRequest clock;
  clock.time_of_day_h = 18.0;
  clock.sun_rate = 600.0;
  // No sky: the stand-in sun has no hour, so the offset is zero whatever is asked.
  renderer::FrameClock c = renderer::frame_clock(clock, scene);
  CHECK(c.offset_s == 0.0);
  CHECK(c.sun_rate == 600.0);
  CHECK(c.at(60) == 600.0);
  // A sky over ground three years and five hours in: 18:00 is thirteen hours on.
  scene.sky.emplace();
  scene.terrain.enabled = true;
  scene.terrain.time_s = 94'608'000.0 + 5.0 * 3600.0;
  c = renderer::frame_clock(clock, scene);
  CHECK(c.offset_s == 13.0 * 3600.0);
  CHECK(c.at(0) == 13.0 * 3600.0);
  CHECK(c.at(120) == 13.0 * 3600.0 + 1200.0);
  // The frame and the flight read the same numbers.
  renderer::Camera camera;
  camera.position = Vec3{1.0f, 2.0f, 3.0f};
  const renderer::FrameDesc frame = renderer::frame_at(c, camera, 120);
  CHECK(frame.frame_index == 120);
  CHECK(frame.sun_time_s == c.at(120));
  CHECK(frame.camera.position.y == 2.0f);
  CHECK(frame.view_mode == renderer::FrameDesc{}.view_mode);
  renderer::FlightOptions flight;
  renderer::flight_clock(c, flight);
  CHECK(flight.sun_rate == 600.0);
  CHECK(flight.sun_time_s == c.offset_s);
  // A clock that asks for nothing is still: the `renderer.sun.rate` tunable's 0 and no offset.
  const renderer::FrameClock still = renderer::frame_clock(renderer::ClockRequest{}, scene);
  CHECK(still.offset_s == 0.0);
  CHECK(still.at(1000) == 0.0);
}

TEST_CASE("request: settings_for resolves the page budget's share and the morph weights") {
  renderer::SceneData scene;
  RenderRequest request;
  renderer::RenderSettings out;
  std::string error;
  // Nothing asked and nothing authored: the request's settings, untouched.
  REQUIRE(renderer::settings_for(request, scene, out, &error));
  CHECK(out == request.settings);

  geometry::ClusterPageDesc page;
  page.bytes = 3000;
  scene.pages.pages.push_back(page);
  page.bytes = 1000;
  scene.pages.pages.push_back(page);
  request.page_budget_pct = 25;
  REQUIRE(renderer::settings_for(request, scene, out, &error));
  CHECK(out.page_budget_bytes == 1000);

  // Two channels, the second authored at 0.5: it starts there unless a pose stage plays them.
  scene.lod.mesh.morph_channels.resize(2);
  scene.lod.mesh.morph_channels[1].default_weight = 0.5f;
  scene.lod.mesh.morph_names.push_back(std::string("smile"));
  scene.lod.mesh.morph_names.push_back(std::string("frown"));
  REQUIRE(renderer::settings_for(request, scene, out, &error));
  REQUIRE(out.morph_static_weights.size() == 2);
  CHECK(out.morph_static_weights[0] == 0.0f);
  CHECK(out.morph_static_weights[1] == 0.5f);
  request.morph.push_back(std::string("smile=0.25"));
  request.morph.push_back(std::string("1=-0.5"));
  REQUIRE(renderer::settings_for(request, scene, out, &error));
  CHECK(out.morph_static_weights[0] == 0.25f);
  CHECK(out.morph_static_weights[1] == -0.5f);
  request.morph_defaults = false;
  request.morph.clear();
  REQUIRE(renderer::settings_for(request, scene, out, &error));
  CHECK(out.morph_static_weights.empty());
  request.morph.push_back(std::string("grin=1"));
  CHECK(!renderer::settings_for(request, scene, out, &error));
  CHECK(error.find("grin") != std::string::npos);
}

TEST_CASE("request: the view layout and the ground's file options come from the request") {
  renderer::RenderSettings s;
  s.views = renderer::ViewLayout::Surround3;
  s.side_yaw = 0.25f;
  s.panini_d = 0.75f;
  s.peripheral_lod = 2.0f;
  const renderer::ViewSetDesc views = renderer::view_set_desc(s);
  CHECK(views.layout == renderer::ViewLayout::Surround3);
  CHECK(views.surround.side_yaw == 0.25f);
  CHECK(views.panini_d == 0.75f);
  CHECK(views.peripheral_lod == 2.0f);

  RenderRequest request;
  renderer::SceneFileOptions options;
  renderer::scene_file_options(request, options);
  CHECK(!options.ground_time_s.has_value());
  request.ground_time_s = 189'216'000.0;
  renderer::scene_file_options(request, options);
  REQUIRE(options.ground_time_s.has_value());
  CHECK(*options.ground_time_s == 189'216'000.0);

  // A ground with no levels does nothing and lays nothing out.
  renderer::MovingGround ground;
  CHECK(!ground.active());
  CHECK(!ground.moving());
  CHECK(ground.level_set() == nullptr);
  CHECK(std::string_view(renderer::ground_layout(ground.motion())).empty());
}
