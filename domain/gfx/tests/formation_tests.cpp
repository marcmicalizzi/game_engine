// The world formation (W85, W94; domain/gfx/formation.h, shaders/formation.slang): the schema's
// ranges, the twin's periodicity and fold, an authored sequence against W85's own arithmetic, the
// drawn sequence (no repeat, the stated distribution, the members' shares), the variation along
// strike (continuous, in range, the filter exact with the tops moved), the fault, what the solvers
// read, the drawn table's bytes pinned, and the shader held to the twin over a lattice of points
// from the origin to 10,000 km out.
#include <core/math/math.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/formation.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <shaders/formation_probe.spv.h>
#include <string>

using namespace engine;

namespace {

scene::FormationBed bed(const char* name, f32 thickness, Vec3 tint, f32 roughness, f32 resistance) {
  scene::FormationBed b;
  b.name = name;
  b.thickness_m = thickness;
  b.tint = tint;
  b.roughness = roughness;
  b.resistance = resistance;
  return b;
}

// Four beds of a sandstone-shale alternation (thicknesses drawn from a lognormal about 0.9 m, the
// grounding page's numbers) and three members at tens of metres: W85's authored fixture.
scene::Formation fixture() {
  scene::Formation f;
  f.origin = WorldPos{123.4567, -20.0, -987.6543};
  f.dip_deg = 30.0f;
  f.azimuth_deg = 35.0f;
  f.fold_amplitude_m = 12.0f;
  f.fold_wavelength_m = 180.0f;
  f.fold2_amplitude_m = 40.0f;
  f.fold2_wavelength_m = 2400.0f;
  f.soft_m = 0.1f;
  f.beds.push_back(bed("sandstone", 1.42f, Vec3{1.10f, 1.02f, 0.90f}, -0.05f, 0.85f));
  f.beds.push_back(bed("shale", 0.37f, Vec3{0.70f, 0.72f, 0.78f}, 0.10f, 0.15f));
  f.beds.push_back(bed("siltstone", 0.81f, Vec3{0.92f, 0.90f, 0.88f}, 0.02f, 0.45f));
  f.beds.push_back(bed("limestone", 2.23f, Vec3{1.25f, 1.22f, 1.18f}, -0.08f, 0.95f));
  f.macro_beds.push_back(bed("lower", 34.0f, Vec3{1.0f, 0.95f, 0.9f}, 0.0f, 0.6f));
  f.macro_beds.push_back(bed("middle", 52.5f, Vec3{0.85f, 0.88f, 0.92f}, 0.03f, 0.3f));
  f.macro_beds.push_back(bed("upper", 41.0f, Vec3{1.1f, 1.08f, 1.0f}, -0.02f, 0.7f));
  return f;
}

scene::FormationMember member(const char* name, f32 thickness, f32 resistant, f32 bed_m) {
  scene::FormationMember m;
  m.name = name;
  m.thickness_m = thickness;
  m.resistant = resistant;
  m.bed_m = bed_m;
  return m;
}

// A drawn section of six members, 264 m: two cliff-formers over slope-formers, a ledgy unit, a
// massive wall with no partings and a shale with none of the other, the tops moving (the members'
// defaults), a fault, and W85's folds.
scene::Formation drawn(u64 seed) {
  scene::Formation f;
  f.origin = WorldPos{123.4567, -20.0, -987.6543};
  f.dip_deg = 12.0f;
  f.azimuth_deg = 35.0f;
  f.fold_amplitude_m = 12.0f;
  f.fold_wavelength_m = 180.0f;
  f.fold2_amplitude_m = 40.0f;
  f.fold2_wavelength_m = 2400.0f;
  f.soft_m = 0.1f;
  f.seed = seed;
  f.members.push_back(member("cap", 38.0f, 0.92f, 2.2f));
  f.members.push_back(member("slope", 55.0f, 0.15f, 3.0f));
  f.members.push_back(member("ledges", 24.0f, 0.55f, 0.8f));
  f.members.push_back(member("wall", 70.0f, 1.0f, 4.0f));
  f.members.push_back(member("shale", 46.0f, 0.05f, 2.5f));
  f.members.push_back(member("upper", 31.0f, 0.8f, 1.2f));
  f.members[0].tint = Vec3{1.05f, 1.0f, 0.92f};
  f.members[1].tint = Vec3{0.9f, 0.86f, 0.84f};
  f.fault_throw_m = 3.25f;
  f.fault_azimuth_deg = 20.0f;
  f.fault_offset_m = 17.5;
  return f;
}

f64 wrap(f64 a, f64 period) {
  const f64 r = a - period * std::floor(a / period);
  return r > 0.5 * period ? r - period : r;
}

// A block is 98 KB: on the heap in a test that holds several.
std::unique_ptr<gfx::FormationParams> packed(const scene::Formation& f) {
  auto p = std::make_unique<gfx::FormationParams>();
  std::string error;
  REQUIRE_MESSAGE(gfx::pack_formation(f, *p, &error), error);
  return p;
}

u64 thickness_mm(const gfx::FormationBedGpu& b) {
  return static_cast<u64>(std::llround(static_cast<f64>(b.thickness_m) * 1000.0));
}

// The reference section's value at a coordinate inside the period, straight from the beds: what
// the filter is checked against.
f64 channel(const Vec4& v, u32 c) {
  return static_cast<f64>(c == 0 ? v.x : (c == 1 ? v.y : (c == 2 ? v.z : v.w)));
}

u64 fnv1a(const void* data, usize size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  u64 h = 0xcbf29ce484222325ull;
  for (usize i = 0; i < size; ++i) {
    h ^= bytes[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

}  // namespace

TEST_CASE("formation: the schema's ranges are refused with a sentence") {
  std::string error;
  auto block = std::make_unique<gfx::FormationParams>();
  gfx::FormationParams& p = *block;
  CHECK(gfx::pack_formation(fixture(), p, &error));
  CHECK(p.flags == gfx::k_formation_valid);
  CHECK(p.beds.period_mm == 1420u + 370u + 810u + 2230u);
  const auto refuse = [&](scene::Formation f, const char* fragment) {
    error.clear();
    CHECK_FALSE(gfx::pack_formation(f, p, &error));
    CHECK_MESSAGE(error.find(fragment) != std::string::npos, error);
    CHECK(error.rfind("formation: ", 0) == 0);
  };
  scene::Formation f = fixture();
  f.dip_deg = 86.0f;
  refuse(f, "dip_deg");
  f = fixture();
  f.dip_deg = -90.0f;
  refuse(f, "dip_deg");
  f = fixture();
  f.beds.clear();
  refuse(f, "at least one bed");
  f = fixture();
  f.beds[1].thickness_m = 0.0f;
  refuse(f, "thickness_m");
  f = fixture();
  f.macro_beds[0].thickness_m = -3.0f;
  refuse(f, "thickness_m");
  f = fixture();
  f.soft_m = 11.0f;
  refuse(f, "soft_m");
  f = fixture();
  f.soft_m = -0.1f;
  refuse(f, "soft_m");
  f = fixture();
  f.fold_wavelength_m = 0.5f;
  refuse(f, "wavelength");
  f = fixture();
  f.beds[0].tint = Vec3{2.5f, 1.0f, 1.0f};
  refuse(f, "tint");
  // W94: a table holds 512 beds, where W85's held eight.
  f = fixture();
  const scene::FormationBed extra = f.beds[0];
  for (u32 i = 0; i < 508; ++i)
    f.beds.push_back(extra);
  CHECK(gfx::pack_formation(f, p, &error));
  CHECK(p.beds.count == 512u);
  f.beds.push_back(extra);
  refuse(f, "at most 512");
  // The variation's ranges, and a lens's two rules.
  f = fixture();
  f.beds[0].variation = 0.5f;
  refuse(f, "variation");
  f = fixture();
  f.beds[3].lens = true;
  refuse(f, "lens");
  f = fixture();
  f.beds[1].lens = true;
  f.beds[2].lens = true;
  refuse(f, "lens");
  f = fixture();
  f.lateral_m = 5.0f;
  refuse(f, "lateral_m");
  f = fixture();
  f.fault_throw_m = 1200.0f;
  refuse(f, "fault_throw_m");
  // Members: one or the other, their own ranges, and no more beds than a table holds.
  f = fixture();
  f.members.push_back(member("extra", 40.0f, 0.5f, 1.0f));
  refuse(f, "not both");
  f = drawn(1);
  CHECK(gfx::pack_formation(f, p, &error));
  f.members[2].resistant = 1.5f;
  refuse(f, "resistant");
  f = drawn(1);
  f.members[0].bed_m = 0.01f;
  refuse(f, "bed_m");
  f = drawn(1);
  f.members[3].sigma = 2.0f;
  refuse(f, "sigma");
  f = drawn(1);
  f.members[1].variation = 0.6f;
  refuse(f, "variation");
  f = drawn(1);
  f.members[4].thickness_m = 900.0f;
  f.members[4].resistant = 0.5f;
  f.members[4].bed_m = 0.2f;  // some 3,500 beds
  refuse(f, "more than 512 beds");
  CHECK(error.find("shale") != std::string::npos);
}

TEST_CASE(
    "formation: a bed repeats with its sequence's period, and the fold moves it by its "
    "amplitude") {
  std::string error;
  auto block = std::make_unique<gfx::FormationParams>();
  gfx::FormationParams& p = *block;
  REQUIRE(gfx::pack_formation(fixture(), p, &error));
  const f64 period = static_cast<f64>(p.beds.period_mm) / 1000.0;
  // Periodicity: the same bed, the same fraction, a whole number of periods away, either side.
  for (f64 c = -7.3; c < 7.3; c += 0.137) {
    const gfx::FormationBedSample a = gfx::formation_bed(p.beds, c);
    for (const f64 k : {-3.0, 1.0, 250.0}) {
      const gfx::FormationBedSample b = gfx::formation_bed(p.beds, c + k * period);
      CHECK(a.index == b.index);
      CHECK(std::fabs(a.fraction - b.fraction) < 1e-9 * (1.0 + std::fabs(k)) * 100.0);
    }
  }
  // The beds in order from the base: 1.42 m of the first, then the second.
  CHECK(gfx::formation_bed(p.beds, 0.01).index == 0u);
  CHECK(gfx::formation_bed(p.beds, 1.43).index == 1u);
  CHECK(gfx::formation_bed(p.beds, 1.80).index == 2u);
  CHECK(gfx::formation_bed(p.beds, 4.82).index == 3u);
  CHECK(gfx::formation_bed(p.beds, -0.01).index == 3u);
  // A box wider than many periods reads the sequence's mean.
  const Vec4 mean = gfx::formation_filtered(p.beds, 0.3, 40.0 * period);
  CHECK(std::fabs(static_cast<f64>(mean.x) - static_cast<f64>(p.beds.mean.x)) < 1e-4);
  CHECK(std::fabs(static_cast<f64>(mean.w) - static_cast<f64>(p.beds.mean.w)) < 1e-4);
  // The fold: horizontal beds (dip 0, azimuth 0, so the fold runs along x) with one 12 m fold of
  // 180 m. The bed surface through the origin stands at y = A sin(2 pi x / L): +A at a quarter
  // wavelength, -A at three quarters, zero at the hinge points between.
  scene::Formation flat = fixture();
  flat.origin = WorldPos{0.0, 0.0, 0.0};
  flat.dip_deg = 0.0f;
  flat.azimuth_deg = 0.0f;
  flat.fold2_amplitude_m = 0.0f;
  REQUIRE(gfx::pack_formation(flat, p, &error));
  const gfx::FormationFrame frame = gfx::formation_frame(p);
  CHECK(std::fabs(gfx::formation_coordinate(frame, 45.0, 12.0, 3.0).coordinate) < 1e-9);
  CHECK(std::fabs(gfx::formation_coordinate(frame, 135.0, -12.0, -7.0).coordinate) < 1e-9);
  CHECK(std::fabs(gfx::formation_coordinate(frame, 90.0, 0.0, 0.0).coordinate) < 1e-9);
  CHECK(std::fabs(gfx::formation_coordinate(frame, 45.0, 0.0, 0.0).coordinate + 12.0) < 1e-9);
  // A wavelength on, the same phase: a point 5 m up there is 7 m under the folded surface.
  CHECK(std::fabs(gfx::formation_coordinate(frame, 225.0, 5.0, 0.0).coordinate - 5.0 + 12.0) <
        1e-9);
}

TEST_CASE("formation: an authored sequence is the table W85 packed, and filters as it did") {
  const scene::Formation f = fixture();
  const auto p = packed(f);
  // W85's own arithmetic, kept here: starts and thicknesses in whole millimetres, a linear search,
  // and the plain integral of the value from the base.
  for (u32 s = 0; s < 2; ++s) {
    const Vector<scene::FormationBed>& beds = s == 0 ? f.beds : f.macro_beds;
    const gfx::FormationSequence& table = s == 0 ? p->beds : p->macro;
    const u32 count = static_cast<u32>(beds.size());
    REQUIRE(table.count == count);
    f64 start[9] = {};
    for (u32 i = 0; i < count; ++i) {
      const f64 thick =
          static_cast<f64>(std::llround(static_cast<f64>(beds[i].thickness_m) * 1000.0)) / 1000.0;
      CHECK(table.beds[i].start_m == static_cast<f32>(start[i]));
      CHECK(table.beds[i].thickness_m == static_cast<f32>(thick));
      CHECK(table.beds[i].resistance == beds[i].resistance);
      CHECK(table.beds[i].value.x == beds[i].tint.value_or(Vec3{1.0f, 1.0f, 1.0f}).x);
      CHECK(table.beds[i].value.w == beds[i].roughness);
      // No top of an authored bed moves unless its `variation` or `lens` says so.
      CHECK(table.beds[i].base_amp_m[0] == 0.0f);
      CHECK(table.beds[i].base_amp_m[1] == 0.0f);
      start[i + 1] = start[i] + thick;
    }
    const f64 period = start[count];
    const auto integral = [&](f64 x, u32 c) {
      const f64 k = std::floor(x / period);
      const f64 y = x - k * period;
      f64 sum = 0.0;
      for (u32 i = 0; i < count; ++i) {
        const f64 hi = y < start[i + 1] ? y : start[i + 1];
        if (hi > start[i]) sum += (hi - start[i]) * channel(table.beds[i].value, c);
      }
      f64 whole = 0.0;
      for (u32 i = 0; i < count; ++i)
        whole += (start[i + 1] - start[i]) * channel(table.beds[i].value, c);
      return k * whole + sum;
    };
    f64 worst = 0.0;
    for (f64 c = -3.1 * period; c < 3.1 * period; c += period / 37.3) {
      for (const f64 width : {0.004, 0.1, 0.77, 3.0, 2.6 * period}) {
        const Vec4 got = gfx::formation_filtered(table, c, width);
        for (u32 ch = 0; ch < 4; ++ch) {
          const f64 want = (integral(c + 0.5 * width, ch) - integral(c - 0.5 * width, ch)) / width;
          const f64 e = std::fabs(channel(got, ch) - want);
          worst = e > worst ? e : worst;
        }
      }
      // W85's `formation_integral`, the plain integral from the base, still is one.
      f64 plain[4];
      gfx::formation_integral(table, c, plain);
      CHECK(std::fabs(plain[0] - integral(c, 0)) < 1e-4 * (1.0 + std::fabs(c)));
    }
    MESSAGE("sequence " << s << ": the filter against W85's integral, worst " << worst);
    // The worst is the 4 mm box across a contact: two f32 prefixes a few units in size, an ulp
    // apart, over 0.004. (W85's plain integral, up to the period times the value, was 2e-3 there.)
    CHECK(worst < 1e-4);
  }
}

TEST_CASE("formation: a drawn sequence does not repeat inside its section, over five seeds") {
  for (u64 seed = 1; seed <= 5; ++seed) {
    const scene::Formation f = drawn(seed);
    const auto p = packed(f);
    const gfx::FormationSequence& s = p->beds;
    // The section is the members' whole thickness, to the millimetre, and the macro sequence is
    // the members.
    CHECK(s.period_mm == 264000u);
    CHECK(p->macro.period_mm == 264000u);
    CHECK(p->macro.count == 6u);
    CHECK(gfx::formation_section_m(*p) == 264.0);
    CHECK(s.count > 100u);
    CHECK(s.count <= gfx::k_formation_max_beds);
    // Every bed a centimetre or more, in its member, and each member's beds fill it exactly.
    u64 in_member = 0;
    u32 m = 0;
    for (u32 i = 0; i < s.count; ++i) {
      CHECK(thickness_mm(s.beds[i]) >= 10u);
      if (s.beds[i].member != m) {
        CHECK(in_member == thickness_mm(p->macro.beds[m]));
        CHECK(s.beds[i].member == m + 1);
        m = s.beds[i].member;
        in_member = 0;
      }
      in_member += thickness_mm(s.beds[i]);
    }
    CHECK(m == 5u);
    CHECK(in_member == thickness_mm(p->macro.beds[5]));
    // No run of four beds' thicknesses is found twice in the section. (W85's fixture, eight beds
    // at most, was its own run again after 4.83 m.)
    u32 repeats = 0;
    for (u32 a = 0; a + 4 <= s.count; ++a)
      for (u32 b = a + 1; b + 4 <= s.count; ++b) {
        bool same = true;
        for (u32 k = 0; k < 4 && same; ++k)
          same = thickness_mm(s.beds[a + k]) == thickness_mm(s.beds[b + k]);
        repeats += same ? 1u : 0u;
      }
    CHECK(repeats == 0u);
    // And another seed is another section.
    const auto other = packed(drawn(seed + 100));
    CHECK(other->beds.period_mm == s.period_mm);
    u32 differ = 0;
    for (u32 i = 0; i < 40; ++i)
      differ += thickness_mm(other->beds.beds[i]) != thickness_mm(s.beds[i]) ? 1u : 0u;
    CHECK(differ > 30u);
  }
}

TEST_CASE("formation: a member's beds are lognormal about bed_m, and its share is `resistant`") {
  // One member a kilometre thick, so the draw is the whole of it: two lithologies of one median.
  f64 sum = 0.0, squares = 0.0, resistant_mm = 0.0, total_mm = 0.0;
  u32 n = 0;
  for (u64 seed = 1; seed <= 5; ++seed) {
    scene::Formation f;
    f.seed = seed;
    f.members.push_back(member("even", 1000.0f, 0.5f, 2.0f));
    const auto p = packed(f);
    REQUIRE(p->beds.count > 300u);
    // The member's last bed is what was left, not a draw.
    for (u32 i = 0; i + 1 < p->beds.count; ++i) {
      const f64 l = det::log(static_cast<f64>(thickness_mm(p->beds.beds[i])) / 2000.0);
      sum += l;
      squares += l * l;
      ++n;
    }
    scene::Formation cliff;
    cliff.seed = seed;
    cliff.members.push_back(member("cliff", 1000.0f, 0.8f, 3.0f));
    const auto q = packed(cliff);
    for (u32 i = 0; i < q->beds.count; ++i) {
      const f64 mm = static_cast<f64>(thickness_mm(q->beds.beds[i]));
      total_mm += mm;
      // The kind is the lithology; the resistance says the same.
      const bool resistant = (q->beds.beds[i].kind & gfx::k_formation_bed_resistant) != 0;
      CHECK(resistant == (q->beds.beds[i].resistance >= gfx::k_formation_cliff_resistance));
      if (resistant) resistant_mm += mm;
    }
  }
  const f64 mean = sum / n;
  const f64 deviation = std::sqrt(squares / n - mean * mean);
  MESSAGE(n << " beds: ln(thickness / bed_m) mean " << mean << ", deviation " << deviation
            << " (sigma 0.7); resistant share " << resistant_mm / total_mm << " (0.8)");
  // 1,900 draws: the mean's standard error is 0.016 and the deviation's 0.011.
  CHECK(std::fabs(mean) < 0.05);
  CHECK(std::fabs(deviation - 0.7) < 0.04);
  CHECK(std::fabs(resistant_mm / total_mm - 0.8) < 0.04);
}

TEST_CASE("formation: the thinnest member and the thickest draw inside the same rules") {
  // The defaults were chosen about a metre bed in a member of tens of metres. The two ends they
  // must also serve: laminated shale, 5 cm beds in half a metre, and a pile of 100 m flows a
  // kilometre thick, each at the widest spread and the most variation and lenses a member allows.
  for (u64 seed = 1; seed <= 4; ++seed) {
    for (u32 which = 0; which < 2; ++which) {
      scene::Formation f;
      f.seed = seed;
      scene::FormationMember m = which == 0 ? member("laminated", 0.5f, 0.3f, 0.05f)
                                            : member("flows", 1000.0f, 0.9f, 100.0f);
      m.sigma = 1.5f;
      m.variation = gfx::k_formation_max_variation;
      m.lenses = 1.0f;
      f.members.push_back(m);
      f.members.push_back(member("beside", 20.0f, 0.5f, 1.0f));
      const auto p = packed(f);
      const gfx::FormationSequence& s = p->beds;
      const u64 whole = which == 0 ? 500u : 1000000u;
      CHECK(s.period_mm == whole + 20000u);
      u64 in_member = 0;
      u32 beds = 0;
      for (u32 i = 0; i < s.count && s.beds[i].member == 0; ++i) {
        CHECK(thickness_mm(s.beds[i]) >= 10u);
        in_member += thickness_mm(s.beds[i]);
        ++beds;
      }
      CHECK(in_member == whole);
      CHECK(beds >= 1u);
      // No top crosses another at any pair of lateral fields' ends, nor between them (to the f32
      // of a start a kilometre up the section, 0.06 mm).
      const f64 period = static_cast<f64>(s.period_mm) / 1000.0;
      for (const f64 g0 : {0.0, 0.37, 1.0})
        for (const f64 g1 : {0.0, 0.61, 1.0}) {
          gfx::FormationLateral g;
          g.g[0] = g0;
          g.g[1] = g1;
          for (u32 i = 0; i < s.count; ++i) {
            const f64 top = i + 1 < s.count ? gfx::formation_base(s, i + 1, g) : period;
            CHECK(top - gfx::formation_base(s, i, g) >= -2e-4);
          }
        }
    }
  }
}

TEST_CASE("formation: a top varies along strike continuously and inside its range") {
  u32 lenses = 0, lenses_gone = 0, lenses_whole = 0;
  for (u64 seed = 1; seed <= 4; ++seed) {
    scene::Formation f = drawn(seed);
    f.fault_throw_m = 0.0f;
    const auto p = packed(f);
    const gfx::FormationSequence& s = p->beds;
    const gfx::FormationFrame frame = gfx::formation_frame(*p);
    REQUIRE(frame.lateral_cell[0] == 150.0);
    REQUIRE(std::fabs(frame.lateral_cell[1] - 150.0 * gfx::k_formation_lateral_ratio) < 1e-3);
    const u32 count = s.count;
    Vector<f64> thinnest, thickest;
    for (u32 i = 0; i < count; ++i) {
      thinnest.push_back(1e9);
      thickest.push_back(0.0);
    }
    const f64 period = static_cast<f64>(s.period_mm) / 1000.0;
    // Twenty kilometres along a line across both fields (some fifty cells of each), 1.98 m a step.
    gfx::FormationLateral before = gfx::formation_lateral(frame, -7000.0, 0.0, -7000.0);
    f64 steepest = 0.0;
    bool reached[2][2] = {};
    for (i32 step = 1; step <= 10000; ++step) {
      const f64 at = -7000.0 + 1.4 * static_cast<f64>(step);
      const gfx::FormationLateral g = gfx::formation_lateral(frame, at, 0.0, at);
      for (u32 k = 0; k < 2; ++k) {
        CHECK(g.g[k] >= 0.0);
        CHECK(g.g[k] <= 1.0);
        const f64 d = std::fabs(g.g[k] - before.g[k]) * frame.lateral_cell[k];
        steepest = d > steepest ? d : steepest;
        reached[k][0] = reached[k][0] || g.g[k] == 0.0;
        reached[k][1] = reached[k][1] || g.g[k] == 1.0;
      }
      before = g;
      for (u32 i = 0; i < count; ++i) {
        const f64 base = gfx::formation_base(s, i, g);
        const f64 top = i + 1 < count ? gfx::formation_base(s, i + 1, g) : period;
        const f64 thick = top - base;
        thinnest[i] = thick < thinnest[i] ? thick : thinnest[i];
        thickest[i] = thick > thickest[i] ? thick : thickest[i];
      }
    }
    // Continuous: a field's slope is at most its sharpening times the cubic's 1.5 a cell, so a
    // step of 1.98 m moves it by no more than gain * 1.5 * 1.98 / cell.
    CHECK(steepest <= static_cast<f64>(gfx::k_formation_lateral_gain) * 1.5 * 1.98 * 1.001);
    // Each field rests at both ends of its travel somewhere on the line.
    for (u32 k = 0; k < 2; ++k)
      CHECK((reached[k][0] && reached[k][1]));
    u32 moved = 0;
    for (u32 i = 0; i < count; ++i) {
      const f64 t = static_cast<f64>(s.beds[i].thickness_m);
      // Never inverted: a bed keeps at least a tenth of itself (two tops at 0.45 of it), a lens
      // goes to nothing and no further (a twentieth of a millimetre of f32).
      CHECK(thinnest[i] >= -1e-4);
      moved += thickest[i] - thinnest[i] > 1e-3 ? 1u : 0u;
      if ((s.beds[i].kind & gfx::k_formation_bed_lens) != 0) {
        ++lenses;
        lenses_gone += thinnest[i] < 1e-3 ? 1u : 0u;
        lenses_whole += thickest[i] > t - 1e-3 ? 1u : 0u;
      } else {
        // The members' variation is 0.3: a bed keeps 1 - 2 * 0.3 of itself under any pair of tops.
        CHECK(thinnest[i] >= t * 0.4 - 1e-3);
      }
    }
    CHECK(moved > count / 2);
    // The members' tops and the sequence's own stay where the description put them.
    for (u32 i = 1; i < count; ++i)
      if (s.beds[i].member != s.beds[i - 1].member) {
        CHECK(s.beds[i].base_amp_m[0] == 0.0f);
        CHECK(s.beds[i].base_amp_m[1] == 0.0f);
      }
    CHECK(s.beds[0].base_amp_m[0] == 0.0f);
    CHECK(s.beds[0].base_amp_m[1] == 0.0f);
  }
  MESSAGE(lenses << " lenses over four seeds: " << lenses_gone << " pinch out on the line, "
                 << lenses_whole << " stand whole on it");
  CHECK(lenses >= 20u);
  CHECK(lenses_gone == lenses);
  CHECK(lenses_whole == lenses);
}

TEST_CASE("formation: the box filter is the varied sequence's own average") {
  // The prefix sums of the tops' travel against a sum over the beds themselves: 300 boxes at
  // lateral fields of every kind, each compared with 4,000 point samples of the bed under it.
  f64 worst = 0.0;
  for (u64 seed = 1; seed <= 3; ++seed) {
    const auto p = packed(drawn(seed));
    const f64 period = static_cast<f64>(p->beds.period_mm) / 1000.0;
    for (u32 q = 0; q < 100; ++q) {
      const u64 key = gfx::formation_key(seed, 77, q);
      gfx::FormationLateral g;
      g.g[0] = q % 5 == 0 ? 1.0 : gfx::formation_unit(gfx::formation_key(key, 1, 0));
      g.g[1] = q % 7 == 0 ? 1.0 : gfx::formation_unit(gfx::formation_key(key, 2, 0));
      // Boxes through the period's wrap too.
      const f64 c = (gfx::formation_unit(gfx::formation_key(key, 3, 0)) * 3.0 - 1.5) * period;
      const f64 width = 0.05 + 12.0 * gfx::formation_unit(gfx::formation_key(key, 4, 0));
      const Vec4 got = gfx::formation_filtered(p->beds, c, width, g);
      f64 sum[4] = {};
      const u32 samples = 4000;
      for (u32 k = 0; k < samples; ++k) {
        const f64 at = c - 0.5 * width + (static_cast<f64>(k) + 0.5) * width / samples;
        const Vec4 v = gfx::formation_bed(p->beds, at, g).value;
        for (u32 ch = 0; ch < 4; ++ch)
          sum[ch] += channel(v, ch);
      }
      for (u32 ch = 0; ch < 4; ++ch) {
        const f64 e = std::fabs(channel(got, ch) - sum[ch] / samples);
        worst = e > worst ? e : worst;
      }
    }
  }
  MESSAGE("the filter against 4,000 samples a box, worst " << worst);
  // A box of 12 m crosses a dozen tops, each sampled to a 4,000th of the box: under 1e-3.
  CHECK(worst < 2e-3);
}

TEST_CASE("formation: a fault throws every bed beyond it, and nothing else is discontinuous") {
  scene::Formation f = drawn(2);
  f.dip_deg = 0.0f;
  f.fold_amplitude_m = 0.0f;
  f.fold2_amplitude_m = 0.0f;
  f.origin = WorldPos{0.0, 0.0, 0.0};
  f.fault_azimuth_deg = 0.0f;  // the plane x = 17.5
  const auto p = packed(f);
  const gfx::FormationFrame frame = gfx::formation_frame(*p);
  CHECK(p->fault_throw_mm == 3250);
  CHECK(p->fault_offset_mm == 17500);
  CHECK(gfx::formation_coordinate(frame, 17.499, 5.0, 3.0).coordinate == 5.0);
  CHECK(gfx::formation_coordinate(frame, 17.501, 5.0, -900.0).coordinate == 8.25);
  CHECK(gfx::formation_coordinate(frame, -4000.0, 5.0, 3.0).coordinate == 5.0);
  CHECK(gfx::formation_coordinate(frame, 4000.0, 5.0, 3.0).coordinate == 8.25);
  // The bed at a point beyond the fault is the bed 3.25 m higher in the section this side of it.
  const gfx::FormationSample near = gfx::formation_sample(*p, frame, 17.0, 20.0, 0.0);
  const gfx::FormationSample far = gfx::formation_sample(*p, frame, 18.0, 20.0, 0.0);
  CHECK(far.at.coordinate - near.at.coordinate == 3.25);
  // Without a throw the frame holds no fault and the coordinate is W85's.
  f.fault_throw_m = 0.0f;
  const auto plain = packed(f);
  CHECK(plain->fault_q[0] == 0);
  CHECK(gfx::formation_coordinate(gfx::formation_frame(*plain), 4000.0, 5.0, 3.0).coordinate ==
        5.0);
}

TEST_CASE("formation: what a solver reads at a point: the bed, its member, and their bounds") {
  const scene::Formation f = drawn(3);
  const auto p = packed(f);
  const gfx::FormationFrame frame = gfx::formation_frame(*p);
  u32 cliff = 0, slope = 0;
  for (u32 q = 0; q < 2000; ++q) {
    const u64 key = gfx::formation_key(9, 1, q);
    const f64 x = (gfx::formation_unit(gfx::formation_key(key, 1, 0)) - 0.5) * 4000.0;
    const f64 y = (gfx::formation_unit(gfx::formation_key(key, 2, 0)) - 0.5) * 900.0;
    const f64 z = (gfx::formation_unit(gfx::formation_key(key, 3, 0)) - 0.5) * 4000.0;
    const gfx::FormationSample s = gfx::formation_sample(*p, frame, x, y, z);
    const f64 c = s.at.coordinate;
    // The bed's bounds hold the point, the member's hold the bed, and the two agree on the member.
    CHECK(s.bed.base <= c);
    CHECK(c <= s.bed.top);
    CHECK(s.member.base <= s.bed.base + 1e-4);
    CHECK(s.bed.top <= s.member.top + 1e-4);
    CHECK(s.member.index == s.bed.member);
    CHECK(s.member.index == p->beds.beds[s.bed.index].member);
    // A member's bounds are its thickness apart wherever the point is: its tops do not move.
    CHECK(std::fabs(s.member.top - s.member.base -
                    static_cast<f64>(f.members[s.member.index].thickness_m)) < 1e-3);
    // Its resistance is its share's, and the line between a cliff-former and a slope-former a half.
    const f64 share = static_cast<f64>(f.members[s.member.index].resistant);
    CHECK(std::fabs(static_cast<f64>(s.member.resistance) - (0.2 + 0.65 * share)) < 1e-6);
    const bool forms_cliff = s.member.resistance >= gfx::k_formation_cliff_resistance;
    CHECK(forms_cliff == (share > 0.47));
    cliff += forms_cliff ? 1u : 0u;
    slope += forms_cliff ? 0u : 1u;
    // The plain query is the reference section's: the same bed wherever no top near it moves.
    const gfx::FormationBedSample plain = gfx::formation_bed(p->beds, c);
    CHECK(plain.member == s.bed.member);
  }
  CHECK(cliff > 500u);
  CHECK(slope > 500u);
  // A formation with no macro sequence is one member: the whole period.
  scene::Formation bare = fixture();
  bare.macro_beds.clear();
  const auto b = packed(bare);
  const gfx::FormationBedSample whole = gfx::formation_member(*b, 7.0, gfx::FormationLateral{});
  CHECK(whole.index == 0u);
  CHECK(whole.base == 4.83);
  CHECK(whole.top == 9.66);
  CHECK(std::fabs(static_cast<f64>(whole.resistance) -
                  (1.42 * 0.85 + 0.37 * 0.15 + 0.81 * 0.45 + 2.23 * 0.95) / 4.83) < 1e-6);
}

TEST_CASE("formation: a drawn table is the same bytes on every compiler") {
  // The whole block of the drawn fixture: integer hashes, det::exp and det::sin, whole
  // millimetres. Pinned on MSVC (msvc-debug, 2026-11-10) and reproduced by Clang and GCC.
  const auto p = packed(drawn(1));
  const u64 hash = fnv1a(p.get(), sizeof(gfx::FormationParams));
  MESSAGE("drawn(1): " << p->beds.count << " beds, block hash " << hash);
  CHECK(p->beds.count == 140u);
  CHECK(hash == 8898964259588582771ull);
}

TEST_CASE("formation: the frame-only pack is the whole block's frame to the bit") {
  // T94: a caller that wants the frame packs 192 bytes of head and no table. The head and the frame
  // it gives are the packed block's, byte for byte, for an authored field (no lateral fields), an
  // authored one whose beds' tops move, one whose macro beds' tops alone move, and drawn ones
  // (lateral fields from the plans, a fault).
  scene::Formation moving = fixture();
  moving.beds[1].variation = 0.3f;
  moving.beds[2].lens = true;
  scene::Formation macro_moving = fixture();
  macro_moving.macro_beds[0].variation = 0.4f;
  scene::Formation still = drawn(3);
  for (scene::FormationMember& m : still.members) {
    m.variation = 0.0f;
    m.lenses = 0.0f;
  }
  const scene::Formation cases[] = {fixture(), moving, macro_moving, drawn(1), drawn(7), still};
  u32 with_fields = 0;
  for (const scene::Formation& f : cases) {
    const auto p = packed(f);
    const gfx::FormationFrame want = gfx::formation_frame(*p);
    gfx::FormationFrame got;
    std::string error;
    REQUIRE_MESSAGE(gfx::pack_formation_frame(f, got, &error), error);
    CHECK(std::memcmp(&got, &want, sizeof(gfx::FormationFrame)) == 0);
    gfx::FormationHead head;
    Vector<gfx::FormationBedPlan> beds, members;
    REQUIRE(gfx::pack_formation_head(f, head, beds, members, &error));
    CHECK(std::memcmp(&head, p.get(), sizeof(gfx::FormationHead)) == 0);
    const gfx::FormationHead copied = gfx::formation_head(*p);
    CHECK(std::memcmp(&head, &copied, sizeof(gfx::FormationHead)) == 0);
    CHECK(gfx::check_formation(f, &error));
    // The fields are on exactly when a top of either table moves.
    bool moves = false;
    for (const gfx::FormationSequence* s : {&p->beds, &p->macro})
      for (u32 i = 0; i < s->count; ++i)
        moves = moves || s->beds[i].base_amp_m[0] != 0.0f || s->beds[i].base_amp_m[1] != 0.0f;
    CHECK((head.lateral[0].cell_mm != 0u) == moves);
    with_fields += moves ? 1u : 0u;
  }
  CHECK(with_fields == 4u);
  // A formation that does not pack: the same sentence from all three, and the frame left as it was.
  scene::Formation bad = fixture();
  bad.dip_deg = 89.0f;
  gfx::FormationFrame untouched;
  untouched.origin[0] = 42.0;
  std::string frame_error, block_error, check_error;
  CHECK_FALSE(gfx::pack_formation_frame(bad, untouched, &frame_error));
  CHECK(untouched.origin[0] == 42.0);
  auto block = std::make_unique<gfx::FormationParams>();
  CHECK_FALSE(gfx::pack_formation(bad, *block, &block_error));
  CHECK_FALSE(gfx::check_formation(bad, &check_error));
  CHECK(frame_error == block_error);
  CHECK(check_error == block_error);
  CHECK(block->flags == 0u);
  CHECK(block->beds.count == 0u);
}

TEST_CASE("formation: the shader's coordinate, bed and filter are its f64 twin's") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  const auto module = gfx::create_shader_module(device, shaders::k_formation_probe_spirv,
                                                shaders::k_formation_probe_spirv_size, &error);
  REQUIRE_MESSAGE(module.valid(), error);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "formation_probe_main", {}, 32,
                                               pipeline, &error),
                  error);
  struct Point {
    i32 cell[3];
    f32 width;
    f32 local[3];
    f32 pad;
  };
  Vector<Point> points;
  // A lattice round the origin and round four far places, 10 km and 10,000 km out either way: the
  // reduction is what keeps the far ones exact.
  const i32 centres[][3] = {
      {0, 0, 0}, {156, 1, -156}, {-157, -2, 155}, {156250, 3, 156250}, {-156250, -1, -156250}};
  for (const auto& centre : centres)
    for (i32 a = -3; a <= 3; ++a)
      for (i32 b = -2; b <= 2; ++b)
        for (i32 c = -3; c <= 3; ++c) {
          Point q{};
          q.cell[0] = centre[0] + (a < 0 ? -1 : 0);
          q.cell[1] = centre[1] + (b < 0 ? -1 : 0);
          q.cell[2] = centre[2] + (c < 0 ? -1 : 0);
          q.local[0] = static_cast<f32>(std::fmod(17.3 * a + 64.0 + 0.001 * c, 64.0));
          q.local[1] = static_cast<f32>(std::fmod(9.71 * b + 64.0, 64.0));
          q.local[2] = static_cast<f32>(std::fmod(13.9 * c + 64.0 + 0.37 * a, 64.0));
          q.width = static_cast<f32>((a + 3) * 0.25);
          points.push_back(q);
        }
  const auto storage = gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx::BufferResource block, input, output;
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::FormationParams), storage, true, block, &error));
  REQUIRE(
      gfx::create_buffer(device, u64{points.size()} * sizeof(Point), storage, true, input, &error));
  std::memcpy(input.mapped, points.data(), u64{points.size()} * sizeof(Point));
  REQUIRE(gfx::create_buffer(device, u64{points.size()} * 4 * sizeof(Vec4), storage, true, output,
                             &error));
  struct Push {
    u64 output;
    u64 formation;
    u64 points;
    u32 count;
    u32 pad;
  };
  const Push push{output.address, block.address, input.address, points.size(), 0};
  // W85's authored fixture, then the drawn one with its tops moving, its fault and its folds.
  for (u32 which = 0; which < 2; ++which) {
    const auto held = packed(which == 0 ? fixture() : drawn(1));
    const gfx::FormationParams& params = *held;
    const gfx::FormationFrame frame = gfx::formation_frame(params);
    std::memcpy(block.mapped, &params, sizeof(params));
    REQUIRE_MESSAGE(gfx::submit_immediate(
                        device,
                        [&](gfx::CommandList cb) {
                          cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
                          cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0,
                                            sizeof(push), &push);
                          cb.dispatch((points.size() + 63) / 64, 1, 1);
                        },
                        &error),
                    error);
    const auto* got = static_cast<const Vec4*>(output.mapped);
    const f64 period = static_cast<f64>(params.beds.period_mm) / 1000.0;
    const f64 macro_period = static_cast<f64>(params.macro.period_mm) / 1000.0;
    f64 worst = 0.0, worst_macro = 0.0, worst_gradient = 0.0, worst_filter = 0.0,
        worst_lateral = 0.0;
    u32 compared = 0, edges = 0, moved = 0, thrown = 0;
    for (u32 i = 0; i < points.size(); ++i) {
      const Point& q = points[i];
      // What the float local means, exactly: the twin's own reading of the probe's point.
      const f64 x = static_cast<f64>(q.cell[0]) * 64.0 + static_cast<f64>(q.local[0]);
      const f64 y = static_cast<f64>(q.cell[1]) * 64.0 + static_cast<f64>(q.local[1]);
      const f64 z = static_cast<f64>(q.cell[2]) * 64.0 + static_cast<f64>(q.local[2]);
      const gfx::FormationSample want = gfx::formation_sample(params, frame, x, y, z);
      const f64 coordinate = want.at.coordinate;
      const Vec4 a = got[4 * i], g = got[4 * i + 1], filtered = got[4 * i + 2],
                 extra = got[4 * i + 3];
      const f64 e = std::fabs(wrap(static_cast<f64>(a.x) - coordinate, period));
      const f64 em = std::fabs(wrap(static_cast<f64>(a.y) - coordinate, macro_period));
      worst = e > worst ? e : worst;
      worst_macro = em > worst_macro ? em : worst_macro;
      CHECK_MESSAGE(e < 1e-3, "point " << i << ": " << a.x << " against " << coordinate);
      CHECK_MESSAGE(em < 1e-3, "point " << i);
      const f64 grad[3] = {static_cast<f64>(g.x), static_cast<f64>(g.y), static_cast<f64>(g.z)};
      for (u32 k = 0; k < 3; ++k) {
        const f64 d = std::fabs(grad[k] - want.at.gradient[k]);
        worst_gradient = d > worst_gradient ? d : worst_gradient;
      }
      // The lateral fields: the same lattice integers, the cubic in f32.
      const f64 lateral[2] = {static_cast<f64>(extra.x), static_cast<f64>(extra.y)};
      for (u32 k = 0; k < 2; ++k) {
        const f64 d = std::fabs(lateral[k] - want.lateral.g[k]);
        worst_lateral = d > worst_lateral ? d : worst_lateral;
      }
      moved += want.lateral.g[0] > 0.0 || want.lateral.g[1] > 0.0 ? 1u : 0u;
      thrown += frame.fault_throw != 0.0 && frame.fault[0] * (x - frame.origin[0]) +
                                                    frame.fault[1] * (z - frame.origin[2]) >
                                                frame.fault_offset
                    ? 1u
                    : 0u;
      // The bed, where the point is not within three millimetres of a boundary as it stands here
      // (a top that moves five metres carries the lateral field's error times that).
      const gfx::FormationBedSample& b = want.bed;
      const gfx::FormationBedSample& m = want.member;
      if (coordinate - b.base < 3e-3 || b.top - coordinate < 3e-3 || coordinate - m.base < 3e-3 ||
          m.top - coordinate < 3e-3) {
        ++edges;
        continue;
      }
      ++compared;
      CHECK(static_cast<u32>(a.z) == b.index);
      CHECK(static_cast<u32>(a.w) == m.index);
      CHECK(std::fabs(static_cast<f64>(g.w) - b.fraction) < 3e-3 / (b.top - b.base) + 2e-3);
      CHECK(extra.w == b.resistance);
      const Vec4 wf =
          gfx::formation_filtered(params.beds, coordinate, static_cast<f64>(q.width), want.lateral);
      const Vec4 wm = gfx::formation_filtered(params.macro, coordinate, static_cast<f64>(q.width),
                                              want.lateral);
      const f64 df = std::fabs(static_cast<f64>(filtered.x) - static_cast<f64>(wf.x)) +
                     std::fabs(static_cast<f64>(filtered.w) - static_cast<f64>(wf.w)) +
                     std::fabs(static_cast<f64>(extra.z) - static_cast<f64>(wm.x));
      worst_filter = df > worst_filter ? df : worst_filter;
    }
    MESSAGE(std::string(which == 0 ? "authored" : "drawn")
            << ": coordinate error " << worst << " m, macro " << worst_macro << " m, gradient "
            << worst_gradient << ", lateral " << worst_lateral << ", filter " << worst_filter
            << "; " << compared << " beds compared, " << edges << " within 3 mm of an edge, "
            << moved << " with a field over zero, " << thrown << " beyond the fault");
    CHECK(worst_gradient < 1e-4);
    CHECK(worst_lateral < 1e-3);
    // The box's edges move with the coordinate, by the coordinate's error, and with the tops.
    CHECK(worst_filter < 2e-3);
    CHECK(compared > points.size() / 2);
    if (which == 1) {
      CHECK(moved > points.size() / 4);
      CHECK(thrown > points.size() / 10);
      CHECK(thrown < points.size() - points.size() / 10);
    }
  }
  for (gfx::BufferResource* b : {&block, &input, &output})
    gfx::destroy_buffer(device, *b);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}
