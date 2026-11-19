// The eye's adaptation on a GPU (exposure.h, src/exposure_pass.h; docs/subsystems/renderer.md,
// "Exposure: the eye's adaptation"; roadmap R168):
//
//   1. **A walk from noon into a room with one door and out again.** Outside the exposure settles
//      on its target within the first second; through the door it brightens towards the room's
//      target at dark adaptation's rate — still more than half a stop short a frame after the step,
//      within a quarter stop by the time the model gives — and back out it darkens at light
//      adaptation's, well inside a second. Every frame's step is the CPU model's from the frame
//      before, to a hundredth of a stop, and its target is the model's for the luminance it
//      metered.
//   2. **The same path is the same exposure**: two fresh renderers walk it to the same bits at
//      every frame and the same picture at its end (the bounce off: its own sampling is not
//      reproducible, renderer.md "Indirect light").
//   3. **Pinned and off.** A fixed exposure value holds the frame (no adaptation, the encode's
//   scale
//      1, the picture the unadapted one's to the half's rounding), and a renderer without the
//      switch registers no pass, so its frames are the bytes they were.
//
// The room is lit through its door by the sky and, where the device traces, by the world-space
// cache's sky visibility (`renderer.gi.mode sky`), which is what makes a room dark; without it the
// sky's ambient reaches the room unoccluded, only the sun is lost, and the room is under the noon
// outside by what its darker walls take (1.7 stops measured; `write_room` has why).

#include "test_device.h"

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/exposure.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 160;
constexpr u32 k_height = 90;
constexpr f64 k_dt = 1.0 / 60.0;

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
std::string f(f32 v) { return std::to_string(static_cast<double>(v)); }

struct MeshBuilder {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<u32> indices;

  // A quad whose front faces `normal`: the corners are put in the order that makes it so.
  void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 normal) {
    if (dot(cross(b - a, c - a), normal) < 0.0f) std::swap(b, d);
    const u32 base = static_cast<u32>(positions.size());
    for (const Vec3& p : {a, b, c, d}) {
      positions.push_back(p);
      normals.push_back(normal);
    }
    for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u})
      indices.push_back(base + i);
  }
  // An axis-aligned rectangle in the plane `axis` = `at`, spanning [u0, u1] x [v0, v1] on the other
  // two axes (in x, y, z order), facing `sign` along the axis.
  void rect(u32 axis, f32 at, f32 u0, f32 u1, f32 v0, f32 v1, f32 sign) {
    auto point = [&](f32 u, f32 v) {
      if (axis == 0) return Vec3{at, u, v};
      if (axis == 1) return Vec3{u, at, v};
      return Vec3{u, v, at};
    };
    Vec3 normal{0, 0, 0};
    if (axis == 0) normal.x = sign;
    if (axis == 1) normal.y = sign;
    if (axis == 2) normal.z = sign;
    quad(point(u0, v0), point(u1, v0), point(u1, v1), point(u0, v1), normal);
  }
};

// Two primitives over one set of vertices, u32 indices (as gi_cache_tests.cpp writes its box): the
// indices before `split` take the first material, the rest the second.
bool write_glb(const std::string& path, const MeshBuilder& m, u32 split, Vec3 first_color,
               Vec3 second_color, f32 roughness) {
  std::vector<u8> bin;
  for (const Vec3& p : m.positions) {
    put_f32(bin, p.x);
    put_f32(bin, p.y);
    put_f32(bin, p.z);
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (const Vec3& v : m.normals) {
    put_f32(bin, v.x);
    put_f32(bin, v.y);
    put_f32(bin, v.z);
  }
  const u32 uv_offset = static_cast<u32>(bin.size());
  for (const Vec3& p : m.positions) {
    put_f32(bin, p.x + p.z);
    put_f32(bin, p.y);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  for (const u32 i : m.indices)
    put_u32(bin, i);
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  Vec3 lo = m.positions[0];
  Vec3 hi = m.positions[0];
  for (const Vec3& p : m.positions) {
    lo = Vec3{std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
    hi = Vec3{std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
  }
  const u64 count = m.positions.size();
  auto material = [&](Vec3 c) {
    return "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" + f(c.x) + "," + f(c.y) + "," +
           f(c.z) + ",1],\"metallicFactor\":0,\"roughnessFactor\":" + f(roughness) + "}}";
  };
  const std::string attributes = "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2}";
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{" +
      attributes + ",\"indices\":3,\"material\":0},{" + attributes +
      ",\"indices\":4,\"material\":1}]}],"
      "\"materials\":[" +
      material(first_color) + "," + material(second_color) +
      "],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      n(count) + ",\"type\":\"VEC3\",\"min\":[" + f(lo.x) + "," + f(lo.y) + "," + f(lo.z) +
      "],\"max\":[" + f(hi.x) + "," + f(hi.y) + "," + f(hi.z) +
      "]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      n(count) +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" +
      n(count) +
      ",\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5125,\"count\":" +
      n(split) +
      ",\"type\":\"SCALAR\"},"
      "{\"bufferView\":3,\"byteOffset\":" +
      n(u64{split} * 4) + ",\"componentType\":5125,\"count\":" + n(m.indices.size() - split) +
      ",\"type\":\"SCALAR\"}],"
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
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return file.good();
}

// The room: 6 m square inside, 3 m to the ceiling, walls 0.3 m thick, a door 1.2 m wide and 2.1 m
// high in its east (+x) wall, on a ground 120 m across. Two shells — the inside facing in, the
// outside facing out — and the door's reveals, so it is closed to the sky but for the door from
// every side a ray or a camera comes.
//
// **The inside is darker than the outside** (a second material, about 0.55 of the outside's
// albedo), and that is what keeps walking in a step of more than a stop where the device does not
// trace: without the world-space cache nothing occludes the sky's ambient, so the room is lit as an
// unshadowed surface out of the sun is, and the outside view is mostly the door's wall, which the
// noon sun does not reach either. With one albedo the two metered a stop apart there (7574 and 3779
// cd/m2, the targets 0.98 stops apart); with the inside at 0.55 they are 7556 and 2247, and the
// targets 1.7 stops apart. Where the cache traces, the room meters about 25 cd/m2.
bool write_room(const std::string& path) {
  MeshBuilder m;
  const f32 h = 3.0f;    // the inside's half width
  const f32 t = 0.3f;    // the walls
  const f32 top = 3.0f;  // the ceiling
  const f32 d = 0.6f;    // the door's half width
  const f32 dh = 2.1f;   // and its height
  const f32 g = 60.0f;   // the ground's half width
  // The ground, around the room's floor: a three by three grid of rects without its middle, so
  // every edge is shared whole (no T-junctions at the floor's).
  const f32 cuts[4] = {-g, -h, h, g};
  for (u32 i = 0; i < 3; ++i) {
    for (u32 j = 0; j < 3; ++j) {
      if (i == 1 && j == 1) continue;
      m.rect(1, 0.0f, cuts[i], cuts[i + 1], cuts[j], cuts[j + 1], 1.0f);
    }
  }
  // The outside, facing out.
  const f32 o = h + t;
  m.rect(1, top + t, -o, o, -o, o, 1.0f);
  m.rect(0, -o, 0.0f, top + t, -o, o, -1.0f);
  m.rect(2, -o, -o, o, 0.0f, top + t, -1.0f);
  m.rect(2, o, -o, o, 0.0f, top + t, 1.0f);
  m.rect(0, o, 0.0f, top + t, -o, -d, 1.0f);
  m.rect(0, o, 0.0f, top + t, d, o, 1.0f);
  m.rect(0, o, dh, top + t, -d, d, 1.0f);
  // The door's reveals: its two sides and its head.
  m.rect(2, -d, h, o, 0.0f, dh, 1.0f);
  m.rect(2, d, h, o, 0.0f, dh, -1.0f);
  m.rect(1, dh, h, o, -d, d, -1.0f);
  const u32 inside_from = static_cast<u32>(m.indices.size());
  // The inside, facing in: its floor, its ceiling and its walls.
  m.rect(1, 0.0f, -h, h, -h, h, 1.0f);
  m.rect(1, top, -h, h, -h, h, -1.0f);
  m.rect(0, -h, 0.0f, top, -h, h, 1.0f);
  m.rect(2, -h, -h, h, 0.0f, top, 1.0f);
  m.rect(2, h, -h, h, 0.0f, top, -1.0f);
  m.rect(0, h, 0.0f, top, -h, -d, -1.0f);
  m.rect(0, h, 0.0f, top, d, h, -1.0f);
  m.rect(0, h, dh, top, -d, d, -1.0f);
  return write_glb(path, m, inside_from, Vec3{0.6f, 0.58f, 0.55f}, Vec3{0.33f, 0.32f, 0.3f}, 0.9f);
}

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

// A tunable set for the length of a scope: `renderer.exposure.*` and `renderer.gi.*` are read when
// a renderer is made.
struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  bool traces = false;  // the world-space cache can run (it traces the shadow rays' query)
  Gpu() {
    std::string error;
    if (!device.create(renderer_test::device_options(), &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    traces = device.features().cluster_acceleration_structure && device.features().ray_query;
    ok = true;
  }
};

SceneDesc room_scene(const std::filesystem::path& dir, scene::SkyExposure exposure) {
  const std::string room = slashes(dir / "room.glb");
  REQUIRE(write_room(room));
  SceneDesc desc;
  desc.meshes.push_back(room);
  desc.ddc = slashes(dir / "ddc");
  desc.instances.push_back(SceneInstance{0, Transform3{}});
  scene::Sky sky;
  sky.latitude_deg = 40.0f;
  sky.day_of_year = 172.0f;
  sky.turbidity = 2.5f;
  sky.exposure = exposure;
  desc.sky = sky;
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc) {
    if (!load_scene(desc, data, error)) return false;
    RenderSettings settings;
    settings.shadows = ShadowMode::Auto;
    settings.lights = false;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

// **The walk**: a second outside, 12 m east of the door looking in at it; two seconds walking in to
// 1.5 m inside the door; six seconds held there, looking at the west wall; two seconds back out at
// the first place. Eye height 1.6 m, noon at 40 degrees north on the longest day.
constexpr u32 k_outside_end = 60;
constexpr u32 k_walk_end = 180;
constexpr u32 k_inside_end = 540;
constexpr u32 k_out_end = 660;

FrameDesc walk_frame(u32 index) {
  f64 x = 15.0;
  if (index >= k_outside_end && index < k_walk_end) {
    const f64 s =
        static_cast<f64>(index - k_outside_end) / static_cast<f64>(k_walk_end - k_outside_end);
    x = 15.0 + (1.5 - 15.0) * s;
  } else if (index >= k_walk_end && index < k_inside_end) {
    x = 1.5;
  }
  FrameDesc frame;
  frame.frame_index = index;
  frame.camera.position = WorldPos{x, 1.6, 0.0};
  frame.camera.target = WorldPos{x - 10.0, 1.4, 0.0};
  frame.camera.znear = 0.05f;
  frame.sky_time_s = 12.0 * 3600.0;
  frame.world_time_s = static_cast<f64>(index) * k_dt;
  return frame;
}

bool walk_to(Rig& rig, u32 from, u32 to, std::vector<ExposureState>* states) {
  for (u32 i = from; i < to; ++i) {
    if (!rig.renderer.render_offscreen(walk_frame(i), &rig.error)) return false;
    if (states != nullptr) {
      ExposureState s;
      if (!rig.renderer.exposure_state(s)) {
        rig.error = "no exposure state";
        return false;
      }
      states->push_back(s);
    }
  }
  return true;
}

// The first frame from `from` after which every frame up to `to` stands within `stops` of its
// target.
u32 settled_at(const std::vector<ExposureState>& states, u32 from, u32 to, f64 stops) {
  u32 at = to;
  for (u32 i = to; i-- > from;) {
    if (std::abs(static_cast<f64>(states[i].ev100 - states[i].target_ev100)) > stops) break;
    at = i;
  }
  return at;
}

bool same_bits(f32 a, f32 b) {
  u32 x = 0;
  u32 y = 0;
  std::memcpy(&x, &a, 4);
  std::memcpy(&y, &b, 4);
  return x == y;
}

}  // namespace

TEST_CASE("exposure: walked from noon into a room and out, the eye's adaptation at its speeds") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_exposure_walk");
  const ScopedTunable gi("renderer.gi.mode", gpu.traces ? "sky" : "off");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, room_scene(tmp.native(), scene::SkyExposure::Adapted)),
                  rig.error);
  REQUIRE(rig.renderer.frame_pass_count() == 1);
  std::vector<ExposureState> states;
  REQUIRE_MESSAGE(walk_to(rig, 0, k_out_end, &states), rig.error);
  const ExposureOptions options;  // the defaults the tunables hold

  // Every frame: its target is the model's for what it metered, and its step is the model's from
  // the frame before (the first frame takes its target).
  f64 worst_target = 0.0;
  f64 worst_step = 0.0;
  for (u32 i = 0; i < states.size(); ++i) {
    const ExposureState& s = states[i];
    REQUIRE(s.frames == i + 1);
    CHECK(s.pinned == 0);
    CHECK(s.weight > 0);
    const f64 luminance = static_cast<f64>(s.luminance);
    const f64 target = exposure_target_ev100(luminance, 0.0, options);
    worst_target = std::max(worst_target, std::abs(target - static_cast<f64>(s.target_ev100)));
    const f64 want =
        i == 0 ? static_cast<f64>(s.target_ev100)
               : exposure_step(static_cast<f64>(states[i - 1].ev100),
                               static_cast<f64>(s.target_ev100), luminance, k_dt, options);
    worst_step = std::max(worst_step, std::abs(want - static_cast<f64>(s.ev100)));
    CHECK(
        std::abs(static_cast<f64>(s.scale) - std::exp2(static_cast<f64>(s.base_ev100 - s.ev100))) <=
        1e-4 * static_cast<f64>(s.scale));
  }
  MESSAGE("the model against the GPU: targets within " << worst_target << " stops, steps within "
                                                       << worst_step);
  CHECK(worst_target <= 0.01);
  CHECK(worst_step <= 0.01);

  const ExposureState& outside = states[k_outside_end - 1];
  const ExposureState& entered = states[k_walk_end - 1];
  const ExposureState& inside = states[k_inside_end - 1];
  const ExposureState& back = states[k_out_end - 1];
  MESSAGE("outside: metered " << outside.luminance << " cd/m2, EV100 " << outside.ev100
                              << " (target " << outside.target_ev100 << ", the rule's "
                              << outside.base_ev100 << "); inside: " << inside.luminance
                              << " cd/m2, EV100 " << inside.ev100 << " (target "
                              << inside.target_ev100 << "); out again " << back.ev100
                              << std::string(gpu.traces ? "" : " (no cache)"));
  // Outside, settled within the first second; inside, the room's target lower by more than a stop.
  CHECK(std::abs(outside.ev100 - outside.target_ev100) <= 0.05f);
  CHECK(inside.target_ev100 <= outside.target_ev100 - 1.0f);

  // **Into the room: dark adaptation.** The model's time to come within a quarter stop of a step of
  // `gap` stops is tau ln(gap / 0.25); the walk's last frame is where the step ends.
  const f64 gap = std::abs(static_cast<f64>(entered.ev100 - inside.target_ev100));
  const u32 settled = settled_at(states, k_walk_end, k_inside_end, 0.25);
  const f64 model_s =
      gap > 0.25 ? static_cast<f64>(options.dark_tau_s) * std::log(gap / 0.25) : 0.0;
  const f64 took_s = static_cast<f64>(settled - k_walk_end) * k_dt;
  MESSAGE("into the room: " << gap << " stops to go at the threshold, within a quarter stop after "
                            << took_s << " s (the model: " << model_s << " s)");
  CHECK(took_s <= model_s + 0.1);
  CHECK(std::abs(inside.ev100 - inside.target_ev100) <= 0.1f);
  // Not at once: a frame after the walk the room is still more than half a stop short, when the
  // step was more than a stop.
  if (gap > 1.0) {
    CHECK(std::abs(states[k_walk_end].ev100 - states[k_walk_end].target_ev100) > 0.5f);
  }

  // **Back out: light adaptation**, at the cones' and the rods' rate: within a quarter stop well
  // inside a second.
  const u32 out_settled = settled_at(states, k_inside_end, k_out_end, 0.25);
  const f64 out_s = static_cast<f64>(out_settled - k_inside_end) * k_dt;
  MESSAGE("back out: within a quarter stop after " << out_s << " s");
  CHECK(out_s <= 1.0);
  CHECK(std::abs(back.ev100 - back.target_ev100) <= 0.05f);

  // The renderer's statistics carry it, a slot late.
  CHECK(rig.renderer.stats().exposure.frames > 0);
}

TEST_CASE("exposure: the same walk is the same exposure, frame by frame, and the same picture") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_exposure_same");
  const ScopedTunable gi("renderer.gi.mode", "off");
  const SceneDesc desc = room_scene(tmp.native(), scene::SkyExposure::Adapted);
  constexpr u32 k_frames = 120;
  std::vector<ExposureState> runs[2];
  CapturedFrame pictures[2];
  for (u32 r = 0; r < 2; ++r) {
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, desc), rig.error);
    REQUIRE_MESSAGE(walk_to(rig, 40, 40 + k_frames, &runs[r]), rig.error);
    CaptureChannels channels;
    channels.color = true;
    REQUIRE_MESSAGE(
        rig.renderer.capture(walk_frame(40 + k_frames), channels, pictures[r], &rig.error),
        rig.error);
  }
  u32 differ = 0;
  for (u32 i = 0; i < k_frames; ++i) {
    if (!same_bits(runs[0][i].ev100, runs[1][i].ev100) ||
        !same_bits(runs[0][i].luminance, runs[1][i].luminance) ||
        runs[0][i].weight != runs[1][i].weight) {
      ++differ;
    }
  }
  CHECK(differ == 0);
  CHECK(pictures[0].color == pictures[1].color);
}

TEST_CASE("exposure: a fixed exposure value pins it, and without the switch there is no pass") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_exposure_pinned");
  const ScopedTunable gi("renderer.gi.mode", "off");
  CaptureChannels channels;
  channels.color = true;
  FrameDesc frame = walk_frame(200);
  frame.exposure.fixed = true;
  frame.exposure.ev100 = 12.0f;

  // Without the switch: no pass, so the frame is drawn straight into its target as it always was.
  CapturedFrame plain;
  {
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, room_scene(tmp.native(), scene::SkyExposure::Auto)),
                    rig.error);
    CHECK(rig.renderer.frame_pass_count() == 0);
    ExposureState none;
    CHECK_FALSE(rig.renderer.exposure_state(none));
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, plain, &rig.error), rig.error);
  }
  // With it, through the tunable, at a fixed value: pinned, scale 1, and the picture the plain one
  // through the linear image's half — a code apart at most (frame_pass_tests.cpp's bound).
  {
    const ScopedTunable adapt("renderer.exposure.adapt", "true");
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, room_scene(tmp.native(), scene::SkyExposure::Auto)),
                    rig.error);
    CHECK(rig.renderer.frame_pass_count() == 1);
    CapturedFrame pinned;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, pinned, &rig.error), rig.error);
    ExposureState s;
    REQUIRE(rig.renderer.exposure_state(s));
    CHECK(s.pinned == 1);
    CHECK(s.scale == 1.0f);
    CHECK(s.ev100 == 12.0f);
    REQUIRE(pinned.color.size() == plain.color.size());
    i32 worst = 0;
    for (u32 i = 0; i < plain.color.size(); ++i) {
      worst = std::max(
          worst, std::abs(static_cast<i32>(pinned.color[i]) - static_cast<i32>(plain.color[i])));
    }
    CHECK(worst <= 1);
  }
}
