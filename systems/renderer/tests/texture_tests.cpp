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
// data cache in the test's own scratch directory — where the load, on a cold cache, builds the
// `.tex` the container's records name, as `engine-content build --cache` would — or with no cache
// at all, which is the decoded path. Skipped with a message on a machine with no Vulkan device, as
// every renderer test is.
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
#include <span>
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

// A unit-half-width quad in the XY plane facing +z, UV (0,0) at the top left and (uv_max, uv_max)
// at the bottom right, with one white material whose base colour is `image.png` beside it.
// `sampler` is a glTF sampler object for the texture, or empty for none (the glTF defaults);
// `transform` is a KHR_texture_transform object for the reference, or empty for none; `image_uri`
// is what the image names instead of `image.png` — a data URI embeds it.
bool write_quad(const std::filesystem::path& dir, f32 uv_max = 1.0f,
                const std::string& sampler = "", const std::string& transform = "",
                const std::string& image_uri = "image.png") {
  std::vector<u8> bin;
  const f32 positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const f32 normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 uvs[8] = {0, uv_max, uv_max, uv_max, uv_max, 0, 0, 0};
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
  const std::string texture_ref =
      transform.empty()
          ? std::string("{\"index\":0}")
          : "{\"index\":0,\"extensions\":{\"KHR_texture_transform\":" + transform + "}}";
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":" +
      texture_ref +
      ","
      "\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true}]," +
      (sampler.empty()
           ? std::string("\"textures\":[{\"source\":0}],")
           : "\"samplers\":[" + sampler + "],\"textures\":[{\"source\":0,\"sampler\":0}],") +
      "\"images\":[{\"uri\":\"" + image_uri +
      "\"}],"
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

  // `cache` true reads and fills the derived-data root in the test's scratch directory — which
  // builds the quad's texture there as `engine-content build --cache` would — and false draws the
  // image decoded at one level, the path a load without a cache takes.
  bool build(const std::string& mesh, const std::string& ddc, bool cache = true) {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      skip = "device unavailable: " + error;
      return false;
    }
    SceneDesc desc;
    desc.meshes.push_back(mesh);
    desc.cache = cache;  // the records name the texture in this scratch root, and only there
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
    frame.camera.position = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, distance});
    frame.camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
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

// What `engine-content build --cache` makes of image.png for the quad's base colour: the options
// that slot asks for, keyed by the file's bytes. The renderer's own cache fill builds exactly this
// (it calls the same texture step), which the cold-cache test holds it to byte for byte.
texture::TextureData expected_texture(const Vector<u8>& png) {
  texture::TextureBuildOptions options;
  options.format = texture::FormatChoice::bc7;
  options.color_space = texture::ColorSpace::srgb;
  // The quad's texture names no sampler, so it repeats both ways (the glTF default), and the
  // record asks for its mips to be filtered with repeating edges to match.
  options.edge_x = texture::EdgeMode::repeat;
  options.edge_y = texture::EdgeMode::repeat;
  texture::TextureData built;
  std::string error;
  REQUIRE_MESSAGE(texture::build_texture_from_encoded(std::span<const u8>(png.data(), png.size()),
                                                      options, built, nullptr, &error),
                  error);
  return built;
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
    if (!h.build(mesh, ddc, false)) {
      MESSAGE(h.skip);
      return;
    }
    CHECK(h.scene.textures_decoded() == 1);
    CHECK(h.scene.textures_built() == 0);
    decoded = h.shot(k_near);
  }
  Harness h;
  REQUIRE(h.build(mesh, ddc));  // a cold cache: the load builds the texture it then uploads
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

// A scene loaded from a glTF on a cold cache — nothing under the derived-data root, no
// `engine-content build` run over it — samples built textures on its first frame: the renderer's
// own cache fill runs the content build's texture step (content_build::run_texture_step) beside the
// container it writes, and the `.tex` it leaves is byte for byte what engine-content builds for
// that image and slot. Until it did, such a scene drew its textures decoded at one level, with no
// mips, and shimmered at a distance until someone ran engine-content over it. A second load finds
// the entry rather than building it again.
TEST_CASE("renderer: a glTF on a cold cache builds its textures and samples them") {
  const test::TempDir tmp("renderer_texture_cold");
  const std::filesystem::path dir = tmp.native();
  REQUIRE(write_quad(dir));
  Vector<u8> png;
  REQUIRE(write_image(dir, k_size, smooth_image(k_size), png));
  const std::string mesh = (dir / "quad.gltf").string();
  const std::string ddc = (dir / "ddc").string();
  const texture::TextureData expected = expected_texture(png);
  const std::string path = texture::texture_cache_path(ddc, expected.build_key);
  CHECK_FALSE(std::filesystem::exists(path));
  {
    Harness h;
    if (!h.build(mesh, ddc)) {
      MESSAGE(h.skip);
      return;
    }
    CHECK(h.scene.textures_built() == 1);
    CHECK(h.scene.textures_decoded() == 0);
    REQUIRE(std::filesystem::exists(path));
    texture::TextureData written;
    std::string error;
    REQUIRE_MESSAGE(texture::read_texture_file(path, written, &error), error);
    CHECK(texture::texture_file_hash(written) == texture::texture_file_hash(expected));
    CHECK(written.levels.size() == texture::texture_full_level_count(k_size, k_size));
  }
  // Warm: the container and the texture are both found; nothing is decoded.
  const auto written_at = std::filesystem::last_write_time(path);
  Harness h;
  REQUIRE(h.build(mesh, ddc));
  CHECK(h.scene.textures_built() == 1);
  CHECK(h.scene.textures_decoded() == 0);
  CHECK(std::filesystem::last_write_time(path) == written_at);
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
    if (!h.build(mesh, ddc, false)) {
      MESSAGE(h.skip);
      return;
    }
    spread(h.shot(k_far), decoded_mean, decoded_deviation);
  }
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
  options.edge_x = texture::EdgeMode::repeat;  // the glTF default sampler's wrap, as above
  options.edge_y = texture::EdgeMode::repeat;
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

// The glTF sampler's wrap modes and KHR_texture_transform, end to end: a glTF quad whose UVs run
// from 0 to 3 across and down, textured with a 2x2 image of four unlike colours through a
// nearest-magnifying sampler, is drawn through the albedo view and read back cell by cell. The
// quad is six texels across, so each of its 6 x 6 cells is one texel of one tile:
//
//   repeat   nine whole tiles: cell (i, j) shows texel (i mod 2, j mod 2)
//   mirror   every other tile reflected: the texel column goes 0 1 1 0 0 1 across
//   clamp    one tile, then the edge texel for ever: column min(i, 1), row min(j, 1)
//   transform  UVs from 0 to 1 and a KHR_texture_transform scale of 3 on a repeating sampler:
//            the same nine tiles as `repeat`, from the material table's transform alone
//
// Each case is drawn twice — from the decoded source image (one level, the decode path's samplers)
// and from the built texture (the content build's BC7 with its mips filtered with the sampler's
// edges, the mipmapping samplers) — because the two paths create their samplers separately and
// both have to honour the file. A cell is read at its centre and matched to the nearest of the
// four colours, so neither the 16x16-block compression nor the display curve is in the comparison.
TEST_CASE("renderer: glTF wrap modes and texture transforms tile, mirror and clamp") {
  // Red, green / blue, white: row 0 is the top of the image, where v = 0.
  const u8 colours[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
  std::vector<u8> rgba(2 * 2 * 4);
  for (u32 t = 0; t < 4; ++t) {
    rgba[t * 4 + 0] = colours[t][0];
    rgba[t * 4 + 1] = colours[t][1];
    rgba[t * 4 + 2] = colours[t][2];
    rgba[t * 4 + 3] = 255;
  }
  struct Case {
    const char* name;
    u32 wrap;  // the glTF enum
    texture::EdgeMode edge;
    f32 uv_max;
    const char* transform;
    u32 (*texel)(u32 cell);  // the texel column (or row) a cell index shows
  };
  const Case cases[] = {
      {"repeat", 10497, texture::EdgeMode::repeat, 3.0f, "", [](u32 i) { return i % 2; }},
      {"mirror", 33648, texture::EdgeMode::mirror, 3.0f, "",
       [](u32 i) { return (i % 4 == 1 || i % 4 == 2) ? 1u : 0u; }},
      {"clamp", 33071, texture::EdgeMode::clamp, 3.0f, "", [](u32 i) { return i < 1 ? 0u : 1u; }},
      {"transform", 10497, texture::EdgeMode::repeat, 1.0f, "{\"scale\":[3,3]}",
       [](u32 i) { return i % 2; }},
  };
  constexpr f32 k_distance = 2.3f;
  const Mat4 view_proj =
      perspective_reversed_z(k_fov, 1.0f, 0.1f) *
      look_at(Vec3{0.0f, 0.0f, k_distance}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
  auto pixel_of = [&](f32 x, f32 y, u32& px, u32& py) {
    const Vec4 clip = view_proj * Vec4{x, y, 0.0f, 1.0f};
    px = static_cast<u32>((clip.x / clip.w * 0.5f + 0.5f) * static_cast<f32>(k_size));
    py = static_cast<u32>((0.5f - clip.y / clip.w * 0.5f) * static_cast<f32>(k_size));
  };

  const test::TempDir tmp("renderer_texture_wrap");
  for (const Case& c : cases) {
    const std::filesystem::path dir = tmp.native() / c.name;
    std::filesystem::create_directories(dir);
    const std::string sampler =
        "{\"magFilter\":9728,\"minFilter\":9984,\"wrapS\":" + std::to_string(c.wrap) +
        ",\"wrapT\":" + std::to_string(c.wrap) + "}";
    REQUIRE(write_quad(dir, c.uv_max, sampler, c.transform));
    Vector<u8> png;
    REQUIRE(write_image(dir, 2, rgba, png));
    const std::string mesh = (dir / "quad.gltf").string();
    const std::string ddc = (dir / "ddc").string();
    for (const bool built : {false, true}) {
      // Decoded without a cache; built through one, where the load builds the texture itself —
      // BC7 sRGB for the base colour, its mips filtered with the edges the sampler wraps with,
      // which the record asks for and the key names.
      Harness h;
      if (!h.build(mesh, ddc, built)) {
        MESSAGE(h.skip);
        return;
      }
      CHECK(h.scene.textures_built() == (built ? 1u : 0u));
      if (built) {
        texture::TextureBuildOptions options;
        options.format = texture::FormatChoice::bc7;
        options.color_space = texture::ColorSpace::srgb;
        options.edge_x = c.edge;
        options.edge_y = c.edge;
        const u64 key = texture::texture_cache_key(hash_bytes(png.data(), png.size()), options);
        CHECK(std::filesystem::exists(texture::texture_cache_path(ddc, key)));
      }
      CHECK(h.scene.material_samplers() == 1u);
      const CapturedFrame f = h.shot(k_distance);
      u32 wrong = 0;
      std::string first_wrong;
      for (u32 j = 0; j < 6; ++j) {
        for (u32 i = 0; i < 6; ++i) {
          // The cell's centre on the quad: x runs -1..1 with u, y runs 1..-1 with v.
          const f32 x = -1.0f + (static_cast<f32>(i) + 0.5f) / 3.0f;
          const f32 y = 1.0f - (static_cast<f32>(j) + 0.5f) / 3.0f;
          u32 px = 0;
          u32 py = 0;
          pixel_of(x, y, px, py);
          REQUIRE(px < k_size);
          REQUIRE(py < k_size);
          const u8* p = &f.color[(py * k_size + px) * 4];
          u32 nearest = 0;
          i32 best = 1 << 30;
          for (u32 t = 0; t < 4; ++t) {
            i32 d = 0;
            for (u32 ch = 0; ch < 3; ++ch) {
              const i32 e = static_cast<i32>(p[ch]) - colours[t][ch];
              d += e * e;
            }
            if (d < best) {
              best = d;
              nearest = t;
            }
          }
          const u32 expected = c.texel(j) * 2 + c.texel(i);
          if (nearest != expected) {
            if (wrong == 0) {
              first_wrong = "cell " + std::to_string(i) + "," + std::to_string(j) +
                            " shows texel " + std::to_string(nearest) + ", expected " +
                            std::to_string(expected);
            }
            ++wrong;
          }
        }
      }
      CHECK_MESSAGE(wrong == 0, c.name << (built ? " (built)" : " (decoded)") << ": " << wrong
                                       << " of 36 cells wrong; " << first_wrong);
    }
  }
}

namespace {

std::string base64(const Vector<u8>& bytes) {
  static constexpr char k_digits[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (u32 i = 0; i < bytes.size(); i += 3) {
    const u32 n = std::min<u32>(3, bytes.size() - i);
    const u32 v = u32{bytes[i]} << 16 | (n > 1 ? u32{bytes[i + 1]} << 8 : 0u) |
                  (n > 2 ? u32{bytes[i + 2]} : 0u);
    out += k_digits[(v >> 18) & 63u];
    out += k_digits[(v >> 12) & 63u];
    out += n > 1 ? k_digits[(v >> 6) & 63u] : '=';
    out += n > 2 ? k_digits[v & 63u] : '=';
  }
  return out;
}

// One scene of a kit's members, instanced, drawn shaded with no shadows (so a scene whose instances
// come and go may draw it too), with the lights' reach pinned so that a scene holding only a prefix
// of the instances is lit as the whole one is.
struct KitRig {
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;

  bool build(const gfx::Device& device, const Vector<std::string>& meshes,
             std::span<const SceneInstance> instances, const std::string& ddc, bool cache,
             bool share, bool dynamic, std::string& skip) {
    std::string error;
    SceneDesc desc;
    for (const std::string& mesh : meshes)
      desc.meshes.push_back(mesh);
    for (const SceneInstance& instance : instances)
      desc.instances.push_back(instance);
    desc.cache = cache;
    desc.ddc = ddc;
    REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
    data.center = WorldPos::origin();
    data.radius = 6.0f;
    data.dynamic = dynamic;
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
    settings.share_textures = share;
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

  CapturedFrame shot() {
    FrameDesc frame;
    frame.camera.position = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 14.0f});
    frame.camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
    frame.camera.fov_y = k_fov;
    CaptureChannels channels;
    channels.color = true;
    channels.ids = true;
    channels.depth = true;
    CapturedFrame out;
    std::string error;
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out, &error), error);
    return out;
  }

  ~KitRig() {
    renderer.destroy();
    scene.destroy();
  }
};

// Pixels whose id, depth or colour differ between two captures of one size.
u32 differing_pixels(const CapturedFrame& a, const CapturedFrame& b) {
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  u32 differ = 0;
  for (u32 p = 0; p < a.width * a.height; ++p) {
    bool d = a.depth[p] != b.depth[p];
    for (u32 w = 0; w < k_id_words; ++w)
      d = d || a.ids[u64{p} * k_id_words + w] != b.ids[u64{p} * k_id_words + w];
    for (u32 c = 0; c < 4; ++c)
      d = d || a.color[u64{p} * 4 + c] != b.color[u64{p} * 4 + c];
    differ += d ? 1u : 0u;
  }
  return differ;
}

}  // namespace

// One upload per distinct image per scene (docs/subsystems/renderer.md, "One upload per distinct
// image"). Four kit members, each a glTF quad of its own in a directory of its own — the way the
// E33 ashlar kit's nineteen members each embed one atlas:
//
//   member 0 and member 1  name their own copy of the atlas, `image.png` beside them;
//   member 2               embeds the same bytes as a data URI, with no path at all;
//   member 3               names an `image.png` too, which is another picture.
//
// Three instances each, drawn from the decoded images (no cache) and from the built textures (a
// cold cache the load fills). With sharing, the scene holds two textures — the atlas once for the
// three members whose bytes match, whatever they called it, and member 3's, which only shares a
// name — and the picture, ids, depth and colour, is the one each member's own upload draws, byte
// for byte. So is the picture of the same scene loaded with one instance and given the rest as a
// tail between frames, which is how a streamed world draws a kit; the tail uploads nothing.
TEST_CASE("renderer: kit members that sample one image upload it once and draw the same picture") {
  const test::TempDir tmp("renderer_texture_kit");
  const std::filesystem::path root = tmp.native();
  constexpr u32 k_image = 64;
  constexpr u32 k_members = 4;
  constexpr u32 k_copies = 3;
  Vector<std::string> meshes;
  for (u32 m = 0; m < k_members; ++m) {
    const std::filesystem::path dir = root / ("member" + std::to_string(m));
    std::filesystem::create_directories(dir);
    Vector<u8> png;
    REQUIRE(
        write_image(dir, k_image, m == 3 ? checker_image(k_image) : smooth_image(k_image), png));
    const std::string uri = m == 2 ? "data:image/png;base64," + base64(png) : "image.png";
    // Different UVs, so four different meshes and four different containers.
    REQUIRE(write_quad(dir, 1.0f + 0.5f * static_cast<f32>(m), "", "", uri));
    meshes.push_back((dir / "quad.gltf").string());
  }
  Vector<SceneInstance> instances;
  for (u32 m = 0; m < k_members; ++m) {
    for (u32 c = 0; c < k_copies; ++c) {
      SceneInstance instance;
      instance.mesh = m;
      instance.transform.position =
          Vec3{(static_cast<f32>(c) - 1.0f) * 2.4f, (1.5f - static_cast<f32>(m)) * 2.4f, 0.0f};
      instances.push_back(instance);
    }
  }
  const std::string ddc = (root / "ddc").string();

  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  for (const bool built : {false, true}) {
    const std::string path = built ? "built" : "decoded";
    INFO(path);
    std::string skip;
    CapturedFrame own_frame;
    u64 own_bytes = 0;
    {
      KitRig own;  // every member uploads its own copy, as before 2026-09-25
      if (!own.build(device, meshes, instances, ddc, built, false, false, skip)) {
        MESSAGE(skip);
        break;
      }
      CHECK((built ? own.scene.textures_built() : own.scene.textures_decoded()) == k_members);
      CHECK(own.scene.texture_references() == k_members);
      CHECK(own.scene.textures_shared() == 0);
      CHECK(own.scene.texture_bytes_saved() == 0);
      own_bytes = own.scene.texture_bytes();
      own_frame = own.shot();
    }
    CHECK(own_frame.covered > 0);

    KitRig shared;
    REQUIRE(shared.build(device, meshes, instances, ddc, built, true, false, skip));
    CHECK(shared.scene.textures_built() == (built ? 2u : 0u));
    CHECK(shared.scene.textures_decoded() == (built ? 0u : 2u));
    CHECK(shared.scene.texture_references() == k_members);
    CHECK(shared.scene.textures_shared() == 2);
    // Four textures of one size before; two now, and the two not uploaded are the saving.
    CHECK(shared.scene.texture_bytes() * 2 == own_bytes);
    CHECK(shared.scene.texture_bytes() + shared.scene.texture_bytes_saved() == own_bytes);
    const CapturedFrame shared_frame = shared.shot();
    MESSAGE(path << ": " << own_bytes << " texture bytes each member its own, "
                 << shared.scene.texture_bytes() << " shared; " << own_frame.covered
                 << " pixels covered");
    CHECK(differing_pixels(own_frame, shared_frame) == 0);

    // The same scene as a prefix of one instance and a tail of the rest, taken between frames: the
    // textures are the scene's, loaded with its meshes, and a tail touches none of them.
    KitRig streamed;
    REQUIRE(streamed.build(device, meshes, std::span<const SceneInstance>(instances.data(), 1), ddc,
                           built, true, true, skip));
    const u32 textures = streamed.scene.textures_built() + streamed.scene.textures_decoded();
    const u64 bytes = streamed.scene.texture_bytes();
    CHECK(textures == 2);
    CHECK(bytes == shared.scene.texture_bytes());
    (void)streamed.shot();
    REQUIRE_MESSAGE(
        streamed.renderer.set_dynamic_instances(
            std::span<const SceneInstance>(instances.data() + 1, instances.size() - 1), &error),
        error);
    CHECK(streamed.scene.textures_built() + streamed.scene.textures_decoded() == textures);
    CHECK(streamed.scene.texture_bytes() == bytes);
    CHECK(streamed.scene.textures_shared() == 2);
    (void)streamed.shot();  // a frame of culling after the change may differ; a pixel may not
    CHECK(differing_pixels(streamed.shot(), shared_frame) == 0);
  }
  device.destroy();
}
