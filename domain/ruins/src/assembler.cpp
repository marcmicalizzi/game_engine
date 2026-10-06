#include "grid.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/ruins/assembler.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::ruins {

namespace {

// ---- seeded draws
// ---------------------------------------------------------------------------------
//
// Every choice draws from the building's seed, a constant naming what the draw is for, and an
// index, so adding a draw for one purpose moves no other. The constants are arbitrary and fixed:
// changing one changes every building, which the golden hash would say.
enum Purpose : u64 {
  k_shape = 0x7275696e73000001ull,
  k_fill = 0x7275696e73000002ull,
  k_yaw = 0x7275696e73000003ull,
  k_place = 0x7275696e73000004ull,
  k_state = 0x7275696e73000005ull,
  k_level = 0x7275696e73000006ull,
  k_jitter = 0x7275696e73000007ull,
  k_breach = 0x7275696e73000008ull,
  k_opening = 0x7275696e73000009ull,
  k_opening_member = 0x7275696e7300000aull,
  k_opening_at = 0x7275696e7300000bull,
  k_run = 0x7275696e7300000cull,
  k_variant = 0x7275696e7300000dull,
  // 0x...0e and 0x...0f were the debris pieces' own count and placement, before both
  // representations laid their rubble on one field; they are not reused.
  k_tile_rank = 0x7275696e73000010ull,
  // The rubble field (ruins.md, "The rubble rule"): how many sites a wall has, and each site's
  // place, side, distance and yaw. Shared by both representations, so the block layer's fallen
  // blocks and the assembler's debris lie on the same draws.
  k_rubble_count = 0x7275696e73000011ull,
  k_rubble = 0x7275696e73000012ull,
  // The section form's own: which debris members make a site's heap, and where each lies in it.
  k_heap = 0x7275696e73000013ull,
};

// The most debris members one site's heap takes, whatever the kit's pieces weigh: a kit of pebbles
// fills its sites to this and no further.
constexpr u32 k_max_heap = 16;

// The seeded draws, the Q14 turn and the quarter turns are the module's one copy, shared with the
// block layer (src/grid.h).
using grid::draw;
using grid::isqrt_ceil;
using grid::k_dx;
using grid::k_dz;
using grid::left_of;
using grid::overlaps;
using grid::pick;
using grid::Rect;
using grid::right_of;
using grid::rotate_cm;
using grid::side_rect;

// cos of k * 22.5 degrees as floats: what turns a member's mesh offset for the renderer, never a
// decision. A table rather than the C library, like its integer twin in grid.h.
constexpr f32 k_cosf[16] = {
    1.0f,  0.92387953f,  0.70710678f,  0.38268343f,  0.0f, -0.38268343f, -0.70710678f, -0.92387953f,
    -1.0f, -0.92387953f, -0.70710678f, -0.38268343f, 0.0f, 0.38268343f,  0.70710678f,  0.92387953f};

// A weighted pick among members: `members` indexes the kit's.
u32 weighted(const Kit& kit, const Vector<u32>& members, u64 value) noexcept {
  u64 total = 0;
  for (const u32 m : members)
    total += kit.members[m].weight;
  if (total == 0) return members[pick(value, members.size())];
  u64 at = ((value >> 32) * total) >> 32;
  for (const u32 m : members) {
    if (at < kit.members[m].weight) return m;
    at -= kit.members[m].weight;
  }
  return members.back();
}

}  // namespace

// ---- the public helpers
// ---------------------------------------------------------------------------

const char* shape_name(Shape shape) noexcept {
  switch (shape) {
    case Shape::rectangle: return "rectangle";
    case Shape::l_shape: return "l";
    case Shape::u_shape: return "u";
    case Shape::courtyard: return "courtyard";
  }
  return "unknown";
}

Detail detail_for_distance(f32 distance_m, f32 tile_m) noexcept {
  // A debris block is half a metre; eight tiles away (256 m at the default 32 m tile) it is two
  // pixels at 1080p and a 55 degree field of view, so the tile keeps its walls and drops it.
  return distance_m > 8.0f * tile_m ? Detail::walls : Detail::full;
}

u64 building_seed(u64 world_seed, TileCoord tile) noexcept {
  const u64 h = hash_combine(k_hash_seed, world_seed);
  return hash_combine(hash_combine(h, static_cast<u64>(static_cast<u32>(tile.x))),
                      static_cast<u64>(static_cast<u32>(tile.z)));
}

u32 tile_rank(u64 world_seed, TileCoord tile) noexcept {
  return static_cast<u32>(draw(building_seed(world_seed, tile), k_tile_rank, 0) >> 40);
}

bool tile_has_building(u64 world_seed, TileCoord tile, f32 density) noexcept {
  const f64 scaled = std::floor(static_cast<f64>(density) * 16777216.0 + 0.5);
  const u32 threshold =
      scaled <= 0.0 ? 0u : (scaled >= 16777216.0 ? 16777216u : static_cast<u32>(scaled));
  return tile_rank(world_seed, tile) < threshold;
}

void choose_tiles(u64 world_seed, TileCoord min, TileCoord max, u32 count, f32 density,
                  Vector<TileCoord>& out) {
  out.clear();
  if (max.x < min.x || max.z < min.z) return;
  struct Ranked {
    u32 rank;
    TileCoord tile;
  };
  Vector<Ranked> ranked;
  const u64 span = static_cast<u64>(max.x - min.x + 1) * static_cast<u64>(max.z - min.z + 1);
  ranked.reserve(static_cast<u32>(std::min<u64>(span, 1u << 26)));
  for (i32 z = min.z; z <= max.z; ++z) {
    for (i32 x = min.x; x <= max.x; ++x) {
      const TileCoord t{x, z};
      const u32 rank = tile_rank(world_seed, t);
      if (count == 0 && !tile_has_building(world_seed, t, density)) continue;
      ranked.push_back(Ranked{rank, t});
    }
  }
  std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
    if (a.rank != b.rank) return a.rank < b.rank;
    if (a.tile.z != b.tile.z) return a.tile.z < b.tile.z;
    return a.tile.x < b.tile.x;
  });
  const u32 n = count == 0 ? ranked.size() : std::min<u32>(count, ranked.size());
  out.reserve(n);
  for (u32 i = 0; i < n; ++i)
    out.push_back(ranked[i].tile);
}

u8 yaw_step_from_degrees(f32 degrees) noexcept {
  const f64 steps = std::floor(static_cast<f64>(degrees) / 22.5 + 0.5);
  const i64 s = static_cast<i64>(steps);
  return static_cast<u8>(((s % 16) + 16) % 16);
}

f32 step_cos(u32 step) noexcept { return k_cosf[step & 15u]; }
f32 step_sin(u32 step) noexcept { return k_cosf[(step + 12u) & 15u]; }

u32 instance_yaw_step(const Kit& kit, const Instance& instance) noexcept {
  return (static_cast<u32>(instance.yaw) + kit.members[instance.member].yaw_step) & 15u;
}

WorldPos instance_translation(const Kit& kit, const Instance& instance) noexcept {
  const Member& member = kit.members[instance.member];
  const u32 step = instance_yaw_step(kit, instance);
  const f32 c = step_cos(step);
  const f32 s = step_sin(step);
  const Vec3 o = member.offset;
  // The offset turned in float32, the size of a member, and added to the piece's place in f64.
  return instance.position + DVec3{Vec3{c * o.x + s * o.z, o.y, c * o.z - s * o.x}};
}

u64 hash_output(const Output& out) noexcept {
  u64 h = hash_combine(k_hash_seed, out.sites.size());
  auto f = [](f32 v) { return static_cast<u64>(std::bit_cast<u32>(v)); };
  auto wide = [](f64 v) { return std::bit_cast<u64>(v); };
  for (const Site& s : out.sites) {
    h = hash_combine(h, s.seed);
    h = hash_combine(
        h, static_cast<u64>(static_cast<u32>(s.tile.x)) << 32 | static_cast<u32>(s.tile.z));
    h = hash_combine(h, wide(s.origin.x));
    h = hash_combine(h, wide(s.origin.y));
    h = hash_combine(h, wide(s.origin.z));
    h = hash_combine(h, static_cast<u64>(s.first_instance) << 32 | s.instance_count);
    h = hash_combine(h, static_cast<u64>(s.first_drift) << 32 | s.drift_count);
    h = hash_combine(h, static_cast<u64>(s.walls) << 16 | static_cast<u64>(s.shape) << 8 | s.yaw);
  }
  h = hash_combine(h, out.instances.size());
  for (const Instance& i : out.instances) {
    h = hash_combine(h, wide(i.position.x));
    h = hash_combine(h, wide(i.position.y));
    h = hash_combine(h, wide(i.position.z));
    h = hash_combine(h, i.member);
    h = hash_combine(h,
                     static_cast<u64>(i.building) << 32 | static_cast<u64>(i.wall) << 16 | i.slot);
    h = hash_combine(h, static_cast<u64>(i.height_q) << 16 | static_cast<u64>(i.kind) << 8 | i.yaw);
  }
  h = hash_combine(h, out.drifts.size());
  for (const Drift& d : out.drifts) {
    h = hash_combine(h, f(d.from.x) << 32 | f(d.from.y));
    h = hash_combine(h, f(d.to.x) << 32 | f(d.to.y));
    h = hash_combine(h, f(d.normal.x) << 32 | f(d.normal.y));
    h = hash_combine(h, f(d.height) << 32 | f(d.reach));
    h = hash_combine(
        h, static_cast<u64>(d.building) << 32 | static_cast<u64>(d.wall) << 8 | d.windward);
  }
  return h;
}

// ---- the footprint
// ----------------------------------------------------------------------------------

// Walks a closed ring of `count` sides from (x0, z0) along +x: `turns[k]` is 1 when the ring turns
// left (an outside corner) at vertex k and 0 when it turns right (an inside one), `lengths[k]` is
// side k's length on the centre line. Every side's fill is what its two corners leave, and it has
// to be a whole number of modules that sections alone can make; the ring has to close.
bool Assembler::ring_from(const u8* turns, const i32* lengths, u32 count, i32 x0, i32 z0, u8 ring) {
  const i32 m = kit_.module_cm;
  i32 x = x0;
  i32 z = z0;
  u8 q = 0;
  for (u32 k = 0; k < count; ++k) {
    if (k > 0) q = turns[k] != 0 ? left_of(q) : right_of(q);
    Side s;
    s.x0 = x;
    s.z0 = z;
    s.dir = q;
    s.ring = ring;
    s.length_cm = lengths[k];
    s.start_convex = turns[k];
    s.end_convex = turns[(k + 1) % count];
    s.start_cm = s.start_convex != 0 ? kit_.corner_out_cm : kit_.reflex_out_cm;
    s.end_cm = s.end_convex != 0 ? kit_.corner_in_cm : kit_.reflex_in_cm;
    s.fill_cm = s.length_cm - s.start_cm - s.end_cm;
    if (s.length_cm <= 0 || s.fill_cm < 0 || s.fill_cm % m != 0) return false;
    const u32 modules = static_cast<u32>(s.fill_cm / m);
    if (modules > kit_.max_fill_modules || kit_.fillable[modules] == 0) return false;
    sides_.push_back(s);
    x += k_dx[q] * s.length_cm;
    z += k_dz[q] * s.length_cm;
  }
  // Closed: back where it started, facing where it started once the first vertex's turn is made.
  const u8 back = turns[0] != 0 ? left_of(q) : right_of(q);
  return x == x0 && z == z0 && back == 0;
}

bool Assembler::footprint_valid() const noexcept {
  // No two walls overlap: every pair but the two that meet at a corner of one ring is disjoint.
  const i32 half = (kit_.thickness_cm + 1) / 2;
  for (u32 a = 0; a < sides_.size(); ++a) {
    const Rect ra = side_rect(sides_[a], half);
    for (u32 b = a + 1; b < sides_.size(); ++b) {
      const Side& sa = sides_[a];
      const Side& sb = sides_[b];
      if (sa.ring == sb.ring) {
        // Neighbours in one ring: b follows a, or a is the first and b the last.
        u32 first = 0;
        while (first < sides_.size() && sides_[first].ring != sa.ring)
          ++first;
        u32 last = first;
        while (last + 1 < sides_.size() && sides_[last + 1].ring == sa.ring)
          ++last;
        if (b == a + 1 || (a == first && b == last)) continue;
      }
      if (overlaps(ra, side_rect(sb, half))) return false;
    }
  }
  return true;
}

bool Assembler::build_footprint(u64 seed, i32 /*tile_cm*/, u32 attempt) {
  sides_.clear();
  const Rules& rules = kit_.rules;
  const i32 m = kit_.module_cm;
  const i32 co = kit_.corner_out_cm;
  const i32 ci = kit_.corner_in_cm;
  const i32 ro = kit_.reflex_out_cm;
  const i32 ri = kit_.reflex_in_cm;
  // Each attempt halves the range the fills are drawn from, so a building too big for its tile
  // shrinks toward the smallest the rules allow rather than being redrawn from scratch.
  const u32 lo = rules.min_fill;
  const u32 range = attempt >= 31 ? 0u : (rules.max_fill - rules.min_fill) >> attempt;
  // A fill `lo + pick` near the drawn one that sections can make: the kit's sections need not
  // include one a module long.
  auto fill = [&](u32 index) {
    const u32 want = lo + pick(draw(seed, k_fill, index), range + 1);
    for (u32 d = 0; d <= kit_.max_fill_modules; ++d) {
      if (want >= d && kit_.fillable[want - d] != 0) return want - d;
      if (want + d <= kit_.max_fill_modules && kit_.fillable[want + d] != 0) return want + d;
    }
    return 0u;
  };
  auto side = [&](i32 start, u32 modules, i32 end) {
    return start + static_cast<i32>(modules) * m + end;
  };
  switch (shape_) {
    case Shape::rectangle: {
      const u8 turns[4] = {1, 1, 1, 1};
      const i32 lx = side(co, fill(0), ci);
      const i32 lz = side(co, fill(1), ci);
      const i32 lengths[4] = {lx, lz, lx, lz};
      return ring_from(turns, lengths, 4, 0, 0, 0);
    }
    case Shape::l_shape: {
      // (0,0) +x, -z, -x, (inside corner) -z, -x, +z: a rectangle with its far corner cut away.
      const u8 turns[6] = {1, 1, 1, 0, 1, 1};
      const i32 l1 = side(co, fill(1), ci);
      const i32 l2 = side(co, fill(2), ri);
      const i32 l3 = side(ro, fill(3), ci);
      const i32 l4 = side(co, fill(4), ci);
      const i32 lengths[6] = {l2 + l4, l1, l2, l3, l4, l1 + l3};
      return ring_from(turns, lengths, 6, 0, 0, 0);
    }
    case Shape::u_shape: {
      // (0,0) +x, -z, -x, (a notch into the back) +z, -x, -z, -x, +z.
      const u8 turns[8] = {1, 1, 1, 1, 0, 0, 1, 1};
      const i32 l1 = side(co, fill(1), ci);
      const i32 l2 = side(co, fill(2), ci);
      const i32 l3 = side(co, fill(3), ri);
      const i32 l4 = side(ro, fill(4), ri);
      // The notch's far side as deep as its near one when the arms allow it, so the back of the
      // U is one line; otherwise as the draw says.
      const i32 even = l3 - ro - ci;
      const u32 f5 = even >= 0 && even % m == 0 ? static_cast<u32>(even / m) : fill(5);
      const i32 l5 = side(ro, f5, ci);
      const i32 l6 = side(co, fill(6), ci);
      const i32 l7 = l1 - l3 + l5;
      const i32 lengths[8] = {l2 + l4 + l6, l1, l2, l3, l4, l5, l6, l7};
      return ring_from(turns, lengths, 8, 0, 0, 0);
    }
    case Shape::courtyard: {
      // An outer rectangle and, inside it, the court's own wall: a ring that turns right at every
      // corner, so that its inside — the rooms — is between the two.
      const i32 li0 = side(ro, fill(10), ri);
      const i32 li1 = side(ro, fill(11), ri);
      const u32 room_min = static_cast<u32>((kit_.thickness_cm + m) / m);
      const i32 rooms[4] = {static_cast<i32>(room_min + pick(draw(seed, k_fill, 12), 2)) * m,
                            static_cast<i32>(room_min + pick(draw(seed, k_fill, 13), 2)) * m,
                            static_cast<i32>(room_min + pick(draw(seed, k_fill, 14), 2)) * m,
                            static_cast<i32>(room_min + pick(draw(seed, k_fill, 15), 2)) * m};
      const i32 lx = rooms[0] + li0 + rooms[1];
      const i32 lz = rooms[2] + li1 + rooms[3];
      const u8 outer_turns[4] = {1, 1, 1, 1};
      const i32 outer[4] = {lx, lz, lx, lz};
      if (!ring_from(outer_turns, outer, 4, 0, 0, 0)) return false;
      const u8 inner_turns[4] = {0, 0, 0, 0};
      const i32 inner[4] = {li0, li1, li0, li1};
      return ring_from(inner_turns, inner, 4, rooms[0], -lz + rooms[3], 1);
    }
  }
  return false;
}

// The height the ruin rule asks of a wall at `at_cm` along it, Q10 of the intact wall.
i32 Assembler::profile_q(const Side& s, u32 wall, u64 seed, i32 at_cm) const noexcept {
  const Rules& rules = kit_.rules;
  i32 h = k_q_one;
  if (s.state == static_cast<u8>(WallState::collapsed)) {
    // Its level, broken per module by up to a tenth either way, so the top line is not a ruler.
    const u32 module = static_cast<u32>(std::clamp(at_cm, 0, s.length_cm) / kit_.module_cm);
    h = s.level_q + static_cast<i32>(pick(draw(seed, k_jitter, u64{wall} << 20 | module), 205)) -
        102;
  } else if (s.state == static_cast<u8>(WallState::breached)) {
    // A gap, and the wall climbing back to full height over two modules either side of it.
    const i32 d = std::abs(at_cm - s.breach_at_cm) - s.breach_half_cm;
    h = d <= 0 ? 0
               : std::min<i32>(k_q_one, static_cast<i32>(i64{d} * k_q_one / (2 * kit_.module_cm)));
  }
  // An outside corner of the outer wall is the most exposed stone of the building: a wall that is
  // coming down loses more near one, falling off over `corner_reach`.
  if (s.state != static_cast<u8>(WallState::intact) && s.ring == 0 && rules.corner_reach_cm > 0) {
    if (s.start_convex != 0 && at_cm < rules.corner_reach_cm) {
      h -= static_cast<i32>(i64{rules.corner_drop_q} * (rules.corner_reach_cm - at_cm) /
                            rules.corner_reach_cm);
    }
    const i32 from_end = s.length_cm - at_cm;
    if (s.end_convex != 0 && from_end < rules.corner_reach_cm) {
      h -= static_cast<i32>(i64{rules.corner_drop_q} * (rules.corner_reach_cm - from_end) /
                            rules.corner_reach_cm);
    }
  }
  return std::clamp(h, 0, k_q_one);
}

i32 Assembler::height_q(u32 wall, i32 at_cm) const noexcept {
  return wall < sides_.size() ? profile_q(sides_[wall], wall, frame_.seed, at_cm) : 0;
}

void Assembler::to_world_cm(i64 x, i64 z, i64& wx, i64& wz) const noexcept {
  rotate_cm(frame_.yaw, x, z, wx, wz);
  wx += frame_.origin_x_cm;
  wz += frame_.origin_z_cm;
}

// ---- the rubble field
// -------------------------------------------------------------------------------
//
// One rule for what a building's walls dropped, which both representations lay: before it, the
// block layer dropped every block that fell beside its wall while the assembler scattered a few
// debris members by a count of its own, and the two forms of one building agreed on everything but
// the ground — which was most of what popped when a tile changed form (E35).
const Vector<RubbleSite>& Assembler::rubble() {
  if (rubble_ready_) return rubble_;
  rubble_ready_ = true;
  rubble_.clear();
  const Rules& rules = kit_.rules;
  if (rules.debris_per_module_q <= 0 || kit_.rubble_cm3 <= 0) return rubble_;
  const i32 m = kit_.module_cm;
  const i32 half = (kit_.thickness_cm + 1) / 2;
  const i32 r = kit_.rubble_radius_cm;
  const u64 seed = frame_.seed;
  const u32 spread = static_cast<u32>(rules.debris_spread_cm + 1);
  // The ruin line is sampled at an eighth of a module: finer than its breaks, which are a module
  // apart, and coarse enough to cost a building a few hundred samples.
  const i32 step = std::max(m / 8, 5);
  for (u32 w = 0; w < sides_.size(); ++w) {
    const Side& s = sides_[w];
    // An intact wall stands to the full height all along (the corners' extra loss is for walls
    // coming down): nothing came down.
    if (s.state == static_cast<u8>(WallState::intact)) continue;
    // What came down: the intact height less the ruin line's, along the wall — the removed
    // material's volume, in centimetres of wall times Q10 of its height. Kept cumulative over
    // the samples, so a site's place can be drawn where the wall came down.
    removed_.clear();
    i64 total = 0;
    for (i32 a = 0; a < s.length_cm; a += step) {
      const i32 b = std::min(a + step, s.length_cm);
      total += i64{k_q_one - profile_q(s, w, seed, a + (b - a) / 2)} * (b - a);
      removed_.push_back(total);
    }
    if (total <= 0) continue;
    // How many sites: the removed volume in modules of wall fully brought down, times the rule's
    // blocks of rubble for each, the fraction settled by a draw.
    const i64 expected = total * rules.debris_per_module_q / (i64{m} * k_q_one);
    u32 count = static_cast<u32>(expected >> 10);
    if (static_cast<i64>(pick(draw(seed, k_rubble_count, w), k_q_one)) < (expected & 1023)) ++count;
    for (u32 k = 0; k < count; ++k) {
      const u64 key = u64{w} << 40 | u64{k} << 8;
      // Along the wall where it came down: the inverse of the cumulative removal at a uniform
      // draw, then anywhere in that sample.
      const u64 at = ((draw(seed, k_rubble, key) >> 32) * static_cast<u64>(total)) >> 32;
      const u32 sample = static_cast<u32>(
          std::upper_bound(removed_.begin(), removed_.end(), static_cast<i64>(at)) -
          removed_.begin());
      const i32 a = static_cast<i32>(sample) * step;
      const i32 b = std::min(a + step, s.length_cm);
      const i32 along =
          a + static_cast<i32>(pick(draw(seed, k_rubble, key | 1), static_cast<u32>(b - a)));
      const bool outward =
          static_cast<i32>(pick(draw(seed, k_rubble, key | 2), k_q_one)) < rules.debris_outward_q;
      // Nearer the wall more often than not: the lesser of two draws, so a fallen wall leaves a
      // heap at its foot thinning outward rather than a ring at a uniform distance.
      const i32 dist = half + r +
                       static_cast<i32>(std::min(pick(draw(seed, k_rubble, key | 3), spread),
                                                 pick(draw(seed, k_rubble, key | 4), spread)));
      const u8 n = outward ? right_of(s.dir) : left_of(s.dir);
      i32 x = s.x0 + k_dx[s.dir] * along + k_dx[n] * dist;
      i32 z = s.z0 + k_dz[s.dir] * along + k_dz[n] * dist;
      // Out of every wall, pushed across the wall it landed in to the nearer face; a site that
      // cannot be put clear, or whose piece would leave the tile, is not a site.
      if (!grid::settle_clear(sides_, half, r, x, z)) continue;
      i64 wx = 0, wz = 0;
      to_world_cm(x, z, wx, wz);
      if (wx - r < frame_.tile_x0_cm || wx + r > frame_.tile_x0_cm + frame_.tile_cm ||
          wz - r < frame_.tile_z0_cm || wz + r > frame_.tile_z0_cm + frame_.tile_cm) {
        continue;
      }
      RubbleSite site;
      site.x_cm = x;
      site.z_cm = z;
      site.along_cm = along;
      site.wall = static_cast<u16>(w);
      site.index = static_cast<u16>(std::min<u32>(k, 0xffffu));
      site.yaw = static_cast<u8>(pick(draw(seed, k_rubble, key | 5), 16));
      site.outward = outward ? 1 : 0;
      rubble_.push_back(site);
    }
  }
  return rubble_;
}

bool Assembler::assemble(const Placement& placement, TileCoord tile, Output& out,
                         std::string* error) {
  const Kit& kit = kit_;
  const Rules& rules = kit.rules;
  const i32 m = kit.module_cm;
  const i32 wall_h = kit.wall_height_cm;
  const u64 seed = building_seed(placement.world_seed, tile);

  // ---- 1. the footprint: a shape, its walls, and a tile it fits
  // -----------------------------------
  u32 total = 0;
  for (const u32 w : rules.shape_weight)
    total += w;
  u32 at = pick(draw(seed, k_shape, 0), total);
  shape_ = Shape::rectangle;
  for (u32 s = 0; s < 4; ++s) {
    if (at < rules.shape_weight[s]) {
      shape_ = static_cast<Shape>(s);
      break;
    }
    at -= rules.shape_weight[s];
  }
  const i64 tile_cm = placement.tile_cm;
  i32 lo_x = 0, lo_z = 0, hi_x = 0, hi_z = 0;
  i64 radius = 0;
  auto fits = [&]() {
    lo_x = hi_x = sides_[0].x0;
    lo_z = hi_z = sides_[0].z0;
    for (const Side& s : sides_) {
      lo_x = std::min(lo_x, s.x0);
      hi_x = std::max(hi_x, s.x0);
      lo_z = std::min(lo_z, s.z0);
      hi_z = std::max(hi_z, s.z0);
    }
    // The bounding circle of everything a building puts down — its walls, its debris, its drift —
    // at any yaw: half the box's diagonal plus the kit's margin on each side.
    const i64 hx = (i64{hi_x} - lo_x + 1) / 2 + kit.margin_cm;
    const i64 hz = (i64{hi_z} - lo_z + 1) / 2 + kit.margin_cm;
    radius = isqrt_ceil(hx * hx + hz * hz);
    return 2 * radius <= tile_cm;
  };
  const Shape wanted = shape_;
  bool placed = false;
  for (u32 attempt = 0; attempt < 8 && !placed; ++attempt) {
    placed = build_footprint(seed, placement.tile_cm, attempt) && footprint_valid() && fits();
  }
  if (!placed && wanted != Shape::rectangle) {
    // A shape the kit's corners cannot close, or cannot fit: the rectangle, which always closes.
    shape_ = Shape::rectangle;
    for (u32 attempt = 0; attempt < 32 && !placed; ++attempt)
      placed = build_footprint(seed, placement.tile_cm, attempt) && footprint_valid() && fits();
  }
  if (!placed) {
    if (error != nullptr) {
      *error = "no footprint of the kit '" + kit.name + "' fits a " +
               std::to_string(placement.tile_cm) + " cm tile with its " +
               std::to_string(kit.margin_cm) + " cm margin";
    }
    return false;
  }

  // ---- 2. where it stands: a yaw, a place in the tile, and the ground under it
  // ----------------------
  const u32 steps = rules.yaw_steps;
  const u8 yaw = static_cast<u8>(pick(draw(seed, k_yaw, 0), steps) * (16u / steps));
  const i64 slack = tile_cm / 2 - radius;
  const i64 tile_x0 = placement.origin_x_cm + i64{tile.x} * tile_cm;
  const i64 tile_z0 = placement.origin_z_cm + i64{tile.z} * tile_cm;
  const i64 centre_x =
      tile_x0 + tile_cm / 2 +
      (slack > 0
           ? static_cast<i64>(pick(draw(seed, k_place, 0), static_cast<u32>(2 * slack + 1))) - slack
           : 0);
  const i64 centre_z =
      tile_z0 + tile_cm / 2 +
      (slack > 0
           ? static_cast<i64>(pick(draw(seed, k_place, 1), static_cast<u32>(2 * slack + 1))) - slack
           : 0);
  // The origin is where the footprint's first vertex lands: the box's centre, turned, is put on
  // the tile's chosen point.
  i64 cx = 0, cz = 0;
  rotate_cm(yaw, (i64{lo_x} + hi_x) / 2, (i64{lo_z} + hi_z) / 2, cx, cz);
  const i64 origin_x = centre_x - cx;
  const i64 origin_z = centre_z - cz;
  auto world = [&](i64 x, i64 z, i64& wx, i64& wz) {
    rotate_cm(yaw, x, z, wx, wz);
    wx += origin_x;
    wz += origin_z;
  };
  auto metres = [](i64 cm) { return static_cast<f32>(cm) * 0.01f; };
  // The ground under the building: its lowest point under any vertex or any module boundary of any
  // wall — every place a member stands on, and where each one ends — on the centimetre grid, less
  // the embed. One height for the whole building: the walls meet at their corners at one level,
  // and the uphill side is buried, which is what sand does anyway.
  f32 lowest = 0.0f;
  bool first = true;
  for (const Side& s : sides_) {
    const i32 boundaries = s.fill_cm / m;
    for (i32 k = -1; k <= boundaries; ++k) {
      i64 wx = 0, wz = 0;
      const i64 along = k < 0 ? 0 : s.start_cm + k * m;
      world(s.x0 + k_dx[s.dir] * along, s.z0 + k_dz[s.dir] * along, wx, wz);
      const f32 h = placement.ground.at_cm(wx, wz);
      lowest = first || h < lowest ? h : lowest;
      first = false;
    }
  }
  const i32 base_cm = static_cast<i32>(std::floor(lowest * 100.0f)) - rules.embed_cm;
  frame_ = Frame{seed, origin_x, origin_z, base_cm, yaw, tile_x0, tile_z0, placement.tile_cm};
  rubble_ready_ = false;

  // ---- 3. each wall's ruin state: the wind and the corners decide how far it came down
  // --------------
  for (u32 w = 0; w < sides_.size(); ++w) {
    Side& s = sides_[w];
    const u32 outward = (static_cast<u32>(right_of(s.dir)) * 4u + yaw) & 15u;
    const u32 diff = (outward + 16u - placement.wind_step) & 15u;
    const u32 off = std::min(diff, 16u - diff);
    s.facing = s.ring != 0 ? 2 : (off <= 3 ? 0 : (off == 4 ? 1 : 2));
    const bool windward = s.facing == 0;
    i32 p_collapse = rules.collapse_q + (windward ? rules.windward_q : 0);
    i32 p_breach = rules.breach_q + (windward ? rules.windward_q / 2 : 0);
    if (s.ring != 0) {  // a court's wall is sheltered by the building around it
      p_collapse /= 2;
      p_breach /= 2;
    }
    const i32 u = static_cast<i32>(pick(draw(seed, k_state, w), k_q_one));
    s.state = static_cast<u8>(u < p_collapse                               ? WallState::collapsed
                              : u < p_collapse + p_breach && s.fill_cm > 0 ? WallState::breached
                                                                           : WallState::intact);
    if (s.state == static_cast<u8>(WallState::collapsed)) {
      const u32 spread = static_cast<u32>(rules.collapse_max_q - rules.collapse_min_q + 1);
      s.level_q = rules.collapse_min_q + static_cast<i32>(pick(draw(seed, k_level, w), spread)) -
                  (windward ? rules.windward_q / 2 : 0);
      s.level_q = std::max(s.level_q, 0);
    } else if (s.state == static_cast<u8>(WallState::breached)) {
      s.breach_at_cm =
          s.start_cm + static_cast<i32>(pick(draw(seed, k_breach, w), static_cast<u32>(s.fill_cm)));
      const u32 widths = static_cast<u32>(rules.breach_max_cm - rules.breach_min_cm + 1);
      s.breach_half_cm = (rules.breach_min_cm +
                          static_cast<i32>(pick(draw(seed, k_breach, w | 1u << 16), widths))) /
                         2;
    }
  }

  // ---- 4. corners, openings and sections, wall by wall
  // ---------------------------------------------
  const u32 building = out.sites.size();
  const u32 first_instance = out.instances.size();
  // Chooses the member of `kind` and `length_cm` (0: any length) whose height is nearest the
  // target from below, and how far to sink it when every one is taller.
  auto choose = [&](PieceKind kind, i32 length_cm, i32 target_q, u64 index, u32& member,
                    i32& sink_cm) {
    const Vector<u32>& all = kit.by_kind[static_cast<u32>(kind)];
    const i32 target_cm = static_cast<i32>(i64{target_q} * wall_h / k_q_one);
    const i32 tolerance = wall_h / 20;
    i32 best = -1;
    i32 lowest_h = 0x7fffffff;
    for (const u32 c : all) {
      const Member& mb = kit.members[c];
      if (length_cm != 0 && mb.length_cm != length_cm) continue;
      if (mb.height_cm <= target_cm + tolerance) best = std::max(best, mb.height_cm);
      lowest_h = std::min(lowest_h, mb.height_cm);
    }
    const i32 height = best >= 0 ? best : lowest_h;
    candidates_.clear();
    for (const u32 c : all) {
      const Member& mb = kit.members[c];
      if ((length_cm == 0 || mb.length_cm == length_cm) && mb.height_cm == height)
        candidates_.push_back(c);
    }
    member = weighted(kit, candidates_, draw(seed, k_variant, index));
    sink_cm = best >= 0 ? 0
                        : std::min(height - target_cm,
                                   static_cast<i32>(i64{height} * rules.max_sink_q / k_q_one));
  };
  auto emit = [&](i64 x, i64 z, i32 sink_cm, u32 member, u32 wall, u32 slot, i32 target_q,
                  PieceKind kind, u32 yaw_local) {
    i64 wx = 0, wz = 0;
    world(x, z, wx, wz);
    Instance inst;
    inst.position = WorldPos{grid::metres_f64(wx), grid::metres_f64(i64{base_cm} - sink_cm),
                             grid::metres_f64(wz)};
    inst.member = member;
    inst.building = building;
    inst.wall = static_cast<u16>(wall);
    inst.slot = static_cast<u16>(slot);
    inst.height_q = static_cast<u16>(std::clamp(target_q, 0, k_q_one));
    inst.kind = static_cast<u8>(kind);
    inst.yaw = static_cast<u8>((yaw_local + yaw) & 15u);
    out.instances.push_back(inst);
  };
  const Vector<u32>& openings_doors = kit.by_kind[static_cast<u32>(PieceKind::doorway)];
  const Vector<u32>& openings_windows = kit.by_kind[static_cast<u32>(PieceKind::window)];

  u32 ring_first = 0;
  for (u32 w = 0; w < sides_.size(); ++w) {
    const Side& s = sides_[w];
    if (w > 0 && s.ring != sides_[w - 1].ring) ring_first = w;
    u32 ring_last = w;
    while (ring_last + 1 < sides_.size() && sides_[ring_last + 1].ring == s.ring)
      ++ring_last;
    const Side& prev = sides_[w == ring_first ? ring_last : w - 1];
    const u32 prev_w = w == ring_first ? ring_last : w - 1;

    // The corner at this wall's start: an outside corner where the ring turns left, an inside one
    // where it turns right — the kit's own, or its outside corner turned half round.
    {
      const i32 h_prev = profile_q(prev, prev_w, seed, prev.length_cm);
      const i32 h_this = profile_q(s, w, seed, 0);
      const i32 target = std::max(std::min(h_prev, h_this), rules.min_height_q);
      PieceKind kind = PieceKind::corner;
      u32 yaw_local = static_cast<u32>(s.dir) * 4u;
      if (s.start_convex == 0) {
        if (kit.has_inside_corner) {
          kind = PieceKind::inside_corner;
        } else {
          yaw_local = (static_cast<u32>(prev.dir) + 2u) * 4u & 15u;
        }
      }
      u32 member = 0;
      i32 sink = 0;
      choose(kind, 0, target, u64{w} << 20, member, sink);
      emit(s.x0, s.z0, sink, member, w, 0, target, kind, yaw_local);
    }

    // What fills between the corners: an opening where the wall still stands, and sections.
    const u32 fill_modules = static_cast<u32>(s.fill_cm / m);
    u32 opening = ~0u;
    u32 opening_at = 0;
    const bool can_open =
        s.state != static_cast<u8>(WallState::collapsed) &&
        (!openings_doors.empty() || !openings_windows.empty()) &&
        static_cast<i32>(pick(draw(seed, k_opening, w), k_q_one)) < rules.opening_q;
    if (can_open) {
      // Doors to the lee, where the drift is lower; windows either side. Both by weight.
      candidates_.clear();
      for (const u32 d : openings_doors) {
        candidates_.push_back(d);
        if (s.facing == 2) candidates_.push_back(d);
      }
      for (const u32 d : openings_windows)
        candidates_.push_back(d);
      const u32 member = weighted(kit, candidates_, draw(seed, k_opening_member, w));
      const Member& mb = kit.members[member];
      const u32 width = static_cast<u32>(mb.length_cm / m);
      // The member's own height, as a fraction of the kit's wall: a member taller than the wall
      // (E33's doorway is 3.0 m in a 2.8 m kit) asks for the wall intact and no more.
      const i32 needs_q =
          std::min<i32>(k_q_one, static_cast<i32>(i64{mb.height_cm} * k_q_one / wall_h));
      // Every place along the fill where the runs either side can be filled and the wall stands
      // the member's full height over it: never on a run that has come down.
      candidates_.clear();
      for (u32 p = 0; width <= fill_modules && p + width <= fill_modules; ++p) {
        if (kit.fillable[p] == 0 || kit.fillable[fill_modules - width - p] == 0) continue;
        bool standing = true;
        for (u32 k = 0; k <= width && standing; ++k)
          standing = profile_q(s, w, seed, s.start_cm + static_cast<i32>((p + k)) * m) >= needs_q;
        if (standing) candidates_.push_back(p);
      }
      if (!candidates_.empty()) {
        opening = member;
        opening_at = candidates_[pick(draw(seed, k_opening_at, w), candidates_.size())];
      }
    }
    // The runs of sections either side of the opening. A wall that has come down is laid in its
    // shortest sections, so that its broken top line has the resolution of one; an intact wall
    // takes any length the draw gives it.
    const bool intact = s.state == static_cast<u8>(WallState::intact);
    u32 slot = 1;
    u64 run_draw = 0;
    auto run = [&](u32 from, u32 modules) {
      while (modules > 0) {
        candidates_.clear();
        for (const u32 len : kit.section_modules) {
          if (len > modules || kit.fillable[modules - len] == 0) continue;
          if (!intact && !candidates_.empty()) break;  // the shortest that leaves a fillable rest
          candidates_.push_back(len);
        }
        const u32 len =
            candidates_[pick(draw(seed, k_run, u64{w} << 20 | run_draw++), candidates_.size())];
        const i32 a = s.start_cm + static_cast<i32>(from) * m;
        const i32 b = a + static_cast<i32>(len) * m;
        const i32 target = std::min({profile_q(s, w, seed, a), profile_q(s, w, seed, (a + b) / 2),
                                     profile_q(s, w, seed, b)});
        if (target >= rules.min_height_q) {
          u32 member = 0;
          i32 sink = 0;
          choose(PieceKind::section, static_cast<i32>(len) * m, target, u64{w} << 20 | slot, member,
                 sink);
          emit(s.x0 + k_dx[s.dir] * a, s.z0 + k_dz[s.dir] * a, sink, member, w, slot, target,
               PieceKind::section, static_cast<u32>(s.dir) * 4u);
        }
        ++slot;
        from += len;
        modules -= len;
      }
    };
    if (opening != ~0u) {
      const Member& mb = kit.members[opening];
      const u32 width = static_cast<u32>(mb.length_cm / m);
      run(0, opening_at);
      const i32 a = s.start_cm + static_cast<i32>(opening_at) * m;
      sides_[w].opening = opening;
      sides_[w].opening_at_cm = a;
      emit(s.x0 + k_dx[s.dir] * a, s.z0 + k_dz[s.dir] * a, 0, opening, w, slot, k_q_one, mb.kind,
           static_cast<u32>(s.dir) * 4u);
      ++slot;
      run(opening_at + width, fill_modules - opening_at - width);
    } else {
      run(0, fill_modules);
    }
  }

  // ---- 5. debris: a heap of the kit's debris members on every site of the rubble field
  // --------------
  // The field is the building's, and the block layer lays one fallen block on each of its sites;
  // here a site takes debris members, picked by the kit's weights — which a kit weights inversely
  // by volume (E33), so every pick brings about the same stone — until the heap is as large as the
  // block it stands for: the last piece is kept when it brings the heap nearer the block's volume
  // than stopping short would. The first lies at the site's centre as the block would, turned as
  // it would be; the rest are scattered round it, every one within the site's radius, which the
  // site was put clear of the walls and inside the tile by.
  const Vector<u32>& debris = kit.by_kind[static_cast<u32>(PieceKind::debris)];
  if (placement.detail == Detail::full && !debris.empty()) {
    const Vector<RubbleSite>& sites = rubble();
    for (const RubbleSite& site : sites) {
      i64 volume = 0;
      for (u32 j = 0; j < k_max_heap && volume < kit.rubble_cm3; ++j) {
        const u64 key = u64{site.wall} << 40 | u64{site.index} << 16 | u64{j} << 4;
        const u32 member = weighted(kit, debris, draw(seed, k_heap, key));
        const Member& mb = kit.members[member];
        if (j > 0 && volume + mb.volume_cm3 - kit.rubble_cm3 > kit.rubble_cm3 - volume) break;
        volume += mb.volume_cm3;
        i32 x = site.x_cm;
        i32 z = site.z_cm;
        u8 piece_yaw = site.yaw;
        if (j > 0) {
          // Anywhere in the square of half side (radius - own radius) / sqrt 2 (181/256), which
          // keeps the piece inside the site's radius at any draw.
          const i32 reach = std::max(0, ((kit.rubble_radius_cm - mb.radius_cm) * 181) >> 8);
          const u32 span = static_cast<u32>(2 * reach + 1);
          x += static_cast<i32>(pick(draw(seed, k_heap, key | 1), span)) - reach;
          z += static_cast<i32>(pick(draw(seed, k_heap, key | 2), span)) - reach;
          piece_yaw = static_cast<u8>(pick(draw(seed, k_heap, key | 3), 16));
        }
        i64 wx = 0, wz = 0;
        world(x, z, wx, wz);
        // On the ground where it lies, not the building's base: debris follows the terrain.
        const f32 ground = placement.ground.at_cm(wx, wz);
        Instance inst;
        inst.position = WorldPos{
            grid::metres_f64(wx),
            grid::metres_f64(static_cast<i64>(std::floor(ground * 100.0f)) - rules.embed_cm),
            grid::metres_f64(wz)};
        inst.member = member;
        inst.building = building;
        inst.wall = site.wall;
        inst.slot = site.index;
        inst.height_q = 0;
        inst.kind = static_cast<u8>(PieceKind::debris);
        inst.yaw = piece_yaw;
        out.instances.push_back(inst);
      }
    }
  }

  // ---- 6. the sand each wall asks for, and the site
  // -------------------------------------------------
  const u32 first_drift = out.drifts.size();
  const i32 half = (kit.thickness_cm + 1) / 2;
  for (u32 w = 0; w < sides_.size(); ++w) {
    const Side& s = sides_[w];
    const u8 n = right_of(s.dir);
    i64 ax = 0, az = 0, bx = 0, bz = 0;
    world(s.x0 + k_dx[n] * half, s.z0 + k_dz[n] * half, ax, az);
    world(s.x0 + k_dx[s.dir] * s.length_cm + k_dx[n] * half,
          s.z0 + k_dz[s.dir] * s.length_cm + k_dz[n] * half, bx, bz);
    const u32 normal_step = (static_cast<u32>(n) * 4u + yaw) & 15u;
    Drift d;
    d.from = Vec2{metres(ax), metres(az)};
    d.to = Vec2{metres(bx), metres(bz)};
    d.normal = Vec2{step_cos(normal_step), -step_sin(normal_step)};
    const i32 height = s.facing == 0   ? rules.drift_windward_cm
                       : s.facing == 1 ? (rules.drift_windward_cm + rules.drift_lee_cm) / 2
                                       : rules.drift_lee_cm;
    d.height = metres(height);
    d.reach = metres(rules.drift_reach_cm);
    d.building = building;
    d.wall = static_cast<u16>(w);
    d.windward = s.facing == 0 ? 1 : 0;
    out.drifts.push_back(d);
  }
  Site site;
  site.seed = seed;
  site.tile = tile;
  i64 ox = 0, oz = 0;
  world(0, 0, ox, oz);
  site.origin = WorldPos{grid::metres_f64(ox), grid::metres_f64(base_cm), grid::metres_f64(oz)};
  site.first_instance = first_instance;
  site.instance_count = out.instances.size() - first_instance;
  site.first_drift = first_drift;
  site.drift_count = out.drifts.size() - first_drift;
  site.walls = static_cast<u16>(sides_.size());
  site.shape = static_cast<u8>(shape_);
  site.yaw = yaw;
  out.sites.push_back(site);
  return true;
}

bool assemble_tiles(const Kit& kit, const Placement& placement, std::span<const TileCoord> tiles,
                    jobs::JobSystem* jobs, Output& out, std::string* error) {
  const u32 count = static_cast<u32>(tiles.size());
  const u32 workers = jobs != nullptr ? jobs->worker_count(jobs::Pool::Performance) + 1 : 1;
  if (jobs == nullptr || workers <= 1 || count < 2) {
    Assembler assembler(kit);
    for (const TileCoord& t : tiles) {
      if (!assembler.assemble(placement, t, out, error)) return false;
    }
    return true;
  }
  // Contiguous runs of tiles, one output each, joined in tile order: the result is the one a
  // single thread writes, whatever the thread count.
  const u32 runs = std::min(count, workers * 4);
  Vector<Output> parts(runs);
  Vector<std::string> errors(runs);
  Vector<u8> ok(runs, u8{1});
  jobs->parallel_for(jobs::Pool::Performance, runs, 1, [&](u32 begin, u32 end) {
    for (u32 r = begin; r < end; ++r) {
      Assembler assembler(kit);
      const u32 from = static_cast<u32>(u64{count} * r / runs);
      const u32 to = static_cast<u32>(u64{count} * (r + 1) / runs);
      for (u32 i = from; i < to && ok[r] != 0; ++i)
        ok[r] = assembler.assemble(placement, tiles[i], parts[r], &errors[r]) ? 1 : 0;
    }
  });
  for (u32 r = 0; r < runs; ++r) {
    if (ok[r] == 0) {
      if (error != nullptr) *error = errors[r];
      return false;
    }
  }
  u32 instances = out.instances.size();
  u32 drifts = out.drifts.size();
  u32 sites = out.sites.size();
  for (const Output& p : parts) {
    instances += p.instances.size();
    drifts += p.drifts.size();
    sites += p.sites.size();
  }
  out.instances.reserve(instances);
  out.drifts.reserve(drifts);
  out.sites.reserve(sites);
  for (Output& p : parts) {
    const u32 site_base = out.sites.size();
    const u32 instance_base = out.instances.size();
    const u32 drift_base = out.drifts.size();
    for (Instance i : p.instances) {
      i.building += site_base;
      out.instances.push_back(i);
    }
    for (Drift d : p.drifts) {
      d.building += site_base;
      out.drifts.push_back(d);
    }
    for (Site s : p.sites) {
      s.first_instance += instance_base;
      s.first_drift += drift_base;
      out.sites.push_back(s);
    }
  }
  return true;
}

}  // namespace engine::ruins
