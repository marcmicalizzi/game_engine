#pragma once

// Where a streamed page's bytes come from (docs/plan/04-renderer.md §4.9,
// docs/subsystems/renderer.md "Geometry streaming"). There are two answers and
// `GeometryStreamer::copy_page` is the seam between them.
//
// **In memory** is what the first streaming change had: a page's payload is copied out of the
// `SceneData` the load produced. It costs nothing to implement, it is what the procedural
// heightfield and a glTF loaded with `--no-cache` have to use because there is no file behind
// them — and it means streaming saves *device* memory and not host memory, because the whole
// scene's geometry is still sitting in the process.
//
// **From the container** is this file. Every mesh of the scene came out of a `.clusters`
// container — one named outright, a derived-data cache hit, or the entry the load just wrote —
// and `geometry::ClusterFileReader` can address that container's sections by byte range
// ([geometry](../../../../docs/subsystems/geometry.md), "The container read by range"). A page's
// payload is then three or four contiguous ranged reads on the job system's Efficiency pool, the
// merged host streams can be released once the scene is on the GPU, and the residency budget
// bounds host memory as well as device memory.
//
// **Nothing here blocks.** `begin` issues a page's reads and returns; `done` polls one counter.
// A load that is still outstanding when its page is wanted is simply not staged this frame, which
// is the same lateness the feedback path already has and the same fallback clause covers it.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>

#include <string>

namespace engine::renderer {

// How many page loads may be outstanding at once. Eight is more than the default upload budget
// can stage in a frame (256 KiB is about two 128 KB pages), so the reads are never the thing that
// holds a convergence up, and it is small enough that the buffers behind them — one largest page
// each — are a megabyte rather than a budget of their own.
inline constexpr u32 k_page_loads = 8;

// The reads one page takes: quantized positions, attributes, packed triangles, and, when the
// frame builds acceleration structures, the float positions. The cluster descriptors are patched
// on the host out of the scene's own always-resident array and the 8-bit indices are packed from
// the triangles that were just read, so neither of those is a read.
inline constexpr u32 k_page_reads = 4;

// What `GeometryStreamer` asks of a source of page payloads that arrive **later**: start a page's
// load into one of a fixed number of load slots, poll it, take its bytes, give the slot back.
// `FilePageSource` is the one the hosts use; a test puts a source here whose loads take as long as
// it says, which is how a stall that needs a cold disk to happen is made to happen on every run
// (docs/subsystems/renderer.md, "Admission never waits on a read it cannot start").
class PageSource {
 public:
  static constexpr u32 k_no_load = ~u32{0};
  virtual ~PageSource() = default;

  virtual bool valid() const noexcept = 0;
  // Starts page `page`'s load. False when every load slot is busy or the page cannot be read;
  // `handle` names the load until `release`.
  virtual bool begin(u32 page, u32& handle) = 0;
  // Whether the load has finished. Polls; never waits.
  virtual bool done(u32 handle) const noexcept = 0;
  // Whether it finished *and* every byte landed. A load that did not must not be staged.
  virtual bool complete(u32 handle) const noexcept = 0;
  // The page, laid out as `GpuScene::PageStage` says; the descriptor block at offset 0 and the
  // 8-bit indices are the streamer's to fill.
  virtual const u8* bytes(u32 handle) const noexcept = 0;
  // Gives the load slot back, waiting first if the load is still outstanding.
  virtual void release(u32 handle) = 0;
  virtual u32 in_flight() const noexcept = 0;
  virtual u32 capacity() const noexcept = 0;
  // What a summary reports: reads started, their bytes, and what the source let the host free.
  virtual u64 bytes_read() const noexcept = 0;
  virtual u64 reads() const noexcept = 0;
  virtual u64 released_bytes() const noexcept = 0;
};

class FilePageSource final : public PageSource {
 public:
  FilePageSource() noexcept = default;
  ~FilePageSource() override { destroy(); }
  ENGINE_NON_COPYABLE(FilePageSource);

  // Opens one reader per mesh of the scene and sizes the load buffers.
  //
  // **All or nothing.** A scene whose meshes are not *all* backed by a container is refused, with
  // the reason, rather than served in part: the caller's next act is to release the merged host
  // streams, and a source that could answer for half the pages would leave the other half with
  // nowhere to read from. The heightfield and `--no-cache` land here, and they keep the in-memory
  // source, which is the honest outcome — there is no file to read.
  bool create(const SceneData& data, const GpuScene& scene, jobs::JobSystem& jobs,
              std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept override { return scene_ != nullptr; }

  // Starts page `page`'s reads into a load buffer. False when every load is busy; `handle` names
  // the load until `release`.
  bool begin(u32 page, u32& handle) override;
  // Whether every read of the load has finished. Polls a counter; never waits.
  bool done(u32 handle) const noexcept override;
  // Whether it finished *and* every range landed in full. A short or failed read is a page that
  // must not be staged: the pool would hold a cluster's descriptors over somebody else's bytes.
  bool complete(u32 handle) const noexcept override;
  // The staged page, laid out as `GpuScene::PageStage` says, ready to be copied into the ring.
  // The cluster-descriptor block at offset 0 and the 8-bit indices are the streamer's to fill.
  const u8* bytes(u32 handle) const noexcept override;
  u32 page_of(u32 handle) const noexcept;
  // Gives the load back. Waits first if it is still outstanding, because the buffer it is reading
  // into is about to be handed to another page — which happens only when a scene is torn down or
  // a load is cancelled, never in a steady frame.
  void release(u32 handle) override;
  u32 in_flight() const noexcept override { return in_flight_; }
  u32 capacity() const noexcept override { return k_page_loads; }
  u64 bytes_read() const noexcept override { return bytes_read_; }
  u64 reads() const noexcept override { return reads_; }
  // What the merged host streams cost, which is what releasing them saves.
  u64 released_bytes() const noexcept override { return released_bytes_; }
  void set_released_bytes(u64 bytes) noexcept { released_bytes_ = bytes; }

 private:
  // One mesh's container, and the bases that turn a merged page's ranges back into the file's own.
  // A page of mesh *m* names merged indices; the file names mesh-local ones, and the difference is
  // constant per mesh — `merge_paged_cluster_meshes` shifts a page's three `first_*` by exactly
  // the bases of the meshes before it, so mesh *m*'s first page carries them.
  struct MeshFile {
    geometry::ClusterFileReader reader;
    u32 first_page = 0;
    u32 page_count = 0;
    u32 cluster_base = 0;
    u32 vertex_base = 0;
    u32 triangle_base = 0;
  };
  struct Load {
    jobs::Counter counter;
    u32 page = ~u32{0};
    u32 reads = 0;
    bool busy = false;
  };

  const MeshFile* mesh_of_page(u32 page) const noexcept;

  const SceneData* data_ = nullptr;
  const GpuScene* scene_ = nullptr;
  jobs::JobSystem* jobs_ = nullptr;
  Vector<MeshFile> meshes_;
  Load loads_[k_page_loads];
  Vector<io::AsyncRangeRead> requests_;  // k_page_loads * k_page_reads, sized once
  Vector<u8> buffers_;                   // k_page_loads * the largest page, sized once
  u64 buffer_stride_ = 0;
  u32 in_flight_ = 0;
  u64 bytes_read_ = 0;
  u64 reads_ = 0;
  u64 released_bytes_ = 0;
};

// Opens the containers behind a streamed scene and **releases the merged host streams** the pages
// were being copied out of — the quantized positions, the attributes, the packed triangles, the
// float positions, the per-vertex source indices — which is the whole point: with them gone the
// residency budget bounds host memory the way it already bounds device memory, and without the
// file source they could not go. Returns false with the reason and leaves `data` untouched, which
// is what a scene with no container behind it (the heightfield, `--no-cache`) gets.
//
// Called between `GpuScene::create`, which is what uploads everything that stays resident, and
// `SceneRenderer::create`, which is handed the source through `Desc::page_source`. It takes the
// `SceneData` by reference because releasing the streams *is* the point of attaching the source,
// and doing the two in one call is what stops a caller doing one without the other.
//
// **What it does not release** is the cluster and LOD descriptor arrays: they are 96 bytes a
// cluster against the ~1.5 KB a cluster a page carries, the streamer patches a page's descriptors
// out of them on the way past, and the cull pass needs them for every pair of every frame anyway.
bool attach_page_source(SceneData& data, const GpuScene& scene, jobs::JobSystem& jobs,
                        FilePageSource& out, std::string* error = nullptr);

// What the paged host streams of `data` cost right now, which is what attaching a source saves.
u64 paged_stream_bytes(const SceneData& data) noexcept;

}  // namespace engine::renderer
