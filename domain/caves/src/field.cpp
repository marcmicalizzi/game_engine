// The void's field (field.h): the sections, the union, the floors, the walls' detail, the ground
// under a mouth.
#include "field.h"

#include <algorithm>

namespace engine::caves::detail {

namespace {

constexpr f64 mm(i64 v) noexcept { return static_cast<f64>(v) / 1000.0; }

// A first-order distance to an ellipsoid (exact to first order at its surface, which is where a
// vertex is placed; a bound elsewhere): products, sums, two square roots and a division.
f64 ellipsoid(f64 x, f64 y, f64 z, f64 rx, f64 ry, f64 rz) noexcept {
  const f64 px = x / rx, py = y / ry, pz = z / rz;
  const f64 k0 = std::sqrt(px * px + py * py + pz * pz);
  const f64 qx = px / rx, qy = py / ry, qz = pz / rz;
  const f64 k1 = std::sqrt(qx * qx + qy * qy + qz * qz);
  if (k1 < 1e-9) return -std::min(rx, std::min(ry, rz));
  return k0 * (k0 - 1.0) / k1;
}

f64 rounded_box(f64 x, f64 y, f64 z, f64 hx, f64 hy, f64 hz, f64 r) noexcept {
  const f64 qx = std::fabs(x) - hx + r, qy = std::fabs(y) - hy + r, qz = std::fabs(z) - hz + r;
  const f64 ox = std::max(qx, 0.0), oy = std::max(qy, 0.0), oz = std::max(qz, 0.0);
  return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max(qx, std::max(qy, qz)), 0.0) - r;
}

// Smooth value noise over the plan, [-1, 1], from an integer hash: the sediment's gentle relief.
f64 plan_noise(u64 seed, f64 x, f64 z, f64 period) noexcept {
  const f64 fx = x / period, fz = z / period;
  const f64 ix = std::floor(fx), iz = std::floor(fz);
  const f64 tx = smooth01(fx - ix), tz = smooth01(fz - iz);
  const auto at = [&](f64 cx, f64 cz) {
    return unit(hash3(seed, static_cast<u64>(static_cast<i64>(cx)),
                      static_cast<u64>(static_cast<i64>(cz)), 0x5ED1u)) *
               2.0 -
           1.0;
  };
  const f64 a = lerp(at(ix, iz), at(ix + 1.0, iz), tx);
  const f64 b = lerp(at(ix, iz + 1.0), at(ix + 1.0, iz + 1.0), tx);
  return lerp(a, b, tz);
}

struct SegmentEval {
  f64 f = k_far;      // the section's distance, rock positive
  f64 fill = -k_far;  // the floor's height over the point (positive: under the floor)
  f64 t = 0.0;
  f64 x = 0.0, y = 0.0;  // the point in the tube's section
  f64 s = 0.0;           // metres along the edge
  bool scallops = false;
  u32 cell = 0;
};

SegmentEval eval_segment(const Field& field, const FieldSegment& seg, V3 p) noexcept {
  const FieldStation& s0 = field.stations[seg.i0];
  const FieldStation& s1 = field.stations[seg.i1];
  SegmentEval e;
  const V3 r = p - s0.p;
  const f64 along = dot(r, seg.d);
  const f64 t = clamp01(along / seg.length);
  const V3 q = r - seg.d * (t * seg.length);
  const f64 z = along < 0.0 ? along : (along > seg.length ? along - seg.length : 0.0);
  const f64 a = lerp(s0.a, s1.a, t), b = lerp(s0.b, s1.b, t);
  const V3 across = normalized(lerp(s0.across, s1.across, t), s0.across);
  const V3 up = normalized(lerp(s0.up, s1.up, t), s0.up);
  e.t = t;
  e.x = dot(q, across);
  e.y = dot(q, up);
  e.s = lerp(s0.s, s1.s, t) + z;
  if (a > 0.05 && b > 0.05) e.f = ellipsoid(e.x, e.y, z, a, b, std::min(a, b));
  const f64 hw = lerp(s0.hw, s1.hw, t);
  if (hw > 0.05) {
    const V3 level = normalized(lerp(s0.level, s1.level, t), s0.level);
    const f64 top = lerp(s0.top, s1.top, t), bot = lerp(s0.bot, s1.bot, t);
    const f64 slot =
        rounded_box(dot(q, level), q.y - 0.5 * (top + bot), z, hw, 0.5 * (top - bot), hw, 0.8 * hw);
    e.f = std::min(e.f, slot);
  }
  if (!s0.shaft && !s1.shaft) {
    const f64 floor_y =
        lerp(s0.floor_y, s1.floor_y, t) + 0.05 * plan_noise(field.seed, p.x, p.z, 2.5);
    e.fill = floor_y - p.y;
  }
  e.scallops = s0.scallops && s1.scallops;
  e.cell = t < 0.5 ? s0.cell : s1.cell;
  return e;
}

struct HallEval {
  f64 f = k_far;
  f64 fill = -k_far;
};

HallEval eval_hall(const FieldHall& h, V3 p) noexcept {
  HallEval e;
  const V3 r = p - h.c;
  const f64 lx = dot(r, h.axis), lz = dot(r, h.perp);
  e.f = ellipsoid(lx, r.y, lz, h.ra, h.rh, h.rb);
  const f64 ux = lx / h.ra, uz = lz / h.rb;
  const f64 rho = std::sqrt(ux * ux + uz * uz);
  // The pile: a cone at about the angle of repose, and slabs a bed thick on it, each a hashed
  // step, lower toward the walls (docs/experiments/cave-networks.md, "Halls").
  const f64 cone = h.pile * std::max(0.0, 1.0 - rho / 0.9);
  const f64 bx = std::floor(lx / 1.4), bz = std::floor(lz / 1.1);
  const f64 step = unit(hash3(h.seed, static_cast<u64>(static_cast<i64>(bx)),
                              static_cast<u64>(static_cast<i64>(bz)), 0xB10Cu));
  const f64 block = (0.1 + 0.6 * step) * clamp01(1.25 - rho);
  e.fill = h.floor_y + cone + block - p.y;
  return e;
}

// The integral of the beds' resistance from the sequence's base to bed coordinate `x`, through
// whole periods.
f64 resistance_integral(const Field& field, f64 x) noexcept {
  const gfx::FormationSequence& s = field.beds;
  const f64 period = static_cast<f64>(s.period_mm) / 1000.0;
  const f64 k = std::floor(x / period);
  const f64 y = x - k * period;
  u32 i = 0;
  while (i + 1 < s.count && y >= static_cast<f64>(s.beds[i + 1].start_m))
    ++i;
  return k * field.bed_total + field.bed_prefix[i] +
         (y - static_cast<f64>(s.beds[i].start_m)) * static_cast<f64>(s.beds[i].resistance);
}

// How far the rock at the point stands out of a wall (positive) or back (negative), in units of
// the ledge: the beds' resistance averaged across two lattice spacings of bed coordinate (a box,
// as the terrain's bands are filtered by a pixel), so a bed thinner than the lattice can carry
// fades to the mean of its neighbours instead of aliasing into slots a cube cannot hold.
f64 ledge_delta(const Field& field, V3 p) noexcept {
  if (field.beds.count == 0 || field.ledge <= 0.0) return 0.0;
  const f64 c = gfx::formation_coordinate(field.frame, field.anchor_m.x + p.x,
                                          field.anchor_m.y + p.y, field.anchor_m.z + p.z)
                    .coordinate;
  const f64 width = std::max(0.16, 2.0 * static_cast<f64>(field.voxel_mm) / 1000.0);
  const f64 mean =
      (resistance_integral(field, c + 0.5 * width) - resistance_integral(field, c - 0.5 * width)) /
      width;
  return 2.0 * mean - 1.0;
}

// Scallops: bowls dissolved into the wall, one a cell of a lattice laid along the flow, each steep
// on its upstream side and long on its downstream one, the deepest of the bowls a point is in
// (so two bowls meet in a crest). Negative: the rock is dissolved back.
f64 scallop_delta(const Field& field, u32 edge, f64 s_along, f64 x, f64 y) noexcept {
  const f64 len = field.scallop;
  if (len <= 0.0) return 0.0;
  const f64 wide = 0.7 * len;
  const f64 xi = s_along / len, eta = x / wide, zeta = y / wide;
  const f64 ci = std::floor(xi), cj = std::floor(eta), ck = std::floor(zeta);
  const u64 seed = hash_combine(field.seed, 0x5CA110u + edge);
  f64 best = 0.0;
  for (i32 di = -1; di <= 1; ++di) {
    for (i32 dj = -1; dj <= 1; ++dj) {
      for (i32 dk = -1; dk <= 1; ++dk) {
        const f64 cx = ci + di, cy = cj + dj, cz = ck + dk;
        const u64 h =
            hash3(seed, static_cast<u64>(static_cast<i64>(cx)),
                  static_cast<u64>(static_cast<i64>(cy)), static_cast<u64>(static_cast<i64>(cz)));
        const f64 dx = xi - (cx + 0.2 + 0.6 * unit(h));
        const f64 dy = eta - (cy + 0.2 + 0.6 * unit(mix64(h + 1)));
        const f64 dz = zeta - (cz + 0.2 + 0.6 * unit(mix64(h + 2)));
        const f64 sx = dx < 0.0 ? dx / 0.42 : dx / 0.78;
        const f64 r2 = sx * sx + (dy * dy + dz * dz) / (0.62 * 0.62);
        if (r2 < 1.0) best = std::max(best, 1.0 - r2);
      }
    }
  }
  return -0.12 * len * best;
}

void grow(V3& lo, V3& hi, V3 p, f64 r) noexcept {
  lo.x = std::min(lo.x, p.x - r);
  lo.y = std::min(lo.y, p.y - r);
  lo.z = std::min(lo.z, p.z - r);
  hi.x = std::max(hi.x, p.x + r);
  hi.y = std::max(hi.y, p.y + r);
  hi.z = std::max(hi.z, p.z + r);
}

f64 station_reach(const FieldStation& s) noexcept {
  return std::max(std::max(s.a, s.b), std::max(s.hw, std::max(std::fabs(s.top), std::fabs(s.bot))));
}

bool inside(V3 lo, V3 hi, V3 p) noexcept {
  return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
}

}  // namespace

f64 scallop_length(f64 flow_speed_m_s, f64 voxel_m) noexcept {
  // Curl (1966): v L / nu = 22,500; nu = 1.307e-6 m2/s, water at 10 C. A lattice of spacing h
  // carries nothing shorter than 2.4 h, and what is shorter is the material's.
  const f64 curl = 22500.0 * 1.307e-6 / flow_speed_m_s;
  return std::max(curl, 2.4 * voxel_m);
}

void Field::set_beds(const gfx::FormationSequence& sequence) noexcept {
  beds = sequence;
  bed_total = 0.0;
  for (u32 i = 0; i < beds.count && i < gfx::k_formation_max_beds; ++i) {
    bed_prefix[i] = bed_total;
    bed_total +=
        static_cast<f64>(beds.beds[i].thickness_m) * static_cast<f64>(beds.beds[i].resistance);
  }
}

void Field::add_station(const CellStation& s, V3 bed_normal, V3 strike) {
  FieldStation f;
  f.p = {mm(s.x - ax), mm(s.y - ay), mm(s.z - az)};
  f.tangent = normalized({static_cast<f64>(s.tx) * k_q20, static_cast<f64>(s.ty) * k_q20,
                          static_cast<f64>(s.tz) * k_q20},
                         {1.0, 0.0, 0.0});
  f.shaft = s.frame == SectionFrame::Shaft;
  f.level = normalized(cross(V3{0.0, 1.0, 0.0}, f.tangent), strike);
  if (f.shaft) {
    f.across = strike;
  } else if (s.frame == SectionFrame::Plumb) {
    f.across = f.level;
  } else {
    f.across = normalized(cross(bed_normal, f.tangent), f.level);
  }
  f.up = normalized(cross(f.tangent, f.across), bed_normal);
  f.a = mm(s.a);
  f.b = mm(s.b);
  f.hw = mm(s.slot_hw);
  f.top = mm(s.slot_top);
  f.bot = mm(s.slot_bot);
  f.floor_y = mm(s.floor - ay);
  f.s = mm(s.s);
  f.cell = s.cell;
  f.edge = s.edge;
  f.index = s.index;
  f.scallops = (s.flags & 1u) != 0;
  stations.push_back(f);
}

void Field::add_hall(const CellHall& h) {
  FieldHall f;
  f.c = {mm(h.x - ax), mm(h.y - ay), mm(h.z - az)};
  f.ra = mm(h.ra);
  f.rb = mm(h.rb);
  f.rh = mm(h.rh);
  f.axis = normalized({static_cast<f64>(h.axis_x) * k_q20, 0.0, static_cast<f64>(h.axis_z) * k_q20},
                      {1.0, 0.0, 0.0});
  f.perp = {-f.axis.z, 0.0, f.axis.x};
  f.floor_y = mm(h.floor - ay);
  f.pile = mm(h.pile);
  f.cell = h.cell;
  f.seed = h.seed;
  f.id = (u64{1} << 63) | static_cast<u64>(halls.size());
  const f64 r = std::max(f.ra, f.rb) + margin;
  f.lo = {f.c.x - r, f.c.y - f.rh - margin, f.c.z - r};
  f.hi = {f.c.x + r, f.c.y + f.rh + margin, f.c.z + r};
  halls.push_back(f);
}

void Field::finish() {
  segments.clear();
  for (u32 i = 0; i + 1 < stations.size(); ++i) {
    const FieldStation& s0 = stations[i];
    const FieldStation& s1 = stations[i + 1];
    if (s0.edge != s1.edge || s1.index != s0.index + 1) continue;
    FieldSegment seg;
    seg.i0 = i;
    seg.i1 = i + 1;
    const V3 d = s1.p - s0.p;
    seg.length = length(d);
    if (seg.length < 1e-6) continue;
    seg.d = d * (1.0 / seg.length);
    seg.lo = seg.hi = s0.p;
    grow(seg.lo, seg.hi, s0.p, station_reach(s0) + margin);
    grow(seg.lo, seg.hi, s1.p, station_reach(s1) + margin);
    seg.id = (static_cast<u64>(s0.edge) << 32) | s0.index;
    segments.push_back(seg);
  }
  bin_first.clear();
  bin_items.clear();
  bins_x = bins_y = bins_z = 0;
  const u32 count = segments.size() + halls.size();
  if (count == 0) return;
  V3 lo{k_far, k_far, k_far}, hi{-k_far, -k_far, -k_far};
  const auto bounds = [&](u32 item, V3& a, V3& b) {
    if (item < segments.size()) {
      a = segments[item].lo;
      b = segments[item].hi;
    } else {
      a = halls[item - segments.size()].lo;
      b = halls[item - segments.size()].hi;
    }
  };
  for (u32 item = 0; item < count; ++item) {
    V3 a, b;
    bounds(item, a, b);
    grow(lo, hi, a, 0.0);
    grow(lo, hi, b, 0.0);
  }
  bin_lo = lo;
  bins_x = static_cast<i32>(std::floor((hi.x - lo.x) / bin_size)) + 1;
  bins_y = static_cast<i32>(std::floor((hi.y - lo.y) / bin_size)) + 1;
  bins_z = static_cast<i32>(std::floor((hi.z - lo.z) / bin_size)) + 1;
  const u32 bins = static_cast<u32>(bins_x) * static_cast<u32>(bins_y) * static_cast<u32>(bins_z);
  bin_first.assign(bins + 1, 0u);
  const auto range = [&](u32 item, i32 r[6]) {
    V3 a, b;
    bounds(item, a, b);
    r[0] = static_cast<i32>(std::floor((a.x - lo.x) / bin_size));
    r[1] = static_cast<i32>(std::floor((a.y - lo.y) / bin_size));
    r[2] = static_cast<i32>(std::floor((a.z - lo.z) / bin_size));
    r[3] = std::min(bins_x - 1, static_cast<i32>(std::floor((b.x - lo.x) / bin_size)));
    r[4] = std::min(bins_y - 1, static_cast<i32>(std::floor((b.y - lo.y) / bin_size)));
    r[5] = std::min(bins_z - 1, static_cast<i32>(std::floor((b.z - lo.z) / bin_size)));
  };
  const auto bin_of = [&](i32 x, i32 y, i32 z) {
    return (static_cast<u32>(z) * static_cast<u32>(bins_y) + static_cast<u32>(y)) *
               static_cast<u32>(bins_x) +
           static_cast<u32>(x);
  };
  for (u32 pass = 0; pass < 2; ++pass) {
    for (u32 item = 0; item < count; ++item) {
      i32 r[6];
      range(item, r);
      for (i32 z = r[2]; z <= r[5]; ++z)
        for (i32 y = r[1]; y <= r[4]; ++y)
          for (i32 x = r[0]; x <= r[3]; ++x) {
            const u32 bin = bin_of(x, y, z);
            if (pass == 0) {
              ++bin_first[bin + 1];
            } else {
              bin_items[bin_first[bin]++] = item;
            }
          }
    }
    if (pass == 0) {
      for (u32 b = 0; b < bins; ++b)
        bin_first[b + 1] += bin_first[b];
      bin_items.assign(bin_first[bins], 0u);
    } else {
      // The fill advanced every start to its end: shift them back.
      for (u32 b = bins; b > 0; --b)
        bin_first[b] = bin_first[b - 1];
      bin_first[0] = 0;
    }
  }
}

bool Field::ground_at(f64 x, f64 z, f64& out) const noexcept {
  if (patch == nullptr || patch_nx < 2 || patch_nz < 2) return false;
  const f64 step = static_cast<f64>(voxel_mm) / 1000.0;
  // The lattice's indices count from the anchor, as the point's metres do.
  const f64 fx = x / step - static_cast<f64>(patch_i);
  const f64 fz = z / step - static_cast<f64>(patch_j);
  if (!(fx >= 0.0 && fz >= 0.0 && fx <= static_cast<f64>(patch_nx - 1) &&
        fz <= static_cast<f64>(patch_nz - 1)))
    return false;
  const u32 i = std::min(patch_nx - 2, static_cast<u32>(fx));
  const u32 j = std::min(patch_nz - 2, static_cast<u32>(fz));
  const i32 h00 = patch[j * patch_nx + i], h10 = patch[j * patch_nx + i + 1];
  const i32 h01 = patch[(j + 1) * patch_nx + i], h11 = patch[(j + 1) * patch_nx + i + 1];
  if (h00 == k_no_ground || h10 == k_no_ground || h01 == k_no_ground || h11 == k_no_ground)
    return false;
  const f64 tx = fx - static_cast<f64>(i), tz = fz - static_cast<f64>(j);
  out = lerp(lerp(mm(h00), mm(h10), tx), lerp(mm(h01), mm(h11), tx), tz);
  return true;
}

FieldSample Field::sample(V3 p) const noexcept {
  FieldSample out;
  struct Candidate {
    f64 f, fill;
    u64 id;
    u32 prim, cell;
    f64 t, x, y, s;
    bool scallops, hall;
    u32 edge;
  };
  constexpr u32 k_most = 64;
  Candidate cands[k_most];
  u32 count = 0;
  if (bins_x > 0) {
    const i32 bx = static_cast<i32>(std::floor((p.x - bin_lo.x) / bin_size));
    const i32 by = static_cast<i32>(std::floor((p.y - bin_lo.y) / bin_size));
    const i32 bz = static_cast<i32>(std::floor((p.z - bin_lo.z) / bin_size));
    if (bx >= 0 && by >= 0 && bz >= 0 && bx < bins_x && by < bins_y && bz < bins_z) {
      const u32 bin = (static_cast<u32>(bz) * static_cast<u32>(bins_y) + static_cast<u32>(by)) *
                          static_cast<u32>(bins_x) +
                      static_cast<u32>(bx);
      for (u32 k = bin_first[bin]; k < bin_first[bin + 1] && count < k_most; ++k) {
        const u32 item = bin_items[k];
        if (item < segments.size()) {
          const FieldSegment& seg = segments[item];
          if (!inside(seg.lo, seg.hi, p)) continue;
          const SegmentEval e = eval_segment(*this, seg, p);
          cands[count++] =
              Candidate{e.f, e.fill, seg.id, item,       e.cell, e.t,
                        e.x, e.y,    e.s,    e.scallops, false,  stations[seg.i0].edge};
        } else {
          const FieldHall& h = halls[item - segments.size()];
          if (!inside(h.lo, h.hi, p)) continue;
          const HallEval e = eval_hall(h, p);
          cands[count++] = Candidate{e.f,    e.fill, h.id, 0x80000000u | (item - segments.size()),
                                     h.cell, 0.0,    0.0,  0.0,
                                     0.0,    false,  true, 0u};
        }
      }
    }
  }
  if (count > 0) {
    u32 owner = 0;
    for (u32 k = 1; k < count; ++k)
      if (cands[k].f < cands[owner].f ||
          (cands[k].f == cands[owner].f && cands[k].id < cands[owner].id))
        owner = k;
    out.raw = cands[owner].f;
    // The walls' detail, the same for every candidate at the point: the owner's.
    f64 delta = 0.0;
    if (std::fabs(out.raw) < 1.0) {
      const Candidate& c = cands[owner];
      delta = ledge * (c.hall ? 2.2 : 1.0) * ledge_delta(*this, p);
      if (c.scallops) delta += scallop_delta(*this, c.edge, c.s, c.x, c.y);
    }
    u32 best = 0;
    f64 best_f = k_far;
    f64 value[k_most];
    u32 order[k_most];
    for (u32 k = 0; k < count; ++k) {
      const f64 v = std::max(cands[k].f + delta, cands[k].fill);
      value[k] = v;
      if (k == 0 || v < best_f || (v == best_f && cands[k].id < cands[best].id)) {
        best = k;
        best_f = v;
      }
      // By group (an edge, a hall), then by id: the order the union below is folded in, whatever
      // order the bins handed the candidates over in.
      u32 at = k;
      while (at > 0) {
        const Candidate& before = cands[order[at - 1]];
        const u32 pg = before.hall ? before.prim : before.edge;
        const u32 kg = cands[k].hall ? cands[k].prim : cands[k].edge;
        if (pg < kg || (pg == kg && before.id < cands[k].id)) break;
        order[at] = order[at - 1];
        --at;
      }
      order[at] = k;
    }
    // The union: within an edge the nearest of its segments; between edges and halls a smooth
    // minimum two lattice spacings wide, so two voids that meet at a shallow angle meet in a
    // fillet and the blade of rock between them ends where it would be thinner than the lattice
    // can carry, instead of in a knife edge the surface cannot hold.
    const f64 blend = 2.0 * static_cast<f64>(voxel_mm) / 1000.0;
    f64 folded = k_far;
    for (u32 k = 0; k < count;) {
      const Candidate& head = cands[order[k]];
      const u32 group = head.hall ? head.prim : head.edge;
      f64 nearest = value[order[k]];
      u32 next = k + 1;
      while (next < count) {
        const Candidate& c = cands[order[next]];
        if ((c.hall ? c.prim : c.edge) != group) break;
        nearest = std::min(nearest, value[order[next]]);
        ++next;
      }
      if (k == 0) {
        folded = nearest;
      } else {
        const f64 h = std::max(blend - std::fabs(folded - nearest), 0.0) / blend;
        folded = std::min(folded, nearest) - 0.25 * h * h * blend;
      }
      k = next;
    }
    out.void_f = folded;
    out.cell = cands[best].cell;
    out.prim = cands[best].prim;
    out.along = static_cast<f32>(cands[best].t);
    out.cls = cands[best].fill > cands[best].f + delta
                  ? (cands[best].hall ? k_surface_breakdown : k_surface_sediment)
                  : k_surface_rock;
    if (has_water && water_y - p.y > out.void_f) {
      out.void_f = water_y - p.y;
      out.cls = k_surface_water;
    }
  }
  out.phi = out.void_f;
  f64 h = 0.0;
  if (ground_at(p.x, p.z, h)) {
    // The mesh's ground stands 3 cm over the terrain's at the rim and sinks 6 cm under it at the
    // apron's edge, so the lip covers the hole's teeth and its own edge is never seen.
    const f64 lift =
        apron > 0.0 ? 0.03 - 0.09 * smooth01((out.raw - 0.5 * apron) / (0.5 * apron)) : 0.0;
    const f64 rock = h + lift - p.y;
    if (rock < out.void_f) {
      out.phi = rock;
      out.ground = true;
    }
  }
  return out;
}

}  // namespace engine::caves::detail
