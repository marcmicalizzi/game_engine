#include <domain/geometry/limit_surface.h>
#include <domain/geometry/surface_binding.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::tissue {

using namespace synthetic;

namespace {

template <class T>
void put(TissueFile& file, const std::string& name, BlockKind kind, const Vector<T>& data,
         u32 per_element = 1) {
  add_block(file, name, kind, std::span<const T>(data.data(), data.size()), per_element);
}

Vec3 torus_point(f64 u, f64 v, f64 rho) {
  const f64 centre = -(k_major + k_outer + k_cover);
  const f64 ring = k_major + rho * std::cos(v);
  return Vec3{static_cast<f32>(ring * std::sin(u)), static_cast<f32>(rho * std::sin(v)),
              static_cast<f32>(centre + ring * std::cos(u))};
}

Vector<Vec3> region_nodes() {
  Vector<Vec3> out;
  for (u32 layer = 0; layer < k_layers; ++layer) {
    const f64 rho = k_outer - (k_outer - k_inner) * static_cast<f64>(layer) / (k_layers - 1);
    for (u32 j = 0; j < k_nv; ++j)
      for (u32 i = 0; i < k_nu; ++i)
        out.push_back(
            torus_point(-k_u + 2.0 * k_u * i / (k_nu - 1), -k_v + 2.0 * k_v * j / (k_nv - 1), rho));
  }
  return out;
}

// The (i, j) grid of one layer, two triangles a quad, counter-clockwise seen from +z at the apex
// (outward for the top layer); `flip` winds them the other way (outward for the support layer).
Vector<u32> layer_faces(u32 layer, bool flip) {
  Vector<u32> out;
  for (u32 j = 0; j + 1 < k_nv; ++j)
    for (u32 i = 0; i + 1 < k_nu; ++i) {
      const u32 a = node(i, j, layer);
      const u32 b = node(i + 1, j, layer);
      const u32 c = node(i + 1, j + 1, layer);
      const u32 d = node(i, j + 1, layer);
      const u32 tris[6] = {a, b, c, a, c, d};
      for (u32 k = 0; k < 6; k += 3) {
        out.push_back(tris[k]);
        out.push_back(flip ? tris[k + 2] : tris[k + 1]);
        out.push_back(flip ? tris[k + 1] : tris[k + 2]);
      }
    }
  return out;
}

// 72 hexahedra, each six tetrahedra around its main diagonal, oriented positive.
Vector<u32> tetrahedra(const Vector<Vec3>& nodes) {
  Vector<u32> out;
  for (u32 l = 0; l + 1 < k_layers; ++l)
    for (u32 j = 0; j + 1 < k_nv; ++j)
      for (u32 i = 0; i + 1 < k_nu; ++i) {
        u32 c[8];
        for (u32 k = 0; k < 8; ++k)
          c[k] = node(i + (k & 1u), j + ((k >> 1) & 1u), l + ((k >> 2) & 1u));
        const u32 kuhn[6][4] = {{0, 1, 3, 7}, {0, 3, 2, 7}, {0, 2, 6, 7},
                                {0, 6, 4, 7}, {0, 4, 5, 7}, {0, 5, 1, 7}};
        for (const auto& t : kuhn) {
          u32 v[4] = {c[t[0]], c[t[1]], c[t[2]], c[t[3]]};
          const Vec3 e1 = nodes[v[1]] - nodes[v[0]];
          const Vec3 e2 = nodes[v[2]] - nodes[v[0]];
          const Vec3 e3 = nodes[v[3]] - nodes[v[0]];
          if (dot(e1, cross(e2, e3)) < 0.0f) std::swap(v[1], v[2]);
          for (const u32 x : v)
            out.push_back(x);
        }
      }
  return out;
}

// The rim of a refined surface as directed boundary edges, (a, b) as a triangle traverses them.
Vector<u32> boundary_edges(const Vector<u32>& faces) {
  Vector<u64> keys;
  for (u32 t = 0; t < faces.size(); t += 3)
    for (u32 k = 0; k < 3; ++k) {
      const u32 a = faces[t + k];
      const u32 b = faces[t + (k + 1) % 3];
      keys.push_back((u64{std::min(a, b)} << 32) | u64{std::max(a, b)});
    }
  std::sort(keys.begin(), keys.end());
  Vector<u32> out;
  for (u32 t = 0; t < faces.size(); t += 3)
    for (u32 k = 0; k < 3; ++k) {
      const u32 a = faces[t + k];
      const u32 b = faces[t + (k + 1) % 3];
      const u64 key = (u64{std::min(a, b)} << 32) | u64{std::max(a, b)};
      const auto lo = std::lower_bound(keys.begin(), keys.end(), key);
      const auto hi = std::upper_bound(keys.begin(), keys.end(), key);
      if (hi - lo == 1) {
        out.push_back(a);
        out.push_back(b);
      }
    }
  return out;
}

f32 smooth_bump(f32 s, f32 t) {  // 1 at the centre, 0 at |s| = 1 or |t| = 1
  const f32 a = std::max(0.0f, 1.0f - s * s);
  const f32 b = std::max(0.0f, 1.0f - t * t);
  return a * a * b * b;
}

// A vertex node's grid position and its torus parameters, from its index.
u32 grid_i(u32 n) { return n % k_nu; }
u32 grid_j(u32 n) { return (n % (k_nu * k_nv)) / k_nu; }
u32 grid_layer(u32 n) { return n / (k_nu * k_nv); }
f64 param_u(u32 n) { return -k_u + 2.0 * k_u * grid_i(n) / (k_nu - 1); }
f64 param_v(u32 n) { return -k_v + 2.0 * k_v * grid_j(n) / (k_nv - 1); }
f64 param_rho(u32 n) {
  return k_outer - (k_outer - k_inner) * static_cast<f64>(grid_layer(n)) / (k_layers - 1);
}
// The grid coordinates in [-1, 1] the states' bump is evaluated at.
f32 grid_s(u32 n) { return -1.0f + 2.0f * static_cast<f32>(grid_i(n)) / (k_nu - 1); }
f32 grid_t(u32 n) { return -1.0f + 2.0f * static_cast<f32>(grid_j(n)) / (k_nv - 1); }

// The ten-node slab's edge nodes: one per edge of the four-node slab's tetrahedra, numbered after
// the vertices in the order of their edges' (lower, higher) keys, so that an edge of the top layer
// and the same edge of the support layer have the same rank among their layers' edge nodes and the
// two sheets' refined surfaces keep one numbering (the stitch depends on it).
struct EdgeNodes {
  Vector<u64> keys;  // sorted, unique
  u32 first = 0;     // the first edge node's index: the vertex count

  static u64 key(u32 a, u32 b) { return (u64{std::min(a, b)} << 32) | u64{std::max(a, b)}; }
  u32 node(u32 a, u32 b) const {
    return first +
           static_cast<u32>(std::lower_bound(keys.begin(), keys.end(), key(a, b)) - keys.begin());
  }
  u32 end_a(u32 edge_node) const { return static_cast<u32>(keys[edge_node - first] >> 32); }
  u32 end_b(u32 edge_node) const { return static_cast<u32>(keys[edge_node - first] & 0xffffffffu); }
};

EdgeNodes edge_nodes(const Vector<u32>& tets, u32 vertices) {
  EdgeNodes out;
  out.first = vertices;
  for (u32 t = 0; t < tets.size(); t += 4)
    for (u32 a = 0; a < 4; ++a)
      for (u32 b = a + 1; b < 4; ++b)
        out.keys.push_back(EdgeNodes::key(tets[t + a], tets[t + b]));
  std::sort(out.keys.begin(), out.keys.end());
  out.keys.erase(std::unique(out.keys.begin(), out.keys.end()), out.keys.end());
  return out;
}

// Each triangle split in four over its edges' nodes, wound as it was.
Vector<u32> split_faces(const Vector<u32>& faces, const EdgeNodes& edges) {
  Vector<u32> out;
  for (u32 f = 0; f < faces.size(); f += 3) {
    const u32 a = faces[f];
    const u32 b = faces[f + 1];
    const u32 c = faces[f + 2];
    const u32 ab = edges.node(a, b);
    const u32 bc = edges.node(b, c);
    const u32 ca = edges.node(c, a);
    const u32 tris[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};
    for (const u32 v : tris)
      out.push_back(v);
  }
  return out;
}

}  // namespace

SyntheticTissue make_synthetic_tissue(const SyntheticOptions& synthetic_options) {
  const bool quadratic = synthetic_options.quadratic;
  SyntheticTissue out;
  TissueFile& file = out.file;
  TissueDefinition& d = file.definition;
  d.format = k_tissue_format;
  d.name = quadratic ? "synthetic quadratic slab" : "synthetic slab";
  d.description =
      quadratic ? "a torus-section slab of ten-node cells under a flat skin, on a box frame, as a "
                  "reference body: generated, not measured"
                : "a torus-section slab under a flat skin, on a box frame: generated, not measured";
  d.provenance = quadratic ? "domain/tissue/src/synthetic.cpp, make_synthetic_tissue (quadratic)"
                           : "domain/tissue/src/synthetic.cpp, make_synthetic_tissue";

  // ---- the skin: the base, the observation, the domain -------------------------------------
  Vector<Vec3> base;
  Vector<u32> quads;
  for (u32 j = 0; j < k_grid; ++j)
    for (u32 i = 0; i < k_grid; ++i)
      base.push_back(Vec3{-0.1f + 0.2f * static_cast<f32>(i) / (k_grid - 1),
                          -0.1f + 0.2f * static_cast<f32>(j) / (k_grid - 1), 0.0f});
  for (u32 j = 0; j + 1 < k_grid; ++j)
    for (u32 i = 0; i + 1 < k_grid; ++i) {
      const u32 a = j * k_grid + i;
      quads.push_back(a);
      quads.push_back(a + 1);
      quads.push_back(a + 1 + k_grid);
      quads.push_back(a + k_grid);
    }
  Vector<u32> domain;
  Vector<Vec3> base_outside;
  Vector<f32> deviation;
  Vector<Vec3> observed = base;
  for (u32 id = 0; id < base.size(); ++id) {
    const Vec3 p = base[id];
    if (std::fabs(p.x) <= 0.06f && std::fabs(p.y) <= 0.04f) {
      domain.push_back(id);
      const f32 lift = 0.001f * smooth_bump(p.x / 0.06f, p.y / 0.04f);
      observed[id].z = lift;
      deviation.push_back(lift);
    } else {
      base_outside.push_back(p);
    }
  }
  put(file, "skin.faces", BlockKind::BaseQuads, quads, 4);
  put(file, "skin.observed", BlockKind::ObservedPositions, observed);
  put(file, "skin.domain", BlockKind::CanonicalIds, domain);
  put(file, "skin.base_outside", BlockKind::BaseOutsidePositions, base_outside);
  put(file, "skin.deviation", BlockKind::IdValues, deviation);
  Observation& o = d.observation;
  o.base_id = "synthetic-plane-41x41";
  o.vertex_count = static_cast<u32>(base.size());
  o.base_faces = "skin.faces";
  o.topology_sha256 = topology_sha256(quads);
  o.positions = "skin.observed";
  o.domain = "skin.domain";
  o.base_outside = "skin.base_outside";
  o.signed_deviation = "skin.deviation";
  o.load.gravity = Vec3{0.0f, 0.0f, -9.81f};
  o.load.pose = "standing";
  const u32 landmark = (k_grid / 2) * k_grid + k_grid / 2;
  o.landmarks.push_back(
      Landmark{"apex", landmark, observed[landmark], "the skin over the slab's apex"});
  o.acceptance.accepted = false;
  o.acceptance.note = "a generated fixture; nobody accepted it";

  // ---- the region
  // --------------------------------------------------------------------------------
  Vector<Vec3> nodes = region_nodes();
  const u32 vertex_count = nodes.size();
  const Vector<u32> tets = tetrahedra(nodes);
  Vector<u32> top_faces = layer_faces(0, false);
  Vector<u32> support_faces = layer_faces(k_layers - 1, true);
  // The ten-node slab: a node on every edge, placed on the torus at the mean of its ends'
  // parameters (so the cells are gently curved, as a mesher's boundary-fitted ones are), the cells
  // in Gmsh's order, and each sheet's control triangle split in four so that the sheets carry the
  // edge nodes of their faces. The support's split faces are the top's, moved to the support layer
  // and reversed, as the four-node sheets are: the stitch needs the two to number alike.
  EdgeNodes edges;
  Vector<u32> cells = tets;
  if (quadratic) {
    edges = edge_nodes(tets, vertex_count);
    for (const u64 key : edges.keys) {
      const u32 a = static_cast<u32>(key >> 32);
      const u32 b = static_cast<u32>(key & 0xffffffffu);
      nodes.push_back(torus_point(0.5 * (param_u(a) + param_u(b)), 0.5 * (param_v(a) + param_v(b)),
                                  0.5 * (param_rho(a) + param_rho(b))));
    }
    cells.clear();
    for (u32 t = 0; t < tets.size(); t += 4) {
      const u32* v = tets.data() + t;
      const u32 cell[10] = {v[0],
                            v[1],
                            v[2],
                            v[3],
                            edges.node(v[0], v[1]),
                            edges.node(v[1], v[2]),
                            edges.node(v[0], v[2]),
                            edges.node(v[0], v[3]),
                            edges.node(v[2], v[3]),
                            edges.node(v[1], v[3])};
      for (const u32 n : cell)
        cells.push_back(n);
    }
    top_faces = split_faces(top_faces, edges);
    const u32 offset = (k_layers - 1) * k_nu * k_nv;
    const auto on_support = [&](u32 n) {
      return n < vertex_count ? n + offset
                              : edges.node(edges.end_a(n) + offset, edges.end_b(n) + offset);
    };
    support_faces.clear();
    for (u32 f = 0; f < top_faces.size(); f += 3) {
      support_faces.push_back(on_support(top_faces[f]));
      support_faces.push_back(on_support(top_faces[f + 2]));
      support_faces.push_back(on_support(top_faces[f + 1]));
    }
  }
  put(file, "slab.nodes", BlockKind::RegionNodes, nodes);
  if (quadratic)
    put(file, "slab.tets", BlockKind::QuadraticTetrahedra, cells, 10);
  else
    put(file, "slab.tets", BlockKind::Tetrahedra, tets, 4);
  put(file, "slab.top", BlockKind::SheetFaces, top_faces, 3);
  put(file, "slab.support", BlockKind::SheetFaces, support_faces, 3);
  Vector<f32> gland(static_cast<u32>(tets.size() / 4), 0.2f);
  put(file, "slab.gland", BlockKind::PhaseFraction, gland);
  put(file, "slab.membrane", BlockKind::MembraneTriangles, top_faces, 3);
  Vector<u32> cables;
  for (u32 j = 1; j + 1 < k_nv; ++j)
    for (u32 i = 1; i + 1 < k_nu; ++i) {
      cables.push_back(node(i, j, 0));
      cables.push_back(node(i, j, k_layers - 1));
    }
  put(file, "slab.cables", BlockKind::CableEdges, cables, 2);
  // The rim is the slab's whole side wall, every layer: a side node on neither sheet that could
  // move would be a surface node the skin binding does not carry (volume.omitted_volume).
  Vector<u32> rim;
  Vector<u32> posterior;
  for (u32 layer = 0; layer < k_layers; ++layer)
    for (u32 j = 0; j < k_nv; ++j)
      for (u32 i = 0; i < k_nu; ++i) {
        const bool edge = i == 0 || j == 0 || i + 1 == k_nu || j + 1 == k_nv;
        if (edge) rim.push_back(node(i, j, layer));
        if (!edge && layer + 1 == k_layers) posterior.push_back(node(i, j, layer));
      }
  // An edge node is on the side wall when its edge lies in one of the wall's four planes, and on
  // the support when its edge lies in the support layer; the wall wins, as for the vertices.
  if (quadratic)
    for (u32 n = vertex_count; n < nodes.size(); ++n) {
      const u32 a = edges.end_a(n);
      const u32 b = edges.end_b(n);
      const bool wall =
          (grid_i(a) == 0 && grid_i(b) == 0) || (grid_i(a) + 1 == k_nu && grid_i(b) + 1 == k_nu) ||
          (grid_j(a) == 0 && grid_j(b) == 0) || (grid_j(a) + 1 == k_nv && grid_j(b) + 1 == k_nv);
      if (wall) {
        rim.push_back(n);
      } else if (grid_layer(a) + 1 == k_layers && grid_layer(b) + 1 == k_layers) {
        posterior.push_back(n);
      }
    }
  std::sort(rim.begin(), rim.end());
  put(file, "slab.rim", BlockKind::NodeSet, rim);
  put(file, "slab.posterior", BlockKind::NodeSet, posterior);

  Region region;
  region.name = "slab";
  region.nodes = "slab.nodes";
  region.tetrahedra = "slab.tets";
  if (quadratic) {
    // 845 nodes: a reference body, which no runtime cage of ADR-0029's is, as the certified
    // quadratic bodies the authoring side publishes are not.
    region.cage = CageKind::TetrahedralQuadratic;
    region.role = RegionRole::Reference;
  }
  MaterialPhase fat;
  fat.material = Material{"fat", 100.0e3, 350.0, 950.0};
  MaterialPhase glandular;
  glandular.material = Material{"gland", 100.0e3, 1000.0, 1040.0};
  glandular.fraction = "slab.gland";
  region.phases.push_back(fat);
  region.phases.push_back(glandular);
  Membrane membrane;
  membrane.name = "top membrane";
  membrane.triangles = "slab.membrane";
  membrane.stiffness_n_per_m = 25.0;
  region.membranes.push_back(membrane);
  CableSet cable_set;
  cable_set.name = "through-thickness";
  cable_set.edges = "slab.cables";
  cable_set.stiffness_n_per_m = 0.3;
  cable_set.slack = 0.02;
  cable_set.recruitment = 0.05;
  region.cables.push_back(cable_set);
  region.node_sets.push_back(NodeSetRef{"rim", "slab.rim", "rim"});
  region.node_sets.push_back(NodeSetRef{"posterior", "slab.posterior", "posterior"});
  // The ten-node sheets carry four control triangles for each of the four-node sheets' one, so
  // one level less gives the same dense surface: 4,608 refined triangles either way.
  const u32 level = quadratic ? 2u : 3u;
  region.sheets.push_back(Sheet{"top", "slab.top", level});
  region.sheets.push_back(Sheet{"support", "slab.support", level});
  region.top_sheet = "top";
  region.support_sheet = "support";

  // The closed shell's stitch: the two sheets' refined surfaces have the same numbering (one's
  // control faces are the other's reversed, over nodes in the same order), so the top's rim edge
  // (a, b) pairs with the support's (a + T, b + T).
  geometry::LoopSurfaceOptions options;
  options.level = level;
  options.tangents = true;
  // The sheet's control vertices are its nodes in ascending order, as the validator numbers them:
  // the top layer's 0 .. 48 already, and after them the top layer's edge nodes.
  Vector<u32> top_nodes = top_faces;
  std::sort(top_nodes.begin(), top_nodes.end());
  top_nodes.erase(std::unique(top_nodes.begin(), top_nodes.end()), top_nodes.end());
  Vector<u32> top_local;
  for (const u32 v : top_faces)
    top_local.push_back(static_cast<u32>(std::lower_bound(top_nodes.begin(), top_nodes.end(), v) -
                                         top_nodes.begin()));
  geometry::build_loop_limit_surface(top_local, top_nodes.size(), options, out.top);
  const u32 refined = out.top.vertex_count();
  const Vector<u32> rim_edges = boundary_edges(out.top.faces);
  Vector<u32> stitch;
  for (u32 e = 0; e < rim_edges.size(); e += 2) {
    const u32 a = rim_edges[e];
    const u32 b = rim_edges[e + 1];
    const u32 tri[6] = {b, a, a + refined, b, a + refined, b + refined};
    for (const u32 v : tri)
      stitch.push_back(v);
  }
  put(file, "slab.stitch", BlockKind::ShellStitch, stitch, 3);
  region.shell_stitch = "slab.stitch";
  d.regions.push_back(region);

  // ---- the frame and the support surface
  // ----------------------------------------------------------
  f32 lowest = 1.0f;
  for (const Vec3& p : nodes)
    lowest = std::min(lowest, p.z);
  const f32 top_z = lowest - 0.004f;
  const f32 bottom_z = top_z - 0.1f;
  const f32 h = 0.12f;
  const Vector<Vec3> box = {Vec3{-h, -h, bottom_z}, Vec3{h, -h, bottom_z}, Vec3{h, h, bottom_z},
                            Vec3{-h, h, bottom_z},  Vec3{-h, -h, top_z},   Vec3{h, -h, top_z},
                            Vec3{h, h, top_z},      Vec3{-h, h, top_z}};
  const Vector<u32> box_faces = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  put(file, "box.vertices", BlockKind::FrameVertices, box);
  put(file, "box.triangles", BlockKind::FrameTriangles, box_faces, 3);
  Vector<u8> provenance(8, u8{2});
  put(file, "box.provenance", BlockKind::FrameCoverProvenance, provenance);
  Frame frame;
  frame.name = "box";
  frame.vertices = "box.vertices";
  frame.triangles = "box.triangles";
  frame.cover_provenance = "box.provenance";
  frame.provenance = "a box placed 4 mm under the slab's lowest node";
  d.frames.push_back(frame);
  put(file, "support.vertices", BlockKind::SurfaceVertices, nodes);
  put(file, "support.triangles", BlockKind::SurfaceTriangles, support_faces, 3);
  d.surfaces.push_back(Surface{"support", "support.vertices", "support.triangles", true,
                               "the support sheet at its construction, standing in for a region"});
  d.attachments.push_back(
      Attachment{"rim", "slab", "rim", AttachmentKind::Fixed, TargetKind::Frame, "box", {}});
  d.attachments.push_back(Attachment{"posterior",
                                     "slab",
                                     "posterior",
                                     AttachmentKind::SlidingBilateral,
                                     TargetKind::Surface,
                                     "support",
                                     {}});

  // ---- the states
  // ---------------------------------------------------------------------------------
  const auto moved = [&](const Vector<Vec3>& from, Vec3 at_centre) {
    Vector<Vec3> out_nodes = from;
    for (u32 layer = 0; layer < k_layers; ++layer)
      for (u32 j = 0; j < k_nv; ++j)
        for (u32 i = 0; i < k_nu; ++i) {
          const f32 s = -1.0f + 2.0f * static_cast<f32>(i) / (k_nu - 1);
          const f32 t = -1.0f + 2.0f * static_cast<f32>(j) / (k_nv - 1);
          out_nodes[node(i, j, layer)] =
              out_nodes[node(i, j, layer)] + at_centre * smooth_bump(s, t);
        }
    // An edge node moves by the bump at its own place, the mean of its ends' grid coordinates,
    // not by the mean of their moves: the states' ten-node cells curve a little further.
    for (u32 n = vertex_count; n < out_nodes.size(); ++n) {
      const u32 a = edges.end_a(n);
      const u32 b = edges.end_b(n);
      const f32 s = 0.5f * (grid_s(a) + grid_s(b));
      const f32 t = 0.5f * (grid_t(a) + grid_t(b));
      out_nodes[n] = out_nodes[n] + at_centre * smooth_bump(s, t);
    }
    return out_nodes;
  };
  out.reference_nodes = moved(nodes, Vec3{0.0f, 0.0f, -0.0005f});
  const Vector<Vec3> rest = moved(nodes, Vec3{0.0f, 0.0f, 0.001f});
  const Vector<Vec3> pressed = moved(out.reference_nodes, Vec3{0.0f, 0.001f, -0.003f});
  put(file, "state.standing", BlockKind::StateNodes, out.reference_nodes);
  put(file, "state.rest", BlockKind::StateNodes, rest);
  put(file, "state.pressed", BlockKind::StateNodes, pressed);
  RegionState standing;
  standing.name = "standing";
  standing.region = "slab";
  standing.role = StateRole::Reference;
  standing.provenance = StateProvenance::InverseStatics;
  standing.nodes = "state.standing";
  standing.load = o.load;
  InverseFit fit;
  fit.method = "surface-targeted";
  fit.interior_extension = "harmonic";
  fit.step_alpha = 0.7;
  fit.held_node_sets.push_back("rim");
  fit.fit_residual_p95_mm = 0.1;
  fit.min_cell_ratio = 0.95;
  // The engine's runtime defaults (ADR-0029: 8 iterations, 2 sub-steps of a 60 Hz tick). The
  // fixture's rest is constructed, not solved, which the implementation says; its equilibrium gap
  // is correspondingly large, and the validator reports it without failing.
  InverseSolver solver;
  solver.kind = "xpbd";
  solver.iterations = 8;
  solver.step_s = 1.0 / 120.0;
  solver.implementation =
      "synthetic: declared for the fixture; the rest is the reference raised "
      "by a bump, constructed rather than solved";
  fit.solver = solver;
  standing.inverse = fit;
  RegionState rest_state;
  rest_state.name = "rest";
  rest_state.region = "slab";
  rest_state.role = StateRole::Rest;
  rest_state.provenance = StateProvenance::InverseStatics;
  rest_state.inverse = fit;
  rest_state.nodes = "state.rest";
  rest_state.load.pose = "unloaded";
  RegionState pressed_state;
  pressed_state.name = "pressed";
  pressed_state.region = "slab";
  pressed_state.role = StateRole::Response;
  pressed_state.provenance = StateProvenance::ForwardFromReference;
  pressed_state.nodes = "state.pressed";
  pressed_state.load.gravity = Vec3{0.0f, 9.81f, 0.0f};
  pressed_state.load.pose = "supine";
  d.states.push_back(standing);
  d.states.push_back(rest_state);
  d.states.push_back(pressed_state);

  // ---- the skin binding: footpoints by the nearest binder, stored as authored
  // --------------------
  Vector<u32> binding_domain;
  Vector<u32> footprint;
  Vector<u32> band;
  Vector<f32> band_weight;
  for (const u32 id : domain) {
    const Vec3 p = base[id];
    if (std::fabs(p.x) > 0.05f || std::fabs(p.y) > 0.03f) continue;
    binding_domain.push_back(id);
    const f32 sx = std::fabs(p.x) / 0.03f;
    const f32 sy = std::fabs(p.y) / 0.018f;
    const f32 r = std::max(sx, sy);
    if (r <= 1.0f) {
      footprint.push_back(id);
    } else if (r < 1.6f) {
      band.push_back(id);
      const f32 x = (r - 1.0f) / 0.6f;
      band_weight.push_back(1.0f - x * x * (3.0f - 2.0f * x));
    }
  }
  for (f32& w : band_weight)
    w = std::clamp(w, 0.01f, 0.99f);
  Vector<Vec3> top_reference;
  for (const u32 n : top_nodes)
    top_reference.push_back(out.reference_nodes[n]);
  geometry::BindingFrame at_rest;
  geometry::evaluate_binding_frame(geometry::NormalMode::limit_interpolated, out.top, top_reference,
                                   top_reference, at_rest);
  Vector<Vec3> bound;
  for (const u32 id : binding_domain)
    bound.push_back(observed[id]);
  Vector<geometry::SurfaceBinding> records;
  geometry::bind_to_surface(geometry::NormalMode::limit_interpolated, bound,
                            at_rest.view(out.top.faces), geometry::SurfaceBindOptions{}, records);
  Vector<u32> foot_triangles;
  Vector<Vec3> foot_barycentrics;
  Vector<f32> foot_offsets;
  Vector<Vec3> authored_normals;
  for (const geometry::SurfaceBinding& r : records) {
    foot_triangles.push_back(r.triangle);
    foot_barycentrics.push_back(geometry::binding_barycentrics(r));
    foot_offsets.push_back(geometry::binding_normal_offset(r));
    authored_normals.push_back(geometry::footpoint_normal(geometry::NormalMode::limit_interpolated,
                                                          at_rest.view(out.top.faces), r.triangle,
                                                          geometry::binding_barycentrics(r)));
  }
  put(file, "skin.binding.domain", BlockKind::CanonicalIds, binding_domain);
  put(file, "skin.binding.footprint", BlockKind::CanonicalIds, footprint);
  put(file, "skin.binding.band", BlockKind::CanonicalIds, band);
  put(file, "skin.binding.band_weights", BlockKind::IdValues, band_weight);
  put(file, "skin.binding.triangles", BlockKind::FootpointTriangles, foot_triangles);
  put(file, "skin.binding.barycentrics", BlockKind::FootpointBarycentrics, foot_barycentrics);
  put(file, "skin.binding.offsets", BlockKind::FootpointOffsets, foot_offsets);
  put(file, "skin.binding.normals", BlockKind::AuthoredNormals, authored_normals);
  SkinBinding binding;
  binding.name = "skin over slab";
  binding.region = "slab";
  binding.sheet = "top";
  binding.level = level;
  binding.normal_mode = "limit-interpolated";
  binding.domain = "skin.binding.domain";
  binding.footprint.ids = "skin.binding.footprint";
  binding.band.ids = "skin.binding.band";
  binding.band.weights = "skin.binding.band_weights";
  binding.band.width_m = 0.018;
  binding.band.profile = "smoothstep over the square distance from the footprint";
  binding.records.triangles = "skin.binding.triangles";
  binding.records.barycentrics = "skin.binding.barycentrics";
  binding.records.offsets = "skin.binding.offsets";
  binding.records.authored_normals = "skin.binding.normals";
  d.bindings.push_back(binding);

  // ---- the depth budget
  // ---------------------------------------------------------------------------
  DepthBudget budget;
  budget.region = "slab";
  budget.frame = "box";
  budget.slot = "synthetic slot";
  budget.cover_purpose = "a residual layer chosen for the fixture";
  d.budgets.push_back(budget);
  return out;
}

}  // namespace engine::tissue
