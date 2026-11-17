// The spray kind (docs/subsystems/matgen.md, "Spray sets"; the sources and the measurements,
// docs/experiments/conifer-crown-coverage.md): one conifer branchlet an image, for a card that
// takes the image whole — a twig from the middle of the top row (the card's base) down the image,
// its side twigs, and their needles, with the spray's outline in the field's alpha.
//
// **A spray has structure, not mass.** A spruce's needles stand all round each twig, so a twig
// with its needles is a bottle brush three to four centimetres across; the twigs leave their
// branchlet one after another, to one side and the other, and the gaps between them are open. A
// pine carries tufts of paired needles at the ends of bare shoots. The image is drawn that way: a
// brush a twig, the card's background between the brushes, a ragged outline at every scale. The
// first sprays (the second leg of G203) filled the card to 53 to 57 % and read in the engine as
// flat dark plates; what was wanted was the same needles, thicker, not a card filled in.
//
// **A needle is drawn as the eye sees it on its shoot.** Each has an azimuth round its shoot and
// an angle to it and is drawn as that projection: one that points across the view shows its whole
// length, one that points at the eye a stub over the twig; the ones behind the shoot first and a
// little darker, the wood, then the ones in front.
//
// **The image is drawn for the card it lies on.** A conifer's card is not a square: it is as wide
// as the cards stand apart along their branch (`card_width_mm`), as long as `tile_mm`, and leans
// forward from the branch by `card_angle_deg`, a parallelogram. Everything here is laid out in
// millimetres on that card (across it and along it) and written through the card's map to texels,
// so a twig is as wide in the world as its recipe says, whatever the card's shape, and a needle's
// angle to its twig is that angle on the tree. With no width given the card is the square the tile
// is and the map is the identity.
//
// **Nothing here is narrower than four texels**, measured along whichever image axis shows the
// stroke's width best: the encoder and the first mips take a thinner strand apart (texture.md,
// "Alpha-tested masks"). A needle is drawn at its recipe's width or that, whichever is more, and
// stands for the needles beside it that the image has no texels for.
//
// The spray lives on the torus like every set, though it never reaches the image's edge: its
// strokes are placed from the window-free torus coordinates and wrapped, so a build from another
// window is the same bytes rotated.
#include "field.h"

#include <algorithm>

namespace engine::matgen::detail {

namespace {

constexpr f64 k_min_radius = 2.0;  // texels: no stroke narrower than four

// The card under the image: texel offsets to millimetres across the card's own axis (q) and along
// it (p), and back. The image's x runs along the branch the card stands on and its y along the
// card; the card leans from the branch by the angle whose sine and cosine these are.
struct Card {
  f64 u_mm = 1;  // millimetres a texel along the image's x (along the branch)
  f64 v_mm = 1;  // millimetres a texel down the image (along the card)
  f64 sin_a = 1, cos_a = 0;
  f64 q_of(f64 ox) const noexcept { return ox * u_mm * sin_a; }
  f64 p_of(f64 ox, f64 oy) const noexcept { return oy * v_mm + ox * u_mm * cos_a; }
  f64 x_of(f64 q) const noexcept { return q / (u_mm * sin_a); }
  f64 y_of(f64 q, f64 p) const noexcept { return (p - q * cos_a / sin_a) / v_mm; }
  // The texels a millimetre across a stroke that runs along (dx, dy), on the image axis that shows
  // its width best.
  f64 texels_across(f64 dx, f64 dy) const noexcept {
    const f64 nq = -dy, np = dx;
    const f64 tx = std::fabs(x_of(nq));
    const f64 ty = std::fabs(y_of(nq, np));
    return tx > ty ? tx : ty;
  }
  // The coarser of the two texel pitches, millimetres: the width an edge is softened over.
  f64 pitch() const noexcept {
    const f64 a = u_mm * sin_a;
    return a > v_mm ? a : v_mm;
  }
};

struct Shoot {
  f64 x = 0, y = 0;    // its base, texels on the torus
  f64 dx = 0, dy = 1;  // unit, along it, in the card's millimetres (across, along)
  f64 length = 0;      // millimetres
  f64 radius = 0;      // of its wood at its base, millimetres
  u32 order = 0;
};

// A needle or a twig as drawn: a capsule from a base in sub-texels, in the card's millimetres, its
// radius held over the first `hold` of its length and running to `r1` at its tip.
struct Stroke {
  i64 bx = 0, by = 0;
  f64 dx = 0, dy = 1;
  f64 length = 0;
  f64 r0 = 0, r1 = 0;
  f64 hold = 0;
  f64 depth = 0;  // -1 behind its shoot .. 1 in front of it; a twig is 0
  bool twig = false;
  f32 colour[3] = {0, 0, 0};
  f32 rough = 0.5f;
  f64 tip = 1.0;      // the tone at its tip over the tone at its base
  f64 crown_m = 0;    // the height of its round section over its line
};

// Every texel a capsule of `reach` millimetres round the stroke's line touches, with its distance
// from the line in millimetres and its place along it (0 at the base, 1 at the tip).
template <typename Visit>
void walk_stroke(const Field& field, const Card& card, const Stroke& s, f64 reach, Visit&& visit) {
  const i64 n = static_cast<i64>(field.n);
  const f64 bx = static_cast<f64>(s.bx) / static_cast<f64>(k_sub);
  const f64 by = static_cast<f64>(s.by) / static_cast<f64>(k_sub);
  const f64 ex = bx + card.x_of(s.dx * s.length);
  const f64 ey = by + card.y_of(s.dx * s.length, s.dy * s.length);
  const f64 rx = reach / (card.u_mm * card.sin_a) + 1.5;
  const f64 ry = reach / (card.v_mm * card.sin_a) + 1.5;
  const i64 x0 = static_cast<i64>(std::floor(std::min(bx, ex) - rx));
  const i64 x1 = static_cast<i64>(std::floor(std::max(bx, ex) + rx));
  const i64 y0 = static_cast<i64>(std::floor(std::min(by, ey) - ry));
  const i64 y1 = static_cast<i64>(std::floor(std::max(by, ey) + ry));
  for (i64 ty = y0; ty <= y1; ++ty) {
    const u32 sy = field.stored(ty, field.window.y);
    const f64 oy = offset_texels(ty, s.by, n);
    for (i64 tx = x0; tx <= x1; ++tx) {
      const f64 ox = offset_texels(tx, s.bx, n);
      const f64 q = card.q_of(ox), p = card.p_of(ox, oy);
      f64 along = q * s.dx + p * s.dy;
      along = along < 0.0 ? 0.0 : (along > s.length ? s.length : along);
      const f64 px = q - along * s.dx, py = p - along * s.dy;
      const f64 d = std::sqrt(px * px + py * py);
      const f64 t = s.length > 0.0 ? along / s.length : 0.0;
      visit(field.index(field.stored(tx, field.window.x), sy), d, t);
    }
  }
}

// **The height is each needle's own round section and nothing else** (G203's fourth leg,
// 2026-11-16). The third leg laid a round back under every brush (a solid cylinder's height, its
// normal leaning 45 degrees at the brush's edge) and lifted each needle over the ones behind it by
// how far in front of its shoot it stood. Measured in the engine under a sky at 15:30, the same
// cards and the same image with the normal map left out returned 16 to 24 % more light per unit
// of albedo at 10 to 100 m and 10 % more at 300 m, on the sunlit needles 22 to 30 %
// (conifer-crown-coverage.md, "Where the light went: the fourth leg"): that was the darkening.
// Both were wrong for what a brush is. A brush is porous: a shoot's silhouette is about half of
// its envelope (Palmroth et al. 2002's STAR and needle counts; the grounding page, "What a shoot's
// silhouette is"), so the sun reaches the needles of its far flank through the gaps about half the
// time, where a solid cylinder's far flank is unlit; and the crown's lean on the card's vertex
// normal (flora's conifer) already says which way a bough faces. And a needle over another is
// separated from it by air: a height step there makes a wall facing sideways along every needle's
// edge, which no needle has. What stays is a needle's own roundness, the crown of its section,
// which is a solid's and is real.
void lay_stroke(Field& field, const Card& card, const Stroke& s) {
  const f64 soft = card.pitch();
  walk_stroke(field, card, s, std::max(s.r0, s.r1) + soft, [&](u32 i, f64 d, f64 t) {
    const f64 r = t <= s.hold ? s.r0 : mix(s.r0, s.r1, (t - s.hold) / (1.0 - s.hold));
    const f64 cover = clamp01((r - d) / soft + 0.5);
    if (cover <= 0.0) return;
    Texel& texel = field.texels[i];
    // A needle is paler toward its tip (`tip` is the tip's tone over the base's).
    const f64 tone = mix(1.0, s.tip, t);
    texel.r = static_cast<f32>(
        mix(static_cast<f64>(texel.r), clamp01(static_cast<f64>(s.colour[0]) * tone), cover));
    texel.g = static_cast<f32>(
        mix(static_cast<f64>(texel.g), clamp01(static_cast<f64>(s.colour[1]) * tone), cover));
    texel.b = static_cast<f32>(
        mix(static_cast<f64>(texel.b), clamp01(static_cast<f64>(s.colour[2]) * tone), cover));
    texel.rough =
        static_cast<f32>(mix(static_cast<f64>(texel.rough), static_cast<f64>(s.rough), cover));
    if (cover >= 0.5) {
      // The stroke's own round section: the crown along its line, what is drawn in front wins.
      const f64 across = r > 0.0 ? clamp01(d / r) : 1.0;
      texel.height = static_cast<f32>(s.crown_m * (1.0 - across * across));
    }
    if (static_cast<f64>(field.alpha[i]) < cover) field.alpha[i] = static_cast<f32>(cover);
  });
}

i64 to_sub(f64 texels) noexcept {
  return static_cast<i64>(std::floor(texels * static_cast<f64>(k_sub) + 0.5));
}

// The largest share of the step (sx, sy) from (x, y) that stays inside [lo, hi] on an axis.
f64 fit_axis(f64 at, f64 step, f64 lo, f64 hi) noexcept {
  if (step > 0.0 && at + step > hi) return (hi - at) / step;
  if (step < 0.0 && at + step < lo) return (lo - at) / step;
  return 1.0;
}

}  // namespace

void build_spray(const Set& set, Field& field, SetStats& stats, Shade& shade) {
  const Spray& sp = set.spray;
  const u32 n = field.n;
  const f64 size = static_cast<f64>(n);
  const f64 rad = k_pi / 180.0;
  const bool pine = sp.form == SprayForm::pine;

  // ---- the card: `tile_mm` long; as wide along its branch as the recipe says, or square.
  Card card;
  const f64 length_mm = field.texel_m * 1000.0 * size;
  const f64 width_mm = sp.card_width_mm == 0 ? length_mm : static_cast<f64>(sp.card_width_mm);
  card.v_mm = length_mm / size;
  card.u_mm = width_mm / size;
  if (sp.card_angle_deg != 0 && sp.card_angle_deg != 90) {
    card.sin_a = det::sin(static_cast<f64>(sp.card_angle_deg) * rad);
    card.cos_a = det::cos(static_cast<f64>(sp.card_angle_deg) * rad);
  }
  // The normal map's step along the image's x is the card's, not the tile's.
  field.texel_u_m = card.u_mm * 0.001;
  const f64 across_mm = width_mm * card.sin_a;  // the card's width across its own axis

  const f64 needle_half = static_cast<f64>(sp.needle_width_um) * 0.0005;
  const f64 needle_min = static_cast<f64>(sp.needle_min_um) * 0.001;
  const f64 needle_max = static_cast<f64>(sp.needle_max_um) * 0.001;
  const f64 needle_angle = static_cast<f64>(sp.needle_angle_deg) * rad;
  const f64 needle_spread = static_cast<f64>(sp.needle_spread_deg) * rad;
  const f64 relief_m = static_cast<f64>(sp.relief_um) * 1.0e-6;
  // A brush's radius: how far a needle's tip stands from its twig's axis.
  const f64 brush_mm = needle_max * det::sin(needle_angle);
  const f64 margin = std::max(2.0, size * 0.015);
  // A needle keeps the margin; a shoot's tip keeps its brush inside it.
  const f64 lo = margin, hi = size - margin;
  const f64 keep_x = 0.8 * brush_mm / (card.u_mm * card.sin_a);
  const f64 keep_y = 0.8 * brush_mm / (card.v_mm * card.sin_a);
  const auto radius_of = [&card](f64 half_mm, f64 dx, f64 dy) {
    return std::max(half_mm, k_min_radius / card.texels_across(dx, dy));
  };

  // ---- the skeleton: the twig, and the side shoots of each order, on a stream of their own, so a
  // needle's parameters move no shoot.
  Rng skeleton(set.seed ^ 0x73686f6f7473ull);
  Vector<Shoot> shoots;
  {
    Shoot twig;
    twig.x = size * 0.5;
    twig.y = lo + 1.0;
    twig.length = (hi - twig.y) * card.v_mm - needle_max * det::cos(needle_angle);
    twig.radius = static_cast<f64>(sp.twig_um) * 0.0005;
    shoots.push_back(twig);
  }
  const f64 reach = permille(sp.reach_permille) * across_mm;
  const f64 taper = permille(sp.taper_permille);
  const f64 shoot_angle = static_cast<f64>(sp.shoot_angle_deg) * rad;
  const f64 shoot_spread = static_cast<f64>(sp.shoot_spread_deg) * rad;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot parent = shoots[i];
    if (parent.order >= sp.orders) continue;
    // A spruce's card is several small boughs, not one comb: the twig's side shoots stand a node
    // apart with open card between them, and each carries its own short twigs three times as
    // close, so a side shoot with its twigs is a little frond a few centimetres wide. A pine
    // whorl's laterals are half as many on a side shoot.
    const bool first = parent.order == 0;
    const f64 node = static_cast<f64>(sp.node_mm) * (first || pine ? 1.0 : 0.3);
    const u32 per_node = first ? sp.per_node : std::max(1u, (sp.per_node + 1) / 2);
    f64 s = first ? node * 0.6 : node;
    u32 index = 0;
    while (s < parent.length - node * 0.5) {
      const f64 whorl = skeleton.unit();  // where round its parent a whorl's first lateral stands
      for (u32 k = 0; k < per_node; ++k) {
        const f64 t = s / parent.length;
        f64 length =
            (first ? reach : 0.45 * parent.length) * (1.0 - taper * t) * skeleton.range(0.75, 1.0);
        const f64 beta = shoot_angle + shoot_spread * (2.0 * skeleton.unit() - 1.0);
        const f64 along = det::cos(beta);
        f64 across = det::sin(beta);
        if (pine) {
          // A whorl stands all round its parent, its laterals evenly spaced: one at azimuth theta
          // shows the cosine of it across the view, and is the shorter for what points at the eye.
          const f64 turn =
              (static_cast<f64>(k) + 0.6 * (skeleton.unit() - 0.5)) / static_cast<f64>(per_node);
          across *= det::cos(2.0 * k_pi * (whorl + turn));
        } else {
          // A frond is flat: its side shoots lie in it, to one side and then the other, not evenly.
          const bool left = (index + k) % 2 == 0;
          across *= (skeleton.unit() < 0.15) != left ? -1.0 : 1.0;
        }
        const f64 shown = std::sqrt(along * along + across * across);
        Shoot child;
        child.x = parent.x + card.x_of(parent.dx * s);
        child.y = parent.y + card.y_of(parent.dx * s, parent.dy * s);
        child.dx = (parent.dx * along - parent.dy * across) / shown;
        child.dy = (parent.dy * along + parent.dx * across) / shown;
        length *= shown;
        // Shortened to keep its brush inside the image.
        f64 share = fit_axis(child.x, card.x_of(child.dx * length), lo + keep_x, hi - keep_x);
        share = std::min(share, fit_axis(child.y, card.y_of(child.dx * length, child.dy * length),
                                         lo + keep_y, hi - keep_y));
        length *= share < 0.0 ? 0.0 : share;
        child.length = length;
        child.radius = parent.radius * 0.6;
        child.order = parent.order + 1;
        if (length >= 0.6 * needle_max) shoots.push_back(child);
      }
      s += node * skeleton.range(0.75, 1.25);
      ++index;
    }
  }

  // ---- the strokes: every shoot's wood, and its needles as the eye sees them.
  Rng draw(set.seed ^ 0x6e6565646c6573ull);
  Vector<Stroke> strokes;
  const f64 tone = permille(sp.tone_permille);
  const f64 needle_rgb[3] = {permille(sp.needle_r_permille), permille(sp.needle_g_permille),
                             permille(sp.needle_b_permille)};
  const f64 step = 10.0 / static_cast<f64>(sp.needles_per_cm);
  const f64 flat = permille(sp.flat_permille);
  u32 needles = 0;
  f64 thinnest = 1.0e9;
  for (const Shoot& shoot : shoots) {
    Stroke wood;
    wood.bx = to_sub(shoot.x);
    wood.by = to_sub(shoot.y);
    wood.dx = shoot.dx;
    wood.dy = shoot.dy;
    wood.length = shoot.length;
    wood.r0 = radius_of(shoot.radius, shoot.dx, shoot.dy);
    wood.r1 = radius_of(shoot.radius * 0.5, shoot.dx, shoot.dy);
    wood.twig = true;
    wood.colour[0] = static_cast<f32>(permille(sp.twig_r_permille));
    wood.colour[1] = static_cast<f32>(permille(sp.twig_g_permille));
    wood.colour[2] = static_cast<f32>(permille(sp.twig_b_permille));
    wood.rough = 0.85f;
    wood.crown_m = 0.2 * relief_m;
    strokes.push_back(wood);

    const f64 needled =
        sp.needled_mm == 0 ? shoot.length : std::min(shoot.length, static_cast<f64>(sp.needled_mm));
    const f64 from = shoot.length - needled;

    f64 s = from + step * draw.unit();
    while (s <= shoot.length) {
      // Where round the shoot the needle stands: any azimuth, or in the spray's plane for the
      // share that lies flat.
      f64 phi = 2.0 * k_pi * draw.unit();
      const f64 lies = draw.unit();
      const f64 which = draw.unit();
      if (lies < flat) phi = (which < 0.5 ? 0.0 : k_pi) + 0.5 * (draw.unit() - 0.5);
      f64 alpha = needle_angle + needle_spread * (2.0 * draw.unit() - 1.0);
      // The last needles of a shoot close over its bud: they point forward.
      const f64 to_tip = (shoot.length - s) / (0.5 * needle_max);
      if (to_tip < 1.0) alpha *= mix(0.4, 1.0, to_tip);
      const f64 along = det::cos(alpha);
      const f64 across = det::sin(alpha) * det::cos(phi);
      const f64 front = det::sin(phi);
      const f64 shown = std::sqrt(along * along + across * across);
      const f64 length = draw.range(needle_min, needle_max);
      // A needle's colour is its own (the published reflectance of a needle), a little darker
      // behind its shoot than in front of it and paler on the year's growth, the last fifth of a
      // shoot. How dark a crown reads is the light's to say, not the image's: the second leg's
      // sprays ran from half the colour to 1.1 of it by depth and read near black in a stand.
      const f64 young = s > 0.8 * shoot.length ? 1.15 : 1.0;
      const f64 shade_of_depth = (0.8 + 0.25 * (0.5 + 0.5 * front)) * young;
      const u32 count = pine ? 2u : 1u;
      for (u32 k = 0; k < count; ++k) {
        // A pine's pair leaves one sheath: the second needle a few degrees off the first.
        const f64 turn = k == 0 ? 0.0 : draw.range(0.08, 0.2) * (draw.unit() < 0.5 ? -1.0 : 1.0);
        const f64 ct = det::cos(turn), st = det::sin(turn);
        const f64 ux = (shoot.dx * along - shoot.dy * across) / shown;
        const f64 uy = (shoot.dy * along + shoot.dx * across) / shown;
        Stroke needle;
        const f64 x = shoot.x + card.x_of(shoot.dx * s);
        const f64 y = shoot.y + card.y_of(shoot.dx * s, shoot.dy * s);
        needle.dx = ux * ct - uy * st;
        needle.dy = uy * ct + ux * st;
        needle.length = length * shown * (k == 0 ? 1.0 : draw.range(0.9, 1.0));
        const f64 keep = draw.range(0.85, 1.25);
        const f64 own = 1.0 + tone * (2.0 * draw.unit() - 1.0);
        const f64 tx = x + card.x_of(needle.dx * needle.length);
        const f64 ty = y + card.y_of(needle.dx * needle.length, needle.dy * needle.length);
        if (tx < lo || tx > hi || ty < lo || ty > hi) continue;
        needle.bx = to_sub(x);
        needle.by = to_sub(y);
        needle.r0 = radius_of(needle_half * keep, needle.dx, needle.dy);
        needle.r1 = 0.5 * needle.r0;
        needle.hold = 0.7;
        needle.depth = front;
        for (u32 c = 0; c < 3; ++c)
          needle.colour[c] = static_cast<f32>(clamp01(needle_rgb[c] * own * shade_of_depth));
        needle.tip = 1.2;
        needle.rough = static_cast<f32>(permille(sp.needle_roughness_permille));
        needle.crown_m = 0.3 * relief_m;
        // Its width in texels, as `radius_of` chose it (the floor is exact: four).
        thinnest = std::min(
            thinnest, 2.0 * std::max(needle_half * keep * card.texels_across(needle.dx, needle.dy),
                                     k_min_radius));
        strokes.push_back(needle);
        ++needles;
      }
      s += step * draw.range(0.6, 1.4);
    }
  }

  // ---- drawn from the back: the needles behind their shoots, the wood, the needles in front. The
  // order is a function of the strokes alone (depth, then the order they were made in).
  Vector<u32> order;
  order.resize(strokes.size());
  for (u32 i = 0; i < order.size(); ++i)
    order[i] = i;
  const auto layer = [&strokes](u32 i) -> f64 {
    const Stroke& s = strokes[i];
    return s.twig ? 0.0 : (s.depth < 0.0 ? s.depth - 1.0 : s.depth + 1.0);
  };
  std::sort(order.begin(), order.end(), [&layer](u32 a, u32 b) {
    const f64 la = layer(a), lb = layer(b);
    return la != lb ? la < lb : a < b;
  });

  // Under the outline's outside the colour is the needles' own, so no mip and no block of the
  // encoder mixes a background into the spray's edge.
  Texel ground;
  ground.r = static_cast<f32>(needle_rgb[0] * 0.9);
  ground.g = static_cast<f32>(needle_rgb[1] * 0.9);
  ground.b = static_cast<f32>(needle_rgb[2] * 0.9);
  ground.rough = static_cast<f32>(permille(sp.needle_roughness_permille));
  ground.height = 0.0f;
  field.texels.assign(n * n, ground);
  field.alpha.assign(n * n, 0.0f);
  // No brush's round back and no needle lifted over another (`lay_stroke` says why): the height
  // is the round section of whatever is drawn in front.
  for (const u32 i : order)
    lay_stroke(field, card, strokes[i]);

  u64 covered = 0;
  for (const f32 a : field.alpha)
    covered += a >= 0.5f ? 1u : 0u;
  stats.spray_cover = static_cast<f64>(covered) / (size * size);
  stats.spray_shoots = static_cast<u32>(shoots.size());
  stats.spray_needles = needles;
  stats.spray_needle_px = needles == 0 ? 0.0 : thinnest;
  // A needle under its neighbours is in their shade: full at the spray's own depth.
  shade.cavity_m = std::max(relief_m, 1.0e-5);
  shade.cavity_px = 3;
}

}  // namespace engine::matgen::detail
