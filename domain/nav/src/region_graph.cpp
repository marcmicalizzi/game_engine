// The coarse connectivity tier. No Recast or Detour here at all: the graph is built from the
// tile bytes, which is what makes it impossible for the two tiers to disagree about
// reachability — a region edge exists exactly where a tile border portal does, and a tile border
// portal is what Recast wrote into the polygon mesh when it found a walkable edge on the border.

#include "tile_format.h"

#include <core/base/assert.h>
#include <domain/nav/region_graph.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace engine::nav {
namespace {

constexpr f32 min_f(f32 a, f32 b) noexcept { return a < b ? a : b; }
constexpr f32 max_f(f32 a, f32 b) noexcept { return a > b ? a : b; }

// The tile that lies across `side` from this one. 0 = -x, 1 = +z, 2 = +x, 3 = -z, Detour's own
// order, which is also the order Recast writes into a polygon's border edges.
TileCoord neighbour_of(TileCoord coord, u8 side) noexcept {
  switch (side) {
    case 0: return TileCoord{coord.x - 1, coord.y};
    case 1: return TileCoord{coord.x, coord.y + 1};
    case 2: return TileCoord{coord.x + 1, coord.y};
    default: return TileCoord{coord.x, coord.y - 1};
  }
}

constexpr u8 opposite_side(u8 side) noexcept { return static_cast<u8>((side + 2u) & 3u); }

// The world point two regions meet at, given the overlapping stretch of their shared border.
Vec3 portal_point(const TilePortal& a, const TilePortal& b) noexcept {
  const f32 lo = max_f(a.lo, b.lo);
  const f32 hi = min_f(a.hi, b.hi);
  const f32 along = (lo + hi) * 0.5f;
  const f32 y = (max_f(a.y_min, b.y_min) + min_f(a.y_max, b.y_max)) * 0.5f;
  const f32 axis = (a.axis + b.axis) * 0.5f;
  if (a.side == 0 || a.side == 2) return Vec3(axis, y, along);
  return Vec3(along, y, axis);
}

// Two stretches of the same border join when they overlap along it and their heights meet. The
// height test is what keeps the two landings of a stairwell that share a tile border from being
// welded into one region: they overlap perfectly in plan and not at all in y.
bool portals_meet(const TilePortal& a, const TilePortal& b, f32 join_eps, f32 climb) noexcept {
  if (std::abs(a.axis - b.axis) > join_eps) return false;
  if (a.lo > b.hi + join_eps || b.lo > a.hi + join_eps) return false;
  if (a.y_min > b.y_max + climb || b.y_min > a.y_max + climb) return false;
  return true;
}

}  // namespace

// --- structure ----------------------------------------------------------------------------

void RegionGraph::clear() noexcept {
  nodes_.clear();
  edges_.clear();
  free_nodes_.clear();
  tiles_.clear();
  links_.clear();
  live_nodes_ = 0;
  ++revision_;
}

u32 RegionGraph::allocate_node() {
  if (!free_nodes_.empty()) {
    const u32 index = free_nodes_.back();
    free_nodes_.pop_back();
    nodes_[index] = RegionNode{};
    edges_[index].clear();
    ++live_nodes_;
    return index;
  }
  const u32 index = nodes_.size();
  nodes_.push_back(RegionNode{});
  edges_.push_back(SmallVector<Edge, 4>{});
  ++live_nodes_;
  return index;
}

void RegionGraph::release_node(u32 index) {
  unlink_node(index);
  nodes_[index].alive = false;
  free_nodes_.push_back(index);
  --live_nodes_;
}

void RegionGraph::unlink_node(u32 index) {
  for (const Edge& edge : edges_[index]) {
    SmallVector<Edge, 4>& other = edges_[edge.node];
    for (u32 i = 0; i < other.size(); ++i) {
      if (other[i].node == index) {
        other.erase(other.begin() + i);
        break;
      }
    }
  }
  edges_[index].clear();
}

void RegionGraph::connect(u32 a, u32 b, Vec3 point) {
  if (a == b) return;
  for (const Edge& edge : edges_[a]) {
    if (edge.node == b) return;  // one edge per pair; the first portal found is the crossing
  }
  edges_[a].push_back(Edge{b, point});
  edges_[b].push_back(Edge{a, point});
}

void RegionGraph::disconnect(u32 a, u32 b) {
  for (u32 i = 0; i < edges_[a].size(); ++i) {
    if (edges_[a][i].node == b) {
      edges_[a].erase(edges_[a].begin() + i);
      break;
    }
  }
  for (u32 i = 0; i < edges_[b].size(); ++i) {
    if (edges_[b][i].node == a) {
      edges_[b].erase(edges_[b].begin() + i);
      break;
    }
  }
}

bool RegionGraph::set_tile_bytes(std::span<const u8> bytes) {
  NavTileData tile;
  if (!tile.from_bytes(bytes)) return false;
  set_tile(tile);
  return true;
}

void RegionGraph::set_tile(const NavTileData& tile) {
  if (tile.empty()) {
    remove_tile(tile.coord());
    return;
  }
  // The grid comes from the tiles rather than from a separate configure() call: a graph and a
  // mesh fed the same tiles cannot then be told different grids.
  if (grid_tile_size_ == 0.0f) {
    grid_origin_ = tile.grid_origin();
    grid_tile_size_ = tile.grid_tile_size();
    // Two portals join only if their heights meet within the step the agent can climb. Anything
    // further apart is two floors, and welding them would make the coarse tier claim a route
    // that does not exist — the one error this tier must not make.
    grid_climb_ = tile.agent_climb();
  }

  remove_tile(tile.coord());

  const u64 key = tile_key(tile.coord());
  TileEntry entry;
  const std::span<const TileRegionInfo> regions = tile.regions();
  if (regions.empty()) return;

  Vector<u32> indices;
  indices.reserve(static_cast<u32>(regions.size()));
  for (u32 i = 0; i < regions.size(); ++i) {
    const u32 index = allocate_node();
    RegionNode& node = nodes_[index];
    node.centroid = regions[i].centroid;
    node.bounds = regions[i].bounds;
    node.area = regions[i].area;
    node.coord = tile.coord();
    node.component = static_cast<u16>(i);
    node.alive = true;
    indices.push_back(index);
  }
  entry.nodes.assign(indices.begin(), indices.end());
  const std::span<const TilePortal> portals = tile.portals();
  entry.portals.assign(portals.begin(), portals.end());
  tiles_.insert_or_assign(key, std::move(entry));

  link_tile_to_neighbours(tile.coord());
  relink_links_touching(tile.coord());
  ++revision_;
}

void RegionGraph::remove_tile(TileCoord coord) {
  const u64 key = tile_key(coord);
  TileEntry* entry = tiles_.find_value(key);
  if (entry == nullptr) return;
  for (const u32 index : entry->nodes)
    release_node(index);
  tiles_.erase(key);
  relink_links_touching(coord);
  ++revision_;
}

bool RegionGraph::contains_tile(TileCoord coord) const noexcept {
  return tiles_.contains(tile_key(coord));
}

void RegionGraph::link_tile_to_neighbours(TileCoord coord) {
  for (u8 side = 0; side < 4; ++side)
    link_border(coord, side);
}

void RegionGraph::link_border(TileCoord a, u8 side) {
  const TileEntry* entry_a = tiles_.find_value(tile_key(a));
  if (entry_a == nullptr) return;
  const TileCoord b = neighbour_of(a, side);
  const TileEntry* entry_b = tiles_.find_value(tile_key(b));
  if (entry_b == nullptr) return;
  const u8 other_side = opposite_side(side);

  // The join tolerance is a fraction of the tile, not of the cell size, because the graph does
  // not know the cell size and does not need to: two portals on the same border line are either
  // the same line to within float error or a different border entirely.
  const f32 join_eps = grid_tile_size_ * 0.01f;
  for (const TilePortal& pa : entry_a->portals) {
    if (pa.side != side) continue;
    for (const TilePortal& pb : entry_b->portals) {
      if (pb.side != other_side) continue;
      if (!portals_meet(pa, pb, join_eps, grid_climb_)) continue;
      if (static_cast<u32>(pa.region) >= entry_a->nodes.size() ||
          static_cast<u32>(pb.region) >= entry_b->nodes.size())
        continue;
      connect(entry_a->nodes[pa.region], entry_b->nodes[pb.region], portal_point(pa, pb));
    }
  }
}

// --- off-mesh links -------------------------------------------------------------------------

bool RegionGraph::add_link(u64 id, Vec3 from, Vec3 to) {
  remove_link(id);
  Link link;
  link.id = id;
  link.from = from;
  link.to = to;
  link.point = (from + to) * 0.5f;
  link.a = find_region(from);
  link.b = find_region(to);
  links_.push_back(link);
  if (link.a != k_invalid_region && link.b != k_invalid_region) {
    connect(link.a, link.b, link.point);
    ++revision_;
    return true;
  }
  // A link whose endpoints have no mesh yet is kept, not refused: destruction opening a passage
  // into a room whose tile has not been rebuilt is a normal race, and the link becomes live the
  // moment either tile lands.
  return false;
}

bool RegionGraph::remove_link(u64 id) {
  for (u32 i = 0; i < links_.size(); ++i) {
    if (links_[i].id != id) continue;
    if (links_[i].a != k_invalid_region && links_[i].b != k_invalid_region)
      disconnect(links_[i].a, links_[i].b);
    links_.erase(links_.begin() + i);
    ++revision_;
    return true;
  }
  return false;
}

void RegionGraph::relink_links_touching(TileCoord coord) {
  if (grid_tile_size_ == 0.0f) return;
  for (Link& link : links_) {
    const TileCoord from_tile = nav::tile_containing(link.from, grid_origin_, grid_tile_size_);
    const TileCoord to_tile = nav::tile_containing(link.to, grid_origin_, grid_tile_size_);
    if (from_tile != coord && to_tile != coord) continue;
    if (link.a != k_invalid_region && link.b != k_invalid_region) disconnect(link.a, link.b);
    link.a = find_region(link.from);
    link.b = find_region(link.to);
    if (link.a != k_invalid_region && link.b != k_invalid_region)
      connect(link.a, link.b, link.point);
  }
}

// --- reading --------------------------------------------------------------------------------

u32 RegionGraph::node_count() const noexcept { return live_nodes_; }

u32 RegionGraph::edge_count() const noexcept {
  u32 total = 0;
  for (const SmallVector<Edge, 4>& list : edges_)
    total += list.size();
  return total;
}

const RegionNode* RegionGraph::node(u32 index) const noexcept {
  if (index >= nodes_.size() || !nodes_[index].alive) return nullptr;
  return &nodes_[index];
}

u32 RegionGraph::neighbour_count(u32 index) const noexcept {
  if (index >= edges_.size()) return 0;
  return edges_[index].size();
}

u32 RegionGraph::neighbour(u32 index, u32 slot) const noexcept {
  if (index >= edges_.size() || slot >= edges_[index].size()) return k_invalid_region;
  return edges_[index][slot].node;
}

u32 RegionGraph::find_region(Vec3 point) const noexcept {
  if (grid_tile_size_ == 0.0f) return k_invalid_region;
  const TileCoord coord = nav::tile_containing(point, grid_origin_, grid_tile_size_);
  const TileEntry* entry = tiles_.find_value(tile_key(coord));
  if (entry == nullptr) return k_invalid_region;

  u32 best = k_invalid_region;
  f32 best_score = 0.0f;
  for (const u32 index : entry->nodes) {
    const RegionNode& node_ref = nodes_[index];
    if (!node_ref.alive) continue;
    // Inside the region's own box beats outside it; among those, the nearest centroid wins. A
    // point above a region and a point beside it are both "in" it as far as the coarse tier is
    // concerned, which is the whole reason the tier is cheap.
    const f32 score = node_ref.bounds.contains(point)
                          ? distance(point, node_ref.centroid)
                          : distance(point, node_ref.centroid) + grid_tile_size_;
    if (best == k_invalid_region || score < best_score) {
      best = index;
      best_score = score;
    }
  }
  return best;
}

bool RegionGraph::connected(Vec3 from, Vec3 to) const {
  f32 ignored = 0.0f;
  return estimate_distance(from, to, ignored);
}

bool RegionGraph::estimate_distance(Vec3 from, Vec3 to, f32& out) const {
  out = 0.0f;
  const u32 start = find_region(from);
  const u32 goal = find_region(to);
  if (start == k_invalid_region || goal == k_invalid_region) return false;
  if (start == goal) {
    out = distance(from, to);
    return true;
  }

  const u32 count = nodes_.size();
  if (g_score_.size() != count) {
    const u32 old = visit_stamp_.size();
    g_score_.resize(count);
    entry_point_.resize(count);
    visit_stamp_.resize(count);
    closed_stamp_.resize(count);
    for (u32 i = old; i < count; ++i) {
      visit_stamp_[i] = 0;
      closed_stamp_[i] = 0;
    }
  }
  ++visit_epoch_;
  heap_.clear();

  g_score_[start] = 0.0f;
  entry_point_[start] = from;
  visit_stamp_[start] = visit_epoch_;

  // The heap holds (f as float bits) << 32 | node. Every f is finite and non-negative, so its
  // IEEE-754 bit pattern orders the same way the number does and one integer comparison sorts
  // the queue — no comparator, no pair, no allocation.
  auto push = [&](f32 f_score, u32 node_index) {
    u32 bits = 0;
    std::memcpy(&bits, &f_score, sizeof(bits));
    heap_.push_back((static_cast<u64>(bits) << 32) | node_index);
    std::push_heap(heap_.begin(), heap_.end(), std::greater<u64>{});
  };

  push(distance(from, nodes_[start].centroid), start);

  while (!heap_.empty()) {
    std::pop_heap(heap_.begin(), heap_.end(), std::greater<u64>{});
    const u64 top = heap_.back();
    heap_.pop_back();
    const u32 current = static_cast<u32>(top & 0xFFFFFFFFu);
    // A node can sit in the heap more than once, because a cheaper way in is found after it was
    // pushed. The stamp is the "already expanded" mark that keeps the second copy free.
    if (closed_stamp_[current] == visit_epoch_) continue;
    closed_stamp_[current] = visit_epoch_;

    if (current == goal) {
      out = g_score_[current] + distance(entry_point_[current], to);
      return true;
    }

    for (const Edge& edge : edges_[current]) {
      if (closed_stamp_[edge.node] == visit_epoch_) continue;
      const f32 tentative = g_score_[current] + distance(entry_point_[current], edge.point);
      const bool seen = visit_stamp_[edge.node] == visit_epoch_;
      if (seen && g_score_[edge.node] <= tentative) continue;
      visit_stamp_[edge.node] = visit_epoch_;
      g_score_[edge.node] = tentative;
      entry_point_[edge.node] = edge.point;
      push(tentative + distance(edge.point, to), edge.node);
    }
  }
  return false;
}

RegionGraphStats RegionGraph::stats() const noexcept {
  RegionGraphStats out;
  out.node_count = live_nodes_;
  out.edge_count = edge_count();
  out.tile_count = tiles_.size();
  out.revision = revision_;
  return out;
}

}  // namespace engine::nav
