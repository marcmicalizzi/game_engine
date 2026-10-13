// The scatter layer as a placement generator (scene_gen.h; docs/subsystems/scene_gen.md,
// docs/subsystems/scatter.md): `expand` is a whole read of the layer's region, `tile` one tile of
// it at the ring's far tier, and the two agree because both are `Placer::place_tile`. The
// renderer's scene reader and the world's placements consumer find it by name and link nothing of
// it.
#include <core/jobs/job_system.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/scatter/scatter.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace engine::scatter {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_scatter, "scatter");

// The entry, read once: the layer, its placer and its meshes, one per kind in kind order.
struct Entry {
  std::string where;
  Placer placer;
  Vector<scene_gen::PlacementMesh> meshes;
};

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not a scatter layer") : text;
}

void to_placements(std::span<const Point> points, scene_gen::Placements& out) {
  out.instances.reserve(out.instances.size() + static_cast<u32>(points.size()));
  for (const Point& p : points) {
    scene_gen::Placement placement;
    placement.position = position(p);
    placement.rotation = p.rotation;
    placement.scale = Vec3{p.scale, p.scale, p.scale};
    placement.mesh = p.kind;
    placement.tag = static_cast<u8>(p.kind);
    out.instances.push_back(placement);
  }
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  auto entry = std::make_unique<Entry>();
  entry->where = std::string(context.where);
  const std::string& where = entry->where;
  Layer layer;
  schema::ReadContext ctx;
  if (!schema::from_json(layer, params, ctx) || !ctx.ok())
    return fail(where + ": " + schema_errors(ctx));
  // A streamed layer is placed one ring tile at a time, so its grid must be the ring's.
  if (context.world != nullptr &&
      std::fabs(static_cast<f64>(layer.tile_size) - static_cast<f64>(context.tile_size)) > 1e-4) {
    return fail(where + ": its tile_size is " + std::to_string(layer.tile_size) +
                " and the world's " + std::to_string(context.tile_size) +
                "; a streamed layer is placed one ring tile at a time, so they must be one grid");
  }
  std::string why;
  if (!entry->placer.open(layer, context.dir, context.world_seed, &why))
    return fail(where + ": " + why);
  if (!kind_meshes(layer, context.dir, entry->meshes, &why)) return fail(where + ": " + why);
  *state = entry.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<Entry*>(state); }

// The whole region, every kind (a whole read has no camera to be far from), tile by tile in rows
// of x in order of z; more than 32 tiles go to the job system's performance pool in contiguous
// runs joined in tile order, so the instances are the ones one thread makes.
// **A whole read's earlier placements, by tile** (scatter.md, "Exclusion by earlier placements"):
// the reader hands `expand` every earlier entry's placements at once, and a tile needs the ones
// that can reach it — within the largest footprint and `clear` of its rectangle — so they are
// sorted by the tile they stand on once, and each tile gathers its neighbourhood's by binary
// search. Read-only once made, so the tiles' jobs share it.
class Nearby {
 public:
  Nearby(const Placer& placer, const scene_gen::Context& context)
      : context_(context), tile_mm_(placer.tile_mm()) {
    if (context.earlier.empty()) return;
    f64 radius = 0.0;
    for (const scene_gen::Footprint& f : context.earlier)
      radius = std::max(radius, f.radius);
    const i64 reach = scene_gen::nearest_mm(radius) + placer.earlier_reach_mm();
    span_ = static_cast<i32>((reach + tile_mm_ - 1) / tile_mm_);
    sorted_.reserve(static_cast<u32>(context.earlier.size()));
    for (const scene_gen::Footprint& f : context.earlier)
      sorted_.push_back(Keyed{key(floor_div(f.z_mm), floor_div(f.x_mm)), f});
    std::sort(sorted_.begin(), sorted_.end(), [](const Keyed& a, const Keyed& b) {
      if (a.key != b.key) return a.key < b.key;
      if (a.f.z_mm != b.f.z_mm) return a.f.z_mm < b.f.z_mm;
      return a.f.x_mm < b.f.x_mm;
    });
  }
  Site site(scene_gen::TileCoord t, Vector<scene_gen::Footprint>& scratch) const {
    Site out;
    out.ground = context_.ground;
    out.stand = context_.stand;
    if (sorted_.empty()) return out;
    scratch.clear();
    for (i32 dz = -span_; dz <= span_; ++dz) {
      const i64 row = key(static_cast<i64>(t.z) + dz, static_cast<i64>(t.x) - span_);
      const i64 end = key(static_cast<i64>(t.z) + dz, static_cast<i64>(t.x) + span_);
      auto it = std::lower_bound(sorted_.begin(), sorted_.end(), row,
                                 [](const Keyed& a, i64 k) { return a.key < k; });
      for (; it != sorted_.end() && it->key <= end; ++it)
        scratch.push_back(it->f);
    }
    out.earlier = std::span<const scene_gen::Footprint>(scratch.data(), scratch.size());
    return out;
  }

 private:
  struct Keyed {
    i64 key = 0;
    scene_gen::Footprint f;
  };
  // Rows of z, then x, in one ordered key: tiles are i32, so 2^32 apart is room for every x.
  static i64 key(i64 z, i64 x) noexcept { return z * (i64{1} << 33) + x; }
  i64 floor_div(i64 mm) const noexcept {
    return mm >= 0 ? mm / tile_mm_ : -((-mm + tile_mm_ - 1) / tile_mm_);
  }
  const scene_gen::Context& context_;
  i64 tile_mm_ = 1;
  i32 span_ = 0;
  Vector<Keyed> sorted_;
};

bool expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
            std::string* error) {
  const Entry& entry = *static_cast<const Entry*>(state);
  const i64 start = time::monotonic_ns();
  Vector<scene_gen::TileCoord> tiles;
  std::string why;
  if (!entry.placer.region_tiles(tiles, &why)) {
    if (error != nullptr) *error = entry.where + ": " + why;
    return false;
  }
  const u32 all = entry.placer.kind_count() >= 32 ? ~0u : (1u << entry.placer.kind_count()) - 1u;
  const Nearby near(entry.placer, context);
  const u32 count = tiles.size();
  jobs::JobSystem* workers = count > 32 ? context.jobs() : nullptr;
  const u32 threads = workers != nullptr ? workers->worker_count(jobs::Pool::Performance) + 1 : 1;
  Vector<Point> points;
  if (threads <= 1) {
    Vector<scene_gen::Footprint> scratch;
    for (const scene_gen::TileCoord& t : tiles)
      entry.placer.place_tile(t, near.site(t, scratch), all, points);
  } else {
    const u32 runs = std::min(count, threads * 4);
    Vector<Vector<Point>> parts(runs);
    workers->parallel_for(jobs::Pool::Performance, runs, 1, [&](u32 begin, u32 end) {
      Vector<scene_gen::Footprint> scratch;
      for (u32 r = begin; r < end; ++r) {
        const u32 from = static_cast<u32>(u64{count} * r / runs);
        const u32 to = static_cast<u32>(u64{count} * (r + 1) / runs);
        for (u32 i = from; i < to; ++i)
          entry.placer.place_tile(tiles[i], near.site(tiles[i], scratch), all, parts[r]);
      }
    });
    u32 total = 0;
    for (const Vector<Point>& part : parts)
      total += part.size();
    points.reserve(total);
    for (const Vector<Point>& part : parts) {
      for (const Point& p : part)
        points.push_back(p);
    }
  }
  for (const scene_gen::PlacementMesh& mesh : entry.meshes)
    out.meshes.push_back(mesh);
  to_placements(std::span<const Point>(points.data(), points.size()), out);
  ENGINE_LOG_INFO(
      log_scatter, "scatter placed", log::field("entry", entry.where),
      log::field("layer", entry.placer.layer().name), log::field("tiles", count),
      log::field("instances", points.size()), log::field("threads", threads),
      log::field("ms", static_cast<f64>(time::monotonic_ns() - start) / 1.0e6),
      log::field("hash", hash_points(std::span<const Point>(points.data(), points.size()))));
  return true;
}

bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  const Entry& entry = *static_cast<const Entry*>(state);
  for (const scene_gen::PlacementMesh& mesh : entry.meshes)
    out.push_back(mesh);
  return true;
}

bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  return static_cast<const Entry*>(state)->placer.occupies(tile);
}

// The far tier a ring draws: how many kinds its distance leaves out. The kinds a ring keeps are
// those whose `far` is past its nearest tile, a set that only shrinks outwards, so the count of
// those left out names it.
u8 representation(const void* state, const scene_gen::Context& context) noexcept {
  const Entry& entry = *static_cast<const Entry*>(state);
  const u32 mask = entry.placer.ring_kind_mask(context);
  u32 left_out = 0;
  for (u32 k = 0; k < entry.placer.kind_count(); ++k)
    left_out += ((mask >> k) & 1u) != 0 ? 0u : 1u;
  return static_cast<u8>(left_out);
}

// One tile at its ring's far tier: the tile `expand` places, less the kinds the ring leaves out.
bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string*) {
  const Entry& entry = *static_cast<const Entry*>(state);
  Vector<Point> points;
  entry.placer.place_tile(at, Site{context.ground, context.stand, context.earlier},
                          entry.placer.ring_kind_mask(context), points);
  to_placements(std::span<const Point>(points.data(), points.size()), out);
  out.kind = representation(state, context);
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_scatter{
    .name = k_placement_generator,
    .open = &open,
    .close = &close,
    .expand = &expand,
    .meshes = &meshes,
    .tile = &tile,
    .occupies = &occupies,
    .representation = &representation,
};
const scene_gen::Registrar k_scatter_registrar{k_scatter};

}  // namespace

}  // namespace engine::scatter
