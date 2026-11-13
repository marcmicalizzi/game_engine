// The plan of one footbridge (docs/subsystems/bridges.md, "The form", "The members"): the form a
// footpath would build at a crossing, chosen by a rule over the crossing's bankfull width and depth
// and the bed's stone, and every member solved in whole millimetres of the bridge's frame. A
// function of the crossing and its seed alone, integer arithmetic throughout.

#include <core/hash/hash.h>
#include <domain/bridges/bridges.h>

#include <algorithm>

namespace engine::bridges {
namespace {

// a / b rounded to nearest, halves away from zero; b > 0.
i64 div_round(i64 a, i64 b) noexcept { return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b); }

// A stream of draws from a seed: each draw a fresh mix, so a member's shape never depends on how
// many draws another took.
struct Draws {
  u64 state;
  u64 next() noexcept {
    state += 0x9E3779B97F4A7C15ull;
    return mix64(state);
  }
  // Uniform in [lo, hi], integers.
  i32 range(i32 lo, i32 hi) noexcept {
    const u64 span = static_cast<u64>(static_cast<i64>(hi) - lo + 1);
    return lo + static_cast<i32>(next() % span);
  }
};

i32 ground_rel(const Crossing& c, i64 x) noexcept {
  return static_cast<i32>(profile_at(c, x) - c.bed_mm);
}

// The highest ground under [x0, x1] (the profile's points inside and both ends).
i32 ground_max(const Crossing& c, i32 x0, i32 x1) noexcept {
  i32 g = std::max(ground_rel(c, x0), ground_rel(c, x1));
  const i32 step = static_cast<i32>(c.profile_step_mm);
  for (i32 x = x0 + step; x < x1; x += step)
    g = std::max(g, ground_rel(c, x));
  return g;
}
i32 ground_min(const Crossing& c, i32 x0, i32 x1) noexcept {
  i32 g = std::min(ground_rel(c, x0), ground_rel(c, x1));
  const i32 step = static_cast<i32>(c.profile_step_mm);
  for (i32 x = x0 + step; x < x1; x += step)
    g = std::min(g, ground_rel(c, x));
  return g;
}

u32 add(BridgePlan& plan, Member m) {
  m.seed = static_cast<u32>(mix64(plan.seed ^ (static_cast<u64>(plan.members.size()) << 32)));
  plan.members.push_back(m);
  return plan.members.size() - 1;
}

Member block(MemberKind kind, Shape shape, u8 material, i32 x0, i32 x1, i32 y0, i32 y1, i32 z0,
             i32 z1, i32 support) noexcept {
  Member m;
  m.x0 = x0;
  m.x1 = x1;
  m.y0 = y0;
  m.y1 = y1;
  m.z0 = z0;
  m.z1 = z1;
  m.support = support;
  m.kind = kind;
  m.shape = shape;
  m.material = material;
  return m;
}

// A stone support from `foot` up to `top`, centred on x, `along` wide in x and `across` in z: one
// rough block a course of about 300 mm, each resting on the one below, the last's top `top`
// exactly. Returns the top course's index.
i32 stone_stack(BridgePlan& plan, Draws& d, MemberKind kind, i32 x, i32 along, i32 across, i32 foot,
                i32 top) {
  const i32 height = top - foot;
  const i32 courses = std::max<i32>(1, static_cast<i32>(div_round(height, 300)));
  i32 below = -1;
  i32 y = foot;
  for (i32 k = 0; k < courses; ++k) {
    const i32 y1 =
        k + 1 == courses
            ? top
            : foot + static_cast<i32>(div_round(static_cast<i64>(height) * (k + 1), courses));
    // Each course a little inside the one below it, and shifted, as a laid pier is.
    const i32 inset = 20 * k;
    const i32 jx = d.range(-25, 25), jz = d.range(-40, 40);
    const i32 ax = std::max(along - 2 * inset, along / 2),
              az = std::max(across - 2 * inset, across / 2);
    below = static_cast<i32>(
        add(plan, block(kind, Shape::rough, k_stone, x - ax / 2 + jx, x + ax / 2 + jx, y, y1,
                        -az / 2 + jz, az / 2 + jz, below)));
    y = y1;
  }
  return below;
}

void plan_stones(const Crossing& c, Draws& d, BridgePlan& p) {
  const i32 w = static_cast<i32>(c.width_mm);
  const i32 water = p.water;
  const i32 count = std::max<i32>(1, (w + k_stone_pitch_mm - 1) / k_stone_pitch_mm);
  const i32 first = -((count - 1) * k_stone_pitch_mm) / 2;
  const i32 top = water + k_stone_rise_mm;
  p.deck = top;
  p.soffit = top;
  p.width = 0;
  p.x_start = 0;
  p.x_end = 0;
  bool any = false;
  for (i32 k = 0; k < count; ++k) {
    const i32 x = first + k * k_stone_pitch_mm + d.range(-40, 40);
    const i32 along = d.range(300, 380), across = d.range(380, 500);
    const i32 x0 = x - along / 2, x1 = x + along / 2;
    // Dry footing already: a stone is set only where the ground is under the stone's top by more
    // than its rise (the bank's toe is a step of its own).
    const i32 ground = ground_min(c, x0, x1);
    if (ground > top - k_stone_rise_mm) continue;
    const i32 jz = d.range(-60, 60);
    add(p, block(MemberKind::stone, Shape::rough, k_stone, x0, x1, ground - k_stone_bed_mm, top,
                 -across / 2 + jz, across / 2 + jz, -1));
    if (!any) p.x_start = x0;
    p.x_end = x1;
    p.width = std::max(p.width, across);
    any = true;
  }
  p.span = k_stone_pitch_mm;
}

void plan_clapper(const Crossing& c, Draws& d, BridgePlan& p) {
  const i32 half = static_cast<i32>(c.width_mm) / 2;
  const i32 abut = k_clapper_pier_mm;
  const i32 a0 = -(half + k_setback_mm + abut / 2), a1 = half + k_setback_mm + abut / 2;
  const i32 length = a1 - a0;
  const i32 spans = std::max<i32>(1, (length + k_clapper_max_slab_mm - 1) / k_clapper_max_slab_mm);
  // The slabs lie at the banks' level, above the water, and on the higher of the two seats.
  const i32 seat0 = ground_max(c, a0 - abut / 2, a0 + abut / 2);
  const i32 seat1 = ground_max(c, a1 - abut / 2, a1 + abut / 2);
  const i32 soffit = std::max({p.bankfull, p.water + 200, seat0, seat1});
  const i32 pitch = length / spans;
  const i32 thick = std::clamp(pitch / 10, 150, 250);
  p.soffit = soffit;
  p.deck = soffit + thick;
  p.width = k_clapper_deck_mm;
  p.piers = static_cast<u32>(spans - 1);
  p.span = (a1 - abut / 2) - (a0 + abut / 2);
  const i32 pier_across = k_clapper_deck_mm + 300;
  // The supports, abutment to abutment: each a stack founded below the ground (in the channel,
  // below the bed: `k_pier_found_mm`).
  Vector<i32> centre, top;
  for (i32 k = 0; k <= spans; ++k) {
    const i32 x = k == spans ? a1 : a0 + k * pitch;
    const bool end = k == 0 || k == spans;
    const i32 along = end ? abut : k_clapper_pier_mm;
    const i32 foot = ground_min(c, x - along / 2, x + along / 2) - k_pier_found_mm;
    centre.push_back(x);
    top.push_back(stone_stack(p, d, end ? MemberKind::abutment : MemberKind::pier, x, along,
                              pier_across, foot, soffit));
  }
  // The slabs, support to support, meeting over each pier with a joint, bearing on the whole of
  // each abutment.
  for (i32 k = 0; k < spans; ++k) {
    const i32 x0 = k == 0 ? centre[0] - abut / 2 : centre[k] + 10;
    const i32 x1 = k + 1 == spans ? centre[k + 1] + abut / 2 : centre[k + 1] - 10;
    const i32 jz = d.range(-50, 50), dw = d.range(-80, 0);
    add(p,
        block(MemberKind::slab, Shape::rough, k_stone, x0, x1, soffit, soffit + thick,
              -k_clapper_deck_mm / 2 - dw / 2 + jz, k_clapper_deck_mm / 2 + dw / 2 + jz, top[k]));
  }
  p.x_start = centre[0] - abut / 2;
  p.x_end = centre[spans] + abut / 2;
}

// A seat for a timber superstructure at x (a sill log across the path, on a stone abutment where
// the seat stands over the ground by more than the sill): returns the sill's index; the sill's top
// is `seat`.
i32 timber_seat(BridgePlan& p, Draws& d, const Crossing& c, i32 x, i32 sill, i32 length, i32 seat) {
  i32 below = -1;
  // A sill whose underside is in the ground under the whole of it lies there, sunk at most a third
  // of its diameter (the seat is set no lower: the specification's mud sills, buried); one whose
  // underside stands over any of the ground beneath it — a bank that rises across the sill —
  // lies on a stone abutment founded 200 mm under the lowest ground beneath the abutment. (G186:
  // the test was the highest ground about the sill, which on a rising bank left the low side of a
  // sill seated at the bank 35 mm in the air once the deck no longer stood a metre up.)
  const i32 sill_foot = seat - sill;
  const i32 along = sill + 300;
  if (sill_foot > ground_min(c, x - sill / 2, x + sill / 2))
    below = stone_stack(p, d, MemberKind::abutment, x, along, length + 200,
                        ground_min(c, x - along / 2 - 25, x + along / 2 + 25) - 200, sill_foot);
  return static_cast<i32>(
      add(p, block(MemberKind::sill, Shape::log_z, k_log, x - sill / 2, x + sill / 2, sill_foot,
                   seat, -length / 2, length / 2, below)));
}

void plan_timber(const Crossing& c, Draws& d, BridgePlan& p, bool foot_log) {
  const i32 half = static_cast<i32>(c.width_mm) / 2;
  const i32 sill = 300;
  const i32 s0 = -(half + k_setback_mm + sill / 2), s1 = half + k_setback_mm + sill / 2;
  p.span = (s1 - sill / 2) - (s0 + sill / 2);
  // **The underside by form and span** (bridges.md, "The deck's height"): a foot log, and
  // stringers to the minor span, `k_bank_clearance_mm` over the lower bank's top; stringers past
  // it over the flood stage (bankfull plus a bankfull depth, at least `k_min_freeboard_mm`). And
  // every sill on or in its ground.
  const i32 depth = std::max(p.bankfull, 1);
  const bool minor = foot_log || p.span <= k_minor_span_mm;
  const i32 over =
      minor ? p.bankfull + k_bank_clearance_mm : p.bankfull + std::max(k_min_freeboard_mm, depth);
  const i32 g0 = ground_max(c, s0 - sill, s0 + sill), g1 = ground_max(c, s1 - sill, s1 + sill);
  const i32 soffit = std::max({over, g0 + sill * 2 / 3, g1 + sill * 2 / 3});
  p.soffit = soffit;
  const i32 overhang = 250;
  if (foot_log) {
    const i32 dia = k_foot_log_mm;
    const i32 r = dia / 2;
    // Hewn to a flat 250 mm wide: the chord at half-width 125 stands sqrt(r^2 - 125^2) over the
    // log's axis, in whole millimetres (the integer square root, rounded down).
    i64 h = 0;
    while ((h + 1) * (h + 1) <= static_cast<i64>(r) * r - 125 * 125)
      ++h;
    const i32 hewn = soffit + r + static_cast<i32>(h);
    const i32 length = 1200;
    const i32 a = timber_seat(p, d, c, s0, sill, length, soffit);
    timber_seat(p, d, c, s1, sill, length, soffit);
    Member log = block(MemberKind::log, Shape::log_x, k_log, s0 - sill / 2 - overhang,
                       s1 + sill / 2 + overhang, soffit, soffit + dia, -r, r, a);
    log.hewn = hewn;
    add(p, log);
    p.deck = hewn;
    p.width = 250;
    p.x_start = log.x0;
    p.x_end = log.x1;
  } else {
    // The stringers' diameter by span: the TCV's 250 mm at about 5 m to 400 mm at about 10 m,
    // straight between and held at the ends.
    const i32 span = s1 - s0;
    const i32 dia = std::clamp(
        250 + static_cast<i32>(div_round(static_cast<i64>(span - 5000) * 150, 5000)), 250, 400);
    const i32 length = k_deck_mm + 400;
    const i32 a = timber_seat(p, d, c, s0, sill, length, soffit);
    timber_seat(p, d, c, s1, sill, length, soffit);
    // Two stringers under a 900 mm deck (the TCV), each hewn 50 mm for the planks' bearing.
    const i32 zc = k_deck_mm / 2 - dia / 2 - 25;
    const i32 hewn = soffit + dia - 50;
    i32 stringer = -1;
    for (i32 side = -1; side <= 1; side += 2) {
      Member s = block(MemberKind::stringer, Shape::log_x, k_log, s0 - sill / 2 - overhang / 2,
                       s1 + sill / 2 + overhang / 2, soffit, soffit + dia, side * zc - dia / 2,
                       side * zc + dia / 2, a);
      s.hewn = hewn;
      stringer = static_cast<i32>(add(p, s));
    }
    const i32 x0 = s0 - sill / 2 - overhang / 2, x1 = s1 + sill / 2 + overhang / 2;
    const i32 pitch = k_plank_mm + k_plank_gap_mm;
    const i32 planks = (x1 - x0 + k_plank_gap_mm) / pitch;
    const i32 start = x0 + ((x1 - x0) - (planks * pitch - k_plank_gap_mm)) / 2;
    for (i32 k = 0; k < planks; ++k) {
      const i32 px = start + k * pitch;
      const i32 jw = d.range(-6, 6);
      add(p, block(MemberKind::plank, Shape::box, k_plank, px, px + k_plank_mm + jw, hewn,
                   hewn + k_plank_thick_mm, -k_deck_mm / 2, k_deck_mm / 2, stringer));
    }
    p.deck = hewn + k_plank_thick_mm;
    p.width = k_deck_mm;
    p.x_start = start;
    p.x_end = start + planks * pitch - k_plank_gap_mm;
  }
  // A handrail on a plank deck that stands high over the bed: posts on the planks every 2 m or
  // less, a rail along their tops, both sides. A foot log has none: a post on its 250 mm flat
  // would take the walking surface, and two posts on its sills with a rail 5 m between them is no
  // rail (the first pictures).
  p.handrail = !foot_log && p.deck > k_rail_fall_mm;
  if (!p.handrail) return;
  const i32 post = 100;
  for (i32 side = 1; side >= -1; side -= 2) {
    const i32 x0 = p.x_start + post / 2, x1 = p.x_end - post / 2;
    const i32 zc = side * (k_deck_mm / 2 - post / 2);
    const i32 bays = std::max<i32>(1, (x1 - x0 + 1999) / 2000);
    i32 first_post = -1;
    for (i32 k = 0; k <= bays; ++k) {
      const i32 x = x0 + static_cast<i32>(div_round(static_cast<i64>(x1 - x0) * k, bays));
      // The plank under the post's middle; over a gap, the plank before it.
      i32 on = -1;
      for (u32 m = 0; m < p.members.size(); ++m) {
        const Member& q = p.members[m];
        if (q.kind == MemberKind::plank && q.x0 <= x) on = static_cast<i32>(m);
      }
      if (on < 0) continue;
      const Member& plank = p.members[static_cast<u32>(on)];
      const i32 at = std::clamp(x, plank.x0 + post / 2, plank.x1 - post / 2);
      const i32 idx = static_cast<i32>(
          add(p, block(MemberKind::post, Shape::box, k_plank, at - post / 2, at + post / 2, p.deck,
                       p.deck + k_rail_mm, zc - post / 2, zc + post / 2, on)));
      if (first_post < 0) first_post = idx;
    }
    if (first_post >= 0)
      add(p, block(MemberKind::rail, Shape::box, k_plank, x0 - post / 2, x1 + post / 2,
                   p.deck + k_rail_mm, p.deck + k_rail_mm + 60, zc - 40, zc + 40, first_post));
  }
}

// **The approaches' steps** (bridges.md, "The ground and the neighbours"): from each end of a deck
// that stands over the ground there by more than a riser, rough stones on the ground out along
// the path, the risers equal and at most `k_riser_mm`, each stone `k_step_going_mm` along the
// path, as wide as the tread or the deck (to 1.2 m) and bedded `k_step_bed_mm` under the lowest
// ground beneath it; at most `k_max_steps` an end, and none past where the ground already stands
// at its height. They stand on the ground the crossing was sampled on — the channel's cut in it —
// so nothing the cut lowers is under them, and a fill (`approach_of`) is left only past the last.
void plan_steps(const Crossing& c, Draws& d, BridgePlan& p) {
  const i32 across = std::max<i32>(static_cast<i32>(c.tread_mm), std::min(p.width, 1200));
  for (u32 end = 0; end < 2; ++end) {
    const i32 out = end == 0 ? -1 : 1;
    const i32 x_end = end == 0 ? p.x_start : p.x_end;
    p.approach_x[end] = x_end;
    p.approach_top[end] = p.deck;
    const i32 h = p.deck - ground_rel(c, x_end);
    if (h <= k_riser_mm) continue;  // one riser: the walker's own step onto the deck
    const i32 risers = (h + k_riser_mm - 1) / k_riser_mm;
    const bool all = static_cast<u32>(risers - 1) <= k_max_steps;
    const i32 rise = all ? static_cast<i32>(div_round(h, risers)) : k_riser_mm;
    const u32 blocks = all ? static_cast<u32>(risers - 1) : k_max_steps;
    for (u32 k = 1; k <= blocks; ++k) {
      const i32 top = p.deck - static_cast<i32>(k) * rise;
      const i32 inner = x_end + out * static_cast<i32>(k - 1) * k_step_going_mm;
      const i32 outer = x_end + out * static_cast<i32>(k) * k_step_going_mm;
      const i32 x0 = std::min(inner, outer), x1 = std::max(inner, outer);
      if (top <= ground_max(c, x0, x1)) break;  // the ground is there already
      const i32 jz = d.range(-30, 30), dw = d.range(-60, 0);
      add(p, block(MemberKind::step, Shape::rough, k_stone, x0, x1,
                   ground_min(c, x0, x1) - k_step_bed_mm, top, -(across + dw) / 2 + jz,
                   (across + dw) / 2 + jz, -1));
      ++p.steps[end];
      p.approach_x[end] = outer;
      p.approach_top[end] = top - rise;
    }
  }
}

}  // namespace

bool crossing_in_range(const Crossing& c, std::string& why) {
  if (c.width_mm < 300 || c.width_mm > 20000) {
    why = "width_mm must be 300..20000";
    return false;
  }
  if (c.tread_mm < 300 || c.tread_mm > 3000) {
    why = "tread_mm must be 300..3000";
    return false;
  }
  if (c.water_mm <= c.bed_mm) {
    why = "water_mm must be above bed_mm";
    return false;
  }
  if (c.dir_x == 0 && c.dir_z == 0) {
    why = "dir_x and dir_z must name a direction";
    return false;
  }
  if (c.profile_step_mm < 50 || c.profile_step_mm > 2000) {
    why = "profile_step_mm must be 50..2000";
    return false;
  }
  const u32 n = c.profile_mm.size();
  if (n < 3 || n % 2 == 0) {
    why = "profile_mm must hold an odd number of points, at least three";
    return false;
  }
  const i64 reach = static_cast<i64>((n - 1) / 2) * c.profile_step_mm;
  if (reach < static_cast<i64>(c.width_mm) / 2 + 4000) {
    why = "profile_mm must reach 4 m past the bankfull width each side";
    return false;
  }
  return true;
}

i64 profile_at(const Crossing& c, i64 x) noexcept {
  const u32 n = c.profile_mm.size();
  if (n == 0) return c.bed_mm;
  const i64 s = c.profile_step_mm > 0 ? static_cast<i64>(c.profile_step_mm) : 1;
  const i64 t = x + static_cast<i64>((n - 1) / 2) * s;
  if (t <= 0) return c.profile_mm[0];
  const i64 last = static_cast<i64>(n - 1) * s;
  if (t >= last) return c.profile_mm[n - 1];
  const u32 i = static_cast<u32>(t / s);
  const i64 f = t - static_cast<i64>(i) * s;
  return c.profile_mm[i] + div_round((c.profile_mm[i + 1] - c.profile_mm[i]) * f, s);
}

i32 bankfull_depth_mm(const Crossing& c) noexcept {
  const i64 half = static_cast<i64>(c.width_mm) / 2;
  const i64 top = std::min(profile_at(c, -half), profile_at(c, half));
  return static_cast<i32>(std::max<i64>(top - c.bed_mm, 0));
}

BridgeForm choose_form(const Crossing& c) noexcept {
  const i32 w = static_cast<i32>(c.width_mm);
  const i32 depth = bankfull_depth_mm(c);
  if (depth <= k_stones_max_depth_mm && w <= k_stones_max_width_mm) return BridgeForm::stones;
  if (static_cast<i32>(c.d84_mm) >= k_clapper_min_d84_mm && w <= k_clapper_max_width_mm)
    return BridgeForm::clapper;
  const i32 span = w + 2 * k_setback_mm;
  if (span <= k_foot_log_max_span_mm) return BridgeForm::log;
  if (span <= k_stringers_max_span_mm) return BridgeForm::stringers;
  return BridgeForm::none;
}

u64 crossing_seed(u64 layer_seed, const Crossing& c) noexcept {
  u64 h = hash_combine(layer_seed, c.route);
  h = hash_combine(h, c.line);
  return hash_combine(h, c.station);
}

bool plan_bridge(const Crossing& c, u64 seed, BridgePlan& p, std::string& why) {
  p = {};
  if (!crossing_in_range(c, why)) return false;
  p.seed = seed;
  p.form = choose_form(c);
  p.water = static_cast<i32>(c.water_mm - c.bed_mm);
  p.bankfull = bankfull_depth_mm(c);
  Draws d{seed};
  switch (p.form) {
    case BridgeForm::stones: plan_stones(c, d, p); break;
    case BridgeForm::clapper: plan_clapper(c, d, p); break;
    case BridgeForm::log: plan_timber(c, d, p, true); break;
    case BridgeForm::stringers: plan_timber(c, d, p, false); break;
    case BridgeForm::none: break;
  }
  if (p.form == BridgeForm::clapper || p.form == BridgeForm::log || p.form == BridgeForm::stringers)
    plan_steps(c, d, p);
  return true;
}

}  // namespace engine::bridges
