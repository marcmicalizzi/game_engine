// Channels of flowing water (channel.h; docs/subsystems/water.md, "Channels"; roadmap S96).
#include <core/schema/json_reflect.h>
#include <systems/water/channel.h>

#include <algorithm>
#include <cmath>

namespace engine::water {

namespace {

bool finite(f32 v) noexcept { return std::isfinite(v); }

f32 horizontal(Vec3 d) noexcept { return std::sqrt(d.x * d.x + d.z * d.z); }

// The unit horizontal direction a point's sections are laid across: the mean of its stretches'
// horizontal directions (a mitre at a bend), from the stretches that have a horizontal run; the
// previous point's where none has (a fall straight down with no run before or after it).
Vec2 point_heading(const Channel& c, u32 i, Vec2 previous) noexcept {
  Vec2 sum{0.0f, 0.0f};
  const u32 n = c.points.size();
  for (u32 s = (i == 0 ? 0u : i - 1); s <= i && s + 1 < n; ++s) {
    const Vec3 d = c.points[s + 1] - c.points[s];
    const f32 h = horizontal(d);
    if (h > 1e-4f) sum = Vec2{sum.x + d.x / h, sum.y + d.z / h};
  }
  const f32 len = std::sqrt(sum.x * sum.x + sum.y * sum.y);
  return len > 1e-4f ? Vec2{sum.x / len, sum.y / len} : previous;
}

}  // namespace

bool read_channel(const scene::WaterChannel& block, Channel& out, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = "water channel \"" + block.name + "\": " + std::move(sentence);
    return false;
  };
  out = Channel{};
  out.name = block.name;
  const u32 n = block.points.size();
  if (n < 2) return fail("it has fewer than two points; a channel runs from one point to another");
  if (block.width_m.size() != n || block.depth_m.size() != n || block.slope.size() != n)
    return fail("its points, width_m, depth_m and slope must have one entry each per point (" +
                std::to_string(n) + " points, " + std::to_string(block.width_m.size()) +
                " widths, " + std::to_string(block.depth_m.size()) + " depths, " +
                std::to_string(block.slope.size()) + " slopes)");
  if (!(block.manning_n > 0.0f) || !finite(block.manning_n))
    return fail("its manning_n must be positive");
  if (!(block.shore_m >= 0.0f) || !finite(block.shore_m))
    return fail("its shore_m must not be negative");
  if (!(block.roughness >= 0.02f && block.roughness <= 1.0f))
    return fail("its roughness must be within [0.02, 1]");
  if (!(block.transmission >= 0.0f && block.transmission <= 1.0f))
    return fail("its transmission must be within [0, 1]");
  if (!(block.turbidity >= 0.0f) || !finite(block.turbidity))
    return fail("its turbidity must not be negative");
  f64 lo_x = 1e300, hi_x = -1e300, lo_z = 1e300, hi_z = -1e300;
  for (u32 i = 0; i < n; ++i) {
    const WorldPos& p = block.points[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      return fail("its point " + std::to_string(i) + " is not a number");
    if (!(block.width_m[i] > 0.0f) || !finite(block.width_m[i]))
      return fail("its width_m at point " + std::to_string(i) + " must be positive");
    if (!(block.depth_m[i] > 0.0f) || !finite(block.depth_m[i]))
      return fail("its depth_m at point " + std::to_string(i) + " must be positive");
    if (!finite(block.slope[i]))
      return fail("its slope at point " + std::to_string(i) + " is not a number");
    lo_x = std::min(lo_x, p.x);
    hi_x = std::max(hi_x, p.x);
    lo_z = std::min(lo_z, p.z);
    hi_z = std::max(hi_z, p.z);
  }
  // A slope is the surface's fall along the line: where the line descends into or out of a point,
  // a negative one says the water runs uphill.
  for (u32 i = 0; i < n; ++i) {
    const bool into = i > 0 && block.points[i].y < block.points[i - 1].y;
    const bool out_of = i + 1 < n && block.points[i + 1].y < block.points[i].y;
    if ((into || out_of) && block.slope[i] < 0.0f)
      return fail("its slope at point " + std::to_string(i) +
                  " is negative where the line descends; slope is the fall per metre downstream");
  }
  out.origin = WorldPos{0.5 * (lo_x + hi_x), block.points[0].y, 0.5 * (lo_z + hi_z)};
  out.points.reserve(n);
  for (u32 i = 0; i < n; ++i) {
    const WorldPos& p = block.points[i];
    out.points.push_back(Vec3{static_cast<f32>(p.x - out.origin.x),
                              static_cast<f32>(p.y - out.origin.y),
                              static_cast<f32>(p.z - out.origin.z)});
  }
  out.width_m = block.width_m;
  out.depth_m = block.depth_m;
  out.slope = block.slope;
  out.manning_n = block.manning_n;
  out.shore_m = block.shore_m;
  out.seed = block.seed;
  return true;
}

// The cube root of a >= 0 by Newton's iteration from above, + - * / alone, a fixed number of
// steps, so the speeds — and the mesh's phases — are the same bits on every toolchain (the C
// library's cbrt is not correctly rounded, and differs between them).
f64 cube_root(f64 a) noexcept {
  if (!(a > 0.0)) return 0.0;
  f64 y = std::max(a, 1.0);
  for (u32 i = 0; i < 64; ++i)
    y = (2.0 * y + a / (y * y)) / 3.0;
  return y;
}

f32 manning_speed(f32 n, f32 depth_m, f32 slope) noexcept {
  const f64 d = std::max(static_cast<f64>(depth_m), 0.0);
  const f64 s = std::max(static_cast<f64>(slope), 0.0);
  const f64 v = cube_root(d * d) * std::sqrt(s) / static_cast<f64>(n);
  return static_cast<f32>(std::max(v, static_cast<f64>(k_channel_min_speed)));
}

f64 shear_velocity(f64 speed, f64 depth_m, f64 n) noexcept {
  // u* = sqrt(g d S) with S from Manning's v = (1/n) d^(2/3) S^(1/2): u* = v n sqrt(g) / d^(1/6).
  const f64 sixth = std::sqrt(cube_root(std::max(depth_m, 1e-6)));
  return speed * n * std::sqrt(k_gravity) / sixth;
}

f32 froude(f32 speed, f32 depth_m) noexcept {
  const f64 d = std::max(static_cast<f64>(depth_m), 1e-6);
  return static_cast<f32>(static_cast<f64>(speed) / std::sqrt(k_gravity * d));
}

bool stretch_falls(const Channel& channel, u32 i) noexcept {
  if (i + 1 >= channel.points.size()) return false;
  const Vec3 d = channel.points[i + 1] - channel.points[i];
  const f32 drop = -d.y;
  return drop > 0.0f && drop >= k_channel_fall_gradient * horizontal(d);
}

Channel with_plunge_stretch(const Channel& channel) {
  Channel c = channel;
  const u32 n = c.points.size();
  if (c.pooled || n < 2 || !stretch_falls(c, n - 2)) return c;
  // The fall's top: the first of the run of falling stretches into the last point.
  u32 top = n - 2;
  while (top > 0 && stretch_falls(c, top - 1))
    --top;
  const f32 height = c.points[top].y - c.points[n - 1].y;
  const f32 length =
      std::clamp(k_channel_plunge_share * height, k_channel_plunge_min_m, k_channel_plunge_max_m);
  // On along the last horizontal run there is, the fall's own or the reach's above it.
  Vec2 heading{1.0f, 0.0f};
  for (u32 s = n - 1; s-- > 0;) {
    const Vec3 d = c.points[s + 1] - c.points[s];
    const f32 h = horizontal(d);
    if (h > 1e-4f) {
      heading = Vec2{d.x / h, d.z / h};
      break;
    }
  }
  const Vec3 foot = c.points[n - 1];
  c.points.push_back(Vec3{foot.x + heading.x * length, foot.y, foot.z + heading.y * length});
  c.width_m.push_back(c.width_m[n - 1]);
  c.depth_m.push_back(c.depth_m[n - 1]);
  c.slope.push_back(0.0f);
  return c;
}

f32 channel_spacing(const Channel& channel) noexcept {
  // The body's rule — three vertices across the shore fade — and at least four across the width.
  f32 narrow = 1e30f;
  for (const f32 w : channel.width_m)
    narrow = std::min(narrow, w);
  return std::clamp(std::min(channel.shore_m / 3.0f, narrow / 4.0f), 0.25f, 1.0f);
}

void build_channel_mesh(const Channel& described, ChannelMesh& out) {
  out = ChannelMesh{};
  const Channel c = with_plunge_stretch(described);
  const u32 n = c.points.size();
  if (n < 2) return;
  f32 widest = 0.0f;
  for (const f32 w : c.width_m)
    widest = std::max(widest, w);
  const f32 span = 1.0f + 2.0f * k_channel_edge_extend;
  f32 spacing = channel_spacing(c);
  // The vertex budget, as a body's: the spacing grows by a quarter until the ribbon fits (the
  // wraps' second copies included, at most one a section).
  u32 columns = 0;
  for (;;) {
    columns = std::clamp(static_cast<u32>(std::ceil(widest * span / spacing)), 4u, 64u);
    u32 sections = 1;
    for (u32 i = 0; i + 1 < n; ++i)
      sections += std::max(
          1u, static_cast<u32>(std::ceil(length(c.points[i + 1] - c.points[i]) / spacing)));
    if (2u * sections * (columns + 1) <= k_max_vertices) break;
    spacing *= 1.25f;
  }
  out.columns = columns;
  out.mesh.spacing = spacing;
  out.mesh.edge = columns + 1;

  // Each point's heading, and the speed at it: Manning's on the bed, the free fall's where the
  // stretch into it falls (from the lip's speed, sqrt(v_lip^2 + 2 g h)).
  Vector<Vec2> heading(n);
  Vec2 previous{1.0f, 0.0f};
  for (u32 i = 0; i < n; ++i) {
    heading[i] = point_heading(c, i, previous);
    previous = heading[i];
  }
  // A fall's stretch takes the heading of the bed around it, so its face is square to the flow.
  for (u32 i = 0; i + 1 < n; ++i) {
    if (!stretch_falls(c, i)) continue;
    const Vec3 d = c.points[i + 1] - c.points[i];
    if (horizontal(d) <= 1e-4f) heading[i + 1] = heading[i];
  }
  Vector<f32> bed_speed(n);
  for (u32 i = 0; i < n; ++i)
    bed_speed[i] = manning_speed(c.manning_n, c.depth_m[i], c.slope[i]);
  // The brink (channel.h): the speed at a lip point i a fall leaves from, and the reach over which
  // a subcritical approach speeds up to it (0: none).
  Vector<f32> brink_speed(n);
  Vector<f32> brink_reach(n, 0.0f);
  for (u32 i = 0; i < n; ++i) {
    brink_speed[i] = bed_speed[i];
    if (i == 0 || i + 1 >= n || !stretch_falls(c, i) || stretch_falls(c, i - 1)) continue;
    const f64 v = static_cast<f64>(bed_speed[i]);
    const f64 d = static_cast<f64>(c.depth_m[i]);
    if (!(v < std::sqrt(k_gravity * d))) continue;  // supercritical: over as it is
    const f64 q = v * d;
    const f64 dc = cube_root(q * q / k_gravity);
    brink_speed[i] = static_cast<f32>(q / (k_channel_brink_depth_share * dc));
    brink_reach[i] = static_cast<f32>(k_channel_brink_reach * dc);
  }

  // The sections, stretch by stretch: the station along the line, the place, the heading, the
  // width, the depth, and the speed.
  struct Section {
    Vec3 p;
    Vec2 heading;
    f32 width;
    f32 depth;
    f32 speed;
    f32 s;
    bool falls;  // the strip below it is a fall
  };
  Vector<Section> sections;
  f32 station = 0.0f;
  // The plunge: after a fall the jet's speed decays back to the bed's over `plunge_len`.
  f32 plunge_left = 0.0f;
  f32 plunge_len = 0.0f;
  f32 impact = 0.0f;
  // A fall of several stretches is one fall: its top is the first stretch's lip, and each stretch
  // falls on from the speed the one above it reached.
  f32 fall_top = 0.0f;
  f32 lip_speed = 0.0f;
  for (u32 i = 0; i + 1 < n; ++i) {
    const Vec3 a = c.points[i];
    const Vec3 b = c.points[i + 1];
    const f32 len = length(b - a);
    const u32 steps = std::max(1u, static_cast<u32>(std::ceil(len / spacing)));
    const bool falls = stretch_falls(c, i);
    const bool falling_on = falls && i > 0 && stretch_falls(c, i - 1);
    if (falls && !falling_on) fall_top = a.y;
    lip_speed = falling_on ? sections.back().speed : brink_speed[i];
    for (u32 k = (i == 0 ? 0u : 1u); k <= steps; ++k) {
      const f32 t = static_cast<f32>(k) / static_cast<f32>(steps);
      Section sec;
      sec.p = a + (b - a) * t;
      const Vec2 h{heading[i].x + (heading[i + 1].x - heading[i].x) * t,
                   heading[i].y + (heading[i + 1].y - heading[i].y) * t};
      const f32 hl = std::sqrt(h.x * h.x + h.y * h.y);
      sec.heading = hl > 1e-4f ? Vec2{h.x / hl, h.y / hl} : heading[i];
      sec.width = c.width_m[i] + (c.width_m[i + 1] - c.width_m[i]) * t;
      sec.depth = c.depth_m[i] + (c.depth_m[i + 1] - c.depth_m[i]) * t;
      const f32 ds = len / static_cast<f32>(steps);
      if (k > 0) station += ds;
      sec.s = station;
      if (falls) {
        const f64 h_fall = std::max(0.0, static_cast<f64>(a.y - sec.p.y));
        sec.speed = static_cast<f32>(std::sqrt(
            static_cast<f64>(lip_speed) * static_cast<f64>(lip_speed) + 2.0 * k_gravity * h_fall));
        if (k == steps) {
          impact = sec.speed;
          plunge_len = std::clamp(k_channel_plunge_share * (fall_top - b.y), k_channel_plunge_min_m,
                                  k_channel_plunge_max_m);
          plunge_left = plunge_len;
        }
      } else {
        const f32 bed = bed_speed[i] + (bed_speed[i + 1] - bed_speed[i]) * t;
        sec.speed = bed;
        if (plunge_left > 0.0f && k > 0) {
          plunge_left = std::max(0.0f, plunge_left - ds);
          const f32 share = plunge_left / plunge_len;
          sec.speed = bed + (std::max(impact, bed) - bed) * share * share;
        }
        // Into a brink: the last of the reach before a subcritical lip speeds up to it.
        const f32 reach = brink_reach[i + 1];
        const f32 to_lip = len * (1.0f - t);
        if (reach > 0.0f && to_lip < reach) {
          const f32 share = 1.0f - to_lip / reach;
          sec.speed = std::max(sec.speed, bed + (brink_speed[i + 1] - bed) * share * share);
        }
      }
      // The strip below a section inside this stretch is this stretch's; the strip below its last
      // section is the next stretch's (set after the loop).
      sec.falls = falls && k < steps;
      sections.push_back(sec);
    }
    // The last section of a stretch is the first of the next; it heads the next strip.
    if (i + 2 < n) sections.back().falls = stretch_falls(c, i + 1);
  }
  if (!sections.empty()) sections.back().falls = false;

  // The rows: each section's vertices across, its phase the travel time from the head, wrapped.
  Mesh& m = out.mesh;
  const u32 cols = columns;
  f64 tau = 0.0;
  f64 base = 0.0;
  const f64 period = static_cast<f64>(k_channel_phase_period_s);
  // For each section, the row the strip above it ends on and the row the strip below starts from.
  Vector<u32> row_in;
  Vector<u32> row_out;
  const auto emit_row = [&](const Section& sec, f64 phase) {
    const u32 first = m.positions.size();
    const Vec3 across{-sec.heading.y, 0.0f, sec.heading.x};
    // The ribbon's normal: across x along, along the line's tangent here.
    for (u32 j = 0; j <= cols; ++j) {
      const f32 u = (static_cast<f32>(j) / static_cast<f32>(cols) - 0.5f) * span;  // of width
      const f32 a = u * sec.width;
      m.positions.push_back(sec.p + across * a);
      m.normals.push_back(Vec3{0.0f, 1.0f, 0.0f});  // set below, once the tangents are known
      const f32 x = 2.0f * u;                       // -1 and 1 at the edges of the width
      const f32 depth = std::max(0.0f, sec.depth * (1.0f - x * x));
      m.uvs.push_back(Vec2{static_cast<f32>(phase), depth});
      m.max_depth = std::max(m.max_depth, depth);
    }
    return first;
  };
  for (u32 k = 0; k < sections.size(); ++k) {
    const Section& sec = sections[k];
    if (k > 0) {
      const Section& prev = sections[k - 1];
      const f64 ds = static_cast<f64>(sec.s - prev.s);
      tau += 0.5 * ds * (1.0 / static_cast<f64>(prev.speed) + 1.0 / static_cast<f64>(sec.speed));
    }
    out.section_s.push_back(sec.s);
    out.section_speed.push_back(sec.speed);
    out.section_tau.push_back(tau);
    out.section_falls.push_back(sec.falls ? 1u : 0u);
    const u32 in = emit_row(sec, tau - base);
    u32 row = in;
    if (tau - base >= period) {
      // The wrap: the strip above ends here at `tau - base`, the strip below starts at a period
      // less, the same pattern on both sides of the seam.
      while (tau - base >= period)
        base += period;
      row = emit_row(sec, tau - base);
    }
    row_in.push_back(in);
    row_out.push_back(row);
  }
  m.rings = static_cast<u32>(m.positions.size() / (cols + 1));

  // The normals: across x along, from the neighbouring sections' places.
  for (u32 k = 0; k < sections.size(); ++k) {
    const Vec3 lo = sections[k == 0 ? 0 : k - 1].p;
    const Vec3 hi = sections[std::min<u32>(k + 1, sections.size() - 1)].p;
    const Vec3 along = hi - lo;
    const Vec3 across{-sections[k].heading.y, 0.0f, sections[k].heading.x};
    Vec3 nrm = cross(across, along);
    const f32 l = length(nrm);
    nrm = l > 1e-8f ? nrm * (1.0f / l) : Vec3{0.0f, 1.0f, 0.0f};
    for (u32 j = 0; j <= cols; ++j) {
      m.normals[row_in[k] + j] = nrm;
      if (row_out[k] != row_in[k]) m.normals[row_out[k] + j] = nrm;
    }
  }

  // The strips: (b - a) x (c - a) is across x along, the ribbon's normal (channel.h).
  for (u32 k = 0; k + 1 < sections.size(); ++k) {
    const u32 top = row_out[k];
    const u32 bottom = row_in[k + 1];
    for (u32 j = 0; j < cols; ++j) {
      const u32 a = top + j, b = top + j + 1, cc = bottom + j + 1, d = bottom + j;
      m.indices.push_back(a);
      m.indices.push_back(b);
      m.indices.push_back(cc);
      m.indices.push_back(a);
      m.indices.push_back(cc);
      m.indices.push_back(d);
    }
  }
}

void build_channel_depth_map(const Channel& described, const ChannelMesh& mesh, DepthMap& out) {
  out = DepthMap{};
  const Channel c = with_plunge_stretch(described);
  const Mesh& m = mesh.mesh;
  if (m.positions.empty()) return;
  f32 lo_x = 1e30f, lo_z = 1e30f, hi_x = -1e30f, hi_z = -1e30f;
  for (const Vec3& p : m.positions) {
    lo_x = std::min(lo_x, p.x);
    hi_x = std::max(hi_x, p.x);
    lo_z = std::min(lo_z, p.z);
    hi_z = std::max(hi_z, p.z);
  }
  const f64 side = static_cast<f64>(std::max(hi_x - lo_x, hi_z - lo_z));
  const f64 target = static_cast<f64>(m.spacing) * 0.25;
  const f64 wanted = target > 0.0 ? std::ceil(side / target) + 1.0 : 0.0;
  const u32 size = static_cast<u32>(
      std::clamp(wanted, static_cast<f64>(k_depth_map_min), static_cast<f64>(k_depth_map_max)));
  const f64 texel = side / static_cast<f64>(size - 1);
  const f64 x0 = 0.5 * (static_cast<f64>(lo_x) + static_cast<f64>(hi_x)) - 0.5 * side;
  const f64 z0 = 0.5 * (static_cast<f64>(lo_z) + static_cast<f64>(hi_z)) - 0.5 * side;
  out.size = size;
  out.origin = Vec2{static_cast<f32>(x0), static_cast<f32>(z0)};
  out.texel_m = static_cast<f32>(texel);
  const f64 ox = static_cast<f64>(out.origin.x);
  const f64 oz = static_cast<f64>(out.origin.y);
  const f64 tm = static_cast<f64>(out.texel_m);
  Vector<f64> depth(size * size, 0.0);
  // Each stretch's bed over the texels within its reach: the parabola across its width at the
  // texel's nearest place on the stretch (in x and z), the deepest stretch winning.
  for (u32 i = 0; i + 1 < c.points.size(); ++i) {
    const Vec3 a = c.points[i];
    const Vec3 b = c.points[i + 1];
    const f64 reach = 0.5 * static_cast<f64>(std::max(c.width_m[i], c.width_m[i + 1])) + tm;
    const f64 ax = static_cast<f64>(a.x), az = static_cast<f64>(a.z), bx = static_cast<f64>(b.x),
              bz = static_cast<f64>(b.z);
    const i64 i0 =
        std::max<i64>(0, static_cast<i64>(std::floor((std::min(ax, bx) - reach - ox) / tm)));
    const i64 i1 =
        std::min<i64>(size - 1, static_cast<i64>(std::ceil((std::max(ax, bx) + reach - ox) / tm)));
    const i64 j0 =
        std::max<i64>(0, static_cast<i64>(std::floor((std::min(az, bz) - reach - oz) / tm)));
    const i64 j1 =
        std::min<i64>(size - 1, static_cast<i64>(std::ceil((std::max(az, bz) + reach - oz) / tm)));
    const f64 dx = bx - ax, dz = bz - az;
    const f64 len2 = dx * dx + dz * dz;
    for (i64 j = j0; j <= j1; ++j) {
      for (i64 ii = i0; ii <= i1; ++ii) {
        const f64 x = ox + static_cast<f64>(ii) * tm;
        const f64 z = oz + static_cast<f64>(j) * tm;
        const f64 t =
            len2 > 1e-12 ? std::clamp(((x - ax) * dx + (z - az) * dz) / len2, 0.0, 1.0) : 0.0;
        const f64 px = ax + dx * t - x, pz = az + dz * t - z;
        const f64 dist = std::sqrt(px * px + pz * pz);
        const f64 w = static_cast<f64>(c.width_m[i]) +
                      (static_cast<f64>(c.width_m[i + 1]) - static_cast<f64>(c.width_m[i])) * t;
        const f64 d = static_cast<f64>(c.depth_m[i]) +
                      (static_cast<f64>(c.depth_m[i + 1]) - static_cast<f64>(c.depth_m[i])) * t;
        const f64 r = 2.0 * dist / w;
        const f64 bed = d * (1.0 - r * r);
        f64& slot = depth[static_cast<u32>(j) * size + static_cast<u32>(ii)];
        slot = std::max(slot, bed);
      }
    }
  }
  f64 deepest = 0.0;
  for (const f64 d : depth)
    deepest = std::max(deepest, d);
  out.depth_max = static_cast<f32>(deepest);
  out.texels.resize(size * size);
  const f64 scale = out.depth_max > 0.0f ? 65535.0 / static_cast<f64>(out.depth_max) : 0.0;
  for (u32 t = 0; t < size * size; ++t)
    out.texels[t] = static_cast<u16>(std::clamp(std::floor(depth[t] * scale + 0.5), 0.0, 65535.0));
}

JsonValue channel_generator_params(std::span<const scene::WaterChannel> channels) {
  JsonValue list = JsonValue::array();
  for (const scene::WaterChannel& channel : channels)
    list.push_back(schema::to_json(channel));
  JsonValue params = JsonValue::object();
  params.set("channels", std::move(list));
  return params;
}

}  // namespace engine::water
