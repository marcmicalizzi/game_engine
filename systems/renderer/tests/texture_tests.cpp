// The built textures in the renderer (docs/subsystems/texture.md, "In the renderer"): a quad with
// one texture, drawn through the resolve's albedo view (the textured base colour, unlit, so the
// lights are not in the comparison) once from the decoded source image and once from the content
// build's block-compressed, mipmapped `.tex`:
//
//   1. up close, where level 0 is what either path samples, the two pictures agree within the
//      bound BC7 allows (stated below);
//   2. far away, a one-texel checkerboard resolves to its mean colour through the mips, where the
//      decoded path — one level — aliases to black and white;
//   3. the level the hardware picks is the one the UV's finite difference across a pixel says it
//      should be: a probe texture whose every level is a different red reads back the level of
//      detail the resolve's analytic derivatives selected, at four distances.
//
// The quad is a glTF written at test time with its image beside it, loaded through the derived-
// data cache in the test's own scratch directory, so the container's records name the texture
// and the test writes the `.tex` itself where the renderer will look. Skipped with a message on a
// machine with no Vulkan device, as every renderer test is.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/image/png.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

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

constexpr u32 k_size = 256;  // the frame, square, so the pixel footprint is too
constexpr u32 k_albedo_view = static_cast<u32>(gfx::ResolveMode::Albedo);
constexpr f32 k_fov = 0.9599310886f;  // Camera's default, radians(55)

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

// A unit-half-width quad in the XY plane facing +z, UV (0,0) at the top left, with one white
// material whose base colour is `image.png` beside it.
bool write_quad(const std::filesystem::path& dir) {
  std::vector<u8> bin;
  const f32 positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const f32 normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 uvs[8] = {0, 1, 1, 1, 1, 0, 0, 0};
  for (const f32 v : positions)
    put_f32(bin, v);
  for (const f32 v : normals)
    put_f32(bin, v);
  for (const f32 v : uvs)
    put_f32(bin, v);
  const u16 indices[6] = {0, 1, 2, 0, 2, 3};
  for (const u16 i : indices) {
    bin.push_back(static_cast<u8>(i & 0xff));
    bin.push_back(static_cast<u8>(i >> 8));
  }
  if (!write_file((dir / "quad.bin").string(), bin.data(), bin.size())) return false;
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
      "\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true}],"
      "\"textures\":[{\"source\":0}],\"images\":[{\"uri\":\"image.png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":12}],"
      "\"buffers\":[{\"uri\":\"quad.bin\",\"byteLength\":140}]}";
  return write_file((dir / "quad.gltf").string(), json.data(), json.size());
}

bool write_image(const std::filesystem::path& dir, u32 size, const std::vector<u8>& rgba,
                 Vector<u8>& png) {
  if (!image::encode_png(size, size, 4, std::span<const u8>(rgba.data(), rgba.size()), png))
    return false;
  return write_file((dir / "image.png").string(), png.data(), png.size());
}

// A smooth image with edges in it: two gradients, soft rings, hard-edged bars. No noise — BC7's
// bound on noise is a different, and much looser, statement (texture.md).
std::vector<u8> smooth_image(u32 size) {
  std::vector<u8> rgba(size * size * 4);
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      u8* t = &rgba[(y * size + x) * 4];
      const i32 cx = static_cast<i32>(x) - static_cast<i32>(size / 2);
      const i32 cy = static_cast<i32>(y) - static_cast<i32>(size / 2);
      const u32 ring = static_cast<u32>((cx * cx + cy * cy) / 64) % 64;
      t[0] = static_cast<u8>(x * 255 / (size - 1));
      t[1] = static_cast<u8>(y * 255 / (size - 1));
      t[2] = static_cast<u8>(((x / 32) % 2 == 0 ? 60 : 180) + ring);
      t[3] = 255;
    }
  }
  return rgba;
}

std::vector<u8> checker_image(u32 size) {
  std::vector<u8> rgba(size * size * 4);
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      const u8 v = ((x + y) & 1) == 0 ? 0 : 255;
      u8* t = &rgba[(y * size + x) * 4];
      t[0] = t[1] = t[2] = v;
      t[3] = 255;
    }
  }
  return rgba;
}

struct Harness {
  gfx::Device device;
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  std::string skip;

  bool build(const std::string& mesh, const std::string& ddc) {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      skip = "device unavailable: " + error;
      return false;
    }
    SceneDesc desc;
    desc.meshes.push_back(mesh);
    desc.cache = true;  // the records name the texture in this scratch root, and only there
    desc.ddc = ddc;
    REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
    settings.view_mode = k_albedo_view;
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      skip = "unavailable here: " + unavailable_reason(availability, device);
      return false;
    }
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
    SceneRenderer::Desc rd;
    rd.width = k_size;
    rd.height = k_size;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
    return true;
  }

  // The albedo view from `distance` in front of the quad, looking at its centre.
  CapturedFrame shot(f32 distance) {
    FrameDesc frame;
    frame.camera.position = Vec3{0.0f, 0.0f, distance};
    frame.camera.target = Vec3{0.0f, 0.0f, 0.0f};
    frame.camera.fov_y = k_fov;
    frame.view_mode = k_albedo_view;
    CapturedFrame out;
    std::string error;
    REQUIRE_MESSAGE(renderer.capture(frame, {}, out, &error), error);
    return out;
  }

  ~Harness() {
    renderer.destroy();
    scene.destroy();
    if (device.valid()) device.destroy();
  }
};

// Builds image.png into the `.tex` the container's record for it names — the options the base
// colour slot asks for, keyed by the file's bytes — exactly as `engine-content build` would.
void build_texture_for(const std::filesystem::path& dir, const std::string& ddc,
                       const Vector<u8>& png) {
  texture::TextureBuildOptions options;
  options.format = texture::FormatChoice::bc7;
  options.color_space = texture::ColorSpace::srgb;
  texture::TextureData built;
  std::string error;
  REQUIRE_MESSAGE(texture::build_texture_from_encoded(std::span<const u8>(png.data(), png.size()),
                                                      options, built, nullptr, &error),
                  error);
  const std::string path = texture::texture_cache_path(ddc, built.build_key);
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  REQUIRE_MESSAGE(texture::write_texture_file(path, built, &error), error);
  (void)dir;
}

// The display transform's inverse (material.slang `display_encode`, a 1/2.2 power): what linear
// value the resolve wrote for a captured byte.
f64 linear_of(u8 code) { return std::pow(static_cast<f64>(code) / 255.0, 2.2); }

// The pixel extent of the quad, from where its two corners project: the finite difference of u
// across one pixel is 1 / that, and texels per pixel is the texture's side times it.
f64 texels_per_pixel(f32 distance, u32 texture_side) {
  const Mat4 view_proj =
      perspective_reversed_z(k_fov, 1.0f, 0.1f) *
      look_at(Vec3{0.0f, 0.0f, distance}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
  auto pixel_x = [&](f32 x) {
    const Vec4 clip = view_proj * Vec4{x, 0.0f, 0.0f, 1.0f};
    return (static_cast<f64>(clip.x / clip.w) * 0.5 + 0.5) * k_size;
  };
  const f64 extent = pixel_x(1.0f) - pixel_x(-1.0f);
  return static_cast<f64>(texture_side) / extent;
}

}  // namespace

TEST_CASE("renderer: a built texture at level 0 is its decoded source within BC7's bound") {
  const test::TempDir tmp("renderer_texture_level0");
  const std::filesystem::path dir = tmp.native();
  REQUIRE(write_quad(dir));
  Vector<u8> png;
  REQUIRE(write_image(dir, k_size, smooth_image(k_size), png));
  const std::string mesh = (dir / "quad.gltf").string();
  const std::string ddc = (dir / "ddc").string();

  // Up close: the quad fills the frame, a texel is larger than a pixel, and level 0 is what the
  // mipmapped path samples too.
  constexpr f32 k_near = 1.7f;
  CHECK(texels_per_pixel(k_near, k_size) < 1.0);
  CapturedFrame decoded;
  {
    Harness h;
    if (!h.build(mesh, ddc)) {
      MESSAGE(h.skip);
      return;
    }
    CHECK(h.scene.textures_decoded() == 1);
    CHECK(h.scene.textures_built() == 0);
    decoded = h.shot(k_near);
  }
  build_texture_for(dir, ddc, png);
  Harness h;
  REQUIRE(h.build(mesh, ddc));
  CHECK(h.scene.textures_built() == 1);
  CHECK(h.scene.textures_decoded() == 0);
  const CapturedFrame built = h.shot(k_near);
  REQUIRE(built.covered == decoded.covered);

  // The bound, stated: over the covered pixels' RGB, PSNR above 38 dB and no channel more than 16
  // codes off. BC7 on the source is about 48 dB (domain/texture's bound on a gradient is 45);
  // what is left is BC7's error seen through bilinear magnification and the display curve, which
  // stretches dark codes.
  f64 squared = 0.0;
  u64 samples = 0;
  u32 worst = 0;
  for (u32 p = 0; p < k_size * k_size; ++p) {
    for (u32 c = 0; c < 3; ++c) {
      const i32 d = static_cast<i32>(built.color[p * 4 + c]) - decoded.color[p * 4 + c];
      worst = std::max(worst, static_cast<u32>(d < 0 ? -d : d));
      squared += static_cast<f64>(d) * d;
      ++samples;
    }
  }
  const f64 psnr = squared == 0.0 ? 1000.0 : 10.0 * std::log10(255.0 * 255.0 * samples / squared);
  MESSAGE("level 0, built against decoded: PSNR " << psnr << " dB, worst channel " << worst
                                                  << ", texture bytes " << h.scene.texture_bytes());
  CHECK(psnr > 38.0);
  CHECK(worst <= 16);
}

TEST_CASE("renderer: a checkerboard far away resolves to its mean through the mips") {
  const test::TempDir tmp("renderer_texture_checker");
  const std::filesystem::path dir = tmp.native();
  REQUIRE(write_quad(dir));
  Vector<u8> png;
  REQUIRE(write_image(dir, k_size, checker_image(k_size), png));
  const std::string mesh = (dir / "quad.gltf").string();
  const std::string ddc = (dir / "ddc").string();

  // Far enough that about eight texels fall in a pixel: level 3.
  constexpr f32 k_far = 15.0f;
  const f64 rho = texels_per_pixel(k_far, k_size);
  CHECK(rho > 6.0);
  // The pixels well inside the quad, a quarter of its extent around the centre.
  const u32 half = static_cast<u32>(static_cast<f64>(k_size) / rho / 4.0);
  REQUIRE(half >= 2);
  auto spread = [&](const CapturedFrame& f, f64& mean, f64& deviation) {
    f64 sum = 0.0;
    f64 sum_sq = 0.0;
    u32 count = 0;
    for (u32 y = k_size / 2 - half; y < k_size / 2 + half; ++y) {
      for (u32 x = k_size / 2 - half; x < k_size / 2 + half; ++x) {
        const f64 v = f.color[(y * k_size + x) * 4];
        sum += v;
        sum_sq += v * v;
        ++count;
      }
    }
    mean = sum / count;
    deviation = std::sqrt(std::max(0.0, sum_sq / count - mean * mean));
  };

  f64 decoded_mean = 0.0;
  f64 decoded_deviation = 0.0;
  {
    Harness h;
    if (!h.build(mesh, ddc)) {
      MESSAGE(h.skip);
      return;
    }
    spread(h.shot(k_far), decoded_mean, decoded_deviation);
  }
  build_texture_for(dir, ddc, png);
  Harness h;
  REQUIRE(h.build(mesh, ddc));
  REQUIRE(h.scene.textures_built() == 1);
  f64 mean = 0.0;
  f64 deviation = 0.0;
  spread(h.shot(k_far), mean, deviation);
  // Half the light: linear 0.5, which the display curve writes as 0.5^(1/2.2) of 255 = 186. The
  // mips were filtered in linear light, so it is 186 and not the 128 a gamma-space mean would
  // give; one level of the chain sampled at level 0 is black and white wherever it lands.
  MESSAGE("checker at " << rho << " texels a pixel: built mean " << mean << " deviation "
                        << deviation << "; decoded mean " << decoded_mean << " deviation "
                        << decoded_deviation);
  CHECK(std::abs(mean - 186.0) < 4.0);
  CHECK(deviation < 3.0);
  CHECK(decoded_deviation > 30.0);
}

TEST_CASE("renderer: the level the resolve picks is the one the UV's finite difference says") {
  const test::TempDir tmp("renderer_texture_lod");
  const std::filesystem::path dir = tmp.native();
  REQUIRE(write_quad(dir));
  Vector<u8> png;
  REQUIRE(write_image(dir, k_size, smooth_image(k_size), png));
  const std::string mesh = (dir / "quad.gltf").string();
  const std::string ddc = (dir / "ddc").string();

  // The probe: the texture the container's record names, written by hand as uncompressed data
  // whose level L is red L * 28 everywhere — so the red the albedo view shows, taken back to
  // linear, is 28/255 times the level of detail the sampler used, trilinear fraction included.
  // The renderer takes a `.tex` for what it says it is (format, colour space, levels) once its
  // identity matches the bytes and options the record names.
  constexpr u32 k_step = 28;
  texture::TextureBuildOptions options;
  options.format = texture::FormatChoice::bc7;
  options.color_space = texture::ColorSpace::srgb;
  texture::TextureData probe;
  probe.format = texture::TextureFormat::rgba8;
  probe.color_space = texture::ColorSpace::linear;
  probe.width = k_size;
  probe.height = k_size;
  probe.source_channels = 4;
  probe.source_hash = hash_bytes(png.data(), png.size());
  probe.build_key = texture::texture_cache_key(probe.source_hash, options);
  const u32 levels = texture::texture_full_level_count(k_size, k_size);
  u64 offset = 0;
  for (u32 l = 0; l < levels; ++l) {
    texture::TextureFileLevel level;
    level.width = texture::texture_level_extent(k_size, l);
    level.height = level.width;
    level.offset = offset;
    level.bytes = u64{level.width} * level.height * 4;
    probe.levels.push_back(level);
    offset += (level.bytes + 15) / 16 * 16;
  }
  probe.data.resize(static_cast<u32>(offset));
  for (u32 l = 0; l < levels; ++l) {
    u8* texels = probe.data.data() + probe.levels[l].offset;
    for (u64 t = 0; t < probe.levels[l].bytes / 4; ++t) {
      texels[t * 4 + 0] = static_cast<u8>(l * k_step);
      texels[t * 4 + 1] = 0;
      texels[t * 4 + 2] = 0;
      texels[t * 4 + 3] = 255;
    }
  }
  std::string error;
  const std::string path = texture::texture_cache_path(ddc, probe.build_key);
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  REQUIRE_MESSAGE(texture::write_texture_file(path, probe, &error), error);

  Harness h;
  if (!h.build(mesh, ddc)) {
    MESSAGE(h.skip);
    return;
  }
  REQUIRE(h.scene.textures_built() == 1);
  // Four distances, each doubling the last, so the texels per pixel go 2.08, 4.16, 8.33, 16.7:
  // levels 1.06, 2.06, 3.06 and 4.06. Each expected level is log2 of the texels per pixel the
  // projection's finite difference gives; the measured one is read at the centre pixel. The
  // camera faces the quad, so the footprint is square and anisotropy has no axis to choose.
  //
  // **Just past a whole level, on purpose.** How the sampler blends two levels is the driver's:
  // measured on the RTX 5090 (driver defaults), a fraction f of the way from level L to L + 1
  // reads as min(1, 2f) of the way — the lower level only for the first half, the next level
  // alone after it — which is NVIDIA's trilinear optimization and not trilinear's straight line
  // (texture.md, "What it measured"). Near f = 0 the two agree to within f, so these distances
  // test the level the derivatives select, which is this module's, and not the blend, which is
  // not; a derivative 15% off moves the level by 0.2 and fails.
  for (const f32 distance : {4.0f, 8.0f, 16.0f, 32.0f}) {
    const f64 rho = texels_per_pixel(distance, k_size);
    const f64 expected = std::log2(rho);
    const CapturedFrame f = h.shot(distance);
    const u8 red = f.color[((k_size / 2) * k_size + k_size / 2) * 4];
    const f64 measured = linear_of(red) * 255.0 / k_step;
    MESSAGE("distance " << distance << ": " << rho << " texels a pixel, expected level " << expected
                        << ", measured " << measured);
    CHECK(std::abs(measured - expected) < 0.2);
  }
}
