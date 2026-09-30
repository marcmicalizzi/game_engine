#include <core/jobs/job_system.h>
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/terrain_rings.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

// Read when the rings are built (docs/subsystems/renderer.md, "The rings in the scene").
tunables::Float ring_slack{"renderer.terrain.ring_slack", 1.15, 1.0, 4.0,
                           "How much room a terrain ring's GPU slots and arenas keep above the "
                           "largest ring the layout rule can make, as a factor"};

i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
i64 ceil_div(i64 a, i64 b) noexcept { return -floor_div(-a, b); }

// The heights a ring's chunk is built from: the renderer's own function at a game time — the
// ground's own field and the ridges and basins it adds — in micrometres, read from the level's
// newest evaluated field where that covers the chunk and evaluated through the ground provider's
// re-evaluation entry where it does not.
struct RingSource {
  const TerrainSampler* sampler = nullptr;
  f64 time_s = 0.0;
  struct Field {
    i64 spacing_mm = 0;
    const f32* heights = nullptr;
    gfx::TerrainField window;
  };
  Field fields[k_max_terrain_levels];
  u32 field_count = 0;

  static void heights(const void* context, i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm,
                      jobs::JobSystem*, Vector<i64>& out_um) {
    const auto& self = *static_cast<const RingSource*>(context);
    out_um.resize(static_cast<usize>(nx) * nz);
    const i32 i0 = static_cast<i32>(floor_div(x0, spacing_mm));
    const i32 j0 = static_cast<i32>(floor_div(z0, spacing_mm));
    for (u32 f = 0; f < self.field_count; ++f) {
      const Field& field = self.fields[f];
      if (field.spacing_mm != spacing_mm || field.heights == nullptr) continue;
      const gfx::TerrainField& w = field.window;
      if (i0 < w.i0 || j0 < w.j0 || i0 + static_cast<i32>(nx) > w.i0 + static_cast<i32>(w.nx) ||
          j0 + static_cast<i32>(nz) > w.j0 + static_cast<i32>(w.nz)) {
        continue;
      }
      for (u32 j = 0; j < nz; ++j) {
        const f32* row = field.heights + static_cast<usize>(j0 - w.j0 + j) * w.nx + (i0 - w.i0);
        for (u32 i = 0; i < nx; ++i)
          out_um[j * nx + i] = std::llround(static_cast<f64>(row[i]) * 1.0e6);
      }
      return;
    }
    Vector<f32> h(static_cast<usize>(nx) * nz);
    const TerrainLattice lattice = terrain_ring_lattice(spacing_mm);
    evaluate_terrain_window(*self.sampler, self.time_s, lattice, i0, j0, nx, nz, 0,
                            terrain_window_blocks(nx, nz), std::span<f32>(h.data(), h.size()));
    for (u32 k = 0; k < h.size(); ++k)
      out_um[k] = std::llround(static_cast<f64>(h[k]) * 1.0e6);
  }
};

// A chunk's rest heights, from its own DAG: every copy of a grid vertex on every level is at a
// lattice point of the ring, at the height the chunk was built from.
void rest_of(TerrainChunk& chunk, const TerrainLattice& lattice) {
  const geometry::ClusterMesh& mesh = chunk.lod.mesh;
  const f64 inv = 1.0 / lattice.spacing;
  i32 lo_i = std::numeric_limits<i32>::max();
  i32 lo_j = std::numeric_limits<i32>::max();
  i32 hi_i = std::numeric_limits<i32>::min();
  i32 hi_j = std::numeric_limits<i32>::min();
  const auto grid = [&](u32 v) {
    return v >= mesh.vertex_source.size() || mesh.vertex_source[v] < chunk.grid_vertices;
  };
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    if (!grid(v)) continue;
    const i32 i = static_cast<i32>(std::llround(static_cast<f64>(mesh.vertices[v].x) * inv));
    const i32 j = static_cast<i32>(std::llround(static_cast<f64>(mesh.vertices[v].z) * inv));
    lo_i = std::min(lo_i, i);
    lo_j = std::min(lo_j, j);
    hi_i = std::max(hi_i, i);
    hi_j = std::max(hi_j, j);
  }
  chunk.rest_window = gfx::TerrainField{};
  chunk.rest.clear();
  if (lo_i > hi_i) return;
  chunk.rest_window.i0 = lo_i;
  chunk.rest_window.j0 = lo_j;
  chunk.rest_window.nx = static_cast<u32>(hi_i - lo_i + 1);
  chunk.rest_window.nz = static_cast<u32>(hi_j - lo_j + 1);
  chunk.rest.assign(static_cast<usize>(chunk.rest_window.nx) * chunk.rest_window.nz,
                    std::numeric_limits<f32>::quiet_NaN());
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    if (!grid(v)) continue;
    const i32 i = static_cast<i32>(std::llround(static_cast<f64>(mesh.vertices[v].x) * inv));
    const i32 j = static_cast<i32>(std::llround(static_cast<f64>(mesh.vertices[v].z) * inv));
    chunk.rest[static_cast<usize>(j - lo_j) * chunk.rest_window.nx + (i - lo_i)] =
        mesh.vertices[v].y;
  }
}

}  // namespace

// The ground's own sampler, the rings its provider made (`scene_gen::GroundRings`: the layout
// rule, the re-centre rule and the chunks' DAGs), and where their heights come from.
struct TerrainRingSet::State {
  std::unique_ptr<TerrainSampler> sampler;
  RingSource source;
  scene_gen::GroundRings rings;
  geometry::ClusterLodOptions options;
};

TerrainLevelSet::~TerrainLevelSet() = default;

TerrainRingSet::TerrainRingSet() = default;
TerrainRingSet::~TerrainRingSet() = default;

f64 TerrainLevelSet::padding_of(std::span<const TerrainChunk> chunks, std::span<const f32> field,
                                const gfx::TerrainField& window) {
  f64 most = 0.0;
  for (const TerrainChunk& chunk : chunks) {
    const gfx::TerrainField& r = chunk.rest_window;
    const i32 i0 = std::max(r.i0, window.i0);
    const i32 j0 = std::max(r.j0, window.j0);
    const i32 i1 = std::min(r.i0 + static_cast<i32>(r.nx), window.i0 + static_cast<i32>(window.nx));
    const i32 j1 = std::min(r.j0 + static_cast<i32>(r.nz), window.j0 + static_cast<i32>(window.nz));
    for (i32 j = j0; j < j1; ++j) {
      const f32* rest = chunk.rest.data() + static_cast<usize>(j - r.j0) * r.nx;
      const f32* f = field.data() + static_cast<usize>(j - window.j0) * window.nx;
      for (i32 i = i0; i < i1; ++i) {
        const f32 a = rest[i - r.i0];
        if (std::isnan(a)) continue;
        const f64 d = std::abs(static_cast<f64>(f[i - window.i0]) - static_cast<f64>(a));
        most = d > most ? d : most;
      }
    }
  }
  return most;
}

f64 TerrainRingSet::padding(u32 level, std::span<const f32> field,
                            const gfx::TerrainField& window) const {
  if (level == 0 || level >= levels_) return 0.0;
  std::lock_guard<std::mutex> lock(mutex_);
  const f64 now = padding_of(
      std::span<const TerrainChunk>(chunks_[level].data(), chunks_[level].size()), field, window);
  const f64 before =
      padding_of(std::span<const TerrainChunk>(previous_[level].data(), previous_[level].size()),
                 field, window);
  return std::max(now, before);
}

bool TerrainRingSet::build(const TerrainDesc& desc, f32 camera_x, f32 camera_z,
                           jobs::JobSystem* jobs, std::string* error) {
  levels_ = 0;
  for (Vector<TerrainChunk>& list : chunks_)
    list.clear();
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (!desc.enabled) return fail("terrain rings: the scene has no terrain");
  desc_ = &desc;
  const i64 started = time::monotonic_ns();
  state_ = std::make_unique<State>();
  State& s = *state_;
  s.sampler = std::make_unique<TerrainSampler>(desc);
  // Whether a ground has rings is its provider's to say (the dunes do, the waves do not), and a
  // provider this executable does not carry has none here.
  if (!s.sampler->ok() || !s.sampler->provider().has_rings()) {
    return fail("terrain rings: the scene's terrain has none: its ground provider (\"" +
                std::string(terrain_provider(desc)) + "\") makes no rings in this build");
  }
  // The rings are the world's grids at their spacings, counted from the world's origin in
  // millimetres, and the scene's grid is their outer ring: its corner and spacing must be whole
  // millimetres for its lattice to be one of theirs.
  const f64 spacing_mm = 2000.0 * static_cast<f64>(desc.extent) / static_cast<f64>(desc.size - 1);
  const i64 spacing = std::llround(spacing_mm);
  const i64 extent = std::llround(static_cast<f64>(desc.extent) * 1000.0);
  if (desc.size < 2 || std::abs(spacing_mm - static_cast<f64>(spacing)) > 1.0e-6 || spacing <= 0 ||
      std::abs(static_cast<f64>(desc.extent) * 1000.0 - static_cast<f64>(extent)) > 1.0e-3) {
    return fail(
        "terrain rings: the scene grid's spacing and half-side must be whole millimetres, so its "
        "lattice is the rings' outer grid");
  }
  std::string why;
  if (!s.sampler->provider().make_rings(extent, spacing, s.rings, &why)) return fail(why);
  levels_ = s.rings.count();
  lattice_[0] = terrain_scene_lattice(desc);
  skirt_m_[0] = 0.0f;
  for (u32 level = 1; level < levels_; ++level) {
    const scene_gen::RingSpec spec = s.rings.spec(ring_of_level(level));
    lattice_[level] = terrain_ring_lattice(spec.spacing_mm);
    skirt_m_[level] = static_cast<f32>(static_cast<f64>(spec.skirt_mm) / 1000.0);
    half_mm_[level] = spec.half_mm;
  }
  s.options = geometry::ClusterLodOptions{};
  s.source.sampler = s.sampler.get();
  s.source.time_s = desc.time_s;
  s.source.field_count = 0;
  const u32 moving_mask = (1u << (levels_ - 1)) - 1u;
  if (!s.rings.build(std::llround(static_cast<f64>(camera_x) * 1000.0),
                     std::llround(static_cast<f64>(camera_z) * 1000.0),
                     scene_gen::RingHeights{&RingSource::heights, &s.source}, s.options, jobs,
                     moving_mask, error)) {
    levels_ = 0;
    return false;
  }
  u32 all = 0;
  for (u32 level = 1; level < levels_; ++level)
    all |= 1u << level;
  take_chunks(all);
  for (u32 level = 1; level < levels_; ++level)
    previous_[level].clear();
  // What the GPU scene reserves: room for two of the largest ring this layout rule can make.
  const f64 slack = ring_slack.get();
  for (u32 level = 1; level < levels_; ++level) {
    const scene_gen::RingSpec spec = s.rings.spec(ring_of_level(level));
    const i64 chunk_mm = spec.chunk_mm;
    const u64 side = static_cast<u64>(ceil_div(2 * spec.half_mm, chunk_mm) + 1);
    const u64 most_chunks = side * side;
    u32 clusters = 0;
    u64 vertices = 0;
    u64 triangles = 0;
    u64 most_vertices = 0;
    u64 most_triangles = 0;
    u32 built = 0;
    for (const TerrainChunk& chunk : chunks_[level]) {
      if (chunk.lod.mesh.clusters.empty()) continue;
      clusters = std::max<u32>(clusters, chunk.lod.mesh.clusters.size());
      vertices += chunk.lod.mesh.vertices.size();
      triangles += chunk.lod.mesh.triangles.size();
      most_vertices = std::max<u64>(most_vertices, chunk.lod.mesh.vertices.size());
      most_triangles = std::max<u64>(most_triangles, chunk.lod.mesh.triangles.size());
      ++built;
    }
    Capacity& c = capacity_[level];
    c.slots = static_cast<u32>(2 * most_chunks);
    c.clusters_per_slot =
        static_cast<u32>((static_cast<f64>(clusters) * slack + 63.0) / 64.0) * 64u;
    // The arenas hold two rings: the one drawn and the one replacing it. A ring the layout rule
    // puts elsewhere may be cut into up to `most_chunks` chunks where this one has `built`, and
    // cut differently: a ring over four chunks is one chunk's worth of ground in each corner, over
    // one it is all in one, and the locked borders between chunks keep triangles the simplifier
    // would otherwise take. So a ring is sized as the larger of this one scaled to the most chunks
    // and the most chunks each as large as the largest built here, then doubled, and the slack
    // covers a chunk over rougher sand simplifying less than the ones measured here did.
    const f64 scale = built > 0 ? static_cast<f64>(most_chunks) / static_cast<f64>(built) : 1.0;
    const f64 ring_vertices =
        std::max(static_cast<f64>(vertices) * scale, static_cast<f64>(most_vertices * most_chunks));
    const f64 ring_triangles = std::max(static_cast<f64>(triangles) * scale,
                                        static_cast<f64>(most_triangles * most_chunks));
    c.vertices = static_cast<u64>(ring_vertices * 2.0 * slack) + 1024;
    c.triangles = static_cast<u64>(ring_triangles * 2.0 * slack) + 1024;
    ENGINE_LOG_INFO(log_renderer, "terrain ring", log::field("level", level),
                    log::field("spacing_m", lattice_[level].spacing),
                    log::field("half_m", static_cast<f64>(spec.half_mm) / 1000.0),
                    log::field("chunks", built), log::field("slots", c.slots),
                    log::field("clusters_per_slot", c.clusters_per_slot),
                    log::field("vertices", c.vertices), log::field("triangles", c.triangles));
  }
  last_build_ms_ = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  last_built_ = s.rings.last_built();
  last_kept_ = s.rings.last_reused();
  return true;
}

void TerrainRingSet::take_chunks(u32 level_mask) {
  State& s = *state_;
  for (u32 level = 1; level < levels_; ++level) {
    if ((level_mask & (1u << level)) == 0) continue;
    const u32 ring = ring_of_level(level);
    const u32 count = s.rings.chunk_count(ring);
    // Which of the provider's chunks this set holds already (the same cells, hole and border: the
    // same key), and the new ones with their rest heights, all before anything a reader of
    // `padding` sees changes: only this thread writes the lists, so reading them here is safe.
    Vector<u32> kept_from(count, ~0u);
    Vector<TerrainChunk> fresh;
    const Vector<TerrainChunk>& old = chunks_[level];
    for (u32 c = 0; c < count; ++c) {
      const scene_gen::RingChunkRef rc = s.rings.chunk(ring, c);
      for (u32 o = 0; o < old.size(); ++o) {
        if (old[o].i == rc.i && old[o].j == rc.j && old[o].key == rc.key) {
          kept_from[c] = o;
          break;
        }
      }
      if (kept_from[c] != ~0u) continue;
      TerrainChunk chunk;
      chunk.i = rc.i;
      chunk.j = rc.j;
      chunk.key = rc.key;
      chunk.grid_vertices = rc.grid_vertices;
      chunk.lod = std::move(*rc.lod);
      *rc.lod = geometry::ClusterLodMesh{};
      chunk.rest_time_s = state_->source.time_s;
      rest_of(chunk, lattice_[level]);
      fresh.push_back(std::move(chunk));
    }
    // Then the lists, in the provider's chunk order, under the lock `padding` takes: the kept
    // chunks keep their slots and rests, and what they replace is kept as `previous_`, which is
    // still drawn until the frame that swaps the new chunks in.
    std::lock_guard<std::mutex> lock(mutex_);
    Vector<TerrainChunk> next;
    next.reserve(count);
    Vector<u8> taken(old.size(), u8{0});
    u32 f = 0;
    for (u32 c = 0; c < count; ++c) {
      if (kept_from[c] != ~0u) {
        taken[kept_from[c]] = 1;
        next.push_back(std::move(chunks_[level][kept_from[c]]));
      } else {
        next.push_back(std::move(fresh[f++]));
      }
    }
    Vector<TerrainChunk> replaced;
    for (u32 o = 0; o < taken.size(); ++o) {
      if (taken[o] == 0) replaced.push_back(std::move(chunks_[level][o]));
    }
    chunks_[level] = std::move(next);
    previous_[level] = std::move(replaced);
  }
}

bool TerrainRingSet::update(f32 camera_x, f32 camera_z, f64 time_s, std::span<const Heights> fields,
                            jobs::JobSystem* jobs, u32& moved, std::string* error) {
  moved = 0;
  if (!valid()) return true;
  State& s = *state_;
  const i64 started = time::monotonic_ns();
  s.source.time_s = time_s;
  s.source.field_count = 0;
  for (u32 level = 1; level < levels_ && level < fields.size(); ++level) {
    if (fields[level].heights == nullptr) continue;
    RingSource::Field& f = s.source.fields[s.source.field_count++];
    f.spacing_mm = lattice_[level].spacing_mm;
    f.heights = fields[level].heights;
    f.window = fields[level].window;
  }
  u32 rings = 0;
  const bool ok =
      s.rings.update(std::llround(static_cast<f64>(camera_x) * 1000.0),
                     std::llround(static_cast<f64>(camera_z) * 1000.0), jobs, rings, error);
  s.source.field_count = 0;
  if (!ok) return false;
  for (u32 r = 0; r + 1 < levels_; ++r) {
    if ((rings & (1u << r)) != 0) moved |= 1u << (levels_ - 1 - r);
  }
  if (moved == 0) return true;
  take_chunks(moved);
  last_build_ms_ = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  last_built_ = s.rings.last_built();
  last_kept_ = s.rings.last_reused();
  return true;
}

TerrainRingLayout TerrainRingSet::layout() const noexcept {
  TerrainRingLayout out;
  if (!valid()) return out;
  scene_gen::RingsLayout l;
  state_->rings.layout(l);
  for (u32 level = 1; level < levels_; ++level) {
    const scene_gen::RingPlace& r = l.ring[ring_of_level(level)];
    out.cx[level] = r.cx;
    out.cz[level] = r.cz;
    out.half[level] = r.half;
  }
  return out;
}

Vec4 TerrainRingSet::square(u32 level, const TerrainRingLayout& layout) const noexcept {
  if (level == 0 || level >= levels_) return Vec4{};
  const f64 x0 = static_cast<f64>(layout.cx[level] - layout.half[level]) / 1000.0;
  const f64 z0 = static_cast<f64>(layout.cz[level] - layout.half[level]) / 1000.0;
  const f64 x1 = static_cast<f64>(layout.cx[level] + layout.half[level]) / 1000.0;
  const f64 z1 = static_cast<f64>(layout.cz[level] + layout.half[level]) / 1000.0;
  return Vec4{static_cast<f32>(x0), static_cast<f32>(z0), static_cast<f32>(x1),
              static_cast<f32>(z1)};
}

gfx::TerrainField TerrainRingSet::field_window(u32 level,
                                               const TerrainRingLayout& layout) const noexcept {
  gfx::TerrainField w{};
  if (level == 0) {
    w.nx = lattice_[0].size;
    w.nz = lattice_[0].size;
    return w;
  }
  const i64 s = lattice_[level].spacing_mm;
  const i64 reach = layout.half[level];
  const i64 i0 = floor_div(layout.cx[level] - reach, s) - 1;
  const i64 i1 = ceil_div(layout.cx[level] + reach, s) + 1;
  const i64 j0 = floor_div(layout.cz[level] - reach, s) - 1;
  const i64 j1 = ceil_div(layout.cz[level] + reach, s) + 1;
  w.i0 = static_cast<i32>(i0);
  w.j0 = static_cast<i32>(j0);
  w.nx = static_cast<u32>(i1 - i0 + 1);
  w.nz = static_cast<u32>(j1 - j0 + 1);
  return w;
}

bool TerrainRingSet::covers(u32 level, const TerrainRingLayout& layout,
                            const gfx::TerrainField& window) const noexcept {
  if (level == 0) return true;
  const i64 s = lattice_[level].spacing_mm;
  const i64 i0 = floor_div(layout.cx[level] - layout.half[level], s) - 1;
  const i64 i1 = ceil_div(layout.cx[level] + layout.half[level], s) + 1;
  const i64 j0 = floor_div(layout.cz[level] - layout.half[level], s) - 1;
  const i64 j1 = ceil_div(layout.cz[level] + layout.half[level], s) + 1;
  return i0 >= window.i0 && j0 >= window.j0 && i1 < i64{window.i0} + static_cast<i64>(window.nx) &&
         j1 < i64{window.j0} + static_cast<i64>(window.nz);
}

u64 TerrainRingSet::field_capacity(u32 level) const noexcept {
  if (level == 0) return u64{lattice_[0].size} * lattice_[0].size;
  if (level >= levels_) return 0;
  // A window is the square's side over the spacing, rounded out at both ends, and a sample of
  // apron either side: at most side / s + 4 samples along each axis, wherever the centre is.
  const i64 s = lattice_[level].spacing_mm;
  const u64 side = static_cast<u64>(ceil_div(2 * half_mm_[level], s)) + 4;
  return side * side;
}

// The provider's own re-centre rule on a copy of the layout
// (`scene_gen::GroundRings::next_layout`): a ring clamped at the scene's edge that the camera has
// left behind does not move, and asks for nothing. It reads the ring parameters and nothing
// `update` changes, so the frame may ask while a re-centre is being built.
TerrainRingLayout TerrainRingSet::next_layout(f32 camera_x, f32 camera_z,
                                              const TerrainRingLayout& from) const noexcept {
  if (!valid()) return from;
  scene_gen::RingsLayout rings;
  rings.count = levels_;
  for (u32 level = 1; level < levels_; ++level) {
    scene_gen::RingPlace& r = rings.ring[ring_of_level(level)];
    r.cx = from.cx[level];
    r.cz = from.cz[level];
    r.half = from.half[level];
  }
  scene_gen::RingsLayout moved;
  state_->rings.next_layout(std::llround(static_cast<f64>(camera_x) * 1000.0),
                            std::llround(static_cast<f64>(camera_z) * 1000.0), rings, moved);
  TerrainRingLayout out;
  for (u32 level = 1; level < levels_; ++level) {
    const scene_gen::RingPlace& r = moved.ring[ring_of_level(level)];
    out.cx[level] = r.cx;
    out.cz[level] = r.cz;
    out.half[level] = r.half;
  }
  return out;
}

bool TerrainRingSet::wants_update(f32 camera_x, f32 camera_z,
                                  const TerrainRingLayout& layout) const noexcept {
  return valid() && !(next_layout(camera_x, camera_z, layout) == layout);
}

}  // namespace engine::renderer
