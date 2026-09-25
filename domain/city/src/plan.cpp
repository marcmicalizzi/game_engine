// The whole-island plan (docs/subsystems/city.md, "The plan"). In order, each stage from its own
// draws and reading only what the stages before it fixed:
//
//   1. the coastline (seeded, or authored) and the mountain axis
//   2. the arterial grid and its superblocks, each classed city, greenbelt or nothing
//   3. districts: seeded cells over the superblocks, kinds by the island's shape and the mix
//   4. per superblock: the district style's internal lines and blocks (organic merges, squares)
//   5. the street graph; the blocks off its main component dropped; street pins; the lane rule
//   6. parks by block (central, greenbelt, squares, waterfront, district shares) and civic blocks
//   7. per block: lots, closes and alleys by the style's layout, and each lot's archetype
//   8. pocket parks wherever a dwelling is beyond the walking distance; civic pins on lots
//   9. the final graph and the per-tile index
//
// Every coordinate is an integer centimetre and every fraction Q16; every draw is the engine's hash
// of the island's seed, a purpose and an index.
#include "grid.h"
#include "plan_internal.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/city/plan.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace engine::city {

namespace {

using grid::between;
using grid::chance;
using grid::draw;
using grid::mul_q;
using grid::pick;

// What each draw is for. Never renumbered: a changed constant moves every plan.
constexpr u64 k_purpose_coast = 0xc0a57;
constexpr u64 k_purpose_arterial = 0xa27e;
constexpr u64 k_purpose_district = 0xd157;
constexpr u64 k_purpose_spacing = 0x5ace;
constexpr u64 k_purpose_jitter = 0x717e;
constexpr u64 k_purpose_merge = 0x3e26e;
constexpr u64 k_purpose_square = 0x5c0a2e;
constexpr u64 k_purpose_lane = 0x1a4e;
constexpr u64 k_purpose_lot_width = 0x107;
constexpr u64 k_purpose_lot_depth = 0x10d;
constexpr u64 k_purpose_archetype = 0xa2c4;
constexpr u64 k_purpose_lot = 0x1075eed;

constexpr u32 k_internal_base = 1u << 20;
constexpr u32 k_max_cells = 15;  // internal cells per superblock per axis: blocks < 256
constexpr i32 k_lot_snap = 10;   // lot boundaries on a 10 cm grid

enum class CellKind : u8 { none = 0, city = 1, greenbelt = 2, central = 3 };

struct SuperCell {
  Rect rect;
  CellKind kind = CellKind::none;
  u8 coastal = 0;
  u32 district = k_no_id;
};

// A block while the plan is being made: its cells and what the rules decided of it.
struct BlockDraft {
  Block block;
  u8 kept = 0;
};

i64 isqrt(i64 v) noexcept {
  if (v <= 0) return 0;
  i64 r = static_cast<i64>(std::sqrt(static_cast<f64>(v)));
  while (r * r > v)
    --r;
  while ((r + 1) * (r + 1) <= v)
    ++r;
  return r;
}

i32 street_width(const Params& p, StreetClass cls) noexcept {
  return p.street_width_cm[static_cast<u32>(cls)];
}

bool is_dwelling(Archetype a) noexcept {
  return a == Archetype::ApartmentTower || a == Archetype::MidRiseOverShops ||
         a == Archetype::TerracedHouse || a == Archetype::DetachedHouse;
}

bool is_vehicle(StreetClass c) noexcept {
  return c == StreetClass::Arterial || c == StreetClass::Collector || c == StreetClass::Local;
}

// ---- 1. coastline --------------------------------------------------------------------------

void make_coastline(const Params& p, Vector<i32>& out) {
  out.clear();
  if (!p.coastline_cm.empty()) {
    out = p.coastline_cm;
    return;
  }
  const i64 r0 = p.radius_cm;
  i64 amplitude[7] = {};
  u32 phase[7] = {};
  for (u32 h = 2; h <= 6; ++h) {
    const u64 d = draw(p.seed, k_purpose_coast, h);
    amplitude[h] = mul_q(mul_q(r0, p.roughness_q), between(d, 32768, 65536)) / (h - 1);
    phase[h] = pick(d, 64);
  }
  // The harbour's half width in 256ths of a vertex step: its arc length over the step's.
  const i64 half_w8 =
      std::max<i64>(256, i64{p.harbour_width_cm} * 64 * 256 * 10000 / (62832 * r0 * 2));
  for (u32 k = 0; k < 64; ++k) {
    i64 r = r0;
    for (u32 h = 2; h <= 6; ++h)
      r += (amplitude[h] * grid::cos64(h * k + phase[h])) >> 14;
    const i32 dk = static_cast<i32>((k - p.harbour_step) & 63u);
    const i64 d8 = i64{std::min(dk, 64 - dk)} * 256;
    if (p.harbour_depth_cm > 0 && d8 < half_w8) {
      // A cosine bay: full depth at its axis, nothing at its edges.
      const u32 t = static_cast<u32>(d8 * 32 / half_w8);
      r -= (i64{p.harbour_depth_cm} * (16384 + grid::cos64(t))) >> 15;
    }
    r = std::max<i64>(r, r0 / 4);
    out.push_back(static_cast<i32>((r * grid::cos64(k)) >> 14));
    out.push_back(static_cast<i32>(-((r * grid::sin64(k)) >> 14)));
  }
}

bool point_in_polygon(const Vector<i32>& poly, i64 x, i64 z) noexcept {
  const u32 n = poly.size() / 2;
  bool inside = false;
  for (u32 i = 0, j = n - 1; i < n; j = i++) {
    const i64 xi = poly[2 * i];
    const i64 zi = poly[2 * i + 1];
    const i64 xj = poly[2 * j];
    const i64 zj = poly[2 * j + 1];
    if ((zi > z) == (zj > z)) continue;
    // x < xi + (z - zi) * (xj - xi) / (zj - zi), without the division: multiply through by the
    // denominator and keep its sign.
    const i64 lhs = (x - xi) * (zj - zi);
    const i64 rhs = (z - zi) * (xj - xi);
    if ((zj - zi) > 0 ? lhs < rhs : lhs > rhs) inside = !inside;
  }
  return inside;
}

// ---- 7. lots -------------------------------------------------------------------------------

// Cuts a run of `len` into lots between wmin and wmax wide, at a width drawn from `d`: the
// boundaries, on the 10 cm grid, from 0 to `len`.
void split_run(i32 len, i32 wmin, i32 wmax, u64 d, Vector<i32>& out) {
  out.clear();
  out.push_back(0);
  if (len <= 0) return;
  const i32 target = between(d, wmin, wmax);
  i32 n = std::max<i32>(1, (len + target / 2) / std::max(target, 1));
  const i32 n_min = std::max<i32>(1, (len + wmax - 1) / std::max(wmax, 1));
  const i32 n_max = std::max<i32>(1, len / std::max(wmin, 1));
  if (n_min <= n_max) n = std::clamp(n, n_min, n_max);
  n = std::min(n, 1000);
  for (i32 k = 1; k < n; ++k)
    out.push_back(grid::round_to(static_cast<i32>(i64{len} * k / n), k_lot_snap));
  out.push_back(len);
}

// The fit of an archetype on a lot, by its width along the street and its depth.
bool archetype_fits(Archetype a, i32 w, i32 d) noexcept {
  switch (a) {
    case Archetype::ApartmentTower: return w >= 3000 && d >= 2400;
    case Archetype::MidRiseOverShops: return w >= 900 && d >= 1200;
    case Archetype::Office: return w >= 1800 && d >= 1800;
    case Archetype::TerracedHouse: return w >= 500 && w < 1100 && d >= 1400;
    case Archetype::DetachedHouse: return w >= 1100 && d >= 1800;
    case Archetype::Warehouse: return w >= 2400 && d >= 3000;
    case Archetype::CivicShell: return w >= 2400 && d >= 2400;
  }
  return false;
}

struct LotMaker {
  const Params& p;
  const Style& style;
  const Block& block;
  u32 block_index;
  u32 district;
  Vector<Lot>& lots;
  Vector<Street>& interior;
  u32 first = 0;  // this block's first lot in `lots`
  Vector<i32> runs;

  u64 seed(u64 purpose, u64 index) const noexcept {
    return draw(p.seed, purpose, (u64{block.id} << 16) | index);
  }

  void emit(const Rect& r, u8 front, u32 street) {
    if (lots.size() - first >= 1024) return;  // a lot id holds 1,024 lots a block
    Lot lot;
    lot.id = block.id * 1024 + (lots.size() - first);
    lot.block = block_index;
    lot.rect = r;
    lot.street = street;
    lot.district = static_cast<u16>(district);
    lot.front = front;
    lots.push_back(lot);
  }

  // Lots along one side of a rectangle: `r` is the strip, cut across its run into widths.
  void strip(const Rect& r, bool along_x, u8 front, u32 street, u64 index) {
    const i32 len = along_x ? r.width() : r.depth();
    split_run(len, style.lot_width_min_cm, style.lot_width_max_cm, seed(k_purpose_lot_width, index),
              runs);
    for (u32 k = 0; k + 1 < runs.size(); ++k) {
      Rect lot = r;
      if (along_x) {
        lot.x0 = r.x0 + runs[k];
        lot.x1 = r.x0 + runs[k + 1];
      } else {
        lot.z0 = r.z0 + runs[k];
        lot.z1 = r.z0 + runs[k + 1];
      }
      if (lot.width() > 0 && lot.depth() > 0) emit(lot, front, street);
    }
  }

  u32 interior_id() const noexcept {
    return k_internal_base + block.superblock * 1024 + 512 + (block.id & 255u);
  }

  void rows(const Rect& b) {
    if (b.width() >= b.depth()) {
      const i32 zm = grid::round_to((b.z0 + b.z1) / 2, k_lot_snap);
      strip(Rect{b.x0, b.z0, b.x1, zm}, true, 1, block.streets[1], 0);
      strip(Rect{b.x0, zm, b.x1, b.z1}, true, 3, block.streets[3], 1);
    } else {
      const i32 xm = grid::round_to((b.x0 + b.x1) / 2, k_lot_snap);
      strip(Rect{b.x0, b.z0, xm, b.z1}, false, 2, block.streets[2], 0);
      strip(Rect{xm, b.z0, b.x1, b.z1}, false, 0, block.streets[0], 1);
    }
  }

  void perimeter(const Rect& b) {
    const i32 courtyard = std::max(2 * style.setback_rear_cm, 400);
    const i32 cap = (std::min(b.width(), b.depth()) - courtyard) / 2;
    const i32 d = std::min(
        between(seed(k_purpose_lot_depth, 0), style.lot_depth_min_cm, style.lot_depth_max_cm), cap);
    if (d < style.lot_depth_min_cm) {
      rows(b);
      return;
    }
    strip(Rect{b.x0, b.z0, b.x1, b.z0 + d}, true, 1, block.streets[1], 0);
    if (b.depth() - 2 * d >= style.lot_width_min_cm)
      strip(Rect{b.x1 - d, b.z0 + d, b.x1, b.z1 - d}, false, 0, block.streets[0], 1);
    strip(Rect{b.x0, b.z1 - d, b.x1, b.z1}, true, 3, block.streets[3], 2);
    if (b.depth() - 2 * d >= style.lot_width_min_cm)
      strip(Rect{b.x0, b.z0 + d, b.x0 + d, b.z1 - d}, false, 2, block.streets[2], 3);
  }

  // Across the whole block: lots along its long axis fronting the wider of its long sides.
  void whole(const Rect& b) {
    if (b.width() >= b.depth()) {
      const u8 front = street_width_of(1) >= street_width_of(3) ? 1 : 3;
      strip(b, true, front, block.streets[front], 0);
    } else {
      const u8 front = street_width_of(2) >= street_width_of(0) ? 2 : 0;
      strip(b, false, front, block.streets[front], 0);
    }
  }

  i32 street_width_of(u32 side) const noexcept;
  const Vector<Street>* streets = nullptr;

  void alley_rows(const Rect& b) {
    const i32 aw = street_width(p, StreetClass::Alley);
    Street alley;
    alley.id = interior_id();
    alley.cls = StreetClass::Alley;
    alley.interior = 1;
    alley.width_cm = aw;
    alley.superblock = block.superblock;
    if (b.width() >= b.depth()) {
      const i32 zm = grid::round_to((b.z0 + b.z1) / 2, 100);
      if (zm - aw / 2 - b.z0 < style.lot_depth_min_cm) {
        whole(b);
        return;
      }
      alley.along_x = 1;
      alley.line_cm = zm;
      alley.from_cm = block.rect.x0;
      alley.to_cm = block.rect.x1;
      interior.push_back(alley);
      strip(Rect{b.x0, b.z0, b.x1, zm - aw / 2}, true, 1, block.streets[1], 0);
      strip(Rect{b.x0, zm + aw / 2, b.x1, b.z1}, true, 3, block.streets[3], 1);
    } else {
      const i32 xm = grid::round_to((b.x0 + b.x1) / 2, 100);
      if (xm - aw / 2 - b.x0 < style.lot_depth_min_cm) {
        whole(b);
        return;
      }
      alley.along_x = 0;
      alley.line_cm = xm;
      alley.from_cm = block.rect.z0;
      alley.to_cm = block.rect.z1;
      interior.push_back(alley);
      strip(Rect{b.x0, b.z0, xm - aw / 2, b.z1}, false, 2, block.streets[2], 0);
      strip(Rect{xm + aw / 2, b.z0, b.x1, b.z1}, false, 0, block.streets[0], 1);
    }
  }

  // A close (a cul-de-sac) down the block's long axis from one short side, lots either side of it
  // fronting it, and a row across its head fronting the far street.
  void close_rows(const Rect& b) {
    const i32 cw = street_width(p, StreetClass::Local);
    const i32 d =
        between(seed(k_purpose_lot_depth, 0), style.lot_depth_min_cm, style.lot_depth_max_cm);
    const bool tall = b.depth() >= b.width();
    const i32 across = tall ? b.width() : b.depth();
    const i32 along = tall ? b.depth() : b.width();
    if (across < 2 * style.lot_depth_min_cm + cw || along < d + 2 * style.lot_width_min_cm) {
      rows(b);
      return;
    }
    Street close;
    close.id = interior_id();
    close.cls = StreetClass::Local;
    close.interior = 1;
    close.width_cm = cw;
    close.superblock = block.superblock;
    if (tall) {
      const i32 xm = grid::round_to((b.x0 + b.x1) / 2, 100);
      close.along_x = 0;
      close.line_cm = xm;
      close.from_cm = b.z0 + d;  // the turning head
      close.to_cm = block.rect.z1;
      interior.push_back(close);
      strip(Rect{b.x0, b.z0, b.x1, b.z0 + d}, true, 1, block.streets[1], 0);
      strip(Rect{b.x0, b.z0 + d, xm - cw / 2, b.z1}, false, 0, close.id, 1);
      strip(Rect{xm + cw / 2, b.z0 + d, b.x1, b.z1}, false, 2, close.id, 2);
    } else {
      const i32 zm = grid::round_to((b.z0 + b.z1) / 2, 100);
      close.along_x = 1;
      close.line_cm = zm;
      close.from_cm = b.x0 + d;
      close.to_cm = block.rect.x1;
      interior.push_back(close);
      strip(Rect{b.x0, b.z0, b.x0 + d, b.z1}, false, 2, block.streets[2], 0);
      strip(Rect{b.x0 + d, b.z0, b.x1, zm - cw / 2}, true, 3, close.id, 1);
      strip(Rect{b.x0 + d, zm + cw / 2, b.x1, b.z1}, true, 1, close.id, 2);
    }
  }

  void make() {
    const Rect& b = block.buildable;
    if (b.width() <= 0 || b.depth() <= 0) return;
    if (block.civic != CivicKind::None && style.kind != DistrictKind::Campus) {
      const u8 front = static_cast<u8>(
          std::max_element(std::begin(block.streets), std::end(block.streets),
                           [&](u32 a, u32 c) { return width_by_id(a) < width_by_id(c); }) -
          std::begin(block.streets));
      emit(b, front, block.streets[front]);
      return;
    }
    switch (style.lot_layout) {
      case LotLayout::Perimeter: perimeter(b); break;
      case LotLayout::Rows: rows(b); break;
      case LotLayout::Whole: whole(b); break;
      case LotLayout::AlleyRows: alley_rows(b); break;
      case LotLayout::CloseRows: close_rows(b); break;
    }
  }

  i32 width_by_id(u32 id) const noexcept;
};

const Street* find_street_in(const Vector<Street>& streets, u32 id) noexcept {
  const auto it = std::lower_bound(streets.begin(), streets.end(), id,
                                   [](const Street& s, u32 v) { return s.id < v; });
  return it != streets.end() && it->id == id ? &*it : nullptr;
}

i32 LotMaker::width_by_id(u32 id) const noexcept {
  const Street* s = streets != nullptr ? find_street_in(*streets, id) : nullptr;
  return s != nullptr ? s->width_cm : 0;
}
i32 LotMaker::street_width_of(u32 side) const noexcept { return width_by_id(block.streets[side]); }

// The archetype of a lot: by the style's weights among the archetypes that fit, from the lot's own
// draw; a lot nothing fits takes the first that does in the fallback order, and one too small for
// any is a pocket park.
void choose_archetype(const Params& p, const Style& style, Lot& lot) {
  const bool across_x = lot.front == 1 || lot.front == 3;  // the street runs along x
  const i32 w = across_x ? lot.rect.width() : lot.rect.depth();
  const i32 d = across_x ? lot.rect.depth() : lot.rect.width();
  i32 total = 0;
  for (u32 a = 0; a < k_archetypes; ++a) {
    if (a == static_cast<u32>(Archetype::CivicShell) && lot.use != LotUse::Civic) continue;
    if (archetype_fits(static_cast<Archetype>(a), w, d)) total += style.archetype_q[a];
  }
  if (total > 0) {
    i32 at =
        static_cast<i32>(pick(draw(p.seed, k_purpose_archetype, lot.id), static_cast<u32>(total)));
    for (u32 a = 0; a < k_archetypes; ++a) {
      if (a == static_cast<u32>(Archetype::CivicShell) && lot.use != LotUse::Civic) continue;
      if (!archetype_fits(static_cast<Archetype>(a), w, d)) continue;
      at -= style.archetype_q[a];
      if (at < 0) {
        lot.archetype = static_cast<Archetype>(a);
        return;
      }
    }
  }
  static constexpr Archetype k_fallback[] = {Archetype::MidRiseOverShops, Archetype::DetachedHouse,
                                             Archetype::TerracedHouse, Archetype::Warehouse};
  for (const Archetype a : k_fallback) {
    if (archetype_fits(a, w, d)) {
      lot.archetype = a;
      return;
    }
  }
  lot.use = LotUse::Park;
  lot.park = ParkKind::Pocket;
}

// ---- 5. the graph --------------------------------------------------------------------------

struct Span {
  u32 street = 0;  // index into streets
  i32 lo = 0;
  i32 hi = 0;
};

// Builds nodes and segments from the kept blocks' sides and the interior streets, and each
// street's kept extent. Streets with nothing kept get from > to.
}  // namespace

void build_graph(Plan& plan) {
  plan.nodes.clear();
  plan.segments.clear();
  Vector<Span> spans;
  Vector<Node> nodes;
  auto add_side = [&](u32 street_id, bool along_x, i32 a0, i32 a1) {
    const u32 s = plan.find_street(street_id);
    if (s == k_no_id) return;
    (void)along_x;
    spans.push_back(Span{s, std::min(a0, a1), std::max(a0, a1)});
  };
  for (const Block& b : plan.blocks) {
    const Rect& r = b.rect;
    add_side(b.streets[1], true, r.x0, r.x1);
    add_side(b.streets[3], true, r.x0, r.x1);
    add_side(b.streets[2], false, r.z0, r.z1);
    add_side(b.streets[0], false, r.z0, r.z1);
    nodes.push_back(Node{r.x0, r.z0});
    nodes.push_back(Node{r.x1, r.z0});
    nodes.push_back(Node{r.x0, r.z1});
    nodes.push_back(Node{r.x1, r.z1});
  }
  for (u32 s = 0; s < plan.streets.size(); ++s) {
    const Street& st = plan.streets[s];
    if (st.interior == 0) continue;
    spans.push_back(Span{s, st.from_cm, st.to_cm});
    nodes.push_back(st.along_x ? Node{st.from_cm, st.line_cm} : Node{st.line_cm, st.from_cm});
    nodes.push_back(st.along_x ? Node{st.to_cm, st.line_cm} : Node{st.line_cm, st.to_cm});
  }
  std::sort(nodes.begin(), nodes.end(),
            [](const Node& a, const Node& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
  for (const Node& n : nodes) {
    if (plan.nodes.empty() || plan.nodes.back().x != n.x || plan.nodes.back().z != n.z)
      plan.nodes.push_back(n);
  }
  // The same nodes by (x, z), for the lines along z.
  Vector<u32> by_x(plan.nodes.size());
  for (u32 i = 0; i < by_x.size(); ++i)
    by_x[i] = i;
  std::sort(by_x.begin(), by_x.end(), [&](u32 a, u32 b) {
    const Node& na = plan.nodes[a];
    const Node& nb = plan.nodes[b];
    return na.x != nb.x ? na.x < nb.x : na.z < nb.z;
  });

  std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
    return a.street != b.street ? a.street < b.street : a.lo < b.lo;
  });
  for (Street& st : plan.streets) {
    st.from_cm = 1;
    st.to_cm = 0;
  }
  u32 i = 0;
  while (i < spans.size()) {
    Span merged = spans[i];
    u32 j = i + 1;
    while (j < spans.size() && spans[j].street == merged.street && spans[j].lo <= merged.hi) {
      merged.hi = std::max(merged.hi, spans[j].hi);
      ++j;
    }
    i = j;
    Street& st = plan.streets[merged.street];
    if (st.from_cm > st.to_cm) {
      st.from_cm = merged.lo;
      st.to_cm = merged.hi;
    } else {
      st.from_cm = std::min(st.from_cm, merged.lo);
      st.to_cm = std::max(st.to_cm, merged.hi);
    }
    // The nodes on this stretch of the line, in order along it.
    u32 prev = k_no_id;
    if (st.along_x) {
      auto it = std::lower_bound(
          plan.nodes.begin(), plan.nodes.end(), Node{merged.lo, st.line_cm},
          [](const Node& a, const Node& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
      for (; it != plan.nodes.end() && it->z == st.line_cm && it->x <= merged.hi; ++it) {
        const u32 n = static_cast<u32>(it - plan.nodes.begin());
        if (prev != k_no_id)
          plan.segments.push_back(Segment{merged.street, prev, n, st.cls, {0, 0, 0}});
        prev = n;
      }
    } else {
      auto it = std::lower_bound(by_x.begin(), by_x.end(), Node{st.line_cm, merged.lo},
                                 [&](u32 a, const Node& b) {
                                   const Node& na = plan.nodes[a];
                                   return na.x != b.x ? na.x < b.x : na.z < b.z;
                                 });
      for (; it != by_x.end() && plan.nodes[*it].x == st.line_cm && plan.nodes[*it].z <= merged.hi;
           ++it) {
        if (prev != k_no_id)
          plan.segments.push_back(Segment{merged.street, prev, *it, st.cls, {0, 0, 0}});
        prev = *it;
      }
    }
  }
}

namespace {

u32 find_root(Vector<u32>& parent, u32 x) noexcept {
  while (parent[x] != x) {
    parent[x] = parent[parent[x]];
    x = parent[x];
  }
  return x;
}

void unite(Vector<u32>& parent, u32 a, u32 b) noexcept {
  a = find_root(parent, a);
  b = find_root(parent, b);
  if (a != b) parent[std::max(a, b)] = std::min(a, b);
}

u32 find_node(const Plan& plan, i32 x, i32 z) noexcept {
  const auto it = std::lower_bound(
      plan.nodes.begin(), plan.nodes.end(), Node{x, z},
      [](const Node& a, const Node& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
  return it != plan.nodes.end() && it->x == x && it->z == z
             ? static_cast<u32>(it - plan.nodes.begin())
             : k_no_id;
}

}  // namespace

// ---- lookups --------------------------------------------------------------------------------

u32 Plan::find_block(u32 id) const noexcept {
  const auto it = std::lower_bound(blocks.begin(), blocks.end(), id,
                                   [](const Block& b, u32 v) { return b.id < v; });
  return it != blocks.end() && it->id == id ? static_cast<u32>(it - blocks.begin()) : k_no_id;
}

u32 Plan::find_lot(u32 id) const noexcept {
  const auto it =
      std::lower_bound(lots.begin(), lots.end(), id, [](const Lot& l, u32 v) { return l.id < v; });
  return it != lots.end() && it->id == id ? static_cast<u32>(it - lots.begin()) : k_no_id;
}

u32 Plan::find_street(u32 id) const noexcept {
  const Street* s = find_street_in(streets, id);
  return s != nullptr ? static_cast<u32>(s - streets.data()) : k_no_id;
}

bool on_land(const Plan& plan, i64 x, i64 z) noexcept {
  return point_in_polygon(plan.coastline, x, z);
}

i64 along_mountain(const Plan& plan, i64 x, i64 z) noexcept {
  return (x * plan.mountain_ux + z * plan.mountain_uz) >> 14;
}

// ---- the plan -------------------------------------------------------------------------------

bool generate_plan(const Params& params, Plan& out, std::string& error, jobs::JobSystem* jobs) {
  out = Plan{};
  out.params = params;
  out.key = plan_key(params.source);
  const Params& p = out.params;
  const u64 seed = p.seed;

  // 1. The coastline and the mountain.
  make_coastline(p, out.coastline);
  if (out.coastline.size() < 6) {
    error = "the coastline has fewer than three points";
    return false;
  }
  out.mountain_ux = grid::cos64(p.mountain_step);
  out.mountain_uz = -grid::sin64(p.mountain_step);
  i64 lo_x = out.coastline[0], hi_x = lo_x, lo_z = out.coastline[1], hi_z = lo_z;
  i64 lo_m = along_mountain(out, lo_x, lo_z), hi_m = lo_m;
  for (u32 i = 0; i < out.coastline.size(); i += 2) {
    const i64 x = out.coastline[i];
    const i64 z = out.coastline[i + 1];
    lo_x = std::min(lo_x, x);
    hi_x = std::max(hi_x, x);
    lo_z = std::min(lo_z, z);
    hi_z = std::max(hi_z, z);
    const i64 m = along_mountain(out, x, z);
    lo_m = std::min(lo_m, m);
    hi_m = std::max(hi_m, m);
  }
  out.mountain_start_cm = static_cast<i32>(hi_m - mul_q(hi_m - lo_m, p.mountain_share_q));
  out.city_limit_cm = out.mountain_start_cm - p.greenbelt_cm;
  // The downtown anchor: the city's centroid pulled toward the harbour (below, once the city's
  // superblocks are known). The harbour point: the coast's direction at the harbour step, at the
  // mean radius less the bay.
  const i64 harbour_r = std::max<i64>(p.radius_cm - p.harbour_depth_cm, p.radius_cm / 4);
  const i64 harbour_x = (harbour_r * grid::cos64(p.harbour_step)) >> 14;
  const i64 harbour_z = -((harbour_r * grid::sin64(p.harbour_step)) >> 14);

  // 2. The arterial grid, anchored at the island's centre so the same parameters give the same
  // lines whatever the coastline, with a seeded jitter on each line.
  const i32 spacing = p.arterial_spacing_cm;
  const i32 module = p.rules.module_cm;
  auto lines = [&](i64 lo, i64 hi, u64 axis, Vector<i32>& v) {
    const i32 first = grid::floor_div(lo, spacing);
    const i32 last = grid::floor_div(hi, spacing) + 1;
    for (i32 i = first; i <= last; ++i) {
      const u64 d = draw(seed, k_purpose_arterial, (axis << 32) | static_cast<u32>(i));
      const i32 jitter =
          static_cast<i32>(mul_q(mul_q(spacing, p.arterial_jitter_q), between(d, -65536, 65536)));
      v.push_back(grid::round_to(i * spacing + jitter, module));
    }
  };
  lines(lo_x, hi_x, 0, out.arterials_x);
  lines(lo_z, hi_z, 1, out.arterials_z);
  const u32 nxs = out.arterials_x.size() - 1;  // superblocks along x
  const u32 nzs = out.arterials_z.size() - 1;
  if (u64{nxs} * nzs > 4000000u) {
    error = "the island has more than four million superblocks";
    return false;
  }

  Vector<SuperCell> cells(nxs * nzs);
  i64 city_area = 0;
  i64 sum_x = 0, sum_z = 0, city_cells = 0;
  for (u32 j = 0; j < nzs; ++j) {
    for (u32 i = 0; i < nxs; ++i) {
      SuperCell& c = cells[j * nxs + i];
      c.rect = Rect{out.arterials_x[i], out.arterials_z[j], out.arterials_x[i + 1],
                    out.arterials_z[j + 1]};
      u32 land = 0, city = 0, green = 0;
      for (u32 s = 0; s < 9; ++s) {
        const i64 x = c.rect.x0 + i64{c.rect.width()} * (s % 3) / 2;
        const i64 z = c.rect.z0 + i64{c.rect.depth()} * (s / 3) / 2;
        if (!on_land(out, x, z)) continue;
        ++land;
        const i64 m = along_mountain(out, x, z);
        if (m < out.city_limit_cm)
          ++city;
        else if (m < out.mountain_start_cm)
          ++green;
      }
      c.coastal = land < 9 ? 1 : 0;
      if (city > 0) {
        c.kind = CellKind::city;
        city_area += c.rect.area();
        sum_x += (c.rect.x0 + c.rect.x1) / 2;
        sum_z += (c.rect.z0 + c.rect.z1) / 2;
        ++city_cells;
      } else if (green > 0 && land == 9) {
        c.kind = CellKind::greenbelt;
      }
    }
  }
  if (city_cells == 0) {
    error = "the island has no city: every superblock is water, mountain or greenbelt";
    return false;
  }
  const i64 anchor_x = sum_x / city_cells + (harbour_x - sum_x / city_cells) * 3 / 10;
  const i64 anchor_z = sum_z / city_cells + (harbour_z - sum_z / city_cells) * 3 / 10;
  auto center_of = [](const Rect& r) {
    return std::pair<i64, i64>{(i64{r.x0} + r.x1) / 2, (i64{r.z0} + r.z1) / 2};
  };
  auto d2 = [](i64 ax, i64 az, i64 bx, i64 bz) {
    return (ax - bx) * (ax - bx) + (az - bz) * (az - bz);
  };

  // 3. Districts: seed cells in the order the seed ranks them, each at least 0.6 of a district's
  // extent from the ones taken, then every city and greenbelt superblock to its nearest seed.
  {
    const i64 size = p.district_size_cm;
    const i64 wanted = std::max<i64>(1, (city_area + size * size / 2) / (size * size));
    Vector<std::pair<u64, u32>> ranked;
    for (u32 s = 0; s < cells.size(); ++s) {
      if (cells[s].kind == CellKind::city) ranked.push_back({draw(seed, k_purpose_district, s), s});
    }
    std::sort(ranked.begin(), ranked.end());
    const i64 gap2 = (size * 6 / 10) * (size * 6 / 10);
    Vector<u32> seeds;
    for (const auto& [rank, s] : ranked) {
      (void)rank;
      if (static_cast<i64>(seeds.size()) >= wanted) break;
      const auto [x, z] = center_of(cells[s].rect);
      bool far = true;
      for (const u32 t : seeds) {
        const auto [tx, tz] = center_of(cells[t].rect);
        far = far && d2(x, z, tx, tz) >= gap2;
      }
      if (far) seeds.push_back(s);
    }
    for (u32 k = 0; k < seeds.size(); ++k) {
      District d;
      d.id = k;
      const auto [x, z] = center_of(cells[seeds[k]].rect);
      d.cx = static_cast<i32>(x);
      d.cz = static_cast<i32>(z);
      out.districts.push_back(d);
    }
    for (SuperCell& c : cells) {
      if (c.kind == CellKind::none) continue;
      const auto [x, z] = center_of(c.rect);
      i64 best = -1;
      for (u32 k = 0; k < out.districts.size(); ++k) {
        const i64 dd = d2(x, z, out.districts[k].cx, out.districts[k].cz);
        if (best < 0 || dd < best) {
          best = dd;
          c.district = k;
        }
      }
      if (c.kind == CellKind::city) ++out.districts[c.district].superblocks;
    }
  }

  // Kinds: in a fixed order, each kind takes the unassigned districts it scores best on while
  // that brings its area nearer its share of the city; downtown takes at least one, and whatever
  // is left is residential.
  {
    const u32 nd = out.districts.size();
    Vector<i64> area(nd, i64{0});
    Vector<u32> coast(nd, 0u);
    for (const SuperCell& c : cells) {
      if (c.kind != CellKind::city) continue;
      area[c.district] += c.rect.area();
      coast[c.district] += c.coastal;
    }
    Vector<u8> taken(nd, u8{0});
    static constexpr DistrictKind k_order[] = {DistrictKind::Downtown, DistrictKind::Industrial,
                                               DistrictKind::OldTown,  DistrictKind::Waterfront,
                                               DistrictKind::Campus,   DistrictKind::MixedUse};
    for (const DistrictKind kind : k_order) {
      const i64 target = mul_q(city_area, p.district_share_q[static_cast<u32>(kind)]);
      if (target <= 0 && kind != DistrictKind::Downtown) continue;
      Vector<std::pair<i64, u32>> order;
      for (u32 k = 0; k < nd; ++k) {
        if (taken[k] != 0) continue;
        const District& d = out.districts[k];
        const i64 dc = isqrt(d2(d.cx, d.cz, anchor_x, anchor_z));
        const i64 dh = isqrt(d2(d.cx, d.cz, harbour_x, harbour_z));
        i64 score = dc;
        switch (kind) {
          case DistrictKind::Industrial:
            if (coast[k] == 0) continue;
            score = dh;
            break;
          case DistrictKind::OldTown: score = dh + dc; break;
          case DistrictKind::Waterfront:
            if (coast[k] == 0) continue;
            score = dc - i64{coast[k]} * 1000000;
            break;
          default: break;
        }
        order.push_back({score, k});
      }
      std::sort(order.begin(), order.end());
      i64 assigned = 0;
      for (const auto& [score, k] : order) {
        (void)score;
        const bool first = assigned == 0 && kind == DistrictKind::Downtown;
        const i64 before = assigned > target ? assigned - target : target - assigned;
        const i64 next = assigned + area[k];
        const i64 after = next > target ? next - target : target - next;
        if (!first && after >= before) break;
        out.districts[k].kind = kind;
        taken[k] = 1;
        assigned = next;
      }
    }
    for (u32 k = 0; k < nd; ++k) {
      if (taken[k] == 0) out.districts[k].kind = DistrictKind::Residential;
    }
    for (const Pin& pin : p.pins) {
      if (pin.what != OverrideKind::District) continue;
      if (pin.id >= nd) {
        error = "overrides: no district " + std::to_string(pin.id) + " (the plan has " +
                std::to_string(nd) + ")";
        return false;
      }
      out.districts[pin.id].kind = pin.district_kind;
      out.districts[pin.id].pinned = 1;
    }
  }

  // The central park: the superblocks nearest the anchor that are not downtown, not on the coast
  // and not industry, as many as its share of the city asks for (at least one if it asks at all).
  if (p.central_share_q > 0) {
    const i64 mean = city_area / city_cells;
    const i64 want = std::max<i64>(1, (mul_q(city_area, p.central_share_q) + mean / 2) / mean);
    Vector<std::pair<i64, u32>> order;
    for (u32 s = 0; s < cells.size(); ++s) {
      const SuperCell& c = cells[s];
      if (c.kind != CellKind::city || c.coastal != 0) continue;
      const DistrictKind k = out.districts[c.district].kind;
      if (k == DistrictKind::Downtown || k == DistrictKind::Industrial) continue;
      const auto [x, z] = center_of(c.rect);
      order.push_back({d2(x, z, anchor_x, anchor_z), s});
    }
    std::sort(order.begin(), order.end());
    for (u32 k = 0; k < order.size() && static_cast<i64>(k) < want; ++k)
      cells[order[k].second].kind = CellKind::central;
  }

  // 4. Streets and blocks, superblock by superblock.
  const u32 arterial_x_base = out.arterials_z.size();
  for (u32 j = 0; j < out.arterials_z.size(); ++j) {
    Street s;
    s.id = j;
    s.cls = StreetClass::Arterial;
    s.along_x = 1;
    s.line_cm = out.arterials_z[j];
    s.width_cm = street_width(p, StreetClass::Arterial);
    out.streets.push_back(s);
  }
  for (u32 i = 0; i < out.arterials_x.size(); ++i) {
    Street s;
    s.id = arterial_x_base + i;
    s.cls = StreetClass::Arterial;
    s.along_x = 0;
    s.line_cm = out.arterials_x[i];
    s.width_cm = street_width(p, StreetClass::Arterial);
    out.streets.push_back(s);
  }

  // A block is kept when its corners and centre are on land: in the city when every corner is short
  // of the city limit, in the greenbelt when one is past it and one short of the mountain.
  enum : u8 { k_drop = 0, k_city = 1, k_green = 2 };
  auto block_class = [&](const Rect& r) -> u8 {
    const i64 xs3[3] = {r.x0, (i64{r.x0} + r.x1) / 2, r.x1};
    const i64 zs3[3] = {r.z0, (i64{r.z0} + r.z1) / 2, r.z1};
    if (!on_land(out, xs3[1], zs3[1])) return k_drop;
    i64 lo = 0, hi = 0;
    for (u32 a = 0; a < 3; a += 2) {
      for (u32 b = 0; b < 3; b += 2) {
        if (!on_land(out, xs3[a], zs3[b])) return k_drop;
        const i64 m = along_mountain(out, xs3[a], zs3[b]);
        lo = a + b == 0 ? m : std::min(lo, m);
        hi = a + b == 0 ? m : std::max(hi, m);
      }
    }
    if (hi < out.city_limit_cm) return k_city;
    return lo < out.mountain_start_cm ? k_green : k_drop;
  };

  Vector<i32> xs, zs;
  for (u32 sj = 0; sj < nzs; ++sj) {
    for (u32 si = 0; si < nxs; ++si) {
      const u32 sb = sj * nxs + si;
      const SuperCell& cell = cells[sb];
      if (cell.kind == CellKind::none) continue;
      const u32 sb_base = k_internal_base + sb * 1024;
      const u32 north = sj;
      const u32 south = sj + 1;
      const u32 west = arterial_x_base + si;
      const u32 east = arterial_x_base + si + 1;
      if (cell.kind != CellKind::city) {
        // The central park and the greenbelt: the whole superblock one park block.
        BlockDraft draft;
        Block& b = draft.block;
        b.id = sb * 256;
        b.district = cell.district;
        b.superblock = sb;
        b.rect = cell.rect;
        b.park = cell.kind == CellKind::central ? ParkKind::Central : ParkKind::Greenbelt;
        b.streets[0] = east;
        b.streets[1] = north;
        b.streets[2] = west;
        b.streets[3] = south;
        const u8 cls = block_class(b.rect);
        if (cls == k_green) b.park = ParkKind::Greenbelt;
        if (cls != k_drop) out.blocks.push_back(b);
        continue;
      }
      const District& district = out.districts[cell.district];
      const Style& style = p.styles[static_cast<u32>(district.kind)];
      const Rect& r = cell.rect;
      // The internal lines: a cell count per axis that keeps each block inside the style's range
      // where it can, at a target drawn per superblock; a close pattern's across-axis cells are
      // two lots deep and a close wide.
      auto cells_along = [&](i32 len, u64 axis) {
        i32 lo = style.block_min_cm, hi = style.block_max_cm;
        if (style.pattern == StreetPattern::Closes && axis == 0) {
          const i32 across = 2 * (style.lot_depth_min_cm + style.lot_depth_max_cm) / 2 +
                             2 * street_width(p, StreetClass::Local);
          lo = std::max(lo, across * 9 / 10);
          hi = std::max(lo, across * 11 / 10);
        } else if (style.pattern == StreetPattern::Closes) {
          lo = std::max(lo, hi * 2 / 3);
        }
        const i32 target = between(draw(seed, k_purpose_spacing, (u64{sb} << 1) | axis), lo, hi);
        i32 n = std::max<i32>(1, (len + target / 2) / std::max(target, 1));
        const i32 n_min = (len + hi - 1) / std::max(hi, 1);
        const i32 n_max = len / std::max(lo, 1);
        if (n_min <= n_max) n = std::clamp(n, std::max(n_min, 1), std::max(n_max, 1));
        return std::clamp<i32>(n, 1, static_cast<i32>(k_max_cells));
      };
      const i32 nx = cells_along(r.width(), 0);
      const i32 nz = cells_along(r.depth(), 1);
      xs.clear();
      zs.clear();
      for (i32 k = 0; k <= nx; ++k)
        xs.push_back(
            k == 0    ? r.x0
            : k == nx ? r.x1
                      : grid::round_to(r.x0 + static_cast<i32>(i64{r.width()} * k / nx), module));
      for (i32 k = 0; k <= nz; ++k)
        zs.push_back(
            k == 0    ? r.z0
            : k == nz ? r.z1
                      : grid::round_to(r.z0 + static_cast<i32>(i64{r.depth()} * k / nz), module));
      if (style.pattern == StreetPattern::Organic) {
        // Jittered lines: each internal line moved by up to a fifth of its cell, and kept inside
        // the style's block range of both its neighbours.
        auto jitter = [&](Vector<i32>& v, u64 axis) {
          for (u32 k = 1; k + 1 < v.size(); ++k) {
            const i32 cell_len = (v[k + 1] - v[k - 1]) / 2;
            const u64 d = draw(seed, k_purpose_jitter, (u64{sb} << 8) | (axis << 7) | k);
            const i32 moved =
                grid::round_to(v[k] + between(d, -cell_len / 5, cell_len / 5), module);
            if (moved - v[k - 1] >= style.block_min_cm && v[k + 1] - moved >= style.block_min_cm &&
                moved - v[k - 1] <= style.block_max_cm && v[k + 1] - moved <= style.block_max_cm)
              v[k] = moved;
          }
        };
        jitter(xs, 0);
        jitter(zs, 1);
      }
      // Lines along x (constant z) are internal streets m = 1..nz-1; along z, 256 + k.
      auto line_class = [&](i32 k, i32 n, u64 axis) {
        if (style.pattern == StreetPattern::Wide) return StreetClass::Collector;
        if (n >= 3 && k == n / 2) return StreetClass::Collector;
        if (style.pattern == StreetPattern::Organic &&
            chance(draw(seed, k_purpose_lane, (u64{sb} << 8) | (axis << 7) | u64(k)), style.lane_q))
          return StreetClass::Pedestrian;
        return StreetClass::Local;
      };
      for (i32 m = 1; m < nz; ++m) {
        Street s;
        s.id = sb_base + static_cast<u32>(m);
        s.cls = line_class(m, nz, 1);
        s.along_x = 1;
        s.line_cm = zs[m];
        s.width_cm = street_width(p, s.cls);
        s.superblock = sb;
        out.streets.push_back(s);
      }
      for (i32 k = 1; k < nx; ++k) {
        Street s;
        s.id = sb_base + 256 + static_cast<u32>(k);
        s.cls = line_class(k, nx, 0);
        s.along_x = 0;
        s.line_cm = xs[k];
        s.width_cm = street_width(p, s.cls);
        s.superblock = sb;
        out.streets.push_back(s);
      }
      // Cells to blocks: organic merges join a cell with its +x or +z neighbour when neither is
      // merged yet and the merged block stays inside the style's range; squares take a cell at a
      // junction.
      Vector<i32> owner(static_cast<u32>(nx * nz), -1);
      Vector<u8> square(static_cast<u32>(nx * nz), u8{0});
      for (i32 m = 0; m < nz; ++m) {
        for (i32 k = 0; k < nx; ++k) {
          const i32 c = m * nx + k;
          if (owner[c] >= 0) continue;
          owner[c] = c;
          if (style.pattern != StreetPattern::Organic) continue;
          const u64 d = draw(seed, k_purpose_merge, (u64{sb} << 16) | static_cast<u32>(c));
          if (!chance(d, style.merge_q)) continue;
          const bool along_x = (d >> 20 & 1u) != 0;
          if (along_x && k + 1 < nx && owner[c + 1] < 0 &&
              xs[k + 2] - xs[k] <= style.block_max_cm) {
            owner[c + 1] = c;
          } else if (!along_x && m + 1 < nz && owner[c + nx] < 0 &&
                     zs[m + 2] - zs[m] <= style.block_max_cm) {
            owner[c + nx] = c;
          }
        }
      }
      if (style.pattern == StreetPattern::Organic) {
        for (i32 m = 1; m < nz; ++m) {
          for (i32 k = 1; k < nx; ++k) {
            const i32 c = m * nx + k;
            if (owner[c] != c || (k + 1 < nx && owner[c + 1] == c) ||
                (m + 1 < nz && owner[c + nx] == c))
              continue;
            const u64 d = draw(seed, k_purpose_square, (u64{sb} << 16) | static_cast<u32>(c));
            if (chance(d, style.square_q)) square[c] = 1;
          }
        }
      }
      for (i32 m = 0; m < nz; ++m) {
        for (i32 k = 0; k < nx; ++k) {
          const i32 c = m * nx + k;
          if (owner[c] != c) continue;
          i32 k1 = k + 1, m1 = m + 1;
          if (k + 1 < nx && owner[c + 1] == c) k1 = k + 2;
          if (m + 1 < nz && owner[c + nx] == c) m1 = m + 2;
          Block b;
          b.id = sb * 256 + static_cast<u32>(c);
          b.district = cell.district;
          b.superblock = sb;
          b.rect = Rect{xs[k], zs[m], xs[k1], zs[m1]};
          b.streets[1] = m == 0 ? north : sb_base + static_cast<u32>(m);
          b.streets[3] = m1 == nz ? south : sb_base + static_cast<u32>(m1);
          b.streets[2] = k == 0 ? west : sb_base + 256 + static_cast<u32>(k);
          b.streets[0] = k1 == nx ? east : sb_base + 256 + static_cast<u32>(k1);
          if (square[c] != 0) b.park = ParkKind::Square;
          const u8 cls = block_class(b.rect);
          if (cls == k_green) b.park = ParkKind::Greenbelt;
          if (cls != k_drop) out.blocks.push_back(b);
        }
      }
    }
  }
  std::sort(out.streets.begin(), out.streets.end(),
            [](const Street& a, const Street& b) { return a.id < b.id; });
  std::sort(out.blocks.begin(), out.blocks.end(),
            [](const Block& a, const Block& b) { return a.id < b.id; });

  // 5. The graph, and only the blocks on its main component: a block cut off by the coast or the
  // mountain from the rest of the streets is dropped rather than left unreachable.
  build_graph(out);
  {
    Vector<u32> parent(out.nodes.size());
    for (u32 i = 0; i < parent.size(); ++i)
      parent[i] = i;
    for (const Segment& s : out.segments)
      unite(parent, s.a, s.b);
    Vector<u32> size(out.nodes.size(), 0u);
    for (const Segment& s : out.segments)
      ++size[find_root(parent, s.a)];
    u32 main = 0;
    for (u32 i = 0; i < size.size(); ++i) {
      if (size[i] > size[main]) main = i;
    }
    Vector<Block> kept;
    for (const Block& b : out.blocks) {
      const u32 n = find_node(out, b.rect.x0, b.rect.z0);
      if (n != k_no_id && find_root(parent, n) == main) kept.push_back(b);
    }
    out.blocks = std::move(kept);
  }
  for (const Pin& pin : p.pins) {
    if (pin.what != OverrideKind::Street) continue;
    const u32 s = out.find_street(pin.id);
    if (s == k_no_id) {
      error = "overrides: no street " + std::to_string(pin.id);
      return false;
    }
    out.streets[s].cls = pin.street_class;
    out.streets[s].width_cm = street_width(p, pin.street_class);
    out.streets[s].pinned = 1;
  }
  build_graph(out);
  // The lane rule: a pedestrian lane never ends an arterial. Where an arterial's last segment meets
  // only a lane, the lane is a local street.
  {
    const u32 nn = out.nodes.size();
    Vector<u32> vehicle(nn, 0u);
    Vector<u32> start(nn + 1, 0u);
    for (const Segment& s : out.segments) {
      if (is_vehicle(out.streets[s.street].cls)) {
        ++vehicle[s.a];
        ++vehicle[s.b];
      }
      ++start[s.a + 1];
      ++start[s.b + 1];
    }
    for (u32 n = 0; n < nn; ++n)
      start[n + 1] += start[n];
    Vector<u32> incident(start[nn], 0u);
    Vector<u32> fill(start.begin(), start.end() - 1);
    for (u32 k = 0; k < out.segments.size(); ++k) {
      incident[fill[out.segments[k].a]++] = k;
      incident[fill[out.segments[k].b]++] = k;
    }
    bool changed = false;
    for (const Segment& s : out.segments) {
      if (out.streets[s.street].cls != StreetClass::Arterial) continue;
      for (const u32 n : {s.a, s.b}) {
        if (vehicle[n] != 1) continue;
        for (u32 e = start[n]; e < start[n + 1]; ++e) {
          Street& st = out.streets[out.segments[incident[e]].street];
          if (st.cls == StreetClass::Pedestrian && st.pinned == 0) {
            st.cls = StreetClass::Local;
            st.width_cm = street_width(p, StreetClass::Local);
            changed = true;
          }
        }
      }
    }
    if (changed) build_graph(out);
  }

  // The buildable rectangles: each side in by half its street.
  auto half = [&](u32 id) {
    const u32 s = out.find_street(id);
    return s != k_no_id ? out.streets[s].width_cm / 2 : 0;
  };
  for (Block& b : out.blocks) {
    b.buildable = Rect{b.rect.x0 + half(b.streets[2]), b.rect.z0 + half(b.streets[1]),
                       b.rect.x1 - half(b.streets[0]), b.rect.z1 - half(b.streets[3])};
  }

  // 6. Parks by block. Authored park pins first; then the waterfront's coastal blocks; then each
  // district's share, from the blocks nearest its centre.
  for (const Pin& pin : p.pins) {
    if (pin.what != OverrideKind::Park) continue;
    const u32 b = out.find_block(pin.id);
    if (b == k_no_id) {
      error = "overrides: no block " + std::to_string(pin.id);
      return false;
    }
    out.blocks[b].park = pin.park;
    out.blocks[b].pin_park = 1;
  }
  for (const Pin& pin : p.pins) {
    if (pin.what != OverrideKind::Civic) continue;
    const u32 b = out.find_block(pin.id / 1024);
    if (b == k_no_id) {
      error = "overrides: no lot " + std::to_string(pin.id) + " (its block " +
              std::to_string(pin.id / 1024) + " is not in the plan)";
      return false;
    }
    out.blocks[b].pin_civic = 1;
  }
  // The waterfront: a waterfront block with water a block's own extent beyond one of its sides.
  for (Block& b : out.blocks) {
    if (b.pin_park != 0 || b.park != ParkKind::None) continue;
    if (out.districts[b.district].kind != DistrictKind::Waterfront) continue;
    const i64 cx = (i64{b.rect.x0} + b.rect.x1) / 2;
    const i64 cz = (i64{b.rect.z0} + b.rect.z1) / 2;
    const i64 w = b.rect.width();
    const i64 d = b.rect.depth();
    if (!on_land(out, b.rect.x1 + w, cz) || !on_land(out, b.rect.x0 - w, cz) ||
        !on_land(out, cx, b.rect.z1 + d) || !on_land(out, cx, b.rect.z0 - d))
      b.park = ParkKind::Waterfront;
  }
  {
    const u32 nd = out.districts.size();
    Vector<i64> area(nd, i64{0}), parks(nd, i64{0});
    for (const Block& b : out.blocks) {
      area[b.district] += b.rect.area();
      if (b.park != ParkKind::None) parks[b.district] += b.rect.area();
    }
    for (u32 k = 0; k < nd; ++k) {
      out.districts[k].area_cm2 = area[k];
      const Style& style = p.styles[static_cast<u32>(out.districts[k].kind)];
      const i64 want = mul_q(area[k], style.park_share_q);
      if (parks[k] >= want) continue;
      Vector<std::pair<i64, u32>> order;
      for (u32 b = 0; b < out.blocks.size(); ++b) {
        const Block& blk = out.blocks[b];
        if (blk.district != k || blk.park != ParkKind::None || blk.pin_park != 0 ||
            blk.pin_civic != 0)
          continue;
        const auto [x, z] = center_of(blk.rect);
        order.push_back({d2(x, z, out.districts[k].cx, out.districts[k].cz), b});
      }
      std::sort(order.begin(), order.end());
      for (const auto& [dist, b] : order) {
        (void)dist;
        if (parks[k] >= want) break;
        out.blocks[b].park = ParkKind::District;
        parks[k] += out.blocks[b].rect.area();
      }
    }
  }

  // Civic blocks: hospitals in mixed use, stations downtown, government in the old town, each
  // nearest the anchor first and the next at least a kilometre from the ones taken; universities
  // are the campus districts' lots, or blocks of mixed use where there is no campus.
  bool has_campus = false;
  for (const District& d : out.districts)
    has_campus = has_campus || d.kind == DistrictKind::Campus;
  struct CivicRule {
    CivicKind kind;
    DistrictKind first;
    DistrictKind second;
  };
  static constexpr CivicRule k_civic_rules[] = {
      {CivicKind::Hospital, DistrictKind::MixedUse, DistrictKind::Residential},
      {CivicKind::Station, DistrictKind::Downtown, DistrictKind::MixedUse},
      {CivicKind::Government, DistrictKind::OldTown, DistrictKind::Downtown},
      {CivicKind::University, DistrictKind::MixedUse, DistrictKind::Residential},
  };
  Vector<std::pair<i64, i64>> chosen;
  for (const CivicRule& rule : k_civic_rules) {
    if (rule.kind == CivicKind::University && has_campus) continue;
    const u32 want = p.civic_count[static_cast<u32>(rule.kind)];
    chosen.clear();
    for (const DistrictKind kind : {rule.first, rule.second}) {
      Vector<std::pair<i64, u32>> order;
      for (u32 b = 0; b < out.blocks.size(); ++b) {
        const Block& blk = out.blocks[b];
        if (blk.park != ParkKind::None || blk.civic != CivicKind::None || blk.pin_civic != 0 ||
            out.districts[blk.district].kind != kind)
          continue;
        if (blk.buildable.width() < 4000 || blk.buildable.depth() < 4000) continue;
        const auto [x, z] = center_of(blk.rect);
        order.push_back({d2(x, z, anchor_x, anchor_z), b});
      }
      std::sort(order.begin(), order.end());
      for (const auto& [dist, b] : order) {
        (void)dist;
        if (chosen.size() >= want) break;
        const auto [x, z] = center_of(out.blocks[b].rect);
        bool far = true;
        for (const auto& [cx, cz] : chosen)
          far = far && d2(x, z, cx, cz) >= i64{100000} * 100000;
        if (!far) continue;
        out.blocks[b].civic = rule.kind;
        chosen.push_back({x, z});
      }
    }
  }
  for (Block& b : out.blocks) {
    if (b.park == ParkKind::None && b.civic == CivicKind::None &&
        out.districts[b.district].kind == DistrictKind::Campus)
      b.civic = CivicKind::University;
  }

  // 7. Lots, block by block, on the pool when there is one: each block's lots and interior streets
  // are a function of the block, and the runs are joined in block order.
  {
    const u32 nb = out.blocks.size();
    struct Part {
      Vector<Lot> lots;
      Vector<Street> interior;
    };
    const u32 workers = jobs != nullptr ? jobs->worker_count(jobs::Pool::Performance) + 1 : 1;
    const u32 runs = std::max<u32>(1, std::min(nb, workers * 4));
    Vector<Part> parts(runs);
    auto run = [&](u32 r) {
      const u32 from = static_cast<u32>(u64{nb} * r / runs);
      const u32 to = static_cast<u32>(u64{nb} * (r + 1) / runs);
      for (u32 b = from; b < to; ++b) {
        const Block& blk = out.blocks[b];
        if (blk.park != ParkKind::None) continue;
        const District& d = out.districts[blk.district];
        const Style& style = p.styles[static_cast<u32>(d.kind)];
        Vector<Lot>& lots = parts[r].lots;
        const u32 first = lots.size();
        LotMaker maker{p, style, blk, b, blk.district, lots, parts[r].interior, first, {}};
        maker.streets = &out.streets;
        maker.make();
        for (u32 l = first; l < lots.size(); ++l) {
          Lot& lot = lots[l];
          if (blk.civic != CivicKind::None) {
            lot.use = LotUse::Civic;
            lot.civic = blk.civic;
            lot.archetype = Archetype::CivicShell;
          } else {
            choose_archetype(p, style, lot);
          }
        }
      }
    };
    if (jobs != nullptr && runs > 1) {
      jobs->parallel_for(jobs::Pool::Performance, runs, 1, [&](u32 begin, u32 end) {
        for (u32 r = begin; r < end; ++r)
          run(r);
      });
    } else {
      for (u32 r = 0; r < runs; ++r)
        run(r);
    }
    for (Part& part : parts) {
      for (const Lot& l : part.lots)
        out.lots.push_back(l);
      for (const Street& s : part.interior)
        out.streets.push_back(s);
    }
    std::sort(out.streets.begin(), out.streets.end(),
              [](const Street& a, const Street& b) { return a.id < b.id; });
    // Lots name their street by id while they are made; from here on by index.
    for (Lot& l : out.lots)
      l.street = out.find_street(l.street);
    for (u32 l = 0; l < out.lots.size(); ++l) {
      Block& b = out.blocks[out.lots[l].block];
      if (b.lot_count == 0) b.first_lot = l;
      ++b.lot_count;
    }
  }

  // Parks as records: every park block, then (below) every pocket.
  for (u32 b = 0; b < out.blocks.size(); ++b) {
    const Block& blk = out.blocks[b];
    if (blk.park == ParkKind::None) continue;
    Park park;
    park.kind = blk.park;
    park.block = b;
    park.district = blk.district;
    park.rect = blk.buildable;
    out.parks.push_back(park);
  }
  for (u32 l = 0; l < out.lots.size(); ++l) {
    const Lot& lot = out.lots[l];
    if (lot.use != LotUse::Park) continue;
    Park park;
    park.kind = lot.park;
    park.block = lot.block;
    park.lot = l;
    park.district = lot.district;
    park.rect = lot.rect;
    out.parks.push_back(park);
  }

  // 8. The walking distance: every dwelling lot, in lot order, within `walk_distance` of a park;
  // where one is not, the lot of its block nearest the block's centre becomes a pocket park.
  {
    const i64 reach2 = i64{p.walk_distance_cm} * p.walk_distance_cm;
    for (u32 l = 0; l < out.lots.size(); ++l) {
      const Lot& lot = out.lots[l];
      if (lot.use != LotUse::Building || !is_dwelling(lot.archetype)) continue;
      const i64 cx = (i64{lot.rect.x0} + lot.rect.x1) / 2;
      const i64 cz = (i64{lot.rect.z0} + lot.rect.z1) / 2;
      bool near = false;
      for (const Park& park : out.parks) {
        if (grid::dist2_to_rect(cx, cz, park.rect) <= reach2) {
          near = true;
          break;
        }
      }
      if (near) continue;
      const Block& blk = out.blocks[lot.block];
      const auto [bx, bz] = center_of(blk.rect);
      u32 best = k_no_id;
      i64 best_d = 0;
      for (u32 k = blk.first_lot; k < blk.first_lot + blk.lot_count; ++k) {
        const Lot& other = out.lots[k];
        if (other.use != LotUse::Building) continue;
        const auto [ox, oz] = center_of(other.rect);
        const i64 dd = d2(ox, oz, bx, bz);
        if (best == k_no_id || dd < best_d) {
          best = k;
          best_d = dd;
        }
      }
      if (best == k_no_id) continue;
      Lot& pocket = out.lots[best];
      pocket.use = LotUse::Park;
      pocket.park = ParkKind::Pocket;
      Park park;
      park.kind = ParkKind::Pocket;
      park.block = pocket.block;
      park.lot = best;
      park.district = pocket.district;
      park.rect = pocket.rect;
      out.parks.push_back(park);
    }
  }
  for (const Pin& pin : p.pins) {
    if (pin.what != OverrideKind::Civic || pin.civic == CivicKind::None) continue;
    const u32 l = out.find_lot(pin.id);
    if (l == k_no_id) {
      error = "overrides: no lot " + std::to_string(pin.id);
      return false;
    }
    Lot& lot = out.lots[l];
    if (lot.use == LotUse::Park) {
      for (u32 k = 0; k < out.parks.size(); ++k) {
        if (out.parks[k].lot == l) {
          out.parks.erase(out.parks.begin() + k);
          break;
        }
      }
    }
    lot.use = LotUse::Civic;
    lot.civic = pin.civic;
    lot.park = ParkKind::None;
    lot.archetype = Archetype::CivicShell;
  }

  // 9. The final graph with the closes and alleys, the districts' areas, and the tile index.
  build_graph(out);
  {
    Vector<Street> kept;
    for (const Street& s : out.streets) {
      if (s.from_cm <= s.to_cm) kept.push_back(s);
    }
    if (kept.size() != out.streets.size()) {
      // Lots name streets by index: remap them to the kept list.
      Vector<u32> remap(out.streets.size(), k_no_id);
      u32 k = 0;
      for (u32 s = 0; s < out.streets.size(); ++s) {
        if (out.streets[s].from_cm <= out.streets[s].to_cm) remap[s] = k++;
      }
      for (Lot& l : out.lots)
        l.street = l.street != k_no_id ? remap[l.street] : k_no_id;
      out.streets = std::move(kept);
      build_graph(out);
    }
  }
  for (District& d : out.districts)
    d.area_cm2 = 0;
  for (const Block& b : out.blocks)
    out.districts[b.district].area_cm2 += b.rect.area();
  build_tile_index(out);
  return true;
}

// ---- the per-tile query ---------------------------------------------------------------------

u64 tile_key(TileCoord tile) noexcept {
  return (u64{static_cast<u32>(tile.x)} << 32) | static_cast<u32>(tile.z);
}

TileCoord tile_of(const Plan& plan, i64 x_cm, i64 z_cm) noexcept {
  return TileCoord{grid::floor_div(x_cm, plan.params.tile_cm),
                   grid::floor_div(z_cm, plan.params.tile_cm)};
}

Rect tile_rect(const Plan& plan, TileCoord tile) noexcept {
  const i32 t = plan.params.tile_cm;
  return Rect{tile.x * t, tile.z * t, (tile.x + 1) * t, (tile.z + 1) * t};
}

void build_tile_index(Plan& plan) {
  plan.tile_index.clear();
  for (u32 l = 0; l < plan.lots.size(); ++l) {
    const Rect& r = plan.lots[l].rect;
    // The closed rectangle: a building's facade stands on its lot's line, and a proxy anchored on
    // that line belongs to the tile beyond it when the line is a tile edge.
    const TileCoord lo = tile_of(plan, r.x0, r.z0);
    const TileCoord hi = tile_of(plan, r.x1, r.z1);
    for (i32 z = lo.z; z <= hi.z; ++z) {
      for (i32 x = lo.x; x <= hi.x; ++x)
        plan.tile_index.push_back(TileEntry{tile_key(TileCoord{x, z}), l, 0});
    }
  }
  std::sort(plan.tile_index.begin(), plan.tile_index.end(),
            [](const TileEntry& a, const TileEntry& b) {
              return a.key != b.key ? a.key < b.key : a.lot < b.lot;
            });
}

void lots_in_tile(const Plan& plan, TileCoord tile, Vector<u32>& out) {
  out.clear();
  const u64 key = tile_key(tile);
  auto it = std::lower_bound(plan.tile_index.begin(), plan.tile_index.end(), key,
                             [](const TileEntry& e, u64 k) { return e.key < k; });
  for (; it != plan.tile_index.end() && it->key == key; ++it)
    out.push_back(it->lot);
}

TileCoord owner_tile(const Plan& plan, const Lot& lot) noexcept {
  return tile_of(plan, (i64{lot.rect.x0} + lot.rect.x1) / 2, (i64{lot.rect.z0} + lot.rect.z1) / 2);
}

u64 lot_seed(const Plan& plan, const Lot& lot) noexcept {
  return draw(plan.params.seed, k_purpose_lot, lot.id);
}

// ---- statistics -----------------------------------------------------------------------------

void plan_stats(const Plan& plan, PlanStats& out) noexcept {
  out = PlanStats{};
  for (const District& d : plan.districts) {
    ++out.districts_by_kind[static_cast<u32>(d.kind)];
    out.district_area_cm2[static_cast<u32>(d.kind)] += d.area_cm2;
    out.city_area_cm2 += d.area_cm2;
  }
  for (const Street& s : plan.streets) {
    ++out.streets_by_class[static_cast<u32>(s.cls)];
    out.street_length_cm[static_cast<u32>(s.cls)] += s.to_cm - s.from_cm;
  }
  out.blocks = plan.blocks.size();
  for (const Lot& l : plan.lots) {
    if (l.use == LotUse::Park) continue;
    ++out.lots;
    ++out.lots_by_archetype[static_cast<u32>(l.archetype)];
    if (l.use == LotUse::Civic) ++out.civic_by_kind[static_cast<u32>(l.civic)];
  }
  for (const Park& park : plan.parks) {
    ++out.parks_by_kind[static_cast<u32>(park.kind)];
    out.park_area_cm2 += park.rect.area();
  }
  out.nodes = plan.nodes.size();
  out.segments = plan.segments.size();
}

u64 hash_plan(const Plan& plan) noexcept {
  u64 h = hash_combine(k_hash_seed, plan.key);
  auto rect = [&](const Rect& r) {
    h = hash_combine(h, u64{static_cast<u32>(r.x0)} << 32 | static_cast<u32>(r.z0));
    h = hash_combine(h, u64{static_cast<u32>(r.x1)} << 32 | static_cast<u32>(r.z1));
  };
  h = hash_combine(h, plan.coastline.size());
  for (const i32 v : plan.coastline)
    h = hash_combine(h, static_cast<u32>(v));
  for (const i32 v : plan.arterials_x)
    h = hash_combine(h, static_cast<u32>(v));
  for (const i32 v : plan.arterials_z)
    h = hash_combine(h, static_cast<u32>(v));
  h = hash_combine(h, u64{static_cast<u32>(plan.city_limit_cm)} << 32 |
                          static_cast<u32>(plan.mountain_start_cm));
  h = hash_combine(h, plan.districts.size());
  for (const District& d : plan.districts) {
    h = hash_combine(h, u64{d.id} << 16 | u64{static_cast<u8>(d.kind)} << 8 | d.pinned);
    h = hash_combine(h, u64{static_cast<u32>(d.cx)} << 32 | static_cast<u32>(d.cz));
    h = hash_combine(h, static_cast<u64>(d.area_cm2) ^ (u64{d.superblocks} << 48));
  }
  h = hash_combine(h, plan.streets.size());
  for (const Street& s : plan.streets) {
    h = hash_combine(h, u64{s.id} << 32 | u64{static_cast<u8>(s.cls)} << 16 | u64{s.along_x} << 8 |
                            u64{s.pinned} << 4 | s.interior);
    h = hash_combine(h, u64{static_cast<u32>(s.line_cm)} << 32 | static_cast<u32>(s.width_cm));
    h = hash_combine(h, u64{static_cast<u32>(s.from_cm)} << 32 | static_cast<u32>(s.to_cm));
    h = hash_combine(h, s.superblock);
  }
  h = hash_combine(h, plan.blocks.size());
  for (const Block& b : plan.blocks) {
    h = hash_combine(h, u64{b.id} << 32 | b.district);
    rect(b.rect);
    rect(b.buildable);
    h = hash_combine(h, u64{static_cast<u8>(b.park)} << 8 | static_cast<u8>(b.civic));
    for (const u32 s : b.streets)
      h = hash_combine(h, s);
    h = hash_combine(h, u64{b.first_lot} << 32 | b.lot_count);
  }
  h = hash_combine(h, plan.lots.size());
  for (const Lot& l : plan.lots) {
    h = hash_combine(h, u64{l.id} << 32 | l.block);
    rect(l.rect);
    h = hash_combine(h, u64{l.street} << 32 | u64{l.district} << 16 | l.front);
    h = hash_combine(h, u64{static_cast<u8>(l.use)} << 24 |
                            u64{static_cast<u8>(l.archetype)} << 16 |
                            u64{static_cast<u8>(l.park)} << 8 | static_cast<u8>(l.civic));
  }
  h = hash_combine(h, plan.parks.size());
  for (const Park& park : plan.parks) {
    h = hash_combine(h, u64{static_cast<u8>(park.kind)} << 32 | park.district);
    h = hash_combine(h, u64{park.block} << 32 | park.lot);
    rect(park.rect);
  }
  h = hash_combine(h, u64{plan.nodes.size()} << 32 | plan.segments.size());
  return h;
}

}  // namespace engine::city
