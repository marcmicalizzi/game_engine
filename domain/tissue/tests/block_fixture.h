#pragma once

// A ten-node reference block for the cage and soft-body tests (docs/subsystems/tissue.md, "Cage
// derivation"): a 120 x 80 x 30 mm box of 6 x 4 x 3 hexahedra, each split into six Kuhn cells (432
// ten-node cells over 140 corners and their edges), the edge nodes lifted off their chords by a
// polynomial so the cells curve a little, two phases whose fraction is a polynomial of each cell's
// centre, a rim (the two end walls) fixed to a box frame 4 mm below, a posterior (the floor)
// sliding on a surface, and three states moved by polynomial fields — a rest, a reference and a
// pressed response. **Nothing here calls the C library**: every number is a sum, product or
// quotient of exact inputs, so the block is the same bytes on every toolchain, which is what lets a
// test pin the hashes of the cage derived from it.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/tissue/tissue_file.h>

#include <algorithm>
#include <string>

namespace engine::tissue::block {

inline constexpr u32 k_nx = 6;
inline constexpr u32 k_ny = 4;
inline constexpr u32 k_nz = 3;
inline constexpr f64 k_x = 0.120;
inline constexpr f64 k_y = 0.080;
inline constexpr f64 k_z = 0.030;

inline u32 corner(u32 i, u32 j, u32 k) { return (k * (k_ny + 1) + j) * (k_nx + 1) + i; }

// 16 s (1 - s) t (1 - t) over the block's plan: 1 at the middle, 0 on the end and side walls.
inline f64 bump(f64 x, f64 y) {
  const f64 s = x / k_x;
  const f64 t = y / k_y;
  return 16.0 * s * (1.0 - s) * t * (1.0 - t);
}

struct Block {
  TissueFile file;
  u32 corners = 0;
  u32 nodes = 0;
  u32 cells = 0;
};

template <class T>
inline void put(TissueFile& file, const std::string& name, BlockKind kind, const Vector<T>& data,
                u32 per_element = 1) {
  add_block(file, name, kind, std::span<const T>(data.data(), data.size()), per_element);
}

// `quadratic` false gives the same block as a four-node Runtime cage (its corners and cells).
inline Block make_block(bool quadratic = true) {
  Block out;
  TissueFile& file = out.file;
  TissueDefinition& d = file.definition;
  d.format = k_tissue_format;
  d.name = "block";
  d.description = "a ten-node reference block for the cage tests, generated from polynomials";
  d.provenance = "domain/tissue/tests/block_fixture.h";

  // ---- corners, and the Kuhn cells
  Vector<f64> px;
  Vector<f64> py;
  Vector<f64> pz;
  for (u32 k = 0; k <= k_nz; ++k)
    for (u32 j = 0; j <= k_ny; ++j)
      for (u32 i = 0; i <= k_nx; ++i) {
        px.push_back(k_x * static_cast<f64>(i) / k_nx);
        py.push_back(k_y * static_cast<f64>(j) / k_ny);
        pz.push_back(k_z * static_cast<f64>(k) / k_nz);
      }
  out.corners = px.size();
  Vector<u32> corners_of;  // 4 a cell
  for (u32 k = 0; k < k_nz; ++k)
    for (u32 j = 0; j < k_ny; ++j)
      for (u32 i = 0; i < k_nx; ++i) {
        const u32 v000 = corner(i, j, k);
        const u32 v100 = corner(i + 1, j, k);
        const u32 v010 = corner(i, j + 1, k);
        const u32 v110 = corner(i + 1, j + 1, k);
        const u32 v001 = corner(i, j, k + 1);
        const u32 v101 = corner(i + 1, j, k + 1);
        const u32 v011 = corner(i, j + 1, k + 1);
        const u32 v111 = corner(i + 1, j + 1, k + 1);
        const u32 tets[6][4] = {{v000, v011, v001, v111}, {v000, v010, v011, v111},
                                {v000, v001, v101, v111}, {v000, v101, v100, v111},
                                {v000, v110, v010, v111}, {v000, v100, v110, v111}};
        for (const auto& t : tets) {
          u32 c[4] = {t[0], t[1], t[2], t[3]};
          // Positive: (c1 - c0, c2 - c0, c3 - c0) right-handed.
          const f64 ax = px[c[1]] - px[c[0]], ay = py[c[1]] - py[c[0]], az = pz[c[1]] - pz[c[0]];
          const f64 bx = px[c[2]] - px[c[0]], by = py[c[2]] - py[c[0]], bz = pz[c[2]] - pz[c[0]];
          const f64 cx = px[c[3]] - px[c[0]], cy = py[c[3]] - py[c[0]], cz = pz[c[3]] - pz[c[0]];
          const f64 det =
              ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx);
          if (det < 0.0) std::swap(c[1], c[2]);
          for (const u32 v : c)
            corners_of.push_back(v);
        }
      }
  out.cells = corners_of.size() / 4;

  // ---- the edge nodes (Gmsh's order), numbered after the corners by their sorted edge
  const u32 gmsh[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {2, 3}, {1, 3}};
  Vector<u64> edges;
  for (u32 c = 0; c < out.cells; ++c)
    for (const auto& e : gmsh) {
      const u32 a = corners_of[4 * c + e[0]];
      const u32 b = corners_of[4 * c + e[1]];
      edges.push_back((static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b));
    }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  const auto edge_node = [&](u32 a, u32 b) {
    const u64 key = (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
    return out.corners +
           static_cast<u32>(std::lower_bound(edges.begin(), edges.end(), key) - edges.begin());
  };
  for (const u64 e : edges) {
    const u32 a = static_cast<u32>(e >> 32);
    const u32 b = static_cast<u32>(e & 0xffffffffu);
    const f64 x = 0.5 * (px[a] + px[b]);
    const f64 y = 0.5 * (py[a] + py[b]);
    const f64 z = 0.5 * (pz[a] + pz[b]);
    // Lifted off the chord by up to 0.3 mm, more where the edge is long and high: the cells curve
    // a little, and the floor (z = 0) and the end walls (where the bump is 0) stay flat.
    const f64 dx = px[b] - px[a];
    const f64 dz = pz[b] - pz[a];
    px.push_back(x);
    py.push_back(y);
    pz.push_back(z + 0.3e-3 * bump(x, y) * (z / k_z) * (dx * dx + dz * dz) / (0.02 * 0.02));
  }
  Vector<u32> cells;
  if (quadratic) {
    for (u32 c = 0; c < out.cells; ++c) {
      const u32* n = corners_of.data() + 4 * c;
      for (u32 k = 0; k < 4; ++k)
        cells.push_back(n[k]);
      for (const auto& e : gmsh)
        cells.push_back(edge_node(n[e[0]], n[e[1]]));
    }
  } else {
    cells = corners_of;
    px.resize(out.corners);
    py.resize(out.corners);
    pz.resize(out.corners);
  }
  out.nodes = px.size();
  const auto at = [&](f64 lift, f64 slide) {
    Vector<Vec3> v(out.nodes);
    for (u32 i = 0; i < out.nodes; ++i) {
      const f64 b = bump(px[i], py[i]);
      // Deeper nodes move less: the floor is held by its slide, the top moves most.
      const f64 depth = pz[i] / k_z;
      v[i] = Vec3{static_cast<f32>(px[i] + slide * b * depth), static_cast<f32>(py[i]),
                  static_cast<f32>(pz[i] + lift * b * (0.25 + 0.75 * depth))};
    }
    return v;
  };
  put(file, "block.nodes", BlockKind::RegionNodes, at(0.0, 0.0));
  put(file, "block.cells", quadratic ? BlockKind::QuadraticTetrahedra : BlockKind::Tetrahedra,
      cells, quadratic ? 10u : 4u);
  Vector<f32> gland;
  for (u32 c = 0; c < out.cells; ++c) {
    f64 x = 0.0;
    f64 y = 0.0;
    for (u32 k = 0; k < 4; ++k) {
      x += px[corners_of[4 * c + k]] / 4.0;
      y += py[corners_of[4 * c + k]] / 4.0;
    }
    gland.push_back(static_cast<f32>(0.8 * bump(x, y)));
  }
  put(file, "block.gland", BlockKind::PhaseFraction, gland);
  // Node sets: the rim (the end walls, x = 0 and x = X, every node on them) and the floor.
  Vector<u32> rim;
  Vector<u32> floor;
  const f64 x_end = px[corner(k_nx, 0, 0)];  // the end wall's x, as the corners computed it
  for (u32 i = 0; i < out.nodes; ++i) {
    const bool wall = px[i] == 0.0 || px[i] == x_end;
    if (wall) rim.push_back(i);
    if (pz[i] == 0.0 && !wall) floor.push_back(i);
  }
  put(file, "block.rim", BlockKind::NodeSet, rim);
  put(file, "block.floor", BlockKind::NodeSet, floor);

  Region region;
  region.name = "block";
  region.nodes = "block.nodes";
  region.tetrahedra = "block.cells";
  if (quadratic) {
    region.cage = CageKind::TetrahedralQuadratic;
    region.role = RegionRole::Reference;
  }
  MaterialPhase fat;
  fat.material = Material{"fat", 100.0e3, 350.0, 950.0};
  MaterialPhase glandular;
  glandular.material = Material{"gland", 100.0e3, 1000.0, 1040.0};
  glandular.fraction = "block.gland";
  region.phases.push_back(fat);
  region.phases.push_back(glandular);
  region.node_sets.push_back(NodeSetRef{"rim", "block.rim", "rim"});
  region.node_sets.push_back(NodeSetRef{"floor", "block.floor", "posterior"});
  d.regions.push_back(region);

  // ---- the frame: a box whose top is 4 mm under the floor; the floor as a surface
  const f32 top = -0.004f;
  const f32 bottom = -0.054f;
  const f32 lo_x = -0.02f;
  const f32 hi_x = 0.14f;
  const f32 lo_y = -0.02f;
  const f32 hi_y = 0.10f;
  const Vector<Vec3> box = {Vec3{lo_x, lo_y, bottom}, Vec3{hi_x, lo_y, bottom},
                            Vec3{hi_x, hi_y, bottom}, Vec3{lo_x, hi_y, bottom},
                            Vec3{lo_x, lo_y, top},    Vec3{hi_x, lo_y, top},
                            Vec3{hi_x, hi_y, top},    Vec3{lo_x, hi_y, top}};
  const Vector<u32> box_faces = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  put(file, "base.vertices", BlockKind::FrameVertices, box);
  put(file, "base.triangles", BlockKind::FrameTriangles, box_faces, 3);
  Frame frame;
  frame.name = "base";
  frame.vertices = "base.vertices";
  frame.triangles = "base.triangles";
  frame.provenance = "a box 4 mm under the block's floor";
  d.frames.push_back(frame);
  // The floor: the block's plan at z = 0, two triangles facing down (outward from the block).
  const Vector<Vec3> floor_vertices = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.12f, 0.0f, 0.0f},
                                       Vec3{0.12f, 0.08f, 0.0f}, Vec3{0.0f, 0.08f, 0.0f}};
  const Vector<u32> floor_faces = {0, 2, 1, 0, 3, 2};
  put(file, "floor.vertices", BlockKind::SurfaceVertices, floor_vertices);
  put(file, "floor.triangles", BlockKind::SurfaceTriangles, floor_faces, 3);
  d.surfaces.push_back(Surface{"floor", "floor.vertices", "floor.triangles", true,
                               "the block's floor at its construction"});
  d.attachments.push_back(
      Attachment{"rim", "block", "rim", AttachmentKind::Fixed, TargetKind::Frame, "base", {}});
  d.attachments.push_back(Attachment{"floor",
                                     "block",
                                     "floor",
                                     AttachmentKind::SlidingBilateral,
                                     TargetKind::Surface,
                                     "floor",
                                     {}});

  // ---- the states
  put(file, "state.rest", BlockKind::StateNodes, at(0.0005, 0.0));
  put(file, "state.reference", BlockKind::StateNodes, at(-0.0005, 0.0));
  put(file, "state.pressed", BlockKind::StateNodes, at(-0.0025, 0.0006));
  RegionState rest;
  rest.name = "rest";
  rest.region = "block";
  rest.role = StateRole::Rest;
  rest.provenance = StateProvenance::Authored;
  rest.nodes = "state.rest";
  rest.load.pose = "unloaded";
  RegionState reference;
  reference.name = "reference";
  reference.region = "block";
  reference.role = StateRole::Reference;
  reference.provenance = StateProvenance::ForwardFromAuthoredRest;
  reference.nodes = "state.reference";
  reference.load.gravity = Vec3{0.0f, 0.0f, -9.81f};
  reference.load.pose = "standing";
  RegionState pressed;
  pressed.name = "pressed";
  pressed.region = "block";
  pressed.role = StateRole::Response;
  pressed.provenance = StateProvenance::ForwardFromReference;
  pressed.nodes = "state.pressed";
  pressed.load.gravity = Vec3{0.0f, 0.0f, -9.81f};
  pressed.load.pose = "supine";
  d.states.push_back(rest);
  d.states.push_back(reference);
  d.states.push_back(pressed);
  return out;
}

}  // namespace engine::tissue::block
