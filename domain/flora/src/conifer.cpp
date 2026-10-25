// The conifer's solver (trees.h; docs/subsystems/flora.md, "The conifer"; the grounding is
// docs/experiments/conifer-shoots.md).
#include "kit.h"

#include <domain/flora/trees.h>
#include <domain/geometry/cluster.h>

#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

// The streams. `trunk` and `whorls` draw exactly what they drew before the laterals, the
// interwhorl branches and the branches' fates existed, so a whorl branch stands where it stood;
// `interwhorl` grows the branches between the whorls; `fate` is drawn a fixed number of times per
// whorl and per branch slot; and every slot's laterals have a stream of their own
// (`k_lateral_streams` + the slot) — so a missing branch moves nothing else.
enum class Stream : u64 { trunk = 21, whorls = 22, fate = 23, interwhorl = 24 };
constexpr u64 k_lateral_streams = 4096;

constexpr u32 k_fine_rows = 12;      // a branch's structure, whatever the budget
constexpr u32 k_lateral_rows = 4;    // a lateral's
constexpr f64 k_lateral_card = 0.8;  // a lateral's cards against the branch's own

struct Lateral {
  Axis axis;
  f64 attach = 0.0;  // where it leaves its branch, metres along the branch
  u32 cards = 0;     // cards a side at full detail
  D3 side;           // its spray's lateral: the branch's direction, so a hanging one is a curtain
};

struct Branch {
  Axis axis;
  f64 r0 = 0.0;
  f64 r1 = 0.0;
  bool dead = false;       // bare wood: a dead branch of the self-pruned zone, or a broken stub
  f64 needled_from = 0.0;  // where its needles start, metres along it
  u32 cards = 0;           // its own spray's cards a side at full detail
  f64 lift = 0.0;          // its cards' lift out of the spray's plane: flat shade, bushy sun shoots
  Vector<Lateral> laterals;
};

struct Structure {
  Axis trunk;
  f64 trunk_r0 = 0.0;
  Vector<Branch> branches;
  u32 max_cards = 1;      // the most cards a side any spray has at full detail
  f64 max_lateral = 0.0;  // the longest lateral
};

// The whorl's share of the crown, 0 at the lowest and 1 at the top, and how many branches it has:
// fewer up the crown by `thinning`.
u32 whorl_branches(const Conifer& p, f64 t) {
  const f64 n = static_cast<f64>(p.branches_per_whorl) * (1.0 - static_cast<f64>(p.thinning) * t);
  const u32 c = static_cast<u32>(n + 0.5);
  return c < 2 ? 2u : c;
}

// The thickest branch at the crown's foot, radius in metres: Colin and Houllier's (1992) largest
// branch of a whorl, 15.9 mm + 0.978 mm per cm of breast-height diameter, the trunk's breast-height
// diameter taken as 0.85 of its diameter at the ground.
f64 thickest_branch(const Conifer& p) {
  const f64 dbh_cm = 2.0 * static_cast<f64>(p.trunk_radius_m) * 100.0 * 0.85;
  return (15.9 + 0.978 * dbh_cm) / 2000.0;
}

// The needled part of a branch is its outer `needle_years` of growth.
f64 needled_reach(const Conifer& p) {
  return static_cast<f64>(p.needle_years) * static_cast<f64>(p.shoot_growth_m);
}

// One branch's laterals: alternate sides along its outer two needled reaches (a lateral older
// than that has died in the crown's shade and been shed), each as long as its age allows —
// `lateral_length_m` once it is a needled reach from the tip, 0.3 of it at the tip, never past 0.4
// of the branch — splayed forward from the branch in its level plane, dipped and turned down
// towards the ground by `pendulous`. A lateral is needled whole: its own outer years.
void grow_laterals(const Conifer& p, Branch& b, f64 step, u64 slot) {
  const f64 reach = needled_reach(p);
  const f64 inner = b.axis.length - 2.0 * reach;
  const f64 off_trunk = 0.12 * b.axis.length;
  const f64 from = inner > off_trunk ? inner : off_trunk;
  const f64 span = b.axis.length - from;
  const f64 per_m = static_cast<f64>(p.laterals_per_m);
  const u32 count = static_cast<u32>(span * per_m + 0.5);
  if (count == 0) return;
  Rng rng = stream_of(p.seed, k_lateral_streams + slot);
  const f64 longest = static_cast<f64>(p.lateral_length_m);
  const f64 cap = 0.4 * b.axis.length;
  const f64 pendulous = static_cast<f64>(p.pendulous);
  for (u32 k = 0; k < count; ++k) {
    // Every lateral draws the same three numbers and its axis's, kept or not.
    const f64 jitter_at = rng.symmetric();
    const f64 jitter_angle = rng.symmetric();
    const f64 jitter_length = rng.symmetric();
    const f64 attach =
        from + span * (static_cast<f64>(k) + 0.5 + 0.3 * jitter_at) / static_cast<f64>(count);
    const f64 age = clamp01((b.axis.length - attach) / reach);
    f64 length = longest * (0.3 + 0.7 * age) * (1.0 + 0.15 * jitter_length);
    if (length > cap) length = cap;
    const D3 t = axis_tangent(b.axis, attach);
    D3 level = cross(t, k_up);
    if (dot(level, level) < 1e-6) level = D3{1.0, 0.0, 0.0};
    level = normalized(level);
    const f64 sign = (k & 1u) == 0u ? 1.0 : -1.0;
    const f64 angle = (55.0 + 8.0 * jitter_angle) * k_degree;
    const D3 dir = normalized(t * det::cos(angle) + level * (sign * det::sin(angle)) -
                              k_up * (0.35 * pendulous));
    Lateral lat;
    const f64 pull = length > 0.0 ? pendulous * 3.0 / length : 0.0;
    lat.axis = grow_axis(axis_point(b.axis, attach), dir, length, k_lateral_rows, k_up * -1.0, pull,
                         3.0 * k_degree, rng);
    if (length < 0.5 * step) continue;  // younger than a card: the branch's own spray holds it
    lat.attach = attach;
    const u32 cards = static_cast<u32>(length / step + 0.5);
    lat.cards = cards < 1 ? 1u : cards;
    // The spray's lateral: the branch's direction, signed so the cards lift up, not down.
    const D3 lateral0 = perpendicular(lat.axis.tangent[0], t);
    lat.side = cross(lateral0, lat.axis.tangent[0]).y >= 0.0 ? t : t * -1.0;
    b.laterals.push_back(std::move(lat));
  }
}

Structure grow(const Conifer& p, const GrowthRules& rules) {
  Structure s;
  const f64 height = static_cast<f64>(p.height_m);
  Rng trunk_rng = stream_of(p.seed, static_cast<u64>(Stream::trunk));
  const f64 lean = static_cast<f64>(p.lean_deg) * k_degree;
  const f64 azimuth = trunk_rng.unit() * 2.0 * k_pi;
  const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
  // A conifer's leader is straight: no wander, only the lean taken back towards the vertical.
  s.trunk =
      grow_axis(D3{}, k_up * det::cos(lean) + heading * det::sin(lean), height / det::cos(lean), 32,
                k_up, 0.5 / height, rules.irregular_stations ? 0.018 : 0.0, trunk_rng);
  s.trunk_r0 = static_cast<f64>(p.trunk_radius_m);

  Rng rng = stream_of(p.seed, static_cast<u64>(Stream::whorls));
  Rng between = stream_of(p.seed, static_cast<u64>(Stream::interwhorl));
  Rng fate = stream_of(p.seed, static_cast<u64>(Stream::fate));
  const f64 reach = static_cast<f64>(p.crown_radius_m);
  const f64 base = static_cast<f64>(p.crown_base);
  const f64 upturn = static_cast<f64>(p.upturn) * 40.0 * k_degree;
  const f64 r_max = thickest_branch(p);
  const f64 needled = needled_reach(p);
  // One card spacing for every spray: the longest branch's needled part over `needle_cards`.
  const f64 longest_needled = needled < reach ? needled : reach;
  const f64 step = longest_needled / static_cast<f64>(p.needle_cards);
  const f64 missing = static_cast<f64>(p.missing_branches);
  const f64 sparse_chance = static_cast<f64>(p.sparse_whorls);
  const f64 dead_share = static_cast<f64>(p.dead_share);
  const auto whorl_arc = [&](f64 t) { return s.trunk.length * (base + (0.97 - base) * t); };
  // The crown's cone: the lowest branches reach `crown_radius_m`, the top ones a tenth of it.
  const auto whorl_length = [&](f64 t) {
    return reach * lerp(1.0, 0.1, det::exp(det::log(t + 1e-9) * rules.crown_power));
  };
  const auto whorl_angle = [&](f64 t) {
    return lerp(static_cast<f64>(p.bottom_angle_deg), static_cast<f64>(p.top_angle_deg), t) *
           k_degree;
  };
  u64 slot = 0;
  // One branch grown at `arc` up the trunk, `t` up the crown: its fate drawn (four draws a slot,
  // whatever it decides), then its wood, its needled part and its laterals.
  const auto add = [&](Axis axis, f64 len, f64 r_here, f64 t, bool sparse) {
    const u64 here = slot++;
    const f64 u_missing = fate.unit();
    const f64 u_stub = fate.unit();
    const f64 u_sparse = fate.unit();
    const f64 u_break = fate.unit();
    const bool gone = u_missing < missing || (sparse && u_sparse < 0.5);
    if (gone && u_stub >= 0.5) return;  // shed whole: no part, no triangles
    Branch br;
    br.axis = std::move(axis);
    // The pipe of the whorl: the thickest branch at the crown's foot, thinner as they shorten.
    br.r0 = r_max * det::exp(det::log(len / reach + 1e-9) * 0.8);
    if (br.r0 > 0.5 * r_here) br.r0 = 0.5 * r_here;
    if (p.trunk_radius_m < .03f) {
      br.r0 = std::min(br.r0, .5 * r_here);
      br.r1 = std::min(.004, .65 * br.r0);
    } else {
      if (br.r0 < 0.006) br.r0 = 0.006;
      br.r1 = 0.004;
    }
    const f64 supported = supported_length(br.axis.length, br.r0, rules);
    if (supported < br.axis.length) {
      const f64 scale = supported / br.axis.length;
      const D3 origin = br.axis.point[0];
      for (u32 i = 0; i < br.axis.point.size(); ++i) {
        br.axis.point[i] = origin + (br.axis.point[i] - origin) * scale;
        br.axis.arc[i] *= scale;
      }
      br.axis.length = supported;
    }
    if (gone || t < dead_share) {
      // A broken stub keeps one row of its twelve; a dead branch of the self-pruned zone 35 to
      // 90 % of its length, broken where it broke.
      br.dead = true;
      const f64 kept = static_cast<f64>(k_fine_rows) * (0.35 + 0.55 * u_break) + 0.5;
      br.axis = truncate_axis(br.axis, gone ? 1u : static_cast<u32>(kept));
      br.r0 *= 0.8;
      br.r1 = 0.006 < br.r0 ? 0.006 : br.r0 * 0.7;
      s.branches.push_back(std::move(br));
      return;
    }
    // The needles sit on the outer years; the inner part of a long branch is bare.
    const f64 from = br.axis.length - needled;
    const f64 off_trunk = 0.08 * br.axis.length;
    br.needled_from = from > off_trunk ? from : off_trunk;
    const u32 cards = static_cast<u32>((br.axis.length - br.needled_from) / step + 0.5);
    br.cards = cards < 2 ? 2u : cards;
    if (br.cards > s.max_cards) s.max_cards = br.cards;
    // Shade shoots are flat and sun shoots bushy (Stenberg et al. 1999): the cards lift 10
    // degrees out of the spray's plane at the crown's foot, 35 at its top.
    br.lift = lerp(10.0, 35.0, t) * k_degree * rules.spray_lift;
    grow_laterals(p, br, step, here);
    for (const Lateral& l : br.laterals) {
      if (l.cards > s.max_cards) s.max_cards = l.cards;
      if (l.axis.length > s.max_lateral) s.max_lateral = l.axis.length;
    }
    s.branches.push_back(std::move(br));
  };
  for (u32 w = 0; w < p.whorls; ++w) {
    const f64 t = static_cast<f64>(w) / static_cast<f64>(p.whorls - 1);
    const f64 arc = whorl_arc(t);
    const u32 count = whorl_branches(p, t);
    const f64 turn = rng.unit() * 2.0 * k_pi;
    const bool sparse = fate.unit() < sparse_chance;
    const f64 length = whorl_length(t);
    const f64 angle_t = whorl_angle(t);
    const D3 tangent = axis_tangent(s.trunk, arc);
    const D3 normal = perpendicular(tangent, s.trunk.normal[0]);
    const D3 binormal = cross(tangent, normal);
    const f64 r_here = lerp(s.trunk_r0, std::min(.01, s.trunk_r0 * .3), arc / s.trunk.length);
    for (u32 b = 0; b < count; ++b) {
      const f64 a = turn + 2.0 * k_pi * static_cast<f64>(b) / static_cast<f64>(count) +
                    0.15 * rng.symmetric();
      const D3 radial = normal * det::cos(a) + binormal * det::sin(a);
      const f64 angle = angle_t * (1.0 + 0.05 * rng.symmetric());
      const D3 dir = normalized(tangent * det::cos(angle) + radial * det::sin(angle));
      const f64 len = length * (1.0 + 0.1 * rng.symmetric());
      const f64 station = rules.irregular_stations
                              ? arc + (static_cast<f64>(b) / static_cast<f64>(count) - 0.5) *
                                          s.trunk.length * (0.97 - base) /
                                          static_cast<f64>(p.whorls)
                              : arc;
      Axis axis = grow_axis(axis_point(s.trunk, station) + radial * (0.5 * r_here), dir, len,
                            k_fine_rows, k_up, upturn / len, 2.0 * k_degree, rng);
      add(std::move(axis), len, r_here, t, sparse);
    }
    // The interwhorl branches up to the next whorl: shorter and thinner, at a seeded azimuth
    // each, as steep as the crown is where they stand.
    if (w + 1 == p.whorls) continue;
    for (u32 k = 1; k <= p.interwhorl; ++k) {
      const f64 tk = t + (static_cast<f64>(k) / static_cast<f64>(p.interwhorl + 1)) /
                             static_cast<f64>(p.whorls - 1);
      const f64 arc_k = whorl_arc(tk);
      const D3 tangent_k = axis_tangent(s.trunk, arc_k);
      const D3 normal_k = perpendicular(tangent_k, s.trunk.normal[0]);
      const D3 binormal_k = cross(tangent_k, normal_k);
      const f64 r_k = lerp(s.trunk_r0, std::min(.01, s.trunk_r0 * .3), arc_k / s.trunk.length);
      const f64 a = between.unit() * 2.0 * k_pi;
      const D3 radial = normal_k * det::cos(a) + binormal_k * det::sin(a);
      const f64 angle = whorl_angle(tk) * (1.0 + 0.05 * between.symmetric());
      const D3 dir = normalized(tangent_k * det::cos(angle) + radial * det::sin(angle));
      const f64 len = whorl_length(tk) * (0.45 + 0.15 * between.symmetric());
      Axis axis = grow_axis(axis_point(s.trunk, arc_k) + radial * (0.5 * r_k), dir, len,
                            k_fine_rows, k_up, upturn / len, 2.0 * k_degree, between);
      add(std::move(axis), len, r_k, tk, false);
    }
  }
  return s;
}

struct Plan {
  u32 trunk_sides = 10;
  u32 trunk_rows = 24;
  u32 branch_sides = 4;
  u32 branch_rows = 4;
  bool branch_tubes = true;  // a live branch's wood; a dead one's is always drawn
  u32 card_divisor = 1;      // a spray's cards over this, at least 1
  bool merge_laterals =
      false;  // the laterals folded into their branch's own spray, its cards wider
};

u32 cards_of(u32 full, const Plan& plan) {
  const u32 c = (full + plan.card_divisor - 1) / plan.card_divisor;
  return c < 1 ? 1u : c;
}

// A dead branch has nothing else to stand for it, so it keeps a tube on every rung.
u32 dead_rows(const Plan& plan) { return plan.branch_tubes ? plan.branch_rows : 1u; }
u32 dead_sides(const Plan& plan) { return plan.branch_tubes ? plan.branch_sides : 3u; }

u32 count_triangles(const Conifer& p, const Structure& s, const Plan& plan) {
  u32 total = tube_triangles(plan.trunk_rows, plan.trunk_sides) +
              spray_triangles(cards_of(p.needle_cards, plan));  // the leader's tuft
  for (const Branch& b : s.branches) {
    if (b.dead) {
      total += tube_triangles(dead_rows(plan), dead_sides(plan));
      continue;
    }
    if (plan.branch_tubes) total += tube_triangles(plan.branch_rows, plan.branch_sides);
    total += spray_triangles(cards_of(b.cards, plan));
    if (!plan.merge_laterals) {
      for (const Lateral& l : b.laterals)
        total += spray_triangles(cards_of(l.cards, plan));
    }
  }
  return total;
}

// The ladder: the branches' rows and sides, the trunk's, then fewer and wider cards on every
// spray, then the laterals folded into their branch's spray (its cards as long as the laterals),
// then the branches' woody tubes last (their sprays carry them). No rung drops a branch, a dead one
// included.
bool plan_conifer(const Conifer& p, const Structure& s, Plan& plan, u32& steps) {
  plan = Plan{};
  const u32 rows = static_cast<u32>(static_cast<f64>(p.height_m) / 0.5);
  plan.trunk_rows = rows < 6 ? 6 : (rows > 40 ? 40 : rows);
  steps = 0;
  const auto fits = [&] { return count_triangles(p, s, plan) <= p.triangle_budget; };
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  if (step([&] { plan.branch_rows = 3; })) return true;
  if (step([&] { plan.branch_rows = 2; })) return true;
  if (step([&] { plan.trunk_sides = 8; })) return true;
  if (step([&] { plan.branch_sides = 3; })) return true;
  if (step([&] { plan.trunk_rows = plan.trunk_rows / 2 < 4 ? 4 : plan.trunk_rows / 2; }))
    return true;
  const u32 widest = s.max_cards > p.needle_cards ? s.max_cards : p.needle_cards;
  while (cards_of(widest, plan) > 1) {
    if (step([&] { plan.card_divisor *= 2; })) return true;
  }
  if (step([&] { plan.merge_laterals = true; })) return true;
  if (step([&] { plan.branch_rows = 1; })) return true;
  if (step([&] { plan.trunk_sides = 6; })) return true;
  return step([&] { plan.branch_tubes = false; });
}

}  // namespace

bool conifer_in_range(const Conifer& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Conifer.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.height_m >= .35f && p.height_m <= 60.0f)) return bad("height_m", "0.35 to 60 m");
  if (!(p.trunk_radius_m >= .003f && p.trunk_radius_m <= 2.0f))
    return bad("trunk_radius_m", "0.003 to 2 m");
  if (!(p.crown_base >= 0.0f && p.crown_base <= 0.8f)) return bad("crown_base", "0 to 0.8");
  if (!(p.lean_deg >= 0.0f && p.lean_deg <= 20.0f)) return bad("lean_deg", "0 to 20 degrees");
  if (!(p.whorls >= 2u && p.whorls <= 60u)) return bad("whorls", "2 to 60");
  if (!(p.branches_per_whorl >= 2u && p.branches_per_whorl <= 10u))
    return bad("branches_per_whorl", "2 to 10");
  if (!(p.thinning >= 0.0f && p.thinning <= 0.8f)) return bad("thinning", "0 to 0.8");
  if (!(p.crown_radius_m >= .08f && p.crown_radius_m <= 16.0f))
    return bad("crown_radius_m", "0.08 to 16 m");
  if (!(p.bottom_angle_deg >= 60.0f && p.bottom_angle_deg <= 130.0f))
    return bad("bottom_angle_deg", "60 to 130 degrees");
  if (!(p.top_angle_deg >= 20.0f && p.top_angle_deg <= 100.0f))
    return bad("top_angle_deg", "20 to 100 degrees");
  if (!(p.upturn >= 0.0f && p.upturn <= 1.0f)) return bad("upturn", "0 to 1");
  if (!(p.needle_cards >= 2u && p.needle_cards <= 60u)) return bad("needle_cards", "2 to 60");
  if (!(p.needle_length_m >= 0.03f && p.needle_length_m <= 1.0f))
    return bad("needle_length_m", "0.03 to 1 m");
  if (!(p.triangle_budget >= 8000u && p.triangle_budget <= 2000000u))
    return bad("triangle_budget", "8,000 to 2,000,000");
  if (!(p.needle_years >= 2.0f && p.needle_years <= 15.0f))
    return bad("needle_years", "2 to 15 years");
  if (!(p.shoot_growth_m >= 0.02f && p.shoot_growth_m <= 0.6f))
    return bad("shoot_growth_m", "0.02 to 0.6 m");
  if (p.interwhorl > 6u) return bad("interwhorl", "0 to 6");
  if (!(p.laterals_per_m >= 0.0f && p.laterals_per_m <= 20.0f))
    return bad("laterals_per_m", "0 to 20");
  if (!(p.lateral_length_m >= 0.05f && p.lateral_length_m <= 2.5f))
    return bad("lateral_length_m", "0.05 to 2.5 m");
  if (!(p.pendulous >= 0.0f && p.pendulous <= 1.0f)) return bad("pendulous", "0 to 1");
  if (!(p.missing_branches >= 0.0f && p.missing_branches <= 0.5f))
    return bad("missing_branches", "0 to 0.5");
  if (!(p.sparse_whorls >= 0.0f && p.sparse_whorls <= 0.5f))
    return bad("sparse_whorls", "0 to 0.5");
  if (!(p.dead_share >= 0.0f && p.dead_share <= 0.9f)) return bad("dead_share", "0 to 0.9");
  return true;
}

bool solve_conifer(const Conifer& p, assets::MeshData& out, TreeReport* report, std::string* error,
                   Motion* motion) {
  return detail::solve_conifer_rules(p, GrowthRules{}, out, report, error, motion);
}

bool detail::solve_conifer_rules(const Conifer& p, const GrowthRules& rules, assets::MeshData& out,
                                 TreeReport* report, std::string* error, Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!conifer_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  const Structure s = grow(p, rules);
  Plan plan;
  u32 steps = 0;
  if (!plan_conifer(p, s, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Conifer.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(p, s, plan)) + " triangles";
    }
    return false;
  }

  out.materials.push_back(material("bark", Vec4{0.26f, 0.18f, 0.12f, 1.0f}, 0.9f, false));
  out.materials.push_back(material("needles", Vec4{0.05f, 0.14f, 0.06f, 1.0f}, 0.7f, true));
  Motion parts;
  Emitter emit{out, parts};
  emit.wood = rules.wood;
  emit.wood.seed = p.seed;

  // The bark: the trunk (part 0) tapering to the leader's tip, then every branch's woody shoot,
  // each a part of its own hanging from the trunk; a dead branch's on every rung.
  emit.begin_part(s.trunk.point[0], s.trunk.tangent[0], s.trunk.length,
                  geometry::k_motion_no_parent);
  build_tube(resample_axis(s.trunk, plan.trunk_rows), s.trunk_r0, std::min(.01, s.trunk_r0 * .3),
             plan.trunk_sides, Vec2{0.0f, 0.0f}, emit);
  Vector<u32> part_of(s.branches.size());
  u32 laterals = 0;
  for (u32 i = 0; i < s.branches.size(); ++i) {
    const Branch& b = s.branches[i];
    part_of[i] = emit.begin_part(b.axis.point[0], b.axis.tangent[0], b.axis.length, k_trunk_part);
    if (b.dead) {
      build_tube(resample_axis(b.axis, dead_rows(plan)), b.r0, b.r1, dead_sides(plan),
                 Vec2{0.0f, 0.0f}, emit);
    } else if (plan.branch_tubes) {
      build_tube(resample_axis(b.axis, plan.branch_rows), b.r0, b.r1, plan.branch_sides,
                 Vec2{0.0f, 0.0f}, emit);
    }
    laterals += b.laterals.size();
  }
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  // The needles: on every live branch a spray along its needled years, laid level and splayed
  // forward, and a spray on every lateral — the laterals' vertices weighted by their arc from the
  // branch's base, so they ride their branch's part; the leader's tuft on the trunk's top tenth.
  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  const f64 needle = static_cast<f64>(p.needle_length_m);
  const f64 lateral_card = k_lateral_card * needle;
  const f64 merged_card = s.max_lateral > needle ? s.max_lateral : needle;
  const f64 pendulous = static_cast<f64>(p.pendulous);
  u32 sprays = 1;
  u32 cards_built = 0;
  for (u32 i = 0; i < s.branches.size(); ++i) {
    const Branch& b = s.branches[i];
    if (b.dead) continue;
    emit.part = part_of[i];
    D3 side = cross(b.axis.tangent[0], k_up);
    if (dot(side, side) < 1e-6) side = D3{1.0, 0.0, 0.0};
    const f64 from = plan.branch_tubes ? b.needled_from : 0.0;
    const u32 own = cards_of(b.cards, plan);
    build_spray(b.axis, from, own, plan.merge_laterals ? merged_card : needle, 55.0 * k_degree,
                b.lift, 0.1, normalized(side), emit);
    ++sprays;
    cards_built += 2 * own;
    if (plan.merge_laterals) continue;
    for (const Lateral& l : b.laterals) {
      const u32 first = out.positions.size();
      const u32 cards = cards_of(l.cards, plan);
      build_spray(l.axis, 0.0, cards, lateral_card, 50.0 * k_degree, b.lift, 0.15 + 0.2 * pendulous,
                  l.side, emit);
      for (u32 v = first; v < out.positions.size(); ++v) {
        const f64 along = static_cast<f64>(parts.vertex_weight[v]) * l.axis.length;
        parts.vertex_weight[v] = static_cast<f32>(clamp01((l.attach + along) / b.axis.length));
      }
      ++sprays;
      cards_built += 2 * cards;
    }
  }
  emit.part = k_trunk_part;
  const u32 tuft = cards_of(p.needle_cards, plan);
  build_spray(s.trunk, 0.9 * s.trunk.length, tuft, 0.6 * needle, 30.0 * k_degree, 0.0, 0.0,
              D3{1.0, 0.0, 0.0}, emit);
  cards_built += 2 * tuft;
  assets::Primitive needles;
  needles.first_index = leaf_first;
  needles.index_count = out.indices.size() - leaf_first;
  needles.material = 1;
  out.primitives.push_back(needles);
  {
    Vector<Vec3> normals;
    const std::span<const u32> range(out.indices.data() + leaf_first, needles.index_count);
    geometry::compute_vertex_normals(out.positions, range, normals);
    for (u32 v = leaf_vertex; v < out.positions.size(); ++v)
      out.normals[v] = normals[v];
  }
  if (report != nullptr) {
    *report = TreeReport{};
    report->triangles = out.indices.size() / 3;
    report->wood_triangles = bark.index_count / 3;
    report->leaf_triangles = needles.index_count / 3;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->shoots = 1 + s.branches.size() + laterals;
    report->sprays = sprays;
    report->cards_per_side = cards_of(p.needle_cards, plan);
    report->trunk_sides = plan.trunk_sides;
    report->shoot_sides = plan.branch_tubes ? plan.branch_sides : 0u;
    report->steps = steps;
    report->leaf_cards = cards_built;
    report->leaf_area_m2 = static_cast<f32>(triangles_area(out, leaf_first));
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora
