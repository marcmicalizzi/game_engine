#include <systems/renderer/page_source.h>

#include <algorithm>
#include <renderer_log.h>

namespace engine::renderer {

bool FilePageSource::create(const SceneData& data, const GpuScene& scene, jobs::JobSystem& jobs,
                            std::string* error) {
  destroy();
  auto refuse = [&](std::string why) {
    destroy();
    if (error != nullptr) *error = std::move(why);
    return false;
  };
  if (!scene.streamed()) return refuse("the scene is not streamed");
  if (data.sources.size() != data.parts.size())
    return refuse("the scene has no source record per mesh");

  meshes_.resize(data.sources.size());
  for (u32 m = 0; m < data.sources.size(); ++m) {
    const SourceMesh& source = data.sources[m];
    const geometry::ClusterMeshPart& part = data.parts[m];
    if (source.container.empty()) {
      return refuse("mesh " + std::to_string(m) +
                    " has no .clusters container to read its pages from");
    }
    if (part.page_count == 0)
      return refuse("mesh " + std::to_string(m) + " has no pages in the scene's table");
    MeshFile& file = meshes_[m];
    std::string why;
    if (!file.reader.open(source.container, nullptr, &why))
      return refuse("mesh " + std::to_string(m) + ": " + why);
    file.first_page = part.first_page;
    file.page_count = part.page_count;
    // A mesh's first page is its file's page 0, whose three `first_*` are zero in the file, so the
    // merged values of that page *are* the bases the merge added.
    const geometry::ClusterPageDesc& first = data.pages.pages[part.first_page];
    file.cluster_base = first.first_cluster;
    file.vertex_base = first.first_vertex;
    file.triangle_base = first.first_triangle;
    // What the container has to hold for a page's ranges to exist at all. Checked once, here,
    // rather than per page: a container that is the wrong answer for this mesh is a load-time
    // failure and not a frame that quietly draws nothing.
    u64 offset = 0;
    u64 bytes = 0;
    const u32 pages_in_file =
        static_cast<u32>(file.reader.element_count(geometry::ClusterSection::Pages));
    if (pages_in_file != part.page_count) {
      return refuse("mesh " + std::to_string(m) + ": the container holds " +
                    std::to_string(pages_in_file) + " pages and the scene's table has " +
                    std::to_string(part.page_count));
    }
    if (!file.reader.range(geometry::ClusterSection::Quantized, sizeof(u16), 0, 0, offset, bytes) ||
        !file.reader.range(geometry::ClusterSection::Attributes, sizeof(geometry::VertexAttributes),
                           0, 0, offset, bytes) ||
        !file.reader.range(geometry::ClusterSection::Triangles, sizeof(u32), 0, 0, offset, bytes)) {
      return refuse("mesh " + std::to_string(m) +
                    ": the container is missing a stream a page is made of");
    }
  }

  data_ = &data;
  scene_ = &scene;
  jobs_ = &jobs;
  // One buffer per load, each sized for the largest page, so a frame allocates nothing
  // (AGENTS.md, "no allocations in the frame loop in steady state").
  for (u32 p = 0; p < scene.page_count(); ++p)
    buffer_stride_ = std::max(buffer_stride_, scene.page_payload_bytes(p));
  buffers_.assign(static_cast<u32>(buffer_stride_ * k_page_loads), u8{0});
  requests_.resize(k_page_loads * k_page_reads);
  ENGINE_LOG_INFO(log_renderer, "geometry pages stream from their containers",
                  log::field("meshes", meshes_.size()), log::field("pages", scene.page_count()),
                  log::field("loads", k_page_loads),
                  log::field("load_bytes", buffer_stride_ * k_page_loads));
  return true;
}

void FilePageSource::destroy() noexcept {
  for (u32 i = 0; i < k_page_loads; ++i) {
    if (loads_[i].busy && jobs_ != nullptr) jobs_->wait(loads_[i].counter);
    loads_[i].busy = false;
    loads_[i].page = ~u32{0};
    loads_[i].reads = 0;
  }
  meshes_.clear();
  requests_.clear();
  buffers_.clear();
  buffer_stride_ = 0;
  in_flight_ = 0;
  bytes_read_ = 0;
  reads_ = 0;
  released_bytes_ = 0;
  data_ = nullptr;
  scene_ = nullptr;
  jobs_ = nullptr;
}

const FilePageSource::MeshFile* FilePageSource::mesh_of_page(u32 page) const noexcept {
  for (const MeshFile& file : meshes_) {
    if (page >= file.first_page && page - file.first_page < file.page_count) return &file;
  }
  return nullptr;
}

bool FilePageSource::begin(u32 page, u32& handle) {
  handle = k_no_load;
  if (!valid() || page >= scene_->page_count()) return false;
  const MeshFile* file = mesh_of_page(page);
  if (file == nullptr) return false;
  u32 slot = k_page_loads;
  for (u32 i = 0; i < k_page_loads; ++i) {
    if (!loads_[i].busy) {
      slot = i;
      break;
    }
  }
  if (slot == k_page_loads) return false;

  const geometry::ClusterPageDesc& desc = data_->pages.pages[page];
  const GpuScene::PageStage stage = scene_->page_stage_layout(page);
  u8* dst = buffers_.data() + buffer_stride_ * slot;
  io::AsyncRangeRead* requests = requests_.data() + u64{slot} * k_page_reads;
  const u32 first_vertex = desc.first_vertex - file->vertex_base;
  const u32 first_triangle = desc.first_triangle - file->triangle_base;

  // The page's ranges, in the order the staging layout wants them. Each is one contiguous run,
  // because `build_cluster_pages` reordered the streams so that a page's slice of each is.
  struct Range {
    geometry::ClusterSection kind;
    u32 element_size;
    u64 first;
    u64 count;
    u64 at;
  };
  const Range ranges[k_page_reads] = {
      {geometry::ClusterSection::Quantized, sizeof(u16), u64{first_vertex} * 3,
       u64{desc.vertex_count} * 3, stage.quantized},
      {geometry::ClusterSection::Attributes, sizeof(geometry::VertexAttributes), first_vertex,
       desc.vertex_count, stage.attributes},
      {geometry::ClusterSection::Triangles, sizeof(u32), first_triangle, desc.triangle_count,
       stage.triangles},
      {geometry::ClusterSection::Vertices, sizeof(Vec3), first_vertex, desc.vertex_count,
       stage.vertices},
  };
  const u32 count = stage.ray_tracing ? k_page_reads : k_page_reads - 1;
  u32 issued = 0;
  for (u32 r = 0; r < count; ++r) {
    u64 offset = 0;
    u64 bytes = 0;
    if (!file->reader.range(ranges[r].kind, ranges[r].element_size, ranges[r].first,
                            ranges[r].count, offset, bytes)) {
      // Nothing has been scheduled yet, so there is nothing to unwind: the load slot is simply
      // not taken and the page falls back to being requested again next frame.
      return false;
    }
    if (bytes == 0) continue;  // an empty run is a read to skip, not a read of nothing
    io::AsyncRangeRead& request = requests[issued++];
    request.file = &file->reader.file();
    request.offset = offset;
    request.bytes = bytes;
    request.dst = dst + ranges[r].at;
    request.read = 0;
    request.status = io::Status::Ok;
    bytes_read_ += bytes;
    ++reads_;
  }
  Load& load = loads_[slot];
  load.page = page;
  load.reads = issued;
  load.busy = true;
  ++in_flight_;
  if (issued > 0)
    io::read_ranges_async(*jobs_, std::span<io::AsyncRangeRead>(requests, issued), load.counter);
  handle = slot;
  return true;
}

bool FilePageSource::done(u32 handle) const noexcept {
  if (handle >= k_page_loads || !loads_[handle].busy) return false;
  return loads_[handle].counter.done();
}

bool FilePageSource::complete(u32 handle) const noexcept {
  if (!done(handle)) return false;
  const io::AsyncRangeRead* requests = requests_.data() + u64{handle} * k_page_reads;
  for (u32 r = 0; r < loads_[handle].reads; ++r) {
    if (!requests[r].complete()) return false;
  }
  return true;
}

const u8* FilePageSource::bytes(u32 handle) const noexcept {
  if (handle >= k_page_loads) return nullptr;
  return buffers_.data() + buffer_stride_ * handle;
}

u32 FilePageSource::page_of(u32 handle) const noexcept {
  return handle < k_page_loads ? loads_[handle].page : ~u32{0};
}

void FilePageSource::release(u32 handle) {
  if (handle >= k_page_loads || !loads_[handle].busy) return;
  if (!loads_[handle].counter.done()) jobs_->wait(loads_[handle].counter);
  loads_[handle].busy = false;
  loads_[handle].page = ~u32{0};
  loads_[handle].reads = 0;
  --in_flight_;
}

u64 paged_stream_bytes(const SceneData& data) noexcept {
  const geometry::ClusterMesh& mesh = data.lod.mesh;
  return u64{mesh.quantized.size()} * sizeof(u16) +
         u64{mesh.attributes.size()} * sizeof(geometry::VertexAttributes) +
         u64{mesh.triangles.size()} * sizeof(u32) + u64{mesh.vertices.size()} * sizeof(Vec3) +
         u64{mesh.vertex_source.size()} * sizeof(u32) +
         u64{mesh.skin.size()} * sizeof(geometry::SkinBinding);
}

bool attach_page_source(SceneData& data, const GpuScene& scene, jobs::JobSystem& jobs,
                        FilePageSource& out, std::string* error) {
  if (!out.create(data, scene, jobs, error)) return false;
  const u64 freed = paged_stream_bytes(data);
  geometry::ClusterMesh& mesh = data.lod.mesh;
  // `clear()` keeps the capacity, and a capacity nobody will ever fill again is the memory this
  // change exists to give back, so each one is shrunk to nothing.
  mesh.quantized.clear();
  mesh.quantized.shrink_to_fit();
  mesh.attributes.clear();
  mesh.attributes.shrink_to_fit();
  mesh.triangles.clear();
  mesh.triangles.shrink_to_fit();
  mesh.vertices.clear();
  mesh.vertices.shrink_to_fit();
  mesh.vertex_source.clear();
  mesh.vertex_source.shrink_to_fit();
  mesh.skin.clear();
  mesh.skin.shrink_to_fit();
  // `mesh.vertex_ids` is kept, deliberately: the GPU never had it, so streaming serves nothing it
  // could stand in for, and `mesh_vertex_ids` answers the same way however the scene was loaded.
  out.set_released_bytes(freed);
  ENGINE_LOG_INFO(log_renderer, "paged host streams released", log::field("bytes", freed),
                  log::field("clusters", mesh.clusters.size()));
  return true;
}

}  // namespace engine::renderer
