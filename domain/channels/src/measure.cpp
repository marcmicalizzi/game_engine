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

}  // namespace engine::channels
