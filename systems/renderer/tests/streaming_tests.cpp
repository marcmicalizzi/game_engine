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
#include <domain/gfx/vulkan.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/page_source.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cstring>
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

// How many pixels of `reference` that are **`margin` pixels inside its coverage** are not covered
// by `cover`. Eroding first is what separates "the surface has a hole in it" from "the silhouette
// moved", which is the only difference a coarser cut is allowed to make.
u32 interior_misses(const Vector<u8>& reference, const Vector<u8>& cover, u32 width, u32 height,
                    u32 margin) {
  u32 missing = 0;
  for (u32 y = margin; y + margin < height; ++y) {
    for (u32 x = margin; x + margin < width; ++x) {
      bool interior = true;
      for (u32 dy = 0; dy <= 2 * margin && interior; ++dy) {
        for (u32 dx = 0; dx <= 2 * margin && interior; ++dx)
          interior = reference[(y + dy - margin) * width + (x + dx - margin)] != 0;
      }
      if (interior && cover[y * width + x] == 0) ++missing;
    }
  }
  return missing;
}

// A page source whose loads land **when it says**, not when a disk does: `latency` ticks after
// they begin, where the test ticks once a frame. The bytes are the scene's own host streams laid
// out as `GpuScene::page_stage_layout` says, so a page it serves is the page a file would have
// served. A read that `fail_page` names comes back short `fail_times` times before it lands.
//
// It exists because the stall it reproduces needs reads that take several frames, which a
// container the test just wrote — sitting in the OS file cache — never does: the flythrough of
// 2026-09-24 stalled on its first run over a cold cache and on none of the three reruns
// (docs/subsystems/renderer.md, "Admission never waits on a read it cannot start").
class SlowPageSource final : public PageSource {
 public:
  static constexpr u32 k_loads = k_page_loads;  // the file source's load slots

  void create(const SceneData& data, const GpuScene& scene, u32 latency) {
    data_ = &data;
    scene_ = &scene;
    latency_ = latency;
    u64 stride = 0;
    for (u32 p = 0; p < scene.page_count(); ++p)
      stride = std::max(stride, scene.page_payload_bytes(p));
    stride_ = stride;
    buffers_.assign(static_cast<u32>(stride * k_loads), u8{0});
  }
  void tick() noexcept { ++now_; }
  u32 fail_page = ~u32{0};
  u32 fail_times = 0;

  bool valid() const noexcept override { return scene_ != nullptr; }
  bool begin(u32 page, u32& handle) override {
    handle = k_no_load;
    u32 slot = k_loads;
    for (u32 i = 0; i < k_loads; ++i) {
      if (!loads_[i].busy) {
        slot = i;
        break;
      }
    }
    if (slot == k_loads) return false;
    const geometry::ClusterPageDesc& desc = data_->pages.pages[page];
    const geometry::ClusterMesh& mesh = data_->lod.mesh;
    const GpuScene::PageStage stage = scene_->page_stage_layout(page);
    u8* dst = buffers_.data() + stride_ * slot;
    auto put = [&](const void* source, u64 at, u64 bytes) {
      if (bytes > 0) std::memcpy(dst + at, source, static_cast<size_t>(bytes));
    };
    put(mesh.quantized.data() + u64{desc.first_vertex} * 3, stage.quantized,
        u64{desc.vertex_count} * 3 * sizeof(u16));
    put(mesh.attributes.data() + desc.first_vertex, stage.attributes,
        u64{desc.vertex_count} * sizeof(geometry::VertexAttributes));
    put(mesh.triangles.data() + desc.first_triangle, stage.triangles,
        u64{desc.triangle_count} * sizeof(u32));
    if (stage.ray_tracing) {
      put(mesh.vertices.data() + desc.first_vertex, stage.vertices,
          u64{desc.vertex_count} * sizeof(Vec3));
    }
    Load& load = loads_[slot];
    load.busy = true;
    load.page = page;
    load.began = now_;
    load.fails = page == fail_page && failed_ < fail_times;
    if (load.fails) ++failed_;
    ++in_flight_;
    ++reads_;
    bytes_read_ += stage.total;
    ++begun_;
    handle = slot;
    return true;
  }
  bool done(u32 handle) const noexcept override {
    return handle < k_loads && loads_[handle].busy && now_ - loads_[handle].began >= latency_;
  }
  bool complete(u32 handle) const noexcept override {
    return done(handle) && !loads_[handle].fails;
  }
  const u8* bytes(u32 handle) const noexcept override {
    return handle < k_loads ? buffers_.data() + stride_ * handle : nullptr;
  }
  void release(u32 handle) override {
    if (handle >= k_loads || !loads_[handle].busy) return;
    loads_[handle].busy = false;
    --in_flight_;
  }
  u32 in_flight() const noexcept override { return in_flight_; }
  u32 capacity() const noexcept override { return k_loads; }
  u64 bytes_read() const noexcept override { return bytes_read_; }
  u64 reads() const noexcept override { return reads_; }
  u64 released_bytes() const noexcept override { return 0; }
  u64 begun() const noexcept { return begun_; }

 private:
  struct Load {
    u64 began = 0;
    u32 page = 0;
    bool busy = false;
    bool fails = false;
  };
  const SceneData* data_ = nullptr;
  const GpuScene* scene_ = nullptr;
  u32 latency_ = 0;
  u64 now_ = 0;
  u64 stride_ = 0;
  Vector<u8> buffers_;
  Load loads_[k_loads];
  u32 in_flight_ = 0;
  u32 failed_ = 0;
  u64 reads_ = 0;
  u64 bytes_read_ = 0;
  u64 begun_ = 0;
};

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
  // opened and the merged host streams are released, so the run reads its pages off disk. `slow`
  // non-null serves the pages from a `SlowPageSource` of `latency` ticks instead, over the host
  // streams, which it keeps.
  bool build(const SceneDesc& desc, const RenderSettings& settings, jobs::JobSystem* jobs = nullptr,
             SlowPageSource* slow = nullptr, u32 latency = 0) {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      skip = "device unavailable: " + error;
      return false;
    }
    // Only a device that cannot is a skip: no device above, and `check_availability` below.
    // Everything else is a failure. This harness used to report a renderer that would not build
    // as a skip too, which is how all eight of this file's cases "passed" on the first GPU run on
    // a device without mesh shaders while not one frame was drawn there.
    REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
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
    if (jobs != nullptr) {
      REQUIRE_MESSAGE(attach_page_source(data, scene, *jobs, source, &error),
                      "page source: " << error);
      rd.page_source = &source;
      from_file = true;
    } else if (slow != nullptr) {
      slow->create(data, scene, latency);
      rd.page_source = slow;
      from_file = true;
    }
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
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

// A device-local pool stream copied into host memory. The page pool's buffers are transfer
// sources for exactly this reason: what a test can say about streaming is otherwise limited to
// what reaches a picture, and a page whose bytes are wrong is not always a picture at all.
bool read_pool(const gfx::Device& device, const gfx::BufferResource& source, Vector<u8>& out,
               std::string* error) {
  gfx::BufferResource staging;
  if (!gfx::create_buffer(device, source.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, staging,
                          error)) {
    return false;
  }
  const bool ok = gfx::submit_immediate(
      device,
      [&](VkCommandBuffer cb) {
        const VkBufferCopy copy{0, 0, source.size};
        vkCmdCopyBuffer(cb, source.buffer, staging.buffer, 1, &copy);
      },
      error);
  if (ok) {
    out.resize(static_cast<u32>(source.size));
    std::memcpy(out.data(), staging.mapped, source.size);
  }
  gfx::destroy_buffer(device, staging);
  return ok;
}

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
  // what they may not do is leave a *region* of the surface empty. That is asked the way the
  // fly-in below asks it: the reference coverage **eroded by two pixels**, every surviving pixel
  // covered, with no tolerance to calibrate.
  //
  // It used to be a count against "one part in a thousand of the covered pixels, plus eight",
  // which measured the silhouette's shape rather than the streaming — and was a calibration of
  // one GPU: this budget never settles (a quarter of the pages cannot hold the cut, so the
  // manager uploads and evicts until the frame cap), the capture takes whichever pages are in at
  // that frame, and the RTX 5090 left 65 edge pixels against a tolerance of 66 on the mesh and
  // the vertex path alike. The TITAN Xp's cut of the same frame left 70, every one of them on an
  // edge; the first run of this case there, 2026-09-23, failed on that count
  // (docs/ci/self-hosted-runners.md, "The first run on the Titan Xp").
  const u32 interior_holes = interior_misses(full_cover, starved_cover, k_width, k_height, 2);
  const u32 tolerance = full_shot.covered / 1000 + 8;
  MESSAGE("holes " << holes << " (" << interior_holes << " interior), extra " << extra
                   << ", silhouette allowance " << tolerance);
  CHECK_MESSAGE(interior_holes == 0,
                interior_holes << " interior pixels of " << full_shot.covered << " are missing");
  // And the fly-in's loose bound on the whole frame, which would catch "half the surface is gone"
  // if the erosion ever stopped being the tight statement it is.
  CHECK(u64{holes} * 20 <= u64{full_shot.covered} + 160);
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

TEST_CASE("streaming: a fly-in is coarser at every step and never has a hole") {
  // **The guarantee the feature owes**, as an assertion rather than a table. A fly-in is where a
  // budget is actually under pressure — the cut grows by an order of magnitude over the path and
  // the pages it wants arrive two or three frames after it wanted them — so "coarser, never
  // holes" has to hold at *every step* and not only once the camera has stopped.
  //
  // The reference is the same path drawn with every page resident, step for step: the cameras are
  // a function of the step index alone, so the two runs look at exactly the same thing.
  constexpr u32 k_steps = 24;
  constexpr f32 k_from = 8.0f;  // mesh radii
  constexpr f32 k_to = 0.9f;

  Harness full;
  if (!full.build(heightfield_desc(true, 16 * 1024), streamed_settings(0, 0))) {
    MESSAGE(full.skip);
    return;
  }
  const u64 whole = full.renderer.streamer().stats().page_bytes;
  REQUIRE(whole > 0);
  REQUIRE(full.scene.page_count() > 4);

  std::string error;
  Vector<u32> reference_covered;
  Vector<Vector<u8>> reference_cover;
  reference_cover.resize(k_steps);
  for (u32 s = 0; s < k_steps; ++s) {
    FrameDesc frame;
    frame.camera = fly_camera(full.data.center, full.data.radius, k_from, k_to, s, k_steps);
    frame.frame_index = s;
    // Every page resident before the shot is taken, so the reference is the fully resident
    // picture of that step and not a picture of how far *it* had got.
    for (u32 warm = 0; warm < 8; ++warm)
      REQUIRE_MESSAGE(full.renderer.render_offscreen(frame, &error), error);
    CapturedFrame shot;
    REQUIRE_MESSAGE(full.renderer.capture(frame, {.ids = true}, shot, &error), error);
    coverage_of(shot, reference_cover[s]);
    reference_covered.push_back(shot.covered);
  }

  // A quarter of the page bytes, and a small upload budget so a step cannot converge inside
  // itself: this is the starved case walking the path, which is what the guarantee is about.
  Harness lean;
  if (!lean.build(heightfield_desc(true, 16 * 1024), streamed_settings(whole / 4, 32 * 1024))) {
    MESSAGE(lean.skip);
    return;
  }
  u32 worst_holes = 0;
  u32 worst_step = 0;
  for (u32 s = 0; s < k_steps; ++s) {
    FrameDesc frame;
    frame.camera = fly_camera(lean.data.center, lean.data.radius, k_from, k_to, s, k_steps);
    frame.frame_index = s;
    REQUIRE_MESSAGE(lean.renderer.render_offscreen(frame, &error), error);
    CapturedFrame shot;
    REQUIRE_MESSAGE(lean.renderer.capture(frame, {.ids = true}, shot, &error), error);
    Vector<u8> cover;
    coverage_of(shot, cover);
    REQUIRE(cover.size() == reference_cover[s].size());
    u32 holes = 0;
    u32 extra = 0;
    for (u32 p = 0; p < cover.size(); ++p) {
      holes += reference_cover[s][p] != 0 && cover[p] == 0 ? 1u : 0u;
      extra += reference_cover[s][p] == 0 && cover[p] != 0 ? 1u : 0u;
    }
    // **The guarantee is about the interior, and that is what is asserted.** A coarser cut moves
    // a silhouette — the heightfield's horizon is a jagged edge and a parent cluster's is a
    // different jagged edge — so the two coverages differ along it by construction, and counting
    // those pixels against an area-scaled tolerance measures the shape of the object rather than
    // the streaming: at 8 radii the terrain covers 2,494 pixels of 76,800 and its edge is a
    // sizeable fraction of that, while at 0.9 radii it covers most of the frame and the same
    // edge is a rounding error. So the reference mask is **eroded by two pixels** and every
    // surviving pixel must be covered, which says exactly "no region of the surface is missing"
    // with no tolerance to calibrate. The whole-frame counts stay in the message, because they
    // are the table docs/subsystems/renderer.md carries.
    const u32 interior_holes = interior_misses(reference_cover[s], cover, k_width, k_height, 2);
    if (holes > worst_holes) {
      worst_holes = holes;
      worst_step = s;
    }
    CHECK_MESSAGE(interior_holes == 0, "step " << s << ": " << interior_holes
                                               << " interior pixels of " << reference_covered[s]
                                               << " covered are missing");
    // And a loose bound on the whole frame, which would catch "half the surface is gone" if the
    // erosion ever stopped being the tight statement it is.
    CHECK_MESSAGE(holes * 20 <= reference_covered[s] + 160,
                  "step " << s << ": " << holes << " holes of " << reference_covered[s]);
    MESSAGE("step " << s << ": " << reference_covered[s] << " covered, " << holes << " holes, "
                    << extra << " extra, " << interior_holes << " interior");
  }
  const StreamStats& stats = lean.renderer.streamer().stats();
  MESSAGE("fly-in " << k_from << " -> " << k_to << " radii in " << k_steps << " steps: worst holes "
                    << worst_holes << " at step " << worst_step << ", " << stats.uploads
                    << " uploads, " << stats.evictions << " evictions, " << stats.pages_resident
                    << " of " << stats.pages_total << " pages resident");
  CHECK(stats.uploads > 0);
}

TEST_CASE("streaming: a page's 8-bit indices are its own, under a budget that evicts every frame") {
  // **The case the recorded device loss was in**, and the reason it went unseen: every other case
  // in this file runs with `ShadowMode::Off`, and the 8-bit index stream the cluster acceleration
  // structure builds read is staged **only when the frame builds them**
  // (`GpuScene::page_stage_layout`'s `ray_tracing`). So this one turns the ray chain on and then
  // asks the pool the flattest question there is: does the slot holding page p hold *page p's*
  // 8-bit indices?
  //
  // The two pool streams are written by the same upload, from the same staged bytes, over the same
  // range of the same slot — `indices8[3t .. 3t + 3)` is `pack_cluster_indices` of `triangles[t]`
  // and nothing else can make them agree. That makes this a byte assertion rather than a picture
  // one, which matters because the failure it guards is not a wrong picture: a page staged with
  // another page's indices is a CLAS build record pointing at triangles that are not there, and
  // what that costs is the device.
  Harness probe;
  RenderSettings first = streamed_settings(0, 0);
  first.shadows = ShadowMode::RayTraced;
  if (!probe.build(heightfield_desc(true, 16 * 1024), first)) {
    MESSAGE(probe.skip);
    return;
  }
  // A device with no cluster acceleration structures or no ray query resolves the shadows away,
  // and then there is no 8-bit index stream to check. Say so and skip rather than pass vacuously.
  if (probe.scene.indices8.buffer == VK_NULL_HANDLE) {
    MESSAGE("the device builds no cluster acceleration structures; nothing stages 8-bit indices");
    return;
  }
  const u64 whole = probe.renderer.streamer().stats().page_bytes;
  REQUIRE(whole > 0);
  probe.renderer.destroy();
  probe.scene.destroy();

  // A tenth of the page bytes and a quarter-pixel threshold, which is the recipe the
  // ancestor-closure case above uses to make the pool actually recycle its slots: the cut wants
  // most of the mesh, the budget holds a fraction of it, and the camera moves so that what the
  // manager evicts keeps changing.
  std::string error;
  RenderSettings settings = streamed_settings(whole / 10, 48 * 1024);
  settings.shadows = ShadowMode::RayTraced;
  settings.lod_px = 0.25f;
  ResolvedSettings resolved;
  resolve_settings(settings, probe.device.features(), &probe.data, resolved);
  REQUIRE(resolved.rt_chain);
  REQUIRE_MESSAGE(probe.scene.create(probe.device, probe.data, resolved, &error), error);
  SceneRenderer::Desc rd;
  rd.width = k_width;
  rd.height = k_height;
  REQUIRE_MESSAGE(probe.renderer.create(probe.device, probe.scene, resolved, rd, &error), error);

  for (u32 f = 0; f < 48; ++f) {
    const f32 distance = 1.0f + 3.0f * static_cast<f32>(f % 8) / 7.0f;
    REQUIRE_MESSAGE(probe.renderer.render_offscreen(frame_at(probe.data, distance, f), &error),
                    error);
  }
  const StreamStats& stats = probe.renderer.streamer().stats();
  MESSAGE("ray chain under a tenth budget: " << stats.uploads << " uploads, " << stats.evictions
                                             << " evictions, " << stats.pages_resident << " of "
                                             << stats.pages_total << " pages resident");
  // Without both of these the check below would hold of a pool that was filled once and never
  // recycled, which is the configuration that never failed.
  REQUIRE(stats.uploads > 1);
  REQUIRE(stats.evictions > 0);

  Vector<u8> triangle_bytes;
  Vector<u8> index_bytes;
  REQUIRE_MESSAGE(read_pool(probe.device, probe.scene.triangles, triangle_bytes, &error), error);
  REQUIRE_MESSAGE(read_pool(probe.device, probe.scene.indices8, index_bytes, &error), error);
  const auto* triangles = reinterpret_cast<const u32*>(triangle_bytes.data());
  const u32 slot_triangles = probe.scene.slot_triangles();
  u32 checked = 0;
  u32 wrong = 0;
  u32 first_wrong_page = ~u32{0};
  for (u32 p = 0; p < probe.scene.page_count(); ++p) {
    const u32 slot = probe.renderer.streamer().slot_of_page(p);
    if (slot == GeometryStreamer::k_no_page_slot) continue;
    const geometry::ClusterPageDesc& desc = probe.data.pages.pages[p];
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const u64 index = u64{slot} * slot_triangles + t;
      const u32 packed = triangles[index];
      const u8* bytes = index_bytes.data() + index * 3;
      const bool ok = bytes[0] == static_cast<u8>(packed & 0xffu) &&
                      bytes[1] == static_cast<u8>((packed >> 8) & 0xffu) &&
                      bytes[2] == static_cast<u8>((packed >> 16) & 0xffu);
      ++checked;
      if (!ok && first_wrong_page == ~u32{0}) first_wrong_page = p;
      wrong += ok ? 0u : 1u;
    }
  }
  MESSAGE("pool triangles checked "
          << checked << ", 8-bit indices disagreeing " << wrong
          << (first_wrong_page == ~u32{0} ? std::string()
                                          : ", first on page " + std::to_string(first_wrong_page)));
  CHECK(checked > 0);
  CHECK_MESSAGE(wrong == 0, wrong << " of " << checked
                                  << " triangles in the page pool carry 8-bit indices that are not "
                                     "their own");
}

namespace {

// What a run against a slow source did: how many frames it drew before nothing was outstanding
// (or the cap), and the longest stretch in which something was outstanding and the pool did not
// gain a page — the number a stalled streamer grows without bound.
struct SlowRun {
  u32 frames = 0;
  bool converged = false;
  u32 longest_stall = 0;
  u32 pool_pages = 0;
  u32 manager_pages = 0;
  u64 uploads = 0;
  u64 steals = 0;
  u32 pending = 0;
  u32 loads_in_flight = 0;
};

SlowRun fly_slow(Harness& h, SlowPageSource& slow, u32 max_frames) {
  std::string error;
  SlowRun out;
  u32 quiet = 0;
  u32 stall = 0;
  u32 last_pool = 0;
  for (u32 f = 0; f < max_frames; ++f) {
    // A fly-in over the first 48 frames, then held: the cut grows by an order of magnitude and
    // asks for pages in an order the page table does not share, which is what fills the load
    // slots with pages the walk cannot reach yet.
    FrameDesc frame;
    frame.camera = fly_camera(h.data.center, h.data.radius, 6.0f, 0.5f, std::min(f, 47u), 48);
    frame.frame_index = f;
    REQUIRE_MESSAGE(h.renderer.render_offscreen(frame, &error), error);
    slow.tick();
    const StreamStats& s = h.renderer.streamer().stats();
    const bool outstanding = s.pending > 0 || s.loads_in_flight > 0 ||
                             s.pool_pages < s.pages_resident || h.renderer.streamer().has_uploads();
    if (outstanding && s.pool_pages <= last_pool) {
      ++stall;
      out.longest_stall = std::max(out.longest_stall, stall);
    } else {
      stall = 0;
    }
    last_pool = s.pool_pages;
    quiet = !outstanding && f >= 48 ? quiet + 1 : 0;
    out.frames = f + 1;
    if (quiet >= k_quiet_frames) {
      out.converged = true;
      break;
    }
  }
  const StreamStats& s = h.renderer.streamer().stats();
  out.pool_pages = s.pool_pages;
  out.manager_pages = s.pages_resident;
  out.uploads = s.uploads;
  out.steals = s.steals;
  out.pending = s.pending;
  out.loads_in_flight = s.loads_in_flight;
  return out;
}

}  // namespace

TEST_CASE("streaming: admission never waits on a read it cannot start") {
  // **The stuck flythrough of 2026-09-24**, made to happen every time. One streamed run of the
  // desert overlook admitted nothing for 2,401 frames while three reruns were normal. The cause was
  // a circular wait: the pool is filled in page order, so a load that has landed behind a page
  // still waiting for its read cannot be staged, and a load slot is given back only when its page
  // is staged — so when every load slot held a page behind the walk's head, and the head (a page
  // admitted after them, in priority order, with a lower index) had no load of its own, the head
  // could never start its read and nothing behind it could ever land. Reads that take several
  // frames are what fill the load slots behind the head, which a cold file cache gives a real run
  // and a container this test just wrote never does; the slow source gives it here.
  //
  // Whether the slots fill behind the head depends on the order the cut asks for pages in, and
  // that differs by device (a page's request keeps the priority of whichever lane won it): on the
  // RTX 5090 this fly-in does it on every run and the old streamer stalls at 12 of 21 pages, and
  // on the TITAN Xp it did not, so there the case holds convergence only. The message says which.
  constexpr u32 k_latency = 6;
  SlowPageSource slow;
  Harness h;
  if (!h.build(heightfield_desc(true, 16 * 1024), streamed_settings(0, 32 * 1024), nullptr, &slow,
               k_latency)) {
    MESSAGE(h.skip);
    return;
  }
  REQUIRE(h.scene.page_count() > 2 * SlowPageSource::k_loads);
  const SlowRun run = fly_slow(h, slow, 1200);
  MESSAGE(
      "a " << k_latency << "-frame source: " << std::string(run.converged ? "converged" : "stalled")
           << " after " << run.frames << " frames, " << run.uploads << " uploads, pool "
           << run.pool_pages << " of " << run.manager_pages << " admitted pages, " << run.pending
           << " queued, " << run.loads_in_flight << " loads in flight, " << run.steals
           << " loads given up for the walk's head, longest stall " << run.longest_stall
           << " frames"
           << std::string(run.steals == 0 ? " (the slots never filled behind the head here)" : ""));
  CHECK(run.converged);
  CHECK(run.pool_pages == run.manager_pages);
  // A page at the head waits for its own read and for the frames it takes the feedback to come
  // back, and nothing else: the pool gains a page at least every few read latencies.
  CHECK(run.longest_stall <= 4 * k_latency + 8);
}

TEST_CASE("streaming: a read that fails is read again, and the rest of the queue goes on") {
  // A short read drops the page rather than staging it (the pool would hold its descriptors over
  // whatever was in the buffer). Dropping it must not strand it: the walk reaches it again, starts
  // its read again, and when that lands the pages behind it follow.
  constexpr u32 k_latency = 3;
  SlowPageSource slow;
  Harness h;
  if (!h.build(heightfield_desc(true, 16 * 1024), streamed_settings(0, 32 * 1024), nullptr, &slow,
               k_latency)) {
    MESSAGE(h.skip);
    return;
  }
  // The first page past the roots: everything the fly-in refines into is behind it.
  u32 first_child = 0;
  while (first_child < h.data.pages.pages.size() &&
         (h.data.pages.pages[first_child].flags & geometry::k_page_root) != 0) {
    ++first_child;
  }
  REQUIRE(first_child < h.data.pages.pages.size());
  slow.fail_page = first_child;
  slow.fail_times = 4;
  const SlowRun run = fly_slow(h, slow, 1200);
  MESSAGE("page " << first_child << " failed " << slow.fail_times << " times: "
                  << std::string(run.converged ? "converged" : "stalled") << " after " << run.frames
                  << " frames, " << run.uploads << " uploads, pool " << run.pool_pages << " of "
                  << run.manager_pages << ", longest stall " << run.longest_stall << " frames");
  CHECK(run.converged);
  CHECK(run.pool_pages == run.manager_pages);
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
