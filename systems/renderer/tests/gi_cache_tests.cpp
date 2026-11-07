// Indirect light: the world-space cache (R69 stage 1; docs/subsystems/renderer.md, "Indirect light:
// the world-space cache"; docs/experiments/gi-cache-2026-11-07.md).
//
//   1. **Off is the old picture.** The cache's first frame clears it and reads nothing, so every
//      pixel falls back to the harmonics: that frame is the bytes `renderer.gi.mode=off` draws,
//      which is the resolve's ambient as it was — the fallback is the old term to the bit.
//   2. **Toward the reference.** A closed box with one opening and a sphere inside, and a field of
//      cubes on a slab: the reference path tracer at one bounce (direct light plus the environment
//      through the scene's real visibility) is the judge, and the cache's picture is nearer to it
//      by FLIP than the harmonics' are.
//   3. **Converges.** At one pose, the change from one frame to the next falls as the cells'
//      averages fill, and never grows back.
//   4. **World-space keys.** The cube field and its camera moved by whole cells draw the same
//      bytes with the cache on: its keys come from the eye's integer cell and the point's offset
//      from the eye, never from a float of the world.
//   5. **Made at load.** The frames with the cache on make no device buffer and no more engine
//      allocations than the same frames with it off.
//
// Every case skips where the machine cannot trace (the cache traces the shadow rays' query).

#include <core/math/math.h>
#include <core/math/world.h>
#include <core/memory/memory.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <foundation/image/metrics.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 128;
constexpr u32 k_height = 128;

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
};

// A GLB of one primitive and one material, u32 indices, no UVs needed by the material but given.
bool write_glb(const std::string& path, const MeshBuilder& m, Vec3 base_color, f32 roughness) {
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
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
      f(base_color.x) + "," + f(base_color.y) + "," + f(base_color.z) +
      ",1],\"metallicFactor\":0,\"roughnessFactor\":" + f(roughness) +
      "}}],"
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
      n(m.indices.size()) +
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

// A 4 m box seen from inside — every wall faces in — with a 1.6 m square opening in its +x wall,
// split into a grid of quads so it has clusters to cull.
bool write_box(const std::string& path) {
  MeshBuilder m;
  const f32 h = 2.0f;
  const f32 o = 0.8f;
  // Floor, ceiling and four walls as 4 x 4 grids; the +x wall leaves out its opening.
  const u32 cells = 4;
  const f32 step = 2.0f * h / static_cast<f32>(cells);
  for (u32 i = 0; i < cells; ++i) {
    for (u32 j = 0; j < cells; ++j) {
      const f32 a0 = -h + step * static_cast<f32>(i);
      const f32 a1 = a0 + step;
      const f32 b0 = -h + step * static_cast<f32>(j);
      const f32 b1 = b0 + step;
      m.quad(Vec3{a0, -h, b0}, Vec3{a1, -h, b0}, Vec3{a1, -h, b1}, Vec3{a0, -h, b1}, Vec3{0, 1, 0});
      m.quad(Vec3{a0, h, b0}, Vec3{a1, h, b0}, Vec3{a1, h, b1}, Vec3{a0, h, b1}, Vec3{0, -1, 0});
      m.quad(Vec3{-h, a0, b0}, Vec3{-h, a1, b0}, Vec3{-h, a1, b1}, Vec3{-h, a0, b1}, Vec3{1, 0, 0});
      m.quad(Vec3{a0, b0, -h}, Vec3{a1, b0, -h}, Vec3{a1, b1, -h}, Vec3{a0, b1, -h}, Vec3{0, 0, 1});
      m.quad(Vec3{a0, b0, h}, Vec3{a1, b0, h}, Vec3{a1, b1, h}, Vec3{a0, b1, h}, Vec3{0, 0, -1});
    }
  }
  // The +x wall round its opening: below, above, and the two sides between them.
  const Vec3 nx{-1, 0, 0};
  m.quad(Vec3{h, -h, -h}, Vec3{h, -o, -h}, Vec3{h, -o, h}, Vec3{h, -h, h}, nx);
  m.quad(Vec3{h, o, -h}, Vec3{h, h, -h}, Vec3{h, h, h}, Vec3{h, o, h}, nx);
  m.quad(Vec3{h, -o, -h}, Vec3{h, o, -h}, Vec3{h, o, -o}, Vec3{h, -o, -o}, nx);
  m.quad(Vec3{h, -o, o}, Vec3{h, o, o}, Vec3{h, o, h}, Vec3{h, -o, h}, nx);
  return write_glb(path, m, Vec3{0.7f, 0.7f, 0.7f}, 0.9f);
}

// A unit sphere, 24 x 16, outward.
bool write_sphere(const std::string& path) {
  MeshBuilder m;
  const u32 seg = 24;
  const u32 rings = 16;
  for (u32 r = 0; r <= rings; ++r) {
    const f32 v = static_cast<f32>(r) / static_cast<f32>(rings);
    const f32 phi = v * 3.14159265f;
    for (u32 s = 0; s <= seg; ++s) {
      const f32 theta = static_cast<f32>(s) / static_cast<f32>(seg) * 6.2831853f;
      const Vec3 p{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
      m.positions.push_back(p);
      m.normals.push_back(p);
    }
  }
  for (u32 r = 0; r < rings; ++r) {
    for (u32 s = 0; s < seg; ++s) {
      const u32 a = r * (seg + 1) + s;
      const u32 b = a + seg + 1;
      // Counter-clockwise seen from outside.
      for (const u32 i : {a, a + 1, b, a + 1, b + 1, b})
        m.indices.push_back(i);
    }
  }
  return write_glb(path, m, Vec3{0.8f, 0.5f, 0.3f}, 0.7f);
}

// A unit cube, outward.
bool write_cube(const std::string& path) {
  MeshBuilder m;
  const f32 h = 0.5f;
  m.quad(Vec3{h, -h, -h}, Vec3{h, h, -h}, Vec3{h, h, h}, Vec3{h, -h, h}, Vec3{1, 0, 0});
  m.quad(Vec3{-h, -h, -h}, Vec3{-h, h, -h}, Vec3{-h, h, h}, Vec3{-h, -h, h}, Vec3{-1, 0, 0});
  m.quad(Vec3{-h, h, -h}, Vec3{h, h, -h}, Vec3{h, h, h}, Vec3{-h, h, h}, Vec3{0, 1, 0});
  m.quad(Vec3{-h, -h, -h}, Vec3{h, -h, -h}, Vec3{h, -h, h}, Vec3{-h, -h, h}, Vec3{0, -1, 0});
  m.quad(Vec3{-h, -h, h}, Vec3{h, -h, h}, Vec3{h, h, h}, Vec3{-h, h, h}, Vec3{0, 0, 1});
  m.quad(Vec3{-h, -h, -h}, Vec3{h, -h, -h}, Vec3{h, h, -h}, Vec3{-h, h, -h}, Vec3{0, 0, -1});
  return write_glb(path, m, Vec3{0.6f, 0.6f, 0.6f}, 0.8f);
}

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

// A tunable set for the length of a scope: `renderer.gi.*` is read when a renderer is made.
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
  Gpu() {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    if (!device.features().cluster_acceleration_structure || !device.features().ray_query) {
      why = unavailable_reason(RenderAvailability::NoAccelerationStructures, device);
      return;
    }
    ok = true;
  }
};

// The scene, the settings, the GPU scene, the renderer and, when asked, the reference, with the
// shadows traced (the cache traces their query).
struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  ReferenceRenderer reference;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, bool with_reference) {
    RenderSettings settings;
    settings.shadows = ShadowMode::RayTraced;
    settings.lights = false;
    if (!load_scene(desc, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (with_reference && !reference_available(resolved, device, &error)) return false;
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    return !with_reference || reference.create(device, scene, renderer, {}, &error);
  }
};

struct Fixture {
  std::string name;
  SceneDesc desc;
  Camera camera;
};

Fixture box_fixture(const std::filesystem::path& dir) {
  const std::string box = slashes(dir / "box.glb");
  const std::string sphere = slashes(dir / "sphere.glb");
  REQUIRE(write_box(box));
  REQUIRE(write_sphere(sphere));
  Fixture out;
  out.name = "a closed box with one opening and a sphere inside";
  out.desc.meshes.push_back(box);
  out.desc.meshes.push_back(sphere);
  out.desc.ddc = slashes(dir / "ddc");
  out.desc.instances.push_back(SceneInstance{0, Transform3{}});
  SceneInstance ball;
  ball.mesh = 1;
  ball.transform.position = Vec3{0.2f, -1.2f, 0.3f};
  ball.transform.scale = Vec3{0.8f, 0.8f, 0.8f};
  out.desc.instances.push_back(ball);
  out.camera.position = WorldPos{-1.7, 0.6, 1.5};
  out.camera.target = WorldPos{1.0, -0.8, -0.6};
  out.camera.znear = 0.05f;
  return out;
}

// A slab and 28 cubes of three sizes turned every way: the world translation test's field.
Fixture field_fixture(const std::filesystem::path& dir, WorldPos at) {
  const std::string cube = slashes(dir / "cube.glb");
  REQUIRE(write_cube(cube));
  Fixture out;
  out.name = "a field of cubes on a slab";
  out.desc.meshes.push_back(cube);
  out.desc.ddc = slashes(dir / "ddc");
  SceneInstance slab;
  slab.origin = at;
  slab.transform.position = Vec3{0.0f, -0.25f, 0.0f};
  slab.transform.scale = Vec3{24.0f, 0.5f, 24.0f};
  out.desc.instances.push_back(slab);
  for (i32 z = -2; z <= 1; ++z) {
    for (i32 x = -3; x <= 3; ++x) {
      SceneInstance c;
      c.origin = at;
      c.transform.position = Vec3{static_cast<f32>(x) * 2.25f + 0.125f, 0.5f + 0.25f * (x & 1),
                                  static_cast<f32>(z) * 2.75f - 0.0625f};
      c.transform.rotation =
          quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.3f * static_cast<f32>(x + 3 * z));
      const f32 s = 1.0f + 0.25f * static_cast<f32>((x + z) & 3);
      c.transform.scale = Vec3{s, s, s};
      out.desc.instances.push_back(c);
    }
  }
  out.camera.position = at + DVec3{3.25, 4.5, 9.75};
  out.camera.target = at + DVec3{-0.5, 0.25, -1.5};
  out.camera.znear = 0.05f;
  return out;
}

image::Image image_of(u32 width, u32 height, const Vector<u8>& rgba) {
  image::Image out;
  out.width = width;
  out.height = height;
  out.channels = 4;
  out.pixels.resize(rgba.size());
  std::memcpy(out.pixels.data(), rgba.data(), rgba.size());
  return out;
}

u64 bytes_apart(const CapturedFrame& a, const CapturedFrame& b) {
  u64 out = 0;
  for (u32 i = 0; i < a.color.size(); ++i)
    out += static_cast<u64>(std::abs(static_cast<i32>(a.color[i]) - static_cast<i32>(b.color[i])));
  return out;
}

bool capture_after(Rig& rig, const Camera& camera, u32 frames, CapturedFrame& out) {
  FrameDesc frame;
  frame.camera = camera;
  for (u32 i = 1; i < frames; ++i) {
    frame.frame_index = i - 1;
    if (!rig.renderer.render_offscreen(frame, &rig.error)) return false;
  }
  frame.frame_index = frames - 1;
  return rig.renderer.capture(frame, CaptureChannels{}, out, &rig.error);
}

}  // namespace

TEST_CASE("gi cache: its first frame reads nothing and draws the bytes the mode off draws") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_off");
  const Fixture fixture = box_fixture(tmp.native());
  CapturedFrame off;
  {
    const ScopedTunable mode("renderer.gi.mode", "off");
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
    CHECK(rig.resolved.gi_mode == GiMode::Off);
    REQUIRE_MESSAGE(capture_after(rig, fixture.camera, 1, off), rig.error);
    CHECK(rig.renderer.stats().gpu_gi_trace == 0.0);
  }
  CapturedFrame first;
  CapturedFrame later;
  {
    const ScopedTunable mode("renderer.gi.mode", "sky");
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
    REQUIRE(rig.resolved.gi_mode == GiMode::Sky);
    REQUIRE_MESSAGE(capture_after(rig, fixture.camera, 1, first), rig.error);
    REQUIRE_MESSAGE(capture_after(rig, fixture.camera, 8, later), rig.error);
  }
  REQUIRE(off.color.size() == first.color.size());
  CHECK(std::memcmp(off.color.data(), first.color.data(), off.color.size()) == 0);
  // And the cache, once it has samples, does change the picture: the box is mostly in shadow.
  const u64 moved = bytes_apart(off, later);
  MESSAGE("the box after eight frames of the cache: " << moved
                                                      << " colour steps from the mode off");
  CHECK(moved > 0);
}

TEST_CASE("gi cache: the cache moves the picture towards the reference path tracer") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_reference");
  const Fixture fixtures[] = {box_fixture(tmp.native()), field_fixture(tmp.native(), WorldPos{})};
  for (const Fixture& fixture : fixtures) {
    // The judge: the reference at one bounce — the sun and the environment through the scene's
    // own visibility — with the jitter off, as the resolve-agreement case runs it.
    ReferenceFrame reference;
    image::ImageMetrics metric[2];
    for (u32 m = 0; m < 2; ++m) {
      const ScopedTunable mode("renderer.gi.mode", m == 0 ? "off" : "sky");
      Rig rig;
      REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, m == 0), rig.error);
      CapturedFrame shot;
      REQUIRE_MESSAGE(capture_after(rig, fixture.camera, m == 0 ? 1 : 64, shot), rig.error);
      if (m == 0) {
        ReferenceSettings settings;
        settings.spp = 512;
        settings.batch = 64;
        settings.max_bounces = 1;
        settings.pixel_center = true;
        REQUIRE_MESSAGE(rig.reference.render(fixture.camera, settings, reference, &rig.error),
                        rig.error);
      }
      const image::Image a = image_of(shot.width, shot.height, shot.color);
      const image::Image b = image_of(reference.width, reference.height, reference.color);
      std::string error;
      REQUIRE_MESSAGE(
          image::compare_images(b, a, image::MetricsOptions{}, metric[m], nullptr, &error), error);
      MESSAGE(fixture.name << ", mode " << (m == 0 ? "off" : "sky") << ": FLIP mean "
                           << metric[m].flip_mean << ", p95 " << metric[m].flip_percentile
                           << ", PSNR " << metric[m].psnr);
    }
    CHECK(metric[1].flip_mean < metric[0].flip_mean);
  }
}

TEST_CASE("gi cache: at one pose the change from frame to frame falls and does not grow back") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_converge");
  const Fixture fixture = box_fixture(tmp.native());
  const ScopedTunable mode("renderer.gi.mode", "sky");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
  // Frame 1 clears the cache and frame 2 is its first estimate; from frame 3 on, each frame's
  // change from the one before is the noise the averages still carry.
  constexpr u32 k_frames = 40;
  std::vector<u64> change;
  CapturedFrame previous;
  FrameDesc frame;
  frame.camera = fixture.camera;
  for (u32 i = 0; i < k_frames; ++i) {
    frame.frame_index = i;
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, CaptureChannels{}, shot, &rig.error), rig.error);
    if (i >= 2) change.push_back(bytes_apart(previous, shot));
    previous = std::move(shot);
  }
  auto mean = [&](u32 from, u32 to) {
    u64 sum = 0;
    for (u32 i = from; i < to; ++i)
      sum += change[i];
    return static_cast<f64>(sum) / static_cast<f64>(to - from);
  };
  const f64 early = mean(0, 4);
  const f64 middle = mean(16, 20);
  const f64 late = mean(static_cast<u32>(change.size()) - 4, static_cast<u32>(change.size()));
  MESSAGE("colour steps between frames: " << early << " early, " << middle << " in the middle, "
                                          << late << " late");
  CHECK(early > 0.0);
  CHECK(middle < early);
  CHECK(late <= middle * 1.25);
}

TEST_CASE("gi cache: a scene moved by whole cells draws the same bytes with the cache on") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_translation");
  const ScopedTunable mode("renderer.gi.mode", "sky");
  CapturedFrame home;
  const f64 shifts[] = {0.0, 6548.0, 156250.0};
  for (const f64 shift : shifts) {
    const Fixture fixture = field_fixture(tmp.native(), WorldPos{shift * 64.0, 0.0, -shift * 64.0});
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
    CapturedFrame shot;
    REQUIRE_MESSAGE(capture_after(rig, fixture.camera, 12, shot), rig.error);
    if (shift == 0.0) {
      home = std::move(shot);
      continue;
    }
    const u64 apart = bytes_apart(home, shot);
    MESSAGE("moved by " << shift << " cells: " << apart << " colour steps apart");
    CHECK(apart == 0);
  }
}

TEST_CASE("gi cache: the frames with the cache on make no buffer and allocate as the mode off") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_frame_loop");
  const Fixture fixture = box_fixture(tmp.native());
  u64 allocations[2] = {0, 0};
  for (u32 m = 0; m < 2; ++m) {
    const ScopedTunable mode("renderer.gi.mode", m == 0 ? "off" : "sky");
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
    FrameDesc frame;
    frame.camera = fixture.camera;
    for (u32 i = 0; i < 8; ++i) {
      frame.frame_index = i;
      REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
    }
    const u64 buffers = gfx::buffers_created_on_thread();
    const u64 before = mem::allocations_on_thread();
    for (u32 i = 8; i < 24; ++i) {
      frame.frame_index = i;
      REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
    }
    allocations[m] = mem::allocations_on_thread() - before;
    CHECK(gfx::buffers_created_on_thread() == buffers);
  }
  MESSAGE("engine allocations over sixteen frames: " << allocations[0] << " off, " << allocations[1]
                                                     << " with the cache");
  CHECK(allocations[1] <= allocations[0]);
}
