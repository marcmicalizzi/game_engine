// The plan of a layer's lines and how a slot stands on the ground (docs/subsystems/walls.md,
// "Stations" and "Standing on the ground"). Everything here is integers until a module is stood:
// a line is integer millimetres, a station whole centimetres of arc length (each segment's length
// one correctly rounded square root of an integer, rounded to the centimetre), a module's ends
// the millimetres its stations interpolate to. So which module stands where is a function of the
// layer alone, the same from any tile and any start; and the ground is asked at the same integers
// whichever tile realises the module.

#include <core/hash/hash.h>
#include <domain/walls/walls.h>

#include <algorithm>
#include <cmath>

namespace engine::walls {
namespace {

i64 floor_div(i64 n, i64 d) noexcept {
  const i64 q = n / d;
  return (n % d != 0 && ((n < 0) != (d < 0))) ? q - 1 : q;
}
// n / d to the nearest integer, halves up, for d > 0.
i64 div_round(i64 n, i64 d) noexcept { return floor_div(2 * n + d, 2 * d); }

constexpr i64 k_max_segment_mm = 10'000'000;  // 10 km: dx * dx stays exact in an f64
constexpr i64 k_min_segment_mm = 500;
constexpr i64 k_min_run_cm = 60;

bool fail(std::string& why, std::string_view what) {
  why = std::string(what);
  return false;
}

Module module_of(const Layer& layer) {
  Module m;
  m.length_cm = layer.module_cm;
  m.height_cm = layer.height_cm;
  m.cope_cm = layer.cope_cm;
  m.base_width_cm = layer.base_width_cm;
  m.top_width_cm = layer.top_width_cm;
  m.footing_cm = layer.footing_cm;
  return m;
}

bool tile_less(const TileRef& a, const TileRef& b) {
  if (a.z != b.z) return a.z < b.z;
  if (a.x != b.x) return a.x < b.x;
  return a.index < b.index;
}

// The quaternion turning unit vector u to unit vector v (both given unnormalized), with only a
// correctly rounded square root on the way: the half-way rotation (1 + u.v, u x v), normalized.
Quat turn_to(f64 ux, f64 uy, f64 uz, f64 vx, f64 vy, f64 vz) {
  const f64 lu = std::sqrt(ux * ux + uy * uy + uz * uz),
            lv = std::sqrt(vx * vx + vy * vy + vz * vz);
  ux /= lu;
  uy /= lu;
  uz /= lu;
  vx /= lv;
  vy /= lv;
  vz /= lv;
  const f64 w = 1.0 + ux * vx + uy * vy + uz * vz;
  f64 x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx;
  f64 ww = w;
  if (w < 1e-12) {  // opposite: a half turn about +y (every turn here is about y or z)
    x = 0;
    y = 1;
    z = 0;
    ww = 0;
  }
  const f64 l = std::sqrt(x * x + y * y + z * z + ww * ww);
  return {static_cast<f32>(x / l), static_cast<f32>(y / l), static_cast<f32>(z / l),
          static_cast<f32>(ww / l)};
}

// What a module's ends and feet stand on: the chord between the ground at its two ends, and how
// far it is sunk so that its footing's bed meets or goes under the ground at every foot sampled
// (both faces' feet beyond the scarcement, at five places along it).
struct Chord {
  f64 h0 = 0, h1 = 0, length = 0, dx = 0, dz = 0, sink = 0;
};
Chord chord_of(i64 x0, i64 z0, i64 x1, i64 z1, f64 half_width, f64 footing,
               const scene_gen::Ground& ground) {
  Chord c;
  c.h0 = ground.floor_mm(x0, z0);
  c.h1 = ground.floor_mm(x1, z1);
  c.dx = static_cast<f64>(x1 - x0) * 0.001;
  c.dz = static_cast<f64>(z1 - z0) * 0.001;
  c.length = std::sqrt(c.dx * c.dx + c.dz * c.dz);
  const f64 nx = -c.dz / c.length, nz = c.dx / c.length;  // across the wall
  f64 worst = 0;
  for (i32 i = 0; i <= 4; ++i) {
    const f64 u = 0.25 * static_cast<f64>(i);
    const f64 chord = c.h0 + (c.h1 - c.h0) * u;
    for (i32 side = -1; side <= 1; ++side) {
      const f64 off = static_cast<f64>(side) * half_width;
      const i64 px =
          x0 + static_cast<i64>(std::llround(static_cast<f64>(x1 - x0) * u + nx * off * 1000.0));
      const i64 pz =
          z0 + static_cast<i64>(std::llround(static_cast<f64>(z1 - z0) * u + nz * off * 1000.0));
      worst = std::max(worst, chord - ground.floor_mm(px, pz));
    }
  }
  c.sink = std::max(0.0, worst - footing);
  return c;
}

}  // namespace

scene_gen::TileCoord tile_of(i64 x_mm, i64 z_mm, i64 tile_mm) noexcept {
  return {static_cast<i32>(floor_div(x_mm, tile_mm)), static_cast<i32>(floor_div(z_mm, tile_mm))};
}

std::span<const TileRef> refs_on(const Vector<TileRef>& refs, scene_gen::TileCoord tile) noexcept {
  TileRef lo;
  lo.x = tile.x;
  lo.z = tile.z;
  lo.index = 0;
  TileRef hi = lo;
  hi.index = 0xFFFFFFFFu;
  const TileRef* b = std::lower_bound(refs.begin(), refs.end(), lo, tile_less);
  const TileRef* e = std::upper_bound(b, refs.end(), hi, tile_less);
  return {b, static_cast<u32>(e - b)};
}

void point_at(const LinePlan& line, i64 s, i64& x, i64& z) noexcept {
  const u32 n = line.station_cm.size();
  u32 i = 0;
  while (i + 2 < n && line.station_cm[i + 1] <= s)
    ++i;
  const i64 den = line.station_cm[i + 1] - line.station_cm[i];
  const i64 num = s - line.station_cm[i];
  x = line.x_mm[i] + div_round((line.x_mm[i + 1] - line.x_mm[i]) * num, den);
  z = line.z_mm[i] + div_round((line.z_mm[i + 1] - line.z_mm[i]) * num, den);
}

u32 kit_size(u32 variants) noexcept { return 10 * variants + 19; }

u32 kit_index(ModuleKind kind, u32 variant, i32 c, u32 variants) noexcept {
  const u32 uc = static_cast<u32>(std::clamp(c < 0 ? -c : c, 0, k_grade_classes));
  const u32 sc =
      static_cast<u32>(std::clamp(c, -k_grade_classes, k_grade_classes) + k_grade_classes);
  switch (kind) {
    case ModuleKind::run: return variant * 5 + uc;
    case ModuleKind::fallen: return 5 * variants + variant * 5 + uc;
    case ModuleKind::head: return 10 * variants + sc;
    case ModuleKind::broken: return 10 * variants + 9 + sc;
    default: return 10 * variants + 18;
  }
}

void kit_recipes(const Layer& layer, Vector<Module>& out) {
  out.clear();
  out.resize(kit_size(layer.variants));
  const Module base = module_of(layer);
  auto put = [&](ModuleKind kind, u32 variant, i32 c) {
    Module m = base;
    m.kind = kind;
    m.grade_permille = c * k_grade_step_permille;
    m.seed = hash_combine(hash_combine(hash_combine(layer.seed, static_cast<u64>(kind)), variant),
                          static_cast<u64>(c + 16));
    if (kind == ModuleKind::strip) {
      m.length_cm = 100;
      m.grade_permille = 0;
    }
    out[kit_index(kind, variant, c, layer.variants)] = m;
  };
  for (u32 v = 0; v < layer.variants; ++v)
    for (i32 c = 0; c <= k_grade_classes; ++c) {
      put(ModuleKind::run, v, c);
      put(ModuleKind::fallen, v, c);
    }
  for (i32 c = -k_grade_classes; c <= k_grade_classes; ++c) {
    put(ModuleKind::head, 0, c);
    put(ModuleKind::broken, 0, c);
  }
  put(ModuleKind::strip, 0, 0);
}

bool make_plan(const Layer& layer, i64 tile_mm, Plan& out, std::string& why) {
  out = {};
  out.tile_mm = tile_mm;
  if (tile_mm <= 0) return fail(why, "the tile size must be positive");
  if (layer.module_cm < 200 || layer.module_cm > 400)
    return fail(why, "module_cm must be 200..400");
  if (layer.gateway_cm < 100 || layer.gateway_cm > 800)
    return fail(why, "gateway_cm must be 100..800");
  if (!(layer.ruined >= 0.0f && layer.ruined <= 0.9f)) return fail(why, "ruined must be 0..0.9");
  if (layer.gap_modules < 1 || layer.gap_modules > 8) return fail(why, "gap_modules must be 1..8");
  if (layer.variants < 1 || layer.variants > 4) return fail(why, "variants must be 1..4");
  if (layer.lines.size() > 4096) return fail(why, "at most 4096 lines");
  {
    std::string r;
    if (!module_in_range(module_of(layer), r)) return fail(why, r);
  }
  const f64 half_width = 0.5 * static_cast<f64>(layer.base_width_cm) * 0.01 + k_scarcement_m;
  for (u32 li = 0; li < layer.lines.size(); ++li) {
    const WallLine& w = layer.lines[li];
    const std::string where = "line " + std::to_string(li) + ": ";
    if (w.points_mm.size() < 4 || w.points_mm.size() % 2 != 0)
      return fail(why, where + "points_mm must hold two or more x, z pairs");
    LinePlan line;
    for (u32 i = 0; i < w.points_mm.size(); i += 2) {
      line.x_mm.push_back(w.points_mm[i]);
      line.z_mm.push_back(w.points_mm[i + 1]);
    }
    if (w.closed) {
      // From the entry, never from the vector being grown (its storage may move).
      line.x_mm.push_back(w.points_mm[0]);
      line.z_mm.push_back(w.points_mm[1]);
    }
    line.station_cm.push_back(0);
    for (u32 i = 0; i + 1 < line.x_mm.size(); ++i) {
      const i64 dx = line.x_mm[i + 1] - line.x_mm[i], dz = line.z_mm[i + 1] - line.z_mm[i];
      if (dx > k_max_segment_mm || -dx > k_max_segment_mm || dz > k_max_segment_mm ||
          -dz > k_max_segment_mm)
        return fail(why, where + "a segment is longer than 10 km");
      const i64 mm = static_cast<i64>(std::llround(std::sqrt(static_cast<f64>(dx * dx + dz * dz))));
      if (mm < k_min_segment_mm) return fail(why, where + "a segment is shorter than 0.5 m");
      line.station_cm.push_back(line.station_cm.back() + div_round(mm, 10));
    }
    line.length_cm = line.station_cm.back();
    // The runs: the stretches between vertices and gateways.
    const i64 half_gate = static_cast<i64>(layer.gateway_cm) / 2;
    Vector<i64> gates;
    gates.assign(w.gateways_cm.begin(), w.gateways_cm.end());
    std::sort(gates.begin(), gates.end());
    for (u32 g = 0; g < gates.size(); ++g) {
      if (gates[g] - half_gate <= 0 || gates[g] + half_gate >= line.length_cm)
        return fail(why, where + "a gateway does not lie inside the line");
      if (g > 0 && gates[g] - gates[g - 1] < 2 * half_gate + k_min_run_cm)
        return fail(why, where + "two gateways overlap");
    }
    line.first_slot = static_cast<u32>(out.slots.size());
    struct Run {
      i64 a, b;
      bool head_a, head_b, corner_a, corner_b;
      u32 seg;
    };
    Vector<Run> runs;
    for (u32 seg = 0; seg + 1 < line.station_cm.size(); ++seg) {
      i64 a = line.station_cm[seg];
      const i64 end = line.station_cm[seg + 1];
      const bool first_seg = seg == 0, last_seg = seg + 2 == line.station_cm.size();
      bool head_a = first_seg && !w.closed;
      bool corner_a = !head_a && (!first_seg || w.closed);
      for (i64 g : gates) {
        if (g + half_gate <= a || g - half_gate >= end) continue;
        if (g - half_gate > a && g - half_gate - a >= k_min_run_cm)
          runs.push_back({a, g - half_gate, head_a, true, corner_a, false, seg});
        a = g + half_gate;
        head_a = true;
        corner_a = false;
      }
      if (end - a >= k_min_run_cm) {
        const bool head_b = last_seg && !w.closed;
        runs.push_back({a, end, head_a, head_b, corner_a, !head_b && (!last_seg || w.closed), seg});
      }
    }
    for (u32 run_index = 0; run_index < runs.size(); ++run_index) {
      const Run& r = runs[run_index];
      const i64 len = r.b - r.a;
      const i64 n = std::max<i64>(1, div_round(len, static_cast<i64>(layer.module_cm)));
      // The corner's mitre: each module at a vertex runs on past it by half its width times the
      // tangent of half the turn, so the outside of the corner is closed (to a 120 degree turn).
      auto mitre_mm = [&](u32 vertex) -> f64 {
        const u32 nv = line.x_mm.size();
        u32 prev = vertex == 0 ? nv - 2 : vertex - 1;
        u32 next = vertex + 1 >= nv ? 1 : vertex + 1;
        const f64 ux = static_cast<f64>(line.x_mm[vertex] - line.x_mm[prev]),
                  uz = static_cast<f64>(line.z_mm[vertex] - line.z_mm[prev]);
        const f64 vx = static_cast<f64>(line.x_mm[next] - line.x_mm[vertex]),
                  vz = static_cast<f64>(line.z_mm[next] - line.z_mm[vertex]);
        const f64 lu = std::sqrt(ux * ux + uz * uz), lv = std::sqrt(vx * vx + vz * vz);
        const f64 c = (ux * vx + uz * vz) / (lu * lv), s = std::abs(ux * vz - uz * vx) / (lu * lv);
        return 1000.0 * half_width * std::min(s / (1.0 + c), 1.7320508075688772);
      };
      for (i64 j = 0; j < n; ++j) {
        Slot slot;
        slot.line = li;
        slot.run = run_index;
        slot.s0_cm = r.a + (len * j) / n;
        slot.s1_cm = r.a + (len * (j + 1)) / n;
        point_at(line, slot.s0_cm, slot.x0_mm, slot.z0_mm);
        point_at(line, slot.s1_cm, slot.x1_mm, slot.z1_mm);
        const f64 dx = static_cast<f64>(slot.x1_mm - slot.x0_mm),
                  dz = static_cast<f64>(slot.z1_mm - slot.z0_mm);
        const f64 dl = std::sqrt(dx * dx + dz * dz);
        if (j == 0 && r.corner_a) {
          const f64 e = mitre_mm(r.seg);
          slot.x0_mm -= std::llround(dx / dl * e);
          slot.z0_mm -= std::llround(dz / dl * e);
        }
        if (j == n - 1 && r.corner_b) {
          const f64 e = mitre_mm(r.seg + 1);
          slot.x1_mm += std::llround(dx / dl * e);
          slot.z1_mm += std::llround(dz / dl * e);
        }
        if (j == 0 && r.head_a) {
          slot.kind = ModuleKind::head;
        } else if (j == n - 1 && r.head_b) {
          slot.kind = ModuleKind::head;
          slot.head_at_end = true;
        }
        slot.variant = static_cast<u8>(
            hash_combine(hash_combine(layer.seed, li), static_cast<u64>(slot.s0_cm)) %
            layer.variants);
        slot.id = static_cast<u32>(out.slots.size()) + 1;
        out.slots.push_back(slot);
      }
    }
    line.slot_count = static_cast<u32>(out.slots.size()) - line.first_slot;
    // The ruin: the asked share of the line's stretches (heads excluded), to the nearest module,
    // taken as stretches of `gap_modules` from starts ranked by a draw of their station.
    Slot* slots = out.slots.data() + line.first_slot;
    Vector<u32> eligible;
    for (u32 i = 0; i < line.slot_count; ++i)
      if (slots[i].kind == ModuleKind::run) eligible.push_back(i);
    line.eligible = static_cast<u32>(eligible.size());
    const u32 target = static_cast<u32>(
        std::floor(static_cast<f64>(layer.ruined) * static_cast<f64>(eligible.size()) + 0.5));
    Vector<u64> rank;
    for (u32 k = 0; k < eligible.size(); ++k)
      rank.push_back((hash_combine(hash_combine(layer.seed ^ 0x6a11ull, li),
                                   static_cast<u64>(slots[eligible[k]].s0_cm)) &
                      ~0xFFFFFull) |
                     k);
    std::sort(rank.begin(), rank.end());
    u32 fallen = 0;
    for (u64 r : rank) {
      if (fallen >= target) break;
      const u32 start = eligible[static_cast<u32>(r & 0xFFFFFull)];
      u32 i = start;
      for (u32 k = 0; k < layer.gap_modules && fallen < target && i < line.slot_count; ++k, ++i) {
        if (slots[i].kind != ModuleKind::run || slots[i].run != slots[start].run) break;
        slots[i].kind = ModuleKind::fallen;
        ++fallen;
      }
    }
    line.fallen = fallen;
    // A standing stretch beside a fallen one is broken toward it.
    for (u32 i = 0; i < line.slot_count; ++i) {
      if (slots[i].kind != ModuleKind::run) continue;
      const bool next = i + 1 < line.slot_count && slots[i + 1].run == slots[i].run &&
                        slots[i + 1].kind == ModuleKind::fallen;
      const bool prev =
          i > 0 && slots[i - 1].run == slots[i].run && slots[i - 1].kind == ModuleKind::fallen;
      if (next) {
        slots[i].kind = ModuleKind::broken;
      } else if (prev) {
        slots[i].kind = ModuleKind::broken;
        slots[i].head_at_end = true;
      }
    }
    out.lines.push_back(std::move(line));
  }
  // The far form's pieces: up to four standing modules of a run under one strip.
  for (u32 i = 0; i < out.slots.size();) {
    if (out.slots[i].kind == ModuleKind::fallen) {
      ++i;
      continue;
    }
    u32 j = i;
    while (j + 1 < out.slots.size() && j - i < 3 && out.slots[j + 1].line == out.slots[i].line &&
           out.slots[j + 1].run == out.slots[i].run && out.slots[j + 1].kind != ModuleKind::fallen)
      ++j;
    Piece p;
    p.x0_mm = out.slots[i].x0_mm;
    p.z0_mm = out.slots[i].z0_mm;
    p.x1_mm = out.slots[j].x1_mm;
    p.z1_mm = out.slots[j].z1_mm;
    p.id = static_cast<u32>(out.pieces.size()) + 1;
    out.pieces.push_back(p);
    i = j + 1;
  }
  for (u32 i = 0; i < out.slots.size(); ++i) {
    const Slot& s = out.slots[i];
    const scene_gen::TileCoord t =
        tile_of(floor_div(s.x0_mm + s.x1_mm, 2), floor_div(s.z0_mm + s.z1_mm, 2), tile_mm);
    out.slot_tiles.push_back({t.x, t.z, i, 0});
  }
  for (u32 i = 0; i < out.pieces.size(); ++i) {
    const Piece& p = out.pieces[i];
    const scene_gen::TileCoord t =
        tile_of(floor_div(p.x0_mm + p.x1_mm, 2), floor_div(p.z0_mm + p.z1_mm, 2), tile_mm);
    out.piece_tiles.push_back({t.x, t.z, i, 0});
  }
  std::sort(out.slot_tiles.begin(), out.slot_tiles.end(), tile_less);
  std::sort(out.piece_tiles.begin(), out.piece_tiles.end(), tile_less);
  return true;
}

const Slot* slot_at(const Plan& plan, u32 line, i64 s) noexcept {
  if (line >= plan.lines.size()) return nullptr;
  const LinePlan& l = plan.lines[line];
  const Slot* b = plan.slots.data() + l.first_slot;
  const Slot* e = b + l.slot_count;
  const Slot* it = std::upper_bound(b, e, s, [](i64 v, const Slot& x) { return v < x.s0_cm; });
  if (it == b) return nullptr;
  --it;
  return s < it->s1_cm ? it : nullptr;
}

Standing stand_slot(const Plan& plan, const Layer& layer, const Slot& slot,
                    const scene_gen::Ground& ground) noexcept {
  (void)plan;
  Standing out;
  const f64 half_width = 0.5 * static_cast<f64>(layer.base_width_cm) * 0.01 + k_scarcement_m;
  const f64 footing = static_cast<f64>(layer.footing_cm) * 0.01;
  const Chord c =
      chord_of(slot.x0_mm, slot.z0_mm, slot.x1_mm, slot.z1_mm, half_width, footing, ground);
  const f64 g = (c.h1 - c.h0) / c.length;
  out.grade = g;
  // Which way the mesh's +x runs: a head's or a broken stretch's wall end decides it; a stretch
  // and a fallen one run uphill, so their kit is solved for rising grades only.
  bool reversed = slot.head_at_end;
  if (slot.kind == ModuleKind::run || slot.kind == ModuleKind::fallen) reversed = g < 0;
  const f64 gm = reversed ? -g : g;
  const i32 lo =
      (slot.kind == ModuleKind::run || slot.kind == ModuleKind::fallen) ? 0 : -k_grade_classes;
  const i32 cls = std::clamp(
      static_cast<i32>(std::floor(gm * 1000.0 / k_grade_step_permille + 0.5)), lo, k_grade_classes);
  out.grade_class = cls;
  const f64 gk = static_cast<f64>(cls * k_grade_step_permille) / 1000.0;
  const f64 L = static_cast<f64>(layer.module_cm) * 0.01;
  // Stretch the solved module along its own x so its foot line spans the chord, then pitch it
  // from its own grade onto the ground's (at most half a class, 3.6 degrees, inside the kit).
  const f64 along = c.length * c.length * (1.0 + gm * gm) - gk * gk * L * L;
  const f64 sx = std::sqrt(std::max(along, 0.01 * L * L)) / L;
  const Quat pitch = turn_to(sx * L, gk * L, 0, 1, gm, 0);
  const f64 dx = reversed ? -c.dx : c.dx, dz = reversed ? -c.dz : c.dz;
  const Quat yaw = turn_to(1, 0, 0, dx, 0, dz);
  scene_gen::Placement& p = out.placement;
  p.position = {0.5 * static_cast<f64>(slot.x0_mm + slot.x1_mm) * 0.001,
                0.5 * (c.h0 + c.h1) - c.sink,
                0.5 * static_cast<f64>(slot.z0_mm + slot.z1_mm) * 0.001};
  p.rotation = normalize(yaw * pitch);
  p.scale = {static_cast<f32>(sx), 1.0f, 1.0f};
  p.mesh = kit_index(slot.kind, slot.variant, cls, layer.variants);
  p.tag = slot.kind == ModuleKind::fallen ? 1 : 0;
  p.id = slot.id;
  out.sink_m = c.sink;
  return out;
}

scene_gen::Placement stand_piece(const Plan& plan, const Layer& layer, const Piece& piece,
                                 const scene_gen::Ground& ground) noexcept {
  (void)plan;
  const f64 half_width = 0.5 * static_cast<f64>(layer.base_width_cm) * 0.01 + k_scarcement_m;
  const f64 footing = static_cast<f64>(layer.footing_cm) * 0.01;
  const Chord c =
      chord_of(piece.x0_mm, piece.z0_mm, piece.x1_mm, piece.z1_mm, half_width, footing, ground);
  const f64 g = (c.h1 - c.h0) / c.length;
  scene_gen::Placement p;
  p.position = {0.5 * static_cast<f64>(piece.x0_mm + piece.x1_mm) * 0.001,
                0.5 * (c.h0 + c.h1) - c.sink,
                0.5 * static_cast<f64>(piece.z0_mm + piece.z1_mm) * 0.001};
  p.rotation = normalize(turn_to(1, 0, 0, c.dx, 0, c.dz) * turn_to(1, 0, 0, 1, g, 0));
  p.scale = {static_cast<f32>(c.length * std::sqrt(1.0 + g * g)), 1.0f, 1.0f};
  p.mesh = kit_index(ModuleKind::strip, 0, 0, layer.variants);
  p.id = piece.id;
  return p;
}

}  // namespace engine::walls
