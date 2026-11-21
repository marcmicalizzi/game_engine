#pragma once

#include "geology.h"

namespace engine::rocks::detail {

// Offline only: one bounded atlas for the whole recipe, six padded charts per body.
class SurfaceBake {
 public:
  bool init(const RockSurfaceMaps& settings, u32 bodies, std::string& error);
  Vec2 uv(u32 chart, f64 u, f64 v) const;
  void body(u32 part, u32 n, u32 first, const Fabric& fabric, RockType type,
            std::span<const D3> samples, std::span<const f64> fresh, std::span<const u8> diagonals,
            const assets::MeshData& mesh);
  bool finish(assets::MeshData& mesh, f32 normal_scale);
  // A height in place of the mineral field's (a function of the sample position), so a case can
  // bake a known slope and hold which way the map's red and green point (R202). Null, the default,
  // is the field.
  void use_height(f64 (*height)(D3)) { height_ = height; }

 private:
  f64 (*height_)(D3) = nullptr;
  RockSurfaceMaps settings_;
  u32 columns_ = 0;
  u32 charts_ = 0;
  u32 cell_ = 0;
  Vector<u8> colour_, roughness_, normal_;
};

}  // namespace engine::rocks::detail
