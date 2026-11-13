#pragma once
// The trunk's skirt as the tests find it (G201; flora.md, "The trunk's skirt"): the skirt's
// vertices are the bark primitive's vertices at bend weight zero with v below zero (the trunk's
// metres continued down; nothing else on a tree's bark has both), and its triangles the bark's that
// touch one. `strip_skirt` takes them out again, so a test can hold the mesh less its skirt to the
// bytes the tree had before it.

#include <domain/assets/gltf.h>
#include <domain/flora/motion.h>

namespace engine::flora::test_support {

struct SkirtFound {
  Vector<u8> vertex;    // 1 for a skirt vertex
  Vector<u8> triangle;  // 1 for a skirt triangle
  u32 vertices = 0;
  u32 triangles = 0;
};

inline SkirtFound find_skirt(const assets::MeshData& mesh, const Motion& motion) {
  SkirtFound s;
  s.vertex.resize(mesh.positions.size(), 0);
  s.triangle.resize(mesh.indices.size() / 3, 0);
  if (mesh.primitives.empty()) return s;
  const assets::Primitive& bark = mesh.primitives[0];
  for (u32 i = bark.first_index; i < bark.first_index + bark.index_count; ++i) {
    const u32 v = mesh.indices[i];
    if (motion.vertex_weight[v] == 0.0f && mesh.uvs[v].y < 0.0f && s.vertex[v] == 0) {
      s.vertex[v] = 1;
      ++s.vertices;
    }
  }
  for (u32 t = bark.first_index / 3; t < (bark.first_index + bark.index_count) / 3; ++t) {
    for (u32 k = 0; k < 3; ++k)
      if (s.vertex[mesh.indices[t * 3 + k]] != 0) s.triangle[t] = 1;
    s.triangles += s.triangle[t];
  }
  return s;
}

// The mesh and its motion contract with the skirt taken out: vertices and triangles in their
// order, indices renumbered, the primitives' ranges shortened.
inline void strip_skirt(assets::MeshData& mesh, Motion& motion) {
  const SkirtFound s = find_skirt(mesh, motion);
  Vector<u32> remap(mesh.positions.size(), 0);
  assets::MeshData out = mesh;
  out.positions.clear();
  out.normals.clear();
  out.uvs.clear();
  out.indices.clear();
  out.primitives.clear();
  Motion m = motion;
  m.vertex_part.clear();
  m.vertex_weight.clear();
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    if (s.vertex[v] != 0) continue;
    remap[v] = out.positions.size();
    out.positions.push_back(mesh.positions[v]);
    out.normals.push_back(mesh.normals[v]);
    out.uvs.push_back(mesh.uvs[v]);
    m.vertex_part.push_back(motion.vertex_part[v]);
    m.vertex_weight.push_back(motion.vertex_weight[v]);
  }
  for (const assets::Primitive& p : mesh.primitives) {
    assets::Primitive q = p;
    q.first_index = out.indices.size();
    for (u32 t = p.first_index / 3; t < (p.first_index + p.index_count) / 3; ++t) {
      if (s.triangle[t] != 0) continue;
      for (u32 k = 0; k < 3; ++k)
        out.indices.push_back(remap[mesh.indices[t * 3 + k]]);
    }
    q.index_count = out.indices.size() - q.first_index;
    out.primitives.push_back(q);
  }
  mesh = std::move(out);
  motion = std::move(m);
}

}  // namespace engine::flora::test_support
