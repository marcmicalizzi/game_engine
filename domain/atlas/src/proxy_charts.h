#pragma once

// Charts cut on a simplified proxy and carried onto the full mesh (docs/subsystems/atlas.md, "Why
// a proxy"). Internal to the atlas module.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <string>

namespace engine::atlas {

// A triangle mesh over welded points: three floats a point, three points a triangle.
struct PointMesh {
  Vector<f32> positions;
  Vector<u32> indices;
  u32 point_count() const noexcept { return positions.size() / 3; }
  u32 triangle_count() const noexcept { return indices.size() / 3; }
};

// What the proxy charting hands the packer: a UV mesh with one vertex per (point, chart), in the
// charts' own parameter space (unpacked), and a chart per triangle.
struct ProxyUvMesh {
  Vector<Vec2> uv;          // per UV vertex
  Vector<u32> uv_point;     // per UV vertex: the point it is a copy of
  Vector<u32> corners;      // per triangle corner: its UV vertex
  Vector<u32> face_chart;   // per triangle: its chart
  u32 proxy_triangles = 0;  // what the simplifier reached
  u32 proxy_charts = 0;     // xatlas's charts on the proxy
  u32 target_charts = 0;    // of those, the ones fine triangles may land on
  u32 charts = 0;           // charts the full mesh ended with
  u32 reassigned = 0;       // triangles the boundary cleanup moved to a neighbouring chart
  u32 folded = 0;           // triangles whose UVs are wound against their chart
};

// Simplifies `mesh` to about `target_triangles`, lets xatlas chart and parameterize that proxy,
// gives every triangle of `mesh` the chart of the proxy triangle nearest it (facing the same
// way), cleans the chart borders up, and parameterizes each point of each chart by projecting it
// onto the proxy triangle of that chart nearest it and carrying that triangle's UVs over by
// barycentrics. Deterministic. False with a sentence when the proxy has no chart to offer.
bool proxy_uv_mesh(const PointMesh& mesh, u32 target_triangles, f32 max_chart_cost,
                   ProxyUvMesh& out, std::string& error);

}  // namespace engine::atlas
