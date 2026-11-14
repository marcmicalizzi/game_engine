// A network's water from its sources (channels.md, "Discharge"; roadmap W120 stage A): the
// discharge summed down the links and conserved at a confluence, a point source joining
// everything downstream of it, a link wet from where its discharge is a sheet's, and the
// planner's links solved downstream first so a tributary meets its main stem's level.
#include <domain/channels/discharge.h>
#include <domain/channels/falls.h>
#include <domain/channels/flow_fixture.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::channels;

namespace {

ChannelLink link_of(std::initializer_list<u32> cells, f64 x0, f64 z0, f64 x1, f64 z1, f32 area0,
                    f32 area1, f32 width, u32 points = 11) {
  ChannelLink link;
  for (const u32 c : cells)
    link.cells.push_back(c);
  for (u32 i = 0; i < points; ++i) {
    const f64 u = static_cast<f64>(i) / static_cast<f64>(points - 1);
    ChannelPoint p;
    p.at = WorldPos{x0 + u * (x1 - x0), 0.0, z0 + u * (z1 - z0)};
    p.width_m = width;
    p.depth_m = 0.3f;
    p.area_m2 = area0 + static_cast<f32>(u) * (area1 - area0);
    link.line.push_back(p);
  }
  link.keep = points;
  link.seed = 77;
  return link;
}

// Two tributaries (links 0 and 1) meeting at cell 12, where link 2 begins. (Not `fork`: that is
// POSIX's, and Clang and GCC find the call ambiguous.)
ChannelNetwork forked_network() {
  ChannelNetwork n;
  n.links.push_back(link_of({10, 11, 12}, 0, 100, 100, 0, 2e5f, 6e5f, 1.0f));
  n.links.push_back(link_of({20, 21, 12}, 0, -100, 100, 0, 1e5f, 3e5f, 1.0f));
  n.links.push_back(link_of({12, 13, 14}, 100, 0, 300, 0, 9e5f, 15e5f, 2.0f));
  return n;
}

f64 fixture_height(const void*, i64 x_mm, i64 z_mm) noexcept {
  return flow_fixture_ground(static_cast<f64>(x_mm) / 1000.0, static_cast<f64>(z_mm) / 1000.0);
}

}  // namespace

TEST_CASE("discharge: runoff over the catchment, summed down the links and conserved") {
  ChannelLayer layer;
  const ChannelNetwork n = forked_network();
  NetworkFlow flow;
  std::string error;
  REQUIRE_MESSAGE(network_flow(layer, n, flow, error), error);
  REQUIRE(flow.links.size() == 3);
  CHECK(flow.links[0].down == 2u);
  CHECK(flow.links[1].down == 2u);
  CHECK(flow.links[2].down == ~0u);
  // 30 l/s a km2 at the default: 6e5 m2 gives 18 l/s.
  CHECK(flow.links[0].tail_m3s == doctest::Approx(0.018));
  CHECK(flow.links[1].tail_m3s == doctest::Approx(0.009));
  // The confluence: what the two bring is what the stem begins with (its area is theirs).
  CHECK(flow.links[2].discharge[0] ==
        doctest::Approx(flow.links[0].tail_m3s + flow.links[1].tail_m3s));
  CHECK(flow.links[2].tail_m3s == doctest::Approx(0.045));
  CHECK(flow.largest_m3s == doctest::Approx(0.045));
  // The stage of the year scales all of it; the runoff is the scene's.
  layer.flow.stage = 0.5f;
  layer.flow.runoff_l_s_km2 = 60.0f;
  NetworkFlow same;
  REQUIRE(network_flow(layer, n, same, error));
  CHECK(same.links[2].tail_m3s == doctest::Approx(0.045));
  layer.flow.stage = 3.0f;
  REQUIRE(network_flow(layer, n, same, error));
  CHECK(same.links[2].tail_m3s == doctest::Approx(0.27));
}

TEST_CASE("discharge: a link is wet from where its water is a sheet over its bed") {
  ChannelLayer layer;
  const ChannelNetwork n = forked_network();
  NetworkFlow flow;
  std::string error;
  REQUIRE(network_flow(layer, n, flow, error));
  // A sheet's: a critical depth of 1 cm, 3.13 l/s a metre of bed.
  const f64 sheet = std::sqrt(9.81 * 1e-6);
  CHECK(sheet == doctest::Approx(0.003132).epsilon(1e-3));
  CHECK(critical_depth(sheet) == doctest::Approx(0.01).epsilon(1e-9));
  // Link 0 (a metre wide, 6 to 18 l/s): wet from its head. Link 1 (3 to 9 l/s): from where it
  // passes 3.13 l/s. Link 2 (2 m wide, 27 to 45 l/s): from its head.
  CHECK(flow.links[0].reason == DryReason::Wet);
  CHECK(flow.links[0].first_wet == 0);
  CHECK(flow.links[1].reason == DryReason::Wet);
  CHECK(flow.links[1].first_wet == 1);
  CHECK(flow.links[1].discharge[0] < sheet);
  CHECK(flow.links[1].discharge[1] >= sheet);
  CHECK(flow.links[2].first_wet == 0);
  CHECK(flow.wet == 3);
  // A dry late summer: a tenth of the mean. The small tributary is a gully, and says so.
  layer.flow.stage = 0.1f;
  REQUIRE(network_flow(layer, n, flow, error));
  CHECK(flow.links[0].reason == DryReason::UnderSheet);
  CHECK(flow.links[1].reason == DryReason::UnderSheet);
  CHECK(flow.links[1].first_wet == 11);
  CHECK(flow.links[1].tail_sheet_m < 0.01);
  CHECK(flow.links[2].reason == DryReason::UnderSheet);  // 4.5 l/s over 2 m
  CHECK(flow.wet == 0);
  CHECK(flow.dry_under_sheet == 3);
  CHECK(flow.dry_m > 0.0);
  // A tributary inside its main stem's banks has nothing to carry it.
  ChannelNetwork inside = forked_network();
  inside.links[1].keep = 1;
  layer.flow.stage = 1.0f;
  REQUIRE(network_flow(layer, inside, flow, error));
  CHECK(flow.links[1].reason == DryReason::TooShort);
  CHECK(flow.dry_too_short == 1);
}

TEST_CASE("discharge: a point source joins its link and everything downstream of it") {
  ChannelLayer layer;
  layer.flow.stage = 0.1f;  // every link dry by its rain alone
  ChannelSource spring;
  spring.name = "spring";
  spring.at = WorldPos{52.0, 0.0, -48.0};  // by link 1's sixth point (50, -50)
  spring.flow_m3s = 0.2f;
  layer.flow.sources.push_back(spring);
  const ChannelNetwork n = forked_network();
  NetworkFlow flow, bare;
  std::string error;
  REQUIRE_MESSAGE(network_flow(layer, n, flow, error), error);
  ChannelLayer no_spring = layer;
  no_spring.flow.sources.clear();
  REQUIRE(network_flow(no_spring, n, bare, error));
  CHECK(flow.sources == 1);
  const f64 gives = 0.2 * 0.1;  // a spring scales with the year's stage
  for (u32 i = 0; i < 11; ++i) {
    CHECK(flow.links[0].discharge[i] == bare.links[0].discharge[i]);  // the other tributary: none
    CHECK(flow.links[1].discharge[i] ==
          doctest::Approx(bare.links[1].discharge[i] + (i >= 5 ? gives : 0.0)));
    CHECK(flow.links[2].discharge[i] == doctest::Approx(bare.links[2].discharge[i] + gives));
  }
  // The spring is what makes its tributary and the stem run, from where it rises.
  CHECK(flow.links[0].reason == DryReason::UnderSheet);
  CHECK(flow.links[1].reason == DryReason::Wet);
  CHECK(flow.links[1].first_wet == 5);
  CHECK(flow.links[2].reason == DryReason::Wet);
  CHECK(flow.links[1].sources_m3s == doctest::Approx(gives));
  // A melt has its own season, whatever the year's stage.
  layer.flow.sources[0].kind = ChannelSourceKind::Melt;
  layer.flow.sources[0].season = 0.5f;
  REQUIRE(network_flow(layer, n, flow, error));
  CHECK(flow.links[2].discharge[0] == doctest::Approx(bare.links[2].discharge[0] + 0.1));
  // A source on no channel is refused by name.
  layer.flow.sources[0].at = WorldPos{50.0, 0.0, 400.0};
  CHECK_FALSE(network_flow(layer, n, flow, error));
  CHECK_MESSAGE(error.find("the source \"spring\" stands on no channel") != std::string::npos,
                error);
  layer.flow.sources[0].at = spring.at;
  layer.flow.sources[0].flow_m3s = 0.0f;
  CHECK_FALSE(network_flow(layer, n, flow, error));
  layer.flow.sources.clear();
  layer.flow.runoff_l_s_km2 = 500.0f;
  CHECK_FALSE(network_flow(layer, n, flow, error));
}

TEST_CASE("discharge: naming a source re-plans nothing") {
  ChannelLayer layer;
  layer.name = "region";
  const u64 before = plan_layer_hash(layer);
  ChannelSource spring;
  spring.name = "spring";
  spring.flow_m3s = 0.2f;
  layer.flow.sources.push_back(spring);
  layer.flow.stage = 0.3f;
  layer.flow.runoff_l_s_km2 = 12.0f;
  CHECK(plan_layer_hash(layer) == before);
  layer.bank_slope = 1.5f;
  CHECK(plan_layer_hash(layer) != before);
}

TEST_CASE("discharge: the planner's links are solved downstream first, and meet") {
  // The fixture's uniform trench as two links, one flowing into the other at x = 38.
  ChannelLayer layer;
  layer.name = "trench";
  layer.flow.runoff_l_s_km2 = 30.0f;
  ChannelNetwork n;
  n.links.push_back(link_of({1, 2, 3}, 8, -20, 38, -20, 2.6e7f, 2.6e7f, 8.0f, 31));
  n.links.push_back(link_of({3, 4, 5}, 38, -20, 48, -20, 2.6e7f, 2.6e7f, 8.0f, 11));
  for (ChannelLink& link : n.links)
    link.reach.assign(link.line.size(), 2);
  NetworkFlow flow;
  std::string error;
  REQUIRE_MESSAGE(network_flow(layer, n, flow, error), error);
  CHECK(flow.links[0].down == 1u);
  CHECK(flow.wet == 2);
  scene_gen::Ground ground;
  ground.surface_fn = &fixture_height;
  ProfileOptions options;
  options.resistance = Resistance::Manning;
  Vector<SolvedLink> solved;
  REQUIRE_MESSAGE(solve_network(layer, n, flow, ground, options, nullptr, solved, error), error);
  REQUIRE(solved.size() == 2);
  REQUIRE(solved[0].wet);
  REQUIRE(solved[1].wet);
  const scene::WaterChannel& upper = solved[0].block;
  const scene::WaterChannel& lower = solved[1].block;
  CHECK(upper.name == "trench link 0");
  CHECK(lower.name == "trench link 1");
  REQUIRE_MESSAGE(scene_gen::water_channel_solved(upper, &error), error);
  REQUIRE_MESSAGE(scene_gen::water_channel_solved(lower, &error), error);
  // The tributary's last station carries the stem's level there, to the millimetre, as a
  // confluence's control.
  const u32 last = upper.points.size() - 1;
  CHECK(upper.points[last].x == lower.points[0].x);
  CHECK(upper.points[last].y == lower.points[0].y);
  CHECK(upper.flags[last] ==
        scene_gen::channel_control_flags(scene_gen::ChannelControl::Confluence));
  CHECK(lower.flags[0] == 0);
  // One reach in two links is the reach: normal depth for 0.78 m3/s throughout.
  const f64 depth = trapezoid_normal(Trapezoid{2.0, 1.0}, 0.78, 0.04, 0.01);
  for (const f32 d : upper.depth_m)
    CHECK(static_cast<f64>(d) == doctest::Approx(depth).epsilon(2e-3));
  for (const f32 d : lower.depth_m)
    CHECK(static_cast<f64>(d) == doctest::Approx(depth).epsilon(2e-3));
  // The planner's own arrays ride along: a reach code a station, the seed.
  CHECK(upper.reach.size() == upper.points.size());
  CHECK(upper.reach[0] == 2);
  CHECK(upper.seed == 77u);
  // A dry network solves to nothing, and says which links.
  layer.flow.stage = 0.0f;
  REQUIRE(network_flow(layer, n, flow, error));
  REQUIRE(solve_network(layer, n, flow, ground, options, nullptr, solved, error));
  CHECK_FALSE(solved[0].wet);
  CHECK_FALSE(solved[1].wet);
}
