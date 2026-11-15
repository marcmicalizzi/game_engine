// The village's plan (docs/subsystems/village.md, "The plan"): lanes stationed on the ground, plots
// of whole perches along them, a house on each plot the ground allows, and what the plan is handed
// over as — ground edits, footprints, mask regions, placements.
#include <domain/village/village.h>

#include <core/hash/hash.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace engine::village {

namespace {

// Half a metre each side between a house and its plot's side boundaries.
constexpr i64 k_side_gap_mm = 500;

i64 mm_of(f64 metres) noexcept { return scene_gen::nearest_mm(metres); }
// a / b to the nearest whole, halves away from zero; b positive.
constexpr i64 div_round(i64 a, i64 b) noexcept {
  return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}
bool in_range(f64 v, f64 lo, f64 hi) noexcept { return v >= lo && v <= hi; }  // false for a NaN

struct Dir {
  f64 x = 1.0, z = 0.0;
  f64 length_mm = 0.0;
};
// The unit direction from a to b and its length: products, sums, one square root, two divisions.
Dir direction(i64 ax, i64 az, i64 bx, i64 bz) noexcept {
  const f64 dx = static_cast<f64>(bx - ax), dz = static_cast<f64>(bz - az);
  Dir d;
  d.length_mm = std::sqrt(dx * dx + dz * dz);
  if (d.length_mm > 0.0) {
    d.x = dx / d.length_mm;
    d.z = dz / d.length_mm;
  }
  return d;
}

// A quarter turn about y by the engine's rotation (x' = x cos + z sin, z' = -x sin + z cos).
constexpr void quarter(i64& x, i64& z) noexcept {
  const i64 nx = z, nz = -x;
  x = nx;
  z = nz;
}
// The cosine and sine of a built plot's whole turn: the row's (the house frame's x along the
// street, its z into the plot) after the house's quarter turns.
void turn_of(const Plot& p, f64& c, f64& s) noexcept {
  c = p.along_x;
  s = -p.along_z;
  for (u8 q = 0; q < p.quarter_turns; ++q) {
    const f64 nc = -s, ns = c;
    c = nc;
    s = ns;
  }
}

// A point of the plot's frame (u along the street from the middle of its front, v into the plot)
// in the world, to the millimetre.
void plot_point(const Plot& p, f64 u_mm, f64 v_mm, i64& x, i64& z) noexcept {
  x = p.front_x_mm + mm_of((p.along_x * u_mm + p.inward_x * v_mm) / 1000.0);
  z = p.front_z_mm + mm_of((p.along_z * u_mm + p.inward_z * v_mm) / 1000.0);
}

bool refuse(std::string& error, std::string why) {
  error = std::move(why);
  return false;
}

std::string lane_name(const VillageLayer& layer, u32 k) {
  if (k < layer.lanes.size() && !layer.lanes[k].name.empty()) return layer.lanes[k].name;
  char text[24];
  std::snprintf(text, sizeof(text), "lane %u", k);
  return text;
}

JsonValue point_json(f64 x, f64 y, f64 z) {
  JsonValue p = JsonValue::array();
  p.push_back(JsonValue(x));
  p.push_back(JsonValue(y));
  p.push_back(JsonValue(z));
  return p;
}
f64 metres(i64 mm) noexcept { return static_cast<f64>(mm) / 1000.0; }

const char* state_name(PlotState s) noexcept {
  switch (s) {
    case PlotState::built: return "built";
    case PlotState::spare: return "spare";
    case PlotState::steep: return "steep";
    case PlotState::wet: return "wet";
    case PlotState::no_fit: return "no_fit";
    case PlotState::refused: return "refused";
  }
  return "spare";
}

// The tread's top nearest a point: the station of the lane it is closest to.
i64 nearest_top(const LaneLine& lane, i64 x, i64 z) noexcept {
  i64 best = 0;
  f64 best_d = 1e300;
  for (const LaneStation& s : lane.stations) {
    const f64 dx = static_cast<f64>(s.x_mm - x), dz = static_cast<f64>(s.z_mm - z);
    const f64 d = dx * dx + dz * dz;
    if (d < best_d) {
      best_d = d;
      best = s.top_mm;
    }
  }
  return best;
}

}  // namespace

bool layer_in_range(const VillageLayer& p, std::string& error) {
  const auto bad = [&](const char* what) {
    return refuse(error, std::string("engine.village.VillageLayer: ") + what);
  };
  if (p.lanes.empty()) return bad("no lanes (a village is laid out along at least one)");
  for (const Lane& lane : p.lanes) {
    if (lane.waypoints.size() < 2) return bad("a lane of fewer than two waypoints");
    for (const WorldPos& w : lane.waypoints)
      if (!std::isfinite(w.x) || !std::isfinite(w.z)) return bad("a waypoint that is not finite");
    for (u32 k = 0; k + 1 < lane.waypoints.size(); ++k)
      if (mm_of(lane.waypoints[k].x) == mm_of(lane.waypoints[k + 1].x) &&
          mm_of(lane.waypoints[k].z) == mm_of(lane.waypoints[k + 1].z))
        return bad("two waypoints of a lane at one place");
  }
  if (p.houses < 1 || p.houses > 256) return bad("houses out of range (1 to 256)");
  if (!in_range(static_cast<f64>(p.perch_m), 4.5, 6.1)) return bad("perch_m out of range (4.5 to 6.1)");
  if (p.frontage_min_perches < 1 || p.frontage_max_perches > 12 ||
      p.frontage_min_perches > p.frontage_max_perches)
    return bad("frontage perches out of range (1 to 12, the least no more than the most)");
  if (p.depth_perches < 2 || p.depth_perches > 30) return bad("depth_perches out of range (2 to 30)");
  if (!in_range(static_cast<f64>(p.street_width_m), 2.0, 40.0))
    return bad("street_width_m out of range (2 to 40)");
  if (!in_range(static_cast<f64>(p.tread_width_m), 1.0, static_cast<f64>(p.street_width_m)))
    return bad("tread_width_m out of range (1 to the street's width)");
  if (!in_range(static_cast<f64>(p.setback_m), 0.0, 20.0)) return bad("setback_m out of range (0 to 20)");
  if (!in_range(static_cast<f64>(p.station_m), 0.5, 8.0)) return bad("station_m out of range (0.5 to 8)");
  if (!in_range(static_cast<f64>(p.pad_margin_m), 0.0, 4.0))
    return bad("pad_margin_m out of range (0 to 4)");
  if (!in_range(static_cast<f64>(p.max_pad_step_m), 0.1, 10.0))
    return bad("max_pad_step_m out of range (0.1 to 10)");
  if (!in_range(static_cast<f64>(p.path_width_m), 0.0, 3.0))
    return bad("path_width_m out of range (0 to 3)");
  if (!in_range(static_cast<f64>(p.bank_slope), 0.25, 8.0) ||
      !in_range(static_cast<f64>(p.fill_slope), 0.1, 8.0))
    return bad("bank_slope (0.25 to 8) or fill_slope (0.1 to 8) out of range");
  if (!in_range(static_cast<f64>(p.face_m), 0.1, 10.0)) return bad("face_m out of range (0.1 to 10)");
  if (p.storeys < 1 || p.storeys > 2) return bad("storeys out of range (1 or 2)");
  return true;
}

bool plan_village(const VillageLayer& layer, const scene_gen::Ground& ground,
                  const scene_gen::Context& context, HouseFn house_fn, void* house_context,
                  VillagePlan& out, std::string& error) {
  out = {};
  if (!layer_in_range(layer, error)) return false;
  if (house_fn == nullptr) return refuse(error, "village: no house filler was handed to the plan");
  const i64 perch = mm_of(static_cast<f64>(layer.perch_m));
  const i64 depth = perch * static_cast<i64>(layer.depth_perches);
  const i64 half_street = mm_of(static_cast<f64>(layer.street_width_m) * 0.5);
  const i64 half_tread = mm_of(static_cast<f64>(layer.tread_width_m) * 0.5);
  const i64 station = mm_of(static_cast<f64>(layer.station_m));
  const i64 setback = mm_of(static_cast<f64>(layer.setback_m));
  const i64 margin = mm_of(static_cast<f64>(layer.pad_margin_m));
  const i64 max_step = mm_of(static_cast<f64>(layer.max_pad_step_m));
  const u32 spread = layer.frontage_max_perches - layer.frontage_min_perches + 1;

  for (u32 li = 0; li < layer.lanes.size(); ++li) {
    const Lane& lane = layer.lanes[li];
    const u32 points = lane.waypoints.size();
    Vector<i64> wx, wz;
    for (const WorldPos& w : lane.waypoints) {
      wx.push_back(mm_of(w.x));
      wz.push_back(mm_of(w.z));
    }
    Vector<Dir> dirs;
    for (u32 k = 0; k + 1 < points; ++k)
      dirs.push_back(direction(wx[k], wz[k], wx[k + 1], wz[k + 1]));

    // The stations: each segment cut into equal steps no longer than `station_m`, the tread's top
    // the ground there, then a five-point mean along the lane so a hummock under one station does
    // not put a step in the tread.
    LaneLine line;
    for (u32 k = 0; k + 1 < points; ++k) {
      const i64 steps = std::max<i64>(1, (static_cast<i64>(dirs[k].length_mm) + station - 1) / station);
      for (i64 q = 0; q < steps; ++q)
        line.stations.push_back(LaneStation{wx[k] + div_round((wx[k + 1] - wx[k]) * q, steps),
                                            wz[k] + div_round((wz[k + 1] - wz[k]) * q, steps), 0});
      line.length_mm += static_cast<i64>(dirs[k].length_mm);
    }
    line.stations.push_back(LaneStation{wx[points - 1], wz[points - 1], 0});
    Vector<i64> raw;
    for (const LaneStation& s : line.stations) {
      const f64 h = ground.surface_mm(s.x_mm, s.z_mm);
      if (!std::isfinite(h))
        return refuse(error, "village: the ground under " + lane_name(layer, li) + " is not finite");
      raw.push_back(mm_of(h));
    }
    const i64 count = static_cast<i64>(raw.size());
    for (i64 k = 0; k < count; ++k) {
      i64 sum = 0;
      for (i64 q = -2; q <= 2; ++q)
        sum += raw[static_cast<u32>(std::clamp<i64>(k + q, 0, count - 1))];
      line.stations[static_cast<u32>(k)].top_mm = div_round(sum, 5);
    }

    // The plots: along each segment and each side, from the segment's start, a gap left on the
    // inside of a bend so two segments' plots do not overlap (tan of half the turn, from the two
    // directions' cross and dot: no angle is ever taken).
    const u32 first_plot = out.plots.size();
    i64 before = 0;  // the lane's length before this segment
    for (u32 k = 0; k + 1 < points; ++k) {
      const Dir& d = dirs[k];
      const i64 length = static_cast<i64>(d.length_mm);
      for (u8 side = 0; side < 2; ++side) {
        if ((side == 0 && lane.rows == Rows::Right) || (side == 1 && lane.rows == Rows::Left))
          continue;
        const auto gap_at = [&](const Dir& from, const Dir& to) -> i64 {
          const f64 cross = from.x * to.z - from.z * to.x;
          const f64 dot = from.x * to.x + from.z * to.z;
          const bool inside = side == 0 ? cross > 0.0 : cross < 0.0;
          if (!inside) return 0;
          const f64 t = dot > -0.99 ? std::fabs(cross) / (1.0 + dot) : 100.0;
          return static_cast<i64>(static_cast<f64>(half_street + depth) * t) + 1;
        };
        const i64 gap_start = k > 0 ? gap_at(dirs[k - 1], d) : 0;
        const i64 gap_end = k + 2 < points ? gap_at(d, dirs[k + 1]) : 0;
        i64 cursor = gap_start;
        for (u32 n = 0;; ++n) {
          const u64 seed = hash_combine(
              hash_combine(hash_combine(hash_combine(layer.seed, li), k), side), n);
          const i64 frontage =
              perch * static_cast<i64>(layer.frontage_min_perches + static_cast<u32>(mix64(seed) % spread));
          if (cursor + frontage > length - gap_end) break;
          Plot p;
          p.lane = li;
          p.segment = k;
          p.side = side;
          p.seed = seed;
          p.frontage_mm = frontage;
          p.depth_mm = depth;
          const f64 mid = static_cast<f64>(cursor + frontage / 2);
          p.along_mm = before + cursor + frontage / 2;
          // Left of (dx, dz) is (-dz, dx). The plot's frame runs `along` so that its left normal
          // points into the plot: the lane's direction on the left row, its reverse on the right.
          const f64 nx = side == 0 ? -d.z : d.z, nz = side == 0 ? d.x : -d.x;
          p.front_x_mm = wx[k] + mm_of((d.x * mid + nx * static_cast<f64>(half_street)) / 1000.0);
          p.front_z_mm = wz[k] + mm_of((d.z * mid + nz * static_cast<f64>(half_street)) / 1000.0);
          p.along_x = side == 0 ? d.x : -d.x;
          p.along_z = side == 0 ? d.z : -d.z;
          p.inward_x = nx;
          p.inward_z = nz;
          out.plots.push_back(p);
          cursor += frontage;
        }
      }
      before += length;
    }
    std::sort(out.plots.begin() + first_plot, out.plots.end(), [](const Plot& a, const Plot& b) {
      return a.along_mm != b.along_mm ? a.along_mm < b.along_mm : a.side < b.side;
    });
    out.lanes.push_back(std::move(line));
  }

  // The houses, in the plots' order until the entry's count stands.
  for (u32 pi = 0; pi < out.plots.size(); ++pi) {
    Plot& p = out.plots[pi];
    if (out.built >= layer.houses) {
      p.state = PlotState::spare;
      ++out.spare;
      continue;
    }
    HouseAsk ask;
    ask.frontage_mm = p.frontage_mm;
    ask.depth_mm = p.depth_mm;
    ask.setback_mm = setback;
    ask.seed = p.seed;
    ask.storeys = layer.storeys;
    ask.index = pi;
    House h;
    std::string why;
    if (!house_fn(house_context, ask, h, why)) {
      p.state = PlotState::refused;
      ++out.refused;
      continue;
    }
    // Turned by quarters until its door faces the street (-z of the plot's frame).
    i64 ox = h.out_x, oz = h.out_z;
    i64 cx[4] = {h.min_x_mm, h.max_x_mm, h.max_x_mm, h.min_x_mm};
    i64 cz[4] = {h.min_z_mm, h.min_z_mm, h.max_z_mm, h.max_z_mm};
    i64 door_x = h.door_x_mm, door_z = h.door_z_mm;
    u8 turns = 0;
    while (!(ox == 0 && oz == -1) && turns < 4) {
      quarter(ox, oz);
      quarter(door_x, door_z);
      for (u32 c = 0; c < 4; ++c)
        quarter(cx[c], cz[c]);
      ++turns;
    }
    const i64 x0 = std::min(std::min(cx[0], cx[1]), std::min(cx[2], cx[3]));
    const i64 x1 = std::max(std::max(cx[0], cx[1]), std::max(cx[2], cx[3]));
    const i64 z0 = std::min(std::min(cz[0], cz[1]), std::min(cz[2], cz[3]));
    const i64 z1 = std::max(std::max(cz[0], cz[1]), std::max(cz[2], cz[3]));
    if (turns == 4 || x1 - x0 + 2 * k_side_gap_mm > p.frontage_mm || setback + (z1 - z0) > p.depth_mm) {
      p.state = PlotState::no_fit;
      ++out.no_fit;
      continue;
    }
    p.quarter_turns = turns;
    // The box's middle on the plot's middle, its front face `setback` behind the plot's front.
    const i64 origin_u = -div_round(x0 + x1, 2), origin_v = setback - z0;
    const auto to_world = [&](i64 hx, i64 hz, i64& x, i64& z) {
      plot_point(p, static_cast<f64>(origin_u + hx), static_cast<f64>(origin_v + hz), x, z);
    };
    to_world(0, 0, p.house_x_mm, p.house_z_mm);
    // The ground under the footprint: its four corners and its middle.
    const i64 sx[5] = {x0, x1, x1, x0, div_round(x0 + x1, 2)};
    const i64 sz[5] = {z0, z0, z1, z1, div_round(z0 + z1, 2)};
    i64 lo = 0, hi = 0, sum = 0;
    bool wet = false;
    for (u32 c = 0; c < 5; ++c) {
      i64 x = 0, z = 0;
      to_world(sx[c], sz[c], x, z);
      const f64 gh = ground.surface_mm(x, z);
      if (!std::isfinite(gh)) return refuse(error, "village: the ground under a plot is not finite");
      const i64 g = mm_of(gh);
      lo = c == 0 ? g : std::min(lo, g);
      hi = c == 0 ? g : std::max(hi, g);
      sum += g;
      wet = wet || context.excluded_mm(x, z,
                                       scene_gen::k_exclude_holes | scene_gen::k_exclude_channels |
                                           scene_gen::k_exclude_water);
    }
    if (wet) {
      p.state = PlotState::wet;
      ++out.wet;
      continue;
    }
    if (hi - lo > max_step) {
      p.state = PlotState::steep;
      ++out.steep;
      continue;
    }
    p.pad_mm = div_round(sum, 5);
    p.floor_mm = h.floor_mm;
    const i64 px[4] = {x0 - margin, x1 + margin, x1 + margin, x0 - margin};
    const i64 pz[4] = {z0 - margin, z0 - margin, z1 + margin, z1 + margin};
    for (u32 c = 0; c < 4; ++c)
      to_world(px[c], pz[c], p.pad_x_mm[c], p.pad_z_mm[c]);
    to_world(door_x, door_z, p.door_x_mm, p.door_z_mm);
    // The path runs square to the street from the door to the tread's near edge.
    plot_point(p, static_cast<f64>(origin_u + door_x), -static_cast<f64>(half_street - half_tread),
               p.path_x_mm, p.path_z_mm);
    p.path_top_mm = nearest_top(out.lanes[p.lane], p.path_x_mm, p.path_z_mm);
    p.state = PlotState::built;
    p.house = out.houses.size();
    out.houses.push_back(std::move(h));
    ++out.built;
  }
  out.size_class = out.built >= 6 ? "village" : "hamlet";
  return true;
}

u64 plan_hash(const VillagePlan& plan) {
  u64 h = hash_combine(k_village_version, plan.lanes.size());
  for (const LaneLine& lane : plan.lanes) {
    h = hash_combine(h, static_cast<u64>(lane.length_mm));
    for (const LaneStation& s : lane.stations)
      h = hash_combine(hash_combine(hash_combine(h, static_cast<u64>(s.x_mm)), static_cast<u64>(s.z_mm)),
                       static_cast<u64>(s.top_mm));
  }
  const auto bits = [](f64 v) {
    u64 b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
  };
  for (const Plot& p : plan.plots) {
    const u64 words[] = {p.lane,
                         p.segment,
                         p.side,
                         static_cast<u64>(p.state),
                         p.quarter_turns,
                         static_cast<u64>(p.front_x_mm),
                         static_cast<u64>(p.front_z_mm),
                         bits(p.along_x),
                         bits(p.along_z),
                         static_cast<u64>(p.frontage_mm),
                         static_cast<u64>(p.depth_mm),
                         p.seed,
                         static_cast<u64>(p.house_x_mm),
                         static_cast<u64>(p.house_z_mm),
                         static_cast<u64>(p.pad_mm),
                         static_cast<u64>(p.door_x_mm),
                         static_cast<u64>(p.door_z_mm),
                         static_cast<u64>(p.path_x_mm),
                         static_cast<u64>(p.path_z_mm),
                         static_cast<u64>(p.path_top_mm)};
    for (const u64 w : words)
      h = hash_combine(h, w);
    for (u32 c = 0; c < 4; ++c)
      h = hash_combine(hash_combine(h, static_cast<u64>(p.pad_x_mm[c])), static_cast<u64>(p.pad_z_mm[c]));
  }
  for (const House& house : plan.houses) {
    const i64 words[] = {house.min_x_mm, house.min_z_mm,  house.max_x_mm, house.max_z_mm,
                         house.door_x_mm, house.door_z_mm, house.out_x,    house.out_z,
                         house.floor_mm};
    for (const i64 w : words)
      h = hash_combine(h, static_cast<u64>(w));
    h = hash_combine(h, hash_bytes(house.recipe.data(), house.recipe.size()));
  }
  return h;
}

void ground_edits(const VillageLayer& layer, const VillagePlan& plan,
                  Vector<scene::TerrainEdit>& out) {
  const auto shaped = [&](scene::TerrainEdit& e) {
    e.mode = scene::TerrainEditMode::Bench;
    e.bank_slope = layer.bank_slope;
    e.fill_slope = layer.fill_slope;
    e.face_m = layer.face_m;
    e.fill_m = layer.face_m;
  };
  const std::string prefix = layer.name.empty() ? std::string("village") : layer.name;
  for (u32 k = 0; k < plan.lanes.size(); ++k) {
    const LaneLine& lane = plan.lanes[k];
    if (lane.stations.size() < 2) continue;
    scene::TerrainEdit e;
    e.name = prefix + " " + lane_name(layer, k) + " tread";
    shaped(e);
    for (const LaneStation& s : lane.stations) {
      e.points.push_back(WorldPos{metres(s.x_mm), metres(s.top_mm), metres(s.z_mm)});
      e.width_m.push_back(layer.tread_width_m);
    }
    out.push_back(std::move(e));
  }
  for (u32 k = 0; k < plan.plots.size(); ++k) {
    const Plot& p = plan.plots[k];
    if (p.state != PlotState::built) continue;
    char number[24];
    std::snprintf(number, sizeof(number), " plot %u", k);
    scene::TerrainEdit pad;
    pad.name = prefix + number + " pad";
    shaped(pad);
    for (u32 c = 0; c < 4; ++c)
      pad.outline.push_back(WorldPos{metres(p.pad_x_mm[c]), metres(p.pad_mm), metres(p.pad_z_mm[c])});
    out.push_back(std::move(pad));
    if (!(layer.path_width_m > 0.0f)) continue;
    if (p.door_x_mm == p.path_x_mm && p.door_z_mm == p.path_z_mm) continue;
    scene::TerrainEdit path;
    path.name = prefix + number + " path";
    shaped(path);
    path.points.push_back(WorldPos{metres(p.door_x_mm), metres(p.pad_mm), metres(p.door_z_mm)});
    path.points.push_back(WorldPos{metres(p.path_x_mm), metres(p.path_top_mm), metres(p.path_z_mm)});
    path.width_m.push_back(layer.path_width_m);
    path.width_m.push_back(layer.path_width_m);
    out.push_back(std::move(path));
  }
}

void footprints(const VillageLayer& layer, const VillagePlan& plan, scene_gen::FootprintList& out,
                i64 tile_mm, i64 tile_x, i64 tile_z, i64 reach_mm) {
  const auto wanted = [&](const scene_gen::Footprint& f) {
    if (tile_mm <= 0) return true;
    const i64 r = static_cast<i64>(f.radius * 1000.0) + 1 + reach_mm;
    return f.x_mm + r >= tile_x * tile_mm && f.x_mm - r < (tile_x + 1) * tile_mm &&
           f.z_mm + r >= tile_z * tile_mm && f.z_mm - r < (tile_z + 1) * tile_mm;
  };
  const i64 half_tread = mm_of(static_cast<f64>(layer.tread_width_m) * 0.5);
  const i64 half_path = mm_of(static_cast<f64>(layer.path_width_m) * 0.5);
  for (const LaneLine& lane : plan.lanes)
    for (u32 k = 0; k + 1 < lane.stations.size(); ++k) {
      const LaneStation& a = lane.stations[k];
      const LaneStation& b = lane.stations[k + 1];
      const scene_gen::Footprint band =
          scene_gen::Footprint::band(a.x_mm, a.z_mm, b.x_mm, b.z_mm, half_tread);
      if (wanted(band)) out.add(band);
    }
  for (const Plot& p : plan.plots) {
    if (p.state != PlotState::built) continue;
    scene_gen::FootprintPoint corners[4];
    for (u32 c = 0; c < 4; ++c)
      corners[c] = scene_gen::FootprintPoint{p.pad_x_mm[c], p.pad_z_mm[c]};
    const scene_gen::Footprint pad = scene_gen::Footprint::polygon(corners);
    if (wanted(pad)) out.add(pad);  // the list copies the points
    if (half_path < 1 || (p.door_x_mm == p.path_x_mm && p.door_z_mm == p.path_z_mm)) continue;
    const scene_gen::Footprint path =
        scene_gen::Footprint::band(p.door_x_mm, p.door_z_mm, p.path_x_mm, p.path_z_mm, half_path);
    if (wanted(path)) out.add(path);
  }
}

JsonValue tread_regions_json(const VillageLayer& layer, const VillagePlan& plan,
                             std::string_view material) {
  JsonValue list = JsonValue::array();
  const f64 half = static_cast<f64>(layer.tread_width_m) * 0.5;
  const std::string prefix = layer.name.empty() ? std::string("village") : layer.name;
  for (u32 k = 0; k < plan.lanes.size(); ++k) {
    const Vector<LaneStation>& st = plan.lanes[k].stations;
    if (st.size() < 2) continue;
    JsonValue left = JsonValue::array();
    Vector<f64> rx, rz;
    for (u32 q = 0; q < st.size(); ++q) {
      const LaneStation& a = st[q > 0 ? q - 1 : 0];
      const LaneStation& b = st[q + 1 < st.size() ? q + 1 : q];
      const Dir d = direction(a.x_mm, a.z_mm, b.x_mm, b.z_mm);
      const LaneStation& c = st[q];
      const auto cm = [](f64 v) { return static_cast<f64>(mm_of(v * 0.1)) / 100.0; };
      left.push_back(point_json(cm(metres(c.x_mm) - d.z * half), 0.0, cm(metres(c.z_mm) + d.x * half)));
      rx.push_back(cm(metres(c.x_mm) + d.z * half));
      rz.push_back(cm(metres(c.z_mm) - d.x * half));
    }
    for (u32 q = rx.size(); q-- > 0;)
      left.push_back(point_json(rx[q], 0.0, rz[q]));
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(prefix + " " + lane_name(layer, k) + " tread");
    o["polygon"] = std::move(left);
    o["material"] = JsonValue(material);
    o["feather_m"] = JsonValue(0.5);
    list.push_back(std::move(o));
  }
  return list;
}

JsonValue plan_json(const VillageLayer& layer, const VillagePlan& plan) {
  JsonValue o = JsonValue::object();
  o["name"] = JsonValue(layer.name);
  o["class"] = JsonValue(plan.size_class);
  o["houses"] = JsonValue(plan.built);
  o["plots"] = JsonValue(plan.plots.size());
  o["spare"] = JsonValue(plan.spare);
  o["steep"] = JsonValue(plan.steep);
  o["wet"] = JsonValue(plan.wet);
  o["no_fit"] = JsonValue(plan.no_fit);
  o["refused"] = JsonValue(plan.refused);
  char hash[24];
  std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(plan_hash(plan)));
  o["hash"] = JsonValue(hash);
  JsonValue lanes = JsonValue::array();
  for (u32 k = 0; k < plan.lanes.size(); ++k) {
    JsonValue l = JsonValue::object();
    l["name"] = JsonValue(lane_name(layer, k));
    l["length_m"] = JsonValue(metres(plan.lanes[k].length_mm));
    l["stations"] = JsonValue(plan.lanes[k].stations.size());
    lanes.push_back(std::move(l));
  }
  o["lanes"] = std::move(lanes);
  JsonValue plots = JsonValue::array();
  for (const Plot& p : plan.plots) {
    JsonValue q = JsonValue::object();
    q["lane"] = JsonValue(p.lane);
    q["side"] = JsonValue(p.side == 0 ? "left" : "right");
    q["state"] = JsonValue(state_name(p.state));
    q["frontage_m"] = JsonValue(metres(p.frontage_mm));
    q["depth_m"] = JsonValue(metres(p.depth_mm));
    q["front"] = point_json(metres(p.front_x_mm), 0.0, metres(p.front_z_mm));
    if (p.state == PlotState::built) {
      const House& h = plan.houses[p.house];
      q["house"] = JsonValue(h.name);
      q["at"] = point_json(metres(p.house_x_mm), metres(p.pad_mm), metres(p.house_z_mm));
      q["door"] = point_json(metres(p.door_x_mm), metres(p.pad_mm + h.floor_mm), metres(p.door_z_mm));
      q["length_m"] = JsonValue(metres(h.max_x_mm - h.min_x_mm));
      q["width_m"] = JsonValue(metres(h.max_z_mm - h.min_z_mm));
    }
    plots.push_back(std::move(q));
  }
  o["plot_list"] = std::move(plots);
  return o;
}

scene_gen::Placement house_placement(const Plot& plot) {
  f64 c = 1.0, s = 0.0;
  turn_of(plot, c, s);
  // The half angle from the cosine and the sine alone: one square root and one division, by the
  // branch that keeps the divisor away from zero.
  f64 half_c = 1.0, half_s = 0.0;
  if (c >= 0.0) {
    half_c = std::sqrt((1.0 + c) * 0.5);
    half_s = s / (2.0 * half_c);
  } else {
    half_s = std::sqrt((1.0 - c) * 0.5);
    if (s < 0.0) half_s = -half_s;
    half_c = s / (2.0 * half_s);
  }
  scene_gen::Placement out;
  out.position = WorldPos{metres(plot.house_x_mm), metres(plot.pad_mm + plot.floor_mm),
                          metres(plot.house_z_mm)};
  out.rotation = Quat{0.0f, static_cast<f32>(half_s), 0.0f, static_cast<f32>(half_c)};
  return out;
}

WorldPos house_point(const Plot& plot, i64 x_mm, i64 y_mm, i64 z_mm) {
  f64 c = 1.0, s = 0.0;
  turn_of(plot, c, s);
  const f64 x = static_cast<f64>(x_mm), z = static_cast<f64>(z_mm);
  return WorldPos{metres(plot.house_x_mm) + (x * c + z * s) / 1000.0,
                  metres(plot.pad_mm + plot.floor_mm + y_mm),
                  metres(plot.house_z_mm) + (-x * s + z * c) / 1000.0};
}

bool box_house(void* context, const HouseAsk& ask, House& out, std::string&) {
  const BoxHouse defaults;
  const BoxHouse& box = context != nullptr ? *static_cast<const BoxHouse*>(context) : defaults;
  // Long side to the street where the frontage holds it, gable-on where it does not.
  const bool gable = box.length_mm + 2 * k_side_gap_mm > ask.frontage_mm;
  const i64 across = gable ? box.width_mm : box.length_mm;
  const i64 deep = gable ? box.length_mm : box.width_mm;
  out = {};
  out.min_x_mm = -across / 2;
  out.max_x_mm = across / 2;
  out.min_z_mm = -deep / 2;
  out.max_z_mm = deep / 2;
  out.door_x_mm = 0;
  out.door_z_mm = out.min_z_mm;
  out.out_x = 0;
  out.out_z = -1;
  out.name = gable ? "box, gable-on" : "box";
  out.reach = static_cast<f32>(
      std::sqrt(static_cast<f64>(across * across + deep * deep)) * 0.5 / 1000.0);
  return true;
}

}  // namespace engine::village
