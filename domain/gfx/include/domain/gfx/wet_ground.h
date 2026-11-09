#pragma once

// **Wet ground** (roadmap S99; docs/subsystems/terrain.md, "Wet ground"; docs/experiments/shore.md,
// "Wet ground"): the CPU half of the model `shaders/wet_ground.slang` applies — the capillary
// fringe's height by the bank's material, which a channel's bank record carries, and the spray band
// a rapid or a fall adds over it. The per-pixel response is the shader's, mirrored in double by the
// tests' `wet_reference.h`.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/visibility_resolve.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace engine::gfx {

// The materials a bank's fringe is known for, by grain size.
enum class BankMaterial : u8 {
  Gravel,      // fine gravel, 2-5 mm
  CoarseSand,  // 0.5-1 mm
  Sand,        // medium sand, 0.25-0.5 mm
  FineSand,    // 0.1-0.25 mm
  Silt,        // 0.002-0.05 mm
};

// **The capillary rise above a still water table**, metres, by material: Lane and Washburn's (1946)
// columns at a porosity near 41% after 72 days, as Lohman (1972, USGS Professional Paper 708) and
// Fetter tabulate them — fine gravel 2.5 cm, coarse sand 13.5, medium sand 24.6, fine sand 42.8,
// silt 105.5. The ranges other tables give (gravel 2-10 cm, sand 0.1-1 m, silt 1-3 m and more;
// shore.md) bracket each. The saturation falls over this height (wet_ground.slang's fringe).
inline constexpr f32 k_capillary_fringe_m[] = {0.025f, 0.135f, 0.246f, 0.428f, 1.055f};

constexpr f32 capillary_fringe_m(BankMaterial m) noexcept {
  return k_capillary_fringe_m[static_cast<u8>(m)];
}

// A material pack's role (terrain layers and the pack name these: "sand", "gravel", "soil") as a
// bank material. "soil" is a loam, whose rise lies between fine sand's and silt's: fine sand's,
// the lower, is taken. Anything else is `k_bank_default`.
inline constexpr BankMaterial k_bank_default = BankMaterial::FineSand;
constexpr BankMaterial bank_material_of_role(std::string_view role) noexcept {
  if (role == "gravel") return BankMaterial::Gravel;
  if (role == "sand") return BankMaterial::Sand;
  if (role == "silt" || role == "mud") return BankMaterial::Silt;
  return k_bank_default;
}

// **The spray band** over a bank where the water breaks: a rapid (Froude number above 1) wets its
// banks `k_spray_rapid_m` higher; a fall wets them by `k_spray_fall_share` of its drop, at most
// `k_spray_fall_max_m`. Stated choices, not measurements (shore.md says what was looked for).
inline constexpr f32 k_spray_rapid_m = 0.25f;
inline constexpr f32 k_spray_fall_share = 0.5f;
inline constexpr f32 k_spray_fall_max_m = 3.0f;

// The vertical slack a stretch's tree node keeps below its lowest surface: the shader's reach
// under a channel's surface (water.slang's `water_channel_saturation`, two metres).
inline constexpr f32 k_bank_below_m = 2.0f;
// The bank's run per metre of rise (water.slang's `k_bank_run`): a stretch's reach and its square
// end where the run alone has dried the ground, `k_bank_run` times its fringe past its half width.
inline constexpr f32 k_bank_run = 25.0f;
// A bank's saturation stays below the film's threshold (wet_ground.slang's `k_wet_film_from`).
inline constexpr f32 k_bank_saturation_max = 0.85f;

// **The banks' tree** (water.md, "Channels"): a scene's channel stretches are tens of thousands
// (the v35 forest: 60,477 in 156 channels), too many for a ground pixel to test one by one, so the
// GPU scene writes them after the bodies' records as a bounding-volume tree in depth-first order
// with escape indices — a node is a record whose `stretch_a.w` is -1, its square `lo`/`hi`, its
// heights (lowest less `k_bank_below_m`, highest plus the fringe) in `stretch_a.x`/`.y`, and in
// `material` the index to go on at when a point is outside it; a leaf is a stretch, gone on from
// by one. Every stretch is local to one instance (`reference`'s, the first channel's), so the tree
// has one frame. The tree is appended to `out`, the whole buffer (the bodies' records before it),
// so an escape index is an index into the buffer. Median splits on the longest axis of the
// centres, so the depth is about log2 of the count.
inline void build_bank_tree(Vector<WaterShore>& leaves, u32 reference, u32 terrain_mesh,
                            Vector<WaterShore>& out) {
  struct Builder {
    Vector<WaterShore>& leaves;
    Vector<WaterShore>& out;
    u32 reference;
    u32 terrain_mesh;
    void bounds(u32 lo, u32 hi, Vec2& a, Vec2& b, f32& y0, f32& y1) const {
      a = Vec2{1e30f, 1e30f};
      b = Vec2{-1e30f, -1e30f};
      y0 = 1e30f;
      y1 = -1e30f;
      for (u32 i = lo; i < hi; ++i) {
        const WaterShore& s = leaves[i];
        a = Vec2{std::min(a.x, s.lo.x), std::min(a.y, s.lo.y)};
        b = Vec2{std::max(b.x, s.hi.x), std::max(b.y, s.hi.y)};
        y0 = std::min(y0, std::min(s.stretch_a.z, s.stretch_b.z) - k_bank_below_m);
        y1 = std::max(y1, std::max(s.stretch_a.z, s.stretch_b.z) + s.wet_m);
      }
    }
    void build(u32 lo, u32 hi) {
      if (hi - lo == 1) {
        WaterShore leaf = leaves[lo];
        leaf.instance = reference;
        leaf.terrain_mesh = terrain_mesh;
        leaf.material = 0xffffffffu;
        out.push_back(leaf);
        return;
      }
      WaterShore node;
      node.instance = reference;
      node.terrain_mesh = terrain_mesh;
      f32 y0 = 0.0f, y1 = 0.0f;
      bounds(lo, hi, node.lo, node.hi, y0, y1);
      node.stretch_a = Vec4{y0, y1, 0.0f, -1.0f};
      const u32 at = out.size();
      out.push_back(node);
      // The longest axis of the centres, split at the median.
      Vec2 c0{1e30f, 1e30f}, c1{-1e30f, -1e30f};
      for (u32 i = lo; i < hi; ++i) {
        const Vec2 c{0.5f * (leaves[i].lo.x + leaves[i].hi.x),
                     0.5f * (leaves[i].lo.y + leaves[i].hi.y)};
        c0 = Vec2{std::min(c0.x, c.x), std::min(c0.y, c.y)};
        c1 = Vec2{std::max(c1.x, c.x), std::max(c1.y, c.y)};
      }
      const bool along_x = (c1.x - c0.x) >= (c1.y - c0.y);
      const auto key = [along_x](const WaterShore& s) {
        return along_x ? s.lo.x + s.hi.x : s.lo.y + s.hi.y;
      };
      const u32 mid = lo + (hi - lo) / 2;
      std::nth_element(leaves.begin() + lo, leaves.begin() + mid, leaves.begin() + hi,
                       [&](const WaterShore& a, const WaterShore& b) { return key(a) < key(b); });
      build(lo, mid);
      build(mid, hi);
      out[at].material = out.size();
    }
  };
  if (leaves.empty()) return;
  Builder b{leaves, out, reference, terrain_mesh};
  b.build(0, leaves.size());
}

// The saturation the shader's traversal finds at a point local to the tree's instance, walked from
// buffer index `first` as `water_ground_wetness` walks it (the tests' mirror of the order and the
// formula, in float): the number of records visited in `visits`.
inline f32 bank_tree_saturation(const Vector<WaterShore>& tree, Vec3 local, u32 first,
                                u32* visits = nullptr) {
  f32 best = 0.0f;
  u32 seen = 0;
  for (u32 i = first; i < tree.size();) {
    ++seen;
    const WaterShore& s = tree[i];
    const bool inside =
        local.x >= s.lo.x && local.z >= s.lo.y && local.x <= s.hi.x && local.z <= s.hi.y;
    if (s.stretch_a.w < 0.0f) {
      const bool level = local.y >= s.stretch_a.x && local.y <= s.stretch_a.y;
      i = inside && level ? i + 1 : s.material;
      continue;
    }
    if (inside) {
      const f32 ax = s.stretch_a.x, az = s.stretch_a.y;
      const f32 dx = s.stretch_b.x - ax, dz = s.stretch_b.y - az;
      const f32 len2 = dx * dx + dz * dz;
      f32 u = len2 > 0.0f ? ((local.x - ax) * dx + (local.z - az) * dz) / len2 : 0.0f;
      u = std::clamp(u, 0.0f, 1.0f);
      const f32 half = s.stretch_a.w + (s.stretch_b.w - s.stretch_a.w) * u;
      const f32 ex = local.x - (ax + dx * u), ez = local.z - (az + dz * u);
      const f32 across = std::sqrt(ex * ex + ez * ez);
      const f32 h = local.y - (s.stretch_a.z + (s.stretch_b.z - s.stretch_a.z) * u);
      if (across < half + k_bank_run * s.wet_m && h > -k_bank_below_m) {
        // Held at least the run from the wetted edge, and below the film (d90f9780).
        const f32 height = std::max(h, std::max(across - half, 0.0f) / k_bank_run);
        f32 sat = 1.0f;
        if (height > 0.0f) {
          const f32 t = std::clamp(height / s.wet_m, 0.0f, 1.0f);
          sat = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        best = std::max(best, std::min(sat, k_bank_saturation_max));
      }
    }
    ++i;
    if (best >= k_bank_saturation_max) break;  // as the shader's walk ends
  }
  if (visits != nullptr) *visits = seen;
  return best;
}

}  // namespace engine::gfx
