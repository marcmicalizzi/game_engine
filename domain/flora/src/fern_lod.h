#pragma once

#include "ground_plant.h"

#include <domain/geometry/cluster_lod.h>

#include <algorithm>
#include <cmath>

namespace engine::flora::detail {

// A content-build rung for the opt-in pinnate fern. Every pinna is copied exactly; only the
// millimetre-wide rachis loses curve rows. Generic decimation deletes these disconnected cards.
struct FernLod {
  Vector<u32> fine_element;
  Vector<u32> indices;
  Vector<u32> coarse_element;
  u32 elements = 0;
  f32 error = 0.0f;
};

inline FernLod fern_lod(const assets::MeshData& mesh, std::span<const u32> fine, u32 pinnae) {
  FernLod out;
  const u32 rows = fern_rachis_rows(pinnae);
  const u32 rachis_triangles = 2u * rows - 1u;
  const u32 triangles = fern_pinnate_triangles(pinnae);
  out.elements = static_cast<u32>(fine.size()) / (3u * triangles);
  for (u32 frond = 0; frond < out.elements; ++frond) {
    const u32 first = frond * triangles * 3u;
    const u32 base = fine[first];  // build_blade's first left vertex; face() never swaps it
    const auto centre = [&](u32 row) {
      return row == rows
                 ? mesh.positions[base + 2u * rows]
                 : (mesh.positions[base + 2u * row] + mesh.positions[base + 2u * row + 1u]) * 0.5f;
    };
    const auto triangle = [&](u32 a, u32 b, u32 c, Vec3 facing) {
      out.indices.push_back(a);
      if (dot(cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]),
              facing) < 0.0f)
        std::swap(b, c);
      out.indices.push_back(b);
      out.indices.push_back(c);
      out.coarse_element.push_back(frond);
    };
    for (u32 row = 0; row < rows;) {
      const u32 next = std::min(row + 4u, rows);
      const u32 left = base + 2u * row;
      const u32 end = base + 2u * next;
      const u32 source = first + 6u * row;
      const Vec3 facing = cross(mesh.positions[fine[source + 1u]] - mesh.positions[fine[source]],
                                mesh.positions[fine[source + 2u]] - mesh.positions[fine[source]]);
      if (next == rows) {
        triangle(left, left + 1u, end, facing);
      } else {
        triangle(left, left + 1u, end + 1u, facing);
        triangle(left, end + 1u, end, facing);
      }
      // Distance to the coarse strip's centreline bounds the deviation of every fine vertex.
      // The strip is planar (one frond-wide lateral axis); its centreline lies on its triangles.
      const Vec3 a = centre(row);
      const Vec3 ab = centre(next) - a;
      const f32 squared = dot(ab, ab);
      for (u32 r = row; r <= next; ++r) {
        for (u32 side = 0; side < (r == rows ? 1u : 2u); ++side) {
          const Vec3 p = mesh.positions[base + 2u * r + side];
          const f32 t = squared > 0.0f ? std::clamp(dot(p - a, ab) / squared, 0.0f, 1.0f) : 0.0f;
          const Vec3 delta = p - (a + ab * t);
          out.error = std::max(out.error, std::sqrt(dot(delta, delta)));
        }
      }
      row = next;
    }
    for (u32 i = rachis_triangles * 3u; i < triangles * 3u; ++i)
      out.indices.push_back(fine[first + i]);
    for (u32 i = rachis_triangles; i < triangles; ++i)
      out.coarse_element.push_back(frond);
    for (u32 i = 0; i < triangles; ++i)
      out.fine_element.push_back(frond);
  }
  return out;
}

}  // namespace engine::flora::detail
