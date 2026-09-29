// A soft body from a tissue region (domain/tissue/soft_body.h; docs/subsystems/tissue.md, "A soft
// body from a runtime region").

#include "body.h"
#include "mesh_query.h"
#include "model.h"

#include <core/time/time.h>
#include <domain/physics/physics.h>
#include <domain/tissue/soft_body.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace engine::tissue {

namespace {

using detail::Body;
using detail::BodyState;
using query::D3;
using query::d3;

// How far a contact surface is set back behind itself (below): 10 um, a thousandth of what the
// settle measures and a thousand times an f32 position's spacing at the fixtures' size.
constexpr f64 k_surface_setback_m = 1.0e-5;

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

std::string str(u64 v) { return std::to_string(v); }

// A frame's rigid transform at a state (its FrameState there, or the identity).
detail::RigidTransform frame_at(const TissueDefinition& d, const std::string& frame,
                                const std::string& state) {
  return detail::rigid_transform(detail::frame_state(d, frame, state));
}

Vec3 to_vec3(D3 p) {
  return Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
}

template <class T>
bool read_block(const TissueFile& file, const std::string& name, BlockKind kind, Vector<T>& out,
                u32 per_element = 1) {
  const TissueBlock* b = file.find(name);
  if (b == nullptr || b->kind != kind) return false;
  out.resize(static_cast<u32>(b->count * per_element));
  if (out.size() * sizeof(T) != b->bytes.size()) return false;
  if (!b->bytes.empty()) std::memcpy(out.data(), b->bytes.data(), b->bytes.size());
  return true;
}

}  // namespace

bool build_soft_body(const TissueFile& file, const SoftBodyOptions& options, TissueSoftBody& out,
                     std::string* error) {
  out = TissueSoftBody{};
  const TissueDefinition& d = file.definition;
  std::string region_name = options.region;
  if (region_name.empty()) {
    u32 found = 0;
    for (const Region& r : d.regions)
      if (r.role == RegionRole::Runtime && r.cage == CageKind::Tetrahedral) {
        region_name = r.name;
        ++found;
      }
    if (found != 1)
      return fail(error, found == 0
                             ? "the file has no Runtime region of four-node cells: derive a "
                               "cage from a reference body first (engine-content tissue cage)"
                             : "the file has " + str(found) + " Runtime regions: name one");
  }
  Body body;
  if (!detail::load_body(file, region_name, body, error)) return false;
  const Region& region = *body.region;
  if (body.quadratic())
    return fail(error, "region " + region.name +
                           " is ten-node cells: a soft body is built from a four-node Runtime "
                           "cage (engine-content tissue cage derives one)");
  // The rest shape and the load.
  const BodyState* rest = nullptr;
  if (!options.rest_state.empty()) {
    rest = body.state(options.rest_state);
    if (rest == nullptr) return fail(error, "no state '" + options.rest_state + "'");
  } else {
    rest = body.state_with_role(StateRole::Rest);
    if (rest == nullptr) rest = &body.states.front();
  }
  const BodyState* load = nullptr;
  if (!options.load_state.empty()) {
    load = body.state(options.load_state);
    if (load == nullptr || load->def == nullptr)
      return fail(error, "no state '" + options.load_state + "' with a load");
  }
  out.region = region.name;
  out.hero = region.hero;
  out.rest_state = rest->name;
  out.load_state = load != nullptr ? load->name : std::string();
  out.rest = rest->nodes;
  const u32 n = body.nodes.size();
  const u32 cells = body.cell_count();

  // ---- mass: a quarter of each cell's density times volume at the construction to its corners.
  Vector<f64> mass(n, 0.0);
  for (u32 c = 0; c < cells; ++c) {
    const f64 v = body.cell_volume(c, body.nodes);
    if (!(v > 0.0)) return fail(error, "cell " + str(c) + " is not positive at the construction");
    out.volume_m3 += v;
    out.mass_kg += v * body.density[c];
    for (u32 k = 0; k < 4; ++k)
      mass[body.cell(c)[k]] += v * body.density[c] / 4.0;
  }
  out.inverse_masses.resize(n);
  for (u32 i = 0; i < n; ++i) {
    if (!(mass[i] > 0.0)) return fail(error, "node " + str(i) + " is in no cell");
    out.inverse_masses[i] = static_cast<f32>(1.0 / mass[i]);
  }

  // ---- the declared energy as XPBD constraints, at the rest.
  struct EdgeSum {
    u64 key;
    f64 weight;
  };
  Vector<EdgeSum> edge_sums;
  static const u32 pairs[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
  for (u32 c = 0; c < cells; ++c) {
    const u32* v = body.cell(c);
    const f64 v0 = query::tet_volume(d3(rest->nodes[v[0]]), d3(rest->nodes[v[1]]),
                                     d3(rest->nodes[v[2]]), d3(rest->nodes[v[3]]));
    if (!(v0 > 0.0))
      return fail(error, "cell " + str(c) + " is not positive at the rest (" + rest->name + ")");
    const f64 mu = body.shear_pa[c];
    const f64 volumetric = body.bulk_pa[c] - 5.0 * mu / 3.0;
    if (!(volumetric > 0.0) || !(mu > 0.0))
      return fail(error, "cell " + str(c) +
                             "'s material has K < 5 mu / 3 or no shear modulus, which bulk-edge-v0 "
                             "cannot represent");
    physics::SoftVolumeConstraint volume;
    for (u32 k = 0; k < 4; ++k)
      volume.vertex[k] = v[k];
    volume.compliance = static_cast<f32>(36.0 * v0 / volumetric);
    out.volumes.push_back(volume);
    for (const auto& p : pairs) {
      const u32 a = std::min(v[p[0]], v[p[1]]);
      const u32 b = std::max(v[p[0]], v[p[1]]);
      edge_sums.push_back(EdgeSum{(static_cast<u64>(a) << 32) | b, 1.25 * mu * v0});
    }
  }
  std::sort(edge_sums.begin(), edge_sums.end(),
            [](const EdgeSum& x, const EdgeSum& y) { return x.key < y.key; });
  for (u32 i = 0; i < edge_sums.size();) {
    f64 weight = 0.0;
    u32 j = i;
    while (j < edge_sums.size() && edge_sums[j].key == edge_sums[i].key)
      weight += edge_sums[j++].weight;
    const u32 a = static_cast<u32>(edge_sums[i].key >> 32);
    const u32 b = static_cast<u32>(edge_sums[i].key & 0xffffffffu);
    const f64 l0 = query::length(d3(rest->nodes[a]) - d3(rest->nodes[b]));
    out.edges.push_back(physics::SoftEdge{a, b, static_cast<f32>(l0 * l0 / (2.0 * weight))});
    i = j;
  }
  out.faces = detail::boundary_faces(body.tets, body.nodes);

  // ---- the load: gravity in the body's coordinates, less the medium's buoyancy.
  if (load != nullptr) {
    const Load& l = load->def->load;
    const f64 density = out.volume_m3 > 0.0 ? out.mass_kg / out.volume_m3 : 0.0;
    const f64 factor = density > 0.0 ? 1.0 - l.medium_density_kg_m3 / density : 1.0;
    out.gravity = to_vec3(d3(l.gravity) * factor);
  }

  // ---- the targets: every frame (contact, and a fixed attachment's anchor), and each surface an
  // attachment names; where the load state puts them.
  const std::string at_state = load != nullptr ? load->name : rest->name;
  for (const Frame& f : d.frames) {
    SoftBodyTarget t;
    t.name = f.name;
    t.frame = true;
    t.contact = true;
    Vector<Vec3> vertices;
    if (!read_block(file, f.vertices, BlockKind::FrameVertices, vertices) ||
        !read_block(file, f.triangles, BlockKind::FrameTriangles, t.triangles, 3))
      return fail(error, "frame " + f.name + " does not resolve");
    const detail::RigidTransform at = frame_at(d, f.name, at_state);
    for (const Vec3& v : vertices)
      t.vertices.push_back(to_vec3(at.apply(d3(v))));
    out.targets.push_back(std::move(t));
  }
  const auto target_of = [&](TargetKind kind, const std::string& name) -> i32 {
    for (u32 i = 0; i < out.targets.size(); ++i)
      if (out.targets[i].name == name && out.targets[i].frame == (kind == TargetKind::Frame))
        return static_cast<i32>(i);
    if (kind != TargetKind::Surface) return -2;
    for (const Surface& s : d.surfaces) {
      if (s.name != name) continue;
      SoftBodyTarget t;
      t.name = s.name;
      t.frame = false;
      if (!read_block(file, s.vertices, BlockKind::SurfaceVertices, t.vertices) ||
          !read_block(file, s.triangles, BlockKind::SurfaceTriangles, t.triangles, 3))
        return -2;
      // The backend's contact is one-sided — a node within 10 cm behind its nearest triangle is
      // projected onto it — so the region has to be in front. Which side that is, is the side the
      // region's nodes are on, summed over all of them: one point's side of an open, curved surface
      // is only as good as its nearest feature, and the region's centroid alone gave the supine
      // fixtures' support the wrong side (135 of 256 nodes up to 38 mm behind it).
      query::SignedSurface signed_surface;
      signed_surface.build(t.vertices, t.triangles);
      f64 side = 0.0;
      for (const Vec3& p : body.nodes)
        side += signed_surface.query(d3(p)).distance;
      if (side < 0.0)
        for (u32 k = 0; k + 2 < t.triangles.size(); k += 3)
          std::swap(t.triangles[k + 1], t.triangles[k + 2]);
      // And set back behind itself by k_surface_setback: a node lying exactly on a vertex or an
      // edge of the surface — a support built from the region's own nodes, as the fixtures' are —
      // finds its nearest feature at zero distance, and the backend drops a contact whose normal
      // is the zero vector, so such a node would fall through. Behind by a hair, its nearest point
      // is a hair away along the surface's normal and the contact holds.
      Vector<D3> normal(t.vertices.size(), D3{});
      for (u32 k = 0; k + 2 < t.triangles.size(); k += 3) {
        const u32* f = t.triangles.data() + k;
        const D3 area = query::cross(d3(t.vertices[f[1]]) - d3(t.vertices[f[0]]),
                                     d3(t.vertices[f[2]]) - d3(t.vertices[f[0]]));
        for (u32 j = 0; j < 3; ++j)
          normal[f[j]] = normal[f[j]] + area;
      }
      for (u32 v = 0; v < t.vertices.size(); ++v)
        t.vertices[v] = to_vec3(d3(t.vertices[v]) - query::unit(normal[v]) * k_surface_setback_m);
      out.targets.push_back(std::move(t));
      return static_cast<i32>(out.targets.size() - 1);
    }
    return -2;
  };

  // ---- the attachments, as the constraints they declare.
  Vector<u8> held(n, 0);
  for (const Attachment& a : d.attachments) {
    if (a.region != region.name) continue;
    Vector<u32> nodes;
    if (!detail::read_node_set(file, region, a.nodes, nodes))
      return fail(error, "attachment " + a.name + " names node set '" + a.nodes + "'");
    if (a.target_kind == TargetKind::Bone || a.target_kind == TargetKind::RegionSheet)
      return fail(error, "attachment " + a.name + " is to a " +
                             (a.target_kind == TargetKind::Bone ? "bone" : "region sheet") +
                             ", which the soft body cannot follow yet");
    i32 target = -1;
    if (a.target_kind != TargetKind::World) {
      target = target_of(a.target_kind, a.target);
      if (target < 0)
        return fail(error, "attachment " + a.name + " names target '" + a.target + "'");
    }
    if (a.kind != AttachmentKind::Fixed) {
      // Sliding and unilateral: contact with the target. The world has no surface to slide on.
      if (target < 0)
        return fail(error, "attachment " + a.name + " slides on the world, which has no surface");
      out.targets[static_cast<u32>(target)].contact = true;
      continue;
    }
    // Held where it rests, carried by its target's motion from the rest to the load state: the
    // frame binding's material map, T_load T_rest^-1 x_rest (the world, or a frame with no frame
    // states, holds it where it rests). An essential attachment's nodes rest where the frame puts
    // their construction positions, so for one this is the definition's own rule.
    detail::RigidTransform at_rest;
    detail::RigidTransform now;
    if (target >= 0 && out.targets[static_cast<u32>(target)].frame) {
      at_rest = frame_at(d, a.target, rest->name);
      now = frame_at(d, a.target, at_state);
    }
    for (const u32 v : nodes) {
      if (v >= n) return fail(error, "attachment " + a.name + " names node " + str(v));
      if (held[v]) continue;
      held[v] = 1;
      TissueSoftBody::Anchor anchor;
      anchor.vertex = v;
      anchor.target = target;
      anchor.local = to_vec3(now.apply(at_rest.unapply(d3(rest->nodes[v]))));
      // Rigid, a spring's stiffest form: the physics module's `Spring` steers a particle's
      // velocity once a step and keeps its mass, so under gravity a held node ends every step
      // g dt^2 / 2 past its anchor (1.4 mm at 60 Hz) — the servo's, not a spring's — and no
      // conversion from `stiffness_pa` to a follow rate exists yet.
      anchor.kind = physics::AttachmentKind::Rigid;
      out.anchors.push_back(anchor);
    }
    if (a.enforcement == AttachmentEnforcement::Spring)
      out.notes.push_back("attachment " + a.name +
                          " (a spring) is held rigid: its stiffness is not converted to a follow "
                          "rate, and the step-rate servo sags a held node by g dt^2 / 2");
  }
  if (!region.membranes.empty())
    out.notes.push_back(str(region.membranes.size()) + " membrane sets are not read");
  if (!region.cables.empty())
    out.notes.push_back(str(region.cables.size()) + " cable sets are not read");
  return true;
}

bool settle_soft_body(const TissueFile& file, const SoftBodyOptions& options, SettleResult& out,
                      std::string* error) {
  out = SettleResult{};
  if (options.load_state.empty()) return fail(error, "a settle needs the state whose load it is");
  TissueSoftBody body;
  if (!build_soft_body(file, options, body, error)) return false;
  physics::WorldOptions world_options;
  world_options.gravity = body.gravity;
  world_options.step_hz = options.step_hz;
  world_options.deterministic = true;
  world_options.job_system = nullptr;  // inline, on this thread
  world_options.max_bodies = 64;
  physics::World world;
  physics::Status status = world.init(world_options);
  if (status != physics::Status::Ok)
    return fail(error, std::string("the physics world refused: ") + physics::status_name(status));
  // The targets, static triangle meshes where the load state puts them.
  Vector<physics::BodyId> bodies;
  for (const SoftBodyTarget& t : body.targets) {
    physics::ShapeId shape;
    status = world.create_mesh(t.vertices, t.triangles, shape);
    if (status != physics::Status::Ok)
      return fail(error, "target " + t.name + ": " + physics::status_name(status));
    physics::BodyDesc desc;
    desc.shape = shape;
    desc.motion = physics::MotionType::Static;
    desc.layer = t.contact ? physics::Layer::Static : physics::Layer::Query;
    physics::BodyId id;
    status = world.create_body(desc, id);
    if (status != physics::Status::Ok)
      return fail(error, "target " + t.name + ": " + physics::status_name(status));
    bodies.push_back(id);
  }
  Vector<physics::SoftAttachment> attachments;
  for (const TissueSoftBody::Anchor& a : body.anchors) {
    physics::SoftAttachment s;
    s.vertex = a.vertex;
    s.body = a.target >= 0 ? bodies[static_cast<u32>(a.target)] : physics::BodyId{};
    s.local_point = a.local;
    s.kind = a.kind;
    s.follow_rate = static_cast<f32>(options.step_hz);  // read by a Spring only
    attachments.push_back(s);
  }
  physics::SoftBodyDesc desc;
  desc.vertices = body.rest;
  desc.inverse_masses = body.inverse_masses;
  desc.edges = body.edges;
  desc.volumes = body.volumes;
  desc.attachments = attachments;
  desc.faces = body.faces;
  desc.linear_damping = options.linear_damping;
  desc.friction = options.friction;
  desc.restitution = 0.0f;
  desc.vertex_radius = 0.0f;
  desc.max_strain = options.max_strain;
  desc.iterations = options.iterations;
  desc.allow_sleeping = false;
  desc.hero = body.hero;
  physics::SoftBodyId soft;
  status = world.create_soft_body(desc, soft);
  if (status != physics::Status::Ok)
    return fail(error, std::string("the soft body was refused: ") + physics::status_name(status));

  const u32 n = body.rest.size();
  Vector<Vec3> previous = body.rest;
  Vector<Vec3> now(n);
  const f32 dt = 1.0f / static_cast<f32>(options.step_hz);
  u32 calm = 0;
  Vector<f64> speed(n, 0.0);
  const u32 quantile_index = static_cast<u32>(
      std::floor(static_cast<f64>(options.settle_quantile) * static_cast<f64>(n - 1)));
  const f64 limit = static_cast<f64>(options.settle_speed_m_s);
  const i64 start = time::monotonic_ns();
  while (out.steps < options.max_steps) {
    status = world.step(dt, options.sub_steps);
    if (status != physics::Status::Ok)
      return fail(error, std::string("a step failed: ") + physics::status_name(status));
    ++out.steps;
    out.strain_clamp_sweeps_max =
        std::max(out.strain_clamp_sweeps_max, world.stats().soft_body_budget.strain_clamp_sweeps);
    world.read_soft_body_vertices(soft, now);
    f64 fastest = 0.0;
    out.still_moving = 0;
    for (u32 i = 0; i < n; ++i) {
      speed[i] = query::length(d3(now[i]) - d3(previous[i])) / static_cast<f64>(dt);
      if (speed[i] > fastest) {
        fastest = speed[i];
        out.fastest_node = i;
      }
      out.still_moving += speed[i] >= limit ? 1u : 0u;
    }
    out.final_speed_m_s = fastest;
    Vector<f64> sorted = speed;
    std::nth_element(sorted.begin(), sorted.begin() + quantile_index, sorted.end());
    const f64 quantile = sorted[quantile_index];
    out.final_quantile_speed_m_s = quantile;
    if (out.steps % options.step_hz == 0) out.speed_trace_m_s.push_back(static_cast<f32>(quantile));
    previous = now;
    if (!std::isfinite(fastest)) break;
    calm = quantile < limit ? calm + 1 : 0;
    if (calm >= options.settle_steps) {
      out.settled = true;
      break;
    }
  }
  out.wall_ms = static_cast<f64>(time::monotonic_ns() - start) / 1.0e6;
  out.simulated_s = static_cast<f64>(out.steps) / static_cast<f64>(options.step_hz);
  out.nodes = now;
  // Against the load state's nodes.
  Body cage;
  if (!detail::load_body(file, body.region, cage, error)) return false;
  const BodyState* target = cage.state(options.load_state);
  out.distance_m.resize(n);
  for (u32 i = 0; i < n; ++i)
    out.distance_m[i] = query::length(d3(now[i]) - d3(target->nodes[i]));
  for (u32 c = 0; c < cage.cell_count(); ++c)
    if (!(cage.cell_volume(c, now) > 0.0)) ++out.inverted;
  return true;
}

JsonValue settle_json(const TissueFile& file, const SoftBodyOptions& options,
                      const SettleResult& result) {
  TissueSoftBody body;
  std::string error;
  build_soft_body(file, options, body, &error);
  JsonValue out = JsonValue::object();
  out.set("region", JsonValue(body.region));
  out.set("rest_state", JsonValue(body.rest_state));
  out.set("load_state", JsonValue(body.load_state));
  JsonValue soft = JsonValue::object();
  soft.set("nodes", JsonValue(static_cast<u64>(body.rest.size())));
  soft.set("cells", JsonValue(static_cast<u64>(body.volumes.size())));
  soft.set("edges", JsonValue(static_cast<u64>(body.edges.size())));
  soft.set("anchors", JsonValue(static_cast<u64>(body.anchors.size())));
  soft.set("mass_kg", detail::number(body.mass_kg));
  JsonValue gravity = JsonValue::array();
  for (const f32 g : {body.gravity.x, body.gravity.y, body.gravity.z})
    gravity.push_back(JsonValue(static_cast<f64>(g)));
  soft.set("gravity", std::move(gravity));
  JsonValue targets = JsonValue::array();
  for (const SoftBodyTarget& t : body.targets) {
    JsonValue e = JsonValue::object();
    e.set("name", JsonValue(t.name));
    e.set("kind", JsonValue(t.frame ? "frame" : "surface"));
    e.set("contact", JsonValue(t.contact));
    e.set("triangles", JsonValue(static_cast<u64>(t.triangles.size() / 3)));
    targets.push_back(std::move(e));
  }
  soft.set("targets", std::move(targets));
  JsonValue notes = JsonValue::array();
  for (const std::string& note : body.notes)
    notes.push_back(JsonValue(note));
  soft.set("not_read", std::move(notes));
  out.set("body", std::move(soft));
  JsonValue solver = JsonValue::object();
  solver.set("backend", JsonValue("domain/physics soft body (Jolt XPBD), single-threaded"));
  solver.set("law", JsonValue("bulk-edge-v0"));
  solver.set("step_hz", JsonValue(options.step_hz));
  solver.set("iterations", JsonValue(options.iterations));
  solver.set("sub_steps", JsonValue(options.sub_steps));
  solver.set("linear_damping_per_s", JsonValue(static_cast<f64>(options.linear_damping)));
  solver.set("max_strain", JsonValue(static_cast<f64>(options.max_strain)));
  solver.set("friction", JsonValue(static_cast<f64>(options.friction)));
  solver.set("settle_speed_m_s", JsonValue(static_cast<f64>(options.settle_speed_m_s)));
  solver.set("settle_quantile", JsonValue(static_cast<f64>(options.settle_quantile)));
  solver.set("settle_steps", JsonValue(options.settle_steps));
  solver.set("max_steps", JsonValue(options.max_steps));
  out.set("solver", std::move(solver));
  out.set("settled", JsonValue(result.settled));
  out.set("steps", JsonValue(result.steps));
  out.set("simulated_s", detail::number(result.simulated_s));
  out.set("wall_ms", detail::number(result.wall_ms));
  out.set("step_us",
          detail::number(result.steps > 0 ? result.wall_ms * 1.0e3 / static_cast<f64>(result.steps)
                                          : 0.0));
  out.set("final_quantile_speed_m_s", detail::number(result.final_quantile_speed_m_s));
  out.set("final_speed_m_s", detail::number(result.final_speed_m_s));
  out.set("fastest_node", JsonValue(result.fastest_node));
  out.set("still_moving", JsonValue(result.still_moving));
  JsonValue trace = JsonValue::array();
  for (const f32 v : result.speed_trace_m_s)
    trace.push_back(JsonValue(static_cast<f64>(v)));
  out.set("quantile_speed_each_second_m_s", std::move(trace));
  out.set("inverted_cells", JsonValue(result.inverted));
  out.set("strain_clamp_sweeps_max", JsonValue(result.strain_clamp_sweeps_max));
  out.set("distance_mm", detail::stats(result.distance_m, detail::k_mm));
  // Where the largest distance is, and what the node is.
  Body cage;
  if (!result.distance_m.empty() && detail::load_body(file, body.region, cage, &error)) {
    u32 worst = 0;
    for (u32 i = 1; i < result.distance_m.size(); ++i)
      if (result.distance_m[i] > result.distance_m[worst]) worst = i;
    JsonValue where = JsonValue::object();
    where.set("node", JsonValue(worst));
    JsonValue sets = JsonValue::array();
    for (const NodeSetRef& s : cage.region->node_sets) {
      Vector<u32> nodes;
      if (detail::read_node_set(file, *cage.region, s.name, nodes) &&
          std::find(nodes.begin(), nodes.end(), worst) != nodes.end())
        sets.push_back(JsonValue(s.name));
    }
    where.set("node_sets", std::move(sets));
    const Vector<u32> faces = detail::boundary_faces(cage.tets, cage.nodes);
    where.set("boundary", JsonValue(std::find(faces.begin(), faces.end(), worst) != faces.end()));
    const BodyState* rest = cage.state(body.rest_state);
    const BodyState* load = cage.state(body.load_state);
    if (rest != nullptr && load != nullptr) {
      where.set("load_state_displacement_mm",
                detail::number(query::length(d3(load->nodes[worst]) - d3(rest->nodes[worst])) *
                               detail::k_mm));
      where.set("settled_displacement_mm",
                detail::number(query::length(d3(result.nodes[worst]) - d3(rest->nodes[worst])) *
                               detail::k_mm));
      Vector<f64> load_move;
      Vector<f64> settled_move;
      for (u32 i = 0; i < rest->nodes.size(); ++i) {
        load_move.push_back(query::length(d3(load->nodes[i]) - d3(rest->nodes[i])));
        settled_move.push_back(query::length(d3(result.nodes[i]) - d3(rest->nodes[i])));
      }
      out.set("load_state_displacement_mm", detail::stats(load_move, detail::k_mm));
      out.set("settled_displacement_mm", detail::stats(settled_move, detail::k_mm));
    }
    out.set("largest", std::move(where));
  }
  return out;
}

}  // namespace engine::tissue
