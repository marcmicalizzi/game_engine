// Geometry residency end to end, headless (docs/plan/04-renderer.md §4.9,
// docs/subsystems/renderer.md "Geometry streaming"). The scene is the procedural heightfield,
// because it is the one scene the engine can build with no content at all and it is large enough
// to be many pages; the cases skip with a message on a machine with no Vulkan device or no 64-bit
// buffer atomics, exactly as engine-view exits 3.
//
// What these hold, in the order they matter: a budget that holds the whole page table draws
// exactly what an unstreamed render draws; a **starved** budget still covers every pixel the full
// picture covers — coarser, never a hole, which is the whole claim of the drawing rule's fallback
// clause; residency stays ancestor-closed and never drops a pinned page, which is what makes the
// fallback crack-free; and a request is eventually served under an upload budget small enough that
// it cannot be served at once.
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/page_source.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 240;

// The heightfield at a resolution that gives the page layout something to do: 129 x 129 vertices
// is 32,768 triangles and, at a 16 KB target, a few dozen pages.
SceneDesc heightfield_desc(bool stream, u32 page_bytes = 0) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 129;
  desc.cache = false;  // nothing here touches the tree's derived-data cache
  desc.stream = stream;
  desc.page_bytes = page_bytes;
  return desc;
}

RenderSettings streamed_settings(u64 budget_bytes, u32 upload_budget) {
  RenderSettings settings;
  settings.raster = RasterMode::Hardware;
  settings.shadows = ShadowMode::Off;
  settings.stream = true;
  settings.page_budget_bytes = budget_bytes;
  settings.upload_budget_bytes = upload_budget;
  return settings;
}

// How many pixels the scene covered, and which ones: a coverage mask is what "coarser but never a
// hole" is measured against, because a coarse cut puts *different* triangles under a pixel and
// only the fact that something is there is comparable.
void coverage_of(const CapturedFrame& frame, Vector<u8>& out) {
  out.assign(frame.width * frame.height, u8{0});
  for (u32 p = 0; p < frame.width * frame.height; ++p)
    out[p] = frame.ids[p * k_id_words] != k_no_id ? u8{1} : u8{0};
}

struct Harness {
  gfx::Device device;
  SceneData data;
  GpuScene scene;
  FilePageSource source;
  SceneRenderer renderer;
  bool ready = false;
  bool from_file = false;
  std::string skip;

  // `jobs` non-null asks for the container-backed page source: the meshes' `.clusters` files are
  // opened and the merged host streams are released, so the run reads its pages off disk.
  bool build(const SceneDesc& desc, const RenderSettings& settings,
             jobs::JobSystem* jobs = nullptr) {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      skip = "device unavailable: " + error;
      return false;
    }
    if (!load_scene(desc, data, error)) {
      skip = "scene: " + error;
      return false;
    }
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      skip = std::string("device ") +
             availability_message(check_availability(resolved, device.features()));
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) {
      skip = "gpu scene: " + error;
      return false;
    }
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    if (jobs != nullptr) {
      if (!attach_page_source(data, scene, *jobs, source, &error)) {
        skip = "page source: " + error;
        return false;
      }
      rd.page_source = &source;
      from_file = true;
    }
    if (!renderer.create(device, scene, resolved, rd, &error)) {
      skip = "renderer: " + error;
      return false;
    }
    ready = true;
    return true;
  }

  ~Harness() {
    renderer.destroy();
    source.destroy();
    scene.destroy();
    if (device.valid()) device.destroy();
  }
};

// The heightfield, laid out in pages and written to a `.clusters` container, so that a scene with
// a **file** behind it can be built without importing anything: the procedural scene has no
// container of its own, which is exactly why it keeps the in-memory source.
bool write_heightfield_container(const std::string& path, u32 page_bytes, std::string& error) {
  SceneData data;
  if (!load_scene(heightfield_desc(true, page_bytes), data, error)) return false;
  geometry::ClusterFileData file;
  file.mesh = std::move(data.lod);
  file.pages = std::move(data.pages);
  return geometry::write_cluster_file(path, file, &error);
}

SceneDesc container_desc(const std::string& path) {
  SceneDesc desc;
  desc.meshes.push_back(path);
  desc.cache = false;  // the container is named outright; nothing goes near the tree's cache
  desc.stream = true;
  return desc;
}

FrameDesc frame_at(const SceneData& data, f32 distance, u64 index) {
  FrameDesc frame;
  frame.camera = orbit_camera(data.center, data.radius, distance, 0);
  frame.frame_index = index;
  return frame;
}

// Run frames until nothing is outstanding, and say how many it took. "Nothing outstanding" has to
// be **several frames in a row**, not one: a frame's requests reach the manager `frames_in_flight`
// frames later, so a queue that is empty right now may only mean that the frame that would have
// filled it has not been read back yet. A cap, because a test that hangs is worse than one that
// fails.
constexpr u32 k_quiet_frames = 4;

u32 converge(SceneRenderer& renderer, const SceneData& data, f32 distance, u32 max_frames) {
  std::string error;
  u32 quiet = 0;
  for (u32 f = 0; f < max_frames; ++f) {
    FrameDesc frame = frame_at(data, distance, f);
    REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
    const StreamStats& stream = renderer.streamer().stats();
    quiet = stream.pending == 0 && !renderer.streamer().has_uploads() ? quiet + 1 : 0;
    if (quiet >= k_quiet_frames) return f + 1;
  }
  return max_frames;
}

}  // namespace

TEST_CASE("streaming: a budget that holds every page draws what an unstreamed render draws") {
  Harness streamed;
  if (!streamed.build(heightfield_desc(true), streamed_settings(0, 0))) {
    MESSAGE(streamed.skip);
    return;
  }
  REQUIRE(streamed.scene.streamed());
  REQUIRE(streamed.scene.page_count() > 1);
  MESSAGE("pages " << streamed.scene.page_count() << ", slots " << streamed.scene.page_slots()
                   << ", page bytes " << streamed.renderer.streamer().stats().page_bytes);

  std::string error;
  converge(streamed.renderer, streamed.data, 6.0f, 64);
  CapturedFrame streamed_shot;
  REQUIRE_MESSAGE(streamed.renderer.capture(frame_at(streamed.data, 6.0f, 8),
                                            {.color = true, .ids = true}, streamed_shot, &error),
                  error);
  // Not every page: streaming is **demand paging**, so a budget that could hold the whole mesh
  // still only holds what the cut has asked for. What has to be true is that nothing was evicted
  // and that the picture is the unstreamed one.
  const StreamStats& stats = streamed.renderer.streamer().stats();
  CHECK(stats.evictions == 0);
  CHECK(stats.pages_resident > 0);
  CHECK(stats.pending == 0);

  // The same scene without streaming. Its clusters are **not** renumbered, so the ids differ; what
  // has to agree is the picture and what it covers, because the two cuts are the same surfaces.
  Harness plain;
  RenderSettings plain_settings;
  plain_settings.raster = RasterMode::Hardware;
  plain_settings.shadows = ShadowMode::Off;
  if (!plain.build(heightfield_desc(false), plain_settings)) {
    MESSAGE(plain.skip);
    return;
  }
  CHECK_FALSE(plain.scene.streamed());
  CapturedFrame plain_shot;
  REQUIRE_MESSAGE(plain.renderer.capture(frame_at(plain.data, 6.0f, 8),
                                         {.color = true, .ids = true}, plain_shot, &error),
                  error);
  REQUIRE(streamed_shot.color.size() == plain_shot.color.size());
  u32 differing = 0;
  for (u32 i = 0; i < streamed_shot.color.size(); ++i)
    differing += streamed_shot.color[i] != plain_shot.color[i] ? 1u : 0u;
  MESSAGE("covered " << streamed_shot.covered << " vs " << plain_shot.covered << ", " << differing
                     << " colour bytes differ");
  CHECK(streamed_shot.covered == plain_shot.covered);
  CHECK(differing == 0);
}

TEST_CASE("streaming: a starved budget is coarser and never has a hole") {
  // Every page resident first, so there is a picture to compare against.
  Harness full;
  if (!full.build(heightfield_desc(true), streamed_settings(0, 0))) {
    MESSAGE(full.skip);
    return;
  }
  std::string error;
  converge(full.renderer, full.data, 6.0f, 64);
  CapturedFrame full_shot;
  REQUIRE_MESSAGE(
      full.renderer.capture(frame_at(full.data, 6.0f, 8), {.ids = true}, full_shot, &error), error);
  Vector<u8> full_cover;
  coverage_of(full_shot, full_cover);
  const u64 whole = full.renderer.streamer().stats().page_bytes;
  REQUIRE(whole > 0);

  Harness starved;
  if (!starved.build(heightfield_desc(true), streamed_settings(whole / 4, 64 * 1024))) {
    MESSAGE(starved.skip);
    return;
  }
  const u32 frames = converge(starved.renderer, starved.data, 6.0f, 256);
  const StreamStats& stats = starved.renderer.streamer().stats();
  MESSAGE("quarter budget: " << stats.pages_resident << " of " << stats.pages_total
                             << " pages resident, converged in " << frames << " frames, "
                             << stats.uploads << " uploads, " << stats.evictions << " evictions");
  CHECK(stats.pages_resident < stats.pages_total);
  CHECK(stats.pages_resident <= stats.page_slots);
  CHECK(stats.uploads > 0);

  CapturedFrame starved_shot;
  REQUIRE_MESSAGE(starved.renderer.capture(frame_at(starved.data, 6.0f, 8), {.ids = true},
                                           starved_shot, &error),
                  error);
  Vector<u8> starved_cover;
  coverage_of(starved_shot, starved_cover);
  REQUIRE(starved_cover.size() == full_cover.size());
  u32 holes = 0;
  u32 extra = 0;
  for (u32 p = 0; p < full_cover.size(); ++p) {
    holes += full_cover[p] != 0 && starved_cover[p] == 0 ? 1u : 0u;
    extra += full_cover[p] == 0 && starved_cover[p] != 0 ? 1u : 0u;
  }
  // A coarser cut moves a silhouette by a pixel or two, so the two coverages are not identical;
  // what they may not do is leave a *region* of the surface empty. One part in a thousand of the
  // covered pixels is the width of the silhouettes in this frame.
  const u32 tolerance = full_shot.covered / 1000 + 8;
  MESSAGE("holes " << holes << ", extra " << extra << ", tolerance " << tolerance);
  CHECK(holes <= tolerance);
  CHECK(extra <= tolerance);
}

TEST_CASE("streaming: residency stays ancestor-closed and never drops a pinned page") {
  // 16 KB pages rather than the default 128 KB, so the mesh is dozens of pages and a budget can
  // hold a fraction of them: with eight pages of a tenth of a megabyte each there is nothing an
  // eviction could choose between.
  Harness h;
  if (!h.build(heightfield_desc(true, 16 * 1024), streamed_settings(0, 0))) {
    MESSAGE(h.skip);
    return;
  }
  const u64 whole = h.renderer.streamer().stats().page_bytes;
  h.renderer.destroy();
  h.scene.destroy();

  // A tenth of the page bytes and a quarter-pixel error threshold, so the cut wants most of the
  // mesh and the budget holds a fraction of it; the camera then moves in and out so that what the
  // manager evicts keeps changing. Without both levers a small scene simply fits and the eviction
  // path is never taken.
  std::string error;
  RenderSettings settings = streamed_settings(whole / 10, 48 * 1024);
  settings.lod_px = 0.25f;
  ResolvedSettings resolved;
  resolve_settings(settings, h.device.features(), &h.data, resolved);
  REQUIRE_MESSAGE(h.scene.create(h.device, h.data, resolved, &error), error);
  SceneRenderer::Desc rd;
  rd.width = k_width;
  rd.height = k_height;
  REQUIRE_MESSAGE(h.renderer.create(h.device, h.scene, resolved, rd, &error), error);

  const geometry::ClusterPages& pages = h.data.pages;
  for (u32 f = 0; f < 48; ++f) {
    const f32 distance = 1.0f + 3.0f * static_cast<f32>(f % 8) / 7.0f;
    FrameDesc frame = frame_at(h.data, distance, f);
    REQUIRE_MESSAGE(h.renderer.render_offscreen(frame, &error), error);
  }
  // The residency the streamer wrote for each frame slot, which is exactly what that frame's cull
  // pass read. Two things have to hold of every one of those snapshots.
  //
  // A **pinned** page is always there: nothing coarser can stand in for one, so evicting it would
  // leave a hole no fallback can fill, and `PageResidencyManager` never offers one as a candidate.
  //
  // And every resident page that is not pinned has **something above it** — at least one page that
  // names it as a child is resident too. That is the honest form of the ancestor-closure invariant
  // on a *page* rather than on a group: a page holds many groups and each group has its own parent
  // group, so "the page holding its parents" is several pages, and what eviction enforces is the
  // conservative direction of it (a page with any resident child page may not go). See
  // docs/subsystems/renderer.md, "What a page's ancestors are".
  Vector<u32> parents;
  for (u32 slot = 0; slot < k_stream_slots; ++slot) {
    const u32* words = h.scene.residency_slot(slot);
    bool any = false;
    for (u32 p = 0; p < h.scene.page_count(); ++p)
      any = any || words[p] != 0;
    if (!any) continue;  // a slot no frame has used yet
    parents.assign(h.scene.page_count(), 0u);
    for (u32 p = 0; p < h.scene.page_count(); ++p) {
      if (words[p] == 0) continue;
      const geometry::ClusterPageDesc& desc = pages.pages[p];
      for (u32 k = 0; k < desc.child_page_count; ++k)
        parents[pages.child_pages[desc.first_child_page + k]] = 1u;
    }
    for (u32 p = 0; p < h.scene.page_count(); ++p) {
      const bool root = (pages.pages[p].flags & geometry::k_page_root) != 0;
      if (root) {
        CHECK_MESSAGE(words[p] != 0, "pinned page " << p << " was evicted");
        continue;
      }
      if (words[p] == 0) continue;
      // A page that holds more than one DAG level is **its own parent**, and the child list says
      // so by omission: `build_cluster_pages` leaves a page out of its own child run, because the
      // run exists for eviction to ask "is anything outside me depending on me". Such a page needs
      // nothing above it.
      const bool self_parent = pages.pages[p].level_min != pages.pages[p].level_max;
      const bool covered = parents[p] != 0 || self_parent;
      if (!covered) {
        std::string who;
        for (u32 q = 0; q < h.scene.page_count(); ++q) {
          const geometry::ClusterPageDesc& d = pages.pages[q];
          for (u32 k = 0; k < d.child_page_count; ++k) {
            if (pages.child_pages[d.first_child_page + k] != p) continue;
            who += " " + std::to_string(q) + (words[q] != 0 ? "(in)" : "(out)");
          }
        }
        MESSAGE("page " << p << " levels " << pages.pages[p].level_min << ".."
                        << pages.pages[p].level_max << " flags " << pages.pages[p].flags
                        << " parents:" << (who.empty() ? std::string(" none") : who));
      }
      CHECK_MESSAGE(covered, "page " << p << " is resident with nothing above it");
    }
  }
  const StreamStats& stats = h.renderer.streamer().stats();
  MESSAGE("evictions " << stats.evictions << ", uploads " << stats.uploads << " ("
                       << stats.uploads_bytes << " bytes), requests " << stats.requests);
  CHECK(stats.evictions > 0);
}

TEST_CASE("streaming: pages read from the container draw what pages read from memory draw") {
  const test::TempDir tmp("engine_renderer_pagesrc");
  const std::string container = tmp.file("terrain.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_heightfield_container(container, 16 * 1024, error), error);

  // The same container twice: once with the payloads copied out of the `SceneData` the load
  // produced, once with them read back out of the file by range. The scene is identical down to
  // the cluster numbering, so this is the strongest comparison available — not "the same surface"
  // but **the same bytes and the same ids**, which is what a source that got an offset wrong
  // could not produce.
  Harness memory;
  if (!memory.build(container_desc(container), streamed_settings(0, 0))) {
    MESSAGE(memory.skip);
    return;
  }
  REQUIRE(memory.scene.streamed());
  REQUIRE(memory.scene.page_count() > 1);
  converge(memory.renderer, memory.data, 6.0f, 128);
  CapturedFrame memory_shot;
  REQUIRE_MESSAGE(memory.renderer.capture(frame_at(memory.data, 6.0f, 8),
                                          {.color = true, .ids = true}, memory_shot, &error),
                  error);
  CHECK_FALSE(memory.renderer.streamer().stats().from_file);

  jobs::JobSystemConfig config;
  config.performance_workers = 2;
  config.efficiency_workers = 2;
  config.pin_threads = false;
  jobs::JobSystem js(config);

  Harness file;
  if (!file.build(container_desc(container), streamed_settings(0, 0), &js)) {
    MESSAGE(file.skip);
    return;
  }
  // The host streams are gone, which is the whole point: the residency budget now bounds host
  // memory the way it already bounded device memory.
  CHECK(file.data.lod.mesh.quantized.empty());
  CHECK(file.data.lod.mesh.attributes.empty());
  CHECK(file.data.lod.mesh.triangles.empty());
  CHECK(file.data.lod.mesh.vertices.empty());
  CHECK(file.source.released_bytes() > 0);
  CHECK(paged_stream_bytes(file.data) == 0);

  const u32 frames = converge(file.renderer, file.data, 6.0f, 256);
  CapturedFrame file_shot;
  REQUIRE_MESSAGE(file.renderer.capture(frame_at(file.data, 6.0f, 8), {.color = true, .ids = true},
                                        file_shot, &error),
                  error);
  const StreamStats& stats = file.renderer.streamer().stats();
  MESSAGE("from file: converged in " << frames << " frames, " << stats.uploads << " uploads, "
                                     << stats.file_reads << " reads of " << stats.file_bytes
                                     << " bytes, " << stats.host_bytes_freed << " host bytes freed"
                                     << ", " << stats.load_waits << " frames waiting on a read");
  CHECK(stats.from_file);
  CHECK(stats.file_reads > 0);
  CHECK(stats.file_bytes > 0);
  CHECK(stats.host_bytes_freed > 0);
  CHECK(stats.loads_in_flight == 0);
  CHECK(stats.pending == 0);
  // A page is three reads here — quantized positions, attributes, triangles — because this scene
  // builds no acceleration structures. The float positions are the fourth when it does.
  CHECK(stats.file_reads == stats.uploads * 3);

  REQUIRE(file_shot.color.size() == memory_shot.color.size());
  u32 differing = 0;
  for (u32 i = 0; i < file_shot.color.size(); ++i)
    differing += file_shot.color[i] != memory_shot.color[i] ? 1u : 0u;
  u32 id_differing = 0;
  REQUIRE(file_shot.ids.size() == memory_shot.ids.size());
  for (u32 i = 0; i < file_shot.ids.size(); ++i)
    id_differing += file_shot.ids[i] != memory_shot.ids[i] ? 1u : 0u;
  MESSAGE("covered " << file_shot.covered << " vs " << memory_shot.covered << ", " << differing
                     << " colour bytes and " << id_differing << " id words differ");
  CHECK(file_shot.covered == memory_shot.covered);
  CHECK(differing == 0);
  CHECK(id_differing == 0);
}

TEST_CASE("streaming: a scene with no container behind it keeps the in-memory source") {
  // The procedural heightfield has no file, and a source that could answer for some pages and not
  // others would be worse than none — the caller's next act is to release the streams the rest
  // would have to come from. So it is refused with the reason, and the run streams from memory.
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(heightfield_desc(true, 16 * 1024), data, error), error);
  CHECK(paged_stream_bytes(data) > 0);

  Harness h;
  if (!h.build(heightfield_desc(true, 16 * 1024), streamed_settings(0, 0))) {
    MESSAGE(h.skip);
    return;
  }
  jobs::JobSystemConfig config;
  config.performance_workers = 2;
  config.efficiency_workers = 2;
  config.pin_threads = false;
  jobs::JobSystem js(config);
  FilePageSource source;
  CHECK_FALSE(attach_page_source(h.data, h.scene, js, source, &error));
  MESSAGE("refused: " << error);
  CHECK(error.find("container") != std::string::npos);
  // And nothing was released: a refusal leaves the scene exactly as it was, or the run that
  // followed it would have no pages to copy at all.
  CHECK(paged_stream_bytes(h.data) > 0);
}

TEST_CASE("streaming: a request is served under an upload budget of one page a frame") {
  Harness h;
  // An upload budget below a page is raised to one page, which is the smallest a frame can move
  // and still converge; that is the configuration the starvation guard has to survive.
  if (!h.build(heightfield_desc(true), streamed_settings(0, 1))) {
    MESSAGE(h.skip);
    return;
  }
  CHECK(h.scene.upload_budget_bytes() > 0);
  const u32 frames = converge(h.renderer, h.data, 2.0f, 512);
  const StreamStats& stats = h.renderer.streamer().stats();
  MESSAGE("one page a frame: converged in " << frames << " frames, " << stats.uploads
                                            << " uploads, " << stats.requests << " requests");
  CHECK(frames < 512);
  CHECK(stats.pending == 0);
  CHECK(stats.uploads > 1);
  CHECK(stats.overflows == 0);
}
