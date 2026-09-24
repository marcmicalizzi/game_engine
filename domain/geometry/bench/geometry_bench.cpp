// What a deformable region's render surface costs per frame once its cage has moved
// (docs/subsystems/geometry.md, "Limit surfaces and surface bindings"; docs/plan/05-simulation.md
// §5.16's 2026-09-23 addendum, item 3: "evaluated once into a per-region sparse matrix so the
// per-vertex record stays small" and the per-frame cost is a small matvec).
//
// **The rows.**
//   geometry.limit.apply.fixture   the neutral torso fixture's top surface: 121 nodes, level 3,
//                                  7,009 x 121 with 83,233 nonzeros — the authoring side's matrix.
//   geometry.limit.apply.fixture.one_sum  the same product through the first kernel, one running
//                                  sum a row: the comparison `apply`'s lanes and partial sums
//                                  are measured against.
//   geometry.limit.apply.cage800   an 800-node closed surface (a 40 x 20 torus) at level 3: the
//                                  cage-size limit (ADR-0029's 800) with every node on the surface,
//                                  the largest operator a region can ask for.
//   geometry.limit.normals.fixture the two tangent matvecs and the cross products that give exact
//                                  limit normals at every refined vertex — what a validator or a
//                                  dense surface drawn on its own would pay, and why the binding
//                                  does not.
//   geometry.binding.apply.<n>     the displacement transfer for n bound render vertices on the
//                                  fixture's level-3 surface, under the plan's limit-interpolated
//                                  normal: 914 is one side of the fixture's footprint, 8,192 a
//                                  whole region at the render density.
//   geometry.binding.apply.triangle/914  the same under the footpoint triangle's own normal, the
//                                  mode the transfer had before 2026-09-24.
//   geometry.binding.field.<mode>  the per-frame normal field the interpolated modes read, over all
//                                  7,009 refined vertices: `limit` is the two tangent matvecs of
//                                  the nodes' displacement and each vertex's unit-tangent, cross
//                                  and normal changes; `area` each refined triangle's cross-product
//                                  change, the state's own sums and each vertex's normal change.
//   geometry.limit.build.fixture   the content-build cost of the fixture's operator, for scale.
//   geometry.vertex_ids.position_weld.<n>  deriving canonical vertex ids for n imported vertices
//                                  (geometry.md, "Canonical vertex identity"): a content-build
//                                  cost every glTF import without authored ids pays once.
//
// Numbers and the machine's state they were taken on: docs/subsystems/geometry.md.
#include "../tests/limit_fixtures.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/limit_surface.h>
#include <domain/geometry/surface_binding.h>
#include <foundation/bench/bench.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

struct Operator {
  LoopLimitSurface surface;
  Vector<Vec3> nodes;
  Vector<Vec3> state;  // the nodes moved, so nothing is the identity
  Vector<Vec3> reference;
  Vector<Vec3> out;
};

void build_operator(std::span<const u32> faces, u32 vertices, Vector<Vec3> nodes, bool tangents,
                    Operator& op) {
  LoopSurfaceOptions options;
  options.level = 3;
  options.tangents = tangents;
  std::string error;
  if (!build_loop_limit_surface(faces, vertices, options, op.surface, &error)) return;
  op.nodes = std::move(nodes);
  fixture::Random random(17);
  op.state = op.nodes;
  for (Vec3& p : op.state)
    p = p + Vec3{random.uniform(-0.01f, 0.01f), random.uniform(-0.01f, 0.01f),
                 random.uniform(-0.01f, 0.01f)};
  op.reference.resize(op.surface.limit.rows());
  apply(op.surface.limit, op.nodes, std::span<Vec3>(op.reference.data(), op.reference.size()));
  op.out.resize(op.surface.limit.rows());
}

Operator& fixture_operator() {
  static Operator op = [] {
    Operator o;
    build_operator(fixture::ringed_disk_faces(5, 24), fixture::ringed_disk_vertices(5, 24),
                   fixture::ringed_disk_dome(5, 24, 0.070f, 0.086f, 0.080f), true, o);
    return o;
  }();
  return op;
}

Operator& cage800() {
  static Operator op = [] {
    Operator o;
    build_operator(fixture::torus_faces(40, 20), 800,
                   fixture::torus_positions(40, 20, 0.12f, 0.05f), false, o);
    return o;
  }();
  return op;
}

// The fixture's surface at its state under each normal mode (surface_binding.h): the displacement
// and, for the interpolated modes, the reference vertex normals and their change, evaluated once so
// the transfer rows time the transfer alone.
struct Frames {
  BindingFrame mode[k_normal_mode_count];
};

Frames& fixture_frames() {
  static Frames frames = [] {
    Frames f;
    Operator& op = fixture_operator();
    for (u32 m = 0; m < k_normal_mode_count; ++m)
      evaluate_binding_frame(static_cast<NormalMode>(m), op.surface, op.nodes, op.state, f.mode[m]);
    return f;
  }();
  return frames;
}

// Render vertices bound to the fixture's level-3 surface: points a few millimetres off it, spread
// over the footprint, as a region's skin would be, bound under the plan's limit-interpolated rule.
struct Bound {
  Vector<Vec3> base;
  Vector<SurfaceBinding> bindings;
  Vector<Vec3> out;
};

Bound make_bound(u32 count) {
  const Operator& op = fixture_operator();
  const BindingFrame& rest =
      fixture_frames().mode[static_cast<u32>(NormalMode::limit_interpolated)];
  Bound b;
  fixture::Random random(29 + count);
  const u32 triangles = op.surface.triangle_count();
  for (u32 i = 0; i < count; ++i) {
    const u32 t = static_cast<u32>(random.next() % triangles);
    const f32 w0 = random.uniform(0.1f, 0.6f);
    const f32 w1 = random.uniform(0.1f, 0.9f - w0);
    const Vec3 a = op.reference[op.surface.faces[3 * t]];
    const Vec3 c1 = op.reference[op.surface.faces[3 * t + 1]];
    const Vec3 c2 = op.reference[op.surface.faces[3 * t + 2]];
    const Vec3 n = normalize(cross(c1 - a, c2 - a));
    b.base.push_back(a * w0 + c1 * w1 + c2 * (1.0f - w0 - w1) +
                     n * random.uniform(-0.004f, 0.004f));
  }
  SurfaceBindOptions options;
  std::string error;
  bind_to_surface(NormalMode::limit_interpolated, b.base, rest.view(op.surface.faces), options,
                  b.bindings, nullptr, &error);
  b.out.resize(count);
  return b;
}

constexpr i64 k_bound_counts[] = {914, 8192};

}  // namespace

ENGINE_BENCH(geometry_limit_apply_fixture, "geometry.limit.apply.fixture") {
  Operator& op = fixture_operator();
  const std::span<Vec3> out(op.out.data(), op.out.size());
  while (state.keep_running()) {
    apply(op.surface.limit, op.state, out);
    bench::keep(op.out[op.out.size() - 1]);
  }
  state.set_items(op.surface.limit.nonzeros());
  state.set_bytes(op.surface.limit.bytes() + op.out.size() * sizeof(Vec3));
}

// The first kernel, kept as the comparison `apply`'s shape rests on: one running sum a row and
// three scalar gathers an entry, so each row is one chain of dependent additions. Same matrix,
// same state, same output to f32 rounding.
ENGINE_BENCH(geometry_limit_apply_fixture_one_sum, "geometry.limit.apply.fixture.one_sum") {
  Operator& op = fixture_operator();
  const CsrMatrix& m = op.surface.limit;
  while (state.keep_running()) {
    u32 e = 0;
    for (u32 r = 0; r < m.rows(); ++r) {
      const u32 end = m.row_offsets[r + 1];
      f32 sx = 0.0f;
      f32 sy = 0.0f;
      f32 sz = 0.0f;
      for (; e < end; ++e) {
        const Vec3 p = op.state[m.column_index[e]];
        const f32 w = m.weight[e];
        sx += w * p.x;
        sy += w * p.y;
        sz += w * p.z;
      }
      op.out[r] = Vec3{sx, sy, sz};
    }
    bench::keep(op.out[op.out.size() - 1]);
  }
  state.set_items(m.nonzeros());
}

ENGINE_BENCH(geometry_limit_apply_cage800, "geometry.limit.apply.cage800") {
  Operator& op = cage800();
  const std::span<Vec3> out(op.out.data(), op.out.size());
  while (state.keep_running()) {
    apply(op.surface.limit, op.state, out);
    bench::keep(op.out[op.out.size() - 1]);
  }
  state.set_items(op.surface.limit.nonzeros());
  state.set_bytes(op.surface.limit.bytes() + op.out.size() * sizeof(Vec3));
}

ENGINE_BENCH(geometry_limit_normals_fixture, "geometry.limit.normals.fixture") {
  Operator& op = fixture_operator();
  Vector<Vec3> tu(op.surface.tangent_u.rows());
  Vector<Vec3> tv(op.surface.tangent_v.rows());
  Vector<Vec3> normals(op.surface.tangent_u.rows());
  while (state.keep_running()) {
    apply(op.surface.tangent_u, op.state, std::span<Vec3>(tu.data(), tu.size()));
    apply(op.surface.tangent_v, op.state, std::span<Vec3>(tv.data(), tv.size()));
    limit_normals(tu, tv, std::span<Vec3>(normals.data(), normals.size()));
    bench::keep(normals[normals.size() - 1]);
  }
  state.set_items(op.surface.tangent_u.nonzeros() + op.surface.tangent_v.nonzeros());
}

ENGINE_BENCH_ARGS(geometry_binding_apply, "geometry.binding.apply", 914, 8192) {
  const u32 count = static_cast<u32>(state.arg());
  static Bound bound[2] = {make_bound(static_cast<u32>(k_bound_counts[0])),
                           make_bound(static_cast<u32>(k_bound_counts[1]))};
  Bound& b = bound[count == static_cast<u32>(k_bound_counts[0]) ? 0 : 1];
  Operator& op = fixture_operator();
  // The surface's displacement and normal field at the state are computed once here, so the row is
  // the transfer alone (add geometry.limit.apply.fixture and geometry.binding.field.limit for the
  // whole per-frame cost).
  const BindingFrame& f = fixture_frames().mode[static_cast<u32>(NormalMode::limit_interpolated)];
  const BindingSurface surface = f.view(op.surface.faces);
  while (state.keep_running()) {
    apply_binding(NormalMode::limit_interpolated, b.bindings, b.base, surface,
                  std::span<Vec3>(b.out.data(), b.out.size()));
    bench::keep(b.out[b.out.size() - 1]);
  }
  state.set_items(count);
}

// The same transfer under the other two modes, for the comparison the mode choice rests on.
ENGINE_BENCH(geometry_binding_apply_triangle, "geometry.binding.apply.triangle/914") {
  static Bound b = make_bound(static_cast<u32>(k_bound_counts[0]));
  Operator& op = fixture_operator();
  const BindingSurface surface =
      fixture_frames().mode[static_cast<u32>(NormalMode::triangle)].view(op.surface.faces);
  while (state.keep_running()) {
    apply_binding(NormalMode::triangle, b.bindings, b.base, surface,
                  std::span<Vec3>(b.out.data(), b.out.size()));
    bench::keep(b.out[b.out.size() - 1]);
  }
  state.set_items(b.base.size());
}

// The per-frame normal field of the limit rule, over every refined vertex of the fixture: the two
// tangent matvecs of the nodes' displacement and each vertex's unit-tangent, cross-product and
// normal changes. The reference half (the tangents at the reference and the reference normals) is
// evaluated once and kept, as a per-frame pass would.
ENGINE_BENCH(geometry_binding_field_limit, "geometry.binding.field.limit") {
  Operator& op = fixture_operator();
  const u32 rows = op.surface.limit.rows();
  Vector<Vec3> tu(rows);
  Vector<Vec3> tv(rows);
  apply(op.surface.tangent_u, op.nodes, std::span<Vec3>(tu.data(), rows));
  apply(op.surface.tangent_v, op.nodes, std::span<Vec3>(tv.data(), rows));
  Vector<Vec3> node_displacement(op.nodes.size());
  Vector<Vec3> dtu(rows);
  Vector<Vec3> dtv(rows);
  Vector<Vec3> change(rows);
  while (state.keep_running()) {
    for (u32 c = 0; c < op.nodes.size(); ++c)
      node_displacement[c] = op.state[c] - op.nodes[c];
    apply(op.surface.tangent_u, node_displacement, std::span<Vec3>(dtu.data(), rows));
    apply(op.surface.tangent_v, node_displacement, std::span<Vec3>(dtv.data(), rows));
    limit_normal_changes(tu, tv, dtu, dtv, std::span<Vec3>(change.data(), rows));
    bench::keep(change[rows - 1]);
  }
  state.set_items(rows);
}

// The same for the area-weighted mode: every refined triangle's cross-product change and the
// state's own sums (for the degeneracy test), then each vertex's normal change, from the surface
// displacement geometry.limit.apply.fixture produces.
ENGINE_BENCH(geometry_binding_field_area, "geometry.binding.field.area") {
  Operator& op = fixture_operator();
  const BindingFrame& f =
      fixture_frames().mode[static_cast<u32>(NormalMode::interpolated_vertex_area_weighted)];
  const u32 rows = op.surface.limit.rows();
  Vector<Vec3> vectors(rows);
  area_weighted_normal_vectors(op.surface.faces, op.reference,
                               std::span<Vec3>(vectors.data(), rows));
  Vector<Vec3> vector_change(rows);
  Vector<Vec3> state_positions(rows);
  Vector<Vec3> state_vectors(rows);
  Vector<Vec3> change(rows);
  while (state.keep_running()) {
    area_weighted_normal_vector_change(op.surface.faces, op.reference, f.displacement,
                                       std::span<Vec3>(vector_change.data(), rows));
    for (u32 i = 0; i < rows; ++i)
      state_positions[i] = op.reference[i] + f.displacement[i];
    area_weighted_normal_vectors(op.surface.faces, state_positions,
                                 std::span<Vec3>(state_vectors.data(), rows));
    area_weighted_normal_changes(vectors, vector_change, state_vectors,
                                 std::span<Vec3>(change.data(), rows));
    bench::keep(change[rows - 1]);
  }
  state.set_items(rows);
}

// The canonical ids a mesh without authored ones is named by (`position_weld_ids`): a content-build
// cost, paid once per import by both builders, measured on n vertices at n / 4 distinct positions
// in a scrambled order — every point written four times, the way an exporter that splits at every
// face writes a quad mesh — so the sort does real work and the runs are real runs.
ENGINE_BENCH_ARGS(geometry_vertex_ids_position_weld, "geometry.vertex_ids.position_weld", 65536,
                  1048576) {
  const u32 count = static_cast<u32>(state.arg());
  const u32 side = static_cast<u32>(std::sqrt(static_cast<f64>(count / 4)));
  Vector<Vec3> positions(count);
  u64 lcg = 0x9e3779b97f4a7c15ull;
  for (u32 i = 0; i < count; ++i) {
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    const u32 point = static_cast<u32>(lcg >> 33) % (side * side);
    positions[i] =
        Vec3{static_cast<f32>(point % side) * 0.01f, 0.0f, static_cast<f32>(point / side) * 0.01f};
  }
  Vector<u32> ids;
  while (state.keep_running()) {
    bench::keep(position_weld_ids(positions, ids));
  }
  state.set_items(count);
}

ENGINE_BENCH(geometry_limit_build_fixture, "geometry.limit.build.fixture") {
  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  LoopSurfaceOptions options;
  options.level = 3;
  options.tangents = false;
  LoopLimitSurface surface;
  while (state.keep_running()) {
    build_loop_limit_surface(faces, 121, options, surface);
    bench::keep(surface.limit.weight[0]);
  }
  state.set_items(1);
}
