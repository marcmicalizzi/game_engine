// Fires (fire.h; docs/subsystems/renderer.md, "Fires"; roadmap R85): the scene file's block read
// and checked, a fire burnt at a clock, the nearest of a frame's fires into its light array, and
// the set of fires a renderer keeps — the scene's own and its placed props'.

#include <core/math/deterministic.h>
#include <domain/gfx/visibility_resolve.h>
#include <systems/renderer/fire.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>

#include <algorithm>
#include <cmath>
#include <scene_fire.h>
#include <string>

namespace engine::renderer {

namespace {

bool in_range(f32 v, f32 lo, f32 hi) noexcept { return std::isfinite(v) && v >= lo && v <= hi; }

// The seed's phase for sine `i`, in [0, 1): a fixed integer hash, the same on every toolchain.
f64 flicker_phase(u32 seed, u32 i) noexcept {
  u32 h = seed * 0x9E3779B9u ^ (i + 1u) * 0x85EBCA6Bu;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return static_cast<f64>(h >> 8) * (1.0 / 16777216.0);
}

}  // namespace

bool read_fire(const scene::Fire& in, FireDesc& out, std::string* error) {
  const auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (!world_cell_valid(in.position) || !std::isfinite(in.position.x) ||
      !std::isfinite(in.position.y) || !std::isfinite(in.position.z))
    return fail("position is not a position in the world");
  if (!in_range(in.radius_m, 0.01f, 20.0f)) return fail("radius_m is not in [0.01, 20]");
  if (!in_range(in.height_m, 0.01f, 50.0f)) return fail("height_m is not in [0.01, 50]");
  if (!in_range(in.intensity, 0.0f, 1.0e7f)) return fail("intensity is not in [0, 1e7]");
  if (!in_range(in.color_k, 1000.0f, 25000.0f)) return fail("color_k is not in [1000, 25000]");
  if (!in_range(in.smoke.density, 0.0f, 1.0f)) return fail("smoke.density is not in [0, 1]");
  if (!in_range(in.smoke.rise_m_s, 0.0f, 20.0f)) return fail("smoke.rise_m_s is not in [0, 20]");
  if (!in_range(in.smoke.spread, 0.0f, 5.0f)) return fail("smoke.spread is not in [0, 5]");
  if (!in_range(in.flicker, 0.0f, 1.0f)) return fail("flicker is not in [0, 1]");
  out = FireDesc{};
  out.name = in.name;
  out.position = in.position;
  out.radius_m = in.radius_m;
  out.height_m = in.height_m;
  out.intensity = in.intensity;
  out.color_k = in.color_k;
  out.smoke_density = in.smoke.density;
  out.smoke_rise_m_s = in.smoke.rise_m_s;
  out.smoke_spread = in.smoke.spread;
  out.flicker = in.flicker;
  out.seed = in.seed;
  return true;
}

bool read_scene_fires(const scene::Scene& file, const std::string& path, SceneDesc& out,
                      std::string& error) {
  out.fires.clear();
  if (file.fires.size() > k_max_fires) {
    error = path + ": fires: " + std::to_string(file.fires.size()) + " fires, more than the " +
            std::to_string(k_max_fires) + " a frame keeps";
    return false;
  }
  for (u32 i = 0; i < file.fires.size(); ++i) {
    FireDesc fire;
    std::string why;
    if (!read_fire(file.fires[i], fire, &why)) {
      error = path + ": fires " + std::to_string(i) + " (\"" + file.fires[i].name + "\"): " + why;
      return false;
    }
    out.fires.push_back(std::move(fire));
  }
  return true;
}

Vec3 blackbody_rgb(f32 kelvin) noexcept {
  // Kim, Kim, Lee, Kim and Yoon's cubic fit of the Planckian locus (2002), valid 1,667 to 25,000 K.
  const f64 t = std::clamp(static_cast<f64>(kelvin), 1667.0, 25000.0);
  const f64 i1 = 1.0e3 / t;
  const f64 i2 = i1 * i1;
  const f64 i3 = i2 * i1;
  const f64 x = t <= 4000.0 ? -0.2661239 * i3 - 0.2343589 * i2 + 0.8776956 * i1 + 0.179910
                            : -3.0258469 * i3 + 2.1070379 * i2 + 0.2226347 * i1 + 0.240390;
  const f64 x2 = x * x;
  const f64 x3 = x2 * x;
  f64 y = 0.0;
  if (t <= 2222.0) {
    y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
  } else if (t <= 4000.0) {
    y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
  } else {
    y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
  }
  const f64 cx = x / y;
  const f64 cz = (1.0 - x - y) / y;
  f64 r = 3.2404542 * cx - 1.5371385 - 0.4985314 * cz;
  f64 g = -0.9692660 * cx + 1.8760108 + 0.0415560 * cz;
  f64 b = 0.0556434 * cx - 0.2040259 + 1.0572252 * cz;
  r = std::max(r, 0.0);
  g = std::max(g, 0.0);
  b = std::max(b, 0.0);
  const f64 luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  if (!(luminance > 0.0)) return Vec3{1.0f, 1.0f, 1.0f};
  return Vec3{static_cast<f32>(r / luminance), static_cast<f32>(g / luminance),
              static_cast<f32>(b / luminance)};
}

f32 fire_flicker_signal(u32 seed, f64 time_s) noexcept {
  constexpr f64 k_turn = 6.283185307179586476925286766559;
  f64 sum = 0.0;
  for (u32 i = 0; i < 4; ++i) {
    // The cycle reduced in f64 (exact: a floor and a subtraction), so the sine's argument is in
    // [0, 2 pi) at any clock and det::sin is exact enough.
    const f64 cycles = time_s * static_cast<f64>(k_fire_flicker_hz[i]) + flicker_phase(seed, i);
    const f64 frac = cycles - std::floor(cycles);
    sum += static_cast<f64>(k_fire_flicker_weight[i]) * det::sin(k_turn * frac);
  }
  return static_cast<f32>(std::clamp(sum, -1.0, 1.0));
}

f32 fire_light_reach(f32 intensity) noexcept {
  if (!(intensity > 0.0f)) return 0.0f;
  return std::min(std::sqrt(intensity / k_fire_reach_illuminance), k_fire_max_reach_m);
}

FrameFire burn_fire(const FireDesc& desc, const WorldPos& base, f32 scale, f64 time_s,
                    f32 units) noexcept {
  FrameFire out;
  const f32 s = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
  out.base = base;
  out.radius_m = desc.radius_m * s;
  out.height_m = desc.height_m * s;
  out.base_intensity = desc.intensity * s * s;
  const f32 signal = desc.flicker > 0.0f ? fire_flicker_signal(desc.seed, time_s) : 0.0f;
  out.intensity = out.base_intensity * (1.0f + k_fire_flicker_intensity * desc.flicker * signal);
  out.color = blackbody_rgb(desc.color_k * (1.0f + k_fire_flicker_kelvin * desc.flicker * signal));
  // The reach is the brightest the flicker gets, so the window does not breathe with it.
  out.reach_m =
      fire_light_reach(out.base_intensity * (1.0f + k_fire_flicker_intensity * desc.flicker));
  out.intensity *= units;
  out.base_intensity *= units;
  out.flame_color = blackbody_rgb(desc.color_k);
  out.smoke_density = desc.smoke_density;
  out.smoke_rise_m_s = desc.smoke_rise_m_s;
  out.smoke_spread = desc.smoke_spread;
  out.flicker = desc.flicker;
  out.seed = desc.seed;
  return out;
}

u32 add_fire_lights(std::span<const FrameFire> fires, const WorldPos& eye,
                    FrameLighting& out) noexcept {
  u32 added = 0;
  // The nearest first, by selection: at most k_max_fires candidates and k_max_fire_lights picks,
  // and no allocation. A tie keeps the earlier fire.
  bool taken[k_max_fires] = {};
  const u32 n = std::min(static_cast<u32>(fires.size()), k_max_fires);
  while (added < k_max_fire_lights && out.light_count < k_frame_lights) {
    u32 best = n;
    f64 best_d = 0.0;
    for (u32 i = 0; i < n; ++i) {
      if (taken[i] || !(fires[i].intensity > 0.0f)) continue;
      const DVec3 d = fires[i].base - eye;
      const f64 d2 = d.x * d.x + d.y * d.y + d.z * d.z;
      if (best == n || d2 < best_d) {
        best = i;
        best_d = d2;
      }
    }
    if (best == n) break;
    taken[best] = true;
    const FrameFire& f = fires[best];
    const WorldPos at =
        f.base + DVec3{0.0, static_cast<f64>(k_fire_light_height * f.height_m), 0.0};
    gfx::ResolveLight& light = out.lights[out.light_count++];
    light = gfx::ResolveLight{};
    light.position_radius = Vec4{relative(at, eye), f.reach_m};
    light.color_intensity = Vec4{f.color, f.intensity};
    light.direction_type = Vec4{0.0f, 0.0f, 0.0f, gfx::k_light_point};
    ++added;
  }
  return added;
}

void FireSet::reset(const SceneData& scene) {
  fixed_.clear();
  prop_fires_.clear();
  mesh_fire_.clear();
  placed_.clear();
  placed_.reserve(k_max_fires);
  for (const FireDesc& fire : scene.fires) {
    if (fixed_.size() >= k_max_fires) break;
    fixed_.push_back(fire);
  }
  mesh_fire_.resize(scene.parts.size(), 0u);
  for (const SceneProp& prop : scene.props) {
    if (!prop.fire.has_value() || prop.mesh >= mesh_fire_.size()) continue;
    prop_fires_.push_back(*prop.fire);
    mesh_fire_[prop.mesh] = static_cast<u32>(prop_fires_.size());
  }
}

void FireSet::place(std::span<const SceneInstance> tail) noexcept {
  placed_.clear();
  if (prop_fires_.empty()) return;
  add(tail);
}

// The tail as blocks that may name their own instances (`DynamicBlock::instances`, W81), in the
// blocks' order, which is the tail's.
void FireSet::place(std::span<const SceneInstance> tail,
                    std::span<const DynamicBlock> blocks) noexcept {
  placed_.clear();
  if (prop_fires_.empty()) return;
  if (blocks.empty()) {
    add(tail);
    return;
  }
  for (const DynamicBlock& block : blocks) {
    add(block.instances != nullptr ? std::span<const SceneInstance>(block.instances, block.count)
                                   : tail.subspan(block.first, block.count));
  }
}

void FireSet::add(std::span<const SceneInstance> tail) noexcept {
  for (const SceneInstance& instance : tail) {
    if (fixed_.size() + placed_.size() >= k_max_fires) break;
    if (instance.mesh >= mesh_fire_.size() || mesh_fire_[instance.mesh] == 0) continue;
    const u32 index = mesh_fire_[instance.mesh] - 1;
    const FireDesc& fire = prop_fires_[index];
    const Transform3& t = instance.transform;
    const Vec3 offset{static_cast<f32>(fire.position.x) * t.scale.x,
                      static_cast<f32>(fire.position.y) * t.scale.y,
                      static_cast<f32>(fire.position.z) * t.scale.z};
    const Vec3 turned = rotate(t.rotation, offset) + t.position;
    Placed p;
    p.desc = index;
    p.base = instance.origin + DVec3{turned};
    p.scale = std::max(t.scale.x, std::max(t.scale.y, t.scale.z));
    placed_.push_back(p);
  }
}

void FireSet::burn(f64 time_s, f32 units, Vector<FrameFire>& out) const {
  out.clear();
  for (const FireDesc& fire : fixed_)
    out.push_back(burn_fire(fire, fire.position, 1.0f, time_s, units));
  for (const Placed& p : placed_)
    out.push_back(burn_fire(prop_fires_[p.desc], p.base, p.scale, time_s, units));
}

}  // namespace engine::renderer
