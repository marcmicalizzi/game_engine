#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/camera_path.h>

#include <algorithm>
#include <cmath>
#include <schemas/scene.h>

namespace engine::renderer {

namespace {

// A point `k_orientation_reach` metres ahead of an orientation key: far enough that a camera
// turning between two such keys turns about its own position rather than swinging round a
// point just in front of it.
constexpr f32 k_orientation_reach = 100.0f;

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text;
}

// The tangent a cubic leaves key `i` with, per unit of time. The average of the two neighbouring
// segments' velocities, except that a key beside a segment that does not move — a hold, where the
// file repeats a position — gets none: a cubic that carried the incoming velocity through a hold
// would overshoot it and come back, which reads as a wobble on a camera that was told to stop.
template <class T>
T tangent(const Vector<CameraPathKey>& keys, u32 i, T (*get)(const CameraPathKey&),
          bool (*still)(const T&, const T&)) noexcept {
  const u32 n = keys.size();
  if (n < 2) return T{};
  if (i == 0) {
    const T a = get(keys[0]);
    const T b = get(keys[1]);
    return still(a, b) ? T{} : (b - a) * static_cast<f32>(1.0 / (keys[1].time - keys[0].time));
  }
  if (i + 1 >= n) {
    const T a = get(keys[n - 2]);
    const T b = get(keys[n - 1]);
    return still(a, b) ? T{}
                       : (b - a) * static_cast<f32>(1.0 / (keys[n - 1].time - keys[n - 2].time));
  }
  const T prev = get(keys[i - 1]);
  const T here = get(keys[i]);
  const T next = get(keys[i + 1]);
  if (still(prev, here) || still(here, next)) return T{};
  const T in = (here - prev) * static_cast<f32>(1.0 / (keys[i].time - keys[i - 1].time));
  const T out = (next - here) * static_cast<f32>(1.0 / (keys[i + 1].time - keys[i].time));
  return (in + out) * 0.5f;
}

Vec3 position_of(const CameraPathKey& k) { return k.position; }
Vec3 target_of(const CameraPathKey& k) { return k.target; }
bool vec_still(const Vec3& a, const Vec3& b) { return length(b - a) < 1e-4f; }

// The cubic Hermite basis over one segment, with tangents scaled by the segment's duration.
template <class T>
T hermite(const T& p0, const T& m0, const T& p1, const T& m1, f32 dt, f32 s) noexcept {
  const f32 s2 = s * s;
  const f32 s3 = s2 * s;
  const f32 h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
  const f32 h10 = s3 - 2.0f * s2 + s;
  const f32 h01 = -2.0f * s3 + 3.0f * s2;
  const f32 h11 = s3 - s2;
  return p0 * h00 + m0 * (h10 * dt) + p1 * h01 + m1 * (h11 * dt);
}

}  // namespace

u32 CameraPath::frame_count() const noexcept {
  if (keys.size() < 2 || !(fps > 0.0f)) return 1;
  return static_cast<u32>(std::llround(duration() * static_cast<f64>(fps))) + 1;
}

bool parse_camera_path(std::string_view text, const TerrainDesc* terrain, CameraPath& out,
                       std::string& error) {
  JsonValue json;
  const JsonParseResult parsed = parse_json(text, json);
  if (!parsed.ok) {
    error = "line " + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  scene::CameraPath file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, json, ctx) || !ctx.ok()) {
    error = schema_errors(ctx);
    return false;
  }
  if (!file.format.empty() && file.format != "engine.camera-path.v1") {
    error = "format is '" + file.format + "', not engine.camera-path.v1";
    return false;
  }
  if (file.keys.empty()) {
    error = "a camera path needs at least one key";
    return false;
  }
  if (!(file.fps > 0.0f) || file.fps > 1000.0f || !(file.znear > 0.0f)) {
    error = "fps must be within (0, 1000] and znear positive";
    return false;
  }
  out = CameraPath{};
  out.name = file.name;
  out.smooth = file.interpolation == scene::Interpolation::Smooth;
  out.fps = file.fps;
  out.znear = file.znear;
  out.hash = hash_bytes(text.data(), text.size());
  out.keys.reserve(file.keys.size());
  // The dune field drawn once for the path rather than once per key (terrain.h, `TerrainSampler`):
  // the same heights, bit for bit.
  const TerrainDesc no_terrain;
  const TerrainSampler ground(terrain != nullptr ? *terrain : no_terrain);
  for (u32 i = 0; i < file.keys.size(); ++i) {
    const scene::CameraKey& source = file.keys[i];
    const std::string where = "key " + std::to_string(i);
    if (i > 0 && !(source.time > file.keys[i - 1].time)) {
      error = where + ": times must increase strictly";
      return false;
    }
    if (!(source.fov_deg > 1.0f && source.fov_deg < 179.0f)) {
      error = where + ": fov_deg must be within (1, 179)";
      return false;
    }
    if ((source.ground || source.target_ground) && terrain == nullptr) {
      error = where + ": holds a height above the ground, and the scene has no terrain";
      return false;
    }
    CameraPathKey key;
    key.time = source.time;
    key.position = source.position;
    if (source.ground) key.position.y += ground.height(source.position.x, source.position.z);
    if (source.target.has_value()) {
      key.target = *source.target;
      if (source.target_ground) key.target.y += ground.height(key.target.x, key.target.z);
    } else {
      const Quat rotation =
          source.rotation.has_value() ? normalize(*source.rotation) : Quat::identity();
      key.target = key.position + rotate(rotation, Vec3{0.0f, 0.0f, -1.0f}) * k_orientation_reach;
    }
    if (length(key.target - key.position) < 1e-3f) {
      error = where + ": the camera looks at its own position";
      return false;
    }
    key.fov_y = radians(source.fov_deg);
    out.keys.push_back(key);
  }
  for (const scene::CameraMarker& marker : file.markers) {
    if (marker.frame >= out.frame_count()) {
      error = "marker '" + marker.name + "' is at frame " + std::to_string(marker.frame) +
              ", past the path's " + std::to_string(out.frame_count()) + " frames";
      return false;
    }
    out.markers.push_back(CameraPathMarker{marker.frame, marker.name, marker.note});
  }
  return true;
}

bool read_camera_path(const std::string& path, const TerrainDesc* terrain, CameraPath& out,
                      std::string& error) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = "cannot read " + path + ": " + io::status_name(status);
    return false;
  }
  if (!parse_camera_path(text, terrain, out, error)) {
    error = path + ": " + error;
    return false;
  }
  return true;
}

Camera sample_camera_path(const CameraPath& path, f64 time) noexcept {
  // A path's keys are the file's `vec3`s (a float32 on disk this batch, ADR-0053), interpolated in
  // float32 as they always were and widened into the camera's world positions here, at the read.
  auto widen = [](Vec3 p) { return absolute(WorldPos::origin(), p); };
  Camera camera;
  camera.znear = path.znear;
  if (path.keys.empty()) return camera;
  const Vector<CameraPathKey>& keys = path.keys;
  const u32 n = keys.size();
  if (n == 1 || time <= keys[0].time) {
    camera.position = widen(keys[0].position);
    camera.target = widen(keys[0].target);
    camera.fov_y = keys[0].fov_y;
    return camera;
  }
  if (time >= keys[n - 1].time) {
    camera.position = widen(keys[n - 1].position);
    camera.target = widen(keys[n - 1].target);
    camera.fov_y = keys[n - 1].fov_y;
    return camera;
  }
  // The segment holding `time`: the last key at or before it.
  u32 lo = 0;
  u32 hi = n - 1;
  while (hi - lo > 1) {
    const u32 mid = (lo + hi) / 2;
    if (keys[mid].time <= time) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const CameraPathKey& a = keys[lo];
  const CameraPathKey& b = keys[hi];
  const f64 dt = b.time - a.time;
  const f32 s = static_cast<f32>((time - a.time) / dt);
  if (!path.smooth) {
    camera.position = widen(a.position + (b.position - a.position) * s);
    camera.target = widen(a.target + (b.target - a.target) * s);
    camera.fov_y = a.fov_y + (b.fov_y - a.fov_y) * s;
    return camera;
  }
  const f32 dtf = static_cast<f32>(dt);
  camera.position =
      widen(hermite(a.position, tangent<Vec3>(keys, lo, position_of, vec_still), b.position,
                    tangent<Vec3>(keys, hi, position_of, vec_still), dtf, s));
  camera.target = widen(hermite(a.target, tangent<Vec3>(keys, lo, target_of, vec_still), b.target,
                                tangent<Vec3>(keys, hi, target_of, vec_still), dtf, s));
  // The field of view eases between keys rather than following a cubic through them: a zoom that
  // overshot its key would be the one wobble a reader of the pictures could not explain.
  const f32 e = s * s * (3.0f - 2.0f * s);
  camera.fov_y = a.fov_y + (b.fov_y - a.fov_y) * e;
  return camera;
}

f64 camera_path_frame_time(const CameraPath& path, u32 frame, u32 frames) noexcept {
  if (frames <= 1) return path.start();
  const f64 f = static_cast<f64>(std::min(frame, frames - 1));
  return path.start() + path.duration() * f / static_cast<f64>(frames - 1);
}

Camera camera_path_frame(const CameraPath& path, u32 frame, u32 frames) noexcept {
  return sample_camera_path(path, camera_path_frame_time(path, frame, frames));
}

}  // namespace engine::renderer
