// What the water's description draws, measured (W91; channels.md, "Against the proof of
// concept"): the falls the flowing water will draw and the ribbon edges that stand over the ground.
#include "detail.h"

#include <algorithm>
#include <cmath>

namespace engine::channels {
namespace {
using namespace detail;

f64 number_of(const JsonValue& v) {
  f64 out = 0;
  return v.get_f64(out) ? out : 0.0;
}
}  // namespace

WaterStats measure_water(const JsonValue& blocks, const scene_gen::Ground& ground,
                         const scene_gen::Exclusion* water, const f64* region, f64 border_m) {
  WaterStats out;
  if (!blocks.is_array()) return out;
  for (const JsonValue& block : blocks.as_array()) {
    const JsonValue* points = block.find("points");
    const JsonValue* widths = block.find("width_m");
    if (points == nullptr || widths == nullptr || !points->is_array() || !widths->is_array())
      continue;
    const JsonValue* reach = block.find("reach");
    const u32 m = points->as_array().size();
    if (m < 2 || widths->as_array().size() != m) continue;
    Vector<f64> x(m), y(m), z(m), w(m);
    Vector<u8> r(m, 0);
    for (u32 i = 0; i < m; ++i) {
      const JsonValue& p = (*points)[i];
      x[i] = number_of(p.as_array()[0]);
      y[i] = number_of(p.as_array()[1]);
      z[i] = number_of(p.as_array()[2]);
      w[i] = number_of((*widths)[i]);
      if (reach != nullptr && reach->is_array() && i < reach->as_array().size())
        r[i] = static_cast<u8>(std::min(6.0, number_of((*reach)[i])));
    }
    out.points += m;
    if (const JsonValue* depths = block.find("depth_m"); depths != nullptr && depths->is_array()) {
      for (const JsonValue& d : depths->as_array()) {
        if (number_of(d) > 0) continue;
        if (out.depths_not_positive++ == 0) {
          const JsonValue* first = block.find("name");
          if (first != nullptr && first->is_string())
            out.depth_block = std::string(first->as_string());
        }
      }
    }
    bool in_fall = false;
    for (u32 i = 0; i + 1 < m; ++i) {
      const f64 run = std::hypot(x[i + 1] - x[i], z[i + 1] - z[i]);
      const f64 drop = y[i] - y[i + 1];
      ++out.stretches;
      ++out.stretches_by_reach[r[i]];
      // water::stretch_falls: a drop of at least the horizontal run.
      const bool falls = drop > 0 && drop >= run;
      if (falls) {
        ++out.fall_stretches;
        ++out.fall_stretches_by_reach[r[i]];
        out.highest_fall_m = std::max(out.highest_fall_m, drop);
        if (!in_fall) ++out.falls;
      }
      in_fall = falls;
    }
    const JsonValue* name = block.find("name");
    for (u32 i = 0; i < m; ++i) {
      if (water != nullptr && water->body_count() > 0 &&
          water->excluded_mm(ground, scene_gen::nearest_mm(x[i]), scene_gen::nearest_mm(z[i]),
                             scene_gen::k_exclude_water))
        continue;
      // The heading as the water's mesher takes it: the mean of the two stretches' directions.
      f64 hx = 0, hz = 0;
      for (const u32 a : {i > 0 ? i - 1 : 0u, i}) {
        const u32 b = std::min(a + 1, m - 1);
        const f64 dx = x[b] - x[a], dz = z[b] - z[a], l = std::hypot(dx, dz);
        if (a != b && l > 0) {
          hx += dx / l;
          hz += dz / l;
        }
      }
      const f64 hl = std::hypot(hx, hz);
      if (!(hl > 0)) continue;
      const f64 ax = -hz / hl, az = hx / hl, e = k_ribbon_half_widths * w[i];
      const bool border =
          region != nullptr && (x[i] - region[0] < border_m || z[i] - region[1] < border_m ||
                                region[2] - x[i] < border_m || region[3] - z[i] < border_m);
      const std::string block_name =
          name != nullptr && name->is_string() ? std::string(name->as_string()) : std::string();
      for (const f64 side : {-1.0, 1.0}) {
        const f64 ex = x[i] + side * ax * e, ez = z[i] + side * az * e;
        const f64 over =
            y[i] - ground.surface_mm(scene_gen::nearest_mm(ex), scene_gen::nearest_mm(ez));
        if (border) {
          if (over > .05) {
            ++out.border_edges_above_005;
            if (std::find(out.border_blocks.begin(), out.border_blocks.end(), block_name) ==
                out.border_blocks.end())
              out.border_blocks.push_back(block_name);
          }
          out.border_edge_max_m = std::max(out.border_edge_max_m, over);
          continue;
        }
        ++out.edges;
        out.edges_above_005 += over > .05 ? 1u : 0u;
        out.edges_above_03 += over > .3 ? 1u : 0u;
        if (over > out.edge_max_m) {
          out.edge_max_m = over;
          out.edge_max_at = WorldPos{ex, y[i], ez};
          out.edge_max_block = block_name;
        }
      }
    }
  }
  return out;
}

SeatStats measure_seat(const ChannelLayer& p, const ChannelNetwork& n,
                       const scene_gen::Ground& ground) {
  SeatStats out;
  const auto g_at = [&](f64 x, f64 z) {
    return ground.surface_mm(scene_gen::nearest_mm(x), scene_gen::nearest_mm(z));
  };
  for (const ChannelLink& link : n.links) {
    const u32 m = std::min<u32>(link.keep, link.line.size());
    if (m < 3 || link.bed.size() != link.line.size()) continue;
    Vector<f64> s(link.line.size(), 0.0);
    for (u32 i = 1; i < link.line.size(); ++i)
      s[i] = s[i - 1] + std::hypot(link.line[i].at.x - link.line[i - 1].at.x,
                                   link.line[i].at.z - link.line[i - 1].at.z);
    for (u32 i = 0; i < m; ++i) {
      const ChannelPoint& pt = link.line[i];
      const f64 w = wide(pt.width_m);
      if (w < wide(p.bar_min_width_m) || (i < link.knick.size() && link.knick[i])) continue;
      // The heading over a width: the chord between the points half a width either way.
      u32 a = i, b = i;
      while (a > 0 && s[i] - s[a] < .5 * w)
        --a;
      while (b + 1 < m && s[b] - s[i] < .5 * w)
        ++b;
      const f64 tx = link.line[b].at.x - link.line[a].at.x;
      const f64 tz = link.line[b].at.z - link.line[a].at.z;
      const f64 tl = std::hypot(tx, tz);
      if (!(tl > 0)) continue;
      const f64 lx = -tz / tl, lz = tx / tl;  // the left, (-dz, dx)
      const f64 seat = i < link.seat.size() ? wide(link.seat[i]) : 0.0;
      const f64 top = pt.at.y - seat;
      const BendSection section =
          bend_section(p, w, wide(pt.depth_m), i < link.asym.size() ? wide(link.asym[i]) : 0.0,
                       i < link.swell.size() ? wide(link.swell[i]) : 0.0);
      const f64 half = .5 * section.width;
      const f64 lower = std::min(g_at(pt.at.x + lx * half, pt.at.z + lz * half),
                                 g_at(pt.at.x - lx * half, pt.at.z - lz * half));
      const f64 run = i + 1 < m ? s[i + 1] - s[i] : 0.0;
      ++out.points;
      if (seat > 0) out.seated_line_m += run;
      out.seat_max_m = std::max(out.seat_max_m, seat);
      const f64 over = top - lower;
      if (over > k_seat_bound_m) {
        ++out.bank_over;
        out.bank_over_line_m += run;
      }
      if (over > out.bank_over_m) {
        out.bank_over_m = over;
        out.bank_over_at = WorldPos{pt.at.x, top, pt.at.z};
      }
      // The flood bed: its level a share of the thalweg under the top, at each side's reach.
      const f64 thalweg = section_of(w, wide(pt.depth_m), wide(p.bank_slope)).thalweg;
      const f64 level = top - wide(p.flood_bed_depth) * thalweg;
      for (const f64 side : {1.0, -1.0}) {
        const Vector<f32>& reach = side > 0 ? link.flood_left : link.flood_right;
        // A side with no bed of its own reaches the channel's bank (half the width): not read.
        if (i >= reach.size() || !(wide(reach[i]) > std::max(half, .5 * w) + .05)) continue;
        const f64 r = wide(reach[i]);
        ++out.flood_sides;
        const f64 flood = level - g_at(pt.at.x + side * lx * r, pt.at.z + side * lz * r);
        if (flood > k_seat_bound_m) ++out.flood_over;
        if (flood > out.flood_over_m) {
          out.flood_over_m = flood;
          out.flood_over_at = WorldPos{pt.at.x + side * lx * r, level, pt.at.z + side * lz * r};
        }
      }
    }
  }
  return out;
}

}  // namespace engine::channels
