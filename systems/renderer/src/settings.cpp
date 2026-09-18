#include <core/log/log.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

namespace engine::renderer {

ENGINE_LOG_CATEGORY_DEFINE(log_renderer, "renderer");

const char* raster_name(RasterMode mode) noexcept {
  switch (mode) {
    case RasterMode::Direct: return "direct";
    case RasterMode::Hardware: return "hw";
    case RasterMode::Software: return "sw";
    case RasterMode::Auto: return "auto";
    case RasterMode::Vertex: return "vertex";
    case RasterMode::RayTrace: return "rt";
  }
  return "?";
}

bool parse_raster_mode(std::string_view text, RasterMode& out) noexcept {
  if (text == "direct") {
    out = RasterMode::Direct;
  } else if (text == "hw") {
    out = RasterMode::Hardware;
  } else if (text == "sw") {
    out = RasterMode::Software;
  } else if (text == "auto") {
    out = RasterMode::Auto;
  } else if (text == "vertex") {
    out = RasterMode::Vertex;
  } else if (text == "rt") {
    out = RasterMode::RayTrace;
  } else {
    return false;
  }
  return true;
}

const char* shadow_name(ShadowMode mode) noexcept {
  switch (mode) {
    case ShadowMode::Auto: return "auto";
    case ShadowMode::Off: return "off";
    case ShadowMode::RayTraced: return "rt";
  }
  return "?";
}

bool parse_shadow_mode(std::string_view text, ShadowMode& out) noexcept {
  if (text == "off") {
    out = ShadowMode::Off;
  } else if (text == "rt") {
    out = ShadowMode::RayTraced;
  } else if (text == "auto") {
    out = ShadowMode::Auto;
  } else {
    return false;
  }
  return true;
}

const char* deform_name(const RenderSettings& settings) noexcept {
  if (!settings.deform) return "none";
  switch (settings.deform_kind) {
    case gfx::k_deform_wave: return "wave";
    case gfx::k_deform_lattice: return "lattice";
    default: return "identity";
  }
}

bool parse_deform_mode(std::string_view text, bool& deform, u32& kind) noexcept {
  if (text == "none") {
    deform = false;
  } else if (text == "identity") {
    deform = true;
    kind = gfx::k_deform_identity;
  } else if (text == "wave") {
    deform = true;
    kind = gfx::k_deform_wave;
  } else if (text == "lattice") {
    deform = true;
    kind = gfx::k_deform_lattice;
  } else {
    return false;
  }
  return true;
}

const char* view_mode_name(u32 mode) noexcept {
  switch (mode) {
    case 0: return "id";
    case 1: return "tri";
    case 2: return "depth";
    case 3: return "shaded";
    case 4: return "normals";
    case 5: return "uv";
    default: return "?";
  }
}

bool parse_view_mode(std::string_view text, u32& out) noexcept {
  if (text == "id") {
    out = 0;
  } else if (text == "tri") {
    out = 1;
  } else if (text == "depth") {
    out = 2;
  } else if (text == "shaded") {
    out = 3;
  } else if (text == "normals") {
    out = 4;
  } else if (text == "uv") {
    out = 5;
  } else {
    return false;
  }
  return true;
}

const char* view_layout_name(ViewLayout layout) noexcept {
  switch (layout) {
    case ViewLayout::Single: return "single";
    case ViewLayout::Surround3: return "surround3";
    case ViewLayout::Panini: return "panini";
  }
  return "?";
}

bool parse_view_layout(std::string_view text, ViewLayout& out) noexcept {
  if (text == "single") {
    out = ViewLayout::Single;
  } else if (text == "surround3") {
    out = ViewLayout::Surround3;
  } else if (text == "panini") {
    out = ViewLayout::Panini;
  } else {
    return false;
  }
  return true;
}

// The order here is the order the decisions depend on each other in, and changing it changes
// pictures: the mesh-shader fallback decides which path runs, the path decides whether shadows
// can be traced at all, and shadows decide whether two-pass occlusion culling may run (the
// acceleration structures are built from one visible list, and pass 2's survivors are in a
// second one). Everything after that only ever forces a switch *on*, never off.
void resolve_settings(const RenderSettings& requested, const gfx::DeviceFeatures& features,
                      const SceneData* scene, ResolvedSettings& out) {
  out = ResolvedSettings{};
  RenderSettings s = requested;
  if (!s.cull && s.raster == RasterMode::Auto) s.raster = RasterMode::Hardware;
  if (!features.mesh_shader && (s.raster == RasterMode::Direct ||
                                s.raster == RasterMode::Hardware || s.raster == RasterMode::Auto)) {
    s.raster = RasterMode::Vertex;  // the baseline tier
  }
  out.direct = s.raster == RasterMode::Direct;
  out.vertex_path = s.raster == RasterMode::Vertex;
  out.ray_path = s.raster == RasterMode::RayTrace;

  // Ray-traced shadows want the same per-frame structures --raster rt builds, so they need the
  // same device and the same cull output. The resolve is where they are shaded, which rules out
  // the direct path, and they are built from one visible list, which rules out the modes that
  // produce more than one: the software split's second list, and occlusion culling's second
  // pass.
  const bool shadow_device = features.cluster_acceleration_structure && features.ray_query;
  const bool shadow_mode = out.ray_path || s.raster == RasterMode::Hardware || out.vertex_path;
  out.shadows = s.shadows != ShadowMode::Off && shadow_device && shadow_mode;
  if (s.shadows == ShadowMode::RayTraced && !out.shadows) {
    ENGINE_LOG_WARN(
        log_renderer, "ray-traced shadows off",
        log::field("reason", !shadow_device ? "no cluster acceleration structures or ray queries"
                                            : "the direct, sw, and auto paths do not build them"));
  } else if (s.shadows == ShadowMode::Auto && !out.shadows && shadow_mode) {
    ENGINE_LOG_INFO(log_renderer, "ray-traced shadows off",
                    log::field("reason", "no cluster acceleration structures or ray queries"));
  }
  out.rt_chain = out.ray_path || out.shadows;
  // More than one view needs the cull pass: every view's draw reads its own run of the visible
  // list, and there is no run without one. The direct path draws one view and one only, because
  // it writes color rather than a visibility buffer and has nowhere to put a second rectangle.
  out.view_count = view_count_of(s.views);
  if (out.direct && s.views != ViewLayout::Single) {
    s.views = ViewLayout::Single;
    out.view_count = 1;
    ENGINE_LOG_WARN(
        log_renderer, "one view only on the direct path",
        log::field("reason", "it draws straight to color and has no visibility buffer"));
  }
  if (s.views != ViewLayout::Single && !s.cull) {
    s.cull = true;
    ENGINE_LOG_WARN(log_renderer, "culling forced on with more than one view");
  }
  out.occlusion = s.occlusion && s.cull && !out.shadows &&
                  (s.raster == RasterMode::Hardware || out.vertex_path);
  if (out.shadows && s.occlusion && !out.ray_path) {
    ENGINE_LOG_INFO(log_renderer, "two-pass occlusion culling off with ray-traced shadows",
                    log::field("reason", "the structures are built from one visible list"));
  }
  if (out.rt_chain && !s.cull) {
    s.cull = true;  // the ray tracing geometry is built from the cull output
    ENGINE_LOG_WARN(log_renderer, "culling forced on", log::field("raster", raster_name(s.raster)),
                    log::field("shadows", out.shadows));
  }
  if (s.deform && !s.cull) {
    s.cull = true;  // the pool pass walks the cull's visible list, which is the point
    ENGINE_LOG_WARN(log_renderer, "culling forced on with a deformed scene");
  }
  if (s.rt_templates && !out.rt_chain) {
    s.rt_templates = false;
    ENGINE_LOG_WARN(log_renderer, "cluster templates ignored without ray tracing");
  }
  if (scene != nullptr) {
    if (scene->instances.size() > 1 && !s.cull) {
      s.cull = true;  // a scene draws through the cull pass; there is no direct draw of one
      ENGINE_LOG_WARN(log_renderer, "culling forced on with more than one instance");
    }
    // A mesh laid out in streaming pages is ordered coarse to fine, so its leaves are the *last*
    // clusters rather than the first and the direct draw's "clusters 0..leaf_count" would draw
    // the root. The cull pass tests every cluster on its own and does not care which end they
    // are at.
    if (!s.cull && !scene->lod.lod.empty() && scene->lod.lod[0].level != 0) {
      s.cull = true;
      ENGINE_LOG_WARN(log_renderer, "culling forced on for a paged mesh: its leaves are not first");
    }
  }
  out.settings = s;
}

RenderAvailability check_availability(const ResolvedSettings& resolved,
                                      const gfx::DeviceFeatures& features) noexcept {
  if (!features.buffer_int64_atomics) return RenderAvailability::NoVisibilityBuffer;
  if (resolved.ray_path && !(features.cluster_acceleration_structure && features.ray_query)) {
    return RenderAvailability::NoAccelerationStructures;
  }
  return RenderAvailability::Ok;
}

const char* availability_message(RenderAvailability availability) noexcept {
  switch (availability) {
    case RenderAvailability::Ok: return "";
    case RenderAvailability::NoVisibilityBuffer: return "has no 64-bit buffer atomics";
    case RenderAvailability::NoAccelerationStructures:
      return "has no cluster acceleration structures or ray queries";
  }
  return "";
}

}  // namespace engine::renderer
