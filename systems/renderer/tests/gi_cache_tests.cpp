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
//   6. **Stage 4: the light round the eye.** A turn inside the box finds its cells already filled,
//      where main's read them empty; the volume round the eye changes no shadow, id or depth; and
//      on the CPU the volume is the sphere the cull tests.
//
// Every case skips where the machine cannot trace (the cache traces the shadow rays' query).

#include <core/math/math.h>
#include <core/math/world.h>
#include <core/memory/memory.h>
#include <domain/gfx/device.h>
#include <domain/gfx/gi_cache.h>
#include <domain/gfx/resources.h>
#include <foundation/image/metrics.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/eye_shell.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
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

// A unit sphere, 24 x 16 (or as asked), outward.
bool write_sphere(const std::string& path, u32 seg = 24, u32 rings = 16) {
  MeshBuilder m;
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

  bool build(const gfx::Device& device, const SceneDesc& desc, bool with_reference,
             gfx::ResolveMode view_mode = gfx::ResolveMode::Shaded) {
    RenderSettings settings;
    settings.shadows = ShadowMode::RayTraced;
    settings.lights = false;
    settings.view_mode = static_cast<u32>(view_mode);
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

// **A rounded rock** (stage 2's blocks): a smooth sphere 1.5 m across, finely tessellated, resting
// on a slab, seen from 4 m. The slab hides its lower half from the sky, so what its cells hold
// turns with the normal, and the face key splits the normals among six faces.
Fixture rock_fixture(const std::filesystem::path& dir) {
  const std::string rock = slashes(dir / "rock.glb");
  const std::string cube = slashes(dir / "cube.glb");
  REQUIRE(write_sphere(rock, 64, 40));
  REQUIRE(write_cube(cube));
  Fixture out;
  out.name = "a rounded rock on a slab";
  out.desc.meshes.push_back(cube);
  out.desc.meshes.push_back(rock);
  out.desc.ddc = slashes(dir / "ddc");
  SceneInstance slab;
  slab.transform.position = Vec3{0.0f, -0.25f, 0.0f};
  slab.transform.scale = Vec3{16.0f, 0.5f, 16.0f};
  out.desc.instances.push_back(slab);
  SceneInstance ball;
  ball.mesh = 1;
  ball.transform.position = Vec3{0.0f, 0.6f, 0.0f};
  ball.transform.scale = Vec3{0.75f, 0.75f, 0.75f};
  out.desc.instances.push_back(ball);
  out.camera.position = WorldPos{2.4, 1.9, 2.6};
  out.camera.target = WorldPos{0.0, 0.5, 0.0};
  out.camera.znear = 0.05f;
  return out;
}

// The steps of the cache's own term between neighbouring pixels of one instance: the picture with
// the cache less the picture without it, in 8-bit luma, so the direct light, the terminator and the
// highlight cancel and what is left is the indirect term the cells hold. A neighbour pair whose
// difference steps by more than `threshold` is a cell-sized block's edge on a smooth sphere.
u64 cache_steps(const CapturedFrame& with, const CapturedFrame& without, u32 instance,
                i32 threshold) {
  auto luma = [](const CapturedFrame& shot, u32 i) {
    const u8* c = &shot.color[i * 4];
    return 54 * static_cast<i32>(c[0]) + 183 * static_cast<i32>(c[1]) + 19 * static_cast<i32>(c[2]);
  };
  auto term = [&](u32 i) { return luma(with, i) - luma(without, i); };
  auto on = [&](u32 i) {
    return with.ids[i * k_id_words] == instance && without.ids[i * k_id_words] == instance;
  };
  u64 out = 0;
  for (u32 y = 0; y + 1 < with.height; ++y) {
    for (u32 x = 0; x + 1 < with.width; ++x) {
      const u32 i = y * with.width + x;
      if (!on(i)) continue;
      for (const u32 j : {i + 1, i + with.width}) {
        if (on(j) && std::abs(term(i) - term(j)) > threshold * 256) ++out;
      }
    }
  }
  return out;
}

f64 mean_luma(const Vector<u8>& rgba) {
  f64 sum = 0.0;
  for (u32 i = 0; i + 3 < rgba.size(); i += 4)
    sum += 0.2126 * static_cast<f64>(rgba[i]) + 0.7152 * static_cast<f64>(rgba[i + 1]) +
           0.0722 * static_cast<f64>(rgba[i + 2]);
  return sum / static_cast<f64>(rgba.size() / 4);
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

TEST_CASE("gi cache: the bounce moves the picture towards the reference at three bounces") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_bounce");
  const Fixture fixtures[] = {box_fixture(tmp.native()), field_fixture(tmp.native(), WorldPos{})};
  for (const Fixture& fixture : fixtures) {
    // The judge at three bounces: the sun's light off the floor onto the walls and back, which is
    // what the bounce's cache, reading its own cells at every hit, converges towards.
    ReferenceFrame reference;
    const char* modes[3] = {"off", "sky", "bounce"};
    image::ImageMetrics metric[3];
    for (u32 m = 0; m < 3; ++m) {
      const ScopedTunable mode("renderer.gi.mode", modes[m]);
      Rig rig;
      REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, m == 0), rig.error);
      CapturedFrame shot;
      REQUIRE_MESSAGE(capture_after(rig, fixture.camera, m == 0 ? 1 : 64, shot), rig.error);
      if (m == 0) {
        ReferenceSettings settings;
        settings.spp = 512;
        settings.batch = 64;
        settings.max_bounces = 3;
        settings.pixel_center = true;
        REQUIRE_MESSAGE(rig.reference.render(fixture.camera, settings, reference, &rig.error),
                        rig.error);
      }
      const image::Image a = image_of(shot.width, shot.height, shot.color);
      const image::Image b = image_of(reference.width, reference.height, reference.color);
      std::string error;
      REQUIRE_MESSAGE(
          image::compare_images(b, a, image::MetricsOptions{}, metric[m], nullptr, &error), error);
      MESSAGE(fixture.name << ", mode " << std::string(modes[m]) << ", three bounces: FLIP mean "
                           << metric[m].flip_mean << ", p95 " << metric[m].flip_percentile
                           << ", PSNR " << metric[m].psnr << ", mean luma " << mean_luma(shot.color)
                           << " against the reference's " << mean_luma(reference.color));
    }
    CHECK(metric[2].flip_mean < metric[0].flip_mean);
    CHECK(metric[2].flip_mean < metric[1].flip_mean);
    // Stage 2 pinned (2026-11-08, RTX 5090): the young cell's parent, the young cells' rays, the
    // hit's coarser levels and bounded fallback and the faces' blend took the box from 0.125 to
    // 0.105 and the field from 0.048 to 0.042; a tenth of slack over what was measured.
    CHECK(metric[2].flip_mean < (&fixture == &fixtures[0] ? 0.116 : 0.047));
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
  // Stage 4: the volume round the eye and its samples on (their defaults, stated): the samples'
  // directions turn with the cache's frame count, never with a position, and their surfaces' cells
  // are keyed as every pixel's, so they move with the scene by whole cells too.
  const ScopedTunable shell("renderer.gi.shell_m", "48");
  const ScopedTunable shell_rays("renderer.gi.shell_rays", "65536");
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
  // Stage 4's volume round the eye and its samples on (their defaults, stated): the volume is a
  // row of the cull block and the samples ride in the sample pass's dispatch, no buffer of either.
  const ScopedTunable shell("renderer.gi.shell_m", "48");
  const ScopedTunable shell_rays("renderer.gi.shell_rays", "65536");
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

// ---- stage 2 (renderer.md, "Indirect light: the world-space cache", "Stage 2") ----------------

TEST_CASE("gi cache: the young cells' rays never pass the frame's budget") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_budget");
  const Fixture fixture = box_fixture(tmp.native());
  // 3,000 rays over the box's 16,384 pixels: a trickle window of eleven pixels, 1,490 of them, and
  // the young candidates the rest of the budget.
  constexpr u32 k_budget = 3000;
  const ScopedTunable mode("renderer.gi.mode", "bounce");
  const ScopedTunable rays("renderer.gi.rays_per_frame", "3000");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
  const u32 windows =
      (k_width * k_height + gfx::gi_trickle_stride(u64{k_width} * k_height, k_budget) - 1) /
      gfx::gi_trickle_stride(u64{k_width} * k_height, k_budget);
  FrameDesc frame;
  frame.camera = fixture.camera;
  u32 most = 0;
  u32 least = ~0u;
  for (u32 i = 0; i < 48; ++i) {
    frame.frame_index = i;
    REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
    const GiStats& gi = rig.renderer.stats().gi;
    CHECK(gi.rays <= k_budget);
    if (gi.samples == 0) continue;  // the stats of a frame before the first sample
    // Stage 4: the shell's rays (its directions traced, and one more for each that met a surface)
    // are in the budget's count too; what is left is the trickle's and the young cells'.
    const u32 pixel_rays = gi.rays - gi.shell - gi.shell_hits;
    most = std::max(most, pixel_rays);
    least = std::min(least, pixel_rays);
  }
  MESSAGE("rays a frame: " << most << " at most, " << least << " at least, " << windows
                           << " trickle windows, a budget of " << k_budget);
  // The young cells took rays past the trickle while the box filled, and none once it had.
  CHECK(most > windows);
  CHECK(least <= windows);
}

TEST_CASE("gi cache: a held pose fills in within a second's frames") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_fill");
  const Fixture fixture = box_fixture(tmp.native());
  const ScopedTunable mode("renderer.gi.mode", "bounce");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
  // The fill: the first frame from which no stride pixel's own cell is young, and the first from
  // which the change between frames stays under the bound — a twentieth of the first estimate's
  // change, the noise the averages keep.
  constexpr u32 k_frames = 64;
  std::vector<u64> change;
  std::vector<u32> young;
  CapturedFrame previous;
  FrameDesc frame;
  frame.camera = fixture.camera;
  for (u32 i = 0; i < k_frames; ++i) {
    frame.frame_index = i;
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, CaptureChannels{}, shot, &rig.error), rig.error);
    if (i >= 1) change.push_back(bytes_apart(previous, shot));
    young.push_back(rig.renderer.stats().gi.young);
    previous = std::move(shot);
  }
  const u64 first = *std::max_element(change.begin(), change.end());
  const u64 bound = first / 20;
  u32 settled = k_frames;
  for (u32 i = static_cast<u32>(change.size()); i > 0; --i) {
    if (change[i - 1] > bound) break;
    settled = i;  // change[i - 1] is frame i's
  }
  u32 filled = k_frames;
  for (u32 i = static_cast<u32>(young.size()); i > 0; --i) {
    if (young[i - 1] != 0) break;
    filled = i - 1;
  }
  MESSAGE("the box under bounce: the largest change " << first << " colour steps, under " << bound
                                                      << " from frame " << settled
                                                      << "; no young cell from frame " << filled);
  CHECK(filled <= 12);
  CHECK(settled <= 60);
}

TEST_CASE("gi cache: a rounded rock draws no cell-sized blocks where its faces meet") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_rock");
  const Fixture fixture = rock_fixture(tmp.native());
  // Cells 3 to 4 pixels wide at this distance and size, so a block is a few pixels and its edge a
  // step the count sees; the forest's cells are ten pixels at 3840x2160.
  const ScopedTunable scale("renderer.gi.cell_scale", "0.03");
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shots[3];
  const char* names[3] = {"off", "bounce without the faces' blend", "bounce"};
  for (u32 m = 0; m < 3; ++m) {
    const ScopedTunable mode("renderer.gi.mode", m == 0 ? "off" : "bounce");
    const ScopedTunable faces("renderer.gi.face_blend", m == 2 ? "true" : "false");
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
    FrameDesc frame;
    frame.camera = fixture.camera;
    for (u32 i = 0; i + 1 < 64; ++i) {
      frame.frame_index = i;
      REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
    }
    frame.frame_index = 63;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shots[m], &rig.error), rig.error);
  }
  const u64 hard = cache_steps(shots[1], shots[0], 1, 2);
  const u64 blended = cache_steps(shots[2], shots[0], 1, 2);
  MESSAGE("the rock's cache term, steps of more than 2 in 255 between neighbours: "
          << hard << " " << std::string(names[1]) << ", " << blended << " with it");
  CHECK(blended < hard);
}

TEST_CASE("gi cache: stage 2's levers one at a time, against the reference at three bounces") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_levers");
  const Fixture fixtures[] = {box_fixture(tmp.native()), field_fixture(tmp.native(), WorldPos{})};
  // Stage 1's read, each lever alone on it, then each added in turn (the last of those is stage
  // 2), then stage 2 with the far lift. Stage 2b: the table the experiment page holds.
  struct Levers {
    const char* name;
    const char* confident;
    const char* evict;
    const char* young;
    const char* fallback;
    const char* faces;
    const char* far_lift;
    const char* shell;       // stage 4: the volume round the eye, metres (0: none)
    const char* shell_rays;  // stage 4: the samples round the eye a frame (0: none)
  };
  // Stage 4 widened it: every row before stage 4's carries the volume and the samples off, which
  // is the pass and the cull block exactly as they were (no volume fitted, no thread past the
  // trickle windows, the young half whole); then each alone on stage 2, and both.
  const Levers steps[] = {
      {"stage 1", "1", "0", "false", "false", "false", "0", "0", "0"},
      {"alone: the young cell reads its parent", "32", "0", "false", "false", "false", "0", "0",
       "0"},
      {"alone: a full bucket's stale cells freed", "1", "16", "false", "false", "false", "0", "0",
       "0"},
      {"alone: rays to the young cells", "1", "0", "true", "false", "false", "0", "0", "0"},
      {"alone: the hit's coarser levels and fallback", "1", "0", "false", "true", "false", "0", "0",
       "0"},
      {"alone: the faces' blend", "1", "0", "false", "false", "true", "0", "0", "0"},
      {"+ the young cell reads its parent", "32", "0", "false", "false", "false", "0", "0", "0"},
      {"+ a full bucket's stale cells freed", "32", "16", "false", "false", "false", "0", "0", "0"},
      {"+ rays to the young cells", "32", "16", "true", "false", "false", "0", "0", "0"},
      {"+ the hit's coarser levels and fallback", "32", "16", "true", "true", "false", "0", "0",
       "0"},
      {"+ the faces' blend (stage 2)", "32", "16", "true", "true", "true", "0", "0", "0"},
      {"stage 2 + the far lift at 0.5", "32", "16", "true", "true", "true", "0.5", "0", "0"},
      {"stage 2 + the far lift at 1", "32", "16", "true", "true", "true", "1", "0", "0"},
      {"stage 2 + the volume round the eye", "32", "16", "true", "true", "true", "0", "48", "0"},
      {"stage 2 + the samples round the eye", "32", "16", "true", "true", "true", "0", "0",
       "65536"},
      {"+ both (stage 4)", "32", "16", "true", "true", "true", "0", "48", "65536"},
  };
  constexpr u32 k_steps = sizeof(steps) / sizeof(steps[0]);
  constexpr u32 k_stage2 = 10;
  constexpr u32 k_stage4 = 15;
  const ScopedTunable mode("renderer.gi.mode", "bounce");
  for (const Fixture& fixture : fixtures) {
    ReferenceFrame reference;
    f64 flip[k_steps];
    for (u32 s = 0; s < k_steps; ++s) {
      const ScopedTunable confident("renderer.gi.confident", steps[s].confident);
      const ScopedTunable evict("renderer.gi.evict_age", steps[s].evict);
      const ScopedTunable young("renderer.gi.young_rays", steps[s].young);
      const ScopedTunable fallback("renderer.gi.hit_fallback", steps[s].fallback);
      const ScopedTunable faces("renderer.gi.face_blend", steps[s].faces);
      const ScopedTunable far_lift("renderer.gi.far_lift", steps[s].far_lift);
      const ScopedTunable shell("renderer.gi.shell_m", steps[s].shell);
      const ScopedTunable shell_rays("renderer.gi.shell_rays", steps[s].shell_rays);
      Rig rig;
      REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, s == 0), rig.error);
      CapturedFrame shot;
      REQUIRE_MESSAGE(capture_after(rig, fixture.camera, 64, shot), rig.error);
      // With both knobs at 0 no view carries the volume and no direction is traced: the frame is
      // the one stage 2 recorded.
      if (std::string(steps[s].shell) == "0") CHECK(rig.renderer.stats().gi_shell_views == 0);
      if (std::string(steps[s].shell_rays) == "0") CHECK(rig.renderer.stats().gi.shell == 0);
      if (s == 0) {
        ReferenceSettings settings;
        settings.spp = 512;
        settings.batch = 64;
        settings.max_bounces = 3;
        settings.pixel_center = true;
        REQUIRE_MESSAGE(rig.reference.render(fixture.camera, settings, reference, &rig.error),
                        rig.error);
      }
      image::ImageMetrics metric;
      std::string error;
      REQUIRE_MESSAGE(
          image::compare_images(image_of(reference.width, reference.height, reference.color),
                                image_of(shot.width, shot.height, shot.color),
                                image::MetricsOptions{}, metric, nullptr, &error),
          error);
      flip[s] = static_cast<f64>(metric.flip_mean);
      MESSAGE(fixture.name << ", " << std::string(steps[s].name) << ": FLIP mean "
                           << metric.flip_mean << ", p95 " << metric.flip_percentile << ", PSNR "
                           << metric.psnr << ", mean luma " << mean_luma(shot.color)
                           << " against the reference's " << mean_luma(reference.color));
    }
    // Stage 2 is at least as near the reference as stage 1 on both fixtures, and stage 4 — whose
    // parts are for the cells behind the eye — no further from it than stage 2 (2026-11-09: a
    // hundredth of FLIP's mean of slack, a sixth of what stage 2 gained).
    CHECK(flip[k_stage2] <= flip[0]);
    CHECK(flip[k_stage4] <= flip[k_stage2] + 0.01);
  }
}

// ---- stage 4 (renderer.md, "Indirect light: the world-space cache", "Stage 4") ----------------

// The share of the cache's own term (the picture under `bounce` less the picture under `off`, in
// 8-bit luma) by which `frame` stands from `converged`, over the pixels on the same surface in all
// four: the mean |term - converged term| over the mean |converged term|.
static f64 term_error(const CapturedFrame& frame, const CapturedFrame& frame_off,
                      const std::vector<f64>& converged, f64& magnitude) {
  auto luma = [](const CapturedFrame& shot, u32 i) {
    const u8* c = &shot.color[i * 4];
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
  };
  f64 error = 0.0;
  magnitude = 0.0;
  const u32 pixels = frame.width * frame.height;
  for (u32 i = 0; i < pixels; ++i) {
    error += std::abs(luma(frame, i) - luma(frame_off, i) - converged[i]);
    magnitude += std::abs(converged[i]);
  }
  error /= static_cast<f64>(pixels);
  magnitude /= static_cast<f64>(pixels);
  return magnitude > 0.0 ? error / magnitude : 0.0;
}

// **A turn finds the light already there** (the owner's v37 walk under `bounce`, 2026-11-09: "the
// problem is the filling being visible at all"). The camera inside the closed box — geometry all
// round it — held until the cache has converged, turned 180 degrees about the vertical in four
// frames, then held. Main's cache had no cell behind the eye (only the picture's pixels sample) and
// no geometry there in the frame's structures, so the first frame after the turn read its cells
// empty or young. With the volume round the eye and its samples, the cells the turned view reads
// were filled while they were behind it.
//
// The bound comes from the cache's own noise, not from the first frame's value: a converged cell
// is the mean of up to 256 samples, and what is left of its noise is what the converged frames
// show — each of the last eight frames' distance from their mean, as a share of the term's
// magnitude (0.006 to 0.007 here, 2026-11-09, RTX 5090; far under the 1/16 a 256-sample mean of a
// sample that spread as widely as the term itself would show, so that was not the bound). The
// first frame after the turn must stand within three times the largest of the runs' converged
// noise from its converged picture; the same run without the shell (main's bytes) must not. The fixture is the forest's in pixels and rays, as stage 3's turn: a
// cell about ten pixels wide (`cell_scale` 0.12 at 128 wide) and the budget of one pixel in nine of
// the trickle, a quarter of which (the shell's share, half the young half) is 455 directions a
// frame.
TEST_CASE("gi cache: a turn finds the light round the eye already there") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_turn");
  const Fixture fixture = box_fixture(tmp.native());
  constexpr u32 k_hold = 48;
  constexpr u32 k_turn = 4;
  constexpr u32 k_after = 40;
  constexpr u32 k_frames = k_hold + k_turn + k_after;
  constexpr usize k_tail = 8;
  const DVec3 ahead = fixture.camera.target - fixture.camera.position;
  auto camera_at = [&](u32 i) {
    const f64 share = i < k_hold ? 0.0 : std::min(1.0, static_cast<f64>(i - k_hold + 1) / k_turn);
    const f64 a = 3.14159265358979 * share;
    Camera c = fixture.camera;
    c.target = c.position + DVec3{ahead.x * std::cos(a) + ahead.z * std::sin(a), ahead.y,
                                  -ahead.x * std::sin(a) + ahead.z * std::cos(a)};
    return c;
  };
  const ScopedTunable scale("renderer.gi.cell_scale", "0.12");
  const ScopedTunable rays("renderer.gi.rays_per_frame", "3641");
  struct Run {
    const char* name;
    const char* mode;
    const char* shell;
    const char* shell_rays;
  };
  const Run runs[] = {{"off", "off", "0", "0"},
                      {"without the shell (main)", "bounce", "0", "0"},
                      {"the volume round the eye alone", "bounce", "48", "0"},
                      {"the samples round the eye alone", "bounce", "0", "65536"},
                      {"stage 4", "bounce", "48", "65536"}};
  constexpr u32 k_runs = sizeof(runs) / sizeof(runs[0]);
  std::vector<CapturedFrame> shots[k_runs];
  u32 shell_hits[k_runs] = {};
  for (u32 r = 0; r < k_runs; ++r) {
    const ScopedTunable mode("renderer.gi.mode", runs[r].mode);
    const ScopedTunable shell("renderer.gi.shell_m", runs[r].shell);
    const ScopedTunable shell_rays("renderer.gi.shell_rays", runs[r].shell_rays);
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false), rig.error);
    FrameDesc frame;
    for (u32 i = 0; i < k_frames; ++i) {
      frame.frame_index = i;
      frame.camera = camera_at(i);
      if (i + 1 < k_hold + k_turn) {
        REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
        shell_hits[r] = std::max(shell_hits[r], rig.renderer.stats().gi.shell_hits);
        continue;
      }
      CapturedFrame shot;
      REQUIRE_MESSAGE(rig.renderer.capture(frame, CaptureChannels{}, shot, &rig.error), rig.error);
      shots[r].push_back(std::move(shot));
    }
  }
  const usize n = shots[0].size();
  const u32 pixels = shots[0][0].width * shots[0][0].height;
  f64 first[k_runs] = {};
  f64 settled[k_runs] = {};
  for (u32 r = 1; r < k_runs; ++r) {
    // The converged term: the last eight frames', pixel by pixel.
    std::vector<f64> converged(pixels, 0.0);
    for (usize k = n - k_tail; k < n; ++k) {
      for (u32 i = 0; i < pixels; ++i) {
        const u8* c = &shots[r][k].color[i * 4];
        const u8* o = &shots[0][k].color[i * 4];
        converged[i] += (0.2126 * (c[0] - o[0]) + 0.7152 * (c[1] - o[1]) + 0.0722 * (c[2] - o[2])) /
                        static_cast<f64>(k_tail);
      }
    }
    f64 magnitude = 0.0;
    first[r] = term_error(shots[r][0], shots[0][0], converged, magnitude);
    for (usize k = n - k_tail; k < n; ++k)
      settled[r] +=
          term_error(shots[r][k], shots[0][k], converged, magnitude) / static_cast<f64>(k_tail);
    std::string curve;
    for (usize k = 0; k < n; k += (k < 8 ? 1 : 8)) {
      f64 m = 0.0;
      curve += " " + std::to_string(k) + ":" +
               std::to_string(term_error(shots[r][k], shots[0][k], converged, m)).substr(0, 5);
    }
    MESSAGE(std::string(runs[r].name)
            << ": the term's error after the turn, as a share of its magnitude " << magnitude
            << " (frame:share)" << curve << "; the converged frames' own " << settled[r]
            << "; shell directions that met a surface, most in a frame: " << shell_hits[r]);
  }
  const f64 noise = *std::max_element(settled + 1, settled + k_runs);
  const f64 k_bound = 3.0 * noise;
  MESSAGE("the bound: three times the converged frames' own noise, " << k_bound);
  CHECK(first[4] <= k_bound);
  CHECK(first[1] > k_bound);  // main's bytes miss it: the test sees what it is for
  CHECK(shell_hits[1] == 0);
  CHECK(shell_hits[4] > 0);
}

// **The direct picture does not change** (stage 4): a caster round the eye outside the sun's swept
// volume can shadow no receiver in view (the sweep is every point a sun ray from the view reaches),
// so the sun's shadow, the ids and the depth are the same bytes with the volume as without it.
// `ResolveMode::Shadow` draws the sun's term alone; the volume is fitted wherever the cache is
// made, so the resolve's debug mode keeps it (`Stats::gi_shell_views`), and it does add casters.
TEST_CASE("gi cache: the volume round the eye changes no shadow, id or depth") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("gi cache unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_gi_shell_direct");
  const Fixture fixtures[] = {box_fixture(tmp.native()), field_fixture(tmp.native(), WorldPos{})};
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  const ScopedTunable mode("renderer.gi.mode", "bounce");
  for (const Fixture& fixture : fixtures) {
    // Each fixture seen the other way, so its geometry stands behind the eye, where only the
    // volume round the eye (or the sun's sweep) keeps it in the structures.
    Camera camera = fixture.camera;
    camera.target = camera.position + (camera.position - fixture.camera.target);
    CapturedFrame shots[2];
    u32 casters[2] = {0, 0};
    u32 views[2] = {0, 0};
    for (u32 m = 0; m < 2; ++m) {
      const ScopedTunable shell("renderer.gi.shell_m", m == 0 ? "0" : "48");
      Rig rig;
      REQUIRE_MESSAGE(rig.build(gpu.device, fixture.desc, false, gfx::ResolveMode::Shadow),
                      rig.error);
      REQUIRE_MESSAGE(capture_after(rig, camera, 4, shots[m]), rig.error);
      FrameDesc frame;
      frame.camera = camera;
      frame.frame_index = 4;
      CapturedFrame again;
      REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, again, &rig.error), rig.error);
      shots[m] = std::move(again);
      casters[m] = rig.renderer.stats().shadow_casters;
      views[m] = rig.renderer.stats().gi_shell_views;
    }
    MESSAGE(fixture.name << ": casters " << casters[0] << " without the volume, " << casters[1]
                         << " with it; colour steps apart " << bytes_apart(shots[0], shots[1]));
    CHECK(views[0] == 0);
    CHECK(views[1] == 1);
    // The field's cubes behind the eye are outside the sun's sweep, so the volume adds casters
    // there and the comparison sees something; the box's walls are inside the sweep already.
    if (&fixture == &fixtures[1]) CHECK(casters[1] > casters[0]);
    REQUIRE(shots[0].color.size() == shots[1].color.size());
    CHECK(std::memcmp(shots[0].color.data(), shots[1].color.data(), shots[0].color.size()) == 0);
    REQUIRE(shots[0].ids.size() == shots[1].ids.size());
    CHECK(std::memcmp(shots[0].ids.data(), shots[1].ids.data(),
                      shots[0].ids.size() * sizeof(u32)) == 0);
    REQUIRE(shots[0].depth.size() == shots[1].depth.size());
    CHECK(std::memcmp(shots[0].depth.data(), shots[1].depth.data(),
                      shots[0].depth.size() * sizeof(f32)) == 0);
  }
}

// The volume round the eye on the CPU (eye_shell.h): the cull pass's test, which is
// `caster_volume_contains_sphere`, keeps a cluster sphere exactly when it comes within the radius
// of the eye, over a lattice of spheres in and round the cube that bounds it.
TEST_CASE("gi cache: the volume round the eye keeps the spheres that come within its radius") {
  const Vec3 eye{3.5f, -2.0f, 7.25f};
  constexpr f32 k_radius = 24.0f;
  CasterVolume volume;
  REQUIRE(fit_eye_shell_volume(eye, k_radius, volume));
  CHECK(volume.plane_count == 6);
  CasterVolume none;
  CHECK_FALSE(fit_eye_shell_volume(eye, 0.0f, none));
  CHECK(none.plane_count == 0);
  u32 kept = 0;
  u32 wrong = 0;
  for (i32 z = -8; z <= 8; ++z) {
    for (i32 y = -8; y <= 8; ++y) {
      for (i32 x = -8; x <= 8; ++x) {
        const Vec3 c = eye + Vec3{static_cast<f32>(x) * 4.1f, static_cast<f32>(y) * 3.7f,
                                  static_cast<f32>(z) * 4.3f};
        const f32 r = 0.5f + 0.75f * static_cast<f32>((x + 2 * y + 3 * z + 60) % 5);
        const Vec3 d = c - eye;
        const f32 distance = std::sqrt(dot(d, d));
        // A margin of a millimetre either side of the boundary, where float rounding decides.
        if (std::fabs(distance - (k_radius + r)) < 1e-3f) continue;
        const bool expected = distance <= k_radius + r;
        const bool got = caster_volume_contains_sphere(volume, c, r);
        kept += got ? 1u : 0u;
        wrong += got != expected ? 1u : 0u;
      }
    }
  }
  MESSAGE("spheres kept: " << kept << " of " << 17 * 17 * 17);
  CHECK(kept > 0);
  CHECK(wrong == 0);
}
