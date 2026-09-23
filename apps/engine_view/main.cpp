// engine-view: the window on `systems/renderer`. Everything that draws lives in that module now
// (docs/subsystems/renderer.md); this file is the command line over it — the flags, the window
// and its swapchain, the event loop, the capture, and the one JSON line of statistics that
// scripts and agents read. The renderer itself never sees a surface: engine-view acquires a
// swapchain image, hands it in as the frame's color target with the two semaphores, and
// presents it afterwards, which is the only difference between what this draws and what
// engine-host's `render.capture` draws offscreen.
//
// `--frames N --capture out.png` renders N frames and writes the last one as a PNG, then prints
// the statistics line on exit, so there is no need for a human at the window. `--mesh` takes a
// glTF file or a `.clusters` container; a glTF is looked up in the derived-data cache first and
// built into it on a miss. Shaders come from the build's manifest when it is found and
// recompile when their sources change.
//
// Exit codes: 0 ok; 1 runtime error; 2 usage; 3 unavailable (no display, no Vulkan device, no
// mesh shaders, or no presentation support), which tests treat as a skip.
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/log/log.h>
#include <core/math/math.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/swapchain.h>
#include <domain/gfx/vulkan.h>
#include <foundation/bench/machine_state.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <foundation/window/window.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/page_source.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <engine_build_stamp.h>

#if ENGINE_VIEW_ANIMATION
#include "anim_lod.h"

#include <core/ids/id128.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/world_commands.h>
#include <domain/sim/tiers.h>
#include <systems/animation/animation.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

using namespace engine;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_view, "view");

// clang-format off
constexpr const char* k_usage =
    "usage: engine-view [--width <px>] [--height <px>] [--frames <n>] [--capture <file.png>]\n"
    "                   [--no-vsync] [--adapter <index>] [--validation] [--grid <n>] [--log <spec>]\n"
    "                   [--shaders <manifest.json>] [--lod <px>] [--no-cull] [--no-occlusion] [--no-cone]\n"
    "                   [--raster direct|hw|vertex|sw|auto|rt] [--sw-px <px>] [--view <mode>] [--orbit <d>]\n"
    "                   [--mesh <file.gltf|file.glb|file.clusters>] [--scene <file.json>]\n"
    "                   [--grid-instances <n>] [--no-cache] [--ddc <dir>] [--no-lights]\n"
    "                   [--deform none|identity|wave|lattice] [--deform-amplitude <a>] [--rt-templates]\n"
    "                   [--stream] [--page-budget <MiB>] [--upload-budget <KiB>]\n"
    "                   [--shadows off|rt] [--views single|surround3|panini] [--side-yaw <deg>]\n"
    "                   [--panini-d <d>] [--peripheral-lod <mult>]\n"
    "                   [--animate [clip]] [--anim-speed <x>] [--anim-lod on|off]\n"
    "                   [--anim-lod-scale <x>]\n"
    "                   [--morph <name|index>=<weight>] [--morph-animate [clip]]\n"
    "                   [--static-shape-kib <n>]\n"
    "                   [--reference <spp>] [--bounces <n>] [--finest] [--spp-batch <n>]\n"
    "       engine-view --version    the commit this binary was built from, as one JSON line\n"
    "\n"
    "  --frames <n>     render n frames, then exit (0: until the window closes)\n"
    "  --capture <png>  write the last frame as a PNG (implies --frames 60 when unset)\n"
    "  --grid <n>       heightfield resolution, n x n vertices (default 257)\n"
    "  --procedural <n> which scene an absent --mesh builds: heightfield (default), or\n"
    "                   shredded-atlas, the UV-atlas LOD stress fixture (geometry.md)\n"
    "  --uv-seams <r>   what a LOD collapse may do at a UV atlas island edge: none, protect\n"
    "                   (default), or lock. --normal-seams <r> is the same at a hard edge\n"
    "  --uv-weight <n>  UV weight in the simplifier's error metric, in thousandths (default\n"
    "                   500); --normal-weight <n> the same for normals (default 0). All four\n"
    "                   are part of the derived-data cache key\n"
    "  --mesh <file>    render a mesh instead of the heightfield: a glTF 2.0 or GLB file (one\n"
    "                   cluster DAG per primitive; materials with their base-color,\n"
    "                   metallic-roughness, and normal textures from the file), or a .clusters\n"
    "                   container that already holds one\n"
    "  --scene <json>   {\"meshes\":[{\"path\":\"...\"}],\"instances\":[{\"mesh\":0,\"translation\":[x,y,z],\n"
    "                   \"rotation\":[x,y,z,w],\"scale\":[x,y,z]}]}; paths are relative to the file\n"
    "  --grid-instances <n>  place the loaded mesh n x n times with varied rotation and scale\n"
    "  --no-cache       always build a glTF from source; do not read or write ddc/clusters\n"
    "  --ddc <dir>      the derived-data root (default: <repo>/ddc, found beside AGENTS.md)\n"
    "  --lod <px>       screen-space error threshold in pixels for LOD selection (default 1)\n"
    "  --no-cull        draw every leaf cluster; no GPU culling or LOD selection\n"
    "  --no-occlusion   skip two-pass occlusion culling (hw mode only; on by default)\n"
    "  --no-cone        skip backface culling of clusters by their normal cones (on by default)\n"
    "  --no-lights      only the sun and the sky; no orbiting point lights (they are on by default)\n"
    "  --shadows <how>  off, or rt: every light in the resolve casts a ray-traced shadow against\n"
    "                   the structures the frame built from its own visible list. The default is\n"
    "                   rt where the device has cluster acceleration structures and ray queries.\n"
    "                   In a raster mode the frame runs the acceleration structure chain as well,\n"
    "                   which turns two-pass occlusion culling off (one visible list to build from)\n"
    "  --raster <mode>  direct: mesh shaders to color with a depth buffer; hw (default), vertex, sw,\n"
    "                   auto: the visibility buffer through mesh shaders, a vertex shader (the\n"
    "                   baseline tier, chosen automatically without mesh shaders), software, or\n"
    "                   both split by size; rt: ray queries against cluster acceleration structures\n"
    "                   built every frame from the cull output (NVIDIA RTX only)\n"
    "  --sw-px <px>     auto mode: clusters narrower than this go to the software rasterizer (32)\n"
    "  --view <mode>    id, tri, depth, shaded (default: materials with vertex normals, textures,\n"
    "                   and normal maps under a sun), normals, uv\n"
    "  --orbit <d>      orbit at a fixed distance instead of breathing between 8 and 36 units;\n"
    "                   distances scale with the scene radius (10 for the heightfield)\n"
    "  --deform <mode>  deform every instance through the per-frame deformed-vertex pool\n"
    "                   (experiment E25): identity writes the rest pose, wave displaces along the\n"
    "                   vertex normal, lattice runs a 3x3x3 cage over the mesh's bounds\n"
    "  --deform-amplitude <a>  displacement as a fraction of the mesh's bounds (default 0.02)\n"
    "  --deform-pool-mib <n>  the deformed-vertex pool's budget in MiB (default 8). The pool is\n"
    "                   suballocated per frame from the visible list, so this bounds what the\n"
    "                   frame's cut may deform; pairs that do not fit draw their rest pose and are\n"
    "                   counted in deform_overflow_entries. Clamped down to what a block per\n"
    "                   instance's whole mesh would have taken\n"
    "  --stream         geometry pages stream on demand (04 §4.3 step 3, §4.9): the GPU holds a\n"
    "                   budgeted subset of the scene's cluster pages, the cull pass draws what is\n"
    "                   resident and asks for what it is missing, and the picture converges. The\n"
    "                   summary's \"streaming\" block says what it cost. Refuses --deform, a\n"
    "                   skinned scene, and --rt-templates, each with a reason on stderr\n"
    "  --page-budget <MiB>    the residency budget over the scene's page bytes; 0 (the default) is\n"
    "                   every page, which is streaming with nothing to evict. Implies --stream\n"
    "  --upload-budget <KiB>  page payload one frame may copy into the pool (default 256 KiB); a\n"
    "                   value under the largest page is raised to it. Implies --stream\n"
    "  --page-budget-pct <n>  the same budget as a percentage of this scene's page bytes, which\n"
    "                   is how a budget is thought about and cannot be said before the load.\n"
    "                   Overrides --page-budget. Implies --stream\n"
    "  --page-source <s>  where a streamed page's bytes come from: auto (the default: the meshes'\n"
    "                   .clusters containers when every mesh has one, host memory otherwise),\n"
    "                   file (refuse to run if they do not), or host (the SceneData the load\n"
    "                   produced, which is what streaming saved no host memory with)\n"
    "  --fly <a> <b> <n>  a scripted fly-in over n frames, from a mesh radii to b, interpolated\n"
    "                   geometrically because what a LOD cut answers to is the ratio of\n"
    "                   distances. Overrides --orbit and sets --frames when it is not set\n"
    "  --rt-templates   --raster rt: build one cluster template per cluster at load and\n"
    "                   instantiate the cut's templates each frame instead of rebuilding the CLAS\n"
    "  --animate [clip] play a skinned glTF's animation: the skin becomes a skeleton, a clip is\n"
    "                   ticked at the fixed step, and every instance is skinned through the same\n"
    "                   deformed-vertex pool --deform uses. The optional value names the clip by\n"
    "                   name or by index among the skin's clips; with none, the first one plays.\n"
    "                   With --grid-instances every copy gets its own phase offset, so a crowd is\n"
    "                   not in lockstep. A scene file says it per instance, in an \"animation\"\n"
    "                   block: {\"clip\":\"Run\",\"speed\":1.5,\"phase\":0.4}\n"
    "  --morph <n>=<w>  play morph channel <n> at weight <w> through the deform chain's **static\n"
    "                   shape** stage, which is cached per instance and costs nothing per frame.\n"
    "                   <n> is the channel's name (glTF's extras.targetNames) or its index.\n"
    "                   Repeatable; an unknown name is a usage error rather than a silent zero\n"
    "  --morph-animate [clip]  play a clip's weight tracks through the **pose** stage, per frame,\n"
    "                   on top of whatever --morph set. Channels the clip does not name keep the\n"
    "                   asset's own default weights. The clips come from the mesh file, with or\n"
    "                   without a skin; the optional value names one by name or by index among\n"
    "                   the clips that have weight tracks, and with none the first one plays\n"
    "  --static-shape-kib <n>  the budget for the static shape caches (default 4096). An instance\n"
    "                   that gets none runs its static stage every frame over the cut: the same\n"
    "                   picture, more work, and the summary says how many were cached\n"
    "  --anim-speed <x> multiply every animated instance's playback rate (default 1)\n"
    "  --anim-lod on|off  let the camera decide each character's animation tier (default on):\n"
    "                   an observer per view of the layout, the side monitors weighted below the\n"
    "                   centre, sim::TierAssignment over the animated instances' positions, and\n"
    "                   the changes through the animation capability's own hooks. Off animates\n"
    "                   every instance at LOD0, which is what every build before this one did\n"
    "  --anim-lod-scale <x>  multiply the tier band boundaries (default 1): 2 puts everything one\n"
    "                   band nearer and animates the crowd finer, 0.5 coarser\n"
    "  --views <how>    single (default: one rectilinear view over the whole target); surround3:\n"
    "                   three views with per-monitor off-axis frusta, the target divided in three;\n"
    "                   panini: one view rendered rectilinear into an oversampled source and\n"
    "                   resampled with a Panini projection (experiment E9, plan 04 §4.6)\n"
    "  --side-yaw <d>   surround3: degrees the side monitors are turned inward (default 0, flat)\n"
    "  --panini-d <d>   panini: 0 is the ordinary rectilinear picture, 1 the classic Pannini\n"
    "                   (default 1); larger d compresses the periphery harder and needs a wider\n"
    "                   source, which the summary reports as the oversampling factor\n"
    "  --peripheral-lod <m>  multiply the LOD pixel threshold outside the attention region by m:\n"
    "                   surround3's side monitors, which a surround player reads peripherally\n"
    "  --reference <n>  render the reference path tracer at n samples per pixel instead of the\n"
    "                   real-time frame (plan 04 §4.8): no window, no swapchain, one converged\n"
    "                   picture written by --capture and one JSON summary line. Needs a device\n"
    "                   with cluster acceleration structures and ray queries, and settings that\n"
    "                   build them (the default --shadows auto does)\n"
    "  --bounces <n>    --reference: scattering events after the primary hit (default 3). 1 is\n"
    "                   direct lighting plus one bounce, which is what the resolve approximates\n"
    "  --finest         --reference: build the acceleration structures from the finest clusters\n"
    "                   (LOD threshold 0) rather than from the frame's own cut, so the picture is\n"
    "                   a reference for the source geometry and not for the geometry LOD chose\n"
    "  --spp-batch <n>  --reference: samples per dispatch (default 8); smaller keeps each\n"
    "                   submission short and reports progress more often\n"
    "  --log <spec>     log levels, e.g. \"info,gfx=debug\" (stderr shows warnings and up)\n"
    "  --shaders <m>    shader manifest (default: <exe dir>/../shaders/manifest.json when present);\n"
    "                   shaders recompile and reload when their .slang sources change\n"
    "exit codes: 0 ok, 1 error, 2 usage, 3 unavailable (no display, device, mesh shaders)\n";
// clang-format on

constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;
constexpr int k_exit_unavailable = 3;
constexpr u32 k_frames_in_flight = 2;

struct Options {
  u32 width = 1280;
  u32 height = 720;
  u32 frames = 0;
  std::string capture;
  bool vsync = true;
  u32 adapter = 0;
  bool validation = false;
  u32 grid = 257;
  std::string log_spec;
  std::string shaders;
  std::string mesh;        // glTF or .clusters file; empty renders a procedural scene
  std::string procedural;  // which one: "" / "heightfield", or "shredded-atlas"
  // What the LOD builder is told (geometry.md, "What the simplifier is given, and why"). It is
  // part of the derived-data cache key, so `--uv-seams none` builds and addresses its own
  // container and `engine-content build --uv-seams none` writes exactly that one: a before and
  // after comparison of the seam rule is two command lines rather than two builds of the tree.
  geometry::ClusterLodOptions lod;
  std::string scene;       // a scene JSON file: meshes and instances of them
  std::string ddc;         // derived-data root; empty is found from the executable
  u32 grid_instances = 0;  // n: place the one mesh n x n times
  bool cache = true;
  f32 orbit = 0.0f;  // 0: breathe
  // `--fly`: a scripted camera path. `fly_frames` of 0 is off, and `--fly` then also sets
  // `--frames` unless one was given, because a path shorter than itself says nothing.
  f32 fly_from = 0.0f;
  f32 fly_to = 0.0f;
  u32 fly_frames = 0;
  // Where a streamed page's bytes come from. `auto` takes the containers when every mesh has one;
  // `file` refuses to run without them, so a measurement cannot silently fall back; `host` is the
  // in-memory source, which is the A of the host-memory A/B.
  enum class PageSource : u8 { automatic, file, host };
  PageSource page_source = PageSource::automatic;
  // `--page-budget-pct`: the residency budget as a percentage of *this scene's* page bytes, which
  // is how a budget is actually thought about ("a quarter of the working set") and the only way to
  // say it before the scene is loaded and its page table counted. 0 leaves `--page-budget` alone.
  u32 page_budget_pct = 0;
  bool animate = false;
  std::string clip;  // --animate's optional value: a clip name or an index
  // `--morph <name|index>=<weight>`, repeatable: the **static shape** stage's weights, named the
  // way an author names them. They are resolved against the loaded mesh's channel names once the
  // scene exists, because a name is a property of the asset and not of the command line.
  Vector<std::string> morph_requests;
  // `--morph-animate [clip]`: play a clip's weight tracks through the **pose** stage. It is
  // separate from `--animate` because a clip may have weight tracks and no joint tracks (a facial
  // performance) and because a morphed mesh with no skin has no skeleton to animate — which is
  // also why it loads the file's clips itself when `--animate` is not given.
  bool morph_animate = false;
  std::string morph_clip;  // --morph-animate's optional value: a clip name or an index
  f32 anim_speed = 1.0f;
  bool anim_lod = true;       // let the camera decide each character's animation tier
  f32 anim_lod_scale = 1.0f;  // multiplies the capability's band boundaries
  // The reference renderer (docs/plan/04-renderer.md §4.8): spp > 0 takes the whole offscreen
  // path below instead of opening a window, because a converged picture is minutes of compute
  // with nothing to look at while it runs and because CI has no display.
  u32 reference = 0;
  u32 bounces = 3;
  u32 spp_batch = 8;
  bool finest = false;
  renderer::RenderSettings settings;
};

bool next_value(int argc, char** argv, int& i, std::string_view flag, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "engine-view: %.*s needs a value\n", static_cast<int>(flag.size()),
                 flag.data());
    return false;
  }
  out = argv[++i];
  return true;
}

bool parse_u32(const std::string& text, u32& out) {
  char* end = nullptr;
  const unsigned long v = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || v > 0xFFFFFFFFul) return false;
  out = static_cast<u32>(v);
  return true;
}

bool parse_f32(const std::string& text, f32& out) {
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !(v > 0.0) || v > 1.0e6) return false;
  out = static_cast<f32>(v);
  return true;
}

// The same, for the flags whose zero is meaningful: a flat surround has no yaw, and Panini at
// d = 0 is the rectilinear picture, which is the comparison the d parameter is measured against.
bool parse_f32_zero_ok(const std::string& text, f32& out) {
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !(v >= 0.0) || v > 1.0e6) return false;
  out = static_cast<f32>(v);
  return true;
}

// A morph weight: any finite number, **zero and negatives included**. `--morph` used to go through
// `parse_f32`, which is for sizes and speeds and refuses both — so `--morph smile=0` could not turn
// off a channel the asset authors at a non-zero default, and a negative weight, which glTF allows
// and a corrective rig uses, was "not a number".
bool parse_weight(const std::string& text, f32& out) {
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !(v >= -1.0e6 && v <= 1.0e6)) return false;
  out = static_cast<f32>(v);
  return true;
}

// ---- the animated world (docs/subsystems/animation.md, "The app-side glue") -------------------
//
// The whole of `--animate`, and it is here rather than in `systems/renderer` on purpose: the
// renderer must not depend on the animation capability, on `domain/ecs`, or on flecs, so what
// crosses the boundary is one span of `anim::JointMatrix` and one `renderer::InstanceJoints` per
// instance and nothing else. A game writes exactly this — a world, a library, one entity per
// character, a fixed-step tick, and a per-frame read of the runs — which is why it is spelled out
// instead of folded into the frame loop.
//
// It is also the only place in engine-view that an optional capability reaches. Without
// ENGINE_VIEW_ANIMATION (a minimal build, `ENGINE_WITH_ANIMATION=OFF`, `ENGINE_WITH_ECS=OFF`)
// none of this compiles and `--animate` is refused with a sentence.
#if ENGINE_VIEW_ANIMATION
struct AnimatedScene {
  // 60 Hz, which is the rate the deformation phase and the light orbit already run at, so
  // `--frames N` advances the world exactly N steps and two runs draw the same picture.
  ecs::SimWorld sim;
  animation::Library library;
  animation::AnimationSystem system;
  Vector<Id128> entities;                 // one per scene instance; null where it is rigid
  Vector<renderer::InstanceJoints> runs;  // refilled every tick; what the renderer reads
  Vector<u32> mesh_skeleton;              // the skeleton each scene mesh's file contributed
  u32 joints = 0;                         // the widest skeleton attached
  u32 instances = 0;                      // entities attached
  std::string clip_name;                  // what the summary reports

  // ---- the camera-driven LOD (anim_lod.h) -----------------------------------------------------
  //
  // The SoA `sim::TierAssignment` works on, over the **animated** instances only: a rigid one has
  // no pose to coarsen and would only cost a score. `row_instance` maps a row back to its scene
  // instance, which is the one place the tier code's index space and the entity ids meet — the
  // same seam `sim::apply_tier_changes` has, written here because an app cannot make a
  // `sim::EntityHandle` without flecs and `set_tier(Id128, u8)` is the surface that exists for it.
  bool lod = true;
  f32 lod_scale = 1.0f;
  sim::TierParams tier_params;
  sim::ObserverSet observers;
  sim::TierAssignment tiers;
  Vector<u32> row_instance;  // scene instance of each row below
  Vector<Vec3> positions;    // world centre of the instance's padded bounding sphere
  Vector<f32> radii;         // its radius, padding and instance scale included
  Vector<f32> importance;    // what the camera says the row is worth, refilled every frame
  Vector<u8> tier_state;     // in/out of assign_tiers; the tier each row is at
  Vector<u8> instance_tier;  // the same, indexed by scene instance, for `step_animation`
  Vector<sim::TierChange> changes;
  u32 histogram[animation::k_tier_count] = {};
  u32 promotions = 0;
  u32 demotions = 0;
  u32 deferred = 0;
  f64 tick_ns = 0.0;  // summed over the frames, for the summary
  f64 lod_ns = 0.0;

  AnimatedScene() : system(library) {}
};

// A stable, reproducible id per scene instance. `Id128` is the persistent identity ADR-0028 seam 3
// asks for; deriving it from the instance index rather than from a counter keeps a capture
// deterministic across runs.
Id128 instance_id(u32 instance) { return Id128::from_seed(0x656e67'76696577ull, instance + 1); }

// Loads every skin and clip of the scene's mesh files. A `.clusters` container carries the
// per-vertex binding stream but neither a skeleton nor a curve — those are in the source glTF —
// so a run that animates names the glTF, and the derived-data cache still serves its geometry.
bool load_clips(AnimatedScene& scene, const renderer::SceneDesc& desc, std::string& error) {
  scene.mesh_skeleton.resize(desc.meshes.size(), animation::Library::k_not_found);
  for (u32 m = 0; m < desc.meshes.size(); ++m) {
    const std::string& path = desc.meshes[m];
    if (path.empty() || io::extension(path) == ".clusters") continue;
    const u32 before = scene.library.skeleton_count();
    animation::LoadStats stats;
    if (!scene.library.load_gltf(path, {}, &stats, &error)) return false;
    if (scene.library.skeleton_count() > before) scene.mesh_skeleton[m] = before;
    ENGINE_LOG_INFO(log_view, "clips loaded", log::field("path", path),
                    log::field("skeletons", stats.skeletons), log::field("clips", stats.clips),
                    log::field("skipped_channels", stats.skipped_channels));
  }
  return true;
}

// The clip `request` names among the clips of `skeleton`: its index within that skeleton's clips,
// its full library name, or its name without the library's prefix. An empty request takes the
// skeleton's first clip. `k_not_found` when nothing matches, which is a usage error and not a
// silent fall back to some other animation.
u32 find_clip(const animation::Library& library, u32 skeleton, std::string_view request) {
  u32 ordinal = 0;
  u32 index = ~u32{0};
  const bool numeric =
      !request.empty() && request.find_first_not_of("0123456789") == std::string_view::npos;
  if (numeric) index = static_cast<u32>(std::strtoul(std::string(request).c_str(), nullptr, 10));
  for (u32 i = 0; i < library.clip_count(); ++i) {
    if (library.clip(i).skeleton != skeleton) continue;
    if (request.empty() || (numeric && ordinal == index)) return i;
    const std::string& name = library.clip(i).name;
    if (!numeric && (name == request ||
                     (name.size() > request.size() &&
                      name.compare(name.size() - request.size(), request.size(), request) == 0 &&
                      name[name.size() - request.size() - 1] == '/'))) {
      return i;
    }
    ++ordinal;
  }
  return animation::Library::k_not_found;
}

// `--morph-animate`'s clips when nothing is skinned: the weight curves of every glTF the scene
// names, in a library of their own. `--animate` refuses a mesh with no skin — it has no skeleton to
// tick — so before this a morph-only file (every Khronos morph sample is one) played its default
// weights however `--morph-animate` was spelled, with a warning nobody reads. A file with neither
// a skin nor a weight curve has nothing to add and is passed over rather than refused: it is still
// drawn, it just animates nothing.
void load_morph_clips(animation::Library& library, const renderer::SceneDesc& desc) {
  for (const std::string& path : desc.meshes) {
    if (path.empty() || io::extension(path) == ".clusters") continue;
    animation::LoadStats stats;
    std::string reason;
    if (!library.load_gltf(path, {}, &stats, &reason)) {
      ENGINE_LOG_INFO(log_view, "no clips in this mesh", log::field("path", path),
                      log::field("reason", reason));
      continue;
    }
    ENGINE_LOG_INFO(log_view, "morph clips loaded", log::field("path", path),
                    log::field("clips", stats.clips));
  }
}

// The clip `--morph-animate` plays, among the clips that have **weight tracks**: the one `request`
// names — its index among those clips, its full library name, or its name without the library's
// prefix — or the first of them when there is no request. `k_not_found` when nothing matches.
u32 find_morph_clip(const animation::Library& library, std::string_view request) {
  u32 ordinal = 0;
  u32 index = ~u32{0};
  const bool numeric =
      !request.empty() && request.find_first_not_of("0123456789") == std::string_view::npos;
  if (numeric) index = static_cast<u32>(std::strtoul(std::string(request).c_str(), nullptr, 10));
  for (u32 i = 0; i < library.clip_count(); ++i) {
    if (library.clip_data(i).weight_tracks.empty()) continue;
    if (request.empty() || (numeric && ordinal == index)) return i;
    const std::string& name = library.clip(i).name;
    if (!numeric && (name == request ||
                     (name.size() > request.size() &&
                      name.compare(name.size() - request.size(), request.size(), request) == 0 &&
                      name[name.size() - request.size() - 1] == '/'))) {
      return i;
    }
    ++ordinal;
  }
  return animation::Library::k_not_found;
}
#endif  // ENGINE_VIEW_ANIMATION

// Outside the animation guard: static morph weights are a renderer setting, not the animation
// capability, and both frame paths call this whether or not the build has that capability.
//
// `--morph <name|index>=<weight>` against the loaded mesh's channel names. A name is a property of
// the asset, so it cannot be resolved until the scene exists; an index is accepted too, because a
// generated rig may have none worth typing. An unknown name is a **usage error** rather than a
// silently ignored weight: a misspelt expression that quietly does nothing is the kind of thing
// that gets debugged in the picture instead of on the command line. Returns false with `error`.
//
// **The asset's own weights are where the static stage starts.** glTF's `weights` on a mesh or a
// node are the weights it is drawn with when nothing animates them, and a file can author a
// half-applied target as its rest shape — MorphPrimitivesTest does, at 0.5. So the static half
// starts from each channel's `default_weight` and `--morph` overrides the channels it names.
// With `--morph-animate` the defaults are the **pose** stage's starting point instead (the frame
// loop refills it from them before the clip writes the channels it drives), so here they stay at
// zero and are counted once. Until the Khronos samples went through it, defaults were used only
// with `--morph-animate`, and a mesh authored half-morphed drew unmorphed. When every default is
// zero the array stays empty, which is what it always was for such a mesh.
bool resolve_morph_weights(const renderer::SceneData& data, const Options& options,
                           Vector<f32>& out, std::string& error) {
  const geometry::ClusterMesh& mesh = data.lod.mesh;
  out.clear();
  if (!options.morph_animate) {
    bool authored = false;
    for (const geometry::MorphChannel& channel : mesh.morph_channels)
      authored = authored || channel.default_weight != 0.0f;
    if (authored) {
      out.resize(mesh.morph_channels.size(), 0.0f);
      for (u32 c = 0; c < mesh.morph_channels.size(); ++c)
        out[c] = mesh.morph_channels[c].default_weight;
    }
  }
  if (options.morph_requests.empty()) return true;
  if (mesh.morph_channels.empty()) {
    error = "--morph: this mesh has no morph channels";
    return false;
  }
  out.resize(mesh.morph_channels.size(), 0.0f);
  for (const std::string& request : options.morph_requests) {
    const usize split = request.find('=');
    const std::string name = request.substr(0, split);
    f32 weight = 0.0f;
    if (!parse_weight(request.substr(split + 1), weight)) {
      error = "--morph: '" + request.substr(split + 1) + "' is not a number";
      return false;
    }
    u32 channel = ~0u;
    for (u32 c = 0; c < mesh.morph_names.size(); ++c) {
      if (mesh.morph_names[c] == name) channel = c;
    }
    if (channel == ~0u) {
      u32 index = 0;
      if (parse_u32(name, index) && index < mesh.morph_channels.size()) channel = index;
    }
    if (channel == ~0u) {
      error = "--morph: this mesh has no channel named '" + name + "'";
      return false;
    }
    out[channel] = weight;
  }
  return true;
}

#if ENGINE_VIEW_ANIMATION
// Part one of the glue: does this run animate, and which instances play a clip? It runs **before**
// the scene loads, because `SceneInstance::joints` is what makes an instance skinned and the GPU
// scene's deform table is laid out from it. `out` stays null when nothing animates, which is the
// common case and costs one loop over the instance list.
//
// It is a function rather than a block in the frame loop's setup because the *offscreen*
// reference path needs exactly the same three steps in exactly the same order (`run_reference`
// below), and two copies of "which instances are skinned" would be two answers the day one of
// them changed.
bool prepare_animation(std::unique_ptr<AnimatedScene>& out, renderer::SceneDesc& desc,
                       const Options& options, std::string& error) {
  bool wants_animation = options.animate;
  for (const renderer::SceneInstance& instance : desc.instances)
    wants_animation = wants_animation || instance.animation.play;
  if (!wants_animation) return true;

  out = std::make_unique<AnimatedScene>();
  if (!load_clips(*out, desc, error)) return false;
  auto joints_of_mesh = [&](u32 mesh) {
    const u32 skeleton = mesh < out->mesh_skeleton.size() ? out->mesh_skeleton[mesh]
                                                          : animation::Library::k_not_found;
    return skeleton == animation::Library::k_not_found
               ? 0u
               : out->library.skeleton_data(skeleton).joint_count();
  };
  if (desc.instances.empty()) {
    // `--mesh --animate`: the grid is expanded by the loader and a single instance is not, so
    // the one case that has no `SceneInstance` yet gets one here.
    if (desc.grid_instances > 1) {
      desc.grid_joints = joints_of_mesh(0);
    } else {
      renderer::SceneInstance instance;
      instance.mesh = 0;
      instance.joints = joints_of_mesh(0);
      instance.animation.play = true;
      desc.instances.push_back(instance);
    }
  } else {
    for (renderer::SceneInstance& instance : desc.instances) {
      if (options.animate) instance.animation.play = true;
      if (instance.animation.play) instance.joints = joints_of_mesh(instance.mesh);
    }
  }
  const u32 joints = desc.grid_instances > 1
                         ? desc.grid_joints
                         : (desc.instances.empty() ? 0u : desc.instances[0].joints);
  if (joints == 0) {
    error = "no mesh of this scene has a skin; --animate needs a skinned glTF";
    return false;
  }
  return true;
}

// **What the camera says each character is worth, before the world is ticked.**
//
// This is the whole of the join, and it is four calls: build the observer set from the views, ask
// the views which instances are on screen and how much that view is worth, run
// `sim::TierAssignment` over the rows, and hand the accepted changes to the capability. The tier
// code never sees an entity id and the capability never sees a camera; `row_instance` is where
// the two meet, exactly as `sim::apply_tier_changes`' `entities[change.index]` is.
//
// The changes go through `AnimationSystem::set_tier(Id128, u8)` rather than through the hooks
// table, and that is not a shortcut: `sim::MaterializationHooks` speaks `sim::EntityHandle`, only
// `domain/ecs` may make one, and `<flecs.h>` does not belong in `apps/` (AGENTS.md, ADR-0028 seam
// 5). animation.md states that both paths go to one function — `apply_tier`, which acquires or
// releases the pose slot and advances the playhead across the frozen interval — so the demotion
// still releases the slot and the promotion still lands where ticking through the gap would have.
//
// It runs **before** `sim.step()`, so the tick that follows already has the new divisors; and it
// runs on *this* frame's frusta (`SceneRenderer::update_views`) rather than on the ones the last
// frame was drawn with, because a tier that lags the camera by a frame is a pop on every cut.
void update_animation_lod(AnimatedScene& scene, const renderer::ViewSet& views,
                          const renderer::Camera& camera) {
  if (!scene.lod || scene.row_instance.empty()) return;
  view::build_observers(views, camera, scene.observers);
  view::view_importance(views,
                        std::span<const Vec3>(scene.positions.data(), scene.positions.size()),
                        std::span<const f32>(scene.radii.data(), scene.radii.size()),
                        std::span<f32>(scene.importance.data(), scene.importance.size()));
  sim::TierInput input;
  input.positions = {scene.positions.data(), scene.positions.size()};
  input.importance = {scene.importance.data(), scene.importance.size()};
  input.tiers = {scene.tier_state.data(), scene.tier_state.size()};
  scene.changes.clear();
  const sim::TierStats stats =
      scene.tiers.assign_tiers(input, scene.observers, scene.tier_params, scene.changes);
  scene.promotions += stats.promoted;
  scene.demotions += stats.demoted;
  scene.deferred += stats.deferred_promotions + stats.deferred_demotions;
  for (const sim::TierChange& change : scene.changes) {
    scene.system.set_tier(scene.entities[scene.row_instance[change.index]], change.to);
    scene.instance_tier[scene.row_instance[change.index]] = change.to;
  }
  for (u32& bucket : scene.histogram)
    bucket = 0;
  for (const u8 tier : scene.tier_state)
    ++scene.histogram[tier < animation::k_tier_count ? tier : animation::k_tier_count - 1];
}

// One fixed step of the animated world, then where each instance's matrices are. `SimWorld::step`
// reads no clock, so N steps are N steps whatever the machine was doing and two runs draw the
// same picture.
// **What a coarsened instance draws: the rest pose, at LOD2 and LOD3 alike.**
//
// At LOD3 there is no choice — the pool slot is gone and `joint_run` answers false — but at LOD2
// the slot is still held and the matrices in it are simply not rebuilt any more, so handing them
// over would draw the character frozen at whatever pose the last LOD1 tick happened to leave. The
// glue hands a zero-length run instead, which the renderer already reads as "this instance draws
// its rest pose" ([renderer](docs/subsystems/renderer.md), "Skinned instances").
//
// Three reasons, in the order they decided it. It is what the policy already says LOD2 *means* —
// "the pose still exists for gameplay and nothing on screen deforms from it"
// (docs/subsystems/animation.md) — and drawing a stale pose is deforming from it. Keeping the last
// pose would make what is on screen depend on *when* an instance was demoted, so the same
// character at the same distance would differ between two runs and between two cameras, and this
// project compares captures byte for byte. And at LOD3 keeping one would mean the renderer holding
// a private copy of every frozen instance's matrices — the pose pool rebuilt on the far side of
// the boundary that exists to keep poses off it.
//
// What it costs is measured rather than assumed: at the distance where the LOD1/LOD2 switch
// happens the picture changes by **0.0097 FLIP mean** (PSNR 34.4, SSIM 0.988), a tenth of the 0.1
// FLIP calls the threshold of noticing, and the LOD2/LOD3 switch then costs **nothing at all**
// because both draw the same pose ([apps](docs/subsystems/apps.md) has the table).
void step_animation(AnimatedScene& scene) {
  scene.sim.step();
  for (u32 i = 0; i < scene.entities.size(); ++i) {
    renderer::InstanceJoints& run = scene.runs[i];
    run = renderer::InstanceJoints{};
    if (scene.entities[i].is_null()) continue;
    const u8 tier = i < scene.instance_tier.size() ? scene.instance_tier[i] : u8{0};
    if (!animation::lod_plan(tier).skin) continue;  // the rest pose, deliberately
    scene.system.joint_run(scene.entities[i], run.first, run.count);
  }
}

// Part two of the glue: one entity per skinned instance, its clip, and its phase.
//
// It runs *after* the scene has loaded because the renderer's joint buffer is sized by the pose
// pool, and the pool only knows its size once the population is attached — which is also the
// honest place for `SceneData::max_joints` to be filled in.
bool attach_instances(AnimatedScene& scene, const renderer::SceneDesc& desc,
                      renderer::SceneData& data, const Options& options, std::string& error) {
  scene.lod = options.anim_lod;
  scene.lod_scale = options.anim_lod_scale;
  scene.system.install(scene.sim);
  const u32 count = data.instances.size();
  scene.entities.resize(count, Id128{});
  scene.runs.resize(count, renderer::InstanceJoints{});
  // The grid is expanded inside `load_scene`, so its instances have no `SceneInstance` of their
  // own to carry a block; they take the command line's clip and speed and a phase each.
  const bool from_grid = desc.instances.size() != count;

  ecs::WorldCommands commands(scene.sim.world());
  for (u32 i = 0; i < count; ++i) {
    if (data.instance_joints.empty() || data.instance_joints[i] == 0) continue;
    commands.create(instance_id(i));
  }
  commands.apply();

  // The bound each instance's cluster spheres are inflated by is a property of (mesh, clip), not
  // of the instance, so both halves are computed once and cached: the per-joint influence spheres
  // per mesh, and the displacement bound per clip. A thousand-strong crowd of one character
  // samples its clip once.
  Vector<animation::JointBounds> bounds_of_mesh(data.parts.size());
  Vector<bool> bounds_built(data.parts.size(), false);
  Vector<f32> padding_of_clip(scene.library.clip_count(), -1.0f);

  for (u32 i = 0; i < count; ++i) {
    if (data.instance_joints.empty() || data.instance_joints[i] == 0) continue;
    const u32 skeleton_index = scene.mesh_skeleton[data.instances[i].mesh];
    if (skeleton_index == animation::Library::k_not_found) continue;
    renderer::SceneAnimation request;
    if (from_grid) {
      request.clip = options.clip;
      request.speed = options.anim_speed;
    } else {
      request = desc.instances[i].animation;
      if (request.clip.empty()) request.clip = options.clip;
      request.speed *= options.anim_speed;
    }
    const u32 clip_index = find_clip(scene.library, skeleton_index, request.clip);
    if (clip_index == animation::Library::k_not_found) {
      error = request.clip.empty() ? std::string("the skin has no animation to play")
                                   : "no clip named '" + request.clip + "'";
      return false;
    }
    const Id128 id = instance_id(i);
    if (!scene.system.attach(id, scene.library.skeleton(skeleton_index).id,
                             scene.library.clip(clip_index).id)) {
      error = "the animation library does not hold this skin's skeleton";
      return false;
    }
    // The phase: what the scene file asked for, or a spread over the clip so that a crowd of one
    // clip is not a crowd of clones. Every copy still starts at a value that is a function of its
    // index alone, so two runs of `--frames N` draw the same picture.
    const f32 duration = scene.library.clip_data(clip_index).duration;
    f32 phase = request.phase;
    if (phase == 0.0f && count > 1 && duration > 0.0f)
      phase = duration * static_cast<f32>(i) / static_cast<f32>(count);
    if (duration > 0.0f) phase = std::fmod(phase, duration);
    scene.system.set_playhead(id, phase, request.speed);
    scene.entities[i] = id;
    ++scene.instances;
    scene.joints = scene.joints > data.instance_joints[i] ? scene.joints : data.instance_joints[i];
    if (scene.clip_name.empty()) scene.clip_name = scene.library.clip(clip_index).name;

    // The cull pass tests this instance by its **rest-pose** spheres, and a skinned vertex is not
    // in them, so it carries a bound on how far the clip it plays can move one.
    const u32 mesh = data.instances[i].mesh;
    if (!bounds_built[mesh]) {
      bounds_built[mesh] = true;
      const geometry::ClusterMesh& mesh_data = data.lod.mesh;
      if (!mesh_data.skin.empty()) {
        // This mesh's own run of the scene's cluster-ordered streams; the bindings index this
        // mesh's palette, so the spheres are per mesh even though the streams are per scene.
        const u32 first = data.parts[mesh].first_vertex;
        const u32 last = mesh + 1 < data.parts.size() ? data.parts[mesh + 1].first_vertex
                                                      : mesh_data.vertices.size();
        animation::joint_influence_bounds(
            std::span<const Vec3>(mesh_data.vertices.data() + first, last - first),
            std::span<const geometry::SkinBinding>(mesh_data.skin.data() + first, last - first),
            scene.library.skeleton_data(skeleton_index).joint_count(), bounds_of_mesh[mesh]);
      }
    }
    if (padding_of_clip[clip_index] < 0.0f) {
      padding_of_clip[clip_index] = animation::clip_displacement_bound(
          scene.library.skeleton_data(skeleton_index), scene.library.clip_data(clip_index),
          bounds_of_mesh[mesh]);
      Vec3 center{};
      f32 mesh_radius = 0.0f;
      renderer::mesh_bounds(data.lod, data.parts[mesh].first_cluster,
                            data.parts[mesh].leaf_cluster_count, center, mesh_radius);
      ENGINE_LOG_INFO(log_view, "skinned bounds padding",
                      log::field("clip", scene.library.clip(clip_index).name),
                      log::field("mesh_radius", mesh_radius),
                      log::field("padding", padding_of_clip[clip_index]));
    }
    data.instances[i].bounds_padding = padding_of_clip[clip_index];
  }
  if (scene.instances == 0) {
    error = "no instance of this scene is skinned";
    return false;
  }

  // The rows `sim::TierAssignment` scores: one per animated instance, its padded bounding sphere
  // in world space. The instances do not move in engine-view, so this is built once; a game whose
  // characters walk refills `positions` every tick, which is the only line of this that changes.
  //
  // The sphere is the *mesh's* leaf bounds through the instance's transform, plus the same
  // `bounds_padding` the cull pass inflates the cluster spheres by, so an instance whose limb
  // swings out of its rest-pose bounds still counts as on screen. `scale_max` multiplies both, the
  // way `cluster_cull.slang` multiplies them.
  Vector<Vec3> mesh_center(data.parts.size());
  Vector<f32> mesh_radius(data.parts.size(), 0.0f);
  for (u32 m = 0; m < data.parts.size(); ++m) {
    renderer::mesh_bounds(data.lod, data.parts[m].first_cluster, data.parts[m].leaf_cluster_count,
                          mesh_center[m], mesh_radius[m]);
  }
  for (u32 i = 0; i < count; ++i) {
    if (scene.entities[i].is_null()) continue;
    const gfx::InstanceDesc& instance = data.instances[i];
    const Vec4 world_center = instance.world * Vec4{mesh_center[instance.mesh], 1.0f};
    scene.row_instance.push_back(i);
    scene.positions.push_back(Vec3{world_center.x, world_center.y, world_center.z});
    scene.radii.push_back((mesh_radius[instance.mesh] + instance.bounds_padding) *
                          instance.scale_max);
  }
  scene.importance.resize(scene.row_instance.size(), 1.0f);
  // Every instance starts at tier 0, which is where `AnimationSystem::attach` put it, so the first
  // frame's assignment is the only one that has a whole population to move and the rate limits in
  // `sim::TierParams` spread it over the ticks after it.
  scene.tier_state.resize(scene.row_instance.size(), u8{0});
  scene.instance_tier.resize(count, u8{0});
  scene.histogram[0] = scene.row_instance.size();
  scene.tier_params = view::scaled_tier_params(animation::tier_params(), scene.lod_scale);
  // The pool hands out one slot per instance and never splits a run, so its capacity now is the
  // longest span a frame can hand over. The renderer clamps anything longer and says so once.
  data.max_joints = scene.system.poses().joint_capacity();
  // The instances now carry their padding, so the scene's bounding sphere — what the camera
  // frames, and what the light reach and the shadow bias scale with — has to be taken again.
  renderer::update_scene_bounds(data);
  return true;
}
#endif

#if ENGINE_VIEW_ANIMATION
// The animation block of the summary line: what the camera decided, and what the tick cost.
//
// The histogram is the *population by tier* at the last frame, which is the number the LOD policy
// is judged by — `animation`'s bench says a 5/15/30/50 mix is 5.6× cheaper than everything at
// LOD0, so a run whose histogram is all in bucket 0 has a policy that is not working. `tick` and
// `lod` are CPU milliseconds a frame: the world's fixed step (the sampler, the skinning matrices,
// the per-instance `joint_run`) and the tier assignment that precedes it.
JsonValue anim_summary(const AnimatedScene& scene, u64 frames) {
  JsonValue out = JsonValue::object();
  out.set("on", scene.lod);
  out.set("scale", static_cast<f64>(scene.lod_scale));
  out.set("instances", scene.instances);
  JsonValue tiers = JsonValue::array();
  for (const u32 bucket : scene.histogram)
    tiers.push_back(JsonValue(bucket));
  out.set("tiers", std::move(tiers));
  out.set("promotions", scene.promotions);
  out.set("demotions", scene.demotions);
  out.set("deferred", scene.deferred);
  const f64 divisor = frames > 0 ? static_cast<f64>(frames) : 1.0;
  JsonValue ms = JsonValue::object();
  ms.set("tick", scene.tick_ns / 1.0e6 / divisor);
  ms.set("lod", scene.lod_ns / 1.0e6 / divisor);
  out.set("ms", std::move(ms));
  return out;
}
#endif

// The multi-view block of the summary line: what the layout is, and what each view cost. Built
// rather than formatted because it is an array of objects, and built while the renderer is still
// alive because the summary prints after it is gone. A single-view run still carries it, with one
// entry whose numbers are the totals beside it, so a script never has to special-case the shape.
JsonValue views_summary(const renderer::ViewSet& views, const renderer::Stats& stats) {
  JsonValue out = JsonValue::object();
  out.set("layout", renderer::view_layout_name(views.layout()));
  out.set("count", views.size());
  out.set("side_yaw_deg", static_cast<f64>(degrees(views.desc().surround.side_yaw)));
  out.set("panini_d", static_cast<f64>(views.resample() ? views.panini_d() : 0.0f));
  out.set("peripheral_lod", static_cast<f64>(views.desc().peripheral_lod));
  out.set("oversample", static_cast<f64>(views.oversample()));
  out.set("source_width", views.source_width());
  out.set("source_pixels", views.source_pixels());
  JsonValue list = JsonValue::array();
  const f64 timed = stats.timed();
  for (u32 v = 0; v < views.size(); ++v) {
    const renderer::ViewStats& s = stats.views[v];
    JsonValue entry = JsonValue::object();
    entry.set("x", s.x);
    entry.set("y", s.y);
    entry.set("width", s.width);
    entry.set("height", s.height);
    entry.set("source_width", s.source_width);
    entry.set("source_height", s.source_height);
    entry.set("lod_scale", static_cast<f64>(s.lod_scale));
    entry.set("shading_rate", s.shading_rate);
    entry.set("visible_hw", s.visible_hw);
    entry.set("visible_pass2", s.visible_pass2);
    entry.set("visible_sw", s.visible_sw);
    entry.set("visible_pairs", s.visible_pairs());
    JsonValue ms = JsonValue::object();
    ms.set("cull", s.gpu_cull / timed);
    ms.set("hw", s.gpu_hw / timed);
    ms.set("sw", s.gpu_sw / timed);
    ms.set("hiz", s.gpu_hiz / timed);
    ms.set("resolve", s.gpu_resolve / timed);
    ms.set("deform", s.gpu_deform / timed);
    ms.set("trace", s.gpu_trace / timed);
    ms.set("total", s.gpu_sum() / timed);
    entry.set("gpu_ms", std::move(ms));
    list.push_back(std::move(entry));
  }
  out.set("per_view", std::move(list));
  return out;
}

// The geometry residency block (04 §4.9). Always present, so a script never has to test for it:
// `pages_total` is 0 for a run that did not stream, which is what says the rest means nothing.
JsonValue streaming_summary(const renderer::StreamStats& s) {
  JsonValue out = JsonValue::object();
  out.set("pages_total", s.pages_total);
  out.set("pages_resident", s.pages_resident);
  out.set("pages_pinned", s.pages_pinned);
  out.set("page_slots", s.page_slots);
  out.set("pending", s.pending);
  out.set("requests", s.requests);
  out.set("uploads", s.uploads);
  out.set("uploads_bytes", s.uploads_bytes);
  out.set("evictions", s.evictions);
  out.set("stale", s.stale);
  out.set("overflows", s.overflows);
  out.set("frames_to_converge", s.frames_to_converge);
  out.set("resident_bytes", s.resident_bytes);
  out.set("page_bytes", s.page_bytes);
  out.set("budget_bytes", s.budget_bytes);
  // Where the payloads came from and what that cost. `source` is "file" or "host"; the three
  // counters are zero for "host", where the bytes were already in the process.
  out.set("source", std::string(s.from_file ? "file" : "host"));
  out.set("file_reads", s.file_reads);
  out.set("file_bytes", s.file_bytes);
  out.set("host_bytes_freed", s.host_bytes_freed);
  out.set("load_waits", s.load_waits);
  return out;
}

// Progress on stderr, because stdout is the summary line a script reads. One line rewritten in
// place when stderr is a console, one line per batch when it is a file or a pipe.
bool reference_progress(u32 done, u32 total, void*) {
  std::fprintf(stderr, "\rengine-view: reference %u/%u samples", done, total);
  if (done == total) std::fputc('\n', stderr);
  std::fflush(stderr);
  return true;
}

int fail(const char* what, const std::string& error) {
  std::fprintf(stderr, "engine-view: %s: %s\n", what, error.c_str());
  return k_exit_error;
}

int unavailable(const char* what, const std::string& error) {
  std::fprintf(stderr, "engine-view: unavailable: %s%s%s\n", what, error.empty() ? "" : ": ",
               error.c_str());
  return k_exit_unavailable;
}

// `--reference <spp>`: the whole run offscreen, with no window, no surface and no swapchain
// (docs/plan/04-renderer.md §4.8, docs/subsystems/renderer.md "Reference renderer"). A converged
// picture is minutes of compute showing nothing until it is done, the machines that run the
// nightly comparison have no display, and the renderer has never needed a window — so this path
// creates the device without the presentation extensions and goes straight to the offscreen
// contract. It shares the flags and the exit codes with the windowed path; what it does not
// share is the frame loop, because there is one frame.
int run_reference(Options& options) {
  std::string error;
  gfx::DeviceOptions device_options;
  device_options.adapter_index = options.adapter;
  device_options.validation = options.validation;
  gfx::Device device;
  if (!device.create(device_options, &error)) return unavailable("no Vulkan device", error);

  int exit_code = 0;
  renderer::SceneData scene_data;
  renderer::ResolvedSettings resolved;
  renderer::GpuScene scene;
  renderer::SceneRenderer view_renderer;
  renderer::ReferenceRenderer reference;
  renderer::ReferenceFrame frame;
  bench::MachineState machine_start;
  bench::MachineState machine_end;
  bool captured = false;
#if ENGINE_VIEW_ANIMATION
  std::unique_ptr<AnimatedScene> animated;
#endif
  u32 skinned_instances = 0;
  do {
    renderer::SceneDesc desc;
    desc.procedural = options.procedural == "shredded-atlas" ? renderer::Procedural::shredded_atlas
                                                             : renderer::Procedural::heightfield;
    desc.lod = options.lod;
    desc.heightfield_grid = options.grid;
    desc.grid_instances = options.grid_instances;
    desc.ddc = options.ddc;
    desc.cache = options.cache;
    // Loading keeps the page table only when someone is going to stream from it: the layout itself
    // is what the derived-data cache key promises and runs either way.
    desc.stream = options.settings.stream;
    if (!options.scene.empty()) {
      if (!renderer::read_scene_file(options.scene, desc, error)) {
        exit_code = fail("scene", error);
        break;
      }
    } else {
      desc.meshes.push_back(options.mesh);
    }
#if ENGINE_VIEW_ANIMATION
    // The same three steps the windowed path takes, in the same order and through the same
    // functions: which instances are skinned (before the load, because the deform table is laid
    // out from it), the entities and their clips (after it), and **one** tick. One, because a
    // reference render is one frame — so what it draws is the pose the windowed path's first
    // frame would draw, and `--frames` means nothing here.
    if (!prepare_animation(animated, desc, options, error)) {
      exit_code = fail("clips", error);
      break;
    }
#endif
    if (!renderer::load_scene(desc, scene_data, error)) {
      exit_code = fail("mesh", error);
      break;
    }
#if ENGINE_VIEW_ANIMATION
    if (animated) {
      if (!attach_instances(*animated, desc, scene_data, options, error)) {
        exit_code = fail("animate", error);
        break;
      }
      skinned_instances = animated->instances;
      step_animation(*animated);
    }
#endif
    if (!resolve_morph_weights(scene_data, options, options.settings.morph_static_weights, error)) {
      exit_code = fail("morph", error);
      break;
    }
    renderer::resolve_settings(options.settings, device.features(), &scene_data, resolved);
    const renderer::RenderAvailability availability =
        renderer::check_availability(resolved, device.features());
    if (availability != renderer::RenderAvailability::Ok) {
      exit_code = unavailable(
          (std::string(device.adapter().name) + " " + renderer::availability_message(availability))
              .c_str(),
          "");
      break;
    }
    std::string why;
    if (!renderer::reference_available(resolved, device.features(), &why)) {
      exit_code = unavailable((std::string(device.adapter().name) + " " + why).c_str(), "");
      break;
    }
    if (!scene.create(device, scene_data, resolved, &error)) {
      exit_code = fail("scene", error);
      break;
    }
    renderer::SceneRenderer::Desc renderer_desc;
    renderer_desc.width = options.width;
    renderer_desc.height = options.height;
    renderer_desc.offscreen = true;
    renderer_desc.shader_manifest = options.shaders;
    if (!view_renderer.create(device, scene, resolved, renderer_desc, &error)) {
      exit_code = fail("renderer", error);
      break;
    }
    renderer::ReferenceRenderer::Desc reference_desc;
    reference_desc.shader_manifest = options.shaders;
    if (!reference.create(device, scene, view_renderer, reference_desc, &error)) {
      exit_code = fail("reference", error);
      break;
    }
    renderer::ReferenceSettings reference_settings;
    reference_settings.spp = options.reference;
    reference_settings.max_bounces = options.bounces;
    reference_settings.batch = options.spp_batch;
    reference_settings.finest = options.finest;
#if ENGINE_VIEW_ANIMATION
    if (animated) {
      reference_settings.joints = animated->system.joint_matrices();
      reference_settings.instance_joints = {animated->runs.data(), animated->runs.size()};
    }
#endif
    machine_start = bench::sample_machine_state(bench::k_sample_window_ms);
    const renderer::Camera camera =
        renderer::orbit_camera(scene_data.center, scene_data.radius, options.orbit, 0);
    if (!reference.render(camera, reference_settings, frame, &error, reference_progress, nullptr)) {
      exit_code = fail("reference", error);
      break;
    }
    machine_end = bench::sample_machine_state(bench::k_sample_window_ms);
    if (!options.capture.empty()) {
      const io::Status status =
          image::write_png(options.capture, frame.width, frame.height, 4,
                           std::span<const u8>(frame.color.data(), frame.color.size()));
      if (status != io::Status::Ok) {
        exit_code = fail("capture", std::string("cannot write ") + options.capture + ": " +
                                        io::status_name(status));
        break;
      }
      captured = true;
    }
  } while (false);

  view_renderer.sample_gpu_memory();
  const renderer::Stats stats = view_renderer.stats();
  reference.destroy();
  view_renderer.destroy();
  scene.destroy();
  device.destroy();

  if (exit_code == 0) {
    JsonValue machine = JsonValue::object();
    machine.set("start", bench::machine_state_json(machine_start));
    machine.set("end", bench::machine_state_json(machine_end));
    const std::string machine_text = write_json(machine, JsonWriteOptions{.pretty = false});
    std::printf(
        "{\"reference\":true,\"spp\":%u,\"bounces\":%u,\"finest\":%s,\"width\":%u,\"height\":%u,"
        "\"seconds\":%.3f,\"trace_ms\":%.3f,\"samples\":%u,\"visible_pairs\":%u,"
        "\"clusters\":%u,\"leaf_clusters\":%u,\"triangles\":%u,\"instances\":%u,"
        "\"skinned_instances\":%u,"
        "\"gpu_memory\":{\"budget_mib\":%llu,\"used_mib\":%llu,"
        "\"device_local_total_mib\":%llu},\"machine_state\":%s,\"captured\":%s}\n",
        frame.samples, options.bounces, options.finest ? "true" : "false", frame.width,
        frame.height, frame.seconds, frame.trace_ms, frame.samples, frame.visible_pairs,
        scene_data.cluster_count(), scene_data.leaf_count(), scene_data.lod.leaf_triangle_count,
        scene_data.instances.size(), skinned_instances,
        static_cast<unsigned long long>(stats.gpu_memory.budget_mib),
        static_cast<unsigned long long>(stats.gpu_memory.used_mib),
        static_cast<unsigned long long>(stats.gpu_memory.device_local_total_mib),
        machine_text.c_str(), captured ? "true" : "false");
    (void)bench::warn_if_busy(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{},
                              stderr);
  }
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();  // ADR-0031, first statement
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    std::string value;
    if (a == "--help" || a == "-h") {
      std::fputs(k_usage, stdout);
      return 0;
    } else if (a == "--version") {
      // The build stamp (cmake/EngineBuildStamp.cmake), which tools/e10-harness.ps1 checks
      // before it measures anything with this binary. No window, no device.
      std::printf("{\"tool\":\"engine-view\",\"commit\":\"%s\",\"dirty\":%s}\n",
                  engine::build_stamp::commit(), engine::build_stamp::dirty() ? "true" : "false");
      return 0;
    } else if (a == "--width" || a == "--height" || a == "--frames" || a == "--adapter" ||
               a == "--grid" || a == "--grid-instances" || a == "--deform-pool-mib") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      u32 n = 0;
      if (!parse_u32(value, n)) {
        std::fprintf(stderr, "engine-view: %.*s expects a number\n", static_cast<int>(a.size()),
                     a.data());
        return k_exit_usage;
      }
      if (a == "--width") options.width = n;
      if (a == "--height") options.height = n;
      if (a == "--frames") options.frames = n;
      if (a == "--adapter") options.adapter = n;
      if (a == "--grid") options.grid = n;
      if (a == "--grid-instances") options.grid_instances = n;
      // The flag is mebibytes, the setting kibibytes: a caller of the module may want a finer
      // budget than a whole MiB (and a test needs one), while a person at a command line does not.
      if (a == "--deform-pool-mib") options.settings.deform_pool_kib = n * 1024;
    } else if (a == "--lod" || a == "--sw-px" || a == "--orbit" || a == "--deform-amplitude") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      f32 px = 0.0f;
      if (!parse_f32(value, px)) {
        std::fprintf(stderr, "engine-view: %.*s expects a positive number\n",
                     static_cast<int>(a.size()), a.data());
        return k_exit_usage;
      }
      (a == "--lod"     ? options.settings.lod_px
       : a == "--sw-px" ? options.settings.sw_px
       : a == "--orbit" ? options.orbit
                        : options.settings.deform_amplitude) = px;
    } else if (a == "--side-yaw" || a == "--panini-d" || a == "--peripheral-lod") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      f32 v = 0.0f;
      if (!parse_f32_zero_ok(value, v)) {
        std::fprintf(stderr, "engine-view: %.*s expects a number that is not negative\n",
                     static_cast<int>(a.size()), a.data());
        return k_exit_usage;
      }
      if (a == "--side-yaw") options.settings.side_yaw = radians(v);
      if (a == "--panini-d") options.settings.panini_d = v;
      if (a == "--peripheral-lod") options.settings.peripheral_lod = v;
    } else if (a == "--views") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!renderer::parse_view_layout(value, options.settings.views)) {
        std::fprintf(stderr, "engine-view: --views expects single, surround3, or panini\n");
        return k_exit_usage;
      }
    } else if (a == "--deform") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!renderer::parse_deform_mode(value, options.settings.deform,
                                       options.settings.deform_kind)) {
        std::fprintf(stderr, "engine-view: --deform expects none, identity, wave, or lattice\n");
        return k_exit_usage;
      }
    } else if (a == "--morph") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (value.find('=') == std::string_view::npos) {
        std::fprintf(stderr, "engine-view: --morph expects <name|index>=<weight>\n");
        return k_exit_usage;
      }
      options.morph_requests.push_back(std::string(value));
    } else if (a == "--morph-animate") {
      // Optional value, read the way `--animate`'s is: a following argument is the clip unless it
      // is another flag.
      options.morph_animate = true;
      if (i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--")
        options.morph_clip = argv[++i];
    } else if (a == "--static-shape-kib") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!parse_u32(value, options.settings.static_shape_kib)) {
        std::fprintf(stderr, "engine-view: --static-shape-kib expects a number\n");
        return k_exit_usage;
      }
    } else if (a == "--anim-lod") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (value != "on" && value != "off") {
        std::fprintf(stderr, "engine-view: --anim-lod expects on or off\n");
        return k_exit_usage;
      }
      options.anim_lod = value == "on";
    } else if (a == "--anim-lod-scale") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!parse_f32(value, options.anim_lod_scale)) {
        std::fprintf(stderr, "engine-view: --anim-lod-scale expects a positive number\n");
        return k_exit_usage;
      }
    } else if (a == "--anim-speed") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!parse_f32(value, options.anim_speed)) {
        std::fprintf(stderr, "engine-view: --anim-speed expects a positive number\n");
        return k_exit_usage;
      }
    } else if (a == "--animate") {
      // The one flag whose value is optional: `--animate` plays the skin's first clip and
      // `--animate Run` names one. A following argument is the clip unless it is another flag,
      // which is what lets `--animate --frames 6` mean what it reads as.
      options.animate = true;
      if (i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--")
        options.clip = argv[++i];
    } else if (a == "--reference" || a == "--bounces" || a == "--spp-batch") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      u32 n = 0;
      if (!parse_u32(value, n)) {
        std::fprintf(stderr, "engine-view: %.*s expects a number\n", static_cast<int>(a.size()),
                     a.data());
        return k_exit_usage;
      }
      if (a == "--reference") options.reference = n;
      if (a == "--bounces") options.bounces = n;
      if (a == "--spp-batch") options.spp_batch = n;
    } else if (a == "--finest") {
      options.finest = true;
    } else if (a == "--rt-templates") {
      options.settings.rt_templates = true;
    } else if (a == "--stream") {
      options.settings.stream = true;
    } else if (a == "--page-budget" || a == "--upload-budget") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      u32 n = 0;
      if (!parse_u32(value, n)) {
        std::fprintf(stderr, "engine-view: %.*s expects a number\n", static_cast<int>(a.size()),
                     a.data());
        return k_exit_usage;
      }
      // Mebibytes for the residency budget and kibibytes for the per-frame upload, because that is
      // the scale each one is thought about at: a budget is a fraction of a scene and an upload is
      // a fraction of a frame.
      options.settings.stream = true;
      if (a == "--page-budget") options.settings.page_budget_bytes = u64{n} * 1024 * 1024;
      if (a == "--upload-budget") options.settings.upload_budget_bytes = n * 1024;
    } else if (a == "--page-budget-pct") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      u32 n = 0;
      if (!parse_u32(value, n) || n == 0 || n > 100) {
        std::fprintf(stderr, "engine-view: --page-budget-pct expects 1..100\n");
        return k_exit_usage;
      }
      options.page_budget_pct = n;
      options.settings.stream = true;
    } else if (a == "--page-source") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (value == "auto") {
        options.page_source = Options::PageSource::automatic;
      } else if (value == "file") {
        options.page_source = Options::PageSource::file;
      } else if (value == "host") {
        options.page_source = Options::PageSource::host;
      } else {
        std::fprintf(stderr, "engine-view: --page-source expects auto, file, or host\n");
        return k_exit_usage;
      }
      options.settings.stream = true;
    } else if (a == "--fly") {
      // Three values: the distance to start at, the distance to end at, and how many steps.
      std::string to_text;
      std::string steps_text;
      if (!next_value(argc, argv, i, a, value) || !next_value(argc, argv, i, a, to_text) ||
          !next_value(argc, argv, i, a, steps_text)) {
        return k_exit_usage;
      }
      u32 steps = 0;
      if (!parse_f32(value, options.fly_from) || !parse_f32(to_text, options.fly_to) ||
          !parse_u32(steps_text, steps) || steps < 2) {
        std::fprintf(stderr,
                     "engine-view: --fly expects two positive distances in mesh radii and a step "
                     "count of at least 2\n");
        return k_exit_usage;
      }
      options.fly_frames = steps;
    } else if (a == "--raster") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!renderer::parse_raster_mode(value, options.settings.raster)) {
        std::fprintf(stderr, "engine-view: --raster expects direct, hw, vertex, sw, auto, or rt\n");
        return k_exit_usage;
      }
    } else if (a == "--shadows") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      // `auto` is the default, not a spelling the flag takes: it is what no flag means.
      if (value == "auto" || !renderer::parse_shadow_mode(value, options.settings.shadows)) {
        std::fprintf(stderr, "engine-view: --shadows expects off or rt\n");
        return k_exit_usage;
      }
    } else if (a == "--view") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!renderer::parse_view_mode(value, options.settings.view_mode)) {
        std::fprintf(stderr,
                     "engine-view: --view expects id, tri, depth, shaded, normals, or uv\n");
        return k_exit_usage;
      }
    } else if (a == "--capture") {
      if (!next_value(argc, argv, i, a, options.capture)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, a, options.log_spec)) return k_exit_usage;
    } else if (a == "--shaders") {
      if (!next_value(argc, argv, i, a, options.shaders)) return k_exit_usage;
    } else if (a == "--mesh") {
      if (!next_value(argc, argv, i, a, options.mesh)) return k_exit_usage;
    } else if (a == "--procedural") {
      if (!next_value(argc, argv, i, a, options.procedural)) return k_exit_usage;
    } else if (a == "--uv-seams" || a == "--normal-seams") {
      std::string rule;
      if (!next_value(argc, argv, i, a, rule)) return k_exit_usage;
      geometry::SeamRule seam = geometry::SeamRule::protect;
      if (rule == "none") {
        seam = geometry::SeamRule::none;
      } else if (rule == "lock") {
        seam = geometry::SeamRule::lock;
      } else if (rule != "protect") {
        std::fprintf(stderr, "engine-view: a seam rule is none, protect, or lock\n");
        return k_exit_usage;
      }
      (a == "--uv-seams" ? options.lod.uv_seams : options.lod.normal_seams) = seam;
    } else if (a == "--uv-weight" || a == "--normal-weight") {
      std::string thousandths;
      if (!next_value(argc, argv, i, a, thousandths)) return k_exit_usage;
      f32 weight = 0.0f;
      if (!parse_f32_zero_ok(thousandths, weight)) {
        std::fprintf(stderr, "engine-view: %.*s takes a weight in thousandths\n",
                     static_cast<int>(a.size()), a.data());
        return k_exit_usage;
      }
      (a == "--uv-weight" ? options.lod.uv_weight : options.lod.normal_weight) = weight / 1000.0f;
    } else if (a == "--scene") {
      if (!next_value(argc, argv, i, a, options.scene)) return k_exit_usage;
    } else if (a == "--ddc") {
      if (!next_value(argc, argv, i, a, options.ddc)) return k_exit_usage;
    } else if (a == "--no-cache") {
      options.cache = false;
    } else if (a == "--no-vsync") {
      options.vsync = false;
    } else if (a == "--no-cull") {
      options.settings.cull = false;
    } else if (a == "--no-occlusion") {
      options.settings.occlusion = false;
    } else if (a == "--no-cone") {
      options.settings.cone = false;
    } else if (a == "--no-lights") {
      options.settings.lights = false;
    } else if (a == "--validation") {
      options.validation = true;
    } else {
      std::fprintf(stderr, "engine-view: unknown argument %.*s\n%s", static_cast<int>(a.size()),
                   a.data(), k_usage);
      return k_exit_usage;
    }
  }
  if (options.width == 0 || options.height == 0 || options.grid < 2 || options.grid > 2048) {
    std::fprintf(stderr, "engine-view: size must be positive and --grid within 2..2048\n");
    return k_exit_usage;
  }
  if (options.grid_instances > 64) {
    std::fprintf(stderr, "engine-view: --grid-instances must be at most 64\n");
    return k_exit_usage;
  }
  if (!options.procedural.empty() && options.procedural != "heightfield" &&
      options.procedural != "shredded-atlas") {
    std::fprintf(stderr, "engine-view: --procedural must be heightfield or shredded-atlas\n");
    return k_exit_usage;
  }
  if (options.settings.side_yaw > radians(80.0f)) {
    std::fprintf(stderr, "engine-view: --side-yaw must be under 80 degrees\n");
    return k_exit_usage;
  }
  if (options.settings.peripheral_lod < 1.0f) {
    std::fprintf(stderr, "engine-view: --peripheral-lod must be at least 1\n");
    return k_exit_usage;
  }
  if (!options.scene.empty() && (!options.mesh.empty() || options.grid_instances != 0)) {
    std::fprintf(stderr, "engine-view: --scene names its own meshes and instances\n");
    return k_exit_usage;
  }
  if (options.animate && options.mesh.empty() && options.scene.empty()) {
    std::fprintf(stderr,
                 "engine-view: --animate needs a skinned mesh: --mesh <file.gltf|file.glb> or a "
                 "--scene file naming one. The procedural heightfield has no skin.\n");
    return k_exit_usage;
  }
#if !ENGINE_VIEW_ANIMATION
  if (options.morph_animate) {
    // The clips come through the capability's library, so the pose stage has no curve to play in
    // this build; `--morph` (the static stage) is the renderer's and still works.
    std::fprintf(stderr,
                 "engine-view: --morph-animate needs the animation capability, which this build "
                 "does not have. Configure with ENGINE_WITH_ANIMATION=ON and ENGINE_WITH_ECS=ON "
                 "(the minimal presets switch both off); --morph works without it.\n");
    return k_exit_usage;
  }
  if (options.animate) {
    // The capability is not in this build (ENGINE_WITH_ANIMATION=OFF, ENGINE_WITH_ECS=OFF, or a
    // minimal preset). Say which switch, rather than drawing a character standing still.
    std::fprintf(stderr,
                 "engine-view: --animate needs the animation capability, which this build does "
                 "not have. Configure with ENGINE_WITH_ANIMATION=ON and ENGINE_WITH_ECS=ON (the "
                 "minimal presets switch both off).\n");
    return k_exit_usage;
  }
#endif
  if (options.reference != 0 && (options.bounces > 64 || options.spp_batch == 0)) {
    std::fprintf(stderr, "engine-view: --bounces must be at most 64 and --spp-batch at least 1\n");
    return k_exit_usage;
  }
  // A reference of an animated scene is a reference of **one posed frame**, which needs no special
  // path: the frame the reference renders first writes the deformed-vertex pool and builds the
  // acceleration structures from it, and the reference traces those. What it cannot do is play a
  // clip, because it renders one frame — so `--frames` means nothing here and the pose is the one
  // the windowed path's *first* frame would draw. Say so rather than let a caller believe a
  // number that does nothing.
  if (options.reference != 0 && options.animate && options.frames > 1) {
    std::fprintf(stderr,
                 "engine-view: --reference renders one frame, so --frames is ignored and the pose "
                 "is the one after a single tick.\n");
  }
  if (!options.capture.empty() && options.frames == 0) options.frames = 60;
  // A fly-in shorter than the path says nothing about the path, so `--fly` sets the frame count
  // when nothing else did; a caller that gave one keeps it (a longer run repeats the last step,
  // which is how "and then it sat there" is measured).
  if (options.fly_frames > 0 && options.frames == 0) options.frames = options.fly_frames;

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  stderr_sink.set_min_level(log::Level::Warn);
  log::add_sink(&stderr_sink);
  if (!options.log_spec.empty()) {
    // With an explicit spec the category levels decide what reaches stderr.
    stderr_sink.set_min_level(log::Level::Trace);
    log::apply_level_spec("warn");
    log::apply_level_spec(options.log_spec);
  }

  // The reference path never opens a window, so it comes before the display is even asked for.
  if (options.reference != 0) return run_reference(options);

  std::string error;
  if (!window::init(&error)) return unavailable("no display", error);
  const auto extensions = window::Window::vulkan_instance_extensions();
  if (extensions.empty()) {
    window::shutdown();
    return unavailable("SDL has no Vulkan support here", "");
  }
  window::WindowDesc window_desc;
  window_desc.title = "engine-view";
  window_desc.width = options.width;
  window_desc.height = options.height;
  window::Window window;
  if (!window.create(window_desc, &error)) {
    window::shutdown();
    return unavailable("cannot create a window", error);
  }

  gfx::DeviceOptions device_options;
  device_options.adapter_index = options.adapter;
  device_options.validation = options.validation;
  device_options.instance_extensions = extensions.data();
  device_options.instance_extension_count = static_cast<u32>(extensions.size());
  gfx::Device device;
  if (!device.create(device_options, &error)) {
    window.destroy();
    window::shutdown();
    return unavailable("no Vulkan device", error);
  }
  if (!device.features().presentation) {
    const std::string why = std::string(device.adapter().name) + " cannot present";
    device.destroy();
    window.destroy();
    window::shutdown();
    return unavailable(why.c_str(), "");
  }

  int exit_code = 0;
#if ENGINE_VIEW_ANIMATION
  // Heap, and only when `--animate` asks: constructing an `ecs::SimWorld` builds a flecs world
  // with the engine's phases in it, and a run that draws a static mesh should pay nothing for a
  // capability it is not using (plan 11 §11.10).
  std::unique_ptr<AnimatedScene> animated;
  // `--morph-animate`'s clips when `--animate` loaded none (a mesh with no skin): the library
  // alone, no world, because a weight curve is sampled on the frame's clock and ticks nothing.
  std::unique_ptr<animation::Library> morph_library;
#endif
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  gfx::Swapchain swapchain;
  renderer::SceneData scene_data;
  renderer::ResolvedSettings resolved;
  renderer::GpuScene scene;
  // The Efficiency pool a container-backed page source reads on, and the source itself. Two
  // workers: a page is three or four reads and the budget lets two pages a frame through, so more
  // threads would only queue deeper. The pool is built whatever the flags say, because a job
  // system with two workers costs two threads and one branch here would have to be undone the
  // moment anything else in this app wants one.
  jobs::JobSystem page_jobs(
      jobs::JobSystemConfig{.performance_workers = 1, .efficiency_workers = 2});
  renderer::FilePageSource page_source;
  renderer::SceneRenderer view_renderer;
  u64 rendered = 0;
  i64 started_ns = 0;
  i64 finished_ns = 0;
  bench::MachineState machine_start;
  bench::MachineState machine_end;
  bool captured = false;
  u32 extent_width = options.width;
  u32 extent_height = options.height;
  // Read out of the GPU scene while it is alive, because the summary prints after it is gone.
  u64 deform_pool_bytes = 0;
  u64 deform_whole_mesh_bytes = 0;
  // The morph chain's summary numbers and the pose stage's per-frame array.
  u32 morph_channels = 0;
  u64 static_cache_bytes = 0;
  u32 static_cached_instances = 0;
  Vector<f32> morph_pose_weights;
  Vector<f32> morph_defaults;
#if ENGINE_VIEW_ANIMATION
  const anim::Clip* morph_clip = nullptr;  // a clip's weight curves: the animation capability's
#endif
  std::string morph_clip_text;  // the summary's `morph_clip`: which clip the pose stage played
  u64 template_bytes = 0;
  u64 rt_bytes = 0;
  // What the scene's geometry would cost uploaded whole against what the page pool, its staging
  // ring and the always-resident tables actually cost: the pool's own accounting of the device
  // memory streaming saves, beside the driver's `gpu_memory` estimate of the whole process.
  u64 geometry_bytes = 0;
  u64 stream_bytes = 0;
  std::string views_text = "{}";
  std::string streaming_text =
      write_json(streaming_summary(renderer::StreamStats{}), JsonWriteOptions{.pretty = false});
  std::string anim_text = "null";
  u32 skinned_instances = 0;
  u32 joint_matrices = 0;
  std::string clip_text;

  // Everything below unwinds through this block so the destruction order stays in one place.
  do {
    if (!window.create_vulkan_surface(device.handles().instance, surface, &error)) {
      exit_code = fail("surface", error);
      break;
    }
    gfx::SwapchainDesc swapchain_desc;
    swapchain_desc.surface = surface;
    swapchain_desc.width = window.pixel_width();
    swapchain_desc.height = window.pixel_height();
    swapchain_desc.vsync = options.vsync;
    if (!swapchain.create(device, swapchain_desc, &error)) {
      exit_code = fail("swapchain", error);
      break;
    }
    if (!options.capture.empty() && !swapchain.transfer_src()) {
      exit_code = fail("capture", "the surface does not allow reading presented images back");
      break;
    }

    // The scene: the file's meshes and instances, a grid of one mesh, or the heightfield.
    renderer::SceneDesc desc;
    desc.procedural = options.procedural == "shredded-atlas" ? renderer::Procedural::shredded_atlas
                                                             : renderer::Procedural::heightfield;
    desc.lod = options.lod;
    desc.heightfield_grid = options.grid;
    desc.grid_instances = options.grid_instances;
    desc.ddc = options.ddc;
    desc.cache = options.cache;
    desc.stream = options.settings.stream;  // keep the page table for the residency manager
    if (!options.scene.empty()) {
      if (!renderer::read_scene_file(options.scene, desc, error)) {
        exit_code = fail("scene", error);
        break;
      }
    } else {
      desc.meshes.push_back(options.mesh);  // empty: the procedural heightfield
    }

#if ENGINE_VIEW_ANIMATION
    // ---- the animated world, part one: the clips, and which instances play them --------------
    if (!prepare_animation(animated, desc, options, error)) {
      exit_code = fail("clips", error);
      break;
    }
#endif

    if (!renderer::load_scene(desc, scene_data, error)) {
      exit_code = fail("mesh", error);
      break;
    }
    // A budget as a fraction of what this scene's pages come to, which cannot be known until the
    // page table has been built. `ClusterPageDesc::bytes` is the same number the residency manager
    // counts against, so 25 here and 25 in a report mean the same thing.
    if (options.page_budget_pct > 0 && !scene_data.pages.pages.empty()) {
      u64 total = 0;
      for (const geometry::ClusterPageDesc& page : scene_data.pages.pages)
        total += page.bytes;
      options.settings.page_budget_bytes = total * options.page_budget_pct / 100;
    }

#if ENGINE_VIEW_ANIMATION
    // ---- part two: the entities, their clips, and the bounds a moving character needs --------
    if (animated) {
      if (!attach_instances(*animated, desc, scene_data, options, error)) {
        exit_code = fail("animate", error);
        break;
      }
      skinned_instances = animated->instances;
      joint_matrices = static_cast<u32>(animated->system.joint_matrices().size());
      clip_text = animated->clip_name;
      // The summary is a JSON line, so a clip name with a quote or a backslash in it would break
      // whatever reads it. Names come from a file nobody here wrote; replace rather than escape,
      // since this is a label and not an identifier.
      for (char& c : clip_text) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) c = '_';
      }
      ENGINE_LOG_INFO(
          log_view, "animated", log::field("instances", animated->instances),
          log::field("clip", animated->clip_name), log::field("joints", animated->joints),
          log::field("joint_matrices", joint_matrices), log::field("speed", options.anim_speed));
    }
#endif
    if (!resolve_morph_weights(scene_data, options, options.settings.morph_static_weights, error)) {
      exit_code = fail("morph", error);
      break;
    }
    renderer::resolve_settings(options.settings, device.features(), &scene_data, resolved);
    const renderer::RenderAvailability availability =
        renderer::check_availability(resolved, device.features());
    if (availability != renderer::RenderAvailability::Ok) {
      const std::string why =
          std::string(device.adapter().name) + " " + renderer::availability_message(availability);
      exit_code = unavailable(why.c_str(), "");
      break;
    }
    if (!scene.create(device, scene_data, resolved, &error)) {
      exit_code = fail("scene", error);
      break;
    }
    // The pose stage's per-frame array and the defaults it is refilled from, sized once.
    if (options.morph_animate && scene.morphed()) {
      morph_pose_weights.resize(scene.morph_channel_count(), 0.0f);
      morph_defaults.resize(scene.morph_channel_count(), 0.0f);
      for (u32 c = 0; c < scene.morph_channel_count(); ++c)
        morph_defaults[c] = scene_data.lod.mesh.morph_channels[c].default_weight;
#if ENGINE_VIEW_ANIMATION
      // The clips `--animate` already loaded when there are any; otherwise the mesh files' own,
      // which is the case for a morph-only file — it has no skin, so `--animate` refuses it.
      const animation::Library* clips = animated ? &animated->library : nullptr;
      if (clips == nullptr) {
        morph_library = std::make_unique<animation::Library>();
        load_morph_clips(*morph_library, desc);
        clips = morph_library.get();
      }
      const u32 index = find_morph_clip(*clips, options.morph_clip);
      if (index != animation::Library::k_not_found) {
        morph_clip = &clips->clip_data(index);
        morph_clip_text = clips->clip(index).name;
        for (char& c : morph_clip_text) {
          if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) c = '_';
        }
        ENGINE_LOG_INFO(log_view, "morph clip", log::field("clip", clips->clip(index).name),
                        log::field("weight_tracks", morph_clip->weight_tracks.size()),
                        log::field("duration", morph_clip->duration));
      } else if (!options.morph_clip.empty()) {
        exit_code = fail("morph", "--morph-animate: no clip named '" + options.morph_clip +
                                      "' has weight tracks");
        break;
      }
      const bool found_clip = morph_clip != nullptr;
#else
      const bool found_clip = false;  // weight tracks live in clips, which are that capability's
#endif
      if (!found_clip) {
        ENGINE_LOG_WARN(log_view, "--morph-animate found no weight tracks",
                        log::field("channels", scene.morph_channel_count()));
      }
    }
    morph_channels = scene.morph_channel_count();
    static_cache_bytes = scene.static_cache_bytes();
    static_cached_instances = scene.static_cached_instances();
    deform_pool_bytes = scene.deform_pool_bytes();
    deform_whole_mesh_bytes = scene.deform_whole_mesh_bytes();
    template_bytes = scene.template_bytes();
    rt_bytes = scene.rt_bytes();
    geometry_bytes = scene.geometry_bytes();
    stream_bytes = scene.stream_bytes();
    // Where a streamed page's bytes come from. Attaching the container-backed source is also what
    // releases the merged host streams, so it happens here, after the upload and before the
    // renderer that will read from it.
    if (resolved.stream && options.page_source != Options::PageSource::host) {
      std::string why;
      // Which source won is reported by the summary out of `StreamStats::from_file`, not from
      // here; there is nothing to record on success.
      if (!renderer::attach_page_source(scene_data, scene, page_jobs, page_source, &why)) {
        if (options.page_source == Options::PageSource::file) {
          exit_code = fail("page source", why);
          break;
        }
        ENGINE_LOG_INFO(log_view, "geometry pages stream from host memory",
                        log::field("reason", why));
      }
    }
    renderer::SceneRenderer::Desc renderer_desc;
    renderer_desc.page_source = page_source.valid() ? &page_source : nullptr;
    renderer_desc.width = swapchain.extent().width;
    renderer_desc.height = swapchain.extent().height;
    renderer_desc.color_format = swapchain.format();
    renderer_desc.frames_in_flight = k_frames_in_flight;
    renderer_desc.offscreen = false;  // the swapchain image is the target
    renderer_desc.shader_manifest = options.shaders;
    // The layout the flags asked for, over the swapchain. The monitor geometry of a surround is
    // derived from the target (a third of its width each, no bezel correction) unless a caller of
    // the module fills `Surround3` in; engine-view exposes the yaw, which is the parameter a
    // player actually has to set.
    renderer_desc.views.layout = resolved.settings.views;
    renderer_desc.views.surround.side_yaw = resolved.settings.side_yaw;
    renderer_desc.views.panini_d = resolved.settings.panini_d;
    renderer_desc.views.peripheral_lod = resolved.settings.peripheral_lod;
    if (!view_renderer.create(device, scene, resolved, renderer_desc, &error)) {
      exit_code = fail("renderer", error);
      break;
    }
    extent_width = view_renderer.width();
    extent_height = view_renderer.height();
    ENGINE_LOG_INFO(log_view, "ready", log::field("clusters", scene_data.cluster_count()),
                    log::field("leaf_clusters", scene_data.leaf_count()),
                    log::field("triangles", scene_data.lod.leaf_triangle_count),
                    log::field("lod_levels", scene_data.lod.level_cluster_counts.size()),
                    log::field("build_ms", static_cast<f64>(scene_data.build_ns) / 1.0e6),
                    log::field("raster", renderer::raster_name(resolved.settings.raster)),
                    log::field("occlusion", resolved.occlusion),
                    log::field("shadows", resolved.shadows ? "rt" : "off"),
                    log::field("cone", resolved.settings.cone),
                    log::field("mesh_primitives", scene_data.mesh_primitives),
                    log::field("materials", scene.material_count()),
                    log::field("meshes", scene_data.parts.size()),
                    log::field("instances", scene.instance_count()),
                    log::field("pairs", scene.pair_count()),
                    log::field("deform", renderer::deform_name(resolved.settings)),
                    log::field("rt_templates", resolved.settings.rt_templates),
                    log::field("views", renderer::view_layout_name(resolved.settings.views)),
                    log::field("view_count", view_renderer.views().size()),
                    log::field("width", extent_width), log::field("height", extent_height));

    // What else the machine is doing, before the first frame and after the last: the summary is
    // read as a measurement, and on this project's development box it is often taken beside a
    // GPU job and several parallel builds (docs/subsystems/bench.md). The sampler sleeps for its
    // CPU window and spawns a process for the GPU reading, so both samples sit outside the timed
    // region and cost the run nothing.
    machine_start = bench::sample_machine_state(bench::k_sample_window_ms);

    bool running = true;
    bool resize_pending = false;
    i64 last_shader_poll_ns = 0;
    started_ns = time::monotonic_ns();
    while (running) {
      window::Event event;
      while (window.poll(event)) {
        switch (event.kind) {
          case window::EventKind::Quit:
          case window::EventKind::CloseRequested: running = false; break;
          case window::EventKind::Resized: resize_pending = true; break;
          case window::EventKind::KeyDown:
            if (event.key == window::Key::Escape) running = false;
            break;
          default: break;
        }
      }
      if (!running) break;

      // Hot reload: recompile edited shaders four times a second and rebuild every pipeline.
      if (time::monotonic_ns() - last_shader_poll_ns > 250'000'000) {
        last_shader_poll_ns = time::monotonic_ns();
        Vector<std::string> changed;
        std::string reload_error;
        const bool ok = view_renderer.poll_shaders(changed, &reload_error);
        if (!reload_error.empty()) {
          std::fprintf(stderr, "engine-view: shader compile error:\n%s\n", reload_error.c_str());
        }
        if (!ok) {
          exit_code = fail("pipelines", reload_error);
          break;
        }
        for (const std::string& name : changed) {
          ENGINE_LOG_INFO(log_view, "pipelines rebuilt after shader reload",
                          log::field("shader", name));
        }
      }
      if (resize_pending) {
        resize_pending = false;
        if (!swapchain.resize(window.pixel_width(), window.pixel_height(), &error)) {
          exit_code = fail("resize", error);
          break;
        }
        if (window.pixel_width() > 0 && window.pixel_height() > 0 &&
            !view_renderer.resize(swapchain.extent().width, swapchain.extent().height, &error)) {
          exit_code = fail("targets", error);
          break;
        }
      }

      view_renderer.begin_frame();
      u32 image_index = 0;
      const gfx::PresentStatus acquired =
          swapchain.acquire(view_renderer.acquire_semaphore(), image_index);
      if (acquired != gfx::PresentStatus::Ok) {
        view_renderer.abort_frame();
        if (acquired == gfx::PresentStatus::Error) {
          exit_code = fail("acquire", swapchain.last_error());
          break;
        }
        resize_pending = true;  // minimized or out of date: try again next loop
        continue;
      }
      extent_width = view_renderer.width();
      extent_height = view_renderer.height();

      renderer::FrameDesc frame;
      frame.camera =
          options.fly_frames > 0
              ? renderer::fly_camera(scene_data.center, scene_data.radius, options.fly_from,
                                     options.fly_to, static_cast<u32>(rendered), options.fly_frames)
              : renderer::orbit_camera(scene_data.center, scene_data.radius, options.orbit,
                                       rendered);
      frame.frame_index = rendered;
#if ENGINE_VIEW_ANIMATION
      // The camera first, then the tick: `update_animation_lod` reads **this** frame's frusta and
      // sets each instance's tier, and `step_animation` then ticks the world with the divisors
      // that decision left. One fixed step per frame, so `--frames N` advances exactly N steps and
      // two runs produce the same picture whatever the machine was doing.
      if (animated) {
        const i64 lod_started = time::monotonic_ns();
        update_animation_lod(*animated, view_renderer.update_views(frame.camera), frame.camera);
        const i64 ticked = time::monotonic_ns();
        step_animation(*animated);
        animated->lod_ns += static_cast<f64>(ticked - lod_started);
        animated->tick_ns += static_cast<f64>(time::monotonic_ns() - ticked);
      }
#endif
#if ENGINE_VIEW_ANIMATION
      if (animated) {
        // The whole contract: one span, and one run per instance.
        frame.joints = animated->system.joint_matrices();
        frame.instance_joints = {animated->runs.data(), animated->runs.size()};
      }
      // `--morph-animate`: the pose stage's weights, sampled from the clip's weight tracks on
      // exactly the same plain-span contract the joint matrices travel on. The clip writes only
      // the channels its tracks name, so the array is filled with the mesh's *default* weights
      // first — a channel nothing animates then plays at what the asset said rather than at zero.
      if (!morph_pose_weights.empty()) {
        const f32 seconds = static_cast<f32>(frame.frame_index) / 60.0f * options.anim_speed;
        for (u32 c = 0; c < morph_pose_weights.size(); ++c)
          morph_pose_weights[c] = morph_defaults[c];
        if (morph_clip != nullptr) {
          morph_clip->sample_weights(
              seconds, std::span<f32>(morph_pose_weights.data(), morph_pose_weights.size()));
        }
        frame.morph_weights = {morph_pose_weights.data(), morph_pose_weights.size()};
      }
#endif
      frame.color = swapchain.image(image_index);
      frame.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      frame.wait = view_renderer.acquire_semaphore();
      frame.signal = swapchain.render_finished(image_index);
      const u64 value = view_renderer.submit_frame(frame, &error);
      if (value == 0) {
        exit_code = fail("frame", error);
        break;
      }
      ++rendered;

      const bool last = options.frames != 0 && rendered >= options.frames;
      if (last && !options.capture.empty()) {
        view_renderer.wait(value);
        view_renderer.collect_visible();
        gfx::Capture shot;
        Vector<u8> rgba;
        if (!gfx::capture_image(device, swapchain.image(image_index),
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, shot, &error) ||
            !gfx::capture_to_rgba8(shot, rgba)) {
          exit_code = fail("capture", error.empty() ? "unsupported swapchain format" : error);
        } else if (const io::Status status =
                       image::write_png(options.capture, shot.width, shot.height, 4,
                                        std::span<const u8>(rgba.data(), rgba.size()));
                   status != io::Status::Ok) {
          exit_code = fail("capture", std::string("cannot write ") + options.capture + ": " +
                                          io::status_name(status));
        } else {
          captured = true;
        }
      }
      const gfx::PresentStatus presented =
          swapchain.present(image_index, swapchain.render_finished(image_index));
      if (presented == gfx::PresentStatus::Error) {
        exit_code = fail("present", swapchain.last_error());
        break;
      }
      if (presented == gfx::PresentStatus::OutOfDate) resize_pending = true;
      if (last || exit_code != 0) running = false;
    }
    finished_ns = time::monotonic_ns();
    if (view_renderer.valid()) view_renderer.wait_idle();
    view_renderer.sample_gpu_memory();
    if (view_renderer.valid()) {
      views_text = write_json(views_summary(view_renderer.views(), view_renderer.stats()),
                              JsonWriteOptions{.pretty = false});
      streaming_text = write_json(streaming_summary(view_renderer.streamer().stats()),
                                  JsonWriteOptions{.pretty = false});
    }
#if ENGINE_VIEW_ANIMATION
    if (animated)
      anim_text = write_json(anim_summary(*animated, rendered), JsonWriteOptions{.pretty = false});
#endif
    machine_end = bench::sample_machine_state(bench::k_sample_window_ms);
  } while (false);

  const renderer::Stats stats = view_renderer.stats();
  // The process's own footprint, read before anything is torn down: with a container-backed page
  // source this is the number the whole change is about, so it is taken where it still means
  // something. The peak beside it says whether the load ever *materialized* what it then freed.
  const u64 host_memory = platform::process_memory_bytes();
  const u64 host_memory_peak = platform::peak_process_memory_bytes();
  view_renderer.destroy();
  page_source.destroy();
  scene.destroy();
  swapchain.destroy();
  window::Window::destroy_vulkan_surface(device.handles().instance, surface);
  device.destroy();
  window.destroy();
  window::shutdown();

  if (exit_code == 0) {
    const f64 seconds = static_cast<f64>(finished_ns - started_ns) / 1.0e9;
    const f64 avg_ms = rendered > 0 ? seconds * 1000.0 / static_cast<f64>(rendered) : 0.0;
    const u32 visible_min = stats.visible_min == ~u32{0} ? 0u : stats.visible_min;
    // The one block of the line with nulls in it, so it is built rather than formatted.
    JsonValue machine = JsonValue::object();
    machine.set("start", bench::machine_state_json(machine_start));
    machine.set("end", bench::machine_state_json(machine_end));
    const std::string machine_text = write_json(machine, JsonWriteOptions{.pretty = false});
    std::printf(
        "{\"frames\":%llu,\"seconds\":%.3f,\"avg_ms\":%.3f,\"width\":%u,\"height\":%u,"
        "\"clusters\":%u,\"leaf_clusters\":%u,\"triangles\":%u,\"lod_levels\":%u,\"build_ms\":%.1f,"
        "\"mesh_primitives\":%u,\"mesh_cache\":\"%s\",\"meshes\":%u,\"instances\":%u,\"pairs\":%u,"
        "\"cull\":%s,\"occlusion\":%s,\"cone\":%s,\"lod_px\":%.2f,\"raster\":\"%s\","
        "\"shadows\":\"%s\",\"sw_px\":%.1f,"
        "\"visible_hw_last\":%u,\"visible_pass2_last\":%u,\"visible_sw_last\":%u,"
        "\"visible_pairs_last\":%u,\"visible_min\":%u,"
        "\"visible_max\":%u,"
        "\"deform\":\"%s\",\"deform_pool_bytes\":%llu,"
        "\"deform_whole_mesh_bytes\":%llu,\"deform_pool_used_bytes\":%llu,"
        "\"deform_pool_peak_bytes\":%llu,\"deform_entries\":%u,"
        "\"deform_overflow_entries\":%u,\"deform_overflow_bytes\":%llu,"
        "\"morph_channels\":%u,\"morph_static_cache_bytes\":%llu,"
        "\"morph_cached_instances\":%u,\"morph_clip\":\"%s\",\"rt_templates\":%s,"
        "\"skinned_instances\":%u,\"joints\":%u,\"clip\":\"%s\",\"anim\":%s,"
        "\"template_bytes\":%llu,\"rt_bytes\":%llu,\"geometry_bytes\":%llu,\"stream_bytes\":%llu,"
        "\"views\":%s,\"streaming\":%s,"
        "\"host_memory\":{\"bytes\":%llu,\"peak_bytes\":%llu},"
        "\"gpu_memory\":{\"budget_mib\":%llu,\"used_mib\":%llu,"
        "\"device_local_total_mib\":%llu},\"machine_state\":%s,"
        "\"gpu_ms\":{\"cull\":%.4f,\"hw\":%.4f,\"sw\":%.4f,\"hiz\":%.4f,\"resolve\":%.4f,"
        "\"rt\":%.4f,\"clas\":%.4f,\"deform\":%.4f,\"deform_alloc\":%.4f,"
        "\"trace\":%.4f,\"total\":%.4f,"
        "\"frames\":%llu},\"captured\":%s}\n",
        static_cast<unsigned long long>(rendered), seconds, avg_ms, extent_width, extent_height,
        scene_data.cluster_count(), scene_data.leaf_count(), scene_data.lod.leaf_triangle_count,
        scene_data.lod.level_cluster_counts.size(), static_cast<f64>(scene_data.build_ns) / 1.0e6,
        scene_data.mesh_primitives, scene_data.mesh_cache, scene_data.parts.size(),
        scene_data.instances.size(), scene_data.pair_count,
        resolved.settings.cull ? "true" : "false", resolved.occlusion ? "true" : "false",
        resolved.settings.cone ? "true" : "false", static_cast<f64>(resolved.settings.lod_px),
        renderer::raster_name(resolved.settings.raster), resolved.shadows ? "rt" : "off",
        static_cast<f64>(resolved.settings.sw_px), stats.visible_hw, stats.visible_pass2,
        stats.visible_sw, stats.visible_pairs(), visible_min, stats.visible_max,
        renderer::deform_name(resolved.settings),
        static_cast<unsigned long long>(deform_pool_bytes),
        static_cast<unsigned long long>(deform_whole_mesh_bytes),
        static_cast<unsigned long long>(u64{stats.deform_vertices} * 3 * sizeof(f32)),
        static_cast<unsigned long long>(u64{stats.deform_peak_vertices} * 3 * sizeof(f32)),
        stats.deform_entries, stats.deform_overflow_entries,
        static_cast<unsigned long long>(u64{stats.deform_overflow_vertices} * 3 * sizeof(f32)),
        morph_channels, static_cast<unsigned long long>(static_cache_bytes),
        static_cached_instances, morph_clip_text.c_str(),
        resolved.settings.rt_templates ? "true" : "false", skinned_instances, joint_matrices,
        clip_text.c_str(), anim_text.c_str(), static_cast<unsigned long long>(template_bytes),
        static_cast<unsigned long long>(rt_bytes), static_cast<unsigned long long>(geometry_bytes),
        static_cast<unsigned long long>(stream_bytes), views_text.c_str(), streaming_text.c_str(),
        static_cast<unsigned long long>(host_memory),
        static_cast<unsigned long long>(host_memory_peak),
        static_cast<unsigned long long>(stats.gpu_memory.budget_mib),
        static_cast<unsigned long long>(stats.gpu_memory.used_mib),
        static_cast<unsigned long long>(stats.gpu_memory.device_local_total_mib),
        machine_text.c_str(), stats.cull_ms(), stats.hw_ms(), stats.sw_ms(), stats.hiz_ms(),
        stats.resolve_ms(), stats.rt_ms(), stats.clas_ms(), stats.deform_ms(),
        stats.deform_alloc_ms(), stats.trace_ms(), stats.total_ms(),
        static_cast<unsigned long long>(stats.timed_frames), captured ? "true" : "false");
    // stdout is the summary; the caveat goes beside it on stderr, the same line and the same
    // thresholds the bench harness prints.
    (void)bench::warn_if_busy(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{},
                              stderr);
  }
  log::remove_sink(&stderr_sink);
  return exit_code;
}
