// walk_poc_world: the fixture of what a module can ask the world (hotload ABI 10; roadmap A43;
// docs/subsystems/apps.md, "What a module can ask the world"). engine-view's end-to-end case
// (apps/engine_view/tests/world_queries_view_tests.cpp) runs it in a small scene of its own — the
// oasis pool, an orchard tree at (0, 18), a post that declares a perch at (6, 16), a boulder at
// (-6, 16) and a prop `bird` — and holds what it hands `end_run`.
//
// Once the ground under the feet has collision (`game_ready_milestone`) and the walker's ring has
// had its ticks to take its nine tiles, it asks, on one tick:
//   - the supports near the tree's crown, the post's top and the boulder's top, and lands a `bird`
//     on the nearest of each;
//   - a ray at the trunk, sixteen rays straight down through the crown's footprint, a sphere, and a
//     ray down into the pond that stops at the water;
//   - the water at five points of the pond;
// and ends the run with every answer. A fixture: its coordinates are the test scene's, and it keeps
// no state (a reload starts it again).

#include <foundation/hotload/module_abi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace engine;
using namespace engine::hotload;

namespace {

constexpr u32 k_probe_tick = 120;  // the module's ticks before it asks: half a second at 240 Hz
u32 g_ticks = 0;
bool g_done = false;
u32 g_bird = 0;
bool g_props = false;

// The answer, built in a fixed buffer: a module allocates nothing on a tick.
char g_json[16384];
u32 g_at = 0;
void put(const char* text) {
  while (*text != '\0' && g_at + 1 < sizeof(g_json))
    g_json[g_at++] = *text++;
  g_json[g_at] = '\0';
}

f64 ground(ModuleContext* context, f64 x, f64 z) {
  f64 h = 0.0;
  if (context->api->ground_height(context->api->host, x, z, &h) != Result::Ok) return 0.0;
  return h;
}

// The nearest support within `radius` of the point, as JSON, and a bird landed on it.
void supports(ModuleContext* context, const char* name, f64 x, f64 y, f64 z, f32 radius,
              f32 min_size) {
  Support found[8];
  SupportQuery query;
  const f64 point[3] = {x, y, z};
  const Result r = supports_near(context, point, radius, min_size, found, 8, query);
  char line[512];
  std::snprintf(line, sizeof line, "\"%s\":{\"result\":%u,\"count\":%u,\"found\":%u,\"covered\":%u",
                name, static_cast<u32>(r), query.count, query.found, query.covered);
  put(line);
  if (r == Result::Ok && query.count > 0) {
    const Support& s = found[0];
    std::snprintf(line, sizeof line,
                  ",\"kind\":%u,\"part\":%u,\"what\":%u,\"instance\":%u,\"mesh\":%u,"
                  "\"position\":[%.9g,%.9g,%.9g],\"up\":[%.9g,%.9g,%.9g],\"size_m\":%.9g",
                  s.kind, s.part, s.what, s.instance, s.mesh, s.position[0], s.position[1],
                  s.position[2], static_cast<f64>(s.up[0]), static_cast<f64>(s.up[1]),
                  static_cast<f64>(s.up[2]), static_cast<f64>(s.size_m));
    put(line);
    // ABI 12 (A45): the limb's axis and the piece's name, and the flags saying they are filled.
    std::snprintf(line, sizeof line, ",\"flags\":%u,\"along\":[%.9g,%.9g,%.9g],\"placement\":%llu",
                  s.flags, static_cast<f64>(s.along[0]), static_cast<f64>(s.along[1]),
                  static_cast<f64>(s.along[2]), static_cast<unsigned long long>(s.placement));
    put(line);
    if (g_props) {
      u32 prop = 0;
      const Result placed =
          context->api->prop_place(context->api->host, g_bird, s.position, 0.0f, 1.0f, &prop);
      std::snprintf(line, sizeof line, ",\"landed\":%u", placed == Result::Ok ? 1u : 0u);
      put(line);
    }
  }
  put("}");
}

// One entry's answer into the "entries" object: its result code, or 0 for a call with none.
u32 g_entries = 0;
void entry(const char* name, Result r) {
  char line[96];
  std::snprintf(line, sizeof line, "%s\"%s\":%d", g_entries == 0 ? "" : ",", name,
                static_cast<i32>(r));
  put(line);
  ++g_entries;
}

// **Every entry of the host's table, once** (A45; the case ABI 11 never had): what this module did
// not already ask above, each with arguments a module would pass — the props and the HUD's on
// things this module makes, the actors' on a name the scene has none of (`NotFound` is their
// answer), the schema's on a type it does not have — so the end-to-end case holds that every entry
// a host of this header has answers through the host, and the count against the table's size.
void table(ModuleContext* context, const f64* at) {
  const HostApi* api = context->api;
  void* h = api->host;
  const auto text = [](const char* s) { return static_cast<u64>(std::strlen(s)); };
  g_entries = 0;
  put(",\"entries\":{");
  void* p = api->allocate(h, 64, 16);
  entry("allocate", p != nullptr ? Result::Ok : Result::NotFound);
  api->deallocate(h, p, 64, 16);
  entry("deallocate", Result::Ok);
  api->log(h, LogLevel::Info, "table probe", text("table probe"));
  entry("log", Result::Ok);
  entry("tick", api->tick(h) > 0 ? Result::Ok : Result::NotFound);
  entry("fixed_step", api->fixed_step(h) > 0.0 ? Result::Ok : Result::NotFound);
  FieldRef ref{};
  entry("resolve", api->resolve(h, "NoSuchType", text("NoSuchType"), "x", 1, &ref));
  i64 enumerator = 0;
  entry("enum_value", api->enum_value(h, ref, "x", 1, &enumerator));
  Value value{};
  entry("read", api->read(h, View{}, ref, &value));
  entry("read_element", api->read_element(h, View{}, ref, 0, &value));
  // `fail` only words a failure the tick then returns; this tick returns, so it is dropped.
  api->fail(h, "table probe", text("table probe"));
  entry("fail", Result::Ok);
  u32 bit = 0;
  entry("game_action", api->game_action(h, "no_such_action", text("no_such_action"), &bit));
  api->hud_text(h, 0, "table probe", text("table probe"));
  entry("hud_text", Result::Ok);
  api->hud_meter(h, "probe", text("probe"), 1.0, 2.0);
  entry("hud_meter", Result::Ok);
  entry("end_run", Result::Ok);  // called last, with this object
  f64 position[3] = {0.0, 0.0, 0.0};
  f64 distance = 0.0;
  entry("nearest_placement",
        api->nearest_placement(h, "tree", text("tree"), 30.0, position, &distance));
  u32 mesh = 0;
  const Result has_mesh = api->prop_mesh(h, "bird", text("bird"), &mesh);
  entry("prop_mesh", has_mesh);
  u32 prop = 0;
  entry("prop_place", api->prop_place(h, mesh, at, 0.0f, 1.0f, &prop));
  entry("prop_move", api->prop_move(h, prop, at, 0.5f));
  u32 found_prop = 0;
  entry("nearest_prop", api->nearest_prop(h, mesh, 5.0, &found_prop, position, &distance));
  entry("prop_remove", api->prop_remove(h, prop));
  entry("walker_set", api->walker_set(h, 1.0f, 1.0f));
  // Where the session started, so the walker stays where the questions above were asked.
  const f64 start[3] = {0.0, ground(context, 0.0, 12.5), 12.5};
  entry("walker_teleport", api->walker_teleport(h, start, 3.14159265f, 0.0f));
  u32 clip = 0;
  entry("sound_clip", api->sound_clip(h, "no_such_clip", text("no_such_clip"), &clip));
  u32 voice = 0;
  entry("sound_play", api->sound_play(h, clip, at, 1.0f, &voice));
  entry("sound_stop", api->sound_stop(h, voice));
  HudElement element;
  element.id = 7;
  entry("hud_element", api->hud_element(h, &element));
  entry("hud_remove", api->hud_remove(h, 7));
  GraphicsState graphics;
  entry("graphics_state", api->graphics_state(h, &graphics));
  GraphicsRequest request;
  entry("request_graphics", api->request_graphics(h, &request));
  u32 actor = 0;
  entry("actor_find", api->actor_find(h, "no_such_actor", text("no_such_actor"), &actor));
  entry("actor_spawn", api->actor_spawn(h, "no_such_mesh", text("no_such_mesh"), at, 0.0f, &actor));
  entry("actor_place", api->actor_place(h, actor, at, 0.0f));
  entry("actor_play", api->actor_play(h, actor, "walk", text("walk"), 0.0f, 1));
  entry("actor_stop", api->actor_stop(h, actor, 0.0f));
  entry("actor_speed", api->actor_speed(h, actor, 1.0f));
  ActorState state;
  entry("actor_state", api->actor_state(h, actor, &state));
  f64 height = 0.0;
  entry("ground_height", api->ground_height(h, 0.0, 12.5, &height));
  HudImage image;
  entry("hud_image", api->hud_image(h, &image));
  u32 font = 0;
  entry("hud_font", api->hud_font(h, "no_such_font.ttf", text("no_such_font.ttf"), 18.0f, &font));
  const f32 up[3] = {0.0f, 1.0f, 0.0f};
  entry("actor_place_aligned", api->actor_place_aligned(h, actor, at, 0.0f, up, 0.3f));
  GroundSample sample;
  entry("ground_sample", api->ground_sample(h, 0.0, 12.5, &sample));
  const f32 weights[1] = {0.0f};
  entry("actor_morph", api->actor_morph(h, actor, weights, 1));
  entry("actor_scale", api->actor_scale(h, actor, 1.0f));
  MorphInfo morph;
  entry("actor_morph_info", api->actor_morph_info(h, actor, &morph));
  CastHit hit;
  const f32 down[3] = {0.0f, -1.0f, 0.0f};
  entry("cast_ray", api->cast_ray(h, at, down, 20.0f, &hit));
  entry("cast_sphere", api->cast_sphere(h, at, down, 20.0f, 0.1f, &hit));
  WaterSample water;
  entry("water_at", api->water_at(h, 0.0, 0.0, &water));
  Support found[4];
  SupportQuery query;
  query.radius_m = 4.0f;
  query.capacity = 4;
  query.out = found;
  entry("supports_near", api->supports_near(h, at, &query));
  entry("actor_lod_bias", api->actor_lod_bias(h, actor, 1.0f));
  ClipInfo info;
  entry("actor_clip_info", api->actor_clip_info(h, actor, &info));
  entry("actor_seek", api->actor_seek(h, actor, 0.0f));
  char line[64];
  std::snprintf(line, sizeof line, "},\"entry_count\":%u", g_entries);
  put(line);
}

void cast_json(const char* name, Result r, const CastHit& hit) {
  char line[512];
  std::snprintf(line, sizeof line,
                "\"%s\":{\"result\":%u,\"what\":%u,\"instance\":%u,\"mesh\":%u,\"index\":%u,"
                "\"covered\":%u,\"distance\":%.9g,\"position\":[%.9g,%.9g,%.9g],"
                "\"normal\":[%.9g,%.9g,%.9g]}",
                name, static_cast<u32>(r), hit.what, hit.instance, hit.mesh, hit.index, hit.covered,
                static_cast<f64>(hit.distance), hit.position[0], hit.position[1], hit.position[2],
                static_cast<f64>(hit.normal[0]), static_cast<f64>(hit.normal[1]),
                static_cast<f64>(hit.normal[2]));
  put(line);
}

void probe(ModuleContext* context) {
  g_at = 0;
  put("{");
  // The supports: the tree's crown 3 m up, a 3 cm foot; the post's and the boulder's tops.
  supports(context, "tree", 0.0, ground(context, 0.0, 18.0) + 3.0, 18.0, 5.0f, 0.03f);
  put(",");
  supports(context, "post", 6.0, ground(context, 6.0, 16.0) + 1.5, 16.0, 2.0f, 0.0f);
  put(",");
  supports(context, "boulder", -6.0, ground(context, -6.0, 16.0) + 1.5, 16.0, 2.0f, 0.0f);
  // The casts: the trunk a metre up from 1.5 m west of it (short, so a dune's slope does not
  // reach the ray first).
  CastHit hit;
  const f32 east[3] = {1.0f, 0.0f, 0.0f};
  const f32 down[3] = {0.0f, -1.0f, 0.0f};
  const f64 trunk_from[3] = {-1.5, ground(context, 0.0, 18.0) + 1.0, 18.0};
  put(",");
  cast_json("trunk", cast_ray(context, trunk_from, east, 3.0f, hit), hit);
  // Sixteen rays straight down through the crown's footprint, 1.5 m round the trunk: what each met.
  put(",\"crown\":[");
  for (u32 k = 0; k < 16; ++k) {
    // The sixteen points of a circle by a quarter turn's rotations of four, no trigonometry.
    const f64 c4[4][2] = {
        {1.0, 0.0}, {0.9238795, 0.3826834}, {0.7071068, 0.7071068}, {0.3826834, 0.9238795}};
    const f64 cx = c4[k % 4][0];
    const f64 cz = c4[k % 4][1];
    const u32 q = k / 4;
    const f64 rx = q == 0 ? cx : (q == 1 ? -cz : (q == 2 ? -cx : cz));
    const f64 rz = q == 0 ? cz : (q == 1 ? cx : (q == 2 ? -cz : -cx));
    const f64 x = 1.5 * rx;
    const f64 z = 18.0 + 1.5 * rz;
    const f64 from[3] = {x, ground(context, x, z) + 12.0, z};
    CastHit c;
    const Result r = cast_ray(context, from, down, 20.0f, c);
    char line[96];
    std::snprintf(line, sizeof line, "%s[%u,%u,%.9g]", k == 0 ? "" : ",", static_cast<u32>(r),
                  c.what, static_cast<f64>(c.distance));
    put(line);
  }
  put("]");
  // A sphere of 0.1 m rolled at the boulder's west face (x = -6.5) from half a metre off, half way
  // up it.
  const f64 ball_from[3] = {-7.0, ground(context, -6.0, 16.0) + 0.5, 16.0};
  put(",");
  cast_json("sphere", cast_sphere(context, ball_from, east, 2.0f, 0.1f, hit), hit);
  // Down into the pond's middle, stopping at the water.
  CastHit water_hit;
  water_hit.flags = k_cast_water;
  const f64 pond_from[3] = {0.5, 5.0, 0.5};
  put(",");
  cast_json("pond", cast_ray(context, pond_from, down, 20.0f, water_hit), water_hit);
  // The water at five points of the pond.
  const f64 points[5][2] = {{0.0, 0.0}, {3.0, 0.0}, {0.0, -5.0}, {-6.0, 2.0}, {4.0, -4.0}};
  put(",\"water\":[");
  for (u32 k = 0; k < 5; ++k) {
    WaterSample w;
    const Result r = water_at(context, points[k][0], points[k][1], w);
    char line[256];
    std::snprintf(line, sizeof line,
                  "%s{\"result\":%u,\"x\":%.9g,\"z\":%.9g,\"wet\":%u,\"level\":%.9g,\"depth\":%.9g,"
                  "\"velocity\":[%.9g,%.9g],\"source\":%u,\"index\":%u}",
                  k == 0 ? "" : ",", static_cast<u32>(r), points[k][0], points[k][1], w.wet,
                  w.level, static_cast<f64>(w.depth), static_cast<f64>(w.velocity[0]),
                  static_cast<f64>(w.velocity[1]), w.source, w.index);
    put(line);
  }
  put("]");
  // And every other entry of the table once (A45), from a point beside the boulder.
  const f64 beside[3] = {-6.0, ground(context, -6.0, 14.0) + 1.0, 14.0};
  table(context, beside);
  put("}");
  context->api->end_run(context->api->host, g_json, g_at);
}

i32 on_load(ModuleContext* context) {
  // The bird the supports land: `Unsupported` (a scene with no props) is a plain answer.
  const char* name = "bird";
  g_props = context->api->prop_mesh(context->api->host, name, 4, &g_bird) == Result::Ok;
  g_ticks = 0;
  g_done = false;
  return 0;
}

void game_tick(ModuleContext* context, View, f32) {
  if (g_done) return;
  if (++g_ticks < k_probe_tick) return;
  g_done = true;
  probe(context);
}

u32 game_ready_milestone(ModuleContext*) {
  return static_cast<u32>(GameMilestone::GroundCollisionReady);
}

}  // namespace

ENGINE_HOTLOAD_DESCRIBE(api, out) {
  (void)api;
  static const Export exports[] = {
      export_function<TickFn>("game_tick", &game_tick),
      export_function<MilestoneFn>("game_ready_milestone", &game_ready_milestone),
  };
  out->abi_version = k_abi_version;
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
  out->name = "walk_poc_world";
  out->exports = exports;
  out->export_count = static_cast<u32>(sizeof(exports) / sizeof(exports[0]));
  out->on_load = &on_load;
  out->state_type = nullptr;
  out->type_info_size = 0;
  return 0;
}
