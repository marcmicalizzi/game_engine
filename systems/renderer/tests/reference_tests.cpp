// The reference renderer, headless (docs/plan/04-renderer.md §4.8).
//
// A reference renderer is only worth having if it is right, and "looks converged" is not a test.
// These cases pin it four ways, in increasing order of how much of the engine they involve:
//
//   1. **Furnace.** A closed environment of one radiance, nothing else lit, and a white surface:
//      what comes back is the surface's directional albedo, which for a Lambert dielectric is
//      within a few percent of 1 and for a fully rough metal is the third of it that
//      single-scatter GGX keeps. Both numbers are the ones `domain/gfx/tests/sampling_tests.cpp`
//      computes on the CPU, so this is the GPU integrator held to the CPU twin's answer.
//   2. **Determinism.** The same seed is the same bytes, and a different seed is not.
//   3. **Convergence.** The error against a converged picture falls like 1/sqrt(n).
//   4. **Agreement with the resolve.** At one bounce, with the pixel jitter off, the reference
//      converges to the real-time picture on the shading scenes the engine can build with no
//      content at all — a lit plane, a plane under two point lights, a plane with something
//      casting a shadow on it, and a dark plane giving off light through an emissive texture,
//      which the path tracer sees as radiance where its rays land and the resolve adds as it is.
//
// Every case skips with a message where the machine cannot run it, the way engine-view exits 3:
// no Vulkan device, no 64-bit buffer atomics, or — for these, unlike the rest of the renderer's
// tests — no cluster acceleration structures, because the reference traces the structures the
// frame builds and there are none without them.

#include <domain/gfx/device.h>
#include <foundation/image/metrics.h>
#include <foundation/image/png.h>
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

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_u16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v & 0xffu));
  out.push_back(static_cast<u8>(v >> 8));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

std::string n(u32 v) { return std::to_string(v); }
std::string f(f32 v) { return std::to_string(static_cast<double>(v)); }

// An emissive texture for `write_glb`: the encoded PNG the GLB embeds and the factor it multiplies.
struct Emission {
  std::vector<u8> png;
  Vec3 factor{};
};

// A GLB from explicit geometry and one material's three factors. The renderer's own tests write
// a cube; these cases need the *material* under their control — a white Lambert surface, a white
// metal — which is what a furnace is a test of, so the writer takes the factors rather than
// baking them in. `emission`, when given, embeds its PNG as the material's emissive texture.
bool write_glb(const std::string& path, const std::vector<Vec3>& positions,
               const std::vector<Vec3>& normals, const std::vector<Vec2>& uvs,
               const std::vector<u16>& indices, Vec3 base_color, f32 metallic, f32 roughness,
               const Emission* emission = nullptr) {
  std::vector<u8> bin;
  for (const Vec3& p : positions) {
    put_f32(bin, p.x);
    put_f32(bin, p.y);
    put_f32(bin, p.z);
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (const Vec3& v : normals) {
    put_f32(bin, v.x);
    put_f32(bin, v.y);
    put_f32(bin, v.z);
  }
  const u32 uv_offset = static_cast<u32>(bin.size());
  for (const Vec2& v : uvs) {
    put_f32(bin, v.x);
    put_f32(bin, v.y);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  for (const u16 i : indices)
    put_u16(bin, i);
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  while (bin.size() % 4 != 0)
    bin.push_back(0);
  const u32 png_offset = static_cast<u32>(bin.size());
  if (emission != nullptr) {
    bin.insert(bin.end(), emission->png.begin(), emission->png.end());
    while (bin.size() % 4 != 0)
      bin.push_back(0);
  }
  const std::string emissive_fields =
      emission == nullptr
          ? std::string()
          : ",\"emissiveFactor\":[" + f(emission->factor.x) + "," + f(emission->factor.y) + "," +
                f(emission->factor.z) + "],\"emissiveTexture\":{\"index\":0}";
  const std::string image_fields =
      emission == nullptr ? std::string()
                          : "\"textures\":[{\"source\":0}],\"images\":[{\"bufferView\":4,"
                            "\"mimeType\":\"image/png\"}],";
  const std::string png_view =
      emission == nullptr
          ? std::string()
          : ",{\"buffer\":0,\"byteOffset\":" + n(png_offset) +
                ",\"byteLength\":" + n(static_cast<u32>(emission->png.size())) + "}";

  Vec3 lo = positions[0];
  Vec3 hi = positions[0];
  for (const Vec3& p : positions) {
    lo = Vec3{p.x < lo.x ? p.x : lo.x, p.y < lo.y ? p.y : lo.y, p.z < lo.z ? p.z : lo.z};
    hi = Vec3{p.x > hi.x ? p.x : hi.x, p.y > hi.y ? p.y : hi.y, p.z > hi.z ? p.z : hi.z};
  }
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
      f(base_color.x) + "," + f(base_color.y) + "," + f(base_color.z) +
      ",1],\"metallicFactor\":" + f(metallic) + ",\"roughnessFactor\":" + f(roughness) + "}" +
      emissive_fields + "}]," + image_fields +
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      n(static_cast<u32>(positions.size())) + ",\"type\":\"VEC3\",\"min\":[" + f(lo.x) + "," +
      f(lo.y) + "," + f(lo.z) + "],\"max\":[" + f(hi.x) + "," + f(hi.y) + "," + f(hi.z) +
      "]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      n(static_cast<u32>(normals.size())) +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" +
      n(static_cast<u32>(uvs.size())) +
      ",\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":" +
      n(static_cast<u32>(indices.size())) +
      ",\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "}" + png_view + "],\"buffers\":[{\"byteLength\":" + n(static_cast<u32>(bin.size())) + "}]}";
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

bool write_cube(const std::string& path, Vec3 base_color, f32 metallic, f32 roughness) {
  const Vec3 face[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                        Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangent[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                           Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec2> uvs;
  std::vector<u16> indices;
  for (u32 i = 0; i < 6; ++i) {
    const Vec3 nrm = face[i];
    const Vec3 t = tangent[i];
    const Vec3 b = cross(nrm, t);
    const Vec3 corners[4] = {nrm * 0.5f - t * 0.5f - b * 0.5f, nrm * 0.5f + t * 0.5f - b * 0.5f,
                             nrm * 0.5f + t * 0.5f + b * 0.5f, nrm * 0.5f - t * 0.5f + b * 0.5f};
    const Vec2 corner_uv[4] = {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}};
    const u16 base = static_cast<u16>(positions.size());
    for (u32 c = 0; c < 4; ++c) {
      positions.push_back(corners[c]);
      normals.push_back(nrm);
      uvs.push_back(corner_uv[c]);
    }
    const u16 tris[6] = {base, static_cast<u16>(base + 1), static_cast<u16>(base + 2),
                         base, static_cast<u16>(base + 2), static_cast<u16>(base + 3)};
    for (const u16 t2 : tris)
      indices.push_back(t2);
  }
  return write_glb(path, positions, normals, uvs, indices, base_color, metallic, roughness);
}

// A horizontal quad of side `size` at y = 0, split into a grid so that it has enough triangles to
// cluster and to shade smoothly under a point light.
bool write_plane(const std::string& path, f32 size, u32 cells, Vec3 base_color, f32 metallic,
                 f32 roughness, const Emission* emission = nullptr) {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec2> uvs;
  std::vector<u16> indices;
  const f32 half = size * 0.5f;
  const f32 step = size / static_cast<f32>(cells);
  for (u32 z = 0; z <= cells; ++z) {
    for (u32 x = 0; x <= cells; ++x) {
      positions.push_back(
          Vec3{-half + static_cast<f32>(x) * step, 0.0f, -half + static_cast<f32>(z) * step});
      normals.push_back(Vec3{0.0f, 1.0f, 0.0f});
      uvs.push_back(Vec2{static_cast<f32>(x) / static_cast<f32>(cells),
                         static_cast<f32>(z) / static_cast<f32>(cells)});
    }
  }
  for (u32 z = 0; z < cells; ++z) {
    for (u32 x = 0; x < cells; ++x) {
      const u16 a = static_cast<u16>(z * (cells + 1) + x);
      const u16 b = static_cast<u16>(a + 1);
      const u16 c = static_cast<u16>(a + cells + 1);
      const u16 d = static_cast<u16>(c + 1);
      // Counter-clockwise seen from above, which is the front face in y-up space.
      const u16 tris[6] = {a, c, b, b, c, d};
      for (const u16 t : tris)
        indices.push_back(t);
    }
  }
  return write_glb(path, positions, normals, uvs, indices, base_color, metallic, roughness,
                   emission);
}

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

// A device that can run a reference at all, or nothing. Stricter than the renderer's own `Gpu`:
// the reference traces the frame's cluster acceleration structures, so a device without them
// skips rather than fails.
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
      // In the verdict's own words, which name the extension that is missing.
      why = unavailable_reason(RenderAvailability::NoAccelerationStructures, device);
      return;
    }
    ok = true;
  }
};

// The scene, the settings, the GPU scene, the renderer, and the reference, in the order they
// build in. `--shadows rt` is forced on because that is what makes the frame build the
// structures the reference traces.
struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  ReferenceRenderer reference;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, RenderSettings settings, u32 width,
             u32 height) {
    settings.shadows = ShadowMode::RayTraced;
    if (!load_scene(desc, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (!reference_available(resolved, device, &error)) return false;
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    return reference.create(device, scene, renderer, {}, &error);
  }
};

// One pixel's linear radiance, as the accumulator holds it.
Vec3 hdr_at(const ReferenceFrame& frame, u32 x, u32 y) {
  const u32 base = (y * frame.width + x) * 4;
  return Vec3{frame.hdr[base + 0], frame.hdr[base + 1], frame.hdr[base + 2]};
}

// An RGBA8 buffer as an image the metrics take.
image::Image image_of(u32 width, u32 height, const Vector<u8>& rgba) {
  image::Image out;
  out.width = width;
  out.height = height;
  out.channels = 4;
  out.pixels.resize(rgba.size());
  std::memcpy(out.pixels.data(), rgba.data(), rgba.size());
  return out;
}

}  // namespace

TEST_CASE("reference: a closed white environment comes back white") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("reference unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_reference_furnace");
  const std::filesystem::path dir = tmp.native();

  // **The furnace.** A white surface in a closed environment of one radiance, with nothing else
  // lit: `uniform_sky` makes the environment the sky colour in every direction and switches the
  // sun and the analytic lights off, because a furnace with a sun in it is not a furnace. What
  // comes back is the surface's directional albedo times the environment, and that number is
  // computed independently on the CPU in domain/gfx/tests/sampling_tests.cpp.
  //
  // A cube is convex, so no ray that leaves a face can come back to the cube: one scattering
  // event is the whole transport, and anything missing is the model's rather than the scene's.
  struct Case {
    const char* name;
    f32 metallic;
    f32 roughness;
    f32 lo;  // the fraction of the environment that must come back
    f32 hi;
  };
  // The CPU twin's quadrature at n.v = 0.95 gives 0.9726 for the dielectric and 0.3168 for the
  // fully rough metal; a cube shows a range of view angles, so the bounds are wider than the
  // single number and still far from each other.
  const Case cases[2] = {{"white dielectric, roughness 1", 0.0f, 1.0f, 0.90f, 1.00f},
                         {"white metal, roughness 1", 1.0f, 1.0f, 0.20f, 0.45f}};
  for (const Case& c : cases) {
    const std::string mesh = slashes(dir / (std::string(c.name) + ".glb"));
    REQUIRE(write_cube(mesh, Vec3{1.0f, 1.0f, 1.0f}, c.metallic, c.roughness));
    SceneDesc desc;
    desc.meshes.push_back(mesh);
    desc.ddc = slashes(dir / "ddc");
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, desc, RenderSettings{}, 96, 96), rig.error);

    ReferenceSettings settings;
    settings.spp = 512;
    settings.batch = 64;
    settings.max_bounces = 2;
    settings.uniform_sky = true;
    settings.pixel_center = true;
    ReferenceFrame frame;
    std::string error;
    REQUIRE_MESSAGE(rig.reference.render(orbit_camera(rig.data.center, rig.data.radius, 22.0f, 0),
                                         settings, frame, &error),
                    error);
    CHECK(frame.samples == 512);

    // A corner pixel the cube does not cover sees the environment itself, exactly: that is the
    // statement that the furnace really is closed and uniform.
    const Vec3 background = hdr_at(frame, 1, 1);
    CHECK(background.x == doctest::Approx(0.55).epsilon(1e-5));
    CHECK(background.y == doctest::Approx(0.70).epsilon(1e-5));
    CHECK(background.z == doctest::Approx(0.90).epsilon(1e-5));

    // Every covered pixel returns the same fraction of it in all three channels — a white
    // surface has no colour of its own — and that fraction is the directional albedo.
    f64 total = 0.0;
    u32 covered = 0;
    f32 worst_hi = 0.0f;
    f32 worst_lo = 2.0f;
    for (u32 y = 0; y < frame.height; ++y) {
      for (u32 x = 0; x < frame.width; ++x) {
        const Vec3 value = hdr_at(frame, x, y);
        const f32 ratio = value.y / 0.70f;
        if (std::fabs(ratio - 1.0f) < 1e-4f) continue;  // uncovered: the environment itself
        ++covered;
        total += static_cast<f64>(ratio);
        worst_hi = ratio > worst_hi ? ratio : worst_hi;
        worst_lo = ratio < worst_lo ? ratio : worst_lo;
        CHECK(value.x / 0.55f == doctest::Approx(static_cast<double>(ratio)).epsilon(0.02));
        CHECK(value.z / 0.90f == doctest::Approx(static_cast<double>(ratio)).epsilon(0.02));
      }
    }
    REQUIRE(covered > 1000u);
    const f64 mean = total / static_cast<f64>(covered);
    MESSAGE("furnace, " << std::string(c.name) << ": mean " << mean << " of the environment over "
                        << covered << " pixels, range " << worst_lo << ".." << worst_hi);
    CHECK(mean > static_cast<f64>(c.lo));
    CHECK(mean < static_cast<f64>(c.hi));
    CHECK(mean < 1.0);  // in expectation the surface never gains energy
    // Per pixel it may read a little over 1: a single pixel is 512 samples of a noisy estimator
    // and its standard error is a percent or two, so the bound here catches a lobe that is wrong
    // rather than a sample that is unlucky. The mean above is the energy statement.
    CHECK(worst_hi < 1.10f);
  }
}

TEST_CASE("reference: the same seed is the same picture") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("reference unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_reference_determinism");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube(mesh, Vec3{0.8f, 0.5f, 0.2f}, 0.0f, 0.6f));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, RenderSettings{}, 128, 96), rig.error);
  const Camera camera = orbit_camera(rig.data.center, rig.data.radius, 22.0f, 0);

  ReferenceSettings settings;
  settings.spp = 64;
  settings.batch = 16;
  settings.seed = 7;
  ReferenceFrame a;
  ReferenceFrame b;
  std::string error;
  REQUIRE_MESSAGE(rig.reference.render(camera, settings, a, &error), error);
  REQUIRE_MESSAGE(rig.reference.render(camera, settings, b, &error), error);
  REQUIRE(a.color.size() == b.color.size());
  CHECK(std::memcmp(a.color.data(), b.color.data(), a.color.size()) == 0);
  CHECK(std::memcmp(a.hdr.data(), b.hdr.data(), a.hdr.size() * sizeof(f32)) == 0);

  // A different seed is a different picture — otherwise the seed is not reaching the sampler and
  // "deterministic" would only mean "constant".
  settings.seed = 8;
  ReferenceFrame c;
  REQUIRE_MESSAGE(rig.reference.render(camera, settings, c, &error), error);
  CHECK(std::memcmp(a.color.data(), c.color.data(), a.color.size()) != 0);

  // **The batch size changes the summation order, not the samples.** A pixel's Nth sample is a
  // function of (pixel, N, seed) alone, so four batches of sixteen draw exactly the same paths as
  // one batch of sixty-four; what differs is the order the per-batch sums are added in, and float
  // addition is not associative. So the guarantee is "the same seed *and* the same batch size is
  // the same bytes", and across batch sizes it is "the same picture to within the last bit",
  // which this measures rather than assumes.
  //
  // The byte count is measured rather than asserted tightly, because it is the quantity that
  // would move if the display transform ever grew a value sitting on a rounding boundary — which
  // is exactly what `sky.g` = 0.70 did before the tonemap stopped round-tripping the background
  // (see path_trace.h). It is 0 today, and a tiny linear difference with a large byte count would
  // be that hazard coming back rather than a real divergence.
  settings.seed = 7;
  settings.batch = 64;
  ReferenceFrame single;
  REQUIRE_MESSAGE(rig.reference.render(camera, settings, single, &error), error);
  u32 differing = 0;
  u32 worst_byte = 0;
  f32 worst = 0.0f;
  for (u32 i = 0; i < a.color.size(); ++i) {
    const u32 d = a.color[i] > single.color[i] ? u32{a.color[i]} - single.color[i]
                                               : u32{single.color[i]} - a.color[i];
    if (d != 0) ++differing;
    worst_byte = d > worst_byte ? d : worst_byte;
  }
  for (u32 i = 0; i + 3 < a.hdr.size(); i += 4) {
    for (u32 ch = 0; ch < 3; ++ch) {
      const f32 d = std::fabs(a.hdr[i + ch] - single.hdr[i + ch]);
      worst = d > worst ? d : worst;
    }
  }
  MESSAGE("four batches of 16 against one of 64: worst linear difference "
          << worst << ", " << differing << " of " << a.color.size() << " bytes differ, worst by "
          << worst_byte);
  CHECK(worst < 1e-5f);
  CHECK(worst_byte <= 1u);
}

TEST_CASE("reference: the error falls like one over the square root of the samples") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("reference unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_reference_convergence");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "plane.glb");
  REQUIRE(write_plane(mesh, 8.0f, 16, Vec3{0.7f, 0.7f, 0.7f}, 0.0f, 0.8f));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, RenderSettings{}, 96, 96), rig.error);
  const Camera camera = orbit_camera(rig.data.center, rig.data.radius, 22.0f, 0);

  ReferenceSettings settings;
  settings.batch = 128;
  settings.pixel_center = true;  // a fixed primary ray, so only the transport is being measured
  settings.spp = 4096;
  settings.seed = 101;
  ReferenceFrame converged;
  std::string error;
  REQUIRE_MESSAGE(rig.reference.render(camera, settings, converged, &error), error);

  // One block in the middle of the picture, which the plane covers: the root-mean-square linear
  // error against the converged picture, at four sample counts two doublings apart.
  const u32 x0 = 32;
  const u32 y0 = 40;
  const u32 block = 32;
  auto rms_against = [&](const ReferenceFrame& frame) {
    f64 total = 0.0;
    u32 count = 0;
    for (u32 y = y0; y < y0 + block; ++y) {
      for (u32 x = x0; x < x0 + block; ++x) {
        const Vec3 a = hdr_at(frame, x, y);
        const Vec3 b = hdr_at(converged, x, y);
        const f64 d = static_cast<f64>(a.x - b.x) * static_cast<f64>(a.x - b.x) +
                      static_cast<f64>(a.y - b.y) * static_cast<f64>(a.y - b.y) +
                      static_cast<f64>(a.z - b.z) * static_cast<f64>(a.z - b.z);
        total += d;
        count += 3;
      }
    }
    return std::sqrt(total / static_cast<f64>(count));
  };

  f64 previous = 0.0;
  const u32 counts[4] = {16, 64, 256, 1024};
  for (u32 i = 0; i < 4; ++i) {
    settings.spp = counts[i];
    settings.seed = 202 + i;  // an independent estimate at each count, not a prefix of the last
    ReferenceFrame frame;
    REQUIRE_MESSAGE(rig.reference.render(camera, settings, frame, &error), error);
    const f64 rms = rms_against(frame);
    MESSAGE("convergence: " << counts[i] << " spp, block RMS " << rms);
    if (i > 0) {
      // Four times the samples should halve the error. The bound is generous in both directions
      // because these are single realizations rather than an average over trials, and the last
      // step is measured against a picture that is itself only 4096 samples converged.
      const f64 ratio = previous / rms;
      MESSAGE("  ratio against the previous count: " << ratio << " (1/sqrt(n) predicts 2)");
      CHECK(ratio > 1.4);
      CHECK(ratio < 2.8);
    }
    previous = rms;
  }
}

TEST_CASE("reference: at one bounce it converges to the resolve's picture") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("reference unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_reference_agreement");
  const std::filesystem::path dir = tmp.native();
  const std::string plane = slashes(dir / "plane.glb");
  const std::string cube = slashes(dir / "cube.glb");
  const std::string glowing = slashes(dir / "glowing.glb");
  REQUIRE(write_plane(plane, 8.0f, 16, Vec3{0.7f, 0.7f, 0.7f}, 0.0f, 0.8f));
  REQUIRE(write_cube(cube, Vec3{0.6f, 0.3f, 0.2f}, 0.0f, 0.5f));
  // A dark plane whose emissive texture is a 4x4 checker of a warm and a cool colour, under a
  // factor that is not white: the emission is most of the picture, and both integrators have to
  // read the texture through the same UVs and the same sRGB decode for the two to agree.
  Emission emission;
  emission.factor = Vec3{1.0f, 0.9f, 0.8f};
  {
    std::vector<u8> texels(4 * 4 * 4);
    for (u32 t = 0; t < 16; ++t) {
      const bool warm = ((t % 4) + (t / 4)) % 2 == 0;
      texels[t * 4 + 0] = warm ? 230 : 40;
      texels[t * 4 + 1] = warm ? 120 : 90;
      texels[t * 4 + 2] = warm ? 30 : 200;
      texels[t * 4 + 3] = 255;
    }
    Vector<u8> png;
    REQUIRE(image::encode_png(4, 4, 4, std::span<const u8>(texels.data(), texels.size()), png));
    emission.png.assign(png.data(), png.data() + png.size());
  }
  REQUIRE(write_plane(glowing, 8.0f, 16, Vec3{0.1f, 0.1f, 0.1f}, 0.0f, 0.8f, &emission));

  // Four scenes the engine can build with no content at all, which is what makes this case
  // runnable on any machine that has the device: a lit plane, the same plane with the point
  // lights off (sun only), the plane with a cube hanging over it so that something casts, and a
  // dark plane that gives off light through an emissive texture.
  struct Case {
    const char* name;
    bool lights;
    bool occluder;
    bool emissive;
    f64 flip_bound;
  };
  // The bounds are today's measurements with room for driver noise and a different GPU, not
  // targets: 0.019, 0.021 and 0.028 on the RTX 5090 (docs/subsystems/renderer.md), and 0.014 for
  // the emissive plane when its case was added (2026-09-24). FLIP calls about 0.1 the threshold a
  // person starts to notice, so all four are well under "the same picture" and the bounds are
  // there to catch a regression.
  const Case cases[4] = {
      {"diffuse plane, sun and two point lights", true, false, false, 0.05},
      {"diffuse plane, sun only", false, false, false, 0.05},
      {"shadowed: a cube over the plane", true, true, false, 0.06},
      {"emissive: a dark plane lit by its own texture", true, false, true, 0.05},
  };
  for (const Case& c : cases) {
    SceneDesc desc;
    desc.meshes.push_back(c.emissive ? glowing : plane);
    desc.ddc = slashes(dir / "ddc");
    desc.instances.push_back(SceneInstance{0, Transform3{}});
    if (c.occluder) {
      desc.meshes.push_back(cube);
      SceneInstance instance;
      instance.mesh = 1;
      instance.transform.position = Vec3{0.0f, 1.5f, 0.0f};
      instance.transform.scale = Vec3{2.0f, 2.0f, 2.0f};
      desc.instances.push_back(instance);
    }
    RenderSettings settings;
    settings.lights = c.lights;
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, 128, 128), rig.error);
    const Camera camera = orbit_camera(rig.data.center, rig.data.radius, 22.0f, 0);

    // The real-time picture, through the resolve with its ray-traced shadows.
    FrameDesc frame;
    frame.camera = camera;
    CapturedFrame shot;
    std::string error;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, CaptureChannels{}, shot, &error), error);

    // And the reference of the same frame at one bounce — direct lighting plus the single
    // scattering event whose escaped rays are the sky, which is exactly what the resolve
    // approximates with its hemisphere ambient. The jitter is off so that the comparison is
    // about shading rather than about the antialiasing only one of the two has.
    ReferenceSettings reference_settings;
    reference_settings.spp = 512;
    reference_settings.batch = 64;
    reference_settings.max_bounces = 1;
    reference_settings.pixel_center = true;
    ReferenceFrame reference;
    REQUIRE_MESSAGE(rig.reference.render(camera, reference_settings, reference, &error), error);

    const image::Image a = image_of(shot.width, shot.height, shot.color);
    const image::Image b = image_of(reference.width, reference.height, reference.color);
    image::ImageMetrics metrics;
    REQUIRE_MESSAGE(image::compare_images(a, b, image::MetricsOptions{}, metrics, nullptr, &error),
                    error);
    MESSAGE("agreement, " << std::string(c.name) << ": FLIP mean " << metrics.flip_mean << ", p95 "
                          << metrics.flip_percentile << ", PSNR " << metrics.psnr << ", SSIM "
                          << metrics.ssim);
    // The two do not, and should not, meet exactly: the resolve's ambient evaluates the sky at
    // the surface normal and multiplies by the albedo, while the reference integrates the same
    // sky over the hemisphere, and at one bounce the reference also carries the light that
    // bounced off the rest of the scene. The bound is what is left, measured.
    CHECK(metrics.flip_mean < c.flip_bound);
    CHECK(static_cast<f64>(metrics.ssim) > 0.90);
  }
}
