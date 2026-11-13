// Water that leaves its bed (roadmap S104, stages 0 and 1; channels.md, "Where the water leaves
// its bed (S104)"; the design's test scenes in docs/experiments/waterfalls.md, "The test scenes,
// derived before anything is drawn"): the march against the free jet's closed forms, the fall test
// counted on the solver's own steps of known heights down a fall line, across a slope and over a
// sea cliff, and the cook's bytes the same at any thread count and from every toolchain.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/channels/channels.h>
#include <domain/channels/falls.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::channels;

namespace {
constexpr f64 g = 9.81;

// A lip on a level approach over a face, into water: the approach from s = -4 m to the lip at 0 in
// half metres at `lip` m, the face down to the floor at `floor_bed`, the floor on to 12 m with
// `pool` m of water over it. A face of `run` m (1 mm: vertical).
FallProfile ledge(f64 lip, f64 floor_bed, f64 pool, f64 run) {
  FallProfile p;
  for (i32 i = -8; i <= 0; ++i) {
    p.s.push_back(.5 * i);
    p.bed.push_back(lip);
    p.water.push_back(.2);
  }
  p.s.push_back(run);
  p.bed.push_back(floor_bed);
  p.water.push_back(pool);
  for (i32 i = 1; i <= 24; ++i) {
    if (.5 * i <= run) continue;
    p.s.push_back(.5 * i);
    p.bed.push_back(floor_bed);
    p.water.push_back(pool);
  }
  return p;
}
// The brink's speed (Rouse 1936): q over 0.715 of the critical depth.
f64 brink_speed(f64 q) { return q / (.715 * std::cbrt(q * q / g)); }
}  // namespace

// **The free jet from a critical lip** (waterfalls.md, the test scenes A and B): a parcel leaving a
// subcritical stream's brink at v_b = q / (0.715 d_c) horizontally lands v_b sqrt(2 H / g) out
// after sqrt(2 H / g) at sqrt(v_b^2 + 2 g H), as thin as q over that speed. The march's landing is
// the closed form to rounding, and the page's own rounded numbers hold.
TEST_CASE("falls: a free jet from a critical lip lands where the closed form puts it") {
  struct Case {
    f64 q, drop, x_page, t_page, v_page;
    bool fall;
  };
  // A: 10 m of free fall at 0.3 m^2/s; B: a 2 m step at 0.3, 1.5 and 3 m^2/s (3.3 d_c: a fall,
  // barely; 2.1 d_c at 3 m^2/s: a drowned step). The page's numbers, rounded as it prints them.
  const Case cases[] = {{.3, 10, 2.86, 1.43, 14.1, true},
                        {.3, 2, 1.28, 0, 6.6, true},
                        {1.5, 2, 0, 0, 0, true},
                        {3.0, 2, 0, 0, 0, false}};
  for (const Case& c : cases) {
    // The pool is 1.5 m deep: its floor 1.5 m under the surface the jet lands on.
    const FallProfile p = ledge(c.drop + 1.5, 0, 1.5, 1e-3);
    // Subcritical on the approach: a depth of 1.5 critical depths.
    const f64 dc = critical_depth(c.q);
    CHECK(dc == doctest::Approx(std::cbrt(c.q * c.q / g)).epsilon(1e-14));
    const f64 approach = c.q / (1.5 * dc);
    FallMarch march;
    REQUIRE(march_column(p, c.q, approach, .04, 0, p.s.size() - 1, 0.0, march));
    REQUIRE(march.flights.size() == 1);
    const FallFlight& f = march.flights[0];
    const f64 vb = brink_speed(c.q);
    const f64 t = std::sqrt(2 * c.drop / g);
    CHECK(f.s0 == 0);
    CHECK(f.speed0 == doctest::Approx(vb).epsilon(1e-12));
    CHECK(f.landing == FallLanding::Water);
    CHECK(f.drop() == doctest::Approx(c.drop).epsilon(1e-12));
    CHECK(f.s1 == doctest::Approx(vb * t).epsilon(1e-12));
    CHECK(f.time_s == doctest::Approx(t).epsilon(1e-12));
    CHECK(f.speed1 == doctest::Approx(std::sqrt(vb * vb + 2 * g * c.drop)).epsilon(1e-12));
    if (c.x_page > 0) CHECK(std::fabs(f.s1 - c.x_page) < .005);
    if (c.t_page > 0) CHECK(std::fabs(f.time_s - c.t_page) < .005);
    if (c.v_page > 0) CHECK(std::fabs(f.speed1 - c.v_page) < .05);
    if (c.t_page > 0) CHECK(std::fabs(c.q / f.speed1 - .021) < .0005);  // A: 2.1 cm thick
    // The fall test: a flight drop of three critical depths and a critical depth of a centimetre.
    CHECK((f.drop() >= k_fall_min_drop_depths * dc && dc >= k_fall_min_critical_m) == c.fall);
    // The path aloft is the parabola, sampled closer at the lip than lower down.
    u32 aloft = 0;
    f64 first_step = 0, longest_step = 0, previous_s = 0, previous_y = 0;
    for (const FallSample& s : march.path) {
      if (s.phase != FallPhase::Air) {
        previous_s = s.s;
        previous_y = s.y;
        continue;
      }
      const f64 x = s.s;
      CHECK(s.y == doctest::Approx(c.drop + 1.5 - g * x * x / (2 * vb * vb)).epsilon(1e-9));
      const f64 step = std::hypot(s.s - previous_s, s.y - previous_y);
      if (aloft == 0) first_step = step;
      longest_step = std::max(longest_step, step);
      previous_s = s.s;
      previous_y = s.y;
      ++aloft;
    }
    CHECK(aloft >= 3);
    CHECK(first_step < longest_step);
  }
}

// **The slide** (waterfalls.md, test scene C): 0.05 m^2/s over a 60-degree face 8 m high strikes it
// y_r = 2 v_b^2 tan^2 60 / g down (3.91 d_c tan^2 60, 0.74 m) and runs down the rest as a film
// at Manning's normal speed on the face, d = (q n / sqrt(sin 60))^(3/5) (1.7 cm at 3.0 m/s at n
// 0.02): a horsetail, a quarter as fast as a free fall of the same height.
TEST_CASE("falls: water over a 60-degree face strikes it 3.91 d_c tan^2 60 down and slides") {
  const f64 q = .05, n = .02, tan60 = 1.7320508075688772, sin60 = .8660254037844386;
  FallProfile p;
  for (i32 i = -8; i <= 0; ++i) {
    p.s.push_back(.5 * i);
    p.bed.push_back(8);
    p.water.push_back(.1);
  }
  const f64 run = 8 / tan60;
  for (i32 i = 1; i <= 80; ++i) {
    p.s.push_back(run * i / 80.0);
    p.bed.push_back(8 - tan60 * run * i / 80.0);
    p.water.push_back(0);
  }
  for (i32 i = 1; i <= 8; ++i) {
    p.s.push_back(run + .5 * i);
    p.bed.push_back(0);
    p.water.push_back(.02);
  }
  const f64 dc = critical_depth(q);
  FallMarch march;
  REQUIRE(march_column(p, q, q / (1.5 * dc), n, 0, p.s.size() - 1, 0.0, march));
  REQUIRE(march.flights.size() == 1);
  const FallFlight& f = march.flights[0];
  const f64 vb = brink_speed(q);
  CHECK(vb == doctest::Approx(1.10).epsilon(.005));
  const f64 strike = 2 * vb * vb * tan60 * tan60 / g;
  CHECK(f.landing == FallLanding::Rock);
  CHECK(f.drop() == doctest::Approx(strike).epsilon(1e-9));
  CHECK(f.drop() == doctest::Approx(3.91 * dc * 3).epsilon(.002));
  CHECK(std::fabs(f.drop() - .74) < .005);
  CHECK(f.s1 == doctest::Approx(strike / tan60).epsilon(1e-9));
  // The film at the face's foot: Manning's normal speed on the face, within 2 %.
  const f64 depth = std::pow(q * n / std::sqrt(sin60), .6);
  const f64 normal = q / depth;
  CHECK(std::fabs(depth - .017) < .0005);
  CHECK(std::fabs(normal - 3.0) < .05);
  f64 foot = 0;
  u32 slid = 0;
  for (const FallSample& s : march.path) {
    if (s.phase == FallPhase::Slide) {
      ++slid;
      foot = s.speed;
    }
  }
  CHECK(slid > 60);
  CHECK(foot == doctest::Approx(normal).epsilon(.02));
  // A quarter as fast as the free fall from the same height would be at its foot.
  CHECK(foot / std::sqrt(vb * vb + 2 * g * 8) == doctest::Approx(.24).epsilon(.05));
}

namespace {
// **The solver's own steps** (stage 0): a valley falling 3 % toward +x, its floor at z = 128 m,
// with four cliffs across it of known heights — 3 m (no knickpoint for a channel this wide: the
// chute's limit spreads it), 7, 10 and 14 m — at x = 40, 90, 140 and 190 m, each over 25 cm. The
// context's first value is the cross-slope across the line (0: a V of sides at 0.3, the fall line;
// else W116's hillside at that tangent with its bench at the toe, the line across the slope).
struct StepScene {
  f64 cross = 0;
};
constexpr f64 k_step_x[4] = {40, 90, 140, 190};
constexpr f64 k_step_h[4] = {3, 7, 10, 14};
f64 cliffs(f64 x) {
  f64 down = 0;
  for (u32 k = 0; k < 4; ++k) {
    if (x >= k_step_x[k] + .25)
      down += k_step_h[k];
    else if (x > k_step_x[k])
      down += k_step_h[k] * (x - k_step_x[k]) / .25;
  }
  return down;
}
f64 stepped(const void* context, i64 x, i64 z) noexcept {
  const f64 t = static_cast<const StepScene*>(context)->cross;
  const f64 xx = static_cast<f64>(x) / 1000.0, a = static_cast<f64>(z) / 1000.0 - 128;
  f64 across = 0;
  if (t == 0)
    across = .3 * std::fabs(a);
  else
    across = a >= 0 ? t * a : (a >= -1 ? -.03 * a : .03 - t * (-a - 1));
  return 80 - .03 * xx - cliffs(xx) + across;
}
ChannelLayer step_layer(u64 seed) {
  ChannelLayer p;
  p.name = "steps";
  p.region_min = {0, 0};
  p.region_max = {256, 256};
  p.spacing_m = 1;
  p.head_threshold_m2 = 60;
  p.width_at_km2_m = 4;
  p.seed = seed;
  return p;
}
const ChannelLink* longest(const ChannelNetwork& n) {
  const ChannelLink* best = nullptr;
  for (const ChannelLink& link : n.links)
    if (best == nullptr || link.length_m > best->length_m) best = &link;
  return best;
}
// The falls of one link, by the step whose cliff their lip stands at (within 3 m upstream of it).
struct Counted {
  u32 found[4] = {};
  f64 drop[4] = {};
  u32 other = 0;
};
Counted count_steps(const ChannelNetwork& n, const FallSet& set, u32 link) {
  Counted c;
  for (const Fall& f : set.falls) {
    if (f.link != link || static_cast<FallKind>(f.kind) < FallKind::Plunge) continue;
    const f64 x = static_cast<f64>(f.x_mm) / 1e3;
    bool matched = false;
    for (u32 k = 0; k < 4; ++k)
      if (x > k_step_x[k] - 3 && x < k_step_x[k] + 1) {
        ++c.found[k];
        c.drop[k] = std::max(c.drop[k], static_cast<f64>(f.drop_mm) / 1e3);
        matched = true;
      }
    c.other += matched ? 0 : 1;
  }
  (void)n;
  return c;
}
}  // namespace

// **A ladder of small risers** (the coordinator's question of the night: a cut bed read as a
// staircase, a riser at every half metre of line): 20 treads of 0.5 m, level, each ending in a
// vertical riser of h, the water `stage`-deep over each tread. Printed for risers of 0.1, 0.2, 0.3
// and 0.6 m at 0.04 and 0.3 m^2/s (critical depths 5.5 and 20.9 cm). The test counts flights, not
// risers: a riser is a fall when the flight off it drops three critical depths, and a jet that
// clears a tread drops two risers in one flight.
TEST_CASE("falls: a ladder of half-metre treads makes a fall only where one flight drops 3 d_c") {
  for (const f64 q : {.04, .3}) {
    const f64 dc = critical_depth(q);
    for (const f64 h : {.1, .2, .3, .6}) {
      FallProfile p;
      f64 y = 20;
      for (u32 i = 0; i < 20; ++i) {
        p.s.push_back(.5 * i);
        p.bed.push_back(y);
        p.water.push_back(.5 * dc);
        p.s.push_back(.5 * i + .499);
        p.bed.push_back(y);
        p.water.push_back(.5 * dc);
        y -= h;
      }
      FallMarch march;
      REQUIRE(march_column(p, q, q / (1.5 * dc), .04, 0, p.s.size() - 1, 0.0, march));
      u32 falls = 0;
      f64 most = 0;
      for (const FallFlight& f : march.flights) {
        most = std::max(most, f.drop());
        falls += f.drop() >= k_fall_min_drop_depths * dc && dc >= k_fall_min_critical_m ? 1 : 0;
      }
      MESSAGE("q " << q << " (d_c " << dc << "), risers " << h << " m (" << h / dc
                   << " d_c): " << march.flights.size() << " flights, the highest " << most
                   << " m, " << falls << " falls");
      // Every riser is left (19 flights); a flight drops its riser less the water standing on the
      // tread below, so a riser counts when that is three critical depths; and at 0.3 m^2/s the
      // jet off a 0.6 m riser (2.9 d_c) clears the next tread and drops two risers at once, a fall
      // every other riser.
      const u32 expected = q < .1 ? (h >= .2 ? 19u : 0u) : (h >= .6 ? 9u : 0u);
      CHECK(falls == expected);
      CHECK(march.flights.size() >= (h >= .6 && q > .1 ? 10u : 19u));
    }
  }
}

namespace {
// One planned scene of the cliffs and what the cook made of it: the trunk's falls by cliff, the
// stats, and at each cliff (within 3 m of it) the largest seat W116 gave a point and the deepest
// the bed stands under a point downstream of it (a pit at the cliff's foot).
struct StepRun {
  Counted c;
  FallStats stats;
  f64 seat[4] = {};
  f64 pit[4] = {};
  u32 knickpoints = 0;
  bool ok = false;
};
StepRun run_steps(f64 cross, u64 seed, bool move) {
  StepRun r;
  ChannelLayer p = step_layer(seed);
  p.bank_move = move;
  StepScene scene;
  scene.cross = cross;
  scene_gen::Ground ground;
  ground.surface_fn = &stepped;
  ground.context = &scene;
  ChannelNetwork n;
  std::string error;
  if (!plan_channels(p, ground, seed, nullptr, n, error)) return r;
  const ChannelLink* trunk = longest(n);
  FallSet set;
  if (trunk == nullptr || trunk->length_m < 200 ||
      !cook_falls(p, n, 0, FallOptions{}, nullptr, set, error))
    return r;
  const u32 index = static_cast<u32>(trunk - n.links.data());
  r.c = count_steps(n, set, index);
  r.stats = set.stats;
  r.knickpoints = trunk->knickpoints;
  const u32 m = trunk->line.size();
  for (u32 k = 0; k < 4; ++k)
    for (u32 i = 0; i < m && i < trunk->seat.size(); ++i) {
      const f64 x = trunk->line[i].at.x;
      if (x < k_step_x[k] - 3 || x > k_step_x[k] + 3) continue;
      r.seat[k] = std::max(r.seat[k], static_cast<f64>(trunk->seat[i]));
      for (u32 j = i + 1; j < m && trunk->line[j].at.x < k_step_x[k] + 3; ++j)
        r.pit[k] = std::max(r.pit[k], trunk->bed[j] - trunk->bed[i]);
    }
  for (const Fall& f : set.falls)
    if (f.link == index && static_cast<FallKind>(f.kind) >= FallKind::Plunge &&
        (f.critical_um < 10000 || f.knick != 1))
      return r;
  r.ok = true;
  return r;
}
struct StepCase {
  f64 cross;
  u64 seed;
  bool move;
};
// Down the fall line with the move on and off, and across W116's hillside at 20 and 30 degrees.
constexpr StepCase k_step_cases[] = {{0, 1, true},
                                     {0, 2, true},
                                     {0, 1, false},
                                     {0, 2, false},
                                     {.36397023426620234, 1, true},
                                     {.36397023426620234, 2, true},
                                     {.57735026918962573, 1, true},
                                     {.57735026918962573, 2, true}};
}  // namespace

// **The count** (stage 0) on a scene of cliffs of known heights, down a fall line (the move on and
// off) and across a slope: the 7, 10 and 14 m cliffs are knickpoints of a channel a metre wide and
// the 3 m one too; every one of the three is found by the cook as a fall at a marked knickpoint,
// with a sheet's discharge, and nothing far from a cliff is a fall. What each came to — its drop,
// the runs, and what W116's seat did at it — is printed: on b7681ac9 the seat eats steps (the
// case after this one).
TEST_CASE("falls: the solver's steps of known heights are found, down a fall line and across") {
  for (const StepCase& sc : k_step_cases) {
    const StepRun r = run_steps(sc.cross, sc.seed, sc.move);
    REQUIRE(r.ok);
    MESSAGE("cross " << sc.cross << ", seed " << sc.seed << ", bank_move " << sc.move << ": "
                     << r.stats.runs << " runs, " << r.stats.falls << " falls (plunges "
                     << r.stats.plunges << ", horsetails " << r.stats.horsetails << ", tiered "
                     << r.stats.tiered << "); the cliffs' runs " << r.c.found[0] << r.c.found[1]
                     << r.c.found[2] << r.c.found[3] << ", drops " << r.c.drop[0] << ", "
                     << r.c.drop[1] << ", " << r.c.drop[2] << ", " << r.c.drop[3]
                     << " m; the seat at each " << r.seat[0] << ", " << r.seat[1] << ", "
                     << r.seat[2] << ", " << r.seat[3] << " m; the bed's pit at each " << r.pit[0]
                     << ", " << r.pit[1] << ", " << r.pit[2] << ", " << r.pit[3]
                     << " m; knickpoints " << r.knickpoints);
    for (u32 k = 1; k < 4; ++k)
      CHECK_MESSAGE(r.c.found[k] >= 1, "the " << k_step_h[k] << " m cliff");
    CHECK(r.c.other == 0);
    CHECK(r.stats.falls >= 3);
  }
}

// **Every cliff one fall of its own height** (the night's finding for W117, in
// `scratch\b63\S104\seat-eats-step.txt`): this fails on b7681ac9 and is marked so. W116's seat
// reads a point's lower bank across the line's heading; where the line crosses a cliff obliquely
// (the low-pass and the steep wander turn it 20 to 30 degrees there) a reading from a point on or
// above the face lands on the ground below it, the seat becomes the whole step, and its dilation
// over two points carries it onto the lip and the foot: the lip is cut a step deep a metre above
// the cliff and a pit a step deep stands at its foot (seed 2 down the fall line: 12.9 m of seat at
// the 14 m cliff, two runs, drops of 21.9 and 13.9 m). Held when the seat leaves a step its height:
// each cliff one run, a plunge, its drop the cliff's within 0.25 m down the fall line and 0.5 m
// across a slope, and no pit deeper than a decimetre at it.
TEST_CASE("falls: every cliff is one fall of its own height, with no pit at its foot" *
          doctest::may_fail()) {
  for (const StepCase& sc : k_step_cases) {
    const StepRun r = run_steps(sc.cross, sc.seed, sc.move);
    REQUIRE(r.ok);
    const f64 bound = sc.cross == 0 ? .25 : .5;
    for (u32 k = 1; k < 4; ++k) {
      CHECK_MESSAGE(r.c.found[k] == 1, "the " << k_step_h[k] << " m cliff, cross " << sc.cross
                                              << ", seed " << sc.seed);
      CHECK(r.c.drop[k] <= k_step_h[k] + .05);
      CHECK(r.c.drop[k] >= k_step_h[k] - bound);
      CHECK(r.pit[k] <= .1);
    }
    CHECK(r.stats.tiered == 0);
  }
}

namespace {
// A V notch falling 3 % toward +x into a sea over x > 195 m at 34.5 m, a 20 m cliff at x = 200 m
// whose foot stands under the sea's level: the island's streams over their sea cliff, in a valley.
f64 notch_fall(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 cliff = xx >= 200.25 ? 20.0 : (xx > 200 ? 20.0 * (xx - 200) / .25 : 0.0);
  return 60 - .03 * xx - cliff + .3 * std::fabs(zz - 128);
}
scene::WaterBody sea() {
  scene::WaterBody b;
  b.name = "sea";
  b.level_m = 34.5;
  for (const auto& [x, z] : {std::pair<f64, f64>{195, -10}, {300, -10}, {300, 300}, {195, 300}})
    b.shape.polygon.push_back(WorldPos{x, 0, z});
  return b;
}
}  // namespace

// **A mouth over a sea cliff** (the forest's island streams drop 32 and 36 m to the sea and count
// as no fall: their links end at the shore, drain under the description's threshold and carry no
// water block, so a test that reads only the description never sees them). The cook tests every
// link and says what each of these comes to: the link ends at the shoreline on the cliff's face,
// its fan grades the last three widths of bed to the level, and the run at the cliff is a fall
// whose water reaches the sea, counted as a mouth's whether or not the description draws it.
TEST_CASE("falls: a stream over a sea cliff is a fall at its mouth, drawn or not") {
  const ChannelLayer p = step_layer(1);
  const Vector<scene::WaterBody> bodies{sea()};
  scene_gen::Exclusion water;
  std::string error;
  REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
  scene_gen::Ground ground;
  ground.surface_fn = &notch_fall;
  ChannelNetwork n;
  REQUIRE(plan_channels(p, ground, 1, nullptr, n, error, &water));
  const ChannelLink* trunk = longest(n);
  REQUIRE(trunk != nullptr);
  REQUIRE(trunk->mouth);
  FallOptions options;
  options.min_area_m2 = 1e9;  // nothing drawn: the island's case
  FallSet set;
  REQUIRE(cook_falls(p, n, 0, options, nullptr, set, error));
  const u32 index = static_cast<u32>(trunk - n.links.data());
  const Fall* cliff = nullptr;
  for (const Fall& f : set.falls)
    if (f.link == index && f.end + 1 >= trunk->line.size()) cliff = &f;
  REQUIRE(cliff != nullptr);
  MESSAGE("the sea cliff: " << static_cast<u32>(cliff->kind) << " (3 plunge, 4 horsetail), drop "
                            << cliff->drop_mm / 1e3 << " m, landing "
                            << static_cast<u32>(cliff->landing) << ", tail "
                            << cliff->tail_level_mm / 1e3 << " m from the lip; mouth falls "
                            << set.stats.mouth_falls << ", drawn " << set.stats.drawn_falls);
  CHECK(static_cast<FallKind>(cliff->kind) >= FallKind::Plunge);
  CHECK(cliff->drawn == 0);
  CHECK(set.stats.mouth_falls >= 1);
  CHECK(set.stats.drawn_falls == 0);
  // The water comes down to the sea's level: the cliff's 20 m less the bed's thalweg at the top.
  const f64 lip = static_cast<f64>(cliff->y_mm) / 1e3;
  CHECK(lip > 34.5 + 18);
}

// **The bytes** (determinism, a merge fact): the cook is the same bytes at one job and six, the
// file reads back as what was written, its key names the plan and the options, a changed byte is
// refused, and the fall line's bytes hash to a value taken on MSVC that Clang and GCC reproduce
// in the containers. What a fall costs to cook and keep is printed.
namespace {
// A network made by hand, so the pin holds the cook alone and not the planner (which W117 changes
// tonight): three straight links 200 m long in half metres along x, a metre and a fifth wide,
// falling 3 % with two cliffs each (5 + k m at x = 40 + 10 k, 8 + k m at 120), the bed a thalweg
// under the ground, a pool-riffle reach, every cliff a knickpoint.
ChannelNetwork made_by_hand() {
  ChannelNetwork n;
  for (u32 k = 0; k < 3; ++k) {
    ChannelLink link;
    const f64 z = 20.0 + 30.0 * k;
    const f64 c1 = 40.0 + 10.0 * k, h1 = 5.0 + k, c2 = 120, h2 = 8.0 + k;
    for (u32 i = 0; i <= 400; ++i) {
      const f64 x = .5 * i;
      ChannelPoint pt;
      const f64 y = 50 - .03 * x - (x > c1 ? h1 : 0.0) - (x > c2 ? h2 : 0.0);
      pt.at = WorldPos{x, y, z};
      pt.width_m = 1.2f;
      pt.depth_m = .1f;
      pt.slope = .03f;
      pt.area_m2 = 6e4f;
      link.line.push_back(pt);
      link.bed.push_back(y - .12);
      link.reach.push_back(static_cast<u8>(ChannelReach::PoolRiffle));
      const bool near = std::fabs(x - c1) < 3 || std::fabs(x - c2) < 3;
      link.knick.push_back(static_cast<u8>(near ? 1 : 0));
    }
    link.keep = link.line.size();
    link.length_m = 200;
    n.links.push_back(std::move(link));
  }
  return n;
}
}  // namespace

TEST_CASE("falls: the same bytes at one job and six, read back, and pinned") {
  const ChannelLayer p = step_layer(1);
  const ChannelNetwork n = made_by_hand();
  std::string error;
  FallSet serial, parallel;
  const auto began = std::chrono::steady_clock::now();
  REQUIRE(cook_falls(p, n, 0x1234, FallOptions{}, nullptr, serial, error));
  const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - began).count();
  jobs::JobSystemConfig config;
  config.performance_workers = 6;
  config.efficiency_workers = 0;
  config.pin_threads = false;
  jobs::JobSystem js(config);
  REQUIRE(cook_falls(p, n, 0x1234, FallOptions{}, &js, parallel, error));
  const std::string a = falls_bytes(serial), b = falls_bytes(parallel);
  CHECK(a == b);
  FallSet back;
  REQUIRE_MESSAGE(read_falls(a, back, error), error);
  CHECK(falls_bytes(back) == a);
  CHECK(back.plan_key == 0x1234);
  CHECK(back.options == fall_options_hash(FallOptions{}));
  std::string bad = a;
  bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 1);
  CHECK_FALSE(read_falls(bad, back, error));
  CHECK_FALSE(read_falls(a.substr(0, a.size() - 3), back, error));
  FallOptions wrong;
  wrong.columns = 2;
  CHECK_FALSE(cook_falls(p, n, 0, wrong, nullptr, back, error));
  CHECK(fall_options_hash(FallOptions{}) != fall_options_hash(wrong));
  const u64 hash = hash_bytes(a.data(), a.size());
  MESSAGE("falls " << serial.stats.falls << " of " << serial.stats.runs << " runs, "
                   << serial.stats.columns << " columns, " << serial.stats.vertices << " vertices, "
                   << a.size() << " bytes ("
                   << (serial.falls.empty()
                           ? 0.0
                           : static_cast<f64>(a.size()) / static_cast<f64>(serial.falls.size()))
                   << " a run), cooked in " << seconds << " s; hash " << hash);
  CHECK(serial.stats.falls >= 3);
  // Taken on MSVC (msvc-debug, x86-64-v3); the containers reproduce it.
  CHECK(hash == 3190709144914327473ull);
}
