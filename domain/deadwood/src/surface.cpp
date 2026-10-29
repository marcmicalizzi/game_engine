#include "surface.h"

#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <domain/geometry/material_sampling.h>
#include <foundation/image/png.h>

#include <algorithm>
#include <cmath>

namespace engine::deadwood::detail {
namespace {
f64 mix(f64 a, f64 b, f64 t) { return a + (b - a) * t; }
f64 fade(f64 x) { return x * x * (3 - 2 * x); }
f64 random(u64 seed, i64 x, i64 y) {
  const u64 h = hash_combine(hash_combine(seed, static_cast<u64>(x)), static_cast<u64>(y));
  return static_cast<f64>(h >> 32) / 4294967296.0;
}
f64 noise(u64 seed, f64 x, f64 y, i64 period_x = 0, i64 period_y = 0) {
  const i64 ix = static_cast<i64>(std::floor(x)), iy = static_cast<i64>(std::floor(y));
  auto at = [&](i64 a, i64 b) {
    if (period_x) a = (a % period_x + period_x) % period_x;
    if (period_y) b = (b % period_y + period_y) % period_y;
    return random(seed, a, b);
  };
  const f64 u = fade(x - static_cast<f64>(ix)), v = fade(y - static_cast<f64>(iy));
  return mix(mix(at(ix, iy), at(ix + 1, iy), u), mix(at(ix, iy + 1), at(ix + 1, iy + 1), u), v);
}
f64 unit(f64 v) { return std::clamp(v, 0.0, 1.0); }
u8 byte(f64 v) { return static_cast<u8>(unit(v) * 255 + .5); }
struct Sample {
  f64 r, g, b, height, roughness;
};
struct Field {
  const DeadwoodFile& file;
  u32 role;
  f64 span;
  Sample at(f64 x, f64 y) const {
    const bool end = role == 1;
    const bool ring_porous =
        file.tree.species == "oak" || file.tree.species == "ash" || file.tree.species == "elm";
    const bool conifer = file.tree.species == "conifer" || file.tree.species == "pine" ||
                         file.tree.species == "spruce" || file.tree.species == "fir" ||
                         file.tree.species == "cedar" || file.tree.species == "juniper" ||
                         file.tree.species == "hemlock";
    const u64 seed = file.seed;
    const f64 warp = .004 * (noise(seed + 1, x * 4, y * 4, 4, 4) - .5);
    const f64 grain = noise(seed + 2, (x + warp) * 128, y * 8, 128, 8);
    const f64 broad = noise(seed + 3, x * 8, y * 2, 8, 2);
    f64 band = grain;
    f64 ray = 0;
    if (end) {
      const f64 px = (x - .5) * span, py = (y - .5) * span;
      const f64 radius = std::sqrt(px * px + py * py);
      const f64 uneven = .0015 * (noise(seed + 4, px * 18, py * 18) - .5);
      const f64 phase = (radius + uneven) / .006;
      const f64 ring = .5 + .5 * det::sin(phase * 6.283185307179586);
      // Filter unresolved rings instead of claiming micrometre vessels at metre tile sizes.
      const f64 resolved = unit((.006 * file.surface_resolution / span - 2) * .5);
      band = mix(.5, ring * ring * ring, resolved);
      if (ring_porous && radius > .001) {
        ray = unit((noise(seed + 5, px / radius * 32, py / radius * 32) - .7) * 4);
      }
    }
    const f64 fibre = unit((grain - .58) * 2.4);
    f64 decay_relief = fibre;
    if (file.decay_pattern == DecayPattern::cubical) {
      const f64 u = x * 16 + .35 * (noise(seed + 6, x * 8, y * 8, 8, 8) - .5);
      const f64 v = y * 8 + .35 * (noise(seed + 7, x * 8, y * 8, 8, 8) - .5);
      const f64 d = std::min(std::fabs(u - std::floor(u + .5)), std::fabs(v - std::floor(v + .5)));
      decay_relief = unit((.1 - d) * 10);
    }
    const f64 decay = static_cast<f64>(file.decay);
    const f64 contrast = ring_porous ? .22 : (conifer ? .16 : .08);
    const f64 pigment = 1 - contrast * band - .1 * fibre - .1 * ray + .1 * (broad - .5);
    f64 r = conifer ? .76 : (ring_porous ? .69 : .79);
    f64 g = conifer ? .64 : (ring_porous ? .53 : .69);
    f64 b = conifer ? .44 : (ring_porous ? .35 : .51);
    if (file.tree.species == "walnut") {
      r = .50;
      g = .36;
      b = .24;
    }
    if (file.tree.species == "alder") {
      r = .75;
      g = .55;
      b = .39;
    }
    const bool brown = file.decay_pattern == DecayPattern::cubical;
    r = mix(r, brown ? .38 : .66, decay);
    g = mix(g, brown ? .25 : .62, decay);
    b = mix(b, brown ? .16 : .51, decay);
    const f64 shade = pigment * (1 - decay * .35 * decay_relief) * (role == 2 ? .64 : 1);
    return {r * shade, g * shade, b * shade,
            -.0005 * fibre - .00025 * band - decay * .002 * decay_relief -
                (role == 2 ? .001 * fibre : 0),
            unit(.73 + .12 * grain + .12 * decay)};
  }
};
}  // namespace
f32 bark_field(Vec2 uv, u64 seed) {
  const f64 u = static_cast<f64>(uv.x), v = static_cast<f64>(uv.y);
  return static_cast<f32>(.75 * noise(seed + 91, u * 5, v * 1.5) +
                          .25 * noise(seed + 97, u * 17, v * 6));
}
bool bake_surface(const DeadwoodFile& file, u32 role, u32 material, f32 end_span,
                  geometry::ClusterFileData& out, std::string& error) {
  if (file.surface_resolution == 0) return true;
  const u32 size = file.surface_resolution;
  const f64 step = 1.0 / size;
  const Field field{file, role, static_cast<f64>(end_span)};
  Vector<u8> colour(size * size * 3), normal(colour.size()), roughness(colour.size());
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      const f64 u = (x + .5) * step, v = (y + .5) * step;
      const auto sample = field.at(u, v);
      const f64 physical_step = step * (role == 1 ? field.span : 1);
      const f64 dx =
          (field.at(u + step, v).height - field.at(u - step, v).height) / (2 * physical_step);
      const f64 dy =
          (field.at(u, v + step).height - field.at(u, v - step).height) / (2 * physical_step);
      const f64 scale = 1 / std::sqrt(dx * dx + dy * dy + 1);
      const u32 at = (y * size + x) * 3;
      colour[at] = byte(sample.r);
      colour[at + 1] = byte(sample.g);
      colour[at + 2] = byte(sample.b);
      normal[at] = byte(.5 - dx * scale * .5);
      normal[at + 1] = byte(.5 - dy * scale * .5);
      normal[at + 2] = byte(.5 + scale * .5);
      roughness[at] = 255;
      roughness[at + 1] = byte(sample.roughness);
      roughness[at + 2] = 0;
    }
  }
  const i32 first = static_cast<i32>(out.image_paths.size());
  out.images.resize(out.image_paths.size());
  for (const auto* pixels : {&colour, &normal, &roughness}) {
    geometry::ClusterImage image;
    image.mime_type = "image/png";
    if (!image::encode_png(size, size, 3, *pixels, image.bytes, image::Compression::Fast)) {
      error = "deadwood surface PNG encoding failed";
      return false;
    }
    out.image_paths.push_back({});
    out.images.push_back(std::move(image));
  }
  auto& m = out.materials[material];
  m.base_color = {1, 1, 1, 1};
  m.roughness = 1;
  m.normal_scale = 1;
  m.base_color_image = first;
  m.normal_image = first + 1;
  m.metallic_roughness_image = geometry::encode_optional_image(first + 2);
  if (role == 1) {
    for (u32 slot = 0; slot < 3; ++slot) {
      auto& sampling = out.material_sampling[material].slots[slot];
      sampling.scale[0] = sampling.scale[1] = 1 / end_span;
      sampling.offset[0] = sampling.offset[1] = .5f;
      geometry::TextureSampler sampler;
      sampler.wrap_s = sampler.wrap_t = geometry::TextureWrap::clamp_to_edge;
      sampling.sampler = geometry::pack_texture_sampler(sampler);
    }
  }
  return true;
}
}  // namespace engine::deadwood::detail
