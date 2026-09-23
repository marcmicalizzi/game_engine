// The atlas seam defect, as a picture (docs/subsystems/geometry.md, "What the simplifier is
// given, and why"). The geometry module's own test measures UVs on the CPU; this one renders.
//
// Why a picture test is needed at all, and why *this* one: every picture test the project had
// compared the engine against **itself at the same LOD threshold**, which is exactly the
// comparison a LOD defect survives. The one that catches it is coarse against finest — the same
// camera, the same lights, the same materials, only a different cut — and it only says anything
// on a mesh whose texture encodes where in the atlas a UV landed, which is what the shredded
// atlas fixture and its probe texture are for. On any smooth-atlas mesh the coarse and the fine
// picture agree whether the simplifier respected the seams or not.
//
// The scene is procedural, so this case needs no fixture file, no sample asset and no cache
// entry; it skips with a message on a machine with no Vulkan device, exactly as engine-view
// exits 3.
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/stress_mesh.h>
#include <domain/gfx/device.h>
#include <foundation/image/metrics.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 384;
constexpr u32 k_height = 384;

struct Harness {
  gfx::Device device;
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  std::string skip;

  bool build(f32 lod_threshold, bool seam_aware = true) {
    SceneDesc desc;
    desc.meshes.push_back("");
    desc.procedural = Procedural::shredded_atlas;
    desc.cache = false;  // nothing here touches the tree's derived-data cache
    if (!seam_aware) {
      // The builder as it was before 2026-09-19: positions only, permissive simplification,
      // nothing tagged. Only the procedural fixture may ask for this, and only because it has no
      // cache entry for the options to disagree with.
      desc.lod.uv_weight = 0.0f;
      desc.lod.normal_weight = 0.0f;
      desc.lod.uv_seams = geometry::SeamRule::none;
      desc.lod.normal_seams = geometry::SeamRule::none;
      desc.lod.skin_seams = geometry::SeamRule::none;
    }
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      skip = "device unavailable: " + error;
      return false;
    }
    // Only a device that cannot is a skip. A scene that does not load or a renderer that does not
    // build is a failure, and reporting it as a skip is how this case passed on the first GPU run
    // on a device without mesh shaders while the renderer could not be created there at all.
    REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
    RenderSettings settings;
    settings.raster = RasterMode::Hardware;
    settings.shadows = ShadowMode::Off;
    settings.lod_px = lod_threshold;
    // Cone culling off in both renders, so the pair counts below compare *cuts* and nothing else.
    // A coarse cluster spans more curvature and so carries a wider cone, which makes it survive
    // the backface test that its own children failed: with cones on, a cut with a third of the
    // clusters can still report two thirds of the visible pairs, which reads as "the threshold
    // did nothing" and is not what happened.
    settings.cone = false;
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      skip = "unavailable here: " + unavailable_reason(availability, device);
      return false;
    }
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
    return true;
  }

  ~Harness() {
    renderer.destroy();
    scene.destroy();
    if (device.valid()) device.destroy();
  }
};

FrameDesc frame_at(const SceneData& data) {
  FrameDesc frame;
  frame.camera = orbit_camera(data.center, data.radius, 3.0f, 0);
  frame.frame_index = 0;
  return frame;
}

image::Image rgb_of(const CapturedFrame& frame) {
  image::Image out;
  out.width = frame.width;
  out.height = frame.height;
  out.channels = 3;
  out.pixels.resize(frame.width * frame.height * 3, u8{0});
  for (u32 p = 0; p < frame.width * frame.height; ++p) {
    out.pixels[p * 3 + 0] = frame.color[p * 4 + 0];
    out.pixels[p * 3 + 1] = frame.color[p * 4 + 1];
    out.pixels[p * 3 + 2] = frame.color[p * 4 + 2];
  }
  return out;
}

}  // namespace

TEST_CASE("renderer: a coarse cut of a shredded atlas paints the same colours as the finest") {
  // The finest cut the threshold can ask for: every leaf cluster, which is the source surface.
  Harness fine;
  if (!fine.build(0.02f)) {
    MESSAGE(fine.skip);
    return;
  }
  std::string error;
  CapturedFrame fine_shot;
  REQUIRE_MESSAGE(fine.renderer.capture(frame_at(fine.data), {.ids = true}, fine_shot, &error),
                  error);
  REQUIRE(fine_shot.covered > 0);
  const u32 fine_pairs = fine.renderer.stats().visible_pairs();

  // A coarse cut of the same scene from the same camera. Before the seam rule this picture was
  // the one covered in the wrong islands' colours.
  Harness coarse;
  if (!coarse.build(64.0f)) {
    MESSAGE(coarse.skip);
    return;
  }
  CapturedFrame coarse_shot;
  REQUIRE_MESSAGE(
      coarse.renderer.capture(frame_at(coarse.data), {.ids = true}, coarse_shot, &error), error);
  const u32 coarse_pairs = coarse.renderer.stats().visible_pairs();
  // It really is a coarser cut, and by enough to be worth comparing: a threshold that shaves a
  // cluster or two would pass this case for the wrong reason.
  REQUIRE(coarse_pairs * 2 < fine_pairs);

  const image::Image fine_image = rgb_of(fine_shot);
  const image::Image coarse_image = rgb_of(coarse_shot);
  image::ImageMetrics metrics;
  REQUIRE_MESSAGE(image::compare_images(fine_image, coarse_image, image::MetricsOptions{}, metrics,
                                        nullptr, &error),
                  error);
  MESSAGE("seam-aware: coarse " << coarse_pairs << " pairs vs finest " << fine_pairs
                                << ": FLIP mean " << metrics.flip_mean << ", p95 "
                                << metrics.flip_percentile << ", max " << metrics.flip_max
                                << ", PSNR " << metrics.psnr << " dB, SSIM " << metrics.ssim);

  // The same pair of pictures from the builder as it was: the assertion is not only that the fix
  // is good but that the defect was visible, which is the half a regression test usually cannot
  // state. Both sides of this comparison come from one binary because the fixture's LOD options
  // are the one place a scene may vary them.
  Harness fine_before;
  Harness coarse_before;
  REQUIRE(fine_before.build(0.02f, false));
  REQUIRE(coarse_before.build(64.0f, false));
  CapturedFrame fine_before_shot;
  CapturedFrame coarse_before_shot;
  REQUIRE_MESSAGE(fine_before.renderer.capture(frame_at(fine_before.data), {.ids = true},
                                               fine_before_shot, &error),
                  error);
  REQUIRE_MESSAGE(coarse_before.renderer.capture(frame_at(coarse_before.data), {.ids = true},
                                                 coarse_before_shot, &error),
                  error);
  image::ImageMetrics before;
  REQUIRE(image::compare_images(rgb_of(fine_before_shot), rgb_of(coarse_before_shot),
                                image::MetricsOptions{}, before, nullptr, &error));
  MESSAGE("position-only: coarse "
          << coarse_before.renderer.stats().visible_pairs() << " pairs vs finest "
          << fine_before.renderer.stats().visible_pairs() << ": FLIP mean " << before.flip_mean
          << ", p95 " << before.flip_percentile << ", max " << before.flip_max << ", PSNR "
          << before.psnr << " dB, SSIM " << before.ssim);

  // The bound is loose on purpose. A coarse cut moves silhouettes and shading, and this mesh is
  // all silhouette — a torus at three radii fills the frame with curvature — so a percent or two
  // of FLIP is honest simplification. What it may not do is **repaint** the surface: cross-island
  // interpolation drags a saturated hue from one slot of the atlas to a completely different one,
  // which is what the position-only column measures.
  CHECK(metrics.flip_mean < 0.03f);
  CHECK(metrics.psnr > 30.0f);
  CHECK(before.flip_mean > metrics.flip_mean * 3.0f);
  CHECK(before.psnr < metrics.psnr - 6.0f);
}

TEST_CASE("renderer: a paged container's leaves are where the part says they are") {
  // Found while measuring the seam rule, and it is the reason that measurement was wrong twice.
  // A `.clusters` container's clusters are in **page order** — coarse first, leaves last — and
  // the single-mesh load path built its `ClusterMeshPart` by hand with `first_leaf_cluster` left
  // at zero, so everything that asks a part where its leaves are got the *coarsest* clusters
  // instead. The camera is what showed it: it frames the leaves' bounds, a coarse cluster's
  // sphere is looser than the surface it stands for, and how loose depends on how far the
  // simplifier got — so two builds of one mesh that differ only in their coarse levels were drawn
  // at two different sizes, and their pictures disagreed by 0.23 FLIP at the finest threshold,
  // where they draw exactly the same triangles. With this right they are byte-identical.
  engine::test::TempDir temp("renderer_paged_leaves");
  geometry::ShreddedAtlasOptions options;
  options.segments = 48;
  options.rings = 24;
  geometry::ShreddedAtlasMesh fixture;
  geometry::build_shredded_atlas_torus(options, fixture);
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(fixture.normals.data(), fixture.normals.size());
  attributes.uvs = std::span<const Vec2>(fixture.uvs.data(), fixture.uvs.size());
  geometry::ClusterFileData file;
  std::string error;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(fixture.positions, fixture.indices, geometry::ClusterLodOptions{},
                                  file.mesh, &error, attributes),
      error);
  geometry::ClusterPagesOptions page_options;
  page_options.page_bytes = 16 * 1024;  // several pages, so the leaves really do move to the end
  REQUIRE_MESSAGE(
      geometry::build_cluster_pages(file.mesh, page_options, file.pages, &error, nullptr), error);
  const std::string path = temp.file("fixture.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, file, &error), error);

  SceneDesc desc;
  desc.meshes.push_back(path);
  desc.cache = false;
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.parts.size() == 1);
  const geometry::ClusterMeshPart& part = data.parts[0];
  REQUIRE(part.leaf_cluster_count > 0);
  REQUIRE(part.first_leaf_cluster + part.leaf_cluster_count <= part.cluster_count);
  // The leaves are a contiguous run at `first_leaf_cluster`, and nothing before it is one.
  for (u32 i = 0; i < part.cluster_count; ++i) {
    const bool leaf = data.lod.lod[part.first_cluster + i].level == 0;
    CHECK(leaf == (i >= part.first_leaf_cluster));
  }
  // And a paged container really is the case that used to be wrong: its leaves are not at zero.
  CHECK(part.first_leaf_cluster > 0);
  MESSAGE("paged container: " << part.cluster_count << " clusters, " << part.leaf_cluster_count
                              << " leaves starting at " << part.first_leaf_cluster);
}
