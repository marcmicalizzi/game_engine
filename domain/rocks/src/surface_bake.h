#pragma once

#include "geology.h"

namespace engine::rocks::detail {

// Offline only: one bounded atlas for the whole recipe, six padded charts per body.
class SurfaceBake {
 public:
  bool init(const RockSurfaceMaps& settings, u32 bodies, std::string& error);
  Vec2 uv(u32 chart, f64 u, f64 v) const;
  void body(u32 part, u32 n, u32 first, const Fabric& fabric, RockType type,
            std::span<const D3> samples, std::span<const f64> fresh, const assets::MeshData& mesh);
  bool finish(assets::MeshData& mesh, f32 normal_scale);

 private:
  RockSurfaceMaps settings_;
  u32 columns_ = 0;
  u32 charts_ = 0;
  u32 cell_ = 0;
  Vector<u8> colour_, roughness_, normal_;
};

}  // namespace engine::rocks::detail
