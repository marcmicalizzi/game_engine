// The trees in a storm (roadmap W154; flora.md, "Motion in a storm"; docs/experiments/wind.md,
// "The trees in a storm"). Until W154 the sway's strength was held at 2 (10 m/s), so a gust over a
// gale showed nothing in the trees. These cases hold what replaced it, on the CPU half of the
// flora stage: the lean and the sway follow each kind's streamlining law to 42 m/s, a part turns no
// further than the wind carries it, a forced gust over a gale raises both by the law, a step of the
// live mean reaches the trees with no jump, and the padding the cull adds to a swaying instance's
// spheres holds every vertex in a storm, which is what keeps culling from changing the picture.
#if ENGINE_RENDERER_TESTS_FLORA
#include <domain/flora/flora.h>
#endif
#include <domain/geometry/cluster_file.h>
#include <domain/gfx/flora_motion.h>
#include <domain/gfx/wind.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/scene_wind.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

using geometry::ClusterFileMotionPart;
using geometry::ClusterFileMotionVertex;

ClusterFileMotionPart make_part(Vec3 pivot, Vec3 direction, f32 length, u32 parent) {
  ClusterFileMotionPart p;
  p.pivot = pivot;
  p.direction = normalize(direction);
  p.length = length;
  p.parent = parent;
  return p;
}

// A tree of every class: a 10 m trunk, four limbs from 7 m at quarters of a turn, a twig on each
// limb's tip and a twig on that (the depth the catalogue calls a twig), so a part points every way
// the wind can meet it.
Vector<ClusterFileMotionPart> storm_parts() {
  Vector<ClusterFileMotionPart> parts;
  parts.push_back(make_part(Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, 10.0f, geometry::k_motion_no_parent));
  for (u32 k = 0; k < 4; ++k) {
    const f32 a = 1.5707963f * static_cast<f32>(k);
    const Vec3 out{std::cos(a), 0.6f, std::sin(a)};
    const u32 limb = parts.size();
    parts.push_back(make_part(Vec3{0.0f, 7.0f, 0.0f}, out, 4.0f, 0));
    const Vec3 tip = parts[limb].pivot + parts[limb].direction * 4.0f;
    const u32 twig = parts.size();
    parts.push_back(make_part(tip, Vec3{out.x, 1.2f, out.z}, 1.5f, limb));
    const Vec3 end = parts[twig].pivot + parts[twig].direction * 1.5f;
    parts.push_back(make_part(end, Vec3{-out.z, 0.2f, out.x}, 0.8f, twig));
  }
  return parts;
}

FloraMesh mesh_of(const Vector<ClusterFileMotionPart>& parts, const char* species) {
  FloraMesh mesh;
  REQUIRE(flora_mesh(std::span<const ClusterFileMotionPart>(parts.data(), parts.size()), {},
                     species, mesh));
  return mesh;
}

void pose(const FloraMesh& mesh, const FloraWind& wind, f64 t, Vector<FloraPartState>& state,
          Vector<gfx::FloraPartPose>& poses) {
  flora_part_poses(mesh, wind, t, 11u, FloraPush{}, 1,
                   std::span<FloraPartState>(state.data(), state.size()),
                   std::span<gfx::FloraPartPose>(poses.data(), poses.size()));
}

// A part's own bend over twenty seconds at 60 Hz: its mean (the lean) and half its range (the
// sway's amplitude about it).
struct Sway {
  f64 mean = 0.0;
  f64 amplitude = 0.0;
  f32 most = 0.0f;
};
Sway sway_of(const FloraMesh& mesh, u32 part, Vec3 to, f32 strength) {
  Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
  Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
  constexpr u32 k_samples = 1200;
  f32 lo = 1.0e9f;
  f32 hi = -1.0e9f;
  Sway out;
  for (u32 k = 0; k < k_samples; ++k) {
    pose(mesh, FloraWind{to, strength}, static_cast<f64>(k) / 60.0, state, poses);
    const f32 a = poses[part].pivot_angle.w;
    out.mean += static_cast<f64>(a) / k_samples;
    lo = std::fmin(lo, a);
    hi = std::fmax(hi, a);
  }
  out.amplitude = 0.5 * static_cast<f64>(hi - lo);
  out.most = hi;
  return out;
}

}  // namespace

TEST_CASE("flora storm: the load is the strength to a mean day and the kind's law past it") {
  CHECK(flora_streamline_exponent("oak") == doctest::Approx(1.1f));
  CHECK(flora_streamline_exponent("date_palm") == doctest::Approx(1.1f));
  CHECK(flora_streamline_exponent("spruce") == doctest::Approx(1.32f));
  CHECK(flora_streamline_exponent("hemlock") == doctest::Approx(1.32f));
  CHECK(flora_streamline_exponent("sward") == doctest::Approx(4.0f / 3.0f));
  CHECK(flora_streamline_exponent("no_such_species") == 1.0f);
  for (const f32 b : {1.0f, 1.1f, 1.32f, 4.0f / 3.0f}) {
    CAPTURE(b);
    // To the mean day the model's own linear lean, bit for bit; past it s^b; held at 42 m/s.
    for (const f32 s : {0.0f, 0.25f, 0.5f, 1.0f})
      CHECK(flora_storm_load(s, b) == s);
    for (const f32 s : {1.5f, 2.0f, 4.0f, 6.6f, 8.4f})
      CHECK(flora_storm_load(s, b) == doctest::Approx(std::pow(s, b)).epsilon(1e-6));
    CHECK(flora_storm_load(20.0f, b) == flora_storm_load(k_flora_max_wind, b));
    CHECK(flora_storm_load(-1.0f, b) == 0.0f);
    // Continuous at the mean day and rising everywhere: no step of the wind is a jump.
    f32 last = 0.0f;
    for (u32 i = 1; i <= 900; ++i) {
      const f32 s = 0.01f * static_cast<f32>(i);
      const f32 load = flora_storm_load(s, b);
      CHECK(load >= last);
      CHECK(load - last < 0.04f);  // its slope is at most b s^(b - 1), under 2.4 for s to 9
      last = load;
    }
  }
  // The blades' load: the fibre's law past a mean day, held short of 3 (a blade laid over).
  f32 last = 0.0f;
  for (u32 i = 0; i <= 900; ++i) {
    const f32 s = 0.01f * static_cast<f32>(i);
    const f32 load = flora_field_load(s);
    CHECK(load >= last);
    CHECK(load <= k_flora_field_most_load);
    if (s <= 1.0f) CHECK(load == s);
    last = load;
  }
  MESSAGE("the blades' load at 10, 20, 33 and 42 m/s: "
          << flora_field_load(2.0f) << ", " << flora_field_load(4.0f) << ", "
          << flora_field_load(6.6f) << ", " << flora_field_load(8.4f));
}

TEST_CASE("flora storm: lean and sway grow on each kind's law to 42 m/s, past the old 10 m/s") {
  const Vector<ClusterFileMotionPart> parts = storm_parts();
  const Vec3 to{1.0f, 0.0f, 0.0f};
  for (const char* species : {"oak", "spruce", "date_palm"}) {
    CAPTURE(std::string(species));
    const FloraMesh mesh = mesh_of(parts, species);
    const Sway day = sway_of(mesh, 0, to, 1.0f);
    REQUIRE(day.mean > 0.0);
    Sway last{};
    std::string table;
    for (const f32 u : {5.0f, 10.0f, 15.0f, 20.0f, 25.0f, 30.0f, 33.0f, 36.0f, 42.0f}) {
      const f32 s = u / static_cast<f32>(k_flora_wind_mean_m_s);
      const Sway now = sway_of(mesh, 0, to, s);
      // The trunk is never near the cap (an upright part, a level wind: 90 degrees of room), so
      // its bend is its compliance times the load exactly, lean and sway alike.
      const f64 law = static_cast<f64>(flora_storm_load(s, mesh.streamline));
      CHECK(now.mean / day.mean == doctest::Approx(law).epsilon(1e-4));
      CHECK(now.amplitude / day.amplitude == doctest::Approx(law).epsilon(1e-3));
      CHECK(now.mean > last.mean);
      CHECK(now.amplitude > last.amplitude);
      table += " " + std::to_string(static_cast<int>(u)) + " m/s: " + std::to_string(now.mean) +
               " +- " + std::to_string(now.amplitude) + ";";
      last = now;
    }
    // Past the old saturation: 33 m/s bends the trunk (6.6 / 2)^b times as far as 10 m/s, where
    // the old model held every wind over 10 m/s at 10 m/s's bend.
    const Sway ten = sway_of(mesh, 0, to, 2.0f);
    const Sway gale = sway_of(mesh, 0, to, 6.6f);
    CHECK(gale.mean / ten.mean ==
          doctest::Approx(std::pow(3.3, static_cast<f64>(mesh.streamline))).epsilon(1e-3));
    // A conifer's trunk at the 42 m/s its kind breaks near stays under the quarter radian a
    // stem's strain at rupture allows it (flora.md, "Motion in a storm").
    CHECK(last.most < 0.3f);
    MESSAGE(std::string(species) << ": the trunk's bend, radians, mean +- half its range:"
                                 << table);
  }
}

TEST_CASE("flora storm: a part turns no further than the wind carries it, and continuously") {
  // Three twigs at 30, 90 and 170 degrees from where the wind blows to.
  Vector<ClusterFileMotionPart> parts;
  parts.push_back(make_part(Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, 4.0f, geometry::k_motion_no_parent));
  parts.push_back(make_part(Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 0));
  const f32 degrees[3] = {30.0f, 90.0f, 170.0f};
  for (const f32 d : degrees) {
    const f32 r = d * 0.0174532925f;
    parts.push_back(
        make_part(Vec3{0.0f, 5.0f, 0.0f}, Vec3{std::cos(r), std::sin(r), 0.0f}, 1.0f, 1));
  }
  const FloraMesh mesh = mesh_of(parts, "oak");
  const Vec3 to{1.0f, 0.0f, 0.0f};
  Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
  Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
  f32 most_seen[3] = {};
  for (u32 k = 0; k < 12; ++k) {
    const f64 t = 0.83 * k;  // the clock held at each, so only the wind moves a part
    f32 last[3] = {};
    for (u32 i = 0; i <= 1200; ++i) {
      const f32 s = 0.01f * static_cast<f32>(i);  // to 60 m/s: past the hold too
      pose(mesh, FloraWind{to, s}, t, state, poses);
      for (u32 j = 0; j < 3; ++j) {
        const f32 a = poses[2 + j].pivot_angle.w;
        const f32 phi = degrees[j] * 0.0174532925f;
        const f32 most = phi > k_flora_bend_floor ? phi : k_flora_bend_floor;
        CHECK(a <= most);
        CHECK(a >= last[j]);  // a stronger wind never turns a part back
        CHECK(a - last[j] < 0.01f);
        last[j] = a;
        most_seen[j] = std::fmax(most_seen[j], a);
      }
    }
  }
  MESSAGE("twigs 30, 90 and 170 degrees off the wind, at their most: "
          << most_seen[0] << ", " << most_seen[1] << ", " << most_seen[2] << " rad");
  // The one near the wind is held at the floor's 45 degrees; the one across it bends far; the one
  // pointing into the wind is turned past the perpendicular, streaming.
  CHECK(most_seen[0] <= k_flora_bend_floor);
  CHECK(most_seen[1] > 1.0f);
  CHECK(most_seen[2] > most_seen[1]);
}

TEST_CASE("flora storm: a gust held over a gale raises lean and sway by the law") {
  // W151's live wind: a 20 m/s gale over grassland, the forced gust off and held at its top.
  gfx::WindDesc scene;
  scene.kind = gfx::WindKind::fixed;
  scene.speed_m_s = 20.0;
  scene.roughness_m = 0.03;
  gfx::WindLive gale;
  gale.set = true;
  gale.mean_m_s = 20.0;
  gfx::WindLive gust = gale;
  gust.gust = 1.0;
  Vec2 to2{};
  f32 calm_strength = 0.0f;
  f32 gust_strength = 0.0f;
  REQUIRE(scene_flora_wind(scene, 500.0, to2, calm_strength, gale));
  REQUIRE(scene_flora_wind(scene, 500.0, to2, gust_strength, gust));
  const f64 g = gfx::wind_gust_factor(0.03);
  CHECK(static_cast<f64>(gust_strength - calm_strength) ==
        doctest::Approx(20.0 * (g - 1.0) / k_flora_wind_mean_m_s).epsilon(1e-5));
  REQUIRE(calm_strength > 2.0f);  // a gale: where the old sway was already held
  const Vec3 to{to2.x, 0.0f, to2.y};
  for (const char* species : {"oak", "spruce"}) {
    CAPTURE(std::string(species));
    const FloraMesh mesh = mesh_of(storm_parts(), species);
    const Sway a = sway_of(mesh, 0, to, calm_strength);
    const Sway b = sway_of(mesh, 0, to, gust_strength);
    const f64 law = static_cast<f64>(flora_storm_load(gust_strength, mesh.streamline)) /
                    static_cast<f64>(flora_storm_load(calm_strength, mesh.streamline));
    MESSAGE(std::string(species) << ": the gale " << calm_strength * 5.0f << " m/s, with the gust "
                                 << gust_strength * 5.0f << " m/s: the trunk's lean " << a.mean
                                 << " -> " << b.mean << " rad, x" << b.mean / a.mean
                                 << " (the law: x" << law << ")");
    CHECK(law > 1.3);
    CHECK(b.mean / a.mean == doctest::Approx(law).epsilon(1e-4));
    CHECK(b.amplitude / a.amplitude == doctest::Approx(law).epsilon(1e-3));
  }
}

TEST_CASE("flora storm: a step of the live mean reaches the trees with no jump") {
  // A key's step, force 7 to force 8 (15.5 to 18.9 m/s), at 60 frames a second and at 30.
  gfx::WindDesc scene;
  scene.kind = gfx::WindKind::fixed;
  scene.speed_m_s = 15.5;
  scene.roughness_m = 0.5;
  const FloraMesh mesh = mesh_of(storm_parts(), "oak");
  for (const u32 hz : {60u, 30u}) {
    CAPTURE(hz);
    FloraWindLag lag;
    gfx::WindLive live;
    live.set = true;
    live.mean_m_s = 15.5;
    Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
    Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
    f64 last_mean = 0.0;
    f32 last_angle = 0.0f;
    f32 worst_step = 0.0f;
    f64 at_four = 0.0;
    const u32 frames = 16 * hz;
    f32 before = 0.0f;
    f32 after = 0.0f;
    for (u32 f = 0; f <= frames; ++f) {
      const f64 t = static_cast<f64>(f) / hz;
      if (f == 2 * hz) live.mean_m_s = 18.9;  // the key, two seconds in
      const gfx::WindLive trees = lag.follow(live, t);
      if (f > 0) {
        CHECK(trees.mean_m_s >= last_mean - 1e-12);  // no overshoot, no turning back
        CHECK(trees.mean_m_s <= 18.9 + 1e-12);
        // its fastest: w e0 / e = 0.69 of the step a second, at t = 1 / w
        CHECK(trees.mean_m_s - last_mean < 0.75 * 3.4 / hz);
      }
      if (f == 6 * hz) at_four = trees.mean_m_s;
      last_mean = trees.mean_m_s;
      // The trunk's lean through it, the clock held so only the wind moves it.
      Vec2 to2{};
      f32 strength = 0.0f;
      REQUIRE(scene_flora_wind(scene, 900.0, to2, strength, trees));
      pose(mesh, FloraWind{Vec3{to2.x, 0.0f, to2.y}, strength}, 4.0, state, poses);
      const f32 angle = poses[0].pivot_angle.w;
      if (f > 0) worst_step = std::fmax(worst_step, std::fabs(angle - last_angle));
      if (f == 2 * hz - 1) before = angle;
      after = angle;
      last_angle = angle;
    }
    // Within a part in two hundred of the new mean four seconds after the step (critically damped
    // at 0.3 Hz: (1 + w t) exp(-w t) = 0.0045 at 4 s), and there by the end.
    CHECK(std::fabs(at_four - 18.9) < 0.005 * 3.4);
    CHECK(last_mean == doctest::Approx(18.9).epsilon(1e-6));
    const f32 change = after - before;
    MESSAGE(hz << " Hz: the trunk's lean " << before << " -> " << after
               << " rad; the largest change "
               << "a frame " << worst_step << " (" << 100.0f * worst_step / change
               << "% of the step's)");
    CHECK(change > 0.0f);
    CHECK(worst_step < 0.05f * change * (60.0f / static_cast<f32>(hz)));
  }
  // Exactly stepped: the lag does not depend on how the seconds were cut.
  FloraWindLag a;
  FloraWindLag b;
  gfx::WindLive live;
  live.set = true;
  live.mean_m_s = 4.0;
  (void)a.follow(live, 0.0);
  (void)b.follow(live, 0.0);
  live.mean_m_s = 30.0;
  for (u32 f = 1; f <= 120; ++f)
    (void)a.follow(live, f / 60.0);
  for (u32 f = 1; f <= 7; ++f)
    (void)b.follow(live, f * (2.0 / 7.0));
  CHECK(a.mean_m_s == doctest::Approx(b.mean_m_s).epsilon(1e-9));
  CHECK(a.rate == doctest::Approx(b.rate).epsilon(1e-9));
  // Unset, it passes the live wind as it is and forgets; a jump of the clock stands it at the mean.
  CHECK(a.follow(gfx::WindLive{}, 3.0).set == false);
  CHECK(!a.set);
  live.mean_m_s = 12.0;
  (void)a.follow(live, 3.0);
  CHECK(a.follow(live, 100.0).mean_m_s == 12.0);
  live.mean_m_s = 25.0;
  CHECK(a.follow(live, 50.0).mean_m_s == 25.0);  // the clock went back: no lag across it
}

namespace {

// Every vertex of a mesh moved by a pose, against the padding.
void check_padding(const FloraMesh& mesh, std::span<const Vec3> vertices,
                   std::span<const ClusterFileMotionVertex> motion, f32& worst_ratio) {
  const f32 padding = flora_bounds_padding(mesh, vertices, motion);
  REQUIRE(padding > 0.0f);
  Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
  Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
  for (u32 direction = 0; direction < 8; ++direction) {
    const f32 a = 0.785398163f * static_cast<f32>(direction);
    for (const f32 s : {1.0f, 2.0f, 4.0f, 6.96f, k_flora_max_wind, 20.0f}) {
      for (u32 k = 0; k < 16; ++k) {
        pose(mesh, FloraWind{Vec3{std::cos(a), 0.0f, std::sin(a)}, s}, 0.61 * k, state, poses);
        for (u32 v = 0; v < vertices.size(); ++v) {
          Vec3 p;
          Vec3 n;
          gfx::flora_bend(poses[motion[v].part], static_cast<f32>(motion[v].weight) / 65535.0f,
                          vertices[v], Vec3{0.0f, 1.0f, 0.0f}, p, n);
          const f32 moved = length(p - vertices[v]);
          worst_ratio = std::fmax(worst_ratio, moved / padding);
        }
      }
    }
  }
}

}  // namespace

TEST_CASE("flora storm: the padding holds every vertex at force 12 and past it") {
  // A vertex every tenth of every part of the hand-built tree, at its bend weight.
  const Vector<ClusterFileMotionPart> parts = storm_parts();
  Vector<Vec3> vertices;
  Vector<ClusterFileMotionVertex> motion;
  for (u32 p = 0; p < parts.size(); ++p) {
    for (u32 i = 0; i <= 10; ++i) {
      const f32 w = 0.1f * static_cast<f32>(i);
      vertices.push_back(parts[p].pivot + parts[p].direction * (parts[p].length * w) +
                         Vec3{0.05f, 0.0f, -0.03f});
      motion.push_back(
          ClusterFileMotionVertex{static_cast<u16>(p), static_cast<u16>(w * 65535.0f)});
    }
  }
  for (const char* species : {"oak", "spruce", "date_palm", "fern"}) {
    CAPTURE(std::string(species));
    const FloraMesh mesh = mesh_of(parts, species);
    f32 worst = 0.0f;
    check_padding(mesh, {vertices.data(), vertices.size()}, {motion.data(), motion.size()}, worst);
    const f32 storm = flora_bounds_padding(mesh, {vertices.data(), vertices.size()},
                                           {motion.data(), motion.size()});
    const f32 old = flora_bounds_padding(mesh, {vertices.data(), vertices.size()},
                                         {motion.data(), motion.size()}, 2.0f);
    MESSAGE(std::string(species) << ": padding " << storm << " m (" << old
                                 << " m at the old 10 m/s); the "
                                 << "farthest vertex " << worst << " of it");
    CHECK(worst <= 1.0f);
    CHECK(worst > 0.2f);  // a bound the storm comes near, not one nothing reaches
  }
#if ENGINE_RENDERER_TESTS_FLORA
  // Generated trees of the three kinds, every third vertex.
  for (const char* species : {"oak", "spruce", "date_palm"}) {
    CAPTURE(std::string(species));
    flora::FloraFile file;
    file.species = species;
    file.tree.seed = file.palm.seed = 5;
    file.tree.triangle_budget = 8000;
    file.palm.triangle_budget = 9000;
    file.palm.height_m = 3.5f;
    file.tree.card_bake_px = file.tree.impostor_px = 0;
    assets::MeshData generated;
    flora::Motion solved;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_flora(file, generated, solved, nullptr, nullptr, error), error);
    FloraMesh mesh;
    REQUIRE(flora_mesh({solved.parts.data(), solved.parts.size()},
                       {solved.structure.data(), solved.structure.size()}, species, mesh));
    Vector<Vec3> at;
    Vector<ClusterFileMotionVertex> records;
    for (u32 v = 0; v < generated.positions.size(); v += 3) {
      at.push_back(generated.positions[v]);
      records.push_back(ClusterFileMotionVertex{
          static_cast<u16>(solved.vertex_part[v]),
          static_cast<u16>(flora::encode_bend_weight(solved.vertex_weight[v]))});
    }
    f32 worst = 0.0f;
    check_padding(mesh, {at.data(), at.size()}, {records.data(), records.size()}, worst);
    MESSAGE(std::string(species) << " (generated, " << at.size() << " vertices): the farthest "
                                 << worst << " of the padding");
    CHECK(worst <= 1.0f);
  }
#endif
}

// ---- the padding follows the wind (W154-2; flora_motion.h; renderer.md, "What a storm costs") --

namespace {

// Main's `flora_bounds_padding` before W154, as it was written: the strength held at 2, linear.
f32 main_padding(const FloraMesh& mesh, std::span<const Vec3> vertices,
                 std::span<const ClusterFileMotionVertex> motion) {
  const u32 n = mesh.part_count();
  Vector<f32> most(n, 0.0f);
  constexpr f32 k_main_max_wind = 2.0f;
  for (u32 p = 0; p < n; ++p)
    most[p] = mesh.compliance[p] * k_main_max_wind * (0.6f + 0.8f + 0.25f + 0.35f);
  f32 worst = 0.0f;
  for (u32 v = 0; v < vertices.size(); ++v) {
    u32 p = motion[v].part;
    const Vec3 at = vertices[v];
    f32 bound = most[p] * (static_cast<f32>(motion[v].weight) * (1.0f / 65535.0f)) *
                length(at - mesh.parts[p].pivot);
    for (u32 guard = 0; guard < 64 && mesh.parts[p].parent != geometry::k_motion_no_parent;
         ++guard) {
      p = mesh.parts[p].parent;
      bound += most[p] * length(at - mesh.parts[p].pivot);
    }
    worst = bound > worst ? bound : worst;
  }
  return worst;
}

}  // namespace

TEST_CASE("flora storm: the ladder's lowest rung is main's padding, and each rung holds its sway") {
  const Vector<ClusterFileMotionPart> parts = storm_parts();
  Vector<Vec3> vertices;
  Vector<ClusterFileMotionVertex> motion;
  for (u32 p = 0; p < parts.size(); ++p) {
    for (u32 i = 0; i <= 10; ++i) {
      const f32 w = 0.1f * static_cast<f32>(i);
      vertices.push_back(parts[p].pivot + parts[p].direction * (parts[p].length * w));
      motion.push_back(
          ClusterFileMotionVertex{static_cast<u16>(p), static_cast<u16>(w * 65535.0f)});
    }
  }
  const std::span<const Vec3> at{vertices.data(), vertices.size()};
  const std::span<const ClusterFileMotionVertex> records{motion.data(), motion.size()};
  for (const char* species : {"oak", "spruce", "date_palm"}) {
    CAPTURE(std::string(species));
    const FloraMesh mesh = mesh_of(parts, species);
    const f32 calm = flora_padding_at_load(mesh, at, records, k_flora_calm_load);
    // Rung 0 is main's padding to the bit: a calm session's spheres are main's.
    CHECK(calm == main_padding(mesh, at, records));
    CHECK(flora_rung_padding(calm, false, k_flora_calm_load) == calm);
    const f32 top = flora_storm_load(k_flora_max_wind, mesh.streamline);
    const u32 rungs = flora_rung_count(top);
    CHECK(rungs >= 5);
    CHECK(flora_rung_load(rungs - 1, top) == top);
    Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
    Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
    f32 prev = 0.0f;
    for (u32 r = 0; r < rungs; ++r) {
      const f32 load = flora_rung_load(r, top);
      CHECK(load > prev);
      prev = load;
      const f32 padding = flora_rung_padding(calm, false, load);
      // The bound at the load, measured, sits under the rung's padding.
      CHECK(flora_padding_at_load(mesh, at, records, load) <= padding);
      // Every pose the hold allows stays inside it, whatever the wind: the sway is held there.
      f32 worst = 0.0f;
      for (u32 d = 0; d < 8; ++d) {
        const f32 a = 0.785398163f * static_cast<f32>(d);
        for (const f32 s : {1.0f, 2.0f, 4.0f, 8.4f, 20.0f}) {
          for (u32 k = 0; k < 12; ++k) {
            pose(mesh, FloraWind{Vec3{std::cos(a), 0.0f, std::sin(a)}, s, load}, 0.53 * k, state,
                 poses);
            for (u32 v = 0; v < vertices.size(); ++v) {
              Vec3 p;
              Vec3 n;
              gfx::flora_bend(poses[motion[v].part], static_cast<f32>(motion[v].weight) / 65535.0f,
                              vertices[v], Vec3{0.0f, 1.0f, 0.0f}, p, n);
              worst = std::fmax(worst, length(p - vertices[v]) / padding);
            }
          }
        }
      }
      MESSAGE(std::string(species) << " rung " << r << ": load " << load << ", padding " << padding
                                   << " m, the farthest vertex " << worst << " of it");
      CHECK(worst <= 1.0f);
    }
  }
  // A field's padding stops at its blade's most load.
  CHECK(flora_rung_padding(1.0f, true, 10.0f) == doctest::Approx(1.5f).epsilon(1e-5));
  CHECK(flora_rung_padding(1.0f, false, 10.0f) == doctest::Approx(5.0f).epsilon(1e-5));
}

TEST_CASE("flora storm: the rung follows the wind's ceiling, up at once and down after a hold") {
  gfx::WindDesc forest;
  forest.kind = gfx::WindKind::fixed;
  forest.roughness_m = 0.5;  // a forest: the gust factor 1.89
  const auto demand_at = [&](f64 mean_m_s, f64 gust, f32 exponent) {
    gfx::WindLive live;
    live.set = true;
    live.mean_m_s = mean_m_s;
    live.gust = gust;
    f32 ceiling = 0.0f;
    REQUIRE(scene_flora_wind_ceiling(forest, 0.0, live, live, ceiling));
    return flora_storm_load(ceiling, exponent);
  };
  const f32 top = flora_storm_load(k_flora_max_wind, 1.32f);
  // A mean day over a forest, a conifer's law: its three-second gust is a load past main's 2 and
  // inside rung 0's slack, so a mean day pays no rung.
  const f32 mean_day = demand_at(5.0, 0.0, 1.32f);
  MESSAGE("a mean day's ceiling over a forest, a conifer's load: " << mean_day);
  CHECK(mean_day > k_flora_calm_load);
  CHECK(mean_day <= k_flora_calm_load * k_flora_calm_slack);
  FloraLadder ladder;
  CHECK(flora_rung_choose(ladder, mean_day, 0.0, top) == 0u);
  // A gale (force 8, 18.9 m/s) and force 12 (34.8 m/s) step up at once, and a forced gust over the
  // gale asks for more before it has risen.
  const f32 gale = demand_at(18.9, 0.0, 1.32f);
  const u32 gale_rung = flora_rung_choose(ladder, gale, 1.0, top);
  CHECK(gale_rung >= 2u);
  CHECK(flora_rung_load(gale_rung, top) >= gale);
  CHECK(demand_at(18.9, 0.01, 1.32f) > gale);
  const f32 force12 = demand_at(34.8, 0.0, 1.32f);
  const u32 storm_rung = flora_rung_choose(ladder, force12, 2.0, top);
  CHECK(storm_rung > gale_rung);
  CHECK(storm_rung == flora_rung_count(top) - 1);  // past 42 m/s at a gust: the top
  // A wind that gusts back to the gale for a moment does not step down; one that stays down
  // steps down one rung per hold.
  CHECK(flora_rung_choose(ladder, gale, 3.0, top) == storm_rung);
  CHECK(flora_rung_choose(ladder, force12, 4.0, top) == storm_rung);
  CHECK(flora_rung_choose(ladder, gale, 5.0, top) == storm_rung);
  CHECK(flora_rung_choose(ladder, gale, 7.9, top) == storm_rung);
  CHECK(flora_rung_choose(ladder, gale, 8.0, top) == storm_rung - 1);
  u32 rung = storm_rung - 1;
  f64 t = 8.0;
  while (rung > gale_rung) {
    t += k_flora_rung_down_s;
    rung = flora_rung_choose(ladder, gale, t, top);
  }
  CHECK(rung == gale_rung);
  CHECK(flora_rung_choose(ladder, gale, t + 100.0, top) == gale_rung);  // it holds the gale's
  // And calm again: down to rung 0, a rung a hold.
  for (u32 k = 0; k < 16; ++k)
    t += k_flora_rung_down_s, rung = flora_rung_choose(ladder, 0.0f, t, top);
  CHECK(rung == 0u);
}

TEST_CASE("flora storm: a still form's lean is its mesh's mean lean, and its spheres hold it") {
  const Vector<ClusterFileMotionPart> parts = storm_parts();
  Vector<Vec3> vertices;
  Vector<ClusterFileMotionVertex> motion;
  for (u32 p = 0; p < parts.size(); ++p) {
    for (u32 i = 0; i <= 10; ++i) {
      const f32 w = 0.1f * static_cast<f32>(i);
      vertices.push_back(parts[p].pivot + parts[p].direction * (parts[p].length * w));
      motion.push_back(
          ClusterFileMotionVertex{static_cast<u16>(p), static_cast<u16>(w * 65535.0f)});
    }
  }
  for (const char* species : {"oak", "spruce", "date_palm"}) {
    CAPTURE(std::string(species));
    const FloraMesh mesh = mesh_of(parts, species);
    const f32 lean =
        flora_mean_lean(mesh, {vertices.data(), vertices.size()}, {motion.data(), motion.size()});
    // The posed mean: the vertices' displacement along a wind from -x over twenty seconds at a
    // unit load, fit the same way (every fourth vertex above the origin, dx = lean y).
    Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
    Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
    f64 num = 0.0;
    f64 den = 0.0;
    for (u32 k = 0; k < 600; ++k) {
      pose(mesh, FloraWind{Vec3{1.0f, 0.0f, 0.0f}, 1.0f}, static_cast<f64>(k) / 30.0, state, poses);
      for (u32 v = 0; v < vertices.size(); v += 4) {
        if (!(vertices[v].y > 0.0f)) continue;
        Vec3 p;
        Vec3 n;
        gfx::flora_bend(poses[motion[v].part], static_cast<f32>(motion[v].weight) / 65535.0f,
                        vertices[v], Vec3{0.0f, 1.0f, 0.0f}, p, n);
        num += static_cast<f64>(p.x - vertices[v].x) * static_cast<f64>(vertices[v].y);
        den += static_cast<f64>(vertices[v].y) * static_cast<f64>(vertices[v].y);
      }
    }
    const f64 posed = num / den;
    MESSAGE(std::string(species) << ": a still form leans " << lean
                                 << " m a metre at a unit load; the posed mean " << posed);
    CHECK(lean > 0.0f);
    CHECK(static_cast<f64>(lean) == doctest::Approx(posed).epsilon(0.35));
  }
  // The record: no lean is the rest, to the bit; a lean shears about the origin, and every point
  // of a sphere the rest record carries lies inside the sphere the leaned record's cull tests.
  gfx::InstanceDesc rest{};
  Mat4 world = Mat4::identity();
  world.c[0] = Vec4{0.0f, 0.0f, -1.3f, 0.0f};  // turned a quarter and scaled 1.3
  world.c[1] = Vec4{0.0f, 1.3f, 0.0f, 0.0f};
  world.c[2] = Vec4{1.3f, 0.0f, 0.0f, 0.0f};
  world.c[3] = Vec4{7.0f, 2.0f, 9.0f, 1.0f};
  gfx::set_instance_transform(rest, world);
  rest.scale_max = 1.3f;
  const gfx::InstanceDesc unleaned = still_lean_record(rest, Vec2{0.0f, 0.0f});
  CHECK(std::memcmp(&rest, &unleaned, sizeof(rest)) == 0);
  const Vec2 lean{0.21f, -0.08f};
  const gfx::InstanceDesc leaned = still_lean_record(rest, lean);
  CHECK((leaned.flags & gfx::k_instance_uniform_scale) == 0u);
  const auto apply = [](const gfx::InstanceDesc& d, Vec3 p) {
    return Vec3{dot(d.rows[0].xyz(), p) + d.rows[0].w, dot(d.rows[1].xyz(), p) + d.rows[1].w,
                dot(d.rows[2].xyz(), p) + d.rows[2].w};
  };
  const Vec3 top{0.0f, 10.0f, 0.0f};
  const Vec3 rest_top = apply(rest, top);
  const Vec3 lean_top = apply(leaned, top);
  CHECK(lean_top.x - rest_top.x == doctest::Approx(lean.x * 13.0f));
  CHECK(lean_top.z - rest_top.z == doctest::Approx(lean.y * 13.0f));
  CHECK(lean_top.y == doctest::Approx(rest_top.y));
  CHECK(apply(leaned, Vec3{}).x == apply(rest, Vec3{}).x);  // the base stays
  const Vec3 centre{0.4f, 6.0f, -0.7f};
  const f32 radius = 2.5f;
  f32 worst = 0.0f;
  for (u32 i = 0; i < 64; ++i) {
    const f32 a = 0.37f * static_cast<f32>(i);
    const f32 b = 0.11f * static_cast<f32>(i);
    const Vec3 dir{std::cos(a) * std::cos(b), std::sin(b), std::sin(a) * std::cos(b)};
    const Vec3 at = apply(leaned, centre + normalize(dir) * radius);
    worst = std::fmax(worst, length(at - apply(leaned, centre)) / (radius * leaned.scale_max));
  }
  CHECK(worst <= 1.0f);
}
