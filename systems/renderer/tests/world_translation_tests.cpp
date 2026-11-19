// The translation suite (ADR-0053 decision 6; docs/subsystems/renderer.md, "The frame's origin").
//
// **A scene and its camera moved by whole cells draw the same bytes.** The GPU never holds an
// absolute position: an instance is a 64 m cell and a float offset into it, the frame's origin is
// its eye, and every reader measures an instance from the eye with the same arithmetic
// (`instance_from_eye`, shaders/scene.slang). Moving the scene and the eye by the same whole number
// of cells changes no operand any pass sees, so every pass must produce the same bits: the
// visibility buffer (ids and depth) and the colour, on the mesh, software and both vertex
// rasterizers, the ray visibility pass, with traced shadows and with the cascaded maps. An absolute
// float32 anywhere on a path fails this by centimetres 420 km out, which is why the rule is held by
// this test and not by reading the code. The one exception is by design: the cascaded maps' texels
// are fixed in the world (renderer.md, "Cascaded shadow maps"), so a move by whole cells moves the
// scene against them, and their colour is held to the same bytes outside a band round the shadows'
// edges as wide as the filter's footprint; ids and depth are held everywhere.
//
// The scene is rigid cubes on a slab at positions that are whole 1024ths of a metre, so f64 holds
// every position exactly at 1e8 m and a move by whole cells is exact too. Moved by 6,548 cells
// (419 km, the owner's distance), 156,250 (10,000 km) and 1,562,500 (1e8 m), and a second scene
// whose eye stands a millimetre from a cell's edge with cubes on both sides of it. Then the eye
// stepped a millimetre at a time 10,000 km out against the same steps by the origin. Two thirds of
// the cubes carry a hand-off role (G136), and the first test sets a band across the scene (the
// second keeps the default, 21 to 29 m, which drops every far form), so the cull pass's distance
// band, measured from the eye like every other test, is held to the same bytes.
#include "test_device.h"

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

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

// A unit cube as a GLB: 24 vertices with normals and UVs, twelve triangles, one plain material
// (renderer_tests.cpp's, without the morph target).
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

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings,
             u32 width, u32 height) {
    if (!load_scene(desc, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

// A tunable set for the length of a scope and put back after it (rt_capacity_tests.cpp's).
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

// Whole cells along x and z: the scene is moved along both, so the cells' difference is exercised
// on two axes and y stays where the ground is.
DVec3 cells(f64 count) { return DVec3{count * k_world_cell_m, 0.0, -count * k_world_cell_m}; }

// The scene: a slab to stand on and catch shadows, and cubes on it turned by a few angles and
// scaled, at whole 1024ths of a metre from `at`, the scene's own origin (`SceneInstance::origin`).
SceneDesc scene_at(const std::string& mesh, const std::string& ddc, WorldPos at) {
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = ddc;
  SceneInstance slab;
  slab.origin = at;
  slab.transform.position = Vec3{0.0f, -0.25f, 0.0f};
  slab.transform.scale = Vec3{24.0f, 0.5f, 24.0f};
  desc.instances.push_back(slab);
  for (i32 z = -2; z <= 1; ++z) {
    for (i32 x = -3; x <= 3; ++x) {
      SceneInstance cube;
      cube.origin = at;
      cube.transform.position = Vec3{static_cast<f32>(x) * 2.25f + 0.125f, 0.5f + 0.25f * (x & 1),
                                     static_cast<f32>(z) * 2.75f - 0.0625f};
      cube.transform.rotation =
          quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.3f * static_cast<f32>(x + 3 * z));
      const f32 s = 1.0f + 0.25f * static_cast<f32>((x + z) & 3);
      cube.transform.scale = Vec3{s, s, s};
      // **With the hand-off's roles** (G136; renderer.md, "The hand-off by distance"): a third of
      // the cubes are a near form and a third a far one, under the band the test's tunables set
      // across the scene, so the cull pass's band test is held to the same bytes too.
      const i32 third = (x + 3 + 7 * (z + 2)) % 3;
      cube.handoff = third == 1   ? scene_gen::k_handoff_near
                     : third == 2 ? scene_gen::k_handoff_far
                                  : u8{0};
      desc.instances.push_back(cube);
    }
  }
  return desc;
}

struct Path {
  const char* name;
  RasterMode raster;
  ShadowMode shadows;
  bool capacity;  // the vertex path without geometryShader: the capacity draw
};

struct Picture {
  CapturedFrame frame;
  bool ok = false;
};

// Every frame of `eyes` for a scene placed at `at`, on one path, on its own device; `guard`, when
// given, the nodes' guard's two counts after the last (R188: failures, instances checked).
bool draw(const Path& path, const std::string& mesh, const std::string& ddc, WorldPos at,
          const std::vector<Camera>& cameras, std::vector<CapturedFrame>& out, std::string& why,
          u32* guard = nullptr) {
  gfx::DeviceOptions options = renderer_test::device_options();
  if (path.capacity) {
    options.overrides.absent.push_back("VK_EXT_mesh_shader");
    options.overrides.absent.push_back("geometryShader");
  }
  gfx::Device device;
  if (!device.create(options, &why)) return false;
  bool ok = false;
  {
    RenderSettings settings;
    settings.raster = path.raster;
    settings.shadows = path.shadows;
    settings.dither = true;
    Rig rig;
    ok = rig.build(device, scene_at(mesh, ddc, at), settings, 192, 128);
    if (!ok) why = rig.error;
    CaptureChannels channels;
    channels.ids = true;
    channels.depth = true;
    out.clear();
    for (const Camera& camera : cameras) {
      if (!ok) break;
      FrameDesc frame;
      frame.camera = camera;
      frame.camera.position += at - WorldPos::origin();
      frame.camera.target += at - WorldPos::origin();
      CapturedFrame captured;
      ok = rig.renderer.capture(frame, channels, captured, &why);
      out.push_back(std::move(captured));
    }
    if (guard != nullptr) (void)rig.renderer.cull_node_guard(guard[0], guard[1]);
  }
  device.destroy();
  return ok;
}

// The bytes two frames differ by, in each channel.
struct Difference {
  u64 color = 0;
  u64 ids = 0;
  u64 depth = 0;
};

Difference compare(const CapturedFrame& a, const CapturedFrame& b) {
  Difference d;
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(a.ids.size() == b.ids.size());
  REQUIRE(a.depth.size() == b.depth.size());
  for (u32 i = 0; i < a.color.size(); ++i)
    d.color += a.color[i] != b.color[i] ? 1u : 0u;
  for (u32 i = 0; i < a.ids.size(); ++i)
    d.ids += a.ids[i] != b.ids[i] ? 1u : 0u;
  if (!a.depth.empty() && std::memcmp(a.depth.data(), b.depth.data(), a.depth.size() * 4) != 0) {
    for (u32 i = 0; i < a.depth.size(); ++i) {
      u32 x = 0;
      u32 y = 0;
      std::memcpy(&x, &a.depth[i], 4);
      std::memcpy(&y, &b.depth[i], 4);
      d.depth += x != y ? 1u : 0u;
    }
  }
  return d;
}

const Path k_paths[] = {
    {"mesh", RasterMode::Hardware, ShadowMode::Off, false},
    {"software", RasterMode::Software, ShadowMode::Off, false},
    {"vertex indexed", RasterMode::Vertex, ShadowMode::Off, false},
    {"vertex capacity", RasterMode::Vertex, ShadowMode::Off, true},
    {"ray visibility", RasterMode::RayTrace, ShadowMode::Off, false},
    {"traced shadows", RasterMode::Hardware, ShadowMode::RayTraced, false},
    {"cascaded maps", RasterMode::Hardware, ShadowMode::Cascaded, false},
};

// **The cascaded maps' texels are the world's** (renderer.md, "Cascaded shadow maps"): a cascade's
// centre snaps to whole texels in the world's light space, so a scene and its camera moved by whole
// cells fall on the texel grid differently, and a shadow's filtered edge moves by up to a texel
// against the scene. Everywhere else the maps answer the same, wholly lit or wholly shadowed, and
// the bytes are the same. The band left out is every pixel within `k_edge_pixels` of a pixel whose
// shadowed-ness differs from its own, in either picture — the way the cascaded maps' reference test
// leaves out the filter's footprint (shadow_map_tests.cpp, `k_edge_pixels`). A pixel is shadowed
// where its colour differs from the same frame drawn without shadows. Here a texel is a centimetre
// or two against a pixel of about ten, so the 4 x 4 footprint is inside one pixel and two cover it.
constexpr i32 k_edge_pixels = 2;
// The shadowed pixels the comparison must hold away from the band, so it is about shadows at all:
// the cubes' shadows are thin at 192 x 128, 93 to 272 pixels of them outside it (RTX 5090).
constexpr u64 k_min_shadowed = 64;

struct Band {
  Vector<u8> edge;   // per pixel: 1 inside the band
  u64 pixels = 0;    // in the band
  u64 shadowed = 0;  // outside it, shadowed in the first picture
};

bool shadowed(const CapturedFrame& unshadowed, const CapturedFrame& f, u32 p) {
  for (u32 c = 0; c < 3; ++c) {
    if (f.color[u64{p} * 4 + c] != unshadowed.color[u64{p} * 4 + c]) return true;
  }
  return false;
}

Band shadow_band(const CapturedFrame& unshadowed, const CapturedFrame& a, const CapturedFrame& b) {
  const i32 w = static_cast<i32>(a.width);
  const i32 h = static_cast<i32>(a.height);
  Vector<u8> sa(a.width * a.height, u8{0});
  Vector<u8> sb(a.width * a.height, u8{0});
  for (u32 p = 0; p < a.width * a.height; ++p) {
    sa[p] = shadowed(unshadowed, a, p) ? 1 : 0;
    sb[p] = shadowed(unshadowed, b, p) ? 1 : 0;
  }
  Band out;
  out.edge = Vector<u8>(a.width * a.height, u8{0});
  for (i32 y = 0; y < h; ++y) {
    for (i32 x = 0; x < w; ++x) {
      const u32 p = static_cast<u32>(y * w + x);
      bool edge = false;
      for (i32 dy = -k_edge_pixels; dy <= k_edge_pixels && !edge; ++dy) {
        for (i32 dx = -k_edge_pixels; dx <= k_edge_pixels && !edge; ++dx) {
          const i32 qx = x + dx;
          const i32 qy = y + dy;
          if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
          const u32 q = static_cast<u32>(qy * w + qx);
          edge = sa[q] != sa[p] || sb[q] != sb[p];
        }
      }
      out.edge[p] = edge ? 1 : 0;
      out.pixels += edge ? 1u : 0u;
      out.shadowed += !edge && sa[p] != 0 ? 1u : 0u;
    }
  }
  return out;
}

// The colour bytes two frames differ by outside the band.
u64 colour_outside(const CapturedFrame& a, const CapturedFrame& b, const Band& band) {
  u64 out = 0;
  for (u32 p = 0; p < a.width * a.height; ++p) {
    if (band.edge[p] != 0) continue;
    for (u32 c = 0; c < 4; ++c)
      out += a.color[u64{p} * 4 + c] != b.color[u64{p} * 4 + c] ? 1u : 0u;
  }
  return out;
}

}  // namespace

TEST_CASE("world translation: a scene and its camera moved by whole cells draw the same bytes") {
  {
    gfx::Device probe;
    std::string why;
    if (!probe.create(renderer_test::device_options(), &why)) {
      MESSAGE("renderer unavailable here: " << why);
      return;
    }
    if (!probe.features().buffer_int64_atomics) {
      MESSAGE("no 64-bit buffer atomics here");
      probe.destroy();
      return;
    }
    probe.destroy();
  }
  const test::TempDir tmp("engine_world_translation");
  // The hand-off's band, 8 to 12 m from the eye, across the cubes of both layouts (the cubes stand
  // 3 to 19 m from the eyes): the near forms past it and the far forms short of it are dropped.
  const ScopedTunable near_m("renderer.flora.sward_near_m", "10");
  const ScopedTunable fade_m("renderer.flora.sward_fade_m", "12");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string ddc = slashes(dir / "ddc");
  REQUIRE(write_cube_glb(mesh));

  // Two scenes by the origin: the eye over the cubes, and the eye a millimetre short of the cell
  // edge at x = 64 with the cubes either side of it (the scene's origin moved to x = 60).
  struct Layout {
    const char* name;
    WorldPos origin;
    Camera camera;
  };
  Camera over;
  over.position = WorldPos{3.25, 4.5, 9.75};
  over.target = WorldPos{-0.5, 0.25, -1.5};
  over.znear = 0.05f;
  Camera edge;
  edge.position = WorldPos{64.0 - 1.0 / 1024.0, 2.5, 6.0};
  edge.target = WorldPos{60.0, 0.5, -2.0};
  edge.znear = 0.05f;
  const Layout layouts[] = {{"over the cubes", WorldPos{}, over},
                            {"a millimetre from a cell's edge", WorldPos{60.0, 0.0, 0.0}, edge}};
  const f64 shifts[] = {6548.0, 156250.0, 1562500.0};

  for (const Path& path : k_paths) {
    for (const Layout& layout : layouts) {
      // The eye's place in the scene is the layout's; `draw` moves it with the scene.
      Camera local = layout.camera;
      local.position -= layout.origin - WorldPos::origin();
      local.target -= layout.origin - WorldPos::origin();
      const std::vector<Camera> cameras{local};
      std::vector<CapturedFrame> home;
      std::string why;
      if (!draw(path, mesh, ddc, layout.origin, cameras, home, why)) {
        MESSAGE(std::string(path.name) << ": not drawn here: " << why);
        break;
      }
      REQUIRE(home.size() == 1);
      REQUIRE(home[0].covered > home[0].width * home[0].height / 8);
      // The cascaded maps are held to the same bytes outside their texels' band (`Band`), which
      // the same frame without shadows finds.
      const bool maps = path.shadows == ShadowMode::Cascaded;
      std::vector<CapturedFrame> unshadowed;
      if (maps) {
        REQUIRE_MESSAGE(draw(k_paths[0], mesh, ddc, layout.origin, cameras, unshadowed, why), why);
      }
      for (const f64 shift : shifts) {
        std::vector<CapturedFrame> far;
        REQUIRE_MESSAGE(draw(path, mesh, ddc, layout.origin + cells(shift), cameras, far, why),
                        why);
        const Difference d = compare(home[0], far[0]);
        if (maps) {
          const Band band = shadow_band(unshadowed[0], home[0], far[0]);
          const u64 outside = colour_outside(home[0], far[0], band);
          MESSAGE(std::string(path.name)
                  << ", " << std::string(layout.name) << ", moved by " << shift
                  << " cells: " << outside << " colour bytes differ outside the shadows' edges ("
                  << band.pixels << " pixels, " << d.color << " bytes in all; " << band.shadowed
                  << " shadowed pixels compared), " << d.ids << " id words, " << d.depth
                  << " depths differ");
          // The comparison means something: there is shadow away from its edges.
          CHECK(band.shadowed >= k_min_shadowed);
          CHECK(outside == 0);
        } else {
          MESSAGE(std::string(path.name) << ", " << std::string(layout.name) << ", moved by "
                                         << shift << " cells: " << d.color << " colour bytes, "
                                         << d.ids << " id words, " << d.depth << " depths differ");
          CHECK(d.color == 0);
        }
        CHECK(d.ids == 0);
        CHECK(d.depth == 0);
      }
    }
  }
}

TEST_CASE(
    "world translation: the eye stepped a millimetre at a time 10,000 km out draws the "
    "origin's sequence") {
  {
    gfx::Device probe;
    std::string why;
    if (!probe.create(renderer_test::device_options(), &why)) {
      MESSAGE("renderer unavailable here: " << why);
      return;
    }
    probe.destroy();
  }
  const test::TempDir tmp("engine_world_translation_steps");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string ddc = slashes(dir / "ddc");
  REQUIRE(write_cube_glb(mesh));
  // Twelve frames, the eye a 1024th of a metre further along x each, looking along the same line;
  // at 10,000 km a float32 eye would not have moved at all (its step there is a metre).
  std::vector<Camera> steps;
  for (u32 k = 0; k < 12; ++k) {
    Camera c;
    const f64 dx = static_cast<f64>(k) / 1024.0;
    c.position = WorldPos{2.0 + dx, 1.25, 5.5};
    c.target = WorldPos{-1.0 + dx, 0.5, -2.0};
    c.znear = 0.05f;
    steps.push_back(c);
  }
  const Path paths[] = {k_paths[0], k_paths[6]};
  for (const Path& path : paths) {
    std::vector<CapturedFrame> home;
    std::vector<CapturedFrame> far;
    std::string why;
    if (!draw(path, mesh, ddc, WorldPos{}, steps, home, why)) {
      MESSAGE(std::string(path.name) << ": not drawn here: " << why);
      continue;
    }
    REQUIRE_MESSAGE(draw(path, mesh, ddc, WorldPos{} + cells(156250.0), steps, far, why), why);
    REQUIRE(home.size() == steps.size());
    REQUIRE(far.size() == steps.size());
    // The cascaded maps outside their texels' band, as above.
    const bool maps = path.shadows == ShadowMode::Cascaded;
    std::vector<CapturedFrame> unshadowed;
    if (maps) REQUIRE_MESSAGE(draw(k_paths[0], mesh, ddc, WorldPos{}, steps, unshadowed, why), why);
    u64 differing = 0;
    u64 moved = 0;
    u64 band_pixels = 0;
    for (u32 k = 0; k < steps.size(); ++k) {
      const Difference d = compare(home[k], far[k]);
      if (maps) {
        const Band band = shadow_band(unshadowed[k], home[k], far[k]);
        band_pixels += band.pixels;
        CHECK(band.shadowed >= k_min_shadowed);
        differing += colour_outside(home[k], far[k], band) + d.ids + d.depth;
      } else {
        differing += d.color + d.ids + d.depth;
      }
      // And the steps are steps: each frame's depth differs from the one before somewhere.
      if (k > 0) moved += compare(home[k], home[k - 1]).depth > 0 ? 1u : 0u;
    }
    const std::string band_note =
        maps ? " outside the shadows' edges (" + std::to_string(band_pixels) + " edge pixels)"
             : std::string();
    MESSAGE(std::string(path.name)
            << ": " << differing << " differing bytes or words over " << steps.size()
            << " millimetre steps" << band_note << "; " << moved << " of " << steps.size() - 1
            << " steps moved the picture");
    CHECK(differing == 0);
    CHECK(moved == steps.size() - 1);
  }
}

// **Nodes before instances change no byte** (R188 stage 1; renderer.md, "Nodes before
// instances"). The scene read whole is the load's prefix, cut into nodes in slot order: the slab
// and the 28 cubes of the cell-edge layout are one node whose instances stand either side of the
// cell edge at x = 64, two cells, and whose sphere is measured from its anchor's cell. On every
// path — the three rasterizers, the vertex path's capacity draw, the ray path, traced shadows and
// the cascaded maps — the frame with the nodes on is the frame with them off to the byte, at the
// origin and moved by 156,250 cells; with them on, the frame moved is the frame at home (outside
// the cascaded maps' texel band, as above); and the guard found no instance outside its node.
TEST_CASE("world translation: nodes before instances change no byte, at home and moved by cells") {
  {
    gfx::Device probe;
    std::string why;
    if (!probe.create(renderer_test::device_options(), &why)) {
      MESSAGE("renderer unavailable here: " << why);
      return;
    }
    if (!probe.features().buffer_int64_atomics) {
      MESSAGE("no 64-bit buffer atomics here");
      probe.destroy();
      return;
    }
    probe.destroy();
  }
  const test::TempDir tmp("engine_world_translation_nodes");
  const ScopedTunable near_m("renderer.flora.sward_near_m", "10");
  const ScopedTunable fade_m("renderer.flora.sward_fade_m", "12");
  const ScopedTunable guard("renderer.cull.node_guard", "true");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string ddc = slashes(dir / "ddc");
  REQUIRE(write_cube_glb(mesh));
  const WorldPos origin{60.0, 0.0, 0.0};
  Camera edge;
  edge.position = WorldPos{4.0 - 1.0 / 1024.0, 2.5, 6.0};
  edge.target = WorldPos{0.0, 0.5, -2.0};
  edge.znear = 0.05f;
  const std::vector<Camera> cameras{edge};
  const f64 shifts[] = {0.0, 156250.0};
  for (const Path& path : k_paths) {
    std::vector<CapturedFrame> pictures[2][2];  // [shift][nodes]
    bool drawn = true;
    for (u32 s = 0; s < 2 && drawn; ++s) {
      for (u32 on = 0; on < 2 && drawn; ++on) {
        const ScopedTunable nodes("renderer.cull.nodes", on != 0 ? "true" : "false");
        std::string why;
        u32 counts[2] = {0, 0};
        if (!draw(path, mesh, ddc, origin + cells(shifts[s]), cameras, pictures[s][on], why,
                  counts)) {
          MESSAGE(std::string(path.name) << ": not drawn here: " << why);
          drawn = false;
          break;
        }
        if (on != 0) {
          MESSAGE(std::string(path.name) << ", moved by " << shifts[s] << " cells, nodes on: the "
                                         << "guard checked " << counts[1] << " instances, "
                                         << counts[0] << " outside their node");
          CHECK(counts[1] > 0);
          CHECK(counts[0] == 0);
        }
      }
    }
    if (!drawn) continue;
    for (u32 s = 0; s < 2; ++s) {
      const Difference d = compare(pictures[s][0][0], pictures[s][1][0]);
      MESSAGE(std::string(path.name)
              << ", moved by " << shifts[s] << " cells, nodes off against on: " << d.color
              << " colour bytes, " << d.ids << " id words, " << d.depth << " depths differ over "
              << pictures[s][1][0].covered << " covered");
      CHECK(pictures[s][1][0].covered > pictures[s][1][0].width * pictures[s][1][0].height / 8);
      CHECK(d.color == 0);
      CHECK(d.ids == 0);
      CHECK(d.depth == 0);
    }
    const Difference moved = compare(pictures[0][1][0], pictures[1][1][0]);
    CHECK(moved.ids == 0);
    CHECK(moved.depth == 0);
    if (path.shadows != ShadowMode::Cascaded) CHECK(moved.color == 0);
  }
}
// **A moved instance is drawn on the next frame with the nodes on** (R188 stage 1; renderer.md,
// "Nodes before instances", "What keeps a node's sphere true"). A scene read whole moves an
// instance through the next frame's record upload (`move_instances`); the frame that copies it
// measures its node again before the node pass. One cube of the 28 is carried 30 m down the view,
// far out of its node's old sphere (the scene's 24 m slab), then a step back towards it, then home,
// a frame each with the frames before still in flight: every frame is the frame with the nodes off
// to the byte, on the mesh path with occlusion and on the vertex path, and the guard finds nothing.
TEST_CASE("world translation: a cube carried out of its node's sphere is drawn on the next frame") {
  {
    gfx::Device probe;
    std::string why;
    if (!probe.create(renderer_test::device_options(), &why)) {
      MESSAGE("renderer unavailable here: " << why);
      return;
    }
    if (!probe.features().buffer_int64_atomics) {
      MESSAGE("no 64-bit buffer atomics here");
      probe.destroy();
      return;
    }
    probe.destroy();
  }
  const test::TempDir tmp("engine_world_translation_node_move");
  const ScopedTunable guard("renderer.cull.node_guard", "true");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string ddc = slashes(dir / "ddc");
  REQUIRE(write_cube_glb(mesh));
  Camera eye;
  eye.position = WorldPos{3.25, 4.5, 9.75};
  eye.target = WorldPos{-0.5, 0.25, -1.5};
  eye.znear = 0.05f;
  const SceneDesc desc = scene_at(mesh, ddc, WorldPos{});
  constexpr u32 k_moved = 12;  // a cube near the middle of the grid
  SceneInstance home = desc.instances[k_moved];
  const Vec3 places[4] = {Vec3{-6.0f, 1.0f, -30.0f}, Vec3{-3.0f, 1.0f, -14.0f},
                          home.transform.position, Vec3{-6.0f, 1.0f, -30.0f}};
  const Path paths[2] = {k_paths[0], k_paths[2]};
  for (const Path& path : paths) {
    std::vector<CapturedFrame> shots[2];
    for (u32 on = 0; on < 2; ++on) {
      const ScopedTunable nodes("renderer.cull.nodes", on != 0 ? "true" : "false");
      gfx::Device device;
      std::string why;
      REQUIRE_MESSAGE(device.create(renderer_test::device_options(), &why), why);
      {
        RenderSettings settings;
        settings.raster = path.raster;
        settings.shadows = path.shadows;
        settings.occlusion = path.raster == RasterMode::Hardware;
        Rig rig;
        REQUIRE_MESSAGE(rig.build(device, desc, settings, 192, 128), rig.error);
        CaptureChannels channels;
        channels.ids = true;
        channels.depth = true;
        FrameDesc frame;
        frame.camera = eye;
        for (u32 k = 0; k < 2; ++k)
          REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &why), why);
        for (const Vec3& place : places) {
          SceneInstance moved = home;
          moved.transform.position = place;
          const u32 index = k_moved;
          REQUIRE_MESSAGE(
              rig.renderer.move_instances(std::span<const u32>(&index, 1),
                                          std::span<const SceneInstance>(&moved, 1), &why),
              why);
          CapturedFrame shot;
          REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &why), why);
          shots[on].push_back(std::move(shot));
        }
        u32 counts[2] = {0, 0};
        if (on != 0) {
          REQUIRE(rig.renderer.cull_node_guard(counts[0], counts[1]));
          MESSAGE(std::string(path.name) << ": the guard checked " << counts[1] << " instances, "
                                         << counts[0] << " outside their node");
          CHECK(counts[1] > 0);
          CHECK(counts[0] == 0);
        }
      }
      device.destroy();
    }
    for (u32 k = 0; k < 4; ++k) {
      const Difference d = compare(shots[0][k], shots[1][k]);
      MESSAGE(std::string(path.name)
              << ", place " << k << ": nodes off against on, " << d.color << " colour bytes, "
              << d.ids << " id words, " << d.depth << " depths differ over " << shots[1][k].covered
              << " covered");
      CHECK(d.color == 0);
      CHECK(d.ids == 0);
      CHECK(d.depth == 0);
    }
    // The cube is in the picture at its far place: its id is on the screen.
    u64 far_px = 0;
    for (u32 p = 0; p < shots[1][0].width * shots[1][0].height; ++p)
      far_px += shots[1][0].ids[u64{p} * k_id_words] == k_moved ? 1u : 0u;
    MESSAGE(std::string(path.name) << ": the cube 30 m out over " << far_px << " pixels");
    CHECK(far_px > 0);
  }
}
