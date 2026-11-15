#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/litter/litter.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>

namespace engine::litter {
namespace {
// **How many jobs a whole read places its tiles on at once** (T125, T128; litter.md, "Placement"):
// 0, the host's pool, by measurement — since the tile set answers a resident point with no lock
// (T128) the pool is the faster at every size tried, where before it was the slower — 1 for one
// thread, or a number to cap it. The bytes are the same at any value.
tunables::Int whole_read_jobs{"litter.whole_read_jobs", 0, 0, 4096,
                              "Jobs a whole litter read places its tiles on at once: 1 one "
                              "thread, 0 the host's pool, more a cap on it"};
struct Entry {
  Layer p;
  Vector<scene_gen::PlacementMesh> meshes;
  u32 cells = 0;
  f32 tile_size = 0;
};
bool fail(std::string* error, std::string_view why) {
  if (error) *error = "litter placement: " + std::string(why);
  return false;
}
bool unit(f32 v) { return std::isfinite(v) && v >= 0 && v <= 1; }
f64 draw(u64 seed, u64 key) {
  return static_cast<f64>(hash_combine(seed, key) >> 11) / 9007199254740992.0;
}
bool open(const JsonValue& params, const scene_gen::Context& c, void** state, std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext read;
  if (!schema::from_json(e->p, params, read) || !read.ok())
    return fail(error, "invalid engine.litter.Layer");
  const auto& p = e->p;
  if (p.cell_m < 1 || p.cell_m > 16 || !std::isfinite(c.tile_size) || c.tile_size < p.cell_m ||
      c.tile_size > 1024 || std::fmod(c.tile_size, static_cast<f32>(p.cell_m)) != 0)
    return fail(error, "cell_m must divide a tile of at most 1024 m");
  e->cells = static_cast<u32>(c.tile_size) / p.cell_m;
  e->tile_size = c.tile_size;
  if (p.max_cells < 1 || p.max_cells > 65536 || e->cells * e->cells > p.max_cells)
    return fail(error, "max_cells cannot hold candidate lattice");
  if (p.min_x > p.max_x || p.min_z > p.max_z) return fail(error, "inverted tile window");
  const u64 w = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 d = static_cast<u64>(static_cast<i64>(p.max_z) - p.min_z + 1);
  if (w > 0xffffffffull / d || w * d > 0xffffffffull / (e->cells * e->cells))
    return fail(error, "tile window exceeds stable ID space");
  if (p.max_total_cells == 0 || w * d * e->cells * e->cells > p.max_total_cells)
    return fail(error, "max_total_cells cannot hold the whole tile window");
  if (!(p.extent_m >= .05f && p.extent_m <= p.cell_m * .35f) ||
      !(p.edge_m >= .1f && p.edge_m <= 4) || !(p.max_slope >= 0 && p.max_slope <= 2) ||
      !unit(p.background) || !unit(p.hollow_gain) || !unit(p.obstacle_gain) || p.substrate < -1 ||
      p.substrate >= static_cast<i32>(scene_gen::k_mask_max_palette))
    return fail(error, "invalid extent, edge, slope, probability or substrate");
  if (p.meshes.empty() || p.meshes.size() > 64)
    return fail(error, "meshes must contain 1..64 paths");
  for (const auto& path : p.meshes) {
    if (path.empty()) return fail(error, "empty mesh path");
    e->meshes.push_back(
        {(std::filesystem::path(c.dir) / path).lexically_normal().generic_string(), "litter", 0});
  }
  if (p.substrate >= 0) {
    // The ground's first ask for its masks may cook the mask level (a "tiles" ground builds it
    // beside its set on a miss, `ensure_mask_level`), under the provider's own startup stage.
    scene_gen::GroundMasks masks;
    if (!c.ground.masks(masks, error)) return false;
    if (!masks.valid()) return fail(error, "substrate requires cooked ground masks");
  }
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
bool occupies(const void* state, scene_gen::TileCoord t) noexcept {
  const auto& p = static_cast<const Entry*>(state)->p;
  return t.x >= p.min_x && t.x <= p.max_x && t.z >= p.min_z && t.z <= p.max_z;
}
u8 representation(const void*, const scene_gen::Context&) noexcept { return 0; }
bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  out = static_cast<Entry*>(state)->meshes;
  return true;
}
// **The earlier footprints a tile reads are the ones anchored inside it** (litter.md, "Placement"):
// the same test on a whole read and a streamed one, in f64 metres as it always was, so a footprint
// on a tile's border falls where it fell.
bool anchored_in(const scene_gen::Footprint& f, f64 corner_x, f64 corner_z, f64 size) noexcept {
  const f64 fx = static_cast<f64>(f.x_mm) * .001, fz = static_cast<f64>(f.z_mm) * .001;
  return !(fx < corner_x || fx >= corner_x + size || fz < corner_z || fz >= corner_z + size);
}
// The mask tiles a run of litter tiles has read, each once (T125): a cell asks the tile its point
// is in, and a litter tile spans a few, which its neighbours along the row share, so the words are
// kept instead of copied out of the level again for every cell. Held for one `tile` call, or for
// one run of a whole read, which reads one level that does not change under it; at most `k_held` at
// once, so a long run holds a row's worth and not the window's.
struct MaskTiles {
  static constexpr u32 k_held = 64;
  static constexpr u32 k_absent = ~0u;
  struct Held {
    i32 tx = 0, tz = 0;
    u32 at = 0;  // k_absent: the level has no such tile
  };
  Vector<Held> held;
  Vector<u32> words;
  u32 last = ~0u;
  // A band's tiles read ahead by the whole read (`expand`), sorted by (tz, tx), shared read-only by
  // its jobs: what a job finds there it does not ask the level for.
  const MaskTiles* band = nullptr;
  // The tile's words, read on first ask; null when the level refuses it.
  const u32* get(const scene_gen::GroundMasks& masks, u32 n, i32 tx, i32 tz) {
    if (band != nullptr) {
      const auto it = std::lower_bound(
          band->held.begin(), band->held.end(), Held{tx, tz, 0},
          [](const Held& a, const Held& b) { return a.tz != b.tz ? a.tz < b.tz : a.tx < b.tx; });
      if (it != band->held.end() && it->tx == tx && it->tz == tz)
        return it->at == k_absent ? nullptr : band->words.data() + it->at;
    }
    if (last != ~0u && held[last].tx == tx && held[last].tz == tz)
      return words.data() + held[last].at;
    for (u32 i = 0; i < held.size(); ++i)
      if (held[i].tx == tx && held[i].tz == tz) {
        last = i;
        return words.data() + held[i].at;
      }
    if (held.size() >= k_held) {
      held.clear();
      words.clear();
      last = ~0u;
    }
    const u32 at = words.size();
    words.resize(at + n);
    if (!masks.ops->tile(masks.state, tx, tz, std::span<u32>(words.data() + at, n))) {
      words.resize(at);
      return nullptr;
    }
    last = held.size();
    held.push_back(Held{tx, tz, at});
    return words.data() + at;
  }
};
// One tile of the window, its earlier footprints the ones `own` names (indices into `c.earlier`, in
// the order the host handed them): what `tile` and every job of `expand` run.
bool place(const Entry& e, scene_gen::TileCoord t, const scene_gen::Context& c,
           std::span<const u32> own, MaskTiles& held, Vector<scene_gen::Placement>& out,
           std::string* error) {
  const auto& p = e.p;
  scene_gen::GroundMasks masks;
  scene_gen::GroundMaskInfo info;
  u32 words = 0;
  if (p.substrate >= 0) {
    if (!c.ground.masks(masks, error)) return false;
    if (!masks.valid() || !masks.ops->info || !masks.ops->tile ||
        !masks.ops->info(masks.state, info) || info.spacing_mm <= 0 || info.cells < 1 ||
        info.cells > 1024 || (info.cells & (info.cells - 1)) ||
        static_cast<u32>(p.substrate) >= info.palette)
      return fail(error, "invalid ground mask level");
    words = scene_gen::mask_tile_words(info.cells);
  }
  const f64 corner_x = static_cast<f64>(t.x) * static_cast<f64>(c.tile_size),
            corner_z = static_cast<f64>(t.z) * static_cast<f64>(c.tile_size);
  const u64 w = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 tile_id = static_cast<u64>(static_cast<i64>(t.z) - p.min_z) * w +
                      static_cast<u64>(static_cast<i64>(t.x) - p.min_x);
  const auto& stand = c.stand.present() ? c.stand : c.ground;
  const i64 step = scene_gen::nearest_mm(static_cast<f64>(p.extent_m));
  for (u32 z = 0; z < e.cells; ++z)
    for (u32 x = 0; x < e.cells; ++x) {
      const u32 id = static_cast<u32>(tile_id * e.cells * e.cells + z * e.cells + x + 1);
      const u64 seed = hash_combine(hash_combine(c.world_seed, p.seed), id);
      const f64 wx = corner_x + (x + .4 + .2 * draw(seed, 1)) * p.cell_m;
      const f64 wz = corner_z + (z + .4 + .2 * draw(seed, 2)) * p.cell_m;
      const i64 mx = scene_gen::nearest_mm(wx), mz = scene_gen::nearest_mm(wz);
      // G135: none in a hole, a stream's bed and banks or under water (scene_gen.md, "Exclusion").
      if (c.excluded_mm(mx, mz)) continue;
      f64 density = static_cast<f64>(p.background), edge = 0;
      bool blocked = false;
      // Restrict whole reads to the same contract a streamed tile receives: `own`, the footprints
      // anchored in this tile, found once a tile rather than once a cell.
      for (const u32 index : own) {
        const scene_gen::Footprint& f = c.earlier[index];
        const f64 fx = static_cast<f64>(f.x_mm) * .001, fz = static_cast<f64>(f.z_mm) * .001;
        if (!std::isfinite(f.radius) || f.radius < 0)
          return fail(error, "invalid earlier footprint");
        const f64 dx = wx - fx, dz = wz - fz;
        const f64 gap = std::sqrt(dx * dx + dz * dz) - f.radius - static_cast<f64>(p.extent_m);
        if (gap < 0) {
          blocked = true;
          break;
        }
        edge = std::max(edge, std::max(0.0, 1 - gap / static_cast<f64>(p.edge_m)));
      }
      if (blocked) continue;
      const f64 h = c.ground.surface_mm(mx, mz);
      const f64 west = c.ground.surface_mm(mx - step, mz),
                east = c.ground.surface_mm(mx + step, mz),
                south = c.ground.surface_mm(mx, mz - step),
                north = c.ground.surface_mm(mx, mz + step);
      const f64 average = (west + east + south + north) * .25;
      if (!std::isfinite(h) || !std::isfinite(average)) return fail(error, "nonfinite ground");
      density += static_cast<f64>(p.hollow_gain) * std::clamp((average - h) / .1, 0.0, 1.0) +
                 static_cast<f64>(p.obstacle_gain) * edge;
      if (p.substrate >= 0) {
        const i64 tx =
            scene_gen::mask_floor_div(scene_gen::mask_floor_div(mx, info.spacing_mm), info.cells);
        const i64 tz =
            scene_gen::mask_floor_div(scene_gen::mask_floor_div(mz, info.spacing_mm), info.cells);
        if (tx < info.tile_min_x || tx > info.tile_max_x || tz < info.tile_min_z ||
            tz > info.tile_max_z)
          continue;
        const auto ref = scene_gen::mask_tile_of(info, mx, 0, mz, 0);
        const u32* read = held.get(masks, words, ref.tx, ref.tz);
        if (read == nullptr) return fail(error, "mask tile read failed");
        const std::span<const u32> tile_words(read, words);
        const auto sample = scene_gen::mask_sample(info, tile_words, ref, 0);
        f64 weight = 0;
        for (u32 ch = 0; ch < scene_gen::k_mask_channels; ++ch)
          if (((tile_words[0] >> (ch * 8)) & 255u) == static_cast<u32>(p.substrate))
            weight += static_cast<f64>(sample.weight[ch]);
        density = std::min(1.0, density) * weight;
      }
      if (draw(seed, 3) >= std::min(1.0, density)) continue;
      // Standing on the ground decided on (no moving surface): the five heights just read, which
      // the same queries would give again, so the ground is asked five times a cell and not ten.
      const bool moved = c.stand.present();
      const f64 sy = moved ? stand.surface_mm(mx, mz) : h;
      const f64 sx = moved ? (stand.surface_mm(mx + step, mz) - stand.surface_mm(mx - step, mz)) /
                                 (.002 * step)
                           : (east - west) / (.002 * step);
      const f64 sz = moved ? (stand.surface_mm(mx, mz + step) - stand.surface_mm(mx, mz - step)) /
                                 (.002 * step)
                           : (north - south) / (.002 * step);
      if (!std::isfinite(sy) || !std::isfinite(sx) || !std::isfinite(sz))
        return fail(error, "nonfinite standing ground");
      if (sx * sx + sz * sz > static_cast<f64>(p.max_slope) * static_cast<f64>(p.max_slope))
        continue;
      const Vec3 n = normalize(Vec3{static_cast<f32>(-sx), 1, static_cast<f32>(-sz)});
      const Quat tilt = normalize(Quat{n.z, 0, -n.x, 1 + n.y});
      const f64 a = draw(seed, 4) * 3.141592653589793;
      const Quat yaw{0, static_cast<f32>(det::sin(a)), 0, static_cast<f32>(det::cos(a))};
      scene_gen::Placement instance;
      instance.id = id;
      instance.mesh = static_cast<u32>(hash_combine(seed, 5) % e.meshes.size());
      instance.position = {wx, sy, wz};
      instance.rotation = tilt * yaw;
      out.push_back(instance);
    }
  return true;
}
bool tile(void* state, scene_gen::TileCoord t, const scene_gen::Context& c,
          scene_gen::Placements& out, std::string* error) {
  out.clear();
  const auto& e = *static_cast<Entry*>(state);
  if (c.tile_size != e.tile_size) return fail(error, "tile size changed after open");
  if (!occupies(state, t)) return true;
  const f64 size = static_cast<f64>(c.tile_size);
  Vector<u32> own;
  for (u32 i = 0; i < c.earlier.size(); ++i)
    if (anchored_in(c.earlier[i], static_cast<f64>(t.x) * size, static_cast<f64>(t.z) * size, size))
      own.push_back(i);
  scene_gen::Placements result;
  MaskTiles held;
  if (!place(e, t, c, std::span<const u32>(own.data(), own.size()), held, result.instances, error))
    return false;
  result.things = result.instances.size();
  out = std::move(result);
  return true;
}
// **A whole read's earlier footprints by tile** (T125): the host hands `expand` every earlier
// entry's placements at once, and each tile reads the ones anchored in it, so they are sorted by
// the tile their whole millimetres fall in once, and a tile gathers the 3 x 3 tiles about its own
// by binary search and keeps those `anchored_in` it, in the host's order, as the one-tile scan
// does. The neighbours are there because the anchor test is the f64 metres' and the sort the
// integers': a footprint on a border may fall either side. Read-only once made, so every job shares
// it.
class Anchored {
 public:
  Anchored(const scene_gen::Context& c, f32 tile_size) : c_(c), size_(tile_size) {
    tile_mm_ = scene_gen::nearest_mm(static_cast<f64>(tile_size));
    sorted_.reserve(static_cast<u32>(c.earlier.size()));
    for (u32 i = 0; i < c.earlier.size(); ++i)
      sorted_.push_back(Keyed{key(floor_div(c.earlier[i].z_mm), floor_div(c.earlier[i].x_mm)), i});
    std::sort(sorted_.begin(), sorted_.end(), [](const Keyed& a, const Keyed& b) {
      return a.key != b.key ? a.key < b.key : a.index < b.index;
    });
  }
  void gather(scene_gen::TileCoord t, Vector<u32>& out) const {
    out.clear();
    if (sorted_.empty()) return;
    for (i64 dz = -1; dz <= 1; ++dz) {
      const i64 first = key(i64{t.z} + dz, i64{t.x} - 1), last = key(i64{t.z} + dz, i64{t.x} + 1);
      auto it = std::lower_bound(sorted_.begin(), sorted_.end(), first,
                                 [](const Keyed& a, i64 k) { return a.key < k; });
      const f64 size = static_cast<f64>(size_);
      for (; it != sorted_.end() && it->key <= last; ++it)
        if (anchored_in(c_.earlier[it->index], static_cast<f64>(t.x) * size,
                        static_cast<f64>(t.z) * size, size))
          out.push_back(it->index);
    }
    std::sort(out.begin(), out.end());
  }

 private:
  struct Keyed {
    i64 key = 0;
    u32 index = 0;
  };
  // Rows of z, then x, in one ordered key: tiles of at most 1024 m over i64 millimetres are far
  // inside 2^33 apart.
  static i64 key(i64 z, i64 x) noexcept { return z * (i64{1} << 33) + x; }
  i64 floor_div(i64 mm) const noexcept {
    return mm >= 0 ? mm / tile_mm_ : -((-mm + tile_mm_ - 1) / tile_mm_);
  }
  const scene_gen::Context& c_;
  f32 size_ = 0;
  i64 tile_mm_ = 1;
  Vector<Keyed> sorted_;
};
// **A whole read on the job system's performance pool** (T125; litter.md, "Placement"), when
// `litter.whole_read_jobs` asks for one: the window's tiles in row-major order, in bands of whole
// rows, each band cut into contiguous runs, each run placed by one job into its own list, and the
// lists joined in band and run order — never in the order the jobs finish — so the instances,
// their ids rising, are the bytes one thread makes. A run stops at its first refusal, and the
// refusal returned is the one of the earliest tile, which is the one the serial walk met first.
bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  out.clear();
  const auto& e = *static_cast<Entry*>(state);
  if (c.tile_size != e.tile_size) return fail(error, "tile size changed after open");
  const u64 w = static_cast<u64>(static_cast<i64>(e.p.max_x) - e.p.min_x + 1);
  const u64 d = static_cast<u64>(static_cast<i64>(e.p.max_z) - e.p.min_z + 1);
  const u32 count = static_cast<u32>(w * d);  // checked at open: under 2^32
  const Anchored anchored(c, e.tile_size);
  const i64 wanted = whole_read_jobs.get();
  jobs::JobSystem* workers = count > 1 && wanted != 1 ? c.jobs() : nullptr;
  const u32 pool = workers != nullptr ? workers->worker_count(jobs::Pool::Performance) + 1 : 1;
  const u32 threads = wanted == 0 ? pool : static_cast<u32>(std::min<i64>(wanted, pool));
  // Four runs a thread over the host's pool; exactly one a job under a cap, so no more than the cap
  // run at once.
  const u32 per_thread = wanted == 0 ? 4 : 1;
  // The mask level, when the layer reads one: a band's tiles are read ahead on this thread in the
  // level's order, because the level reads through one window and jobs asking it from all over the
  // band each move it (measured: the pool four times slower than one thread on a lock-free ground).
  scene_gen::GroundMasks masks;
  scene_gen::GroundMaskInfo info;
  const bool ahead = threads > 1 && e.p.substrate >= 0 && c.ground.masks(masks, nullptr) &&
                     masks.valid() && masks.ops->info && masks.ops->tile &&
                     masks.ops->info(masks.state, info) && info.spacing_mm > 0 && info.cells >= 1 &&
                     info.cells <= 1024 && (info.cells & (info.cells - 1)) == 0;
  // Bands of whole rows, each enough tiles for every thread four times over.
  const u32 band_rows = threads <= 1
                            ? static_cast<u32>(d)
                            : std::max<u32>(1, static_cast<u32>((threads * 4u + w - 1) / w));
  struct Run {
    Vector<scene_gen::Placement> instances;
    bool ok = true;
    std::string why;
  };
  scene_gen::Placements result;
  result.meshes = e.meshes;
  Vector<Run> parts;
  MaskTiles band;
  for (u64 row = 0; row < d; row += band_rows) {
    const u32 first = static_cast<u32>(row * w);
    const u32 tiles = static_cast<u32>(std::min<u64>(band_rows, d - row) * w);
    const u32 runs = threads <= 1 ? 1 : std::min(tiles, threads * per_thread);
    band.held.clear();
    band.words.clear();
    if (ahead) {
      // Every mask tile the band's cells can fall in, within the level's window.
      const u32 n = scene_gen::mask_tile_words(info.cells);
      const i64 size_mm = scene_gen::nearest_mm(static_cast<f64>(e.tile_size));
      const auto mask_tile = [&](i64 mm) {
        return scene_gen::mask_floor_div(scene_gen::mask_floor_div(mm, info.spacing_mm),
                                         info.cells);
      };
      const i64 z0 =
          std::max<i64>(info.tile_min_z, mask_tile((e.p.min_z + static_cast<i64>(row)) * size_mm));
      const i64 z1 = std::min<i64>(
          info.tile_max_z, mask_tile((e.p.min_z + static_cast<i64>(row + tiles / w)) * size_mm));
      const i64 x0 = std::max<i64>(info.tile_min_x, mask_tile(e.p.min_x * size_mm));
      const i64 x1 = std::min<i64>(info.tile_max_x, mask_tile((e.p.max_x + 1) * size_mm));
      for (i64 tz = z0; tz <= z1; ++tz)
        for (i64 tx = x0; tx <= x1; ++tx) {
          const u32 at = band.words.size();
          band.words.resize(at + n);
          const bool read = masks.ops->tile(masks.state, static_cast<i32>(tx), static_cast<i32>(tz),
                                            std::span<u32>(band.words.data() + at, n));
          if (!read) band.words.resize(at);
          band.held.push_back(MaskTiles::Held{static_cast<i32>(tx), static_cast<i32>(tz),
                                              read ? at : MaskTiles::k_absent});
        }
    }
    parts.clear();
    parts.resize(runs);
    const auto run = [&](u32 r) {
      Vector<u32> own;
      MaskTiles held;
      if (ahead) held.band = &band;
      const u32 from = first + static_cast<u32>(u64{tiles} * r / runs);
      const u32 to = first + static_cast<u32>(u64{tiles} * (r + 1) / runs);
      for (u32 i = from; i < to; ++i) {
        const scene_gen::TileCoord t{static_cast<i32>(e.p.min_x + static_cast<i64>(i % w)),
                                     static_cast<i32>(e.p.min_z + static_cast<i64>(i / w))};
        anchored.gather(t, own);
        if (!place(e, t, c, std::span<const u32>(own.data(), own.size()), held, parts[r].instances,
                   &parts[r].why)) {
          parts[r].ok = false;
          return;
        }
      }
    };
    if (runs == 1) {
      run(0);
    } else {
      workers->parallel_for(jobs::Pool::Performance, runs, 1, [&](u32 begin, u32 end) {
        for (u32 r = begin; r < end; ++r)
          run(r);
      });
    }
    for (const Run& part : parts) {
      if (!part.ok) {
        if (error != nullptr) *error = part.why;
        return false;
      }
      for (const scene_gen::Placement& p : part.instances)
        result.instances.push_back(p);
    }
  }
  result.things = result.instances.size();
  out = std::move(result);
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_generator{.name = "litter",
                                                        .open = open,
                                                        .close = close,
                                                        .expand = expand,
                                                        .meshes = meshes,
                                                        .tile = tile,
                                                        .occupies = occupies,
                                                        .representation = representation};
const scene_gen::Registrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::litter
