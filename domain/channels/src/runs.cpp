#include <domain/channels/runs.h>
#include <domain/scene_gen/water_channel.h>

#include <algorithm>
#include <cmath>

namespace engine::channels {

LineRuns measure_runs(const scene::WaterChannel& block) {
  LineRuns out;
  const u32 n = block.points.size();
  if (n < 2 || block.depth_m.size() != n || block.flags.size() != n) return out;
  out.fall_m = block.points[0].y - block.points[n - 1].y;
  // What the stretch from i to i + 1 is: 0 drawn surface, 1 a fall, 2 not drawn.
  const auto kind = [&](u32 i) {
    const u16 a = block.flags[i], b = block.flags[i + 1];
    if ((a & scene_gen::k_channel_fall_lip) != 0 && (b & scene_gen::k_channel_fall_foot) != 0)
      return 1u;
    return scene_gen::channel_stretch_drawn(a, b, block.depth_m[i], block.depth_m[i + 1],
                                            block.points[i].y, block.points[i + 1].y)
               ? 0u
               : 2u;
  };
  // A piece is a maximal run of drawn surface; a run a maximal run of what a person sees as water,
  // the surface and every nappe the draw lays (water.md, "A fall": where the foot stands lower and
  // the stretch is drawn).
  bool surface = false, water = false, gap = false;
  const auto thin = [&](u32 i) { return static_cast<f64>(block.depth_m[i]) <= k_run_thin_depth_m; };
  out.alternations = 1;
  const bool speeds = block.speed_mps.size() == n;
  for (u32 i = 0; i + 1 < n; ++i) {
    const f64 dx = block.points[i + 1].x - block.points[i].x;
    const f64 dz = block.points[i + 1].z - block.points[i].z;
    const f64 len = std::sqrt(dx * dx + dz * dz);
    out.length_m += len;
    if (thin(i)) out.thin_m += len;
    if (thin(i + 1) != thin(i)) ++out.alternations;
    if (static_cast<f64>(block.depth_m[i]) <= k_run_film_depth_m &&
        static_cast<f64>(block.depth_m[i + 1]) <= k_run_film_depth_m)
      out.film_m += len;
    const u32 k = kind(i);
    bool seen = k == 0;
    if (k == 1) {
      ++out.falls;
      seen = block.points[i + 1].y < block.points[i].y &&
             scene_gen::channel_stretch_drawn(block.flags[i], block.flags[i + 1], block.depth_m[i],
                                              block.depth_m[i + 1], block.points[i].y,
                                              block.points[i + 1].y);
      if (seen) ++out.falls_drawn;
    }
    if (k == 2) out.undrawn_m += len;
    if (k == 0 && speeds && len > 0.0) {
      out.speed_mps.push_back(
          0.5 * (static_cast<f64>(block.speed_mps[i]) + static_cast<f64>(block.speed_mps[i + 1])));
      out.depth_m.push_back(
          0.5 * (static_cast<f64>(block.depth_m[i]) + static_cast<f64>(block.depth_m[i + 1])));
      out.stretch_m.push_back(len);
    }
    if (k == 0 && !surface) ++out.pieces;
    if (seen && !water) ++out.runs;
    if (k == 2 && !gap) ++out.gaps;
    surface = k == 0;
    water = seen;
    gap = k == 2;
  }
  return out;
}

u32 run_class(f64 slope) noexcept {
  u32 c = 0;
  while (c + 1 < k_run_classes && slope >= k_run_class_slope[c])
    ++c;
  return c;
}

f64 weighted_quantile(const Vector<f64>& value, const Vector<f64>& weight, f64 share) {
  const u32 n = std::min(value.size(), weight.size());
  if (n == 0) return 0.0;
  Vector<u32> order(n, 0u);
  f64 total = 0.0;
  for (u32 i = 0; i < n; ++i) {
    order[i] = i;
    total += weight[i];
  }
  // By value, then by index: the same order on every toolchain.
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return value[a] < value[b] || (value[a] == value[b] && a < b);
  });
  const f64 want = std::clamp(share, 0.0, 1.0) * total;
  f64 sum = 0.0;
  for (u32 k = 0; k < n; ++k) {
    sum += weight[order[k]];
    if (sum >= want) return value[order[k]];
  }
  return value[order[n - 1]];
}

void RunsBySlope::add(const LineRuns& line) {
  Class& c = classes[run_class(line.slope())];
  for (u32 i = 0; i < line.speed_mps.size(); ++i) {
    c.speed_mps.push_back(line.speed_mps[i]);
    c.depth_m.push_back(line.depth_m[i]);
    c.stretch_m.push_back(line.stretch_m[i]);
  }
  ++c.lines;
  c.length_m += line.length_m;
  c.film_m += line.film_m;
  c.undrawn_m += line.undrawn_m;
  c.pieces += line.pieces;
  c.runs += line.runs;
  c.falls += line.falls;
  c.falls_drawn += line.falls_drawn;
  c.gaps += line.gaps;
  c.thin_m += line.thin_m;
  c.alternations += line.alternations;
}

JsonValue runs_json(const RunsBySlope& runs) {
  static const char* const names[k_run_classes] = {"under 2%", "2-5%", "5-15%", "15-30%",
                                                   "over 30%"};
  const auto round = [](f64 v, f64 to) { return std::round(v * to) / to; };
  JsonValue out = JsonValue::array();
  for (u32 k = 0; k < k_run_classes; ++k) {
    const RunsBySlope::Class& c = runs.classes[k];
    JsonValue one = JsonValue::object();
    one["slope"] = JsonValue(names[k]);
    one["lines"] = JsonValue(c.lines);
    one["km"] = JsonValue(round(c.length_m / 1000.0, 1e3));
    const f64 per = c.length_m > 0 ? 100.0 / c.length_m : 0.0;
    one["film_share"] = JsonValue(round(c.length_m > 0 ? c.film_m / c.length_m : 0.0, 1e4));
    one["undrawn_share"] = JsonValue(round(c.length_m > 0 ? c.undrawn_m / c.length_m : 0.0, 1e4));
    one["pieces_per_100m"] = JsonValue(round(static_cast<f64>(c.pieces) * per, 1e2));
    one["runs_per_100m"] = JsonValue(round(static_cast<f64>(c.runs) * per, 1e2));
    one["falls"] = JsonValue(c.falls);
    one["falls_drawn"] = JsonValue(c.falls_drawn);
    one["gaps"] = JsonValue(c.gaps);
    one["thin_share"] = JsonValue(round(c.length_m > 0 ? c.thin_m / c.length_m : 0.0, 1e4));
    one["alternations_per_100m"] = JsonValue(round(static_cast<f64>(c.alternations) * per, 1e2));
    one["speed_median_mps"] = JsonValue(round(weighted_quantile(c.speed_mps, c.stretch_m, .5), 1e3));
    one["speed_p10_mps"] = JsonValue(round(weighted_quantile(c.speed_mps, c.stretch_m, .1), 1e3));
    one["speed_p90_mps"] = JsonValue(round(weighted_quantile(c.speed_mps, c.stretch_m, .9), 1e3));
    one["depth_median_m"] = JsonValue(round(weighted_quantile(c.depth_m, c.stretch_m, .5), 1e4));
    out.push_back(std::move(one));
  }
  return out;
}

}  // namespace engine::channels
