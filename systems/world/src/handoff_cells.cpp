// The pairs a hand-off placement holds (G205; docs/subsystems/world.md, "A hand-off placement is
// held by its band"). A placement with the near hand-off role (`scene_gen::k_handoff_near`,
// `renderer::SceneInstance::handoff`) is drawn only short of its band's end, a distance from the
// eye (renderer.md, "The hand-off by distance"), but its tile is drawn over a whole ring: on the
// forest the sward's near forms held 5.08 million pairs in the inner ring's five tiles, and the
// band covers an eighth of them. So the consumer keeps a tile's near placements apart from the rest
// of the tile, grouped by a cell much finer than a tile, and hands a cell on — a run of its own,
// under a key of its own — only while the band can reach it. A cell not handed holds no pair, and
// every near instance in it is one the cull pass would have dropped whole, so the picture is the
// one it was. The rule keys on the role and nothing else: whatever placed the instance.
//
// The members here are `PlacementTiles`'s; placement_tiles.cpp calls them from `finalize`, `admit`
// and `commit_tail`.
#include <core/time/time.h>
#include <domain/geometry/cluster.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/pair_account.h>
#include <systems/world/placement_tiles.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::world {

namespace {

// The most cells one tile's near placements are grouped into. A tile whose near placements span
// more cells than this at the configured size is grouped by a cell twice as large, and again,
// until they fit: coarser cells hold a little more than the band needs and are never wrong.
constexpr u64 k_max_block_cells = 4096;

// The first column of tiles, x = -2^31: the keys the cells' runs are named by. No tile there is
// held while the cells are on (`take`), so a cell's key is never a tile's. A tile's key is its
// coordinates biased by 2^31 (`tile_key`, so keys sort in tile order): that column's high half is
// zero, and a cell's key is its number alone, from one up (zero is the renderer's "no key").
constexpr u64 k_cell_key_base = 0;

f64 instance_x(const renderer::SceneInstance& instance) noexcept {
  return instance.origin.x + static_cast<f64>(instance.transform.position.x);
}
f64 instance_z(const renderer::SceneInstance& instance) noexcept {
  return instance.origin.z + static_cast<f64>(instance.transform.position.z);
}

// The squared ground distance from `eye` to the box.
f64 box_distance2(const WorldPos& eye, f64 lo_x, f64 hi_x, f64 lo_z, f64 hi_z) noexcept {
  const f64 dx = eye.x < lo_x ? lo_x - eye.x : (eye.x > hi_x ? eye.x - hi_x : 0.0);
  const f64 dz = eye.z < lo_z ? lo_z - eye.z : (eye.z > hi_z ? eye.z - hi_z : 0.0);
  return dx * dx + dz * dz;
}

}  // namespace

bool PlacementTiles::cell_key_reserved(u64 key) noexcept {
  return (key >> 32) == (k_cell_key_base >> 32);
}

// **How far from its origin the cull pass can find a placement of `mesh`, at scale 1**: the
// farthest any of its clusters' spheres reaches, plus the padding a swaying mesh's spheres are
// tested with (`renderer::flora_bounds_padding`, the bound `GpuScene` gives the instance). The
// distance in three dimensions, so it holds however the placement is turned. Measured the first
// time a near placement names the mesh, on the committing thread, and kept.
f32 PlacementTiles::cull_reach(u32 mesh) {
  if (mesh >= scene_->parts.size()) return -1.0f;
  if (cull_reach_.size() < scene_->parts.size()) cull_reach_.resize(scene_->parts.size(), -1.0f);
  if (cull_reach_[mesh] >= 0.0f) return cull_reach_[mesh];
  const geometry::ClusterMeshPart& part = scene_->parts[mesh];
  f32 reach = 0.0f;
  const u32 end = part.first_cluster + part.cluster_count;
  for (u32 i = part.first_cluster; i < end && i < scene_->lod.mesh.clusters.size(); ++i) {
    const geometry::ClusterDesc& c = scene_->lod.mesh.clusters[i];
    reach = std::max(reach, length(c.center) + c.radius);
  }
  f32 padding = 0.0f;
  if (mesh < scene_->sources.size()) {
    const renderer::SourceMesh& source = scene_->sources[mesh];
    renderer::FloraMesh tree;
    if (source.has_motion() &&
        part.first_vertex + source.motion_vertices.size() <= scene_->lod.mesh.vertices.size() &&
        renderer::flora_mesh(std::span<const geometry::ClusterFileMotionPart>(
                                 source.motion_parts.data(), source.motion_parts.size()),
                             std::span<const geometry::ClusterFileStructuralPart>(
                                 source.structural_parts.data(), source.structural_parts.size()),
                             source.species, tree)) {
      padding = renderer::flora_bounds_padding(
          tree,
          std::span<const Vec3>(scene_->lod.mesh.vertices.data() + part.first_vertex,
                                source.motion_vertices.size()),
          std::span<const geometry::ClusterFileMotionVertex>(source.motion_vertices.data(),
                                                             source.motion_vertices.size()));
    }
  }
  cull_reach_[mesh] = reach + (padding > 0.0f ? padding : 0.0f);
  return cull_reach_[mesh];
}

// **A build's near placements, apart and by cell.** The tile's instances become its other
// placements in the tile's order and then its near ones, grouped by the cell each stands in and in
// the tile's order within one; the tags and the forms' sources move with them, and a placement the
// ring draws as nothing keeps its place among the others. Only a near placement of a resident mesh
// is moved: a tile's own mesh has no reach until it is built, and stays with the tile. A block
// with none is untouched, so a scene with no hand-off role pays one read of each instance's role.
void PlacementTiles::split_near(TileBlock& block) {
  Handoff& set = block.handoff;
  set.clear();
  if (!cells_on()) return;
  const u32 n = static_cast<u32>(block.instances.size());
  u32 count = 0;
  for (u32 i = 0; i < n && count == 0; ++i)
    count += block.instances[i].handoff == scene_gen::k_handoff_near ? 1u : 0u;
  if (count == 0) return;
  const i64 start = time::monotonic_ns();
  // Which are moved, and the cells they span: at the configured size, or the first doubling of it
  // that keeps the tile within `k_max_block_cells`.
  split_cell_.resize(n);
  f64 size = static_cast<f64>(config_.handoff_cell_m);
  i64 lo_x = 0;
  i64 lo_z = 0;
  u64 width = 0;
  u64 height = 0;
  for (;;) {
    i64 hi_x = std::numeric_limits<i64>::min();
    i64 hi_z = std::numeric_limits<i64>::min();
    lo_x = lo_z = std::numeric_limits<i64>::max();
    count = 0;
    for (u32 i = 0; i < n; ++i) {
      const renderer::SceneInstance& instance = block.instances[i];
      split_cell_[i] = ~0u;
      if (instance.handoff != scene_gen::k_handoff_near || cull_reach(instance.mesh) < 0.0f)
        continue;
      const i64 cx = static_cast<i64>(std::floor(instance_x(instance) / size));
      const i64 cz = static_cast<i64>(std::floor(instance_z(instance) / size));
      lo_x = std::min(lo_x, cx);
      hi_x = std::max(hi_x, cx);
      lo_z = std::min(lo_z, cz);
      hi_z = std::max(hi_z, cz);
      split_cell_[i] = 0;
      ++count;
    }
    if (count == 0) return;
    width = static_cast<u64>(hi_x - lo_x) + 1u;
    height = static_cast<u64>(hi_z - lo_z) + 1u;
    if (width <= k_max_block_cells && height <= k_max_block_cells &&
        width * height <= k_max_block_cells) {
      break;
    }
    size *= 2.0;
  }
  // Each one's cell, the others' places, and how many each cell holds.
  split_counts_.assign(static_cast<u32>(width * height), 0u);
  split_before_.resize(n + 1u);
  u32 base = 0;
  for (u32 i = 0; i < n; ++i) {
    split_before_[i] = base;
    if (split_cell_[i] == ~0u) {
      ++base;
      continue;
    }
    const renderer::SceneInstance& instance = block.instances[i];
    const u64 cx =
        static_cast<u64>(static_cast<i64>(std::floor(instance_x(instance) / size)) - lo_x);
    const u64 cz =
        static_cast<u64>(static_cast<i64>(std::floor(instance_z(instance) / size)) - lo_z);
    split_cell_[i] = static_cast<u32>(cz * width + cx);
    ++split_counts_[split_cell_[i]];
  }
  split_before_[n] = base;
  // The cells that hold any, in cell order; `split_counts_` becomes each one's record.
  u32 offset = 0;
  for (u32 c = 0; c < split_counts_.size(); ++c) {
    const u32 held = split_counts_[c];
    if (held == 0) {
      split_counts_[c] = ~0u;
      continue;
    }
    NearCell cell;
    cell.first = offset;
    cell.lo_x = cell.lo_z = std::numeric_limits<f64>::max();
    cell.hi_x = cell.hi_z = std::numeric_limits<f64>::lowest();
    split_counts_[c] = static_cast<u32>(set.cells.size());
    set.cells.push_back(cell);
    offset += held;
  }
  // The new order, written beside the old and copied back; each cell's box and pairs as it fills.
  const bool sources = block.forms.sources.size() == n;
  split_instances_.resize(n);
  split_tags_.resize(n);
  if (sources) split_sources_.resize(n);
  for (u32 i = 0; i < n; ++i) {
    u32 to = split_before_[i];
    if (split_cell_[i] != ~0u) {
      NearCell& cell = set.cells[split_counts_[split_cell_[i]]];
      to = base + cell.first + cell.count++;
      const renderer::SceneInstance& instance = block.instances[i];
      const Vec3 s = instance.transform.scale;
      const f32 scale = std::max(std::fabs(s.x), std::max(std::fabs(s.y), std::fabs(s.z)));
      const f64 reach =
          static_cast<f64>(scale) *
          static_cast<f64>(cull_reach(instance.mesh) +
                           (instance.bounds_padding > 0.0f ? instance.bounds_padding : 0.0f));
      const f64 x = instance_x(instance);
      const f64 z = instance_z(instance);
      cell.lo_x = std::min(cell.lo_x, x - reach);
      cell.hi_x = std::max(cell.hi_x, x + reach);
      cell.lo_z = std::min(cell.lo_z, z - reach);
      cell.hi_z = std::max(cell.hi_z, z + reach);
      cell.pairs += scene_->parts[instance.mesh].cluster_count;
    }
    split_instances_[to] = block.instances[i];
    split_tags_[to] = block.tags[i];
    if (sources) split_sources_[to] = block.forms.sources[i];
  }
  for (u32 i = 0; i < n; ++i) {
    block.instances[i] = split_instances_[i];
    block.tags[i] = split_tags_[i];
    if (sources) block.forms.sources[i] = split_sources_[i];
  }
  // A placement drawn as nothing sat before the drawn instance `at`: it sits before the same one
  // still, or at the end of the others when that one moved to a cell.
  for (u32& at : block.forms.unseen_at)
    at = split_before_[at < n ? at : n];
  for (NearCell& cell : set.cells)
    cell.reserved = renderer::reserved_tile_pairs(cell.pairs);
  set.count = count;
  set.version = ++next_version_;
  ++stats_.handoff_splits;
  stats_.handoff_ns += time::monotonic_ns() - start;
}

void PlacementTiles::set_handoff_eyes(std::span<const WorldPos> eyes) {
  eyes_.assign(eyes.begin(), eyes.end());
  eyes_set_ = true;
}

// A key no tile and no other cell holds: the next of the reserved column's.
u64 PlacementTiles::next_cell_key() {
  if (++cell_serial_ == 0) {
    // Every number has been given once (2^32 arrivals): the cells handed now take the first ones
    // again, in tile order, and the renderer sees each as a block that left and one that came.
    for (u32 i = 0; i < tiles_.size(); ++i) {
      TileBlock& block = tiles_.value_at(i);
      for (Handoff* set : {&block.handoff, &block.held_handoff}) {
        for (NearCell& cell : set->cells)
          if (cell.in) cell.key = k_cell_key_base | u64{++cell_serial_};
      }
    }
    ++cell_serial_;
  }
  return k_cell_key_base | u64{cell_serial_};
}

// **Which cells the band can reach now.** A cell comes in when its box — its placements' origins,
// each grown by its reach — is within the band's end, the margin and the lead of an eye on the
// ground plane, and goes when it is `handoff_hold_m` farther than that. The ground distance is
// never more than the distance the cull pass measures, and the box never less than the placements,
// so a cell that is out holds only instances the cull pass would drop. **The lead** is what the
// eyes moved since the last update, `handoff_lead_updates` times, and at most the band's end:
// slack for a host whose world update runs a frame behind the camera it draws from. (It is not
// there for the load-in: a cell's block is as old as its tile's, `TileRange::part_of`.) Eyes first
// seen, or not the ones the last update had, take no lead. With no eye known every cell is in.
// True when a cell came or went.
bool PlacementTiles::refresh_cells() {
  if (!cells_on()) return false;
  const i64 start = time::monotonic_ns();
  const std::span<const WorldPos> eyes =
      eyes_set_ ? std::span<const WorldPos>(eyes_.data(), eyes_.size())
                : (ring_ != nullptr ? ring_->observers().positions() : std::span<const WorldPos>());
  const f64 to = static_cast<f64>(config_.handoff_to_m);
  f64 step = -1.0;  // unknown: the eyes are not the ones the last update had
  if (last_eyes_.size() == eyes.size() && cells_stepped_) {
    step = 0.0;
    for (u32 k = 0; k < eyes.size(); ++k) {
      const f64 dx = eyes[k].x - last_eyes_[k].x;
      const f64 dz = eyes[k].z - last_eyes_[k].z;
      step = std::max(step, std::sqrt(dx * dx + dz * dz));
    }
  }
  // Once an update: a second look in the same update (a commit, then `end_update`) sees eyes that
  // did not move and keeps the lead the first one took.
  if (step != 0.0 || !cells_refreshed_) {
    const f64 lead = step < 0.0 ? 0.0 : static_cast<f64>(config_.handoff_lead_updates) * step;
    cell_lead_ = lead < to ? lead : to;
  }
  last_eyes_.assign(eyes.begin(), eyes.end());
  cells_stepped_ = true;
  cells_refreshed_ = true;
  const f64 in_r = to + static_cast<f64>(config_.handoff_margin_m) + cell_lead_;
  const f64 out_r = in_r + static_cast<f64>(config_.handoff_hold_m);
  bool changed = false;
  u32 cells = 0;
  u32 cells_in = 0;
  u32 held = 0;
  u64 pairs_out = 0;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    TileBlock& block = tiles_.value_at(i);
    Handoff& set = block.waiting ? block.held_handoff : block.handoff;
    if (set.cells.empty()) continue;
    for (NearCell& cell : set.cells) {
      bool in = eyes.empty();
      const f64 r = cell.in ? out_r : in_r;
      for (u32 k = 0; k < eyes.size() && !in; ++k)
        in = box_distance2(eyes[k], cell.lo_x, cell.hi_x, cell.lo_z, cell.hi_z) <= r * r;
      if (in != cell.in) {
        cell.in = in;
        changed = true;
        ++stats_.handoff_flips;
        if (in) {
          cell.key = next_cell_key();
          ++set.in_cells;
          set.in_instances += cell.count;
          set.in_pairs += cell.pairs;
          set.in_reserved += cell.reserved;
        } else {
          cell.key = 0;
          --set.in_cells;
          set.in_instances -= cell.count;
          set.in_pairs -= cell.pairs;
          set.in_reserved -= cell.reserved;
        }
      }
      pairs_out += cell.in ? 0u : cell.pairs;
    }
    cells += static_cast<u32>(set.cells.size());
    cells_in += set.in_cells;
    held += set.count;
  }
  stats_.handoff_cells = cells;
  stats_.handoff_cells_in = cells_in;
  stats_.handoff_instances = held;
  stats_.handoff_pairs_out = pairs_out;
  stats_.handoff_ns += time::monotonic_ns() - start;
  return changed;
}

// The runs a commit hands a run sink: each drawn tile's own run — its placements but the near ones
// the cells hold, under the tile's key, with the form changes that fall among them — and then a
// run for each of its cells that is in, under the cell's key and the version of the build that
// made it. A tile with no cell is one run, as it always was.
void PlacementTiles::make_runs(Vector<TileRange>& out) const {
  out.clear();
  u32 first = 0;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const TileBlock& block = tiles_.value_at(i);
    if (block.withheld) continue;
    const std::span<const renderer::SceneInstance> drawn = block.drawn();
    if (drawn.empty()) continue;  // a new tile whose first build still waits draws nothing
    const Handoff& set = block.waiting ? block.held_handoff : block.handoff;
    const u32 base = static_cast<u32>(drawn.size()) - set.count;
    if (base != 0) {
      TileRange range{tiles_.key_at(i), first, base, block.ring, drawn.data(), block.version};
      if (block.reformed_from != 0) {  // its forms changed in place since the last hand-over (G202)
        range.from_version = block.reformed_from;
        range.changed = block.changed.data();
        // Ascending: the ones among the tile's own run (a near placement keeps its mesh).
        range.changed_count =
            static_cast<u32>(std::lower_bound(block.changed.begin(), block.changed.end(), base) -
                             block.changed.begin());
      }
      out.push_back(range);
      first += base;
    }
    for (const NearCell& cell : set.cells) {
      if (!cell.in) continue;
      TileRange range{cell.key,   first, cell.count, block.ring, drawn.data() + base + cell.first,
                      set.version};
      range.part_of = tiles_.key_at(i);
      out.push_back(range);
      first += cell.count;
    }
  }
}

}  // namespace engine::world
