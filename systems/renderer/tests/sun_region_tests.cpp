// The sun's masks by region and by distance (docs/subsystems/renderer.md, "The sun's masks by
// region and by distance"; roadmap R139). A ground at y = 0 and a masked leaf card lying flat a
// metre above it (the checker of alpha_mask_tests.cpp, half of it kept), lit by the renderer's sun
// under traced shadows:
//
// - The thresholds: `gfx::sun_solid_distance` is the penumbra rule's arithmetic, and the
//   measurement switch steps none, sides, far, both eight frames at a time.
// - Past the distance a receiver under the card casts the solid card's shadow; nearer than it, the
//   mask's — so the ground splits by distance in one frame, and a distance past every receiver is
//   the frame as set, byte for byte. (With the micromaps off: the drawn card then has none.)
// - With the micromaps on, as they are by default, the rule turns nothing solid that has one: an
//   opaque ray flag does not undo a micromap's transparent micro-triangles, and the two-state
//   answer still decides the unknown ones, so the card's shadow is the frame as set's.
// - The sides of a surround: with the side columns solid outright, the centre third of a 3:1 frame
//   is the frame as set to the byte, and every pixel that moved is a side pixel the solid card's
//   shadow and the mask's disagree on.

#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/image/png.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_height = 128;
constexpr f32 k_fov = 0.9599310886f;  // Camera's default, radians(55)
constexpr u32 k_texels = 8;

bool write_file(const std::string& path, const void* data, usize size) {
  std::ofstream f(path, std::ios::binary);
  f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return f.good();
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((bits >> (8 * i)) & 0xffu));
}

// The mask: 2x2-texel blocks of an 8x8 image in a checker, kept where white.
bool write_mask_image(const std::filesystem::path& dir) {
  std::vector<u8> rgba(k_texels * k_texels * 4);
  for (u32 y = 0; y < k_texels; ++y) {
    for (u32 x = 0; x < k_texels; ++x) {
      const bool keep = ((x / 2 + y / 2) & 1u) == 0;
      u8* t = &rgba[(y * k_texels + x) * 4];
      t[0] = t[1] = t[2] = keep ? 255 : 0;
      t[3] = keep ? 255 : 0;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(k_texels, k_texels, 4, std::span<const u8>(rgba.data(), rgba.size()), png))
    return false;
  return write_file((dir / "mask.png").string(), png.data(), png.size());
}

// A ground 6 metres across at y = 0 and the card, 2 metres square, flat 1 metre above it, its
// material `alpha_mode` ("MASK" or "OPAQUE").
bool write_scene(const std::filesystem::path& dir, const char* name, const char* alpha_mode) {
  std::vector<u8> bin;
  const f32 card_positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const f32 card_normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 card_uvs[8] = {0, 1, 1, 1, 1, 0, 0, 0};
  const f32 ground_positions[12] = {-3, 0, 3, 3, 0, 3, 3, 0, -3, -3, 0, -3};
  const f32 ground_normals[12] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
  const f32 ground_uvs[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (const f32 v : card_positions)
    put_f32(bin, v);  // 0
  for (const f32 v : card_normals)
    put_f32(bin, v);  // 48
  for (const f32 v : card_uvs)
    put_f32(bin, v);  // 96
  for (const f32 v : ground_positions)
    put_f32(bin, v);  // 128
  for (const f32 v : ground_normals)
    put_f32(bin, v);  // 176
  for (const f32 v : ground_uvs)
    put_f32(bin, v);                          // 224
  const u16 indices[6] = {0, 1, 2, 0, 2, 3};  // 256
  for (const u16 i : indices) {
    bin.push_back(static_cast<u8>(i & 0xff));
    bin.push_back(static_cast<u8>(i >> 8));
  }
  for (u32 i = 0; i < 4; ++i)
    bin.push_back(0);  // 268: pad to 272
  if (!write_file((dir / "scene.bin").string(), bin.data(), bin.size())) return false;
  const std::string json =
      std::string(
          "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
          "\"nodes\":[{\"mesh\":0,\"translation\":[0,1,0],"
          "\"rotation\":[-0.70710678,0,0,0.70710678]},{\"mesh\":1}],"
          "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
          "\"TEXCOORD_0\":2},\"indices\":6,\"material\":0}]},"
          "{\"primitives\":[{\"attributes\":{\"POSITION\":3,\"NORMAL\":4,\"TEXCOORD_0\":5},"
          "\"indices\":6,\"material\":1}]}],"
          "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
          "\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true,\"alphaMode\":\"") +
      alpha_mode +
      "\",\"alphaCutoff\":0.5},"
      "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.9,0.9,1],\"metallicFactor\":0,"
      "\"roughnessFactor\":1},\"doubleSided\":true}],"
      "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728}],"
      "\"textures\":[{\"source\":0,\"sampler\":0}],"
      "\"images\":[{\"uri\":\"mask.png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-3,0,-3],\"max\":[3,0,3]},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":5,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":6,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":176,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":224,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":256,\"byteLength\":12}],"
      "\"buffers\":[{\"uri\":\"scene.bin\",\"byteLength\":272}]}";
  return write_file((dir / name).string(), json.data(), json.size());
}

struct Fixture {
  test::TempDir tmp{"sun_region"};
  std::string masked;
  std::string opaque;
  Fixture() {
    const std::filesystem::path dir(tmp.path());
    REQUIRE(write_mask_image(dir));
    REQUIRE(write_scene(dir, "masked.gltf", "MASK"));
    REQUIRE(write_scene(dir, "opaque.gltf", "OPAQUE"));
    masked = (dir / "masked.gltf").string();
    opaque = (dir / "opaque.gltf").string();
  }
};

struct TunableScope {
  TunableScope(const char* set, const char* restore) : restore_(restore) {
    REQUIRE(tunables::apply_overrides(set));
  }
  ~TunableScope() { (void)tunables::apply_overrides(restore_); }
  TunableScope(const TunableScope&) = delete;
  TunableScope& operator=(const TunableScope&) = delete;
  const char* restore_;
};

struct Shot {
  CapturedFrame frame;
  std::string skip;
  bool baked = false;  // the scene baked opacity micromaps
};

// One shaded frame of `mesh` under traced shadows, `width` x 128, from the side the shadow falls
// on (the sun is up and towards +x +z), so the card hides the ground away from its shadow.
Shot shoot(const std::string& mesh, u32 width) {
  Shot out;
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    out.skip = "device unavailable: " + error;
    return out;
  }
  RenderSettings settings;
  settings.raster = RasterMode::Hardware;
  settings.shadows = ShadowMode::RayTraced;
  settings.view_mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.cache = false;
  REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok ||
      !resolved.shadows || resolved.vertex_path) {
    out.skip = "no mesh shaders or ray-traced shadows on this device";
    device.destroy();
    return out;
  }
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
  SceneRenderer::Desc rd;
  rd.width = width;
  rd.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
  out.baked = scene.micromap_tables_address() != 0;
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), Vec3{-2.5f, 4.0f, -2.5f});
  frame.camera.target = absolute(WorldPos::origin(), Vec3{-0.4f, 0.0f, -0.4f});
  frame.camera.fov_y = k_fov;
  frame.view_mode = settings.view_mode;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  renderer.destroy();
  scene.destroy();
  device.destroy();
  return out;
}

bool covered(const CapturedFrame& f, u32 p) { return f.depth[p] != 0.0f; }

// Per pixel: 0 not ground, 1 lit, 2 shadowed. The ground is the cluster covering the most pixels,
// and a pixel of it is shadowed where its green is below the midpoint of its darkest and brightest.
Vector<u8> shadow_classes(const CapturedFrame& f) {
  const u32 n = f.width * f.height;
  Vector<u8> classes;
  classes.assign(n, u8{0});
  std::map<u32, u32> counts;
  for (u32 p = 0; p < n; ++p)
    if (covered(f, p)) ++counts[f.ids[p * k_id_words + 1]];
  u32 ground = 0;
  u32 most = 0;
  for (const auto& [cluster, count] : counts) {
    if (count > most) {
      most = count;
      ground = cluster;
    }
  }
  u8 lo = 255;
  u8 hi = 0;
  for (u32 p = 0; p < n; ++p) {
    if (!covered(f, p) || f.ids[p * k_id_words + 1] != ground) continue;
    const u8 g = f.color[p * 4 + 1];
    lo = g < lo ? g : lo;
    hi = g > hi ? g : hi;
  }
  const u32 mid = (static_cast<u32>(lo) + hi) / 2;
  for (u32 p = 0; p < n; ++p) {
    if (!covered(f, p) || f.ids[p * k_id_words + 1] != ground) continue;
    classes[p] = f.color[p * 4 + 1] < mid ? 2 : 1;
  }
  return classes;
}

bool same_pixel(const CapturedFrame& a, const CapturedFrame& b, u32 p) {
  return std::memcmp(&a.color[p * 4], &b.color[p * 4], 4) == 0;
}

}  // namespace

TEST_CASE("sun region: the distance is the pixel's footprint over the sun's penumbrae") {
  // The sun's disc: twice the solar radius over an astronomical unit.
  CHECK(static_cast<f64>(gfx::k_sun_angular_diameter) ==
        doctest::Approx(2.0 * 6.957e8 / 1.495978707e11).epsilon(1e-3));
  // 2160 rows at 55 degrees: 2075 pixels a radian. Under a ten-metre canopy a pixel covers one
  // penumbra (9.3 cm) at 193 m.
  const f32 proj = 2160.0f * 0.5f / std::tan(0.5f * k_fov);
  const f64 one = static_cast<f64>(gfx::sun_solid_distance(1.0f, 10.0f, proj));
  CHECK(one == doctest::Approx(193.0).epsilon(0.005));
  CHECK(static_cast<f64>(gfx::sun_solid_distance(2.0f, 10.0f, proj)) == doctest::Approx(2.0 * one));
  // Off: a factor, a height or a projection of 0.
  CHECK(gfx::sun_solid_distance(0.0f, 10.0f, proj) == 0.0f);
  CHECK(gfx::sun_solid_distance(1.0f, 0.0f, proj) == 0.0f);
  CHECK(gfx::sun_solid_distance(1.0f, 10.0f, 0.0f) == 0.0f);
}

TEST_CASE(
    "sun region: the measurement switch steps none, sides, far, both eight frames at a time") {
  TunableScope far("renderer.shadow.solid_far=2", "renderer.shadow.solid_far=0");
  TunableScope sides("renderer.shadow.solid_sides=40", "renderer.shadow.solid_sides=-1");
  {
    // As set: both, as the tunables say.
    const SunRegionRule set = sun_region_rule(0);
    CHECK(set.far_factor == 2.0f);
    CHECK(set.side_m == 40.0f);
    CHECK(set.canopy_m == 10.0f);
  }
  TunableScope cycle("renderer.shadow.region_rule=cycle", "renderer.shadow.region_rule=as_set");
  const struct {
    u64 frame;
    bool far;
    bool sides;
  } expect[] = {{0, false, false}, {7, false, false}, {8, false, true},  {16, true, false},
                {24, true, true},  {31, true, true},  {32, false, false}};
  for (const auto& e : expect) {
    const SunRegionRule r = sun_region_rule(e.frame);
    CHECK_MESSAGE((r.far_factor > 0.0f) == e.far, "frame " << e.frame);
    CHECK_MESSAGE((r.side_m >= 0.0f) == e.sides, "frame " << e.frame);
  }
}

// Past the distance a receiver under the card casts the solid card's shadow, nearer it the
// mask's. The micromaps are off here, so the drawn card's triangles have none and the rule is
// what decides them; the canopy is the card's own metre.
TEST_CASE(
    "sun region: a receiver past the distance casts the solid shadow, a near one the mask's") {
  Fixture fx;
  TunableScope micromaps("renderer.shadow.micromaps=false", "renderer.shadow.micromaps=true");
  TunableScope canopy("renderer.shadow.canopy_m=1", "renderer.shadow.canopy_m=10");
  const Shot solid = shoot(fx.opaque, k_height);
  if (!solid.skip.empty()) {
    MESSAGE("skipped: " << solid.skip);
    return;
  }
  const Shot as_set = shoot(fx.masked, k_height);
  REQUIRE(as_set.skip.empty());
  // 128 rows at 55 degrees are 122.9 pixels a radian, so a factor f under the 1 m canopy is a
  // distance of 1.143 f metres: 1,000 is past every receiver, 0.01 short of them all, and 4.3 is
  // the ground's middle (the camera is 3.2 to 8 m from it).
  Shot never;
  Shot always;
  Shot split;
  {
    TunableScope f("renderer.shadow.solid_far=1000", "renderer.shadow.solid_far=0");
    never = shoot(fx.masked, k_height);
  }
  {
    TunableScope f("renderer.shadow.solid_far=0.01", "renderer.shadow.solid_far=0");
    always = shoot(fx.masked, k_height);
  }
  {
    TunableScope f("renderer.shadow.solid_far=4.3", "renderer.shadow.solid_far=0");
    split = shoot(fx.masked, k_height);
  }
  // Past every receiver: the frame as set, byte for byte.
  CHECK(never.frame.color == as_set.frame.color);
  const Vector<u8> full = shadow_classes(solid.frame);
  const Vector<u8> mask = shadow_classes(as_set.frame);
  const Vector<u8> forced = shadow_classes(always.frame);
  const Vector<u8> halves = shadow_classes(split.frame);
  u32 shared = 0;
  u32 differ = 0;
  u32 closed = 0;
  for (u32 p = 0; p < k_height * k_height; ++p) {
    if (full[p] == 0 || forced[p] == 0) continue;
    ++shared;
    if (full[p] != forced[p]) ++differ;
    if (mask[p] == 1 && forced[p] == 2) ++closed;
  }
  MESSAGE(shared << " ground pixels: " << differ << " differ from the solid card's shadow, "
                 << closed << " of the mask's sunflecks closed");
  CHECK(shared > 4000);
  CHECK(differ == 0);
  CHECK(closed > 200);
  // In the middle: where the mask and the solid card disagree, the near pixels keep the mask's
  // answer and the far ones take the solid card's, and the far ones lie deeper (reversed depth: a
  // smaller value is farther).
  u32 took_mask = 0;
  u32 took_solid = 0;
  u32 other = 0;
  f64 depth_mask = 0.0;
  f64 depth_solid = 0.0;
  for (u32 p = 0; p < k_height * k_height; ++p) {
    if (mask[p] == 0 || forced[p] == 0 || halves[p] == 0 || mask[p] == forced[p]) continue;
    if (halves[p] == mask[p]) {
      ++took_mask;
      depth_mask += static_cast<f64>(split.frame.depth[p]);
    } else if (halves[p] == forced[p]) {
      ++took_solid;
      depth_solid += static_cast<f64>(split.frame.depth[p]);
    } else {
      ++other;
    }
  }
  MESSAGE("split: " << took_mask << " near pixels keep the mask's answer, " << took_solid
                    << " far ones take the solid card's");
  CHECK(other == 0);
  REQUIRE(took_mask > 20);
  REQUIRE(took_solid > 20);
  CHECK(depth_solid / took_solid < depth_mask / took_mask);
}

// With the micromaps on, as by default, the drawn card's triangles carry one: the opaque flag does
// not undo its transparent micro-triangles and the two-state answer still decides its unknown
// ones, so the rule leaves the card's shadow exactly as set (renderer.md, "Opacity micromaps":
// the specification culls a transparent micro-triangle before the ray's flags apply).
TEST_CASE("sun region: a card with a micromap keeps the frame as set's shadow under the rule") {
  Fixture fx;
  TunableScope micromaps("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
  const Shot as_set = shoot(fx.masked, k_height);
  if (!as_set.skip.empty()) {
    MESSAGE("skipped: " << as_set.skip);
    return;
  }
  Shot always;
  {
    TunableScope f("renderer.shadow.solid_far=0.01", "renderer.shadow.solid_far=0");
    always = shoot(fx.masked, k_height);
  }
  REQUIRE(always.skip.empty());
  if (!as_set.baked) {
    MESSAGE("skipped: no opacity micromaps on this device");
    return;
  }
  u32 moved = 0;
  for (u32 p = 0; p < k_height * k_height; ++p)
    moved += same_pixel(as_set.frame, always.frame, p) ? 0u : 1u;
  MESSAGE(moved << " pixels moved with every ray past the distance and the micromaps on");
  CHECK(moved == 0);
  // The witness: the same rule with the micromaps off closes the mask's holes.
  TunableScope off("renderer.shadow.micromaps=false", "renderer.shadow.micromaps=true");
  const Shot plain = shoot(fx.masked, k_height);
  Shot plain_always;
  {
    TunableScope f("renderer.shadow.solid_far=0.01", "renderer.shadow.solid_far=0");
    plain_always = shoot(fx.masked, k_height);
  }
  u32 plain_moved = 0;
  for (u32 p = 0; p < k_height * k_height; ++p)
    plain_moved += same_pixel(plain.frame, plain_always.frame, p) ? 0u : 1u;
  MESSAGE(plain_moved << " moved by the same rule with the micromaps off");
  CHECK(plain_moved > 200);
}

// The sides of a surround: a 3:1 frame's outer thirds (R126's region) solid outright. The centre
// third is the frame as set to the byte, and every pixel that moved is a side pixel where the
// solid card's shadow and the mask's disagree.
TEST_CASE(
    "sun region: the sides solid outright move only side pixels the solid shadow differs on") {
  Fixture fx;
  TunableScope micromaps("renderer.shadow.micromaps=false", "renderer.shadow.micromaps=true");
  constexpr u32 k_width = 3 * k_height;
  const Shot solid = shoot(fx.opaque, k_width);
  if (!solid.skip.empty()) {
    MESSAGE("skipped: " << solid.skip);
    return;
  }
  const Shot as_set = shoot(fx.masked, k_width);
  Shot sides;
  {
    TunableScope s("renderer.shadow.solid_sides=0", "renderer.shadow.solid_sides=-1");
    sides = shoot(fx.masked, k_width);
  }
  REQUIRE(as_set.skip.empty());
  REQUIRE(sides.skip.empty());
  const Vector<u8> full = shadow_classes(solid.frame);
  const Vector<u8> mask = shadow_classes(as_set.frame);
  const u32 x0 = k_height;
  const u32 x1 = 2 * k_height;
  u32 centre_moved = 0;
  u32 side_moved = 0;
  u32 side_moved_unexplained = 0;
  u32 side_could = 0;
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const u32 p = y * k_width + x;
      const bool moved = !same_pixel(as_set.frame, sides.frame, p);
      if (x >= x0 && x < x1) {
        centre_moved += moved ? 1u : 0u;
        continue;
      }
      const bool could = full[p] != 0 && mask[p] != 0 && full[p] != mask[p];
      side_could += could ? 1u : 0u;
      if (!moved) continue;
      ++side_moved;
      // A pixel the class count cannot see (the card itself, its self-shadow, a ground pixel the
      // solid card hides) still moved only if its own sun ray met the card.
      if (!could && full[p] != 0 && mask[p] != 0) ++side_moved_unexplained;
    }
  }
  MESSAGE("sides solid: " << side_moved << " side pixels moved of " << side_could
                          << " the solid shadow differs on; the centre " << centre_moved);
  CHECK(centre_moved == 0);
  CHECK(side_moved > 0);
  CHECK(side_moved_unexplained == 0);
}
