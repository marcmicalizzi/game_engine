#include "grid.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/ruins/blocks.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace engine::ruins {

namespace {

using grid::draw;
using grid::k_dx;
using grid::k_dz;
using grid::left_of;
using grid::metres;
using grid::pick;
using grid::right_of;

// The layer's own draws, beside the assembler's (src/assembler.cpp): a purpose never shared, so a
// block's draws move nothing the section representation decides.
enum Purpose : u64 {
  k_block_top = 0x7275696e73000101ull,      // the broken top: a block's own place about the line
  k_block_variant = 0x7275696e73000102ull,  // crisp or eroded, and which of them
  k_block_kept = 0x7275696e73000103ull,     // a fallen block beside the wall, or under the sand
  k_block_fall = 0x7275696e73000104ull,     // where a fallen block lies, and how it is turned
};

u32 weighted_block(const BlockKit& kit, const Vector<u32>& list, u64 value) noexcept {
  u64 total = 0;
  for (const u32 b : list)
    total += kit.blocks[b].weight;
  if (total == 0) return list[pick(value, list.size())];
  u64 at = ((value >> 32) * total) >> 32;
  for (const u32 b : list) {
    if (at < kit.blocks[b].weight) return b;
    at -= kit.blocks[b].weight;
  }
  return list.back();
}

// A member of `group`, eroded with chance `eroded_q`: when the group has no block of the
// weathering the draw asks for, the other kind stands in.
u32 choose_block(const BlockKit& kit, u32 group, i32 eroded_q, u64 seed, u64 key,
                 bool& eroded) noexcept {
  const BlockKit::Group& g = kit.groups[group];
  const bool want =
      static_cast<i32>(pick(draw(seed, k_block_variant, key << 1), k_q_one)) < eroded_q;
  const Vector<u32>& wanted = want ? g.eroded : g.crisp;
  const Vector<u32>& list = wanted.empty() ? (want ? g.crisp : g.eroded) : wanted;
  const u32 b = weighted_block(kit, list, draw(seed, k_block_variant, key << 1 | 1));
  eroded = kit.blocks[b].eroded;
  return b;
}

i32 overlap(i32 a0, i32 a1, i32 b0, i32 b1) noexcept {
  return std::max(0, std::min(a1, b1) - std::max(a0, b0));
}

}  // namespace

BlockAssembler::BlockAssembler(const Kit& kit, const BlockKit& blocks) noexcept
    : kit_(kit), blocks_(blocks), assembler_(kit) {
  // The section kit's intact wall in whole courses: 2.4 m of 0.3 m courses is eight, and 2.8 m
  // is nine (2.7 m) rather than a sliver of a tenth.
  const i32 course = std::max(blocks.course_cm, 1);
  courses_ = static_cast<u32>(std::clamp((kit.wall_height_cm + course / 2) / course, 1, 255));
}

// A run of wall from `a` to `b` (centimetres along it) in the kit's straight blocks: as many as
// make the slots nearest a block the kit has, each centred in its slot. The slots are the run
// divided evenly — so the joints of one course are as regular as ashlar's, and the next course,
// whose run starts the quoin's offset along, falls between them — and a block a few centimetres
// shorter than its slot leaves a joint, a few longer overlaps its neighbour's end inside the
// wall. A sliver under half the smallest block is left open.
void BlockAssembler::fill(i32 a, i32 b) {
  const BlockKit& bk = blocks_;
  const i32 run = b - a;
  if (run <= 0 || bk.straight.empty()) return;
  const i32 smallest = bk.groups[bk.straight[0]].length_cm;
  if (2 * run < smallest) return;
  // The group nearest a slot, the longer on a tie (the list is by length ascending).
  auto nearest = [&](i32 slot, i32& deviation) {
    u32 best = bk.straight[0];
    deviation = 0x7fffffff;
    for (const u32 g : bk.straight) {
      const i32 d = std::abs(bk.groups[g].length_cm - slot);
      if (d <= deviation) {
        deviation = d;
        best = g;
      }
    }
    return best;
  };
  // How many blocks: the stretcher's count, or one either side of it when that fits the kit's
  // lengths better; the stretcher's count on a tie.
  const i32 stretcher = std::max(bk.stretcher_cm, 1);
  const i32 around = std::max(1, (run + stretcher / 2) / stretcher);
  i32 count = around;
  i32 best_deviation = 0x7fffffff;
  for (const i32 n : {around, around - 1, around + 1}) {
    if (n < 1) continue;
    i32 deviation = 0;
    (void)nearest(run / n, deviation);
    if (deviation < best_deviation) {
      best_deviation = deviation;
      count = n;
    }
  }
  const i32 base = run / count;
  const i32 rest = run % count;
  i32 at = a;
  for (i32 i = 0; i < count; ++i) {
    const i32 slot = base + (i < rest ? 1 : 0);
    i32 deviation = 0;
    const u32 g = nearest(slot, deviation);
    const i32 length = bk.groups[g].length_cm;
    // Centred in its slot, but never past the run's ends: a run can end at a jamb, where a block
    // standing a centimetre into the opening would show.
    const i32 u0 = std::max(a, std::min(at + (slot - length) / 2, b - length));
    slots_.push_back(Slot{u0, u0 + length, g, static_cast<u8>(bk.groups[g].role), 0, 0, 0});
    at += slot;
  }
}

bool BlockAssembler::assemble(const Placement& placement, TileCoord tile, BlockOutput& out,
                              std::string* error) {
  // The building itself is the section assembler's: its footprint, frame, ruin states and
  // openings. Its debris members are not wanted — the fallen blocks are the debris — so it
  // assembles at the far tier, which leaves them out and changes nothing else.
  scratch_.clear();
  Placement walls_only = placement;
  walls_only.detail = Detail::walls;
  if (!assembler_.assemble(walls_only, tile, scratch_, error)) return false;

  const BlockKit& bk = blocks_;
  const BlockRules& rules = bk.rules;
  const Assembler::Frame& frame = assembler_.frame();
  const Vector<Assembler::Side>& sides = assembler_.sides();
  const u32 wall_count = sides.size();
  const u64 seed = frame.seed;
  const u32 building = out.sites.size();
  const u32 first_block = out.blocks.size();
  const i32 course = bk.course_cm;
  const i32 ht = bk.thickness_cm / 2;
  const i32 top_cm = static_cast<i32>(courses_) * course;
  const i32 drop = static_cast<i32>(i64{rules.top_drop_q} * course / k_q_one);

  // Each wall's neighbour along its ring: the wall whose start vertex is its end.
  next_wall_.resize(wall_count);
  for (u32 w = 0, first = 0; w < wall_count; ++w) {
    if (w > 0 && sides[w].ring != sides[w - 1].ring) first = w;
    const bool last = w + 1 == wall_count || sides[w + 1].ring != sides[w].ring;
    next_wall_[w] = last ? first : w + 1;
  }

  below_.clear();
  below_first_.assign(wall_count + 1, 0u);
  corner_below_.assign(wall_count, u8{0});
  fallen_.clear();

  // ---- course by course, every wall: lay the course, decide what stands, and emit it ----------
  for (u32 c = 0; c < courses_; ++c) {
    const i32 bottom = static_cast<i32>(c) * course;
    const i32 top = bottom + course;
    // The walls at a corner take its square in turn: a wall owns its start corner on the even
    // courses and its end corner on the odd ones, so its neighbour, whose start that end is, owns
    // it on the even ones. Owning it, the course runs to the far face of the other wall, with a
    // quoin; not owning it, it stops at that wall's near face.
    const bool owns_start = (c & 1u) == 0;
    const bool owns_end = !owns_start;
    here_.clear();
    here_first_.clear();
    corner_here_.assign(wall_count, u8{0});
    for (u32 w = 0; w < wall_count; ++w) {
      here_first_.push_back(here_.size());
      const Assembler::Side& s = sides[w];
      const i32 length = s.length_cm;
      const i32 from = owns_start ? -ht : ht;
      const i32 to = owns_end ? length + ht : length - ht;

      // What is fixed in this course before the runs are filled, in order along the wall: the
      // quoins, and an opening's gap, lintel or sill.
      struct Fixed {
        i32 u0;
        i32 u1;
        i32 group;  // -1: a gap
        u8 role;
        u8 bearing;
      };
      Fixed fixed[3];
      u32 fixed_count = 0;
      if (owns_start && bk.quoin_group >= 0) {
        fixed[fixed_count++] =
            Fixed{-ht, -ht + bk.quoin_cm, bk.quoin_group, static_cast<u8>(BlockRole::quoin), 0};
      }
      i32 o0 = 0;
      i32 o1 = 0;
      if (s.opening != ~0u) {
        const Member& mb = kit_.members[s.opening];
        o0 = s.opening_at_cm + mb.opening_start_cm;
        o1 = s.opening_at_cm + mb.opening_end_cm;
        // The opening's courses: every one it crosses. Its lintel is the first course wholly
        // above its head and its sill the last wholly below its sill, so an opening is a whole
        // number of courses and never a block cut to its line.
        const i32 lintel_course = (mb.head_cm + course - 1) / course;
        const i32 sill_course = mb.sill_cm > 0 ? mb.sill_cm / course - 1 : -1;
        auto centred = [&](const Vector<u32>& groups, i32 want) {
          u32 g = groups.back();
          for (const u32 candidate : groups) {
            if (bk.groups[candidate].length_cm >= want) {
              g = candidate;
              break;
            }
          }
          const i32 len = bk.groups[g].length_cm;
          const i32 u0 = (o0 + o1 - len) / 2;
          return Fixed{u0, u0 + len, static_cast<i32>(g), static_cast<u8>(bk.groups[g].role), 0};
        };
        if (bottom < mb.head_cm && top > mb.sill_cm) {
          fixed[fixed_count++] = Fixed{o0, o1, -1, 0, 0};
        } else if (static_cast<i32>(c) == lintel_course && !bk.lintels.empty()) {
          Fixed lintel = centred(bk.lintels, o1 - o0 + 2 * rules.lintel_bearing_cm);
          lintel.bearing = 1;
          fixed[fixed_count++] = lintel;
        } else if (static_cast<i32>(c) == sill_course && !bk.sills.empty()) {
          fixed[fixed_count++] = centred(bk.sills, o1 - o0);
        }
      }
      if (owns_end && bk.quoin_group >= 0) {
        fixed[fixed_count++] = Fixed{length + ht - bk.quoin_cm, length + ht, bk.quoin_group,
                                     static_cast<u8>(BlockRole::quoin), 0};
      }
      slots_.clear();
      i32 cursor = from;
      for (u32 f = 0; f < fixed_count; ++f) {
        const Fixed& piece = fixed[f];
        // A piece that would overlap the one before it (a lintel reaching a quoin on a short
        // wall) or leave the wall is not laid; a gap still keeps its opening clear.
        if (piece.u0 < cursor || piece.u1 > to) {
          if (piece.group < 0) cursor = std::max(cursor, piece.u1);
          continue;
        }
        fill(cursor, piece.u0);
        if (piece.group >= 0) {
          slots_.push_back(Slot{piece.u0, piece.u1, static_cast<u32>(piece.group), piece.role,
                                piece.bearing, 0, 0});
        }
        cursor = piece.u1;
      }
      fill(cursor, to);

      // What stands: under the ruin rule's line, broken per block by its own draw either side
      // of it, and carried by the course below.
      const u32 below_begin = below_first_[w];
      const u32 below_end = below_first_[w + 1];
      u32 bi = below_begin;
      const u32 next = next_wall_[w];
      // The corner squares the other wall owned in the course below: carried when its block
      // there stood.
      const bool start_square = c > 0 && owns_start && corner_below_[w] != 0;
      const bool end_square = c > 0 && owns_end && corner_below_[next] != 0;
      for (u32 i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        const u64 key = u64{w} << 40 | u64{c} << 24 | i;
        const i32 centre = (slot.u0 + slot.u1) / 2;
        const i64 line =
            i64{assembler_.height_q(w, std::clamp(centre, 0, length))} * top_cm / k_q_one;
        // Where the rule keeps the wall whole, all of it stands: an intact wall's top course is
        // its coping, not a broken line. Below that, a block's own draw moves the line by up to
        // `top_drop` of a course either way, so the top of what stands is broken block by block.
        const i32 jitter = drop > 0 && line < top_cm
                               ? static_cast<i32>(pick(draw(seed, k_block_top, key),
                                                       static_cast<u32>(2 * drop + 1))) -
                                     drop
                               : 0;
        bool stands = i64{top} <= line + jitter;
        if (stands && c > 0) {
          while (bi < below_end && below_[bi].u1 <= slot.u0)
            ++bi;
          auto carried = [&](i32 a, i32 b) {
            i32 sum = 0;
            for (u32 k = bi; k < below_end && below_[k].u0 < b; ++k)
              sum += overlap(a, b, below_[k].u0, below_[k].u1);
            if (start_square) sum += overlap(a, b, -ht, ht);
            if (end_square) sum += overlap(a, b, length - ht, length + ht);
            return sum;
          };
          if (slot.bearing != 0) {
            // A lintel stands while both jambs carry it: each bearing at least half carried.
            const i32 left = o0 - slot.u0;
            const i32 right = slot.u1 - o1;
            stands = left > 0 && right > 0 && 2 * carried(slot.u0, o0) >= left &&
                     2 * carried(o1, slot.u1) >= right;
          } else {
            stands = i64{carried(slot.u0, slot.u1)} * k_q_one >=
                     i64{rules.support_q} * (slot.u1 - slot.u0);
          }
        }
        slot.stands = stands ? 1 : 0;
        if (!stands) {
          fallen_.push_back(Fallen{w, slot.group, centre, c});
          continue;
        }
        here_.push_back(Span{slot.u0, slot.u1});
        // Exposed: a quoin, the wall's top course, or the top of what stands here.
        const bool exposed = slot.role == static_cast<u8>(BlockRole::quoin) || c + 1 == courses_ ||
                             i64{top} + course > line;
        bool eroded = false;
        const u32 member = choose_block(
            bk, slot.group, exposed ? rules.eroded_exposed_q : rules.eroded_q, seed, key, eroded);
        i64 wx = 0;
        i64 wz = 0;
        assembler_.to_world_cm(s.x0 + k_dx[s.dir] * slot.u0, s.z0 + k_dz[s.dir] * slot.u0, wx, wz);
        Block block;
        block.position = Vec3{metres(wx), metres(i64{frame.base_cm} + bottom), metres(wz)};
        block.block = member;
        block.building = building;
        block.wall = static_cast<u16>(w);
        block.index = static_cast<u16>(i);
        block.course = static_cast<u8>(c);
        block.role = slot.role;
        block.yaw = static_cast<u8>((static_cast<u32>(s.dir) * 4u + frame.yaw) & 15u);
        block.flags = eroded ? k_block_eroded : u8{0};
        out.blocks.push_back(block);
      }
      // The corner squares this wall owned in this course, for the course above.
      if (!slots_.empty()) {
        if (owns_start && slots_[0].u0 <= 0) corner_here_[w] = slots_[0].stands;
        if (owns_end && slots_.back().u1 >= length) corner_here_[next] = slots_.back().stands;
      }
    }
    here_first_.push_back(here_.size());
    std::swap(below_, here_);
    std::swap(below_first_, here_first_);
    std::swap(corner_below_, corner_here_);
  }

  // ---- debris: every block that did not stand, beside its wall and never inside one ----------
  if (placement.detail == Detail::full && rules.debris_kept_q > 0) {
    const Rules& section = kit_.rules;
    const i64 tile_cm = placement.tile_cm;
    const i64 tile_x0 = i64{tile.x} * tile_cm;
    const i64 tile_z0 = i64{tile.z} * tile_cm;
    fallen_index_.assign(wall_count, u16{0});
    for (u32 g = 0; g < fallen_.size(); ++g) {
      const Fallen& fall = fallen_[g];
      if (static_cast<i32>(pick(draw(seed, k_block_kept, g), k_q_one)) >= rules.debris_kept_q)
        continue;
      const Assembler::Side& s = sides[fall.wall];
      bool eroded = false;
      const u32 member =
          choose_block(bk, fall.group, rules.eroded_exposed_q, seed, u64{g} << 3, eroded);
      const KitBlock& kb = bk.blocks[member];
      const i32 r = kb.radius_cm;
      const i32 along = std::clamp(fall.centre_cm, 0, s.length_cm);
      const u64 at = u64{g} << 3;
      const bool outward = static_cast<i32>(pick(draw(seed, k_block_fall, at | 2), k_q_one)) <
                           section.debris_outward_q;
      // Nearer the wall more often than not, as the assembler's debris: the lesser of two draws.
      const u32 spread = static_cast<u32>(section.debris_spread_cm + 1);
      const i32 dist = ht + r +
                       static_cast<i32>(std::min(pick(draw(seed, k_block_fall, at | 3), spread),
                                                 pick(draw(seed, k_block_fall, at | 5), spread)));
      const u8 n = outward ? right_of(s.dir) : left_of(s.dir);
      i32 x = s.x0 + k_dx[s.dir] * along + k_dx[n] * dist;
      i32 z = s.z0 + k_dz[s.dir] * along + k_dz[n] * dist;
      if (!grid::settle_clear(sides, ht, r, x, z)) continue;
      i64 wx = 0;
      i64 wz = 0;
      assembler_.to_world_cm(x, z, wx, wz);
      if (wx - r < tile_x0 || wx + r > tile_x0 + tile_cm || wz - r < tile_z0 ||
          wz + r > tile_z0 + tile_cm) {
        continue;
      }
      // Lying on the ground where it lands, turned by its own draw; its frame's origin is its
      // start end, half its length back from where its centre lies.
      const f32 ground = placement.ground.at(metres(wx), metres(wz));
      const u8 yaw = static_cast<u8>(pick(draw(seed, k_block_fall, at | 4), 16));
      i64 ox = 0;
      i64 oz = 0;
      grid::rotate_cm(yaw, kb.length_cm / 2, 0, ox, oz);
      Block block;
      block.position = Vec3{
          metres(wx - ox), metres(static_cast<i64>(std::floor(ground * 100.0f)) - section.embed_cm),
          metres(wz - oz)};
      block.block = member;
      block.building = building;
      block.wall = static_cast<u16>(fall.wall);
      block.index = fallen_index_[fall.wall]++;
      block.course = static_cast<u8>(fall.course);
      block.role = static_cast<u8>(kb.role);
      block.yaw = yaw;
      block.flags = static_cast<u8>(k_block_fallen | (eroded ? k_block_eroded : 0));
      out.blocks.push_back(block);
    }
  }

  // ---- the sand and the site: the assembler's, pointed at the blocks
  // -----------------------------
  const u32 first_drift = out.drifts.size();
  for (Drift d : scratch_.drifts) {
    d.building = building;
    out.drifts.push_back(d);
  }
  Site site = scratch_.sites[0];
  site.first_instance = first_block;
  site.instance_count = out.blocks.size() - first_block;
  site.first_drift = first_drift;
  site.drift_count = out.drifts.size() - first_drift;
  out.sites.push_back(site);
  return true;
}

bool assemble_block_tiles(const Kit& kit, const BlockKit& blocks, const Placement& placement,
                          std::span<const TileCoord> tiles, jobs::JobSystem* jobs, BlockOutput& out,
                          std::string* error) {
  const u32 count = static_cast<u32>(tiles.size());
  const u32 workers = jobs != nullptr ? jobs->worker_count(jobs::Pool::Performance) + 1 : 1;
  if (jobs == nullptr || workers <= 1 || count < 2) {
    BlockAssembler assembler(kit, blocks);
    for (const TileCoord& t : tiles) {
      if (!assembler.assemble(placement, t, out, error)) return false;
    }
    return true;
  }
  // Contiguous runs of tiles, one output each, joined in tile order: the result is the one a
  // single thread writes, whatever the thread count.
  const u32 runs = std::min(count, workers * 4);
  Vector<BlockOutput> parts(runs);
  Vector<std::string> errors(runs);
  Vector<u8> ok(runs, u8{1});
  jobs->parallel_for(jobs::Pool::Performance, runs, 1, [&](u32 begin, u32 end) {
    for (u32 r = begin; r < end; ++r) {
      BlockAssembler assembler(kit, blocks);
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
  u32 total_blocks = out.blocks.size();
  u32 total_drifts = out.drifts.size();
  u32 total_sites = out.sites.size();
  for (const BlockOutput& p : parts) {
    total_blocks += p.blocks.size();
    total_drifts += p.drifts.size();
    total_sites += p.sites.size();
  }
  out.blocks.reserve(total_blocks);
  out.drifts.reserve(total_drifts);
  out.sites.reserve(total_sites);
  for (BlockOutput& p : parts) {
    const u32 site_base = out.sites.size();
    const u32 block_base = out.blocks.size();
    const u32 drift_base = out.drifts.size();
    for (Block b : p.blocks) {
      b.building += site_base;
      out.blocks.push_back(b);
    }
    for (Drift d : p.drifts) {
      d.building += site_base;
      out.drifts.push_back(d);
    }
    for (Site s : p.sites) {
      s.first_instance += block_base;
      s.first_drift += drift_base;
      out.sites.push_back(s);
    }
  }
  return true;
}

u64 hash_blocks(const BlockOutput& out) noexcept {
  u64 h = hash_combine(k_hash_seed, out.sites.size());
  auto f = [](f32 v) { return static_cast<u64>(std::bit_cast<u32>(v)); };
  for (const Site& s : out.sites) {
    h = hash_combine(h, s.seed);
    h = hash_combine(
        h, static_cast<u64>(static_cast<u32>(s.tile.x)) << 32 | static_cast<u32>(s.tile.z));
    h = hash_combine(h, f(s.origin.x) << 32 | f(s.origin.y));
    h = hash_combine(h, f(s.origin.z));
    h = hash_combine(h, static_cast<u64>(s.first_instance) << 32 | s.instance_count);
    h = hash_combine(h, static_cast<u64>(s.first_drift) << 32 | s.drift_count);
    h = hash_combine(h, static_cast<u64>(s.walls) << 16 | static_cast<u64>(s.shape) << 8 | s.yaw);
  }
  h = hash_combine(h, out.blocks.size());
  for (const Block& b : out.blocks) {
    h = hash_combine(h, f(b.position.x) << 32 | f(b.position.y));
    h = hash_combine(h, f(b.position.z) << 32 | b.block);
    h = hash_combine(h,
                     static_cast<u64>(b.building) << 32 | static_cast<u64>(b.wall) << 16 | b.index);
    h = hash_combine(h, static_cast<u64>(b.course) << 24 | static_cast<u64>(b.role) << 16 |
                            static_cast<u64>(b.yaw) << 8 | b.flags);
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

u32 block_yaw_step(const BlockKit& kit, const Block& block) noexcept {
  return (static_cast<u32>(block.yaw) + kit.blocks[block.block].yaw_step) & 15u;
}

Vec3 block_translation(const BlockKit& kit, const Block& block) noexcept {
  const KitBlock& kb = kit.blocks[block.block];
  const u32 step = block_yaw_step(kit, block);
  const f32 c = step_cos(step);
  const f32 s = step_sin(step);
  const Vec3 o = kb.offset;
  return Vec3{block.position.x + (c * o.x + s * o.z), block.position.y + o.y,
              block.position.z + (c * o.z - s * o.x)};
}

}  // namespace engine::ruins
