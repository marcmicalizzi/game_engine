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

}  // namespace

SyntheticTissue make_synthetic_tissue() {
  SyntheticTissue out;
  TissueFile& file = out.file;
  TissueDefinition& d = file.definition;
  d.format = k_tissue_format;
  d.name = "synthetic slab";
  d.description = "a torus-section slab under a flat skin, on a box frame: generated, not measured";
  d.provenance = "domain/tissue/src/synthetic.cpp, make_synthetic_tissue";

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
  const Vector<Vec3> nodes = region_nodes();
  const Vector<u32> tets = tetrahedra(nodes);
  const Vector<u32> top_faces = layer_faces(0, false);
  const Vector<u32> support_faces = layer_faces(k_layers - 1, true);
  put(file, "slab.nodes", BlockKind::RegionNodes, nodes);
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
  std::sort(rim.begin(), rim.end());
  put(file, "slab.rim", BlockKind::NodeSet, rim);
  put(file, "slab.posterior", BlockKind::NodeSet, posterior);

  Region region;
  region.name = "slab";
  region.nodes = "slab.nodes";
  region.tetrahedra = "slab.tets";
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
  region.sheets.push_back(Sheet{"top", "slab.top", 3});
  region.sheets.push_back(Sheet{"support", "slab.support", 3});
  region.top_sheet = "top";
  region.support_sheet = "support";

  // The closed shell's stitch: the two sheets' refined surfaces have the same numbering (one's
  // control faces are the other's reversed, over nodes in the same order), so the top's rim edge
  // (a, b) pairs with the support's (a + T, b + T).
  geometry::LoopSurfaceOptions options;
  options.level = 3;
  options.tangents = true;
  Vector<u32> top_local = top_faces;  // the top layer's nodes are 0 .. 48 already
  geometry::build_loop_limit_surface(top_local, k_nu * k_nv, options, out.top);
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
  for (u32 k = 0; k < k_nu * k_nv; ++k)
    top_reference.push_back(out.reference_nodes[k]);
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
  binding.level = 3;
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
