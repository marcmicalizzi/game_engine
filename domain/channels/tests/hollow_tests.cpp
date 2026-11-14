// A stream's line never leaves a hollow uphill (roadmap W122; docs/subsystems/channels.md,
// "Hollows"): a bowl on a slope stands as a body of the plan at its spill, a shallow one is
// breached, a scene's own body under its basin's spill is where the basin's water ends, a basin
// the author names dry has no body, and in every case no link's bed rises downstream.
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <domain/channels/channels.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::channels;

namespace {
// The valley every case stands in: a V-notch along x = 128 m falling 5 % toward z = 0, with a
// round bowl sunk into its floor at z = 300 m. `dip` is the bowl's amplitude and `sigma` its
// radius; a dip of `0.05 sigma sqrt(e)` or less leaves the floor falling all the way (no pit).
struct Bowl {
  f64 dip = 8;
  f64 sigma = 25;
};
f64 bowl(const void* context, i64 x, i64 z) noexcept {
  const Bowl& b = *static_cast<const Bowl*>(context);
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 r2 = (xx - 128) * (xx - 128) + (zz - 300) * (zz - 300);
  return .05 * zz + .15 * std::abs(xx - 128) - b.dip * std::exp(-r2 / (2 * b.sigma * b.sigma));
}
scene_gen::Ground ground(const Bowl& b) {
  scene_gen::Ground g;
  g.context = &b;
  g.surface_fn = &bowl;
  return g;
}
ChannelLayer layer() {
  ChannelLayer p;
  p.name = "test";
  p.region_min = {0, 0};
  p.region_max = {256, 512};
  return p;
}
scene::WaterBody pond(f64 level, f64 radius) {
  scene::WaterBody b;
  b.name = "pond";
  b.level_m = level;
  scene::WaterCircle circle;
  circle.center = WorldPos{128, 0, 300};
  circle.radius_m = static_cast<f32>(radius);
  b.shape.circle = circle;
  return b;
}
const Hollow* only(const ChannelNetwork& n, HollowKind kind) {
  const Hollow* found = nullptr;
  u32 count = 0;
  for (const Hollow& h : n.basins.hollows)
    if (h.kind == kind) {
      found = &h;
      ++count;
    }
  return count == 1 ? found : nullptr;
}
// How many points of any line lie under a body of the plan: its cell the body's and its ground
// under the body's level.
u32 points_under(const ChannelNetwork& n) {
  u32 under = 0;
  for (const ChannelLink& link : n.links)
    for (const ChannelPoint& pt : link.line) {
      const i64 c = (std::llround(pt.at.x * 1000) - n.x_mm + n.spacing_mm / 2) / n.spacing_mm;
      const i64 r = (std::llround(pt.at.z * 1000) - n.z_mm + n.spacing_mm / 2) / n.spacing_mm;
      const u32 body = n.body_of[static_cast<u32>(r) * n.width + static_cast<u32>(c)];
      if (body != 0 && pt.at.y < n.basins.hollows[body - 1].level_m - 1e-3) ++under;
    }
  return under;
}
}  // namespace

TEST_CASE("hollows: a bowl on a slope stands at its spill, and no line climbs out of it") {
  const ChannelLayer p = layer();
  const Bowl b;
  std::string error;
  ChannelNetwork n;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, n, error), error);
  // One hollow, deeper than the breach limit: a body of the plan, a notch under its spill.
  const Hollow* lake = only(n, HollowKind::Lake);
  REQUIRE(lake != nullptr);
  CHECK(n.basins.hollows.size() == 1);
  CHECK(lake->depth_m() > 3.0);
  CHECK(lake->depth_m() < b.dip);
  CHECK(lake->level_m == doctest::Approx(lake->spill_m - static_cast<f64>(p.basin_notch_m)));
  CHECK(lake->area_m2 >= static_cast<f64>(p.basin_min_area_m2));
  CHECK(std::fabs(lake->floor_x - 128) <= 4.0);
  CHECK(lake->floor_z > 290);
  CHECK(lake->floor_z < 330);
  // The spill is the low point of the rim: on the notch's line, downstream of the floor.
  CHECK(std::fabs(lake->spill_x - 128) <= 4.0);
  CHECK(lake->spill_z < lake->floor_z);
  CHECK(lake->catchment_m2 > lake->area_m2);
  CHECK(lake->name == "test basin 0");
  // No line lies under it, no channel begins on it, and what enters ends at its level.
  CHECK(points_under(n) == 0);
  for (const u32 head : n.heads)
    CHECK(n.body_of[head] == 0);
  u32 mouths = 0, outlets = 0;
  for (const ChannelLink& link : n.links) {
    if (link.line.size() < 2) continue;
    if (link.mouth) {
      ++mouths;
      CHECK(link.mouth_level == lake->level_m);
      CHECK(link.line.back().at.y == doctest::Approx(lake->level_m).epsilon(1e-4));
    }
    if (link.outlet) {
      ++outlets;
      // The outlet begins at the shore and its bed holds the sill through the lip: the body
      // cannot drain down its own outlet's cut.
      CHECK(link.outlet_level == doctest::Approx(lake->level_m + .02));
      CHECK(link.line.front().at.y == doctest::Approx(lake->level_m).epsilon(1e-4));
      bool past = false;
      for (u32 i = 0; i < link.line.size(); ++i) {
        if (link.line[i].at.y < link.outlet_level) past = true;
        if (!past) CHECK(link.bed[i] >= lake->level_m - 1e-3);
      }
      CHECK(past);
      CHECK(link.bed.back() < lake->level_m - 1.0);
    }
  }
  CHECK(mouths >= 1);
  CHECK(outlets == 1);
  // The rule: no bed's line rises downstream, and no line ends over where it began.
  const HollowStats stats = measure_hollows(p, n);
  CHECK(stats.links >= 2);
  CHECK(stats.climbing == 0);
  CHECK(stats.ends_higher == 0);
  CHECK(stats.rise_max_m == 0);
  // The body the plan writes: a fill seeded at the floor, at the level.
  const JsonValue bodies = plan_bodies_json(p, n);
  REQUIRE(bodies.as_array().size() == 1);
  f64 level = 0, x = 0, z = 0, extent = 0;
  REQUIRE(bodies[0].find("level_m")->get_f64(level));
  CHECK(level == doctest::Approx(lake->level_m));
  const JsonValue* fill = bodies[0].find("shape")->find("fill");
  REQUIRE(fill != nullptr);
  REQUIRE((*fill->find("seed"))[0].get_f64(x));
  REQUIRE((*fill->find("seed"))[2].get_f64(z));
  REQUIRE(fill->find("max_extent_m")->get_f64(extent));
  CHECK(x == lake->floor_x);
  CHECK(z == lake->floor_z);
  CHECK(extent > lake->extent_m);
  CHECK(bodies[0].find("name")->as_string() == "test basin 0");
}

TEST_CASE("hollows: a hollow under the limit is breached, and the cut widens with its depth") {
  ChannelLayer p = layer();
  Bowl b;
  b.dip = 2.2;
  b.sigma = 12;
  std::string error;
  ChannelNetwork n;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, n, error), error);
  // A pit under a metre: no body, counted, and a line through it.
  CHECK(n.basins.hollows.empty());
  CHECK(n.basins.breached >= 1);
  CHECK(n.basins.breached_max_m > .15);
  CHECK(n.basins.breached_max_m <= static_cast<f64>(p.breach_max_m));
  CHECK(plan_bodies_json(p, n).as_array().empty());
  const HollowStats stats = measure_hollows(p, n);
  CHECK(stats.climbing == 0);
  CHECK(stats.ends_higher == 0);
  // The breach is dug: somewhere the bed is held under its section, by no more than the pit's
  // depth and the line's own wander over the valley's side.
  CHECK(stats.trench_max_m > .1);
  CHECK(stats.trench_max_m < n.basins.breached_max_m + .6);
  // **The cut widens with a breach's depth.** The deep bowl with the limit raised over it is
  // breached too, metres deep; there the cut's top is wider than its section by what the bed is
  // held past a second thalweg, over each bank's slope, so the breach is a notch with banks and
  // not a slot.
  {
    ChannelLayer deep = layer();
    deep.breach_max_m = 20;
    const Bowl big;
    ChannelNetwork cut;
    REQUIRE_MESSAGE(plan_channels(deep, ground(big), 0, nullptr, cut, error), error);
    CHECK(cut.basins.hollows.empty());
    CHECK(cut.basins.breached_max_m > 3.0);
    const HollowStats dug = measure_hollows(deep, cut);
    CHECK(dug.climbing == 0);
    CHECK(dug.trench_max_m > 3.0);
    const JsonValue cuts = terrain_cuts_json(deep, cut);
    REQUIRE(dug.trench_link < cut.links.size());
    u32 at = 0;
    for (u32 k = 0; k < dug.trench_link; ++k)
      at += cut.links[k].line.size() >= 2 ? 1u : 0u;
    const ChannelLink& link = cut.links[dug.trench_link];
    const auto& widths = cuts[at].find("width_m")->as_array();
    const auto& depths = cuts[at].find("depth_m")->as_array();
    REQUIRE(widths.size() == link.line.size());
    f64 widest_gain = 0;
    for (u32 i = 0; i < link.line.size(); ++i) {
      f64 w = 0, d = 0;
      REQUIRE(widths[i].get_f64(w));
      REQUIRE(depths[i].get_f64(d));
      const f64 section =
          static_cast<f64>(link.line[i].width_m) * (1 + static_cast<f64>(link.swell[i]));
      widest_gain = std::max(widest_gain, w - section);
      // Never a slot: at the layer's bank slope the two banks reach a bed this deep inside the
      // width, but for the two thalwegs (at most four mean depths) the section itself holds.
      CHECK(w + 1e-3 >= 2 * (d - 4 * static_cast<f64>(link.line[i].depth_m) - 4.0) /
                            static_cast<f64>(deep.bank_slope));
    }
    CHECK(widest_gain > 2 * (dug.trench_max_m - 1.0) / static_cast<f64>(deep.bank_slope));
  }
  // With the limit under the pit's depth the same pit stands as a body instead.
  p.breach_max_m = .1f;
  p.basin_min_area_m2 = 16;
  ChannelNetwork lake;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, lake, error), error);
  CHECK(only(lake, HollowKind::Lake) != nullptr);
  CHECK(measure_hollows(p, lake).climbing == 0);
}

TEST_CASE("hollows: a scene's body under its basin's spill is where the basin's water ends") {
  const ChannelLayer p = layer();
  const Bowl b;
  std::string error;
  ChannelNetwork bare;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, bare, error), error);
  const Hollow* found = only(bare, HollowKind::Lake);
  REQUIRE(found != nullptr);
  const f64 spill = found->spill_m;
  // The pond's circle reaches the lip and no further, so the valley below the lip is not in it.
  const f64 radius = 300 - found->spill_z + 1;
  // Three metres under the spill: no outlet can leave it without a cut three metres deep.
  {
    const Vector<scene::WaterBody> bodies{pond(spill - 3.0, radius)};
    scene_gen::Exclusion water;
    REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
    ChannelNetwork n;
    REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, n, error, &water), error);
    const Hollow* terminal = only(n, HollowKind::AuthoredTerminal);
    REQUIRE(terminal != nullptr);
    CHECK(terminal->level_m == doctest::Approx(spill - 3.0).epsilon(1e-5));
    CHECK(terminal->spill_m == doctest::Approx(spill).epsilon(1e-6));
    CHECK(plan_bodies_json(p, n).as_array().empty());
    // What comes in ends at its shore; nothing begins there.
    CHECK(n.water.mouths >= 1);
    CHECK(n.water.outlets == 0);
    for (const ChannelLink& link : n.links)
      if (link.mouth) CHECK(link.line.back().at.y == doctest::Approx(spill - 3.0).epsilon(1e-3));
    const HollowStats stats = measure_hollows(p, n);
    CHECK(stats.climbing == 0);
    CHECK(stats.ends_higher == 0);
    // No line runs under the body: above the lip, every point of every line is at or over the
    // body's level.
    for (const ChannelLink& link : n.links)
      for (const ChannelPoint& pt : link.line)
        if (pt.at.z > terminal->spill_z) CHECK(pt.at.y >= spill - 3.0 - 1e-3);
  }
  // Half a metre under the spill, inside the breach limit: the author's lake, with its outlet.
  {
    const Vector<scene::WaterBody> bodies{pond(spill - .5, radius)};
    scene_gen::Exclusion water;
    REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
    ChannelNetwork n;
    REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, n, error, &water), error);
    const Hollow* authored = only(n, HollowKind::Authored);
    REQUIRE(authored != nullptr);
    CHECK(authored->level_m == doctest::Approx(spill - .5).epsilon(1e-5));
    CHECK(n.water.mouths >= 1);
    CHECK(n.water.outlets == 1);
    const HollowStats stats = measure_hollows(p, n);
    CHECK(stats.climbing == 0);
    CHECK(stats.ends_higher == 0);
    // The outlet's bed runs through the lip no higher than it left the shore.
    CHECK(stats.trench_max_m <= .5 + .6);
  }
}

TEST_CASE("hollows: a basin the author names dry has no body, and its lines end at its floor") {
  ChannelLayer p = layer();
  const Bowl b;
  std::string error;
  ChannelBasin dry;
  dry.name = "the sink";
  dry.at = {128, 300};
  dry.dry = true;
  p.basins.push_back(dry);
  ChannelNetwork n;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, n, error), error);
  const Hollow* hollow = only(n, HollowKind::Dry);
  REQUIRE(hollow != nullptr);
  CHECK(hollow->name == "the sink");
  CHECK(hollow->depth_m() > 3.0);
  CHECK(plan_bodies_json(p, n).as_array().empty());
  CHECK(n.water.outlets == 0);
  CHECK(n.water.mouths == 0);
  const HollowStats stats = measure_hollows(p, n);
  CHECK(stats.climbing == 0);
  CHECK(stats.ends_higher == 0);
  // A line runs down into the hollow and ends at its lowest cell.
  bool reaches = false;
  for (const ChannelLink& link : n.links) {
    if (link.line.size() < 2) continue;
    const ChannelPoint& end = link.line.back();
    reaches = reaches || (std::fabs(end.at.x - hollow->floor_x) <= 1e-6 &&
                          std::fabs(end.at.z - hollow->floor_z) <= 1e-6);
  }
  CHECK(reaches);
  // A name that stands in no hollow is refused by that name.
  ChannelLayer wrong = layer();
  dry.name = "nowhere";
  dry.at = {40, 100};
  wrong.basins.push_back(dry);
  ChannelNetwork none;
  CHECK_FALSE(plan_channels(wrong, ground(b), 0, nullptr, none, error));
  CHECK(error.find("nowhere") != std::string::npos);
  // And a point outside the region by the range check.
  ChannelLayer outside = layer();
  dry.at = {-40, 100};
  outside.basins.push_back(dry);
  CHECK_FALSE(plan_channels(outside, ground(b), 0, nullptr, none, error));
  CHECK(error.find("basins") != std::string::npos);
}

TEST_CASE("hollows: a basin its catchment cannot fill stands lower, with no outlet") {
  ChannelLayer p = layer();
  const Bowl b;
  std::string error;
  ChannelNetwork full;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, full, error), error);
  const Hollow* lake = only(full, HollowKind::Lake);
  REQUIRE(lake != nullptr);
  // With no loss named the lake's outlet carries all that runs in.
  CHECK(lake->loss_m3s == 0);
  CHECK(lake->inflow_m3s > 0);
  // The default runoff is 30 l/s/km2, 946.7 mm a year. A bed that seeps three times what the
  // catchment sends to each square metre of the full lake cannot be filled.
  const f64 runoff_mm = 30.0 * 31.5576;
  const f64 breakeven = runoff_mm * (lake->catchment_m2 - lake->area_m2) / lake->area_m2;
  const u64 key = plan_layer_hash(p);
  p.basin_seepage_mm_y = static_cast<f32>(3 * breakeven);
  ChannelNetwork n;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, n, error), error);
  const Hollow* low = only(n, HollowKind::Terminal);
  REQUIRE(low != nullptr);
  CHECK(low->level_m < lake->level_m - .25);
  CHECK(low->level_m > low->floor_m);
  CHECK(n.water.outlets == 0);
  CHECK(n.water.mouths >= 1);
  for (const ChannelLink& link : n.links)
    if (link.mouth) CHECK(link.mouth_level == low->level_m);
  CHECK(plan_bodies_json(p, n).as_array().size() == 1);
  // It stands where its losses meet its inflow: within what one more ring of cells would lose.
  CHECK(low->inflow_m3s >= low->loss_m3s);
  CHECK(low->loss_m3s >= .6 * low->inflow_m3s);
  u32 wet = 0;
  for (const u32 body : n.body_of)
    wet += body != 0 ? 1u : 0u;
  CHECK(static_cast<f64>(wet) * 16.0 < .5 * lake->area_m2);
  CHECK(wet >= 4);
  const HollowStats stats = measure_hollows(p, n);
  CHECK(stats.climbing == 0);
  CHECK(stats.ends_higher == 0);
  // A loss makes the water part of what the plan is made from; without one it is not.
  CHECK(plan_layer_hash(p) != key);
  ChannelLayer wetter = p;
  wetter.flow.runoff_l_s_km2 = 60;
  CHECK(plan_layer_hash(wetter) != plan_layer_hash(p));
  ChannelLayer no_loss = layer();
  no_loss.flow.runoff_l_s_km2 = 60;
  CHECK(plan_layer_hash(no_loss) == key);
  // A spring in the catchment that brings what the bed loses fills it to its spill again.
  ChannelLayer fed = p;
  ChannelSource spring;
  spring.name = "spring";
  spring.at = WorldPos{128, 0, 420};
  spring.flow_m3s = 1.0f;
  fed.flow.sources.push_back(spring);
  ChannelNetwork fills;
  REQUIRE_MESSAGE(plan_channels(fed, ground(b), 0, nullptr, fills, error), error);
  REQUIRE(only(fills, HollowKind::Lake) != nullptr);
  CHECK(only(fills, HollowKind::Lake)->level_m == lake->level_m);
  CHECK(fills.water.outlets == 1);
  // Half the seepage and the catchment alone fills it.
  p.basin_seepage_mm_y = static_cast<f32>(.5 * breakeven);
  ChannelNetwork half;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 0, nullptr, half, error), error);
  REQUIRE(only(half, HollowKind::Lake) != nullptr);
  CHECK(only(half, HollowKind::Lake)->level_m == lake->level_m);
  CHECK(only(half, HollowKind::Lake)->loss_m3s > 0);
  CHECK(only(half, HollowKind::Lake)->loss_m3s < only(half, HollowKind::Lake)->inflow_m3s);
  // A floor that leaks more than runs in: nothing stands, and the lines end at its floor.
  ChannelLayer leaky = layer();
  ChannelBasin talus;
  talus.name = "talus floor";
  talus.at = {128, 300};
  talus.leak_m3s = 1.0f;
  leaky.basins.push_back(talus);
  ChannelNetwork dry;
  REQUIRE_MESSAGE(plan_channels(leaky, ground(b), 0, nullptr, dry, error), error);
  REQUIRE(only(dry, HollowKind::Dry) != nullptr);
  CHECK(only(dry, HollowKind::Dry)->loss_m3s >= 1.0);
  CHECK(dry.water.outlets == 0);
  CHECK(plan_bodies_json(leaky, dry).as_array().empty());
  CHECK(measure_hollows(leaky, dry).climbing == 0);
}

TEST_CASE("hollows: the same plan at one job and six, and through its file") {
  ChannelLayer p = layer();
  p.head_threshold_m2 = 300;
  const Bowl b;
  std::string error;
  ChannelPlan serial, parallel;
  REQUIRE_MESSAGE(plan_channels(p, ground(b), 7, nullptr, serial.network, error), error);
  jobs::JobSystemConfig config;
  config.performance_workers = 6;
  config.efficiency_workers = 0;
  config.pin_threads = false;
  jobs::JobSystem js(config);
  REQUIRE(plan_channels(p, ground(b), 7, &js, parallel.network, error));
  const std::string bytes = plan_bytes(serial);
  CHECK(bytes == plan_bytes(parallel));
  CHECK(serial.network.body_of == parallel.network.body_of);
  CHECK(write_json(plan_bodies_json(p, serial.network)) ==
        write_json(plan_bodies_json(p, parallel.network)));
  CHECK(write_json(terrain_cuts_json(p, serial.network)) ==
        write_json(terrain_cuts_json(p, parallel.network)));
  // The file holds the hollows and every outlet's level: a plan read back writes the same
  // bodies and the same cuts.
  ChannelPlan read;
  REQUIRE_MESSAGE(read_plan(bytes, read, error), error);
  REQUIRE(read.network.basins.hollows.size() == serial.network.basins.hollows.size());
  CHECK(read.network.basins.passes == serial.network.basins.passes);
  CHECK(write_json(plan_bodies_json(p, read.network)) ==
        write_json(plan_bodies_json(p, serial.network)));
  CHECK(write_json(terrain_cuts_json(p, read.network)) ==
        write_json(terrain_cuts_json(p, serial.network)));
  CHECK(plan_bytes(read) == bytes);
  const HollowStats a = measure_hollows(p, serial.network), c = measure_hollows(p, read.network);
  CHECK(a.climbing == 0);
  CHECK(c.climbing == 0);
  CHECK(a.trench_max_m == c.trench_max_m);
}
