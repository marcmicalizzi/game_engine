#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
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

// The tangent a cubic leaves key `i` with, metres per second, in f64: the average of the two
// neighbouring segments' velocities, except that a key beside a segment that does not move — a
// hold, where the file repeats a position — gets none: a cubic that carried the incoming velocity
// through a hold would overshoot it and come back, which reads as a wobble on a camera that was
// told to stop. Built from the keys' differences only, so a path moved by whole cells has the same
// tangents to the bit.
using KeyPoint = WorldPos (*)(const CameraPathKey&);
WorldPos position_of(const CameraPathKey& k) { return k.position; }
WorldPos target_of(const CameraPathKey& k) { return k.target; }
bool still(DVec3 d) { return length(d) < 1e-4; }

DVec3 tangent(const Vector<CameraPathKey>& keys, u32 i, KeyPoint get) noexcept {
  const u32 n = keys.size();
  if (n < 2) return DVec3{};
  if (i == 0 || i + 1 >= n) {
    const u32 a = i == 0 ? 0 : n - 2;
    const DVec3 d = get(keys[a + 1]) - get(keys[a]);
    return still(d) ? DVec3{} : d / (keys[a + 1].time - keys[a].time);
  }
  const DVec3 in = get(keys[i]) - get(keys[i - 1]);
  const DVec3 out = get(keys[i + 1]) - get(keys[i]);
  if (still(in) || still(out)) return DVec3{};
  return (in / (keys[i].time - keys[i - 1].time) + out / (keys[i + 1].time - keys[i].time)) * 0.5;
}

// The cubic Hermite through one segment, in f64, **as a displacement from its first key**:
// `p0 + (p1 - p0) h01 + m0 dt h10 + m1 dt h11`, which is the basis `p0 h00 + p1 h01 + ...` with
// h00 = 1 - h01 and no world coordinate scaled. Everything but the last sum is the size of the
// segment, so it is the same numbers wherever the path is (above).
WorldPos hermite(WorldPos p0, DVec3 m0, WorldPos p1, DVec3 m1, f64 dt, f64 s) noexcept {
  const f64 s2 = s * s;
  const f64 s3 = s2 * s;
  const f64 h10 = s3 - 2.0 * s2 + s;
  const f64 h01 = -2.0 * s3 + 3.0 * s2;
  const f64 h11 = s3 - s2;
  return p0 + ((p1 - p0) * h01 + m0 * (h10 * dt) + m1 * (h11 * dt));
}

// The surface under a key at its whole millimetre, at the terrain's own time
// (`GroundOps::height_mm`): no float32 metre on the way, so a key 420 km out stands on the ground
// under it and not under the nearest float. By the origin, the heights `TerrainSampler::height`
// gives at a point a float holds to the millimetre.
f64 ground_under(const TerrainSampler& ground, WorldPos p) noexcept {
  return static_cast<f64>(ground.provider().height_mm(
      ground.desc().time_s, scene_gen::nearest_mm(p.x), scene_gen::nearest_mm(p.z)));
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
    // The key's numbers as the file wrote them (`worldpos`, CameraKey version 2): a version 1
    // path's are the same numbers, read without rounding them to a float.
    CameraPathKey key;
    key.time = source.time;
    key.position = source.position;
    if (source.ground) key.position.y += ground_under(ground, source.position);
    if (source.target.has_value()) {
      key.target = *source.target;
      if (source.target_ground) key.target.y += ground_under(ground, key.target);
    } else {
      const Quat rotation =
          source.rotation.has_value() ? normalize(*source.rotation) : Quat::identity();
      key.target =
          key.position + DVec3{rotate(rotation, Vec3{0.0f, 0.0f, -1.0f}) * k_orientation_reach};
    }
    if (length(key.target - key.position) < 1e-3) {
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
  // In f64 (ADR-0053): the keys are world positions, and a sample is the segment's first key plus a
  // displacement the size of the segment (`hermite`), so it rounds once, at the camera's own place.
  Camera camera;
  camera.znear = path.znear;
  if (path.keys.empty()) return camera;
  const Vector<CameraPathKey>& keys = path.keys;
  const u32 n = keys.size();
  if (n == 1 || time <= keys[0].time) {
    camera.position = keys[0].position;
    camera.target = keys[0].target;
    camera.fov_y = keys[0].fov_y;
    return camera;
  }
  if (time >= keys[n - 1].time) {
    camera.position = keys[n - 1].position;
    camera.target = keys[n - 1].target;
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
  const f64 s = (time - a.time) / dt;
  // The field of view is an angle, not a place: float32, as it always was, from the same fraction.
  const f32 sf = static_cast<f32>(s);
  if (!path.smooth) {
    camera.position = a.position + (b.position - a.position) * s;
    camera.target = a.target + (b.target - a.target) * s;
    camera.fov_y = a.fov_y + (b.fov_y - a.fov_y) * sf;
    return camera;
  }
  camera.position = hermite(a.position, tangent(keys, lo, position_of), b.position,
                            tangent(keys, hi, position_of), dt, s);
  camera.target = hermite(a.target, tangent(keys, lo, target_of), b.target,
                          tangent(keys, hi, target_of), dt, s);
  // The field of view eases between keys rather than following a cubic through them: a zoom that
  // overshot its key would be the one wobble a reader of the pictures could not explain.
  const f32 e = sf * sf * (3.0f - 2.0f * sf);
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
