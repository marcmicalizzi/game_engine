#include <core/jobs/job_system.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/request.h>
#include <systems/renderer/terrain_levels.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <schemas/protocol.h>

namespace engine::renderer {

namespace {

bool parse_weight(const std::string& text, f32& out) {
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !(v >= -1.0e6 && v <= 1.0e6)) return false;
  out = static_cast<f32>(v);
  return true;
}

bool parse_index(const std::string& text, u32& out) {
  char* end = nullptr;
  const unsigned long v = std::strtoul(text.c_str(), &end, 10);
  if (text.empty() || end == text.c_str() || *end != '\0' || v > 0xFFFFFFFFul) return false;
  out = static_cast<u32>(v);
  return true;
}

void set_error(std::string* error, std::string text) {
  if (error != nullptr) *error = std::move(text);
}

}  // namespace

// ---- the request's parts ----------------------------------------------------------------------

void scene_file_options(const RenderRequest& request, SceneFileOptions& out) {
  out.ground_time_s = request.ground_time_s;
}

// The static shape stage's weights (renderer.md, "Morphed instances"). **The asset's own weights
// are where the stage starts**: glTF's `weights` are what a mesh is drawn with when nothing
// animates it, and a file can author a half-applied target as its rest shape (MorphPrimitivesTest
// does, at 0.5), so every channel starts from its `default_weight` — unless a pose stage plays the
// channels (`--morph-animate`), which starts from them itself — and `morph` overrides the channels
// it names, by name or by index, since a generated rig may have no name worth typing. A weight is
// any finite number, zero and negatives included (a corrective rig uses them). An unknown name is
// an error rather than an ignored weight: a misspelt expression that quietly does nothing gets
// debugged in the picture. When every default is zero and nothing is named the array stays empty,
// which is what it always was for such a mesh. This was engine-view's alone until 2026-10-03, and
// engine-host drew a mesh authored half-morphed unmorphed — the drift this file exists to end.
bool settings_for(const RenderRequest& request, const SceneData& scene, RenderSettings& out,
                  std::string* error) {
  out = request.settings;
  if (request.page_budget_pct > 0 && !scene.pages.pages.empty()) {
    u64 total = 0;
    for (const geometry::ClusterPageDesc& page : scene.pages.pages)
      total += page.bytes;
    out.page_budget_bytes = total * request.page_budget_pct / 100;
  }
  const geometry::ClusterMesh& mesh = scene.lod.mesh;
  Vector<f32>& weights = out.morph_static_weights;
  weights.clear();
  if (request.morph_defaults) {
    bool authored = false;
    for (const geometry::MorphChannel& channel : mesh.morph_channels)
      authored = authored || channel.default_weight != 0.0f;
    if (authored) {
      weights.resize(mesh.morph_channels.size(), 0.0f);
      for (u32 c = 0; c < mesh.morph_channels.size(); ++c)
        weights[c] = mesh.morph_channels[c].default_weight;
    }
  }
  if (request.morph.empty()) return true;
  if (mesh.morph_channels.empty()) {
    set_error(error, "this mesh has no morph channels");
    return false;
  }
  weights.resize(mesh.morph_channels.size(), 0.0f);
  for (const std::string& entry : request.morph) {
    const usize split = entry.find('=');
    if (split == std::string::npos) {
      set_error(error, "a morph entry is <name|index>=<weight>; got '" + entry + "'");
      return false;
    }
    const std::string name = entry.substr(0, split);
    f32 weight = 0.0f;
    if (!parse_weight(entry.substr(split + 1), weight)) {
      set_error(error, "'" + entry.substr(split + 1) + "' is not a morph weight");
      return false;
    }
    u32 channel = ~0u;
    for (u32 c = 0; c < mesh.morph_names.size(); ++c) {
      if (mesh.morph_names[c] == name) channel = c;
    }
    if (channel == ~0u) {
      u32 index = 0;
      if (parse_index(name, index) && index < mesh.morph_channels.size()) channel = index;
    }
    if (channel == ~0u) {
      set_error(error, "this mesh has no morph channel named '" + name + "'");
      return false;
    }
    weights[channel] = weight;
  }
  return true;
}

ViewSetDesc view_set_desc(const RenderSettings& resolved) {
  ViewSetDesc out;
  out.layout = resolved.views;
  out.surround.side_yaw = resolved.side_yaw;
  out.panini_d = resolved.panini_d;
  out.peripheral_lod = resolved.peripheral_lod;
  return out;
}

// ---- the clock --------------------------------------------------------------------------------

// The offset `time_of_day` puts on the world's one clock: the hours asked for, less how far into
// its day the scene's ground already stands. Zero without a sky, whose stand-in sun has no hour.
FrameClock frame_clock(const ClockRequest& clock, const SceneData& scene) {
  FrameClock out;
  out.sun_rate = clock.sun_rate.has_value() ? *clock.sun_rate : sun_rate_tunable();
  if (!(out.sun_rate > 0.0)) out.sun_rate = 0.0;
  if (clock.time_of_day_h.has_value() && scene.sky.has_value()) {
    const f64 start = scene.terrain.enabled ? scene.terrain.time_s : 0.0;
    const f64 into_day = start - std::floor(start / 86400.0) * 86400.0;
    out.offset_s = *clock.time_of_day_h * 3600.0 - into_day;
  }
  return out;
}

FrameDesc frame_at(const FrameClock& clock, const Camera& camera, u64 frame_index) {
  FrameDesc frame;
  frame.camera = camera;
  frame.frame_index = frame_index;
  frame.sun_time_s = clock.at(frame_index);
  return frame;
}

void flight_clock(const FrameClock& clock, FlightOptions& out) {
  out.sun_rate = clock.sun_rate;
  out.sun_time_s = clock.offset_s;
}

// ---- the moving ground ------------------------------------------------------------------------

MovingGround::MovingGround() = default;
MovingGround::~MovingGround() { finish(); }

bool MovingGround::prepare(const SceneData& data, const ResolvedSettings& resolved,
                           const Camera& first, std::string* error, bool window) {
  stage_ = "";
  if (!resolved.terrain_levels) return true;
  // The time-lapse's pool is its own, so its evaluations never compete with a page read; a
  // window's leaves the frame's thread CPUs of its own (`terrain_window_workers`).
  jobs::JobSystemConfig config{.pin_threads = false};
  if (window) config.performance_workers = terrain_window_workers();
  jobs_ = std::make_unique<jobs::JobSystem>(config);
  if (resolved.terrain_rings) {
    rings_ = std::make_unique<TerrainRingSet>();
    if (!rings_->build(data.terrain, first.position, jobs_.get(), error)) {
      stage_ = "terrain-rings";
      return false;
    }
  }
  if (resolved.terrain_tiles) {
    ground_ = std::make_unique<TerrainSampler>(data.terrain);
    tiles_ = std::make_unique<TerrainTileSet>();
    // The far levels past the world's rings are the renderer's own (ADR-0051): the request's
    // `terrain_far_levels` lays them out, -1 the tunable's count.
    if (!tiles_->build(data.terrain,
                       terrain_tiles_desc(data.world, resolved.settings.terrain_far_levels),
                       ground_->provider().tiles(), first.position, jobs_.get(), error)) {
      stage_ = "terrain-tiles";
      return false;
    }
  }
  return true;
}

TerrainLevelSet* MovingGround::level_set() noexcept {
  if (rings_ != nullptr) return rings_.get();
  return tiles_.get();
}

const scene_gen::TileSource* MovingGround::tile_source() const noexcept {
  return tiles_ != nullptr ? tiles_->source() : nullptr;
}

bool MovingGround::start(GpuScene& scene, const ResolvedSettings& resolved, bool wait,
                         std::string* error) {
  if (!resolved.terrain_levels) return true;
  TimeLapseConfig lapse = time_lapse_config_from_tunables(resolved.settings.time_rate);
  lapse.wait = wait;
  stage_ = "time-rate";
  if (!motion_.start(scene, level_set(), lapse, jobs_.get(), error)) return false;
  stage_ = "";
  return true;
}

void MovingGround::follow_tiles(WorldPos camera) {
  if (tiles_ == nullptr) return;
  // The world ring's first-fill rule (`terrain_tiles_round`), from the camera in f64.
  terrain_tiles_round(tiles_->tiles_desc(), camera, round_);
  tiles_->set_tiles(std::span<const TerrainTile>(round_.data(), round_.size()));
}

void MovingGround::frame(const Camera& camera) {
  motion_.frame(1.0 / static_cast<f64>(k_frame_hz), camera.position);
}

void MovingGround::finish() { motion_.finish(); }

const char* ground_layout(const TerrainMotion& motion) noexcept {
  if (!motion.active()) return "";
  if (!motion.has_rings()) return "grid";
  return motion.level_set() != nullptr && motion.level_set()->grid_drawn() ? "rings" : "tiles";
}

// ---- the summaries ----------------------------------------------------------------------------

// The summary's `sun` block (apps.md, "--sun-rate"; renderer.md, "The sun's day"): the rate its
// day ran at when the run ended, the one it started with and how often it changed, how far into
// the day the last frame was (game seconds), where it started and the arc's tilt, and where it
// stood at the end — direction, azimuth from +x towards +z and elevation, degrees — and how
// strongly it shone. The renderer's own model, asked the same question the frame asked it.
JsonValue sun_summary(const RenderSettings& settings, const SunDayState& day) {
  const SunArc arc = sun_arc(settings);
  const LightingOptions lit = lighting_options(settings, day.time_s);
  constexpr f64 k_deg = 180.0 / 3.14159265358979323846;
  const Vec3 d = lit.sun;
  f64 azimuth = std::atan2(static_cast<f64>(d.z), static_cast<f64>(d.x)) * k_deg;
  if (azimuth < 0.0) azimuth += 360.0;
  const f64 y = static_cast<f64>(d.y);
  JsonValue out = JsonValue::object();
  out.set("rate", day.rate);
  out.set("start_rate", day.start_rate);
  out.set("rate_changes", day.changes);
  out.set("time_s", day.time_s);
  out.set("start_azimuth_deg", arc.azimuth_deg);
  out.set("start_elevation_deg", arc.elevation_deg);
  out.set("tilt_deg", arc.tilt_deg);
  out.set("azimuth_deg", azimuth);
  out.set("elevation_deg", std::asin(y < -1.0 ? -1.0 : (y > 1.0 ? 1.0 : y)) * k_deg);
  out.set("intensity", static_cast<f64>(lit.sun_intensity));
  JsonValue direction = JsonValue::array();
  direction.push_back(JsonValue(d.x));
  direction.push_back(JsonValue(d.y));
  direction.push_back(JsonValue(d.z));
  out.set("direction", std::move(direction));
  return out;
}

// The same block with a scene's sky (renderer.md, "The sky"): the stand-in's arc is not what drew
// the frame, so a `sky` block beside it says what did — the game time the sky stood at, the day of
// the year and the local hour, the sun and the moon (azimuth from +x towards +z and elevation,
// degrees), the moon's lit fraction, whether the cascaded maps followed it, and the exposure value
// the last frame was drawn at and the light it was metered from.
JsonValue sun_summary(const RenderSettings& settings, const SunDayState& day, const Stats& stats) {
  JsonValue out = sun_summary(settings, day);
  const SkyStats& s = stats.sky;
  if (!s.active) return out;
  JsonValue sky = JsonValue::object();
  sky.set("time_s", s.time_s);
  sky.set("day_of_year", s.day_of_year);
  sky.set("hour", s.hour);
  sky.set("sun_azimuth_deg", static_cast<f64>(s.sun_azimuth_deg));
  sky.set("sun_elevation_deg", static_cast<f64>(s.sun_elevation_deg));
  sky.set("moon_azimuth_deg", static_cast<f64>(s.moon_azimuth_deg));
  sky.set("moon_elevation_deg", static_cast<f64>(s.moon_elevation_deg));
  sky.set("moon_lit", static_cast<f64>(s.moon_lit));
  sky.set("moon_key", s.moon_key);
  sky.set("ev100", static_cast<f64>(s.ev100));
  sky.set("lux", static_cast<f64>(s.lux));
  sky.set("sky_lux", static_cast<f64>(s.sky_lux));
  sky.set("gpu_ms", stats.sky_ms());
  out.set("sky", std::move(sky));
  return out;
}

// The time-lapse block of a summary (terrain_time.h; apps.md, "--time-rate"): the rate and the
// rules it ran under, the game time reached, and per terrain level what its surface stood for, how
// its fields were timed and how long they took, how far any vertex moved in one frame at most, and
// the hash of the last field's bytes — the number two runs of the same frames compare. With rings
// or tiles, a `rings` block counts the re-centres: what they built, kept and uploaded, what the
// last one took on the worker and in frames, and what the slots hold on the device.
JsonValue time_lapse_summary(const TerrainMotion& lapse, const Stats& stats,
                             const GpuScene& scene) {
  if (!lapse.active()) return JsonValue();
  JsonValue out = JsonValue::object();
  out.set("rate", lapse.config().rate);
  out.set("start_rate", lapse.start_rate());
  out.set("rate_changes", lapse.rate_changes());
  out.set("fraction", lapse.config().fraction);
  out.set("min_step_s", lapse.config().min_step_s);
  out.set("max_step_s", lapse.config().max_step_s);
  out.set("lead", lapse.config().lead);
  out.set("wait", lapse.config().wait);
  out.set("game_time_s", lapse.game_time_s());
  out.set("latency_s", lapse.latency_s());
  out.set("max_latency_s", lapse.max_latency_s());
  out.set("latency_frames",
          lapse.config().rate > 0.0 ? lapse.latency_s() / lapse.config().rate * k_frame_hz : 0.0);
  out.set("lag_s", lapse.lag_s());
  out.set("stopped_frames", lapse.stopped_frames());
  out.set("braked_frames", lapse.braked_frames());
  out.set("upload_bytes", stats.terrain_upload_bytes);
  out.set("upload_ms", stats.gpu_terrain_upload);
  JsonValue levels = JsonValue::array();
  for (u32 k = 0; k < lapse.level_count(); ++k) {
    const TerrainMotion::LevelStats s = lapse.level_stats(k);
    JsonValue level = JsonValue::object();
    level.set("spacing_m", s.spacing_m);
    level.set("surface_s", s.surface_s);
    level.set("time_a", s.time_a);
    level.set("time_b", s.time_b);
    level.set("blend", s.blend);
    level.set("padding_m", s.padding_m);
    level.set("evaluated", s.evaluated);
    level.set("installed", s.installed);
    level.set("held", s.held);
    level.set("capped", s.capped);
    level.set("waited", s.waited);
    level.set("late", s.late);
    level.set("max_move_m", s.max_move_m);
    level.set("max_delta_m", s.max_delta_m);
    level.set("max_step_s", s.max_step_s);
    level.set("max_lag_s", s.max_lag_s);
    level.set("last_eval_ms", s.last_eval_ms);
    level.set("mean_eval_ms", s.evaluated > 0 ? s.total_eval_ms / s.evaluated : 0.0);
    level.set("last_turnaround_s", s.last_turnaround_s);
    level.set("max_turnaround_s", s.max_turnaround_s);
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(s.last_hash));
    level.set("last_hash", std::string(hash));
    levels.push_back(std::move(level));
  }
  out.set("levels", std::move(levels));
  if (lapse.has_rings()) {
    const TerrainMotion::RingStats& r = lapse.ring_stats();
    JsonValue rings = JsonValue::object();
    rings.set("layout", ground_layout(lapse));
    // The far levels past the outermost ring (renderer.md, "Ground to the horizon"): 0 for rings.
    rings.set("far_levels", lapse.level_set() != nullptr ? lapse.level_set()->far_levels() : 0u);
    rings.set("chunks", r.chunks_resident);
    rings.set("most_chunks", r.most_chunks);
    rings.set("rebuilds", r.rebuilds);
    rings.set("swaps", r.swaps);
    rings.set("failed", r.failed);
    rings.set("chunks_built", r.chunks_built);
    rings.set("chunks_kept", r.chunks_kept);
    rings.set("chunks_dropped", r.chunks_dropped);
    rings.set("chunks_uploaded", r.chunks_uploaded);
    rings.set("upload_bytes", r.upload_bytes);
    rings.set("upload_frames", r.upload_frames);
    rings.set("last_rebuild_ms", r.last_rebuild_ms);
    rings.set("max_rebuild_ms", r.max_rebuild_ms);
    rings.set("last_pairs_ms", r.last_pairs_ms);
    rings.set("max_pairs_ms", r.max_pairs_ms);
    rings.set("last_upload_frames", r.last_upload_frames);
    rings.set("last_upload_bytes", r.last_upload_bytes);
    rings.set("last_swap_frames", r.last_swap_frames);
    rings.set("last_frozen_frames", r.last_frozen_frames);
    rings.set("arena_peak_share", r.arena_peak_share);
    rings.set("device_bytes", scene.terrain_ring_bytes());
    // How far behind the camera the drawn layout fell, at most (renderer.md, "What a frame waits
    // for"): metres on the ground and frames. Zero offscreen, where every frame waits for it.
    rings.set("max_lag_m", lapse.max_layout_lag_m());
    rings.set("max_lag_frames", lapse.max_layout_lag_frames());
    out.set("rings", std::move(rings));
  }
  return out;
}

// A flight record's `terrain` object (scene::FrameTerrain): the time-lapse as the frame drew it —
// game time, the surface clock's latency, how far behind game time the surface stood and how fast
// it went, and per level the pair and the blend the pool pass drew, the most any vertex moved, and
// whether a late field held or braked it.
std::optional<scene::FrameTerrain> frame_terrain(const TerrainMotion& lapse) {
  if (!lapse.active()) return std::nullopt;
  scene::FrameTerrain out;
  out.game_s = lapse.game_time_s();
  out.latency_s = lapse.latency_s();
  out.lag_s = lapse.lag_s();
  out.speed = lapse.speed_ratio();
  out.rate = lapse.config().rate;
  // The layout as the frame drew it: how far behind the camera, and what its rebuild did.
  const TerrainMotion::FrameLayout& layout = lapse.frame_layout();
  out.layout_lag_m = layout.lag_m;
  out.layout_lag_frames = layout.lag_frames;
  out.chunks_built = layout.built;
  out.chunks_dropped = layout.dropped;
  out.rebuild_ms = layout.rebuild_ms;
  out.chunk_upload_bytes = layout.upload_bytes;
  out.host_ms = layout.host_ms;
  for (u32 k = 0; k < lapse.level_count(); ++k) {
    const TerrainMotion::LevelStats s = lapse.level_stats(k);
    scene::TerrainLevelFrame level;
    level.surface_s = s.surface_s;
    level.time_a = s.time_a;
    level.time_b = s.time_b;
    level.blend = s.blend;
    level.move_m = s.frame_move_m;
    level.held = s.frame_held;
    level.late = s.frame_late;
    level.ahead = s.ahead;
    out.levels.push_back(level);
  }
  return out;
}

// ---- the protocol's spelling ------------------------------------------------------------------

// The schema's strings into the renderer's settings, reporting the first one that is not a
// spelling either host knows. The parsers are settings.h's, so a flag and a protocol field can
// never drift apart. The units are the flags': a budget in MiB or KiB on the wire and bytes in the
// renderer, an angle in degrees and radians — the conversion lives here, in the one place the wire
// meets the module, so `RenderSettings` never has to say what unit a caller was thinking in.
bool read_protocol_settings(const protocol::RenderSettings& in, RenderRequest& out,
                            std::string& error) {
  RenderSettings s;
  if (!parse_raster_mode(in.raster, s.raster)) {
    error = "raster must be direct, hw, vertex, sw, auto, or rt; got '" + in.raster + "'";
    return false;
  }
  if (!parse_shadow_mode(in.shadows, s.shadows)) {
    error = "shadows must be off, rt, csm, or auto; got '" + in.shadows + "'";
    return false;
  }
  if (!parse_view_mode(in.view, s.view_mode)) {
    error =
        "view must be id, tri, depth, shaded, normals, uv, shadow, albedo, occlusion, or detail; "
        "got '" +
        in.view + "'";
    return false;
  }
  if (!parse_deform_mode(in.deform, s.deform, s.deform_kind)) {
    error = "deform must be none, identity, wave, or lattice; got '" + in.deform + "'";
    return false;
  }
  if (!parse_view_layout(in.views, s.views)) {
    error = "views must be single, surround3, or panini; got '" + in.views + "'";
    return false;
  }
  if (in.side_yaw_deg < 0.0f || in.side_yaw_deg > 80.0f) {
    error = "side_yaw_deg must be within 0..80";
    return false;
  }
  if (in.peripheral_lod < 1.0f) {
    error = "peripheral_lod must be at least 1";
    return false;
  }
  if (in.panini_d < 0.0f) {
    error = "panini_d must not be negative";
    return false;
  }
  if (in.shadow_cascades < 1 || in.shadow_cascades > 4) {
    error = "shadow_cascades must be within 1..4";
    return false;
  }
  if (in.shadow_map < 64 || in.shadow_map > k_max_shadow_map) {
    error = "shadow_map must be within 64..4096 texels";
    return false;
  }
  if (!(in.shadow_distance >= 0.0f)) {
    error = "shadow_distance must not be negative";
    return false;
  }
  // The bound keeps the kibibyte product inside the u32 the renderer's field is.
  if (in.upload_budget_kib > 1024u * 1024u) {
    error = "upload_budget_kib must be at most 1048576 (1 GiB)";
    return false;
  }
  if (in.deform_pool_mib > 65536u) {
    error = "deform_pool_mib must be at most 65536 (64 GiB)";
    return false;
  }
  if (in.page_budget_pct > 100u) {
    error = "page_budget_pct must be within 0..100";
    return false;
  }
  if (in.sun_azimuth_deg.has_value() != in.sun_elevation_deg.has_value()) {
    error = "sun_azimuth_deg and sun_elevation_deg go together";
    return false;
  }
  if (in.sun_elevation_deg.has_value() &&
      (!(*in.sun_elevation_deg >= -90.0f && *in.sun_elevation_deg <= 90.0f) ||
       !std::isfinite(*in.sun_azimuth_deg))) {
    error = "the sun's elevation must be within -90..90 degrees, its azimuth finite";
    return false;
  }
  if (!(in.time_rate >= 0.0) || in.time_rate > 1e9) {
    error = "time_rate must be within 0..1e9 game seconds per real second";
    return false;
  }
  if (!(in.exposure_ev >= -30.0f && in.exposure_ev <= 30.0f) ||
      (in.exposure_ev100.has_value() &&
       !(*in.exposure_ev100 >= -30.0f && *in.exposure_ev100 <= 30.0f))) {
    error = "exposure_ev and exposure_ev100 are stops within -30..30";
    return false;
  }
  if (in.terrain_far_levels.has_value() && *in.terrain_far_levels > k_max_far_levels) {
    error = "terrain_far_levels must be within 0.." + std::to_string(k_max_far_levels);
    return false;
  }
  for (const std::string& entry : in.morph) {
    if (entry.find('=') == std::string::npos) {
      error = "a morph entry is <name|index>=<weight>; got '" + entry + "'";
      return false;
    }
  }
  s.stream = in.stream;
  s.page_budget_bytes = u64{in.page_budget_mib} * 1024u * 1024u;
  s.upload_budget_bytes = in.upload_budget_kib * 1024u;
  s.lod_px = in.lod_px;
  s.sw_px = in.sw_px;
  s.cull = in.cull;
  s.occlusion = in.occlusion;
  s.cone = in.cone;
  s.shadow_casters = in.shadow_casters;
  s.shadow_cascades = in.shadow_cascades;
  s.shadow_map = in.shadow_map;
  s.shadow_distance = in.shadow_distance;
  s.lights = in.lights;
  s.orbit_lights = in.orbit_lights;
  s.sun_azimuth_deg = in.sun_azimuth_deg;
  s.sun_elevation_deg = in.sun_elevation_deg;
  s.exposure_ev = in.exposure_ev;
  s.exposure_ev100 = in.exposure_ev100;
  s.dither = in.dither;
  s.deform_amplitude = in.deform_amplitude;
  s.deform_pool_kib = in.deform_pool_mib * 1024u;
  s.static_shape_kib = in.static_shape_kib;
  s.rt_templates = in.rt_templates;
  s.rt_budget_mib = in.rt_budget_mib;
  s.share_textures = in.share_textures;
  s.time_rate = in.time_rate;
  s.terrain_rings = in.terrain_rings;
  s.terrain_tiles = in.terrain_tiles;
  // Null is the tunable's count, as engine-view without `--terrain-far` (-1 in the settings).
  s.terrain_far_levels =
      in.terrain_far_levels.has_value() ? static_cast<i32>(*in.terrain_far_levels) : -1;
  s.side_yaw = radians(in.side_yaw_deg);
  s.panini_d = in.panini_d;
  s.peripheral_lod = in.peripheral_lod;
  out.settings = std::move(s);
  out.page_budget_pct = in.page_budget_pct;
  out.morph.clear();
  for (const std::string& entry : in.morph)
    out.morph.push_back(entry);
  return true;
}

bool read_protocol_clock(const protocol::RenderClock& in, ClockRequest& out, std::string& error) {
  if (in.time_of_day.has_value() && !(*in.time_of_day >= 0.0 && *in.time_of_day < 24.0)) {
    error = "clock.time_of_day is an hour within 0..24 (17.5 is 17:30)";
    return false;
  }
  if (in.sun_rate.has_value() && !(*in.sun_rate >= 0.0 && *in.sun_rate <= 1.0e7)) {
    error = "clock.sun_rate must be within 0..1e7 game seconds per real second";
    return false;
  }
  out.time_of_day_h = in.time_of_day;
  out.sun_rate = in.sun_rate;
  return true;
}

Camera read_protocol_camera(const SceneData& scene, const protocol::RenderCamera* camera,
                            const protocol::RenderOrbit* orbit) {
  if (camera != nullptr) {
    // `RenderCamera.position` and `target` are `worldpos`es (version 2, ADR-0053): the request's
    // f64 numbers are the camera's, with nothing between them and the frame's eye.
    Camera out;
    out.position = camera->position;
    out.target = camera->target;
    out.fov_y = radians(camera->fov_deg);
    out.znear = camera->znear > 0.0f ? camera->znear : 0.01f * scene.radius;
    return out;
  }
  const protocol::RenderOrbit o = orbit != nullptr ? *orbit : protocol::RenderOrbit{};
  return orbit_camera_at(scene.center, scene.radius, o.distance, radians(o.yaw_deg),
                         radians(o.pitch_deg));
}

}  // namespace engine::renderer
