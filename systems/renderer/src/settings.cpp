#include <core/log/log.h>
#include <domain/gfx/requirements.h>
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
    case ShadowMode::Cascaded: return "csm";
  }
  return "?";
}

bool parse_shadow_mode(std::string_view text, ShadowMode& out) noexcept {
  if (text == "off") {
    out = ShadowMode::Off;
  } else if (text == "rt") {
    out = ShadowMode::RayTraced;
  } else if (text == "csm") {
    out = ShadowMode::Cascaded;
  } else if (text == "auto") {
    out = ShadowMode::Auto;
  } else {
    return false;
  }
  return true;
}

const char* resolved_shadow_name(const ResolvedSettings& resolved) noexcept {
  return resolved.shadows ? "rt" : (resolved.csm ? "csm" : "off");
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
    case 6: return "shadow";
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
  } else if (text == "shadow") {
    out = 6;
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
  out.shadows = (s.shadows == ShadowMode::RayTraced || s.shadows == ShadowMode::Auto) &&
                shadow_device && shadow_mode;
  // **Cascaded shadow maps** are the baseline tier's shadow (04 §4.4, the 2026-09-23 direction
  // note). They are drawn by the cull pass and the rasterizers from the light and read by the
  // resolve, so they need a path that rasterizes the picture into the visibility buffer and
  // resolves it: hw (the mesh path, or the vertex path it fell back to) and vertex. The ray path
  // traces its picture and has its own shadows, and the direct, sw and auto paths are refused for
  // the reason rt is: `auto` resolves to maps exactly where it cannot trace.
  const bool map_mode = s.raster == RasterMode::Hardware || out.vertex_path;
  out.csm = map_mode &&
            (s.shadows == ShadowMode::Cascaded || (s.shadows == ShadowMode::Auto && !out.shadows));
  // An explicit `rt` on a device that cannot trace is not a warning here: `check_availability`
  // refuses it, and the host prints the device's own verdict (`unavailable_reason`).
  if (s.shadows == ShadowMode::RayTraced && !out.shadows && shadow_device) {
    ENGINE_LOG_WARN(log_renderer, "ray-traced shadows off",
                    log::field("reason", "the direct, sw, and auto paths do not build them"));
  } else if (s.shadows == ShadowMode::Auto && out.csm) {
    ENGINE_LOG_INFO(log_renderer, "cascaded shadow maps instead of ray-traced shadows",
                    log::field("reason", "no cluster acceleration structures or ray queries"));
  } else if (s.shadows == ShadowMode::Cascaded && !out.csm) {
    ENGINE_LOG_WARN(log_renderer, "cascaded shadow maps off",
                    log::field("reason", "the direct, sw, auto, and rt paths do not draw them"));
  }
  if (out.csm) {
    const u32 cascades = s.shadow_cascades;
    s.shadow_cascades = cascades < 1                            ? 1u
                        : cascades > gfx::k_max_shadow_cascades ? gfx::k_max_shadow_cascades
                                                                : cascades;
    // A power of two keeps the atlas's texel grid exact in the resolve's arithmetic; the size
    // is clamped to what one atlas row of four cascades can hold on any device.
    u32 map = 64;
    while (map < s.shadow_map && map < k_max_shadow_map)
      map <<= 1;
    if (map != s.shadow_map) {
      ENGINE_LOG_INFO(log_renderer, "shadow map size rounded", log::field("asked", s.shadow_map),
                      log::field("texels", map));
    }
    s.shadow_map = map;
    if (!(s.shadow_distance >= 0.0f)) s.shadow_distance = 0.0f;
    out.shadow_cascades = s.shadow_cascades;
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
  if (out.csm && !s.cull) {
    s.cull = true;  // every cascade is the cull pass's output, run from the light
    ENGINE_LOG_WARN(log_renderer, "culling forced on with cascaded shadow maps");
  }
  // The deformed-vertex pool pass runs when the settings deform every instance *or* when the
  // scene has a skinned one, and those are the same pass: skinning is `deform.slang`'s third
  // kind, not a path beside it. Deciding it here rather than at each use is what keeps
  // renderer.md's "every device- and scene-driven override lives in one function" true.
  out.deform_pass = s.deform || (scene != nullptr && (scene->skinned() || scene->morphed()));
  if (out.deform_pass && !s.cull) {
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
  // ---- geometry streaming (04 §4.3 step 3, §4.9) ----------------------------------------------
  //
  // Streaming is refused rather than half-applied, and each of the four refusals is a statement
  // about what the cut means. Without a page table there is nothing to stream. Without the cull
  // pass there is no cut, and the drawing rule's fallback clause *is* the cut. A **deformed or
  // skinned** instance reads the deformed-vertex pool at a scene-wide vertex index, and a streamed
  // scene's vertices live at slot-relative ones, so the two index spaces disagree — joining them is
  // a change to the pool's allocation, which is E25's open suballocation item and not this one.
  // And **cluster templates** are built once at load from every cluster's float positions, which a
  // streamed scene does not hold: a template has to be built when its page arrives and dropped when
  // it leaves, which is a second residency problem with its own budget.
  if (s.stream) {
    const char* refused = nullptr;
    if (scene != nullptr && !scene->paged()) {
      refused = "the scene carries no page table; load it with SceneDesc::stream";
    } else if (out.deform_pass) {
      refused = "a deformed or skinned instance indexes the vertex pool scene-wide";
    }
    if (refused != nullptr) {
      s.stream = false;
      ENGINE_LOG_WARN(log_renderer, "geometry streaming off", log::field("reason", refused));
    } else {
      if (!s.cull) {
        s.cull = true;
        ENGINE_LOG_WARN(log_renderer, "culling forced on with geometry streaming",
                        log::field("reason", "the drawing rule falls back to the LOD cut"));
      }
      if (s.rt_templates) {
        s.rt_templates = false;
        ENGINE_LOG_WARN(
            log_renderer, "cluster templates off with geometry streaming",
            log::field("reason", "a template is built from a page's positions when it arrives"));
      }
    }
  }
  out.stream = s.stream;
  // **Shadow casters.** The cone test drops a cluster that faces away from the *camera*, and the
  // shadow rays trace what the cull pass kept, so without this a card seen from behind casts no
  // shadow and a lit box seen from its dark side loses part of its own (docs/subsystems/
  // geometry.md, "Normal cones"). The pass keeps those clusters instead, in a run no rasterizer
  // reads, and the chain builds them non-opaque so the primary rays pass through them. Only where
  // it means something: shadows traced, the cone test on, and build records rather than template
  // instantiations — an instantiated cluster takes its flags from its template, which is opaque,
  // so a caster could not be kept out of a primary ray. Templates are for deforming meshes, whose
  // instances are never cone-tested at all, so what they give up is a rigid instance's casters.
  out.casters = out.shadows && s.cone && s.shadow_casters && !s.rt_templates;
  // **A morphed mesh is not streamed**, and the reason is said out loud rather than discovered as
  // a wrong picture. A page's payload is the cluster's positions, attributes, triangles and
  // bindings; the morph stream is keyed by cluster too, but its slice directory indexes a
  // scene-wide delta array whose offsets a page copy would have to patch the way it patches
  // `ClusterDesc::vertex_offset`. That is the same work and belongs with it. Until then the whole
  // stream is resident (`ClusterFileReader`'s resident list says so), and a scene that asks for
  // both gets the one that draws the right picture.
  if (out.stream && scene != nullptr && scene->morphed()) {
    out.stream = false;
    ENGINE_LOG_WARN(log_renderer, "streaming refused",
                    log::field("reason", "the scene has morph channels, which are not paged yet"));
  }
  // **The vertex path draws a culled cut indexed** (cluster_vertex_indexed.slang): one draw of the
  // cut's own triangles, whose indices let the vertex cache share a cluster's vertices. Its
  // fragment stage reads SV_PrimitiveID, which a vertex pipeline only has with geometryShader, and
  // an index carries the visible slot above the local vertex, which past 65,536 clusters in a run
  // needs fullDrawIndexUint32. Without either, or without the cull pass to allocate from, the path
  // draws every cluster's whole capacity instead — the same picture, one vertex invocation per
  // corner of every triangle it could hold (gfx.md, "Baseline tier").
  out.vertex_indexed =
      out.vertex_path && s.cull && features.geometry_shader && features.full_draw_index_uint32;
  out.settings = s;
  out.settings.stream = out.stream;
}

RenderAvailability check_availability(const ResolvedSettings& resolved,
                                      const gfx::DeviceFeatures& features) noexcept {
  if (!features.buffer_int64_atomics) return RenderAvailability::NoVisibilityBuffer;
  // An explicit request the device cannot meet is refused rather than dropped. `--raster rt`
  // always was; `--shadows rt` is too, because a picture without the shadows somebody asked for
  // reads as a shadowing bug, and the adapter report already says the device cannot trace. `auto`
  // is the request that degrades quietly. The same flag on a *path* that builds no structures
  // (direct, sw, auto) is a settings conflict rather than a device one, and stays a warning in
  // `resolve_settings`.
  const bool can_trace = features.cluster_acceleration_structure && features.ray_query;
  if (!can_trace && (resolved.ray_path || resolved.settings.shadows == ShadowMode::RayTraced)) {
    return RenderAvailability::NoAccelerationStructures;
  }
  return RenderAvailability::Ok;
}

std::string unavailable_reason(RenderAvailability availability, const gfx::Device& device) {
  std::string text(device.adapter().name);
  if (availability == RenderAvailability::NoAccelerationStructures) {
    const std::string sentence = gfx::ray_tracing_degradation(device.caps());
    if (!sentence.empty()) return text + " cannot trace rays: " + sentence;
  }
  return text + " " + availability_message(availability);
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
