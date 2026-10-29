#pragma once
#include <core/hash/hash.h>
#include <domain/soil/soil.h>

#include <algorithm>
#include <cmath>
namespace engine::soil::detail {
inline f32 smooth(f32 t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
inline f32 taper(const SoilFile& f, f32 x) {
  const f32 u = x / f.width_m + 0.5f;
  return smooth(u / 0.28f) * smooth((1 - u) / 0.38f);
}
// Shared by the mesh, its attachments and excavation. Smooth seeded stations,
// not a hydraulic prediction. Sampling density never changes the planform.
inline f32 noise(u64 seed, f32 t) {
  const f32 q = std::max(0.0f, t);
  const u32 i = static_cast<u32>(q);
  const auto at = [&](u32 k) { return static_cast<f32>(mix64(seed + k) >> 40) / 8388608.0f - 1; };
  const f32 u = smooth(q - static_cast<f32>(i));
  return at(i) * (1 - u) + at(i + 1) * u;
}
inline f32 planform(const SoilFile& f, f32 x) {
  const f32 q = (x + f.width_m * 0.5f) / 2.5f;
  return std::min({0.8f, f.width_m * 0.12f, f.length_m * 0.22f}) *
         (noise(f.seed + 810, q) - noise(f.seed + 810, f.width_m / 5)) * taper(f, x);
}
inline f32 half_channel(const SoilFile& f, f32 x) {
  const f32 q = (x + f.width_m * 0.5f) / 1.7f;
  return f.length_m * 0.5f * (0.85f + 0.15f * noise(f.seed + 910, q));
}
inline f32 bank_line(const SoilFile& f, f32 x, f32 facing) {
  return planform(f, x) - (f.kind == Kind::Gully ? facing * half_channel(f, x) : 0);
}
inline f32 recess(const SoilFile& f) { return f.kind == Kind::Undercut ? f.undercut_m : 0.0f; }
inline f32 back(const SoilFile& f) { return recess(f) + 0.5f; }
inline f32 radius(const SoilFile& f) {
  return std::max(f.width_m * 0.5f + 1.0f, f.length_m + 4.0f);
}
}  // namespace engine::soil::detail
