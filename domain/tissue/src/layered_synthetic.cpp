// The layered model's worked examples (synthetic.h, "the layered model's worked examples";
// docs/subsystems/tissue.md, "The layered model's fixtures"): a fat slab and a fusiform muscle as
// ten-node reference bodies, generated from formulas.
#include <domain/tissue/sha256.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>

namespace engine::tissue {

namespace {

template <class T>
void put(TissueFile& file, const std::string& name, BlockKind kind, const Vector<T>& data,
         u32 per_element = 1) {
  add_block(file, name, kind, std::span<const T>(data.data(), data.size()), per_element);
}

struct D {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

Vec3 to_vec3(D p) {
  return Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
}

// A structured grid of (nx + 1) (ny + 1) (nz + 1) corners, each hexahedron six Kuhn tetrahedra
// about its main diagonal (conforming across hexahedra), each oriented positive.
struct Grid {
  u32 nx = 0;
  u32 ny = 0;
  u32 nz = 0;
  u32 node(u32 i, u32 j, u32 k) const { return (k * (ny + 1) + j) * (nx + 1) + i; }
  u32 corners() const { return (nx + 1) * (ny + 1) * (nz + 1); }
};

Vector<u32> kuhn(const Grid& g, const Vector<D>& x) {
  Vector<u32> out;
  const u32 table[6][4] = {{0, 1, 3, 7}, {0, 3, 2, 7}, {0, 2, 6, 7},
                           {0, 6, 4, 7}, {0, 4, 5, 7}, {0, 5, 1, 7}};
  for (u32 k = 0; k < g.nz; ++k)
    for (u32 j = 0; j < g.ny; ++j)
      for (u32 i = 0; i < g.nx; ++i) {
        u32 c[8];
        for (u32 b = 0; b < 8; ++b)
          c[b] = g.node(i + (b & 1u), j + ((b >> 1) & 1u), k + ((b >> 2) & 1u));
        for (const auto& t : table) {
          u32 v[4] = {c[t[0]], c[t[1]], c[t[2]], c[t[3]]};
          const D a = x[v[0]];
          const D e1{x[v[1]].x - a.x, x[v[1]].y - a.y, x[v[1]].z - a.z};
          const D e2{x[v[2]].x - a.x, x[v[2]].y - a.y, x[v[2]].z - a.z};
          const D e3{x[v[3]].x - a.x, x[v[3]].y - a.y, x[v[3]].z - a.z};
          const f64 det = e1.x * (e2.y * e3.z - e2.z * e3.y) - e1.y * (e2.x * e3.z - e2.z * e3.x) +
                          e1.z * (e2.x * e3.y - e2.y * e3.x);
          if (det < 0.0) std::swap(v[1], v[2]);
          for (const u32 n : v)
            out.push_back(n);
        }
      }
  return out;
}

// The ten-node mesh over the corners: an edge node at each edge's midpoint (straight-sided cells,
// the canonical reference the packet asks for), numbered after the corners in the order of the
// edges' (lower, higher) keys, the cells in Gmsh's order.
struct P2Mesh {
  Vector<D> x;        // corners, then edge nodes
  Vector<u32> cells;  // ten per cell
  Vector<u64> keys;   // the edges, sorted
  u32 corners = 0;
  static u64 key(u32 a, u32 b) { return (u64{std::min(a, b)} << 32) | u64{std::max(a, b)}; }
  u32 edge_node(u32 a, u32 b) const {
    return corners +
           static_cast<u32>(std::lower_bound(keys.begin(), keys.end(), key(a, b)) - keys.begin());
  }
  u32 end_a(u32 n) const { return static_cast<u32>(keys[n - corners] >> 32); }
  u32 end_b(u32 n) const { return static_cast<u32>(keys[n - corners] & 0xffffffffu); }
  u32 cell_count() const { return static_cast<u32>(cells.size() / 10); }
};

P2Mesh p2_mesh(const Vector<D>& corners, const Vector<u32>& tets) {
  P2Mesh m;
  m.x = corners;
  m.corners = static_cast<u32>(corners.size());
  for (u32 t = 0; t < tets.size(); t += 4)
    for (u32 a = 0; a < 4; ++a)
      for (u32 b = a + 1; b < 4; ++b)
        m.keys.push_back(P2Mesh::key(tets[t + a], tets[t + b]));
  std::sort(m.keys.begin(), m.keys.end());
  m.keys.erase(std::unique(m.keys.begin(), m.keys.end()), m.keys.end());
  for (const u64 k : m.keys) {
    const D a = corners[static_cast<u32>(k >> 32)];
    const D b = corners[static_cast<u32>(k & 0xffffffffu)];
    m.x.push_back(D{0.5 * (a.x + b.x), 0.5 * (a.y + b.y), 0.5 * (a.z + b.z)});
  }
  for (u32 t = 0; t < tets.size(); t += 4) {
    const u32* v = tets.data() + t;
    const u32 cell[10] = {v[0],
                          v[1],
                          v[2],
                          v[3],
                          m.edge_node(v[0], v[1]),
                          m.edge_node(v[1], v[2]),
                          m.edge_node(v[0], v[2]),
                          m.edge_node(v[0], v[3]),
                          m.edge_node(v[2], v[3]),
                          m.edge_node(v[1], v[3])};
    for (const u32 n : cell)
      m.cells.push_back(n);
  }
  return m;
}

// Gmsh's local faces of the ten-node tetrahedron, outward (schemas/tissue.schema,
// BoundaryFaceRefs), and their six nodes.
constexpr u32 k_face_corners[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {3, 1, 2}};
constexpr u32 k_face_nodes[4][6] = {
    {0, 2, 1, 6, 5, 4}, {0, 1, 3, 4, 9, 7}, {0, 3, 2, 7, 8, 6}, {3, 1, 2, 9, 5, 8}};

// The boundary faces (cell, local face) whose three corners all satisfy `on`, in cell order.
template <class On>
Vector<u32> boundary_faces(const P2Mesh& m, On on) {
  // A face is on the boundary when no other cell has its corners.
  Vector<std::pair<u64, u32>> all;  // (sorted corner key, cell * 4 + local)
  const auto face_key = [&](u32 c, u32 l) {
    u32 v[3];
    for (u32 i = 0; i < 3; ++i)
      v[i] = m.cells[10 * c + k_face_corners[l][i]];
    std::sort(v, v + 3);
    // 21 bits a node: the fixtures have far fewer.
    return (u64{v[0]} << 42) | (u64{v[1]} << 21) | u64{v[2]};
  };
  for (u32 c = 0; c < m.cell_count(); ++c)
    for (u32 l = 0; l < 4; ++l)
      all.push_back({face_key(c, l), 4 * c + l});
  std::sort(all.begin(), all.end());
  Vector<u32> out;
  for (u32 c = 0; c < m.cell_count(); ++c)
    for (u32 l = 0; l < 4; ++l) {
      const u64 k = face_key(c, l);
      const auto lo = std::lower_bound(all.begin(), all.end(), std::pair<u64, u32>{k, 0});
      u32 uses = 0;
      for (auto it = lo; it != all.end() && it->first == k; ++it)
        ++uses;
      if (uses != 1) continue;
      bool inside = true;
      for (u32 i = 0; i < 3; ++i)
        inside = inside && on(m.x[m.cells[10 * c + k_face_corners[l][i]]]);
      if (!inside) continue;
      out.push_back(c);
      out.push_back(l);
    }
  return out;
}

// The nodes of a set of faces, ascending.
Vector<u32> face_nodes(const P2Mesh& m, const Vector<u32>& refs) {
  Vector<u32> out;
  for (u32 f = 0; f + 1 < refs.size(); f += 2)
    for (u32 k = 0; k < 6; ++k)
      out.push_back(m.cells[10 * refs[f] + k_face_nodes[refs[f + 1]][k]]);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Vector<Vec3> as_vec3(const Vector<D>& x) {
  Vector<Vec3> out;
  for (const D& p : x)
    out.push_back(to_vec3(p));
  return out;
}

// A closed box, wound outward: the synthetic slab's frame's winding.
void put_box(TissueFile& file, const std::string& name, D lo, D hi) {
  const Vector<Vec3> box = {to_vec3(D{lo.x, lo.y, lo.z}), to_vec3(D{hi.x, lo.y, lo.z}),
                            to_vec3(D{hi.x, hi.y, lo.z}), to_vec3(D{lo.x, hi.y, lo.z}),
                            to_vec3(D{lo.x, lo.y, hi.z}), to_vec3(D{hi.x, lo.y, hi.z}),
                            to_vec3(D{hi.x, hi.y, hi.z}), to_vec3(D{lo.x, hi.y, hi.z})};
  const Vector<u32> faces = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                             1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  put(file, name + ".vertices", BlockKind::FrameVertices, box);
  put(file, name + ".triangles", BlockKind::FrameTriangles, faces, 3);
  Frame frame;
  frame.name = name;
  frame.vertices = name + ".vertices";
  frame.triangles = name + ".triangles";
  frame.provenance = "a box placed by formula for the fixture";
  frame.proxy = true;
  file.definition.frames.push_back(frame);
}

RegionState state(std::string name, std::string region, StateRole role, StateProvenance provenance,
                  std::string nodes, std::string pose) {
  RegionState s;
  s.name = std::move(name);
  s.region = std::move(region);
  s.role = role;
  s.provenance = provenance;
  s.nodes = std::move(nodes);
  s.load.gravity = Vec3{0.0f, 0.0f, 0.0f};  // zero, to isolate the mechanisms (RUNS.md)
  s.load.pose = std::move(pose);
  return s;
}

std::string block_hash(const TissueFile& file, const std::string& name) {
  const TissueBlock* b = file.find(name);
  return b != nullptr ? sha256_hex(b->bytes) : std::string();
}

ContactPolicy synthetic_policy(std::string name) {
  ContactPolicy p;
  p.name = std::move(name);
  // What the execution declaration pins, it pins: a fixture names none of it.
  p.potential = "synthetic-unpinned";
  p.proxy_rule = "synthetic-unpinned";
  p.activation_m = 0.0005;
  p.locality_m = 0.005;
  // CONTACT.md's proposed certificate bounds.
  p.proxy_epsilon_m = 1.0e-5;
  p.max_depth = 10;
  p.max_subtriangles = 250000;
  p.max_feature_tests = 2000000;
  p.note = "the fixture's: the potential and the proxy are the execution declaration's to pin";
  return p;
}

}  // namespace

// ---- the slab -----------------------------------------------------------------------------------

TissueFile make_layered_slab() {
  TissueFile file;
  TissueDefinition& d = file.definition;
  d.format = k_tissue_format;
  d.name = "layered slab";
  d.description =
      "a fat slab of ten-node cells over a closed base, with a thickness field, two depots, a "
      "mechanical skin on its upper surface and face contact: generated, not measured";
  d.provenance = "domain/tissue/src/layered_synthetic.cpp, make_layered_slab";

  constexpr f64 length = 0.16;
  constexpr f64 width = 0.08;
  constexpr f64 thickness = 0.015;
  const Grid g{8, 4, 2};
  Vector<D> corners(g.corners());
  for (u32 k = 0; k <= g.nz; ++k)
    for (u32 j = 0; j <= g.ny; ++j)
      for (u32 i = 0; i <= g.nx; ++i)
        corners[g.node(i, j, k)] = D{-0.5 * length + length * i / g.nx,
                                     -0.5 * width + width * j / g.ny, thickness * k / g.nz};
  const P2Mesh m = p2_mesh(corners, kuhn(g, corners));
  const Vector<Vec3> nodes = as_vec3(m.x);
  put(file, "fat.nodes", BlockKind::RegionNodes, nodes);
  put(file, "fat.cells", BlockKind::QuadraticTetrahedra, m.cells, 10);

  Region region;
  region.name = "fat";
  region.nodes = "fat.nodes";
  region.tetrahedra = "fat.cells";
  region.cage = CageKind::TetrahedralQuadratic;
  region.role = RegionRole::Reference;
  MaterialPhase fat;
  fat.material = Material{"fat", 100.0e3, 350.0, 950.0};  // DESIGN.md's slab constants
  region.phases.push_back(fat);
  d.regions.push_back(region);

  // ---- the states: rest and reference at the construction, and a compression
  put(file, "state.rest", BlockKind::StateNodes, nodes);
  put(file, "state.reference", BlockKind::StateNodes, nodes);
  Vector<Vec3> compressed;
  for (const D& p : m.x) {
    const f64 s = std::sin(std::numbers::pi * (p.x + 0.5 * length) / length);
    compressed.push_back(to_vec3(D{0.97 * p.x, p.y, p.z + 0.00025 * s * s}));
  }
  put(file, "state.compressed", BlockKind::StateNodes, compressed);
  d.states.push_back(
      state("rest", "fat", StateRole::Rest, StateProvenance::Authored, "state.rest", "unloaded"));
  d.states.push_back(state("reference", "fat", StateRole::Reference, StateProvenance::Authored,
                           "state.reference", "unloaded"));
  d.states.push_back(state("compressed", "fat", StateRole::Response,
                           StateProvenance::ForwardFromReference, "state.compressed",
                           "shortened 3% with a 0.25 mm upward bias"));

  // ---- the base frame, 0.7 mm under the lower surface
  put_box(file, "base", D{-0.10, -0.06, -0.0207}, D{0.10, 0.06, -0.0007});

  // ---- the material boundary: the upper and the lower surfaces
  const Vector<u32> upper = boundary_faces(m, [&](D p) { return p.z > thickness - 1.0e-9; });
  const Vector<u32> lower = boundary_faces(m, [&](D p) { return p.z < 1.0e-9; });
  put(file, "surface.upper", BlockKind::BoundaryFaceRefs, upper, 2);
  put(file, "surface.lower", BlockKind::BoundaryFaceRefs, lower, 2);
  d.material_surfaces.push_back(
      MaterialBoundarySurface{"upper", "fat", "surface.upper", "p2-six-node-v1", "the skin's"});
  d.material_surfaces.push_back(MaterialBoundarySurface{
      "lower", "fat", "surface.lower", "p2-six-node-v1", "the thickness field's reference"});

  // ---- the thickness field and its depots
  ThicknessField field;
  field.name = "fat";
  field.region = "fat";
  field.surface = "lower";
  field.normal_convention = "inward-normal-v1";
  field.law = "uniform-v1";
  field.mean_thickness_m = thickness;
  field.declared_volume_m3 = length * width * thickness;
  field.declared_mass_kg = 950.0 * length * width * thickness;
  d.thickness_fields.push_back(field);
  Vector<f32> left;
  Vector<f32> right;
  for (u32 c = 0; c < m.cell_count(); ++c) {
    f64 x = 0.0;
    for (u32 k = 0; k < 4; ++k)
      x += m.x[m.cells[10 * c + k]].x / 4.0;
    left.push_back(x < 0.0 ? 1.0f : 0.0f);
    right.push_back(x < 0.0 ? 0.0f : 1.0f);
  }
  put(file, "depot.left", BlockKind::PhaseFraction, left);
  put(file, "depot.right", BlockKind::PhaseFraction, right);
  DepotPartition partition;
  partition.name = "depots";
  partition.field = "fat";
  const f64 half = 0.5 * length * width * thickness;
  partition.depots.push_back(Depot{"left", "depot.left", half, 950.0 * half});
  partition.depots.push_back(Depot{"right", "depot.right", half, 950.0 * half});
  d.depot_partitions.push_back(partition);

  // ---- the mechanical skin on the upper surface, sampled at every node of it
  const Vector<u32> skin_nodes = face_nodes(m, upper);
  Vector<u32> ids;
  Vector<u32> points;  // u32 face, then three f32: written as the 16-byte element
  for (u32 i = 0; i < skin_nodes.size(); ++i) {
    ids.push_back(i);
    u32 face = 0;
    f32 b[3] = {1.0f, 0.0f, 0.0f};
    bool found = false;
    for (u32 f = 0; f + 1 < upper.size() && !found; f += 2)
      for (u32 k = 0; k < 6 && !found; ++k)
        if (m.cells[10 * upper[f] + k_face_nodes[upper[f + 1]][k]] == skin_nodes[i]) {
          face = f / 2;
          const f32 at[6][3] = {{1, 0, 0},       {0, 1, 0},       {0, 0, 1},
                                {0.5f, 0.5f, 0}, {0, 0.5f, 0.5f}, {0.5f, 0, 0.5f}};
          for (u32 q = 0; q < 3; ++q)
            b[q] = at[k][q];
          found = true;
        }
    points.push_back(face);
    for (const f32 v : b) {
      u32 bits = 0;
      std::memcpy(&bits, &v, 4);
      points.push_back(bits);
    }
  }
  put(file, "skin.ids", BlockKind::CanonicalIds, ids);
  put(file, "skin.points", BlockKind::MaterialSurfaceCoordinates, points, 4);
  MaterialSkin skin;
  skin.name = "skin";
  skin.surface = "upper";
  skin.law = "synthetic-unpinned";
  skin.stiffness_n_per_m = 80.0;  // DESIGN.md's E.t
  skin.poisson = 0.45;
  skin.coincident = true;
  skin.sample_ids = "skin.ids";
  skin.sample_coordinates = "skin.points";
  d.material_skins.push_back(skin);

  // ---- contact: the skin against itself, and the lower surface against the base
  d.contact_policies.push_back(synthetic_policy("slab"));
  // Two upper faces that share an edge: an exact incident exclusion (legal and redundant: incident
  // chords are exempt anyway), the one the fixture carries to exercise the row.
  u32 partner = 0;
  for (u32 f = 2; f + 1 < upper.size() && partner == 0; f += 2) {
    u32 shared = 0;
    for (u32 k = 0; k < 3; ++k)
      for (u32 l = 0; l < 3; ++l)
        shared += m.cells[10 * upper[0] + k_face_corners[upper[1]][k]] ==
                          m.cells[10 * upper[f] + k_face_corners[upper[f + 1]][l]]
                      ? 1u
                      : 0u;
    if (shared == 2) partner = f;
  }
  const Vector<u32> exclusion_a = {upper[0], upper[1]};
  const Vector<u32> exclusion_b = {upper[partner], upper[partner + 1]};
  put(file, "exclusion.a", BlockKind::BoundaryFaceRefs, exclusion_a, 2);
  put(file, "exclusion.b", BlockKind::BoundaryFaceRefs, exclusion_b, 2);
  ContactPair self;
  self.name = "skin on skin";
  self.a = ContactSide{ContactSurfaceKind::MaterialSurface, "upper", "fat"};
  self.b = ContactSide{ContactSurfaceKind::MaterialSurface, "upper", "fat"};
  self.self_contact = true;
  self.offset_m = 0.001;  // CONTACT.md: two 0.5 mm collision half-thicknesses
  self.policy = "slab";
  self.exclusions.push_back(ContactExclusion{ExclusionReason::Incident, "exclusion.a",
                                             "exclusion.b", "", "two faces that share an edge"});
  d.contact_pairs.push_back(self);
  ContactPair base;
  base.name = "fat on base";
  base.a = ContactSide{ContactSurfaceKind::MaterialSurface, "lower", "fat"};
  base.b = ContactSide{ContactSurfaceKind::Frame, "base", "base"};
  base.offset_m = 0.0001;  // CONTACT.md: fat against a rigid frame
  base.policy = "slab";
  d.contact_pairs.push_back(base);

  // ---- a certificate record: provenance of nothing certified
  ReferenceCertificate certificate;
  certificate.name = "reference";
  certificate.region = "fat";
  certificate.state = "reference";
  certificate.state_sha256 = block_hash(file, "state.reference");
  certificate.authority = "none: a generated fixture";
  certificate.implementation = "none";
  certificate.outcome = "not certified";
  certificate.note = "the record's shape, for the provenance row; nothing was run";
  d.certificates.push_back(certificate);

  d.requirements.records = {"ThicknessField",      "DepotPartition", "MaterialBoundarySurface",
                            "MaterialSkin",        "ContactPair",    "ContactPolicy",
                            "ReferenceCertificate"};
  d.requirements.blocks = {"BoundaryFaceRefs", "MaterialSurfaceCoordinates"};
  d.requirements.rows = {"surface.material_boundary",
                         "surface.orientation",
                         "surface.embedding",
                         "fat.thickness_positive",
                         "fat.field_jacobian",
                         "fat.depot_partition",
                         "fat.mass_ledger",
                         "skin.material_binding",
                         "skin.rest_metric",
                         "contact.pairs",
                         "contact.exclusions",
                         "contact.curved_clearance",
                         "contact.constraint_compatibility"};
  d.requirements.laws = {"uniform-v1", "p2-six-node-v1"};
  return file;
}

// ---- the fusiform muscle ------------------------------------------------------------------------

TissueFile make_layered_fusiform() {
  TissueFile file;
  TissueDefinition& d = file.definition;
  d.format = k_tissue_format;
  d.name = "layered fusiform";
  d.description =
      "a fusiform muscle of ten-node cells between two box handles, essential at the origin and a "
      "spring at the insertion, with an active rest driver and two parameter samples: generated, "
      "not measured";
  d.provenance = "domain/tissue/src/layered_synthetic.cpp, make_layered_fusiform";

  constexpr f64 length = 0.12;
  const f64 theta = std::numbers::pi / 3.0;
  const auto radius = [&](f64 x) {
    const f64 s = 2.0 * x / length;
    return 0.005 + 0.010 * (1.0 - s * s);
  };
  const Grid g{12, 2, 2};
  Vector<D> corners(g.corners());
  Vector<f64> along(g.corners());  // i / nx: the pose's blend runs on it
  for (u32 k = 0; k <= g.nz; ++k)
    for (u32 j = 0; j <= g.ny; ++j)
      for (u32 i = 0; i <= g.nx; ++i) {
        const f64 x = -0.5 * length + length * i / g.nx;
        const f64 s = -1.0 + 2.0 * j / g.ny;
        const f64 t = -1.0 + 2.0 * k / g.nz;
        // The square to the disk, Fong's elliptical grid mapping.
        const f64 u = s * std::sqrt(1.0 - 0.5 * t * t);
        const f64 v = t * std::sqrt(1.0 - 0.5 * s * s);
        corners[g.node(i, j, k)] = D{x, radius(x) * u, radius(x) * v};
        along[g.node(i, j, k)] = static_cast<f64>(i) / g.nx;
      }
  const P2Mesh m = p2_mesh(corners, kuhn(g, corners));
  for (u32 n = m.corners; n < m.x.size(); ++n)
    along.push_back(0.5 * (along[m.end_a(n)] + along[m.end_b(n)]));
  const Vector<Vec3> nodes = as_vec3(m.x);
  put(file, "muscle.nodes", BlockKind::RegionNodes, nodes);
  put(file, "muscle.cells", BlockKind::QuadraticTetrahedra, m.cells, 10);

  // ---- node sets: the two ends and the origin's collar, the ring one segment in
  Vector<u32> origin;
  Vector<u32> insertion;
  Vector<u32> collar;
  for (u32 n = 0; n < m.x.size(); ++n) {
    if (along[n] == 0.0) origin.push_back(n);
    if (along[n] == 1.0) insertion.push_back(n);
    if (n < m.corners && along[n] == 1.0 / g.nx) collar.push_back(n);
  }
  put(file, "set.origin", BlockKind::NodeSet, origin);
  put(file, "set.insertion", BlockKind::NodeSet, insertion);
  put(file, "set.collar", BlockKind::NodeSet, collar);

  // ---- the rest driver's samples: the identity, and a = 0.5 at 60 degrees
  const auto law = [&](D p, f64 a, f64 pose) {
    const f64 lambda = 1.0 - 0.2 * a;
    const f64 root = std::sqrt(lambda);
    const f64 s = 2.0 * p.x / length;
    return D{lambda * p.x, p.y / root, p.z / root + 0.004 * std::sin(pose) * (1.0 - s * s)};
  };
  Vector<Vec3> active;
  for (const Vec3& p : nodes)
    active.push_back(to_vec3(law(D{p.x, p.y, p.z}, 0.5, theta)));
  put(file, "sample.identity", BlockKind::StateNodes, nodes);
  put(file, "sample.active", BlockKind::StateNodes, active);

  Region region;
  region.name = "muscle";
  region.nodes = "muscle.nodes";
  region.tetrahedra = "muscle.cells";
  region.cage = CageKind::TetrahedralQuadratic;
  region.role = RegionRole::Reference;
  MaterialPhase muscle;
  muscle.material = Material{"muscle", 100.0e3, 1000.0, 1050.0};  // DESIGN.md's fusiform
  region.phases.push_back(muscle);
  region.node_sets.push_back(NodeSetRef{"origin", "set.origin", "the origin's patch"});
  region.node_sets.push_back(NodeSetRef{"insertion", "set.insertion", "the insertion's patch"});
  region.node_sets.push_back(NodeSetRef{"collar", "set.collar", "the origin's transition collar"});
  ActiveRestShape driver;
  driver.law = "fusiform-arch-v1";
  JsonValue coefficients = JsonValue::object();
  coefficients.set("activation_contraction", JsonValue(0.2));
  coefficients.set("arch_m", JsonValue(0.004));
  driver.coefficients = std::move(coefficients);
  driver.reference_state = "reference";
  driver.origin_m = {0.0, 0.0, 0.0};
  driver.axis = {1.0, 0.0, 0.0};
  driver.transverse = {0.0, 1.0, 0.0};
  driver.natural_length_m = length;
  driver.domain = "muscle";
  driver.activation = "activation";
  driver.pose = "pose_angle";
  driver.samples = {"identity", "active"};
  region.rest_driver = driver;
  d.regions.push_back(region);

  ParameterDomain domain;
  domain.name = "muscle";
  domain.parameters.push_back(Parameter{"activation", "1", 0.0, 1.0, 0.0, "rest_driver:muscle"});
  domain.parameters.push_back(
      Parameter{"pose_angle", "rad", 0.0, theta, 0.0, "rest_driver:muscle"});
  domain.note = "RUNS.md's authored ranges: activation 0 to 1, pose 0 to 60 degrees";
  d.parameter_domains.push_back(domain);
  ParameterSample identity;
  identity.name = "identity";
  identity.domain = "muscle";
  identity.region = "muscle";
  identity.values = {ParameterValue{"activation", 0.0}, ParameterValue{"pose_angle", 0.0}};
  identity.snapshot = "sample.identity";
  identity.snapshot_sha256 = block_hash(file, "sample.identity");
  d.parameter_samples.push_back(identity);
  ParameterSample half;
  half.name = "active";
  half.domain = "muscle";
  half.region = "muscle";
  half.values = {ParameterValue{"activation", 0.5}, ParameterValue{"pose_angle", theta}};
  half.snapshot = "sample.active";
  half.snapshot_sha256 = block_hash(file, "sample.active");
  d.parameter_samples.push_back(half);

  // ---- the states: rest and reference at the construction; posed, the insertion turned 60
  // degrees about y through the origin and every node turned by a smoothstep of its place along
  // the body, so the origin's ring stays where it is and the insertion's turns with its handle.
  const auto turn = [](D p, f64 angle) {
    const f64 c = std::cos(angle);
    const f64 s = std::sin(angle);
    return D{c * p.x - s * p.z, p.y, s * p.x + c * p.z};
  };
  Vector<Vec3> posed;
  for (u32 n = 0; n < m.x.size(); ++n) {
    const f64 t = along[n];
    const f64 w = t * t * (3.0 - 2.0 * t);
    posed.push_back(to_vec3(turn(D{nodes[n].x, nodes[n].y, nodes[n].z}, theta * w)));
  }
  put(file, "state.rest", BlockKind::StateNodes, nodes);
  put(file, "state.reference", BlockKind::StateNodes, nodes);
  put(file, "state.posed", BlockKind::StateNodes, posed);
  d.states.push_back(state("rest", "muscle", StateRole::Rest, StateProvenance::Authored,
                           "state.rest", "unloaded"));
  d.states.push_back(state("reference", "muscle", StateRole::Reference, StateProvenance::Authored,
                           "state.reference", "unloaded"));
  d.states.push_back(state("posed", "muscle", StateRole::Response,
                           StateProvenance::ForwardFromReference, "state.posed",
                           "the insertion turned 60 degrees, the body bent to follow: prescribed"));

  // ---- the handles, 0.7 mm beyond the ends, and the insertion's turn
  put_box(file, "origin handle", D{-0.0707, -0.008, -0.008}, D{-0.0607, 0.008, 0.008});
  put_box(file, "insertion handle", D{0.0607, -0.008, -0.008}, D{0.0707, 0.008, 0.008});
  FrameState at_construction;
  at_construction.frame = "insertion handle";
  at_construction.state = "construction";
  at_construction.rotation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  at_construction.translation_m = {0, 0, 0};
  at_construction.interpolation = "synthetic-unpinned";
  d.frame_states.push_back(at_construction);
  FrameState turned = at_construction;
  turned.state = "posed";
  turned.rotation = {std::cos(theta), 0, -std::sin(theta), 0, 1, 0,
                     std::sin(theta), 0, std::cos(theta)};
  d.frame_states.push_back(turned);

  // ---- the material boundary: the whole closed boundary, and the origin's patch on it
  const Vector<u32> boundary = boundary_faces(m, [](D) { return true; });
  const Vector<u32> patch = boundary_faces(m, [&](D p) { return p.x < -0.5 * length + 1.0e-9; });
  put(file, "surface.boundary", BlockKind::BoundaryFaceRefs, boundary, 2);
  put(file, "patch.origin", BlockKind::BoundaryFaceRefs, patch, 2);
  d.material_surfaces.push_back(MaterialBoundarySurface{
      "boundary", "muscle", "surface.boundary", "p2-six-node-v1", "the whole closed boundary"});

  // ---- the attachments: essential at the origin, a spring at the insertion
  Attachment o;
  o.name = "origin";
  o.region = "muscle";
  o.nodes = "origin";
  o.kind = AttachmentKind::Fixed;
  o.target_kind = TargetKind::Frame;
  o.target = "origin handle";
  o.enforcement = AttachmentEnforcement::Essential;
  o.patch_surface = "boundary";
  o.patch = "patch.origin";
  o.transition_band = "collar";
  d.attachments.push_back(o);
  Attachment ins;
  ins.name = "insertion";
  ins.region = "muscle";
  ins.nodes = "insertion";
  ins.kind = AttachmentKind::Fixed;
  ins.target_kind = TargetKind::Frame;
  ins.target = "insertion handle";
  ins.stiffness_pa = 1.0e5;
  d.attachments.push_back(ins);

  // ---- contact against both handles; the origin's patch is its own
  d.contact_policies.push_back(synthetic_policy("muscle"));
  ContactPair at_origin;
  at_origin.name = "muscle on origin handle";
  at_origin.a = ContactSide{ContactSurfaceKind::MaterialSurface, "boundary", "muscle"};
  at_origin.b = ContactSide{ContactSurfaceKind::Frame, "origin handle", "origin handle"};
  at_origin.offset_m = 0.0001;
  at_origin.policy = "muscle";
  at_origin.exclusions.push_back(ContactExclusion{ExclusionReason::Attachment, "patch.origin", "",
                                                  "origin", "the essential patch's own frame"});
  d.contact_pairs.push_back(at_origin);
  ContactPair at_insertion = at_origin;
  at_insertion.name = "muscle on insertion handle";
  at_insertion.b = ContactSide{ContactSurfaceKind::Frame, "insertion handle", "insertion handle"};
  at_insertion.exclusions.clear();  // a spring: every pairing stays active
  d.contact_pairs.push_back(at_insertion);

  d.requirements.records = {"ParameterDomain",
                            "ParameterSample",
                            "ActiveRestShape",
                            "Region.rest_driver",
                            "FrameState",
                            "Attachment.enforcement",
                            "AttachmentEnforcement.Essential",
                            "MaterialBoundarySurface",
                            "ContactPair",
                            "ContactPolicy"};
  d.requirements.blocks = {"BoundaryFaceRefs"};
  d.requirements.rows = {"parameters.domain",        "state.parameters",
                         "muscle.rest_identity",     "muscle.target_volume",
                         "muscle.passive_reference", "muscle.objectivity",
                         "frame.rigidity",           "attachment.enforcement",
                         "attachment.pose_binding",  "attachment.contact_compatibility",
                         "contact.curved_clearance"};
  d.requirements.laws = {"fusiform-arch-v1", "p2-six-node-v1"};
  return file;
}

}  // namespace engine::tissue
