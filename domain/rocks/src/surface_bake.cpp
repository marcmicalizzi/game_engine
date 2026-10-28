#include "surface_bake.h"

#include <domain/texture/texture_build.h>
#include <foundation/image/png.h>

#include <algorithm>
#include <utility>

namespace engine::rocks::detail {
namespace {
constexpr u32 k_pad = 4;
u8 unorm(f64 x) { return static_cast<u8>(clamp01(x) * 255.0 + 0.5); }
D3 position(Vec3 p) { return {wide(p.x), wide(p.y), wide(p.z)}; }
struct Sample {
  D3 colour;
  f64 roughness;
  f64 height;
};
struct Field {
  const RockSurfaceMaps& settings;
  const Fabric& fabric;
  RockType type;
  D3 base;
  f64 roughness;
  D3 vein_axis;

  Sample at(D3 p, f64 footprint) const {
    const f64 grain = wide(settings.grain_m);
    // Nyquist is insufficient for crisp mineral boundaries: fade over 2..4 texels.
    const f64 resolved = clamp01((grain / footprint - 2.0) * 0.5);
    // Rotate and warp the grain domain: sharpening an axis-aligned value lattice prints
    // square checks onto broad fracture planes, even when the texture has ample resolution.
    D3 grain_p{0.8 * p.x + 0.6 * p.z, 0.36 * p.x + 0.8 * p.y - 0.48 * p.z,
               -0.48 * p.x + 0.6 * p.y + 0.64 * p.z};
    grain_p = grain_p * (1.0 / grain);
    const D3 warp{noise3(fabric.seed + 83, grain_p * 0.7) - 0.5,
                  noise3(fabric.seed + 89, grain_p * 0.7) - 0.5,
                  noise3(fabric.seed + 101, grain_p * 0.7) - 0.5};
    grain_p = grain_p + warp * 1.1;
    const f64 mineral = noise3(fabric.seed + 73, grain_p);
    const f64 fine = noise3(fabric.seed + 79, grain_p * 2.0) - 0.5;
    const f64 grains = (fade(clamp01((mineral - 0.25) * 2.0)) - 0.5) * resolved +
                       0.3 * fine * clamp01((grain / footprint - 4.0) * 0.25);
    const f64 broad = noise3(fabric.seed + 19, p * 2.7) - 0.5;
    f64 bands = 0.0;
    if (fabric.model.bed > 0.0) {
      const f64 phase = fabric_coordinate(fabric, p) / fabric.model.bed +
                        0.08 * (noise3(fabric.seed + 61, p * (2.0 / fabric.model.bed)) - 0.5);
      const f64 amplitude = type == RockType::Gneiss ? 0.65 : 0.15;
      const f64 minimum_bed = fabric.model.bed * (1.0 - 0.56 * fabric.irregularity);
      bands = amplitude * det::sin(phase * 2.0 * k_pi) * (0.65 + 0.7 * broad) *
              clamp01((minimum_bed / footprint - 2.0) * 0.5);
    }
    const f64 spacing = wide(settings.vein_spacing_m);
    const f64 width = wide(settings.vein_width_m);
    const f64 q =
        dot(p, vein_axis) / spacing + 0.3 * (noise3(fabric.seed + 97, p * (1.5 / spacing)) - 0.5);
    const f64 distance = std::fabs(q - std::floor(q + 0.5)) * spacing;
    const f64 vein = wide(settings.veins) *
                     clamp01((width * 0.5 + footprint - distance) / (2.0 * footprint)) *
                     clamp01(width / footprint);
    const f64 variation = wide(settings.variation);
    const f64 pigment = 1.0 + variation * (0.65 * broad + 0.65 * grains + bands);
    D3 vein_colour{0.75, 0.72, 0.65};
    if (settings.vein_tint)
      vein_colour = {wide((*settings.vein_tint)[0]), wide((*settings.vein_tint)[1]),
                     wide((*settings.vein_tint)[2])};
    const D3 body = base * pigment;
    const f64 r = roughness + variation * (0.15 * grains + (0.12 + 0.08 * fabric.exposure) * broad);
    return {body * (1.0 - vein) + vein_colour * vein, std::clamp(lerp(r, 0.38, vein), 0.02, 1.0),
            grain * 0.055 * fabric.model.granular * grains * variation};
  }
};
}  // namespace

bool SurfaceBake::init(const RockSurfaceMaps& settings, u32 bodies, std::string& error) {
  settings_ = settings;
  charts_ = bodies * 6u;
  columns_ = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(bodies * 6u))));
  cell_ = settings.resolution / columns_;
  if (cell_ < 24) {
    error =
        "material.surface_maps.resolution leaves fewer than 16 interior texels per chart; "
        "increase resolution or reduce body count";
    return false;
  }
  const u32 bytes = settings.resolution * settings.resolution * 3u;
  colour_.assign(bytes, 0);
  roughness_.assign(bytes, 0);
  normal_.assign(bytes, 128);
  for (u32 i = 0; i < bytes; i += 3) {
    roughness_[i] = 255;
    roughness_[i + 1] = 255;
    normal_[i + 2] = 255;
  }
  return true;
}

Vec2 SurfaceBake::uv(u32 chart, f64 u, f64 v) const {
  const f64 extent = static_cast<f64>(cell_ - 2u * k_pad - 1u);
  return {
      static_cast<f32>((static_cast<f64>((chart % columns_) * cell_ + k_pad) + 0.5 + u * extent) /
                       settings_.resolution),
      static_cast<f32>((static_cast<f64>((chart / columns_) * cell_ + k_pad) + 0.5 + v * extent) /
                       settings_.resolution)};
}

void SurfaceBake::body(u32 part, u32 n, u32 first, const Fabric& fabric, RockType type,
                       std::span<const D3> samples, std::span<const f64> fresh,
                       std::span<const u8> diagonals, const assets::MeshData& mesh) {
  Rng rng(fabric.seed + 0x5645494Eu);
  const assets::Material& material = mesh.materials[0];
  const Field field{
      settings_,
      fabric,
      type,
      {wide(material.base_color.x), wide(material.base_color.y), wide(material.base_color.z)},
      wide(material.roughness),
      rng.direction()};
  const u32 side = n + 1;
  const f64 extent = static_cast<f64>(cell_ - 2u * k_pad - 1u);
  for (u32 f = 0; f < 6; ++f) {
    const u32 chart = part * 6u + f;
    for (u32 y = 0; y < cell_; ++y) {
      for (u32 x = 0; x < cell_; ++x) {
        // Padding repeats the edge sample, never another face's texels.
        const f64 u = clamp01((static_cast<f64>(x) - k_pad) / extent) * n;
        const f64 v = clamp01((static_cast<f64>(y) - k_pad) / extent) * n;
        const u32 i = std::min(static_cast<u32>(u), n - 1u);
        const u32 j = std::min(static_cast<u32>(v), n - 1u);
        const f64 a = u - i, b = v - j;
        const u32 q = f * side * side + j * side + i;
        // Exactly the solver's selected diagonal, including its derivatives.
        u32 corners[3];
        f64 weights[3];
        D3 du, dv, wu, wv;
        const auto s = [&](u32 index) { return samples[index]; };
        const auto w = [&](u32 index) { return position(mesh.positions[first + index]); };
        const bool main = diagonals[(f * n + j) * n + i] != 0;
        if (main && a >= b) {
          corners[0] = q;
          corners[1] = q + 1;
          corners[2] = q + side + 1;
          weights[0] = 1.0 - a;
          weights[1] = a - b;
          weights[2] = b;
          du = s(q + 1) - s(q);
          dv = s(q + side + 1) - s(q + 1);
          wu = w(q + 1) - w(q);
          wv = w(q + side + 1) - w(q + 1);
        } else if (main) {
          corners[0] = q;
          corners[1] = q + side + 1;
          corners[2] = q + side;
          weights[0] = 1.0 - b;
          weights[1] = a;
          weights[2] = b - a;
          du = s(q + side + 1) - s(q + side);
          dv = s(q + side) - s(q);
          wu = w(q + side + 1) - w(q + side);
          wv = w(q + side) - w(q);
        } else if (a + b <= 1.0) {
          corners[0] = q;
          corners[1] = q + 1;
          corners[2] = q + side;
          weights[0] = 1.0 - a - b;
          weights[1] = a;
          weights[2] = b;
          du = s(q + 1) - s(q);
          dv = s(q + side) - s(q);
          wu = w(q + 1) - w(q);
          wv = w(q + side) - w(q);
        } else {
          corners[0] = q + 1;
          corners[1] = q + side + 1;
          corners[2] = q + side;
          weights[0] = 1.0 - b;
          weights[1] = a + b - 1.0;
          weights[2] = 1.0 - a;
          du = s(q + side + 1) - s(q + side);
          dv = s(q + side + 1) - s(q + 1);
          wu = w(q + side + 1) - w(q + side);
          wv = w(q + side + 1) - w(q + 1);
        }
        D3 p{};
        f64 fracture = 0.0;
        for (u32 c = 0; c < 3; ++c) {
          p = p + s(corners[c]) * weights[c];
          fracture += fresh[corners[c]] * weights[c];
        }
        const f64 footprint = std::max(1e-6, std::max(length(du), length(dv)) * n / extent);
        Sample value = field.at(p, footprint);
        f64 grain_scale = 1.0;
        if (settings_.rind) {
          const RockRind& rind = *settings_.rind;
          const f64 coating = wide(rind.amount) * fabric.exposure * (1.0 - clamp01(fracture));
          D3 tint{0.18, 0.12, 0.07};
          if (rind.tint)
            tint = {wide((*rind.tint)[0]), wide((*rind.tint)[1]), wide((*rind.tint)[2])};
          // A coating overlays the existing mineral field; it never reseeds bands or veins.
          value.colour = value.colour * (1.0 - coating) + tint * coating;
          value.roughness = lerp(value.roughness, wide(rind.roughness), coating);
          grain_scale = lerp(1.0, wide(rind.grain_scale), coating);
        }
        const D3 tangent = normalized(wu);
        const D3 bitangent = normalized(cross(normalized(cross(wu, wv)), tangent));
        const D3 st = du * (1.0 / std::max(length(wu), 1e-9));
        const D3 sb = (dv - st * dot(wv, tangent)) * (1.0 / std::max(dot(wv, bitangent), 1e-9));
        const f64 step = std::max(footprint, wide(settings_.grain_m) * 0.125);
        const f64 dx = (field.at(p + st * step, footprint).height -
                        field.at(p - st * step, footprint).height) /
                       (2.0 * step);
        const f64 dy = (field.at(p + sb * step, footprint).height -
                        field.at(p - sb * step, footprint).height) /
                       (2.0 * step);
        // Change grain response, not the height of the coating boundary itself.
        const D3 nn = normalized(D3{-dx * grain_scale, -dy * grain_scale, 1.0});
        const u32 pixel = (((chart / columns_) * cell_ + y) * settings_.resolution +
                           (chart % columns_) * cell_ + x) *
                          3u;
        colour_[pixel] = texture::linear_to_srgb8(static_cast<f32>(value.colour.x));
        colour_[pixel + 1] = texture::linear_to_srgb8(static_cast<f32>(value.colour.y));
        colour_[pixel + 2] = texture::linear_to_srgb8(static_cast<f32>(value.colour.z));
        roughness_[pixel + 1] = unorm(value.roughness);
        normal_[pixel] = unorm(nn.x * 0.5 + 0.5);
        normal_[pixel + 1] = unorm(nn.y * 0.5 + 0.5);
        normal_[pixel + 2] = unorm(nn.z * 0.5 + 0.5);
      }
    }
  }
}

bool SurfaceBake::finish(assets::MeshData& mesh, f32 normal_scale) {
  // Empty atlas cells and the division remainder must not turn distant mips black.
  // Duplicate a filled cell, clamping the right/bottom remainder to its last texel.
  for (u32 y = 0; y < settings_.resolution; ++y) {
    for (u32 x = 0; x < settings_.resolution; ++x) {
      const u32 col = std::min(x / cell_, columns_ - 1u);
      const u32 row = std::min(y / cell_, columns_ - 1u);
      const u32 chart = row * columns_ + col;
      if (chart < charts_ && x < columns_ * cell_ && y < columns_ * cell_) continue;
      const u32 source = std::min(chart, charts_ - 1u);
      const u32 sx = (source % columns_) * cell_ + std::min(x - col * cell_, cell_ - 1u);
      const u32 sy = (source / columns_) * cell_ + std::min(y - row * cell_, cell_ - 1u);
      const u32 from = (sy * settings_.resolution + sx) * 3u;
      const u32 to = (y * settings_.resolution + x) * 3u;
      for (u32 c = 0; c < 3; ++c) {
        colour_[to + c] = colour_[from + c];
        roughness_[to + c] = roughness_[from + c];
        normal_[to + c] = normal_[from + c];
      }
    }
  }
  const Vector<u8>* pixels[] = {&colour_, &roughness_, &normal_};
  const char* names[] = {"rock-base-color", "rock-metallic-roughness", "rock-normal"};
  const i32 first = static_cast<i32>(mesh.images.size());
  for (u32 i = 0; i < 3; ++i) {
    assets::ImageRef image;
    image.name = names[i];
    image.mime_type = "image/png";
    if (!image::encode_png(settings_.resolution, settings_.resolution, 3, *pixels[i], image.bytes,
                           image::Compression::Fast))
      return false;
    mesh.images.push_back(std::move(image));
  }
  assets::Material& m = mesh.materials[0];
  m.base_color = Vec4{1, 1, 1, 1};
  m.roughness = 1;
  m.metallic = 0;
  m.base_color_image = first;
  m.metallic_roughness_image = first + 1;
  m.normal_image = first + 2;
  m.normal_scale = normal_scale;
  return true;
}
}  // namespace engine::rocks::detail
