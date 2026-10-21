#pragma once

// The host-surface foundation (docs/subsystems/cover.md, "The host"): what both solvers — the vine
// (vine.h) and the cover (cover.h) — read of the mesh they grow over. From the host's triangles
// (`assets::GeneratorHostMesh`: a container's finest level, a glTF's import) it builds
//
//   a BVH       for the closest point on the surface, its normal and its triangle (the vine's
//               adhesion, the cover's seeding), and for occlusion rays (the crevice term, the
//               sun's self-shadow)
//   a weld      vertices at the same position are one, so the mask is smoothed across the seams a
//               cluster build or a part boundary leaves
//   exposure    per triangle and per welded vertex: up-ness, north-ness, the sun's direct light
//               with the host's own shadow and the canopy's shade, the sky the surface sees (an
//               ambient-occlusion proxy over `crevice_radius`: its complement is crevice-ness),
//               the height above the ground and the distance to water
//
// **Deterministic**: f64 arithmetic with IEEE `+ - * /` and `sqrt` and the deterministic sines,
// the BVH split by a total order (centroid, then index), every query visiting children in a fixed
// order and breaking ties by the lower triangle, so the same host gives the same bytes on every
// toolchain. The host is read, never written.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/mesh_generator.h>

#include <schemas/cover.h>
#include <string>

namespace engine::cover {

struct P3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};
inline P3 operator+(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline P3 operator-(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline P3 operator*(P3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
inline f64 dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline P3 cross(P3 a, P3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f64 length_of(P3 a);
P3 normalized(P3 a);  // (0, 1, 0) for a zero vector

// The host, welded and measured.
struct HostSurface {
  Vector<P3> positions;          // the host's vertices as given, widened to f64
  Vector<u32> indices;           // three per triangle, into `positions`
  Vector<f32> roughness;         // per triangle
  Vector<u8> holds;              // per triangle
  Vector<u32> weld;              // per vertex: its welded vertex
  u32 welded = 0;                // welded vertices
  Vector<P3> face_normal;        // per triangle, unit (a degenerate triangle's is +y)
  Vector<f64> face_area;         // per triangle
  Vector<P3> welded_normal;      // per welded vertex, area-weighted
  Vector<u32> adjacency_offset;  // CSR over welded vertices: `welded + 1` offsets
  Vector<u32> adjacency;         // each welded vertex's neighbours, ascending
  P3 lo;
  P3 hi;
  u32 triangle_count() const noexcept { return indices.size() / 3u; }
  P3 corner(u32 triangle, u32 k) const noexcept { return positions[indices[triangle * 3u + k]]; }
  P3 centroid(u32 triangle) const noexcept;
};

// False, with a sentence, for a host with no triangles or an index past its vertices.
bool make_host_surface(const assets::GeneratorHostMesh& host, HostSurface& out, std::string& error);

struct HostHit {
  P3 point;
  P3 normal;  // the triangle's
  u32 triangle = 0;
  f64 distance = 0.0;
};

class HostBvh {
 public:
  void build(const HostSurface& surface);

  // The closest point of the surface to `p` within `max_distance` (strictly closer), over every
  // triangle or, with `holding_only`, over the triangles that hold a climber. False when none is
  // that close.
  bool closest(P3 p, f64 max_distance, bool holding_only, HostHit& out) const;
  // Whether the segment from `origin` along the unit `direction` for `length` hits a triangle.
  bool occluded(P3 origin, P3 direction, f64 length) const;

  u32 node_count() const noexcept { return nodes_.size(); }

 private:
  struct Node {
    P3 lo;
    P3 hi;
    u32 first = 0;  // a leaf's first entry of `order_`, or an inner node's left child
    u32 count = 0;  // a leaf's triangles; 0 for an inner node (whose right child is left + 1)
  };
  const HostSurface* surface_ = nullptr;
  Vector<Node> nodes_;
  Vector<u32> order_;
};

// The exposure terms, per triangle (cover.md, "The exposure terms").
struct Exposure {
  f32 up = 0.0f;      // the normal's y, -1 (an overhang) to 1 (a top)
  f32 north = 0.0f;   // the normal along the world's north, -1 to 1
  f32 sun = 0.0f;     // direct light: max(0, n.L), 0 in the host's shadow, times 1 - canopy shade
  f32 sky = 0.0f;     // the cosine-weighted share of the hemisphere no surface hides within the
                      // crevice radius (the ground counts): 1 open, 0 a deep crevice
  f32 height = 0.0f;  // above the site's ground, metres, never below 0
  f32 water = 0.0f;   // metres to water
};

// The site's directions in the host's frame: north (unit, horizontal) and towards the light.
struct SiteFrame {
  P3 north;
  P3 light;
};
SiteFrame site_frame(const HostSite& site);

// The terms for every triangle, over a crevice radius (`CoverGrowth::crevice_radius_m`), with
// `sky_rays` directions of a fixed set. Deterministic.
void host_exposure(const HostSurface& surface, const HostBvh& bvh, const HostSite& site,
                   f64 crevice_radius, u32 sky_rays, Vector<Exposure>& out);

}  // namespace engine::cover
