#include "kit.h"

#include <domain/flora/roots.h>

#include <algorithm>
#include <limits>

namespace engine::flora {
namespace {
using namespace detail;
bool bad(std::string& error, const char* reason) {
  error = std::string("root stem: ") + reason;
  return false;
}
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}  // namespace

bool fit_root_stem(const assets::MeshData& mesh, const Motion& motion, Roots& roots,
                   std::string& error) {
  if (!roots.enabled) {
    roots.stem.reset();
    return true;
  }
  // Ancient trunks own buttress fins and recessed village openings. A ring sleeve would
  // bridge those authored features; keep their standalone roots separate.
  if (roots.species == "ancient")
    return bad(error, "ancient trunks own their buttresses and hollows; use standalone roots");
  if (motion.parts.empty() || motion.vertex_part.size() != mesh.positions.size() ||
      mesh.normals.size() != mesh.positions.size() || mesh.uvs.size() != mesh.positions.size() ||
      mesh.indices.size() % 3 != 0)
    return bad(error, "missing or mismatched trunk streams");
  const auto& part = motion.parts[0];
  const f64 direction_y = static_cast<f64>(part.direction.y);
  const f64 origin_y = static_cast<f64>(part.pivot.y);
  const f64 stem_length = static_cast<f64>(part.length);
  if (!finite(part.pivot) || !finite(part.direction) || direction_y < .5 ||
      !std::isfinite(stem_length) || stem_length <= 0 ||
      !std::isfinite(static_cast<f64>(roots.trunk_radius_m)) || roots.trunk_radius_m <= 0)
    return bad(error, "invalid trunk frame or radius");
  const f64 height =
      std::min(2.5 * static_cast<f64>(roots.trunk_radius_m), .2 * stem_length * direction_y);
  if (mesh.primitives.empty()) return bad(error, "missing bark primitive");
  const auto& bark = mesh.primitives[0];
  const u64 end = static_cast<u64>(bark.first_index) + bark.index_count;
  if (end > mesh.indices.size() || bark.first_index % 3 != 0 || bark.index_count % 3 != 0)
    return bad(error, "invalid bark primitive range");
  f64 bottom = origin_y;
  Vector<u32> faces;
  for (u32 i = bark.first_index; i < end; i += 3) {
    const u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
    if (a >= mesh.positions.size() || b >= mesh.positions.size() || c >= mesh.positions.size())
      return bad(error, "index outside vertex stream");
    if (motion.vertex_part[a] != 0 || motion.vertex_part[b] != 0 || motion.vertex_part[c] != 0)
      continue;
    const Vec3 pa = mesh.positions[a], pb = mesh.positions[b], pc = mesh.positions[c];
    if (!finite(pa) || !finite(pb) || !finite(pc)) return bad(error, "nonfinite trunk vertex");
    for (const u32 v : {a, b, c})
      if (std::abs(mesh.uvs[v].y) < 1e-6f)
        bottom = std::max(bottom, static_cast<f64>(mesh.positions[v].y));
    if (static_cast<f64>(std::min({pa.y, pb.y, pc.y})) > origin_y + height ||
        static_cast<f64>(std::max({pa.y, pb.y, pc.y})) < origin_y)
      continue;
    faces.push_back(i);
  }
  if (faces.empty()) return bad(error, "no lower trunk faces");
  bottom = bottom - origin_y + .001 * height;
  if (!(bottom < height)) return bad(error, "tilted base exceeds collar height");
  f64 u_min = std::numeric_limits<f64>::max(), u_max = -u_min;
  for (const u32 f : faces) {
    for (u32 k = 0; k < 3; ++k) {
      const f64 u = static_cast<f64>(mesh.uvs[mesh.indices[f + k]].x);
      if (!std::isfinite(u)) return bad(error, "nonfinite trunk UV");
      u_min = std::min(u_min, u);
      u_max = std::max(u_max, u);
    }
  }
  RootStem stem;
  for (u32 row = 0; row < stem.rows; ++row) {
    // Avoid a horizontal cap/edge ambiguity at the ground origin.
    const f64 y = lerp(bottom, height, static_cast<f64>(row) / (stem.rows - 1));
    const D3 origin = from_f32(part.pivot) + from_f32(part.direction) * (y / direction_y);
    // Align each ring with the trunk's own UV seam, including its transported frame.
    // Otherwise a uniform world-angle ring would interpolate across the bark UV wrap.
    D3 seam_direction{1, 0, 0};
    bool have_seam = false;
    for (const u32 f : faces) {
      for (u32 k = 0; k < 3; ++k) {
        const u32 ia = mesh.indices[f + k], ib = mesh.indices[f + (k + 1) % 3];
        if (std::abs(static_cast<f64>(mesh.uvs[ia].x) - u_min) > 1e-6 ||
            std::abs(static_cast<f64>(mesh.uvs[ib].x) - u_min) > 1e-6)
          continue;
        const D3 a = from_f32(mesh.positions[ia]), b = from_f32(mesh.positions[ib]);
        if (std::abs(b.y - a.y) < 1e-9) continue;
        const f64 t = (origin.y - a.y) / (b.y - a.y);
        if (t < 0 || t > 1) continue;
        const D3 point = a + (b - a) * t;
        seam_direction = normalized(D3{point.x - origin.x, 0, point.z - origin.z});
        have_seam = true;
        break;
      }
      if (have_seam) break;
    }
    if (!have_seam) return bad(error, "lower trunk has no continuous bark UV seam");
    for (u32 j = 0; j <= stem.sides; ++j) {
      if (j == stem.sides) {
        auto seam = stem.samples[row * (stem.sides + 1)];
        const u32 first = row * (stem.sides + 1);
        const bool descending = stem.samples[first + 1].u > stem.samples.back().u;
        stem.samples[first].u = static_cast<f32>(descending ? u_max : u_min);
        seam.u = static_cast<f32>(descending ? u_min : u_max);
        stem.samples.push_back(seam);
        continue;
      }
      const f64 angle = static_cast<f64>(j) * 2 * k_pi / stem.sides;
      const f64 ca = det::cos(angle), sa = det::sin(angle);
      const D3 direction{seam_direction.x * ca - seam_direction.z * sa, 0,
                         seam_direction.z * ca + seam_direction.x * sa};
      f64 closest = std::numeric_limits<f64>::max();
      RootStemSample sample;
      bool found = false;
      for (const u32 f : faces) {
        const u32 ia = mesh.indices[f], ib = mesh.indices[f + 1], ic = mesh.indices[f + 2];
        if (!finite(mesh.normals[ia]) || !finite(mesh.normals[ib]) || !finite(mesh.normals[ic]))
          return bad(error, "nonfinite trunk normal");
        const D3 a = from_f32(mesh.positions[ia]);
        const D3 e1 = from_f32(mesh.positions[ib]) - a;
        const D3 e2 = from_f32(mesh.positions[ic]) - a;
        const D3 h = cross(direction, e2);
        const f64 det = dot(e1, h);
        if (std::abs(det) < 1e-12) continue;
        const D3 s = origin - a;
        const f64 u = dot(s, h) / det;
        if (u < -1e-8 || u > 1 + 1e-8) continue;
        const D3 q = cross(s, e1);
        const f64 v = dot(direction, q) / det;
        if (v < -1e-8 || u + v > 1 + 1e-8) continue;
        const f64 t = dot(e2, q) / det;
        if (t <= 0 || t >= closest) continue;
        const D3 point = origin + direction * t;
        const D3 normal =
            normalized(from_f32(mesh.normals[ia]) * (1 - u - v) + from_f32(mesh.normals[ib]) * u +
                       from_f32(mesh.normals[ic]) * v);
        const f64 tu = static_cast<f64>(mesh.uvs[ia].x) * (1 - u - v) +
                       static_cast<f64>(mesh.uvs[ib].x) * u + static_cast<f64>(mesh.uvs[ic].x) * v;
        const f64 tv = static_cast<f64>(mesh.uvs[ia].y) * (1 - u - v) +
                       static_cast<f64>(mesh.uvs[ib].y) * u + static_cast<f64>(mesh.uvs[ic].y) * v;
        sample = {static_cast<f32>(point.x),  static_cast<f32>(point.y),
                  static_cast<f32>(point.z),  static_cast<f32>(normal.x),
                  static_cast<f32>(normal.y), static_cast<f32>(normal.z),
                  static_cast<f32>(tu),       static_cast<f32>(tv)};
        closest = t;
        found = true;
      }
      if (!found) {
        error = "root stem: no surface at ring " + std::to_string(row) + ", angular sample " +
                std::to_string(j);
        return false;
      }
      stem.samples.push_back(sample);
    }
  }
  Roots checked = roots;
  checked.stem = std::move(stem);
  if (!roots_in_range(checked, error)) return false;
  roots.stem = std::move(checked.stem);
  return true;
}
}  // namespace engine::flora
