// The flora stage's CPU half (flora_motion.h; renderer.md, "The flora stage").
#include <domain/geometry/flora_species.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/flora_motion.h>

#include <cmath>

namespace engine::renderer {
namespace {

tunables::Bool flora_motion{"renderer.flora.motion", true,
                            "Sway the generated trees in the wind and bend them away from the "
                            "walker (renderer.md, \"The flora stage\"); false draws them rigid"};
tunables::Float flora_wind_strength{
    "renderer.flora.wind_strength", 1.0, 0.0, 2.0,
    "The wind's strength over its record's mean for the trees of a scene whose ground says no wind "
    "(0 calm, 1 a mean day)"};
tunables::Float flora_wind_deg{"renderer.flora.wind_deg", 0.0, -360.0, 360.0,
                               "Where that wind blows to: degrees in the ground plane from +x "
                               "towards +z"};
// **The streamed trees** (renderer.md, "Streamed trees sway"; roadmap W74). A tree a streamed world
// hands over with its tile sways as a loaded one does; `tail` false draws the tail rigid, as before
// W74, for a before-and-after. `sway_m` is the farthest ring floor whose distance form still sways;
// 0, the default, sways the inner ring's full meshes alone. The second ring's sway is visible (at
// 48 m a 0.24 m frond tip is five pixels at 900 lines), but in the v8 forest that ring holds 5,122
// swaying plants at the hardwoods view against the first ring's 261, and took the frame from 8.2
// to 9.3 ms of GPU (renderer.md, "Streamed trees sway", the costs): an operator's choice, not a
// default. At 100 m a 0.1 m flutter is about a pixel, so past that no ring should sway at all.
tunables::Bool flora_tail{"renderer.flora.tail", true,
                          "Sway the trees a streamed world hands over with their tiles, as the "
                          "loaded ones do; false draws the tail rigid (renderer.md, \"Streamed "
                          "trees sway\")"};
tunables::Float flora_sway_m{"renderer.flora.sway_m", 0.0, 0.0, 100'000.0,
                             "Metres: a ring's distance form of a tree sways when the ring's tiles "
                             "come nearer the camera than this, and is drawn rigid past it"};

// **The trees' shadows stop swaying past a distance** (renderer.md, "The caster policy"; R95):
// a cascade's texel past a few tens of metres is larger than a twig's travel, so a tree there can
// cast its rest pose and spare the pool pass its cascade blocks. Off by default: the measured
// option at the v8 hardwoods view is in renderer.md.
tunables::Float flora_rest_pose_cascades_m{
    "renderer.flora.rest_pose_cascades_m", 0.0, 0.0, 100'000.0,
    "Metres: a swaying instance farther than this from the eye casts its rest pose into the "
    "cascaded shadow maps (0: off; grass casts at rest by its species' policy whatever this says)"};

constexpr f32 k_tau = 6.28318530717958647692f;

// A 32-bit integer hash (the "lowbias32" finalizer): the seed's phases, nothing that needs more.
u32 hash32(u32 x) noexcept {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
f32 unit_of(u32 h) noexcept { return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); }

struct Frame {
  Vec3 r[3];  // rows of the rotation
  Vec3 t;
};

Frame frame_of(const gfx::FloraPartPose& pose) noexcept {
  Frame f;
  for (u32 i = 0; i < 3; ++i) {
    f.r[i] = pose.rows[i].xyz();
  }
  f.t = Vec3{pose.rows[0].w, pose.rows[1].w, pose.rows[2].w};
  return f;
}

// Rodrigues as a matrix, rows: c I + s [k]x + (1 - c) k k^T.
void rotation_rows(Vec3 k, f32 angle, Vec3 (&r)[3]) noexcept {
  const f32 c = std::cos(angle);
  const f32 s = std::sin(angle);
  const f32 t = 1.0f - c;
  r[0] = Vec3{c + t * k.x * k.x, t * k.x * k.y - s * k.z, t * k.x * k.z + s * k.y};
  r[1] = Vec3{t * k.y * k.x + s * k.z, c + t * k.y * k.y, t * k.y * k.z - s * k.x};
  r[2] = Vec3{t * k.z * k.x - s * k.y, t * k.z * k.y + s * k.x, c + t * k.z * k.z};
}

Vec3 row_times(const Vec3 (&r)[3], Vec3 v) noexcept {
  return Vec3{dot(r[0], v), dot(r[1], v), dot(r[2], v)};
}

f32 clamp01(f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The closest points of segments [p0, p1] and [q0, q1]: the parameters along each.
void closest_on_segments(Vec3 p0, Vec3 p1, Vec3 q0, Vec3 q1, f32& s, f32& t) noexcept {
  const Vec3 d1 = p1 - p0;
  const Vec3 d2 = q1 - q0;
  const Vec3 r = p0 - q0;
  const f32 a = dot(d1, d1);
  const f32 e = dot(d2, d2);
  const f32 f = dot(d2, r);
  if (a <= 1.0e-12f && e <= 1.0e-12f) {
    s = t = 0.0f;
    return;
  }
  if (a <= 1.0e-12f) {
    s = 0.0f;
    t = clamp01(f / e);
    return;
  }
  const f32 c = dot(d1, r);
  if (e <= 1.0e-12f) {
    t = 0.0f;
    s = clamp01(-c / a);
    return;
  }
  const f32 b = dot(d1, d2);
  const f32 denom = a * e - b * b;
  s = denom > 1.0e-12f ? clamp01((b * f - c * e) / denom) : 0.0f;
  t = (b * s + f) / e;
  if (t < 0.0f) {
    t = 0.0f;
    s = clamp01(-c / a);
  } else if (t > 1.0f) {
    t = 1.0f;
    s = clamp01((b - c) / a);
  }
}

}  // namespace

bool flora_motion_tunable() noexcept { return flora_motion.get(); }
bool flora_tail_tunable() noexcept { return flora_tail.get(); }
f32 flora_sway_m_tunable() noexcept { return static_cast<f32>(flora_sway_m.get()); }
f32 flora_rest_pose_cascades_m_tunable() noexcept {
  return static_cast<f32>(flora_rest_pose_cascades_m.get());
}
FloraWindDefault flora_wind_default() noexcept {
  const f64 a = flora_wind_deg.get() * (3.14159265358979323846 / 180.0);
  return FloraWindDefault{Vec2{static_cast<f32>(std::cos(a)), static_cast<f32>(std::sin(a))},
                          static_cast<f32>(flora_wind_strength.get())};
}

FloraStiffness flora_stiffness(std::string_view species) noexcept {
  const auto* info = geometry::flora_species_info(species);
  if (info == nullptr) return FloraStiffness{0, 0, 0, 0};
  return FloraStiffness{info->compliance[0], info->compliance[1], info->compliance[2],
                        info->compliance[3]};
}

bool flora_part_collides(std::span<const geometry::ClusterFileStructuralPart> structure,
                         u32 part) noexcept {
  if (part == 0) return true;
  if (structure.empty() || part >= structure.size()) return false;
  // A load-bearing limb, or a part flagged a collider: a shrub's stem stout enough to stop a
  // walker (flora.md, "The shrub").
  return (structure[part].flags &
          (geometry::k_structural_load_bearing | geometry::k_structural_collider)) != 0;
}

bool flora_mesh(std::span<const geometry::ClusterFileMotionPart> parts,
                std::span<const geometry::ClusterFileStructuralPart> structure,
                std::string_view species, FloraMesh& out, std::span<const u32> ids) {
  out = FloraMesh{};
  if (parts.empty() || parts.size() > 65536u) return false;
  out.phase_id.resize(static_cast<u32>(parts.size()));
  for (u32 p = 0; p < parts.size(); ++p)
    out.phase_id[p] = ids.size() == parts.size() ? ids[p] : p;
  if (geometry::flora_species_info(species) == nullptr) return false;
  const FloraStiffness stiffness = flora_stiffness(species);
  const geometry::FloraSpeciesInfo& info = *geometry::flora_species_info(species);
  if (info.motion == geometry::FloraMotionKind::none) return false;  // it never moves
  out.field = info.motion == geometry::FloraMotionKind::field;
  out.field_compliance = stiffness.leaf;
  const bool structured = structure.size() == parts.size();
  const u32 n = static_cast<u32>(parts.size());
  out.parts.resize(n);
  out.compliance.resize(n, 0.0f);
  out.attach.resize(n, 0.0f);
  out.reach.resize(n, 0.0f);
  out.pushable.resize(n, u8{0});
  Vector<u32> depth(n, 0u);
  for (u32 p = 0; p < n; ++p) {
    const geometry::ClusterFileMotionPart& part = parts[p];
    out.parts[p] = part;
    const bool root = part.parent == geometry::k_motion_no_parent;
    if (!root && part.parent >= p) return false;  // parents first, or the frames cannot compose
    depth[p] = root ? 0u : depth[part.parent] + 1u;
    const bool load_bearing =
        structured && (structure[p].flags & geometry::k_structural_load_bearing) != 0;
    const bool wooden = !structured || structure[p].base_radius > 0.0f;
    f32 c = depth[p] == 0 ? stiffness.trunk : depth[p] == 1 ? stiffness.limb : stiffness.twig;
    if (!wooden) c = stiffness.leaf;
    if (load_bearing) c *= k_flora_load_bearing;
    out.compliance[p] = c;
    if (!root) {
      const geometry::ClusterFileMotionPart& parent = parts[part.parent];
      out.attach[p] =
          parent.length > 0.0f
              ? clamp01(dot(part.pivot - parent.pivot, parent.direction) / parent.length)
              : 0.0f;
    }
    const f32 wood =
        structured ? std::fmax(structure[p].base_radius, structure[p].tip_radius) : 0.0f;
    out.reach[p] = wood > 0.0f ? wood : k_flora_leaf_reach * part.length;
    // A part that collides is never pushed: the walker stops at it rather than bending it.
    const bool collider = structured && (structure[p].flags & geometry::k_structural_collider) != 0;
    // Sward's contract is one parentless non-woody part per blade, including part zero.
    const bool blade = species == "sward" && structured && !wooden;
    out.pushable[p] = ((blade || (!root && p != 0)) && !load_bearing && !collider) ? u8{1} : u8{0};
  }
  return true;
}

void flora_field_frame(Vec2 to, f32 strength, f64 time_s, const WorldEye& eye,
                       const FrameFlora& frame, gfx::FloraFieldFrame& out) noexcept {
  out = gfx::FloraFieldFrame{};
  out.eye = gfx::frame_eye(eye);
  const f32 s =
      strength < 0.0f ? 0.0f : (strength > k_flora_max_wind ? k_flora_max_wind : strength);
  const f64 tau = 6.28318530717958647692;
  const auto reduce = [tau](f64 phase) {
    const f64 r = std::fmod(phase, tau);
    return static_cast<f32>(r < 0.0 ? r + tau : r);
  };
  out.wind =
      Vec4{to.x, to.y, s, reduce(time_s * (tau * static_cast<f64>(k_flora_field_flutter_hz)))};
  // The eye in the world, in double: the cell's corner plus its offset and what rounding left.
  const f64 ex = static_cast<f64>(eye.cell.x) * k_world_cell_m + static_cast<f64>(eye.local.x) +
                 static_cast<f64>(eye.residual.x);
  const f64 ez = static_cast<f64>(eye.cell.z) * k_world_cell_m + static_cast<f64>(eye.local.z) +
                 static_cast<f64>(eye.residual.z);
  const f64 base = std::atan2(static_cast<f64>(to.y), static_cast<f64>(to.x));
  const f64 lengths[2] = {static_cast<f64>(k_flora_field_honami_m),
                          static_cast<f64>(k_flora_field_sweep_m)};
  const f64 speeds[2] = {static_cast<f64>(k_flora_field_honami_mps),
                         static_cast<f64>(k_flora_field_sweep_mps)};
  f32 k[8];
  f32 phase[4];
  for (u32 j = 0; j < 4; ++j) {
    const u32 scale = j / 2;
    const f64 angle = base + (j % 2 == 0 ? 1.0 : -1.0) * static_cast<f64>(k_flora_field_spread_rad);
    const f64 number = tau / lengths[scale];
    const f64 kx = std::cos(angle) * number;
    const f64 kz = std::sin(angle) * number;
    k[2 * j] = static_cast<f32>(kx);
    k[2 * j + 1] = static_cast<f32>(kz);
    // A crest travels downwind: phase = k . x - omega t, with omega = |k| times the speed. The
    // phase seeds of the four waves keep their crests from lining up at the world's origin.
    phase[j] = reduce(kx * ex + kz * ez - number * speeds[scale] * time_s +
                      static_cast<f64>(j) * 2.39996322972865332);
  }
  out.waves[0] = Vec4{k[0], k[1], k[2], k[3]};
  out.waves[1] = Vec4{k[4], k[5], k[6], k[7]};
  out.phases = Vec4{phase[0], phase[1], phase[2], phase[3]};
  out.weights = Vec4{k_flora_field_weights[0], k_flora_field_weights[1], k_flora_field_weights[2],
                     k_flora_field_weights[3]};
  if (frame.walker) {
    const f64 ey = static_cast<f64>(eye.cell.y) * k_world_cell_m + static_cast<f64>(eye.local.y) +
                   static_cast<f64>(eye.residual.y);
    const Vec3 feet = relative(frame.walker_feet, WorldPos{ex, ey, ez});
    out.walker = Vec4{feet, frame.walker_radius > 0.0f ? frame.walker_radius : 0.0f};
    out.push = Vec4{frame.walker_height, k_flora_push_max, k_flora_push_margin, 0.0f};
  }
}

f32 flora_gust(f64 time_s, u32 seed) noexcept {
  // Two incommensurate sines, 7.7 s and 3.2 s, phases from the seed: a gust that rises and falls
  // over seconds and never repeats in step across a grove.
  const f32 a = unit_of(hash32(seed ^ 0x9e3779b9u)) * k_tau;
  const f32 b = unit_of(hash32(seed ^ 0x85ebca6bu)) * k_tau;
  const f64 t = time_s;
  const f64 tau = static_cast<f64>(k_tau);
  const f32 g = 0.6f * static_cast<f32>(std::sin(t * (tau * 0.13) + static_cast<f64>(a))) +
                0.4f * static_cast<f32>(std::sin(t * (tau * 0.31) + static_cast<f64>(b)));
  return clamp01(0.5f + 0.5f * g);
}

void flora_part_poses(const FloraMesh& mesh, const FloraWind& wind, f64 time_s, u32 seed,
                      const FloraPush& push, u32 ticks, std::span<FloraPartState> state,
                      std::span<gfx::FloraPartPose> out) noexcept {
  const u32 n = mesh.part_count();
  if (state.size() < n || out.size() < n) return;
  const f32 s = wind.strength < 0.0f               ? 0.0f
                : wind.strength > k_flora_max_wind ? k_flora_max_wind
                                                   : wind.strength;
  const f32 gust = s > 0.0f ? flora_gust(time_s, seed) : 0.0f;
  const f32 lean = s * (0.6f + 0.8f * gust);
  const f32 flutter = s * (0.25f + 0.35f * gust);
  const f32 seed_phase = unit_of(hash32(seed));
  const f32 step = static_cast<f32>(ticks) / static_cast<f32>(k_flora_push_ease_ticks);
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const Vec3 a0 = push.feet + up * push.radius;
  const Vec3 a1 =
      push.feet + up * (push.height > 2.0f * push.radius ? push.height - push.radius : push.radius);
  for (u32 p = 0; p < n; ++p) {
    const geometry::ClusterFileMotionPart& part = mesh.parts[p];
    const Vec3 d = part.direction;
    // ---- the wind's rotation vector
    Vec3 omega{};
    if (s > 0.0f && mesh.compliance[p] > 0.0f) {
      Vec3 perp = wind.to - d * dot(d, wind.to);
      if (dot(perp, perp) < 0.04f) perp = up - d * dot(d, up);  // along the wind: a flutter up
      const f32 pl = length(perp);
      if (pl > 1.0e-4f) {
        const Vec3 k = cross(d, perp * (1.0f / pl));
        const f32 kl = length(k);
        if (kl > 1.0e-4f) {
          const f32 phase =
              k_tau * (seed_phase + 0.37f * part.pivot.x + 0.11f * part.pivot.y +
                       0.53f * part.pivot.z + 0.618034f * static_cast<f32>(mesh.phase_id[p]));
          const f32 hz = k_flora_flutter_hz / std::sqrt(part.length > 0.25f ? part.length : 0.25f);
          const f32 sway = static_cast<f32>(
              std::sin(time_s * static_cast<f64>(k_tau * hz) + static_cast<f64>(phase)));
          const f32 angle = mesh.compliance[p] * (lean + flutter * sway);
          omega = k * (angle / kl);
        }
      }
    }
    // ---- the walker's push, eased
    FloraPartState& st = state[p];
    if (mesh.pushable[p] != 0 && (push.active || st.ease > 0.0f)) {
      bool contact = false;
      if (push.active && part.length > 0.0f) {
        f32 sa = 0.0f;
        f32 sb = 0.0f;
        const Vec3 b1 = part.pivot + d * part.length;
        closest_on_segments(a0, a1, part.pivot, b1, sa, sb);
        const Vec3 pa = a0 + (a1 - a0) * sa;
        const Vec3 pb = part.pivot + d * (part.length * sb);
        const f32 reach = push.radius + mesh.reach[p] + k_flora_push_margin;
        Vec3 away = pb - pa;
        const f32 dist = length(away);
        if (dist < reach && sb * part.length > 0.05f) {
          away = away - d * dot(away, d);
          if (dot(away, away) < 1.0e-8f) {
            away = Vec3{pb.x - push.feet.x, 0.0f, pb.z - push.feet.z};
            if (dot(away, away) < 1.0e-8f) away = Vec3{1.0f, 0.0f, 0.0f};
          }
          away = away * (1.0f / length(away));
          const Vec3 k = cross(d, away);
          const f32 kl = length(k);
          if (kl > 1.0e-4f) {
            const f32 arc = std::fmax(sb * part.length, 0.25f * part.length);
            const f32 needed = (reach - dist) / arc;
            const f32 angle = needed < k_flora_push_max ? needed : k_flora_push_max;
            st.push = k * (angle / kl);
            contact = true;
          }
        }
      }
      st.ease = contact ? (st.ease + step < 1.0f ? st.ease + step : 1.0f)
                        : (st.ease - step > 0.0f ? st.ease - step : 0.0f);
      const f32 e = st.ease * st.ease * (3.0f - 2.0f * st.ease);
      omega = omega + st.push * e;
      if (st.ease == 0.0f) st.push = Vec3{};
    }
    // ---- the pose: the parent's frame times the parent's own bend where this part leaves it
    gfx::FloraPartPose pose{};
    bool moving = false;
    if (part.parent != geometry::k_motion_no_parent) {
      const gfx::FloraPartPose& parent = out[part.parent];
      if (parent.axis.w != 0.0f) {
        moving = true;
        const f32 bend = parent.pivot_angle.w * mesh.attach[p];
        if (bend == 0.0f) {
          for (u32 r = 0; r < 3; ++r)
            pose.rows[r] = parent.rows[r];
        } else {
          Vec3 b[3];
          rotation_rows(parent.axis.xyz(), bend, b);
          const Vec3 pivot = parent.pivot_angle.xyz();
          const Vec3 bt = pivot - row_times(b, pivot);
          const Frame f = frame_of(parent);
          // F * B: rows of the product, and F's rotation of B's translation plus F's own.
          const Vec3 cols[3] = {Vec3{b[0].x, b[1].x, b[2].x}, Vec3{b[0].y, b[1].y, b[2].y},
                                Vec3{b[0].z, b[1].z, b[2].z}};
          const Vec3 t = row_times(f.r, bt) + f.t;
          for (u32 r = 0; r < 3; ++r) {
            pose.rows[r] = Vec4{dot(f.r[r], cols[0]), dot(f.r[r], cols[1]), dot(f.r[r], cols[2]),
                                r == 0 ? t.x : (r == 1 ? t.y : t.z)};
          }
        }
      }
    }
    const f32 angle = length(omega);
    pose.pivot_angle = Vec4{part.pivot, angle};
    if (angle > 0.0f) {
      pose.axis = Vec4{omega * (1.0f / angle), 1.0f};
    } else {
      pose.axis = Vec4{0.0f, 0.0f, 1.0f, moving ? 1.0f : 0.0f};
    }
    out[p] = pose;
  }
}

f32 flora_bounds_padding(const FloraMesh& mesh, std::span<const Vec3> vertices,
                         std::span<const geometry::ClusterFileMotionVertex> motion) noexcept {
  // The most any part turns: its lean and flutter at their peaks under the strongest wind. A
  // rotation of a about a pivot moves a point at r from it at most a r, and the bends of a vertex's
  // chain compose, so the sum over the chain bounds the vertex. **The push is not in it**: a pushed
  // part is within a metre of the walker, whose eye is the camera, and it turns away from it — and
  // its 0.9 rad over a 4 m frond would pad every sphere of every palm by 3.6 m at any distance,
  // which the cut pays for in finer clusters everywhere (renderer.md, "The flora stage").
  const u32 n = mesh.part_count();
  if (n == 0 || vertices.size() != motion.size()) return 0.0f;
  Vector<f32> most(n, 0.0f);
  for (u32 p = 0; p < n; ++p) {
    most[p] = mesh.compliance[p] * k_flora_max_wind * (0.6f + 0.8f + 0.25f + 0.35f);
  }
  f32 worst = 0.0f;
  for (u32 v = 0; v < vertices.size(); ++v) {
    u32 p = motion[v].part;
    if (p >= n) continue;
    const Vec3 at = vertices[v];
    f32 bound = most[p] * (static_cast<f32>(motion[v].weight) * (1.0f / 65535.0f)) *
                length(at - mesh.parts[p].pivot);
    for (u32 guard = 0; guard < 64 && mesh.parts[p].parent != geometry::k_motion_no_parent;
         ++guard) {
      p = mesh.parts[p].parent;
      bound += most[p] * length(at - mesh.parts[p].pivot);
    }
    worst = bound > worst ? bound : worst;
  }
  return worst;
}

}  // namespace engine::renderer
