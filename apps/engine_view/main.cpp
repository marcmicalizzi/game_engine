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
// 64-bit buffer atomics, no presentation support, or `--raster rt` / `--shadows rt` on a device
// that cannot trace), which tests treat as a skip. A device without mesh shaders is not one of
// them: it draws through the vertex-shader baseline tier.
#include "fly_camera.h"
#include "pacing.h"
#include "window_input.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/log/log.h>
#include <core/math/math.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>
#include <core/platform/thread.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/swapchain.h>
#include <domain/gfx/vulkan.h>
#include <foundation/bench/machine_state.h>
#include <foundation/image/png.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <foundation/window/window.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/flythrough.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/page_source.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <engine_build_stamp.h>
#include <schemas/scene.h>

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

// ---- the interactive camera's parameters (fly_camera.h) ---------------------------------------
//
// Read once, when a live session starts, and written into its session header: a replay flies with
// the numbers its recording was flown with, so changing one of these never changes what an old
// log replays to. They are the owner's to tune with `--tunable name=value` or `--tunables <file>`.
tunables::Int fly_tick_hz{"view.fly.tick_hz", 240, 30, 4000,
                          "Fixed ticks per second the interactive camera integrates at; frames "
                          "sample it at tick boundaries"};
tunables::Float fly_speed{"view.fly.speed", 0.0, 0.0, 1.0e6,
                          "Interactive camera speed in metres per second; 0 flies a tenth of the "
                          "scene's bounding radius a second, whatever the scene's scale"};
tunables::Float fly_fast{"view.fly.fast", 4.0, 1.0, 1000.0,
                         "Speed multiplier while the fast action (Shift) is held"};
tunables::Float fly_slow{"view.fly.slow", 0.25, 0.001, 1.0,
                         "Speed multiplier while the slow action (Alt) is held"};
tunables::Float fly_look{"view.fly.look", 0.0022, 1.0e-5, 0.1,
                         "Mouse-look sensitivity, radians per pixel of pointer motion"};
tunables::Float fly_turn_rate{"view.fly.turn_rate", 2.5, 0.01, 100.0,
                              "Stick turn rate, radians per second at full deflection"};
tunables::Float fly_pitch_limit{"view.fly.pitch_limit_deg", 89.0, 1.0, 89.9,
                                "How far the interactive camera may pitch up or down, degrees"};

// clang-format off
constexpr const char* k_usage =
    "usage: engine-view [--width <px>] [--height <px>] [--frames <n>] [--capture <file.png>]\n"
    "                   [--no-vsync] [--adapter <index>] [--validation] [--grid <n>] [--log <spec>]\n"
    "                   [--shaders <manifest.json>] [--lod <px>] [--no-cull] [--no-occlusion] [--no-cone]\n"
    "                   [--raster direct|hw|vertex|sw|auto|rt] [--sw-px <px>] [--view <mode>] [--orbit <d>]\n"
    "                   [--mesh <file.gltf|file.glb|file.clusters>] [--scene <file.json>]\n"
    "                   [--grid-instances <n>] [--no-cache] [--ddc <dir>] [--no-lights]\n"
    "                   [--deform none|identity|wave|lattice] [--deform-amplitude <a>] [--rt-templates]\n"
    "                   [--rt-budget-mib <n>]\n"
    "                   [--stream] [--page-budget <MiB>] [--upload-budget <KiB>]\n"
    "                   [--shadows off|rt|csm] [--no-shadow-casters]\n"
    "                   [--shadow-cascades <n>] [--shadow-map <texels>] [--shadow-distance <d>]\n"
    "                   [--views single|surround3|panini] [--side-yaw <deg>]\n"
    "                   [--panini-d <d>] [--peripheral-lod <mult>]\n"
    "                   [--animate [clip]] [--anim-speed <x>] [--anim-lod on|off]\n"
    "                   [--anim-lod-scale <x>]\n"
    "                   [--morph <name|index>=<weight>] [--morph-animate [clip]]\n"
    "                   [--static-shape-kib <n>]\n"
    "                   [--reference <spp>] [--bounces <n>] [--finest] [--spp-batch <n>]\n"
    "                   [--camera-path <file.json>] [--overlay <manifest.json>] [--offscreen]\n"
    "                   [--benchmark <out.jsonl>] [--repeat <n>] [--warmup <frames>] [--census]\n"
    "                   [--warmup-seconds <s>] [--census-pixels]\n"
    "                   [--verify-occlusion] [--marker-captures <dir>]\n"
    "                   [--require-quiet] [--wait-quiet <seconds>]\n"
    "                   [--interactive] [--record-input <log.jsonl>] [--replay-input <log.jsonl>]\n"
    "                   [--inject-input <log.jsonl>] [--input-map <map.json>]\n"
    "                   [--tunables <file.json>] [--tunable <name=value,...>]\n"
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
    "  --shadows <how>  off, rt, or csm. rt: every light in the resolve casts a ray-traced shadow\n"
    "                   against the structures the frame built from its own visible list; in a\n"
    "                   raster mode the frame runs the acceleration structure chain as well, which\n"
    "                   turns two-pass occlusion culling off (one visible list to build from).\n"
    "                   csm: the sun casts through cascaded shadow maps, drawn from the light by\n"
    "                   the cull pass and the picture's own rasterizer and filtered (3x3 PCF) in\n"
    "                   the resolve; the point lights are unshadowed; hw and vertex paths only.\n"
    "                   The default is rt where the device has cluster acceleration structures\n"
    "                   and ray queries, csm where it has not; an explicit rt on a device without\n"
    "                   them exits 3 with the sentence `engine-cli gpu.adapters` reports for it\n"
    "  --shadow-cascades <n>  csm: cascades, 1 to 4 (default 4); a frame whose first cascades\n"
    "                   already hold the whole scene draws fewer\n"
    "  --shadow-map <texels>  csm: texels a side per cascade, a power of two (default 2048)\n"
    "  --shadow-distance <d>  csm: how far from the camera the cascades reach (default: the far\n"
    "                   side of the scene's bounds)\n"
    "  --no-shadow-casters  with rt shadows, drop what the normal-cone test culls from the\n"
    "                   shadows too. By default a cluster that faces away from the camera is\n"
    "                   kept out of the picture but still built into the frame's acceleration\n"
    "                   structures, so a card seen from behind casts its shadow; this is the old\n"
    "                   behaviour, for measuring what that costs and restores\n"
    "  --raster <mode>  direct: mesh shaders to color with a depth buffer; hw (default), vertex, sw,\n"
    "                   auto: the visibility buffer through mesh shaders, a vertex shader (the\n"
    "                   baseline tier, chosen automatically without mesh shaders), software, or\n"
    "                   both split by size; rt: ray queries against cluster acceleration structures\n"
    "                   built every frame from the cull output (NVIDIA RTX only; exit 3 elsewhere)\n"
    "  --sw-px <px>     auto mode: clusters narrower than this go to the software rasterizer (32)\n"
    "  --view <mode>    id, tri, depth, shaded (default: materials with vertex normals, textures,\n"
    "                   and normal maps under a sun), normals, uv, shadow (the sun's shadow\n"
    "                   alone: white lit, black shadowed, grey facing away), albedo (the\n"
    "                   textured base colour, unlit: what filtering and mip selection alone do)\n"
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
    "  --rt-budget-mib <n>  the most device memory the per-frame cluster acceleration structures\n"
    "                   may take (default: the renderer.rt.budget_mib tunable, 1024). They are\n"
    "                   sized by what the frames build, a step above it; a frame that wants more\n"
    "                   than the budget holds drops whole instances' structures, shadow casters\n"
    "                   first, and the summary's \"rt\" block counts it\n"
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
    "  --camera-path <f>  fly a camera path (engine.scene.CameraPath): keyframes of position,\n"
    "                   look-at or orientation and field of view. --frames resamples it; without\n"
    "                   it the path's own frame count is flown\n"
    "  --overlay <m>    a landmark overlay (engine.scene.Overlay): a local manifest from content\n"
    "                   hashes to files, which replace the scene's meshes that name those hashes\n"
    "  --offscreen      render without a window, into the renderer's own target, at --width x\n"
    "                   --height whatever the display is (11520x2160 on any machine)\n"
    "  --benchmark <f>  offscreen: fly the path and write one engine.scene.FrameRecord per frame\n"
    "                   to <f> (JSON lines: GPU ms per pass, visible pairs, page uploads and\n"
    "                   evictions), then an engine.scene.FlythroughSummary with the median, p95\n"
    "                   and p99 per pass, which is also the summary line on stdout\n"
    "  --repeat <n>     --benchmark: fly the path n times (default 3); a frame's figure is the\n"
    "                   median of its repeats\n"
    "  --warmup <n>     --benchmark: frames at the path's first camera before each repeat\n"
    "                   (default 240), so the clocks are up and the history is the same\n"
    "  --warmup-seconds <s>  --benchmark: and at least s seconds of them (default 2): a fast\n"
    "                   card needs time, not frames, to bring its clocks up\n"
    "  --census         --benchmark: fly the path again, untimed, reading each frame's visible\n"
    "                   list back: visible pairs by DAG level and by mesh, per frame\n"
    "  --census-pixels  --census, and the pixels each mesh covers from an id capture per frame:\n"
    "                   the frame a landmark comes into view is the frame its count leaves zero\n"
    "  --verify-occlusion  fly the path twice more, with and without occlusion culling, and\n"
    "                   compare the (instance, cluster) under every pixel and every colour byte\n"
    "  --marker-captures <d>  offscreen: write a PNG of every marker frame of the path into <d>\n"
    "  --require-quiet  --benchmark: measure nothing on a busy machine; exit 4 instead\n"
    "  --wait-quiet <s> --benchmark: wait up to s seconds for a quiet machine, then run anyway\n"
    "  --interactive    fly the camera: WASD, E/Space up, Q/Ctrl down, the mouse to look while the\n"
    "                   window holds the pointer (Esc takes it and gives it back), Shift fast, Alt\n"
    "                   slow, M drops a marker; a gamepad's sticks, triggers, stick clicks, Back\n"
    "                   and North do the same. The camera integrates at a fixed tick\n"
    "                   (view.fly.tick_hz, 240) and never from frame time; the title shows the\n"
    "                   last frame's ms and the p99 over the last second. Starts at the orbit\n"
    "                   camera, or at --camera-path's first frame. --benchmark writes the flythrough\n"
    "                   JSONL of the session (one record per frame, ticks included)\n"
    "  --record-input <f>  --interactive: write the session's input log, with a header naming\n"
    "                   the scene, the start camera, the tick rate, the map and this build\n"
    "  --replay-input <f>  fly a recorded session instead of the window's input: the same camera\n"
    "                   to the last bit, until the log ends (or --frames). In the window at its\n"
    "                   recorded pace, or offscreen (--offscreen, --benchmark, --marker-captures)\n"
    "                   at one frame per four ticks; --marker-captures draws every marker. Names\n"
    "                   its own scene unless --scene or --mesh is given; refuses a log recorded\n"
    "                   against another action map or another camera integration version\n"
    "  --inject-input <f>  --interactive: push a log's keys and mouse motion into the window's own\n"
    "                   event queue at their ticks, as if typed, for a test or a smoke run with\n"
    "                   nobody at the keyboard; the real pointer is never taken\n"
    "  --input-map <f>  the action map to fly with (default: engine-view's own, the same as\n"
    "                   content/input-maps/engine-view.json)\n"
    "  --tunables <f>   load tunable values from a JSON object file (view.fly.* among them)\n"
    "  --tunable <s>    set tunables: \"view.fly.speed=20,view.fly.look=0.003\"\n"
    "  --log <spec>     log levels, e.g. \"info,gfx=debug\" (stderr shows warnings and up)\n"
    "  --shaders <m>    shader manifest (default: <exe dir>/../shaders/manifest.json when present);\n"
    "                   shaders recompile and reload when their .slang sources change\n"
    "exit codes: 0 ok, 1 error, 2 usage, 3 unavailable (no display, no device, no 64-bit\n"
    "            buffer atomics, or rt asked of a device that cannot trace), 4 the machine was\n"
    "            busy and --require-quiet refused to measure\n";
// clang-format on

constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;
constexpr int k_exit_unavailable = 3;
// --require-quiet on a busy machine: the bench harness's own code (bench::k_exit_not_quiet), so a
// script tells "come back later" from a failure the same way for both.
constexpr int k_exit_busy = 4;
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
  // The flythrough (docs/plan/09-testing-profiling.md §9.4, renderer.md "Scenes, camera paths and
  // flythroughs"). A camera path drives the camera in either frame path; `--offscreen` takes the
  // windowless one, and `--benchmark` implies it, because a composited window is a source of noise
  // a measurement does not need (E1 on Pascal) and because 11520x2160 does not fit most displays.
  std::string camera_path;
  std::string overlay;
  bool offscreen = false;
  std::string benchmark;  // the .jsonl the per-frame records and the summary go to
  u32 repeats = 3;
  u32 warmup = 240;
  f32 warmup_seconds = 2.0f;
  bool census = false;
  bool census_pixels = false;  // and the pixels each mesh covers, from an id capture per frame
  bool verify_occlusion = false;
  std::string marker_captures;
  bool require_quiet = false;
  u32 wait_quiet_s = 0;
  // The interactive camera (fly_camera.h). `--replay-input` implies `interactive`; `offscreen`
  // is resolved after parsing, because `--benchmark` means "offscreen" for a flythrough and a
  // replay and "measure the window" for a live session somebody is flying.
  bool interactive = false;
  std::string record_input;
  std::string replay_input;
  std::string inject_input;
  std::string input_map;
  std::string tunables_file;
  std::string tunable_overrides;
  bool procedural_given = false;  // --procedural or --grid: a replay then draws what it is told
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

// The ray tracing chain's block of the summary line (renderer::RtStats): what the per-frame
// structures hold room for now and at most, the budget as a capacity, what they would be sized for
// if they were sized by the scene, their bytes, the resizes, and the frames that wanted more than
// they held. `rt_bytes` beside it is the same `bytes`, kept under its old name.
JsonValue rt_summary(const renderer::RtStats& rt) {
  JsonValue out = JsonValue::object();
  out.set("capacity", rt.capacity);
  out.set("peak_capacity", rt.peak_capacity);
  out.set("limit", rt.limit);
  out.set("union_clusters", rt.union_clusters);
  out.set("bytes", rt.bytes);
  out.set("peak_bytes", rt.peak_bytes);
  out.set("built_last", rt.built);
  out.set("wanted_last", rt.wanted);
  out.set("peak_wanted", rt.peak_wanted);
  out.set("grows", rt.grows);
  out.set("shrinks", rt.shrinks);
  out.set("overflow_frames", rt.overflow_frames);
  out.set("dropped_instances", rt.dropped_instances);
  out.set("dropped_caster_instances", rt.dropped_caster_instances);
  return out;
}

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
    entry.set("shadow_casters", s.shadow_casters);
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
  // The pool's own page count beside the manager's `pages_resident`, and the loads given up so the
  // walk's head could start its read (renderer.md, "Admission never waits on a read it cannot
  // start"). A pool that stops short of the manager while `pending` grows is a stall.
  out.set("pool_pages", s.pool_pages);
  out.set("steals", s.steals);
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

// ---- the interactive camera: what main prepares, and what both frame loops share ------------
//
// `--interactive` flies `view::FlySession` from the window's input in the windowed loop in
// `main`; `--replay-input` flies the same session from a log, in that loop at the recording's
// pace or in `run_offscreen` at a fixed four ticks a frame. Everything about a session that does
// not need a device — the map, the log, its header, the refusals — is settled here first, so a
// log that cannot be replayed is refused before a window or a device is asked for.
struct Interactive {
  bool on = false;
  bool replay = false;
  input::ActionMap map;
  input::InputLog log;         // --replay-input: the recording
  view::SessionHeader header;  // a replay's, from its log; a live session's is built at start
  u64 log_hash = 0;            // the recording file's bytes, which name what a replay measured
  input::InputLog injected;    // --inject-input
  u64 inject_end = 0;          // the tick the injected session ended at
};

// The per-frame line an interactive `--benchmark` keeps until the frame's GPU numbers fold in
// (`Stats::last`), a slot's worth of frames later. A ring, because a session is open-ended: the
// records themselves are the only thing that grows.
struct PendingFrame {
  u64 submission = ~u64{0};
  u32 frame = 0;
  u32 ticks = 0;
  f64 time = 0.0;
  f64 cpu_ms = 0.0;
  f64 frame_ms = 0.0;
};
constexpr u32 k_pending_frames = 8;  // more than k_frames_in_flight + 1

// Moves a folded frame's numbers into a record, if the fold is of a frame this loop submitted.
void take_folded(const renderer::SceneRenderer& renderer, u64& folded,
                 const PendingFrame (&pending)[k_pending_frames],
                 Vector<scene::FrameRecord>& records) {
  const renderer::Stats& stats = renderer.stats();
  if (stats.folded == folded) return;
  folded = stats.folded;
  const PendingFrame& p = pending[stats.last.submission % k_pending_frames];
  if (p.submission != stats.last.submission) return;
  scene::FrameRecord record = renderer::frame_record(stats.last, 0, p.frame, p.time);
  record.cpu_ms = p.cpu_ms;
  record.frame_ms = p.frame_ms;
  record.ticks = p.ticks;
  records.push_back(std::move(record));
}

int prepare_interactive(Options& options, Interactive& it) {
  it.on = true;
  it.replay = !options.replay_input.empty();
  std::string error;
  if (options.input_map.empty()) {
    it.map = view::default_fly_map();
  } else {
    std::string text;
    JsonValue value;
    if (io::read_file(options.input_map, text) != io::Status::Ok) {
      return fail("input map", "cannot read " + options.input_map);
    }
    if (const JsonParseResult parsed = parse_json(text, value); !parsed.ok) {
      return fail("input map", options.input_map + ": " + parsed.message);
    }
    if (!it.map.from_json(value, &error)) return fail("input map", error);
  }
  view::FlyActions actions;
  if (!view::resolve_fly_actions(it.map, actions, &error)) return fail("input map", error);

  if (it.replay) {
    if (it.log.load(options.replay_input, &error) != io::Status::Ok) return fail("replay", error);
    if (!view::session_from_json(it.log.session(), it.header, &error)) {
      return fail("replay", options.replay_input + ": " + error);
    }
    // The refusal the input module makes, made before anything is loaded: the same key would
    // mean another action, and the replay would diverge from its session without saying so.
    const input::InputState probe(it.map);
    if (!it.log.check_map(probe, &error)) return fail("replay", error);
    std::string bytes;
    if (io::read_file(options.replay_input, bytes) == io::Status::Ok) {
      it.log_hash = hash_bytes(bytes.data(), bytes.size());
    }
    // The scene the recording drew, unless the command line names another. The camera flies the
    // same path over either; the pictures are of whatever is loaded.
    const view::SessionHeader& h = it.header;
    if (options.scene.empty() && options.mesh.empty() && !options.procedural_given) {
      options.scene = h.scene;
      options.mesh = h.mesh;
      options.procedural = h.procedural;
      options.grid = h.grid;
      options.grid_instances = h.grid_instances;
    } else if (options.scene != h.scene || options.mesh != h.mesh) {
      std::fprintf(stderr,
                   "engine-view: replaying over a scene the recording did not draw (it drew "
                   "scene '%s' mesh '%s'); the camera flies the same path\n",
                   h.scene.c_str(), h.mesh.c_str());
    }
  }
  if (!options.inject_input.empty()) {
    if (it.injected.load(options.inject_input, &error) != io::Status::Ok) {
      return fail("inject", error);
    }
    view::SessionHeader injected;
    it.inject_end = view::session_from_json(it.injected.session(), injected, nullptr)
                        ? injected.ticks
                        : it.injected.last_tick().value + 1;
  }
  return 0;
}

// A live session's header: this build, what the command line drew, and where the camera starts —
// the camera path's first frame, or the orbit — with the tunables read once, here.
view::SessionHeader live_header(const Options& options, const renderer::SceneData& scene_data,
                                const renderer::CameraPath* path) {
  view::SessionHeader h;
  h.engine_commit = build_stamp::commit();
  h.engine_dirty = build_stamp::dirty();
  h.scene = options.scene;
  h.mesh = options.mesh;
  h.procedural = options.procedural;
  h.grid = options.grid;
  h.grid_instances = options.grid_instances;
  renderer::Camera start;
  if (path != nullptr && !path->keys.empty()) {
    start = renderer::camera_path_frame(*path, 0, path->frame_count());
  } else {
    start = renderer::orbit_camera(scene_data.center, scene_data.radius, options.orbit, 0);
    // The orbit's near plane is a hundredth of the scene's radius, which is right for a camera
    // that never comes closer than the scene's edge and 26 m for one flying low over the desert
    // overlook. Reversed-Z has depth to spare, so a camera that flies in gets a near one.
    if (start.znear > 0.05f) start.znear = 0.05f;
  }
  h.start = view::fly_state_from_camera(start);
  h.fov_y = start.fov_y;
  h.znear = start.znear;
  h.params.tick_hz = static_cast<u32>(fly_tick_hz.get());
  const f64 speed = fly_speed.get();
  const f64 scene_speed = static_cast<f64>(scene_data.radius) * 0.1;
  h.params.speed =
      static_cast<f32>(speed > 0.0 ? speed : (scene_speed > 1.0e-3 ? scene_speed : 1.0e-3));
  h.params.fast = static_cast<f32>(fly_fast.get());
  h.params.slow = static_cast<f32>(fly_slow.get());
  h.params.look = static_cast<f32>(fly_look.get());
  h.params.turn_rate = static_cast<f32>(fly_turn_rate.get());
  h.params.pitch_limit = static_cast<f32>(fly_pitch_limit.get() * 3.14159265358979323846 / 180.0);
  return h;
}

// The summary's `interactive` block: how the session was driven, its header, and the trajectory
// it flew — the hash over every tick's camera, where it ended, and its markers.
JsonValue interactive_summary(const Interactive& it, const view::FlySession& session,
                              const Options& options) {
  JsonValue out = JsonValue::object();
  out.set("mode", it.replay ? "replay" : "live");
  out.set("map", it.map.hash());
  out.set("record_input", options.record_input);
  out.set("replay_input", options.replay_input);
  out.set("inject_input", options.inject_input);
  view::SessionHeader header = session.header();
  header.ticks = session.tick().value;
  out.set("session", view::session_to_json(header));
  out.set("trajectory", view::trajectory_to_json(session.trajectory(), header.params.tick_hz));
  return out;
}

// What a run drew and on what, into a flythrough summary: the scene, the path (a camera path, or
// a replayed session's log), every mesh's identity and one hash over all of them, and the
// renderer's settings. Shared by the flythrough, the offscreen replay and a live session's
// `--benchmark`, so the three summaries name what they measured the same way.
void describe_run(const renderer::SceneData& scene_data, const Options& options,
                  const std::string& path_name, u64 path_hash,
                  const renderer::ResolvedSettings& resolved, const renderer::SceneRenderer& view,
                  scene::FlythroughSummary& summary) {
  summary.scene = !scene_data.name.empty() ? scene_data.name : options.scene;
  summary.scene_hash = renderer::hash_hex(scene_data.file_hash);
  summary.path = path_name;
  summary.path_hash = renderer::hash_hex(path_hash);
  u64 identity = hash_combine(scene_data.file_hash, path_hash);
  for (u32 m = 0; m < scene_data.parts.size(); ++m) {
    const renderer::SourceMesh& source = scene_data.sources[m];
    identity = hash_combine(identity, source.source_hash);
    scene::MeshIdentity mesh;
    const bool described = m < scene_data.mesh_info.size();
    mesh.name = described ? scene_data.mesh_info[m].name : std::string();
    mesh.source = described ? scene_data.mesh_info[m].origin : "scene";
    mesh.path = source.container.empty() ? mesh.source : source.container;
    mesh.hash = renderer::hash_hex(source.source_hash);
    mesh.clusters = scene_data.parts[m].cluster_count;
    const geometry::ClusterMeshPart& part = scene_data.parts[m];
    for (u32 c = 0; c < part.leaf_cluster_count; ++c)
      mesh.triangles +=
          scene_data.lod.mesh.clusters[part.first_cluster + part.first_leaf_cluster + c]
              .triangle_count;
    summary.meshes.push_back(std::move(mesh));
  }
  summary.identity = renderer::hash_hex(identity);
  summary.width = view.width();
  summary.height = view.height();
  summary.views = renderer::view_layout_name(resolved.settings.views);
  summary.raster = renderer::raster_name(resolved.settings.raster);
  summary.shadows = renderer::resolved_shadow_name(resolved);
  summary.occlusion = resolved.occlusion;
  summary.lod_px = resolved.settings.lod_px;
  summary.stream = resolved.stream;
  summary.page_budget_bytes = resolved.settings.page_budget_bytes;
  summary.instances = scene_data.instances.size();
  summary.pairs = scene_data.pair_count;
  summary.clusters = scene_data.cluster_count();
  if (view.valid()) renderer::summarize_rt(view.stats().rt, summary.rt);
}

// The `.jsonl` a benchmark writes: one record per line, then the summary line.
io::Status write_benchmark(const std::string& path, std::span<const scene::FrameRecord> records,
                           const std::string& summary_line) {
  std::string text;
  for (const scene::FrameRecord& record : records) {
    write_json(schema::to_json(record), text, JsonWriteOptions{.pretty = false});
    text.push_back('\n');
  }
  text += summary_line;
  text.push_back('\n');
  return io::write_file(path, text);
}

// Saves a live session's recording: its events, the map's hash, and the session header with the
// number of ticks it ran, so a replay runs exactly those.
bool save_recording(const Options& options, const Interactive& it, const view::FlySession& session,
                    input::InputLog& recording, std::string& error) {
  view::SessionHeader header = session.header();
  header.ticks = session.tick().value;
  recording.set_map(it.map);
  recording.set_session(view::session_to_json(header));
  const io::Status status = recording.save(options.record_input);
  if (status != io::Status::Ok) {
    error = "cannot write " + options.record_input + ": " + io::status_name(status);
    return false;
  }
  return true;
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
      if (!renderer::read_scene_file(options.scene, renderer::SceneFileOptions{options.overlay},
                                     desc, error)) {
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
      exit_code = unavailable(renderer::unavailable_reason(availability, device).c_str(), "");
      break;
    }
    std::string why;
    if (!renderer::reference_available(resolved, device, &why)) {
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
    // The caveat first and the summary last (see the windowed summary for why the order matters).
    (void)bench::warn_if_busy(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{},
                              stderr);
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
    std::fflush(stdout);
  }
  return exit_code;
}

// A marker's name as a file name: letters, digits, '-' and '_' kept, everything else '_'.
std::string file_stem(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_';
    out.push_back(plain ? c : '_');
  }
  return out.empty() ? std::string("marker") : out;
}

// Waits up to `seconds` for a quiet machine, sampling every five, the way the bench harness's
// `--wait-quiet` does (docs/subsystems/bench.md), and says on stderr which way it ended.
bench::MachineState wait_for_quiet(u32 seconds) {
  bench::MachineState state = bench::sample_machine_state(bench::k_sample_window_ms);
  if (seconds == 0 || bench::is_quiet(state, bench::QuietThresholds{})) return state;
  std::fprintf(stderr, "engine-view: waiting up to %u s for a quiet machine: %s\n", seconds,
               bench::describe(state).c_str());
  const i64 deadline = time::monotonic_ns() + i64{seconds} * 1'000'000'000;
  while (time::monotonic_ns() < deadline) {
    const i64 remaining_ms = (deadline - time::monotonic_ns()) / 1'000'000;
    platform::sleep_ms(
        static_cast<u32>(remaining_ms < 5000 ? (remaining_ms > 0 ? remaining_ms : 0) : 5000));
    state = bench::sample_machine_state(bench::k_sample_window_ms);
    if (bench::is_quiet(state, bench::QuietThresholds{})) {
      std::fprintf(stderr, "engine-view: the machine is quiet\n");
      return state;
    }
  }
  std::fprintf(stderr, "engine-view: still busy after %u s, measuring anyway: %s\n", seconds,
               bench::describe(state).c_str());
  return state;
}

// `--offscreen`, `--benchmark`, `--census`, `--verify-occlusion`, `--marker-captures`: the whole
// run with no window, no surface and no swapchain — the flythrough harness of plan 09 §9.4
// (docs/subsystems/renderer.md, "Scenes, camera paths and flythroughs"; docs/subsystems/apps.md).
//
// **The timed pass keeps frames in flight** exactly as the windowed loop and `render.benchmark`
// do, and reads each frame's own numbers out of `Stats::last` when its slot comes around; it never
// waits on a frame, because a measurement that waited would let the clocks fall between frames and
// measure that. Each repeat starts with `--warmup` frames at the path's first camera, which brings
// the clocks up and gives every repeat the same occlusion history, so a frame's visible pairs can
// be compared across repeats — the summary's `deterministic`.
//
// **Everything that reads back comes after it, untimed**: the census (visible pairs by DAG level
// and mesh, per frame), the occlusion check (every frame drawn twice more, with and without
// occlusion culling, compared pixel for pixel), and the marker captures.
//
// **`--replay-input` offscreen** flies a recorded session instead of a path: one frame per
// `tick_hz / 60` ticks (four at the default 240 Hz), whatever the recording's frame rate was. The
// camera is a function of the ticks alone, so this samples the path the player flew at a steady
// nominal 60 Hz — which is what makes a replay a benchmark of the *path*, repeatable run to run —
// and the markers, the ticks where `marker` was pressed, are drawn afterwards from their own
// cameras, so no frame grouping can move them.
int run_offscreen(Options& options, Interactive& interactive) {
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
  jobs::JobSystem page_jobs(
      jobs::JobSystemConfig{.performance_workers = 1, .efficiency_workers = 2});
  renderer::FilePageSource page_source;
  renderer::SceneRenderer view_renderer;
  renderer::ResolvedSettings resolved_off;
  renderer::GpuScene scene_off;
  renderer::SceneRenderer renderer_off;
  renderer::VisibleCensus census;
  renderer::CameraPath path;
  bool have_path = false;
  u32 frames = 0;
  Vector<scene::FrameRecord> records;
  scene::FlythroughSummary summary;
  summary.format = "engine.flythrough.v1";
  std::string summary_line;  // printed after the teardown, so it is the last thing out
  bench::MachineState machine_start;
  bench::MachineState machine_end;
  bool measured = false;
  bool captured = false;
  f64 timed_seconds = 0.0;
  u64 rendered = 0;
  // A replay is flown once: its frames are a function of its ticks, and a second pass over them
  // would measure the same frames again, which `--repeat` exists to do for a path's.
  const u32 repeats = options.benchmark.empty() || interactive.on ? 1u : options.repeats;
  view::FlySession session;
  const u32 tick_hz = interactive.header.params.tick_hz > 0 ? interactive.header.params.tick_hz : 1;
  const u32 ticks_per_frame =
      tick_hz / view::k_frame_index_hz > 0 ? tick_hz / view::k_frame_index_hz : 1u;
  do {
    renderer::SceneDesc desc;
    desc.procedural = options.procedural == "shredded-atlas" ? renderer::Procedural::shredded_atlas
                                                             : renderer::Procedural::heightfield;
    desc.lod = options.lod;
    desc.heightfield_grid = options.grid;
    desc.grid_instances = options.grid_instances;
    desc.ddc = options.ddc;
    desc.cache = options.cache;
    desc.stream = options.settings.stream;
    if (!options.scene.empty()) {
      renderer::SceneFileOptions file_options;
      file_options.overlay = options.overlay;
      if (!renderer::read_scene_file(options.scene, file_options, desc, error)) {
        exit_code = fail("scene", error);
        break;
      }
    } else {
      desc.meshes.push_back(options.mesh);
    }
    if (!renderer::load_scene(desc, scene_data, error)) {
      exit_code = fail("mesh", error);
      break;
    }
    if (options.page_budget_pct > 0 && !scene_data.pages.pages.empty()) {
      u64 total = 0;
      for (const geometry::ClusterPageDesc& page : scene_data.pages.pages)
        total += page.bytes;
      options.settings.page_budget_bytes = total * options.page_budget_pct / 100;
    }
    if (!resolve_morph_weights(scene_data, options, options.settings.morph_static_weights, error)) {
      exit_code = fail("morph", error);
      break;
    }
    renderer::resolve_settings(options.settings, device.features(), &scene_data, resolved);
    const renderer::RenderAvailability availability =
        renderer::check_availability(resolved, device.features());
    if (availability != renderer::RenderAvailability::Ok) {
      exit_code = unavailable(renderer::unavailable_reason(availability, device).c_str(), "");
      break;
    }
    if (options.verify_occlusion && resolved.stream) {
      exit_code = fail("verify-occlusion",
                       "the occlusion check draws the path twice from two uploads of the scene, "
                       "and a streamed scene releases its geometry once the first is made; run "
                       "it without --stream");
      break;
    }
    if (!options.camera_path.empty()) {
      if (!renderer::read_camera_path(options.camera_path,
                                      scene_data.terrain.enabled ? &scene_data.terrain : nullptr,
                                      path, error)) {
        exit_code = fail("camera path", error);
        break;
      }
      have_path = true;
    }
    frames = options.frames != 0 ? options.frames : (have_path ? path.frame_count() : 60u);
    if (interactive.on) {
      // Until the log ends: every tick the recording ran, a frame per `ticks_per_frame` of them.
      const u64 whole = (interactive.header.ticks + ticks_per_frame - 1) / ticks_per_frame;
      frames =
          options.frames != 0 && options.frames < whole ? options.frames : static_cast<u32>(whole);
    }
    if (!scene.create(device, scene_data, resolved, &error)) {
      exit_code = fail("scene", error);
      break;
    }
    // The occlusion check's second scene is built before a page source could release the host
    // streams it uploads from; streaming and the check were refused together above anyway.
    if (options.verify_occlusion) {
      renderer::RenderSettings off = options.settings;
      off.occlusion = false;
      renderer::resolve_settings(off, device.features(), &scene_data, resolved_off);
      if (!scene_off.create(device, scene_data, resolved_off, &error)) {
        exit_code = fail("scene", error);
        break;
      }
    }
    if (resolved.stream && options.page_source != Options::PageSource::host) {
      std::string why;
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
    renderer_desc.width = options.width;
    renderer_desc.height = options.height;
    renderer_desc.frames_in_flight = k_frames_in_flight;
    renderer_desc.offscreen = true;
    renderer_desc.shader_manifest = options.shaders;
    renderer_desc.views.layout = resolved.settings.views;
    renderer_desc.views.surround.side_yaw = resolved.settings.side_yaw;
    renderer_desc.views.panini_d = resolved.settings.panini_d;
    renderer_desc.views.peripheral_lod = resolved.settings.peripheral_lod;
    if (!view_renderer.create(device, scene, resolved, renderer_desc, &error)) {
      exit_code = fail("renderer", error);
      break;
    }
    auto camera_at = [&](u32 f) {
      return have_path
                 ? renderer::camera_path_frame(path, f, frames)
                 : renderer::orbit_camera(scene_data.center, scene_data.radius, options.orbit, f);
    };

    if (interactive.on) {
      // ---- a recorded session, flown again ----------------------------------------------------
      if (!session.start(interactive.map, interactive.header, &error)) {
        exit_code = fail("replay", error);
        break;
      }
      if (!options.benchmark.empty()) {
        const bench::MachineState before = wait_for_quiet(options.wait_quiet_s);
        if (options.require_quiet && !bench::is_quiet(before, bench::QuietThresholds{})) {
          std::fprintf(stderr, "engine-view: the machine is busy, and --require-quiet: %s\n",
                       bench::describe(before).c_str());
          exit_code = k_exit_busy;
          break;
        }
        machine_start = before;
      }
      const u64 end = interactive.header.ticks;
      records.reserve(frames);
      view_renderer.reset_stats();
      PendingFrame pending[k_pending_frames];
      u64 folded = view_renderer.stats().folded;
      u64 submissions = 0;
      u32 cursor = 0;
      i64 previous_start = 0;
      const i64 started = time::monotonic_ns();
      bool ok = true;
      for (u32 f = 0; f < frames; ++f) {
        const i64 start = time::monotonic_ns();
        view_renderer.begin_frame();
        take_folded(view_renderer, folded, pending, records);
        const i64 ready = time::monotonic_ns();
        const u64 before_tick = session.tick().value;
        const u64 to = before_tick + ticks_per_frame < end ? before_tick + ticks_per_frame : end;
        cursor = session.run(interactive.log.events(), cursor, SimTick{to});
        renderer::FrameDesc frame;
        frame.camera = session.camera();
        frame.frame_index = session.frame_index();
        if (view_renderer.submit_frame(frame, &error) == 0) {
          ok = false;
          break;
        }
        PendingFrame& p = pending[submissions % k_pending_frames];
        p.submission = submissions;
        p.frame = f;
        p.ticks = static_cast<u32>(session.tick().value - before_tick);
        p.time = static_cast<f64>(session.tick().value) / static_cast<f64>(tick_hz);
        p.cpu_ms = static_cast<f64>(time::monotonic_ns() - ready) / 1.0e6;
        p.frame_ms = previous_start != 0 ? static_cast<f64>(start - previous_start) / 1.0e6 : 0.0;
        previous_start = start;
        ++submissions;
        ++rendered;
      }
      if (!ok) {
        exit_code = fail("frame", error);
        break;
      }
      // The last frames' numbers fold in when their slots come around again; bring those around
      // with nothing drawn in them, rather than drawing frames nobody asked for.
      for (u32 d = 0; d < k_frames_in_flight; ++d) {
        view_renderer.begin_frame();
        take_folded(view_renderer, folded, pending, records);
        view_renderer.abort_frame();
      }
      view_renderer.wait_idle();
      timed_seconds = static_cast<f64>(time::monotonic_ns() - started) / 1.0e9;
      if (!options.benchmark.empty()) {
        view_renderer.sample_gpu_memory();
        machine_end = bench::sample_machine_state(bench::k_sample_window_ms);
        measured = true;
      }
    } else if (!options.benchmark.empty()) {
      // ---- the timed pass -----------------------------------------------------------------
      bench::MachineState before = wait_for_quiet(options.wait_quiet_s);
      if (options.require_quiet && !bench::is_quiet(before, bench::QuietThresholds{})) {
        std::fprintf(stderr, "engine-view: the machine is busy, and --require-quiet: %s\n",
                     bench::describe(before).c_str());
        exit_code = k_exit_busy;
        break;
      }
      machine_start = before;
      renderer::FlightOptions flight_options;
      flight_options.frames = frames;
      flight_options.repeats = repeats;
      flight_options.warmup = options.warmup;
      flight_options.warmup_seconds = static_cast<f64>(options.warmup_seconds);
      flight_options.frames_in_flight = k_frames_in_flight;
      renderer::Flight flight;
      if (!renderer::fly_camera_path(view_renderer, path, flight_options, flight, &error)) {
        exit_code = fail("frame", error);
        break;
      }
      records = std::move(flight.records);
      timed_seconds = flight.seconds;
      view_renderer.sample_gpu_memory();
      machine_end = bench::sample_machine_state(bench::k_sample_window_ms);
      rendered = flight.submitted;
      measured = true;

      // ---- the census: the same path again, one frame at a time, each list read back ------
      if (options.census) {
        if (!census.create(device, scene, &error)) {
          exit_code = fail("census", error);
          break;
        }
        // The same warm-up the timed repeats had, so the occlusion history the census starts
        // from is theirs and its visible pairs can be compared with theirs frame by frame.
        bool ok = true;
        for (u32 w = 0; w < options.warmup && ok; ++w) {
          view_renderer.begin_frame();
          renderer::FrameDesc frame;
          frame.camera = camera_at(0);
          frame.frame_index = 0;
          ok = view_renderer.submit_frame(frame, &error) != 0;
        }
        view_renderer.wait_idle();
        Vector<Vector<u32>> levels_of(frames);
        Vector<Vector<u32>> meshes_of(frames);
        Vector<Vector<u32>> pixels_of(frames);
        Vector<u32> pairs_of(frames, 0u);
        renderer::CaptureChannels id_channel{false, true, false, false};
        for (u32 f = 0; f < frames && ok; ++f) {
          renderer::FrameDesc frame;
          frame.camera = camera_at(f);
          frame.frame_index = f;
          // With pixels the frame is drawn by the capture, which reads the ids back after it;
          // either way it is one frame per camera, as in the timed pass, so the history matches.
          renderer::CapturedFrame shot;
          ok = options.census_pixels ? view_renderer.capture(frame, id_channel, shot, &error)
                                     : view_renderer.render_offscreen(frame, &error);
          ok = ok && census.count(scene_data, scene, view_renderer.stats(), levels_of[f],
                                  meshes_of[f], &error);
          pairs_of[f] = view_renderer.stats().visible_pairs();
          if (ok && options.census_pixels) {
            pixels_of[f].assign(scene_data.parts.size(), 0u);
            const u32 pixels = shot.width * shot.height;
            for (u32 p = 0; p < pixels; ++p) {
              const u32 instance = shot.ids[p * renderer::k_id_words];
              if (instance < scene_data.instances.size())
                ++pixels_of[f][scene_data.instances[instance].mesh];
            }
          }
        }
        if (!ok) {
          exit_code = fail("census", error);
          break;
        }
        summary.census = true;
        for (scene::FrameRecord& record : records) {
          if (record.frame >= frames) continue;
          record.levels = levels_of[record.frame];
          record.meshes = meshes_of[record.frame];
          record.pixels = pixels_of[record.frame];
          if (record.repeat == 0 && record.visible_pairs != pairs_of[record.frame])
            ++summary.census_mismatched_frames;
        }
      }
    } else if (!options.verify_occlusion && options.marker_captures.empty()) {
      // ---- a plain offscreen run: the frames, and the last one captured -------------------
      view_renderer.reset_stats();
      u64 last = 0;
      for (u32 f = 0; f < frames; ++f) {
        view_renderer.begin_frame();
        renderer::FrameDesc frame;
        frame.camera = camera_at(f);
        frame.frame_index = f;
        last = view_renderer.submit_frame(frame, &error);
        if (last == 0) break;
        ++rendered;
      }
      if (last == 0) {
        exit_code = fail("frame", error);
        break;
      }
      view_renderer.wait(last);
      view_renderer.collect_visible();
    }

    // ---- the occlusion check: every frame of the path with and without it --------------------
    if (options.verify_occlusion) {
      renderer::SceneRenderer::Desc off_desc = renderer_desc;
      off_desc.page_source = nullptr;
      if (!renderer_off.create(device, scene_off, resolved_off, off_desc, &error)) {
        exit_code = fail("renderer", error);
        break;
      }
      scene::OcclusionCheck check;
      check.frames = frames;
      check.width = view_renderer.width();
      check.height = view_renderer.height();
      check.covered_min = ~u32{0};
      renderer::CaptureChannels channels;
      channels.ids = true;
      channels.depth = true;
      bool ok = true;
      for (u32 f = 0; f < frames && ok; ++f) {
        renderer::FrameDesc frame;
        frame.camera = camera_at(f);
        frame.frame_index = f;
        renderer::CapturedFrame a;
        renderer::CapturedFrame b;
        ok = view_renderer.capture(frame, channels, a, &error) &&
             renderer_off.capture(frame, channels, b, &error);
        if (!ok) break;
        u64 surface = 0;
        u64 colour = 0;
        u64 triangle = 0;
        scene::OcclusionFrame differing;
        differing.frame = f;
        differing.lost.assign(scene_data.parts.size(), 0u);
        // A capture is at most 16384 x 16384, so a pixel's first id word fits a u32 index.
        const u32 pixels = a.width * a.height;
        for (u32 p = 0; p < pixels; ++p) {
          u32 pixel_colour = 0;
          for (u32 c = 0; c < 4; ++c)
            pixel_colour += a.color[p * 4 + c] != b.color[p * 4 + c] ? 1u : 0u;
          colour += pixel_colour;
          const u32* ia = &a.ids[p * renderer::k_id_words];
          const u32* ib = &b.ids[p * renderer::k_id_words];
          const bool same_surface = ia[0] == ib[0] && ia[1] == ib[1];
          surface += same_surface ? 0u : 1u;
          triangle += same_surface && ia[2] != ib[2] ? 1u : 0u;
          if (same_surface) continue;
          // Reversed-Z: the larger depth is the nearer surface.
          if (b.depth[p] > a.depth[p]) {
            ++differing.lost_nearer;
          } else if (b.depth[p] < a.depth[p]) {
            ++differing.gained_nearer;
          } else {
            ++differing.tied;
          }
          if (ib[0] < scene_data.instances.size())
            ++differing.lost[scene_data.instances[ib[0]].mesh];
        }
        check.lost_nearer += differing.lost_nearer;
        check.tied += differing.tied;
        check.gained_nearer += differing.gained_nearer;
        check.frames_culled_visible +=
            differing.lost_nearer + differing.gained_nearer > 0 ? 1u : 0u;
        if (surface + colour > 0) {
          differing.surface = static_cast<u32>(surface);
          differing.colour = static_cast<u32>(colour);
          check.differing.push_back(std::move(differing));
        }
        check.frames_differing += surface + colour > 0 ? 1u : 0u;
        check.surface_differences += surface;
        check.colour_differences += colour;
        check.triangle_differences += triangle;
        check.covered_min = a.covered < check.covered_min ? a.covered : check.covered_min;
        check.covered_max = a.covered > check.covered_max ? a.covered : check.covered_max;
      }
      if (!ok) {
        exit_code = fail("verify-occlusion", error);
        break;
      }
      if (check.covered_min == ~u32{0}) check.covered_min = 0;
      summary.occlusion_check = check;
    }

    // ---- a picture of every marker of a replay: the camera after the tick it was pressed on ----
    //
    // Drawn from the marker's own camera and clock (the tick at 60 Hz), after the frames and not
    // among them, so the picture depends on the tick sequence alone — which is what makes two
    // replays' captures the same bytes.
    if (interactive.on && !options.marker_captures.empty()) {
      if (io::make_directories(options.marker_captures) != io::Status::Ok) {
        exit_code = fail("marker-captures", "cannot create " + options.marker_captures);
        break;
      }
      bool ok = true;
      for (const view::FlyMarker& marker : session.trajectory().markers) {
        renderer::FrameDesc frame;
        frame.camera =
            view::fly_view(marker.state, interactive.header.fov_y, interactive.header.znear);
        frame.frame_index = marker.tick * view::k_frame_index_hz / tick_hz;
        for (u32 k = 0; k < 8 && ok; ++k)
          ok = view_renderer.render_offscreen(frame, &error);
        renderer::CapturedFrame shot;
        ok = ok && view_renderer.capture(frame, renderer::CaptureChannels{}, shot, &error);
        if (!ok) break;
        char name[40];
        std::snprintf(name, sizeof(name), "tick-%07llu.png",
                      static_cast<unsigned long long>(marker.tick));
        const std::string file = io::join_path(options.marker_captures, name);
        if (image::write_png(file, shot.width, shot.height, 4,
                             std::span<const u8>(shot.color.data(), shot.color.size())) !=
            io::Status::Ok) {
          error = "cannot write " + file;
          ok = false;
          break;
        }
      }
      if (!ok) {
        exit_code = fail("marker-captures", error);
        break;
      }
    }
    // ---- a picture of every marker, for the README and the write-up -------------------------
    if (!interactive.on && !options.marker_captures.empty()) {
      if (!have_path) {
        exit_code = fail("marker-captures", "there is no camera path to take markers from");
        break;
      }
      if (io::make_directories(options.marker_captures) != io::Status::Ok) {
        exit_code = fail("marker-captures", "cannot create " + options.marker_captures);
        break;
      }
      bool ok = true;
      for (const renderer::CameraPathMarker& marker : path.markers) {
        const u32 f = renderer::marker_frame(path, marker.frame, frames);
        renderer::FrameDesc frame;
        frame.camera = camera_at(f);
        frame.frame_index = f;
        // A few frames at the marker's camera first, so a streamed scene has its pages and the
        // occlusion history is this view's, and then the picture.
        for (u32 k = 0; k < 8 && ok; ++k)
          ok = view_renderer.render_offscreen(frame, &error);
        renderer::CapturedFrame shot;
        ok = ok && view_renderer.capture(frame, renderer::CaptureChannels{}, shot, &error);
        if (!ok) break;
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%05u-", f);
        const std::string file = io::join_path(
            options.marker_captures, std::string(prefix) + file_stem(marker.name) + ".png");
        if (image::write_png(file, shot.width, shot.height, 4,
                             std::span<const u8>(shot.color.data(), shot.color.size())) !=
            io::Status::Ok) {
          error = "cannot write " + file;
          ok = false;
          break;
        }
      }
      if (!ok) {
        exit_code = fail("marker-captures", error);
        break;
      }
    }
    if (!options.capture.empty()) {
      renderer::FrameDesc frame;
      frame.camera = camera_at(frames > 0 ? frames - 1 : 0);
      frame.frame_index = frames > 0 ? frames - 1 : 0;
      if (interactive.on) {  // where the replay ended
        frame.camera = session.camera();
        frame.frame_index = session.frame_index();
      }
      renderer::CapturedFrame shot;
      if (!view_renderer.capture(frame, renderer::CaptureChannels{}, shot, &error)) {
        exit_code = fail("capture", error);
        break;
      }
      const io::Status status =
          image::write_png(options.capture, shot.width, shot.height, 4,
                           std::span<const u8>(shot.color.data(), shot.color.size()));
      if (status != io::Status::Ok) {
        exit_code = fail("capture", std::string("cannot write ") + options.capture + ": " +
                                        io::status_name(status));
        break;
      }
      captured = true;
    }
  } while (false);

  // ---- the summary: what was measured, on what, beside what ---------------------------------
  if (exit_code == 0) {
    // A replay's "path" is its log: the file's bytes name what was flown, as a path file's do.
    const std::string path_name =
        interactive.on ? options.replay_input : (have_path ? path.name : std::string());
    const u64 path_hash = interactive.on ? interactive.log_hash : (have_path ? path.hash : 0);
    describe_run(scene_data, options, path_name, path_hash, resolved, view_renderer, summary);
    summary.frames = frames;
    summary.repeats = measured ? repeats : 0;
    summary.warmup = measured && !interactive.on ? options.warmup : 0;
    if (interactive.on) summary.interactive = interactive_summary(interactive, session, options);
    if (measured) {
      renderer::summarize_frames(
          std::span<const scene::FrameRecord>(records.data(), records.size()), frames, repeats,
          path, summary);
      summary.seconds = timed_seconds;
      summary.wall_ms_per_frame =
          frames > 0 ? timed_seconds * 1000.0 / (static_cast<f64>(frames) * repeats) : 0.0;
      const renderer::Stats& stats = view_renderer.stats();
      summary.gpu_memory_used_mib = stats.gpu_memory.used_mib;
      summary.gpu_memory_budget_mib = stats.gpu_memory.budget_mib;
      JsonValue machine = JsonValue::object();
      machine.set("start", bench::machine_state_json(machine_start));
      machine.set("end", bench::machine_state_json(machine_end));
      summary.machine_state = std::move(machine);
      summary.quiet =
          bench::is_quiet(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{});
    }
    summary_line = write_json(schema::to_json(summary), JsonWriteOptions{.pretty = false});
    if (!options.benchmark.empty()) {
      const io::Status status = write_benchmark(
          options.benchmark, std::span<const scene::FrameRecord>(records.data(), records.size()),
          summary_line);
      if (status != io::Status::Ok) {
        exit_code = fail("benchmark", std::string("cannot write ") + options.benchmark + ": " +
                                          io::status_name(status));
      }
    }
  }
  census.destroy();
  renderer_off.destroy();
  view_renderer.destroy();
  page_source.destroy();
  scene_off.destroy();
  scene.destroy();
  device.destroy();
  if (exit_code == 0) {
    // Everything bound for stderr first — the note a plain run's readers look for, the busy
    // caveat, and anything the teardown above logged — and the summary line last and whole, as
    // the windowed path does and for the same reason.
    if (!measured) {
      std::fprintf(stderr, "engine-view: offscreen, %llu frames, captured %s\n",
                   static_cast<unsigned long long>(rendered), captured ? "yes" : "no");
    } else {
      (void)bench::warn_if_busy(bench::worst_of(machine_start, machine_end),
                                bench::QuietThresholds{}, stderr);
    }
    std::printf("%s\n", summary_line.c_str());
    std::fflush(stdout);
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
               a == "--grid" || a == "--grid-instances" || a == "--deform-pool-mib" ||
               a == "--rt-budget-mib") {
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
      if (a == "--grid") options.procedural_given = true;
      if (a == "--grid-instances") options.grid_instances = n;
      // The flag is mebibytes, the setting kibibytes: a caller of the module may want a finer
      // budget than a whole MiB (and a test needs one), while a person at a command line does not.
      if (a == "--deform-pool-mib") options.settings.deform_pool_kib = n * 1024;
      if (a == "--rt-budget-mib") options.settings.rt_budget_mib = n;
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
    } else if (a == "--side-yaw" || a == "--panini-d" || a == "--peripheral-lod" ||
               a == "--warmup-seconds") {
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
      if (a == "--warmup-seconds") options.warmup_seconds = v;
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
        std::fprintf(stderr, "engine-view: --shadows expects off, rt, or csm\n");
        return k_exit_usage;
      }
    } else if (a == "--shadow-cascades" || a == "--shadow-map") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      u32 n = 0;
      const bool cascades = a == "--shadow-cascades";
      if (!parse_u32(value, n) || (cascades && (n < 1 || n > 4)) ||
          (!cascades && (n < 64 || n > renderer::k_max_shadow_map))) {
        std::fprintf(stderr, cascades ? "engine-view: --shadow-cascades expects 1 to 4\n"
                                      : "engine-view: --shadow-map expects 64 to 4096 texels\n");
        return k_exit_usage;
      }
      (cascades ? options.settings.shadow_cascades : options.settings.shadow_map) = n;
    } else if (a == "--shadow-distance") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!parse_f32(value, options.settings.shadow_distance)) {
        std::fprintf(stderr, "engine-view: --shadow-distance expects a positive distance\n");
        return k_exit_usage;
      }
    } else if (a == "--view") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!renderer::parse_view_mode(value, options.settings.view_mode)) {
        std::fprintf(
            stderr, "engine-view: --view expects id, tri, depth, shaded, normals, uv, or shadow\n");
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
      options.procedural_given = true;
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
    } else if (a == "--no-shadow-casters") {
      options.settings.shadow_casters = false;
    } else if (a == "--no-lights") {
      options.settings.lights = false;
    } else if (a == "--validation") {
      options.validation = true;
    } else if (a == "--camera-path") {
      if (!next_value(argc, argv, i, a, options.camera_path)) return k_exit_usage;
    } else if (a == "--overlay") {
      if (!next_value(argc, argv, i, a, options.overlay)) return k_exit_usage;
    } else if (a == "--offscreen") {
      options.offscreen = true;
    } else if (a == "--benchmark") {
      // Offscreen for a flythrough and a replay; a live session measures its window. Resolved
      // after the loop, once `--interactive` may have been seen.
      if (!next_value(argc, argv, i, a, options.benchmark)) return k_exit_usage;
    } else if (a == "--marker-captures") {
      if (!next_value(argc, argv, i, a, options.marker_captures)) return k_exit_usage;
    } else if (a == "--interactive") {
      options.interactive = true;
    } else if (a == "--record-input") {
      if (!next_value(argc, argv, i, a, options.record_input)) return k_exit_usage;
    } else if (a == "--replay-input") {
      if (!next_value(argc, argv, i, a, options.replay_input)) return k_exit_usage;
      options.interactive = true;
    } else if (a == "--inject-input") {
      if (!next_value(argc, argv, i, a, options.inject_input)) return k_exit_usage;
    } else if (a == "--input-map") {
      if (!next_value(argc, argv, i, a, options.input_map)) return k_exit_usage;
    } else if (a == "--tunables") {
      if (!next_value(argc, argv, i, a, options.tunables_file)) return k_exit_usage;
    } else if (a == "--tunable") {
      if (!next_value(argc, argv, i, a, options.tunable_overrides)) return k_exit_usage;
    } else if (a == "--census") {
      options.census = true;
    } else if (a == "--census-pixels") {
      options.census = true;
      options.census_pixels = true;
    } else if (a == "--verify-occlusion") {
      options.verify_occlusion = true;
    } else if (a == "--require-quiet") {
      options.require_quiet = true;
    } else if (a == "--repeat" || a == "--warmup" || a == "--wait-quiet") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      u32 n = 0;
      if (!parse_u32(value, n) || (a == "--repeat" && (n == 0 || n > 100))) {
        std::fprintf(stderr, "engine-view: %.*s expects a number%s\n", static_cast<int>(a.size()),
                     a.data(), a == "--repeat" ? " within 1..100" : "");
        return k_exit_usage;
      }
      if (a == "--repeat") options.repeats = n;
      if (a == "--warmup") options.warmup = n;
      if (a == "--wait-quiet") options.wait_quiet_s = n;
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
  // ---- the interactive camera's flags ----------------------------------------------------------
  // A live session is somebody at a window; a replay is a file. Everything below follows from
  // which one this is, which is why `--benchmark` and `--marker-captures` are resolved here.
  const bool live = options.interactive && options.replay_input.empty();
  if (options.interactive &&
      (options.animate || options.morph_animate || options.reference != 0 ||
       options.fly_frames > 0 || options.census || options.verify_occlusion)) {
    std::fprintf(stderr,
                 "engine-view: --interactive and --replay-input fly the camera themselves, a fixed "
                 "tick at a time; --animate, --morph-animate, --reference, --fly, --census and "
                 "--verify-occlusion run frame loops of their own\n");
    return k_exit_usage;
  }
  if (live && options.offscreen) {
    std::fprintf(stderr,
                 "engine-view: --interactive needs a window, since nobody can fly one that is not "
                 "there; replay a recording offscreen with --replay-input <log> --offscreen\n");
    return k_exit_usage;
  }
  if (live && !options.marker_captures.empty()) {
    std::fprintf(stderr,
                 "engine-view: a live session draws into its window and has no offscreen target "
                 "to capture a marker from; record it with --record-input, then replay it with "
                 "--replay-input <log> --marker-captures <dir>\n");
    return k_exit_usage;
  }
  if (!options.record_input.empty() && !live) {
    std::fprintf(stderr,
                 "engine-view: --record-input records a live --interactive session; a replay is "
                 "already a recording\n");
    return k_exit_usage;
  }
  if (!options.inject_input.empty() && !live) {
    std::fprintf(stderr,
                 "engine-view: --inject-input feeds the window of a live --interactive "
                 "session\n");
    return k_exit_usage;
  }
  if (!options.replay_input.empty() && !options.camera_path.empty()) {
    std::fprintf(stderr,
                 "engine-view: a replay starts where its recording started; --camera-path would "
                 "start it somewhere else\n");
    return k_exit_usage;
  }
  if (!options.interactive && !options.input_map.empty()) {
    std::fprintf(stderr,
                 "engine-view: --input-map is the interactive camera's bindings; add "
                 "--interactive or --replay-input\n");
    return k_exit_usage;
  }
  if (!live && (!options.benchmark.empty() || !options.marker_captures.empty() ||
                options.verify_occlusion)) {
    options.offscreen = true;
  }
  if (!options.benchmark.empty() && options.camera_path.empty() && !options.interactive) {
    std::fprintf(stderr,
                 "engine-view: --benchmark flies a camera path; name one with --camera-path (a "
                 "still camera is render.benchmark's)\n");
    return k_exit_usage;
  }
  if (options.census && options.benchmark.empty()) {
    std::fprintf(stderr, "engine-view: --census reads back a --benchmark run's frames\n");
    return k_exit_usage;
  }
  if (options.reference != 0 && !options.camera_path.empty()) {
    std::fprintf(stderr,
                 "engine-view: --reference renders one frame from the orbit camera; fly a path "
                 "with --offscreen or in the window\n");
    return k_exit_usage;
  }
  if (options.offscreen && (options.animate || options.morph_animate || options.reference != 0)) {
    std::fprintf(stderr,
                 "engine-view: --offscreen, --benchmark, --verify-occlusion and --marker-captures "
                 "draw rigid scenes; --animate, --morph-animate and --reference have their own "
                 "paths\n");
    return k_exit_usage;
  }
  // An interactive session ends when its log does or its window closes, not after a default count.
  if (!options.capture.empty() && options.frames == 0 && options.camera_path.empty() &&
      !options.interactive) {
    options.frames = 60;
  }
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

  // Tunables before anything reads one: the renderer's, and the interactive camera's, which a
  // live session reads once as it starts.
  if (!options.tunables_file.empty()) {
    Vector<std::string> problems;
    if (!tunables::load_file(options.tunables_file.c_str(), &problems)) {
      for (const std::string& problem : problems)
        std::fprintf(stderr, "engine-view: --tunables: %s\n", problem.c_str());
      log::remove_sink(&stderr_sink);
      return k_exit_usage;
    }
  }
  if (!options.tunable_overrides.empty()) {
    std::string problem;
    if (!tunables::apply_overrides(options.tunable_overrides, &problem)) {
      std::fprintf(stderr, "engine-view: --tunable: %s\n", problem.c_str());
      log::remove_sink(&stderr_sink);
      return k_exit_usage;
    }
  }
  Interactive interactive;
  if (options.interactive) {
    const int prepared = prepare_interactive(options, interactive);
    if (prepared != 0) {
      log::remove_sink(&stderr_sink);
      return prepared;
    }
  }

  // The reference path never opens a window, so it comes before the display is even asked for,
  // and neither does the offscreen one.
  if (options.reference != 0) return run_reference(options);
  if (options.offscreen) {
    const int code = run_offscreen(options, interactive);
    log::remove_sink(&stderr_sink);
    return code;
  }

  std::string error;
  if (!window::init(&error)) {
    if (live) {
      // Said once, beside the reason, because the build may have no display backend at all
      // (ENGINE_WINDOW_BACKENDS=none on the headless server): a recording still replays there.
      std::fprintf(stderr,
                   "engine-view: --interactive needs a display to fly in; a recorded session "
                   "replays without one: --replay-input <log.jsonl> --offscreen\n");
    }
    return unavailable("no display", error);
  }
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
  renderer::CameraPath window_path;  // --camera-path; empty keys: the orbit or the fly-in
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
  u64 texture_bytes = 0;  // the materials' textures on the device (texture.md)
  u32 textures_built = 0;
  u32 textures_decoded = 0;
  std::string views_text = "{}";
  std::string streaming_text =
      write_json(streaming_summary(renderer::StreamStats{}), JsonWriteOptions{.pretty = false});
  std::string anim_text = "null";
  u32 skinned_instances = 0;
  u32 joint_matrices = 0;
  std::string clip_text;
  // ---- the interactive camera (fly_camera.h), with --interactive or --replay-input ----------
  view::FlySession session;
  view::EdgeEvents edge;               // live: converted this frame, fed at the next tick
  input::InputLog recording;           // --record-input
  view::FramePacing pacing;            // the title's last-frame and p99 numbers
  Vector<scene::FrameRecord> records;  // --benchmark
  std::string interactive_text = "null";
  bool pointer_captured = false;  // the edge converts pointer motion only while this is true
  bool pointer_grabbed = false;   // what the window was last asked for
  // An injected run never takes the real pointer: it happens on somebody's desktop, and the
  // capture it toggles is only the edge's decision about which motion is looking.
  const bool grab_pointer = options.inject_input.empty();

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
      if (!renderer::read_scene_file(options.scene, renderer::SceneFileOptions{options.overlay},
                                     desc, error)) {
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
    // A camera path, read against the scene it flies over: a key may hold a height above its
    // terrain. With no `--frames` the window flies the path once at its own rate and closes.
    if (!options.camera_path.empty()) {
      if (!renderer::read_camera_path(options.camera_path,
                                      scene_data.terrain.enabled ? &scene_data.terrain : nullptr,
                                      window_path, error)) {
        exit_code = fail("camera path", error);
        break;
      }
      // An interactive session only starts at the path's first camera; it ends when it ends.
      if (options.frames == 0 && !interactive.on) options.frames = window_path.frame_count();
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
      const std::string why = renderer::unavailable_reason(availability, device);
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
    texture_bytes = scene.texture_bytes();
    textures_built = scene.textures_built();
    textures_decoded = scene.textures_decoded();
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
                    log::field("shadows", renderer::resolved_shadow_name(resolved)),
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

    // ---- the interactive camera: its header, its clock, and where it ends ----------------------
    // A live session's header is built here, where the scene's bounds and the camera path are
    // known; a replay's came out of its log. `session_end` is the tick a replay or an injected
    // session stops at, and 0 for somebody at the window, whose session ends when they close it.
    u64 session_end = 0;
    if (interactive.on) {
      if (!interactive.replay) {
        interactive.header =
            live_header(options, scene_data, window_path.keys.empty() ? nullptr : &window_path);
      }
      if (!session.start(interactive.map, interactive.header, &error)) {
        exit_code = fail("interactive", error);
        break;
      }
      session_end = interactive.replay ? interactive.header.ticks : interactive.inject_end;
      if (!options.benchmark.empty()) records.reserve(1u << 14);  // a minute at 240 Hz
      ENGINE_LOG_INFO(log_view, "interactive", log::field("replay", interactive.replay),
                      log::field("tick_hz", interactive.header.params.tick_hz),
                      log::field("speed", interactive.header.params.speed),
                      log::field("map", interactive.map.hash()));
    }
    const u32 session_hz =
        interactive.header.params.tick_hz > 0 ? interactive.header.params.tick_hz : 240u;
    // The ticks come from a fixed-step clock fed with wall time: a frame runs however many ticks
    // are due, none when it came sooner than one and several after a slow one — capped at a
    // quarter of a second's worth, past which a stall is dropped rather than flown through.
    FixedStepClock clock(session_hz, session_hz / 4 > 0 ? session_hz / 4 : 1u);
    i64 clock_ns = 0;
    u32 replay_cursor = 0;
    u32 inject_cursor = 0;
    u32 capture_seen = 0;
    PendingFrame pending[k_pending_frames];
    u64 folded = view_renderer.stats().folded;
    u64 submissions = 0;
    i64 previous_frame_ns = 0;
    i64 last_title_ns = 0;

    bool running = true;
    bool resize_pending = false;
    i64 last_shader_poll_ns = 0;
    started_ns = time::monotonic_ns();
    clock_ns = started_ns;
    while (running) {
      const i64 frame_start = time::monotonic_ns();
      // --inject-input: the log's keys and motion go onto the window's own queue as their ticks
      // come due, and come back out of poll() below like anything the platform delivered.
      const std::span<const input::RawEvent> injected = interactive.injected.events();
      while (inject_cursor < injected.size() && injected[inject_cursor].tick <= session.next()) {
        window::Event synthetic;
        if (view::window_event_of(injected[inject_cursor], synthetic)) {
          (void)window.push_event(synthetic);
        }
        ++inject_cursor;
      }
      window::Event event;
      while (window.poll(event)) {
        switch (event.kind) {
          case window::EventKind::Quit:
          case window::EventKind::CloseRequested: running = false; break;
          case window::EventKind::Resized: resize_pending = true; break;
          case window::EventKind::KeyDown:
            // Escape is an action in an interactive session (it takes and gives back the pointer),
            // so there the window's close button is the way out.
            if (!interactive.on && event.key == window::Key::Escape) running = false;
            break;
          case window::EventKind::FocusLost:
            // Whoever switched away gets the pointer back, and Escape takes it again.
            if (live && pointer_captured && grab_pointer) pointer_captured = false;
            break;
          default: break;
        }
        // The one conversion (window_input.h): stamped with the tick it will be fed at.
        if (live) edge.add(event, session.next(), pointer_captured);
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

      // The CPU's share of the frame is everything but the two waits below: for a free frame slot
      // (the GPU) and for a swapchain image (the display).
      const i64 before_waits = time::monotonic_ns();
      view_renderer.begin_frame();
      if (interactive.on && !options.benchmark.empty()) {
        take_folded(view_renderer, folded, pending, records);
      }
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
      const i64 after_waits = time::monotonic_ns();
      extent_width = view_renderer.width();
      extent_height = view_renderer.height();

      renderer::FrameDesc frame;
      u32 ticks_this_frame = 0;
      bool session_done = false;
      if (interactive.on) {
        // ---- the ticks due by now, as late as possible before the frame is recorded -----------
        const i64 now = time::monotonic_ns();
        clock.advance(now - clock_ns);
        clock_ns = now;
        while (clock.step()) {
        }
        u64 to = clock.tick().value;
        if (session_end != 0 && to > session_end) to = session_end;
        const u64 before = session.tick().value;
        if (interactive.replay) {
          replay_cursor = session.run(interactive.log.events(), replay_cursor, SimTick{to});
        } else {
          (void)session.run(edge.events(), 0, SimTick{to});
          if (session.tick().value > before) {
            // Fed, so recorded: what the log holds is exactly what the camera was flown with.
            // Events still waiting for a tick are neither, and a session that ends with some
            // waiting drops them from both.
            if (!options.record_input.empty()) {
              for (const input::RawEvent& e : edge.events())
                recording.record(e);
            }
            edge.clear();
          }
          while (capture_seen != session.capture_presses()) {
            ++capture_seen;
            pointer_captured = !pointer_captured;
          }
          if (grab_pointer && pointer_grabbed != pointer_captured) {
            (void)window.set_relative_mouse(pointer_captured);
            pointer_grabbed = pointer_captured;
          }
        }
        ticks_this_frame = static_cast<u32>(session.tick().value - before);
        session_done = session_end != 0 && session.tick().value >= session_end;
        // The camera at the last tick boundary, not interpolated: the frame shows a state the
        // session actually passed through, which is what a replay's frame shows too.
        frame.camera = session.camera();
        frame.frame_index = session.frame_index();
      } else {
        frame.camera = !window_path.keys.empty()
                           ? renderer::camera_path_frame(
                                 window_path, static_cast<u32>(rendered),
                                 options.frames != 0 ? options.frames : window_path.frame_count())
                       : options.fly_frames > 0
                           ? renderer::fly_camera(scene_data.center, scene_data.radius,
                                                  options.fly_from, options.fly_to,
                                                  static_cast<u32>(rendered), options.fly_frames)
                           : renderer::orbit_camera(scene_data.center, scene_data.radius,
                                                    options.orbit, rendered);
        frame.frame_index = rendered;
      }
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
      if (interactive.on) {
        // ---- pacing: what the title says, and what a --benchmark record carries --------------
        const i64 submitted_ns = time::monotonic_ns();
        const f64 frame_ms = previous_frame_ns != 0
                                 ? static_cast<f64>(frame_start - previous_frame_ns) / 1.0e6
                                 : 0.0;
        previous_frame_ns = frame_start;
        if (frame_ms > 0.0) pacing.add(frame_start, static_cast<f32>(frame_ms));
        if (!options.benchmark.empty()) {
          PendingFrame& p = pending[submissions % k_pending_frames];
          p.submission = submissions;
          p.frame = static_cast<u32>(rendered);
          p.ticks = ticks_this_frame;
          p.time = static_cast<f64>(session.tick().value) / static_cast<f64>(session_hz);
          p.cpu_ms =
              static_cast<f64>((before_waits - frame_start) + (submitted_ns - after_waits)) / 1.0e6;
          p.frame_ms = frame_ms;
        }
        // Four times a second: a title rewritten every frame is unreadable, and setting one is a
        // round trip to the window system.
        if (frame_start - last_title_ns >= 250'000'000) {
          last_title_ns = frame_start;
          char title[256];
          const f32 p99 = pacing.percentile(frame_start, 1'000'000'000, 0.99);
          if (interactive.replay) {
            std::snprintf(title, sizeof(title),
                          "engine-view  %.2f ms  p99 %.2f ms (1 s)  replay tick %llu of %llu",
                          static_cast<f64>(pacing.last_ms()), static_cast<f64>(p99),
                          static_cast<unsigned long long>(session.tick().value),
                          static_cast<unsigned long long>(session_end));
          } else {
            std::snprintf(
                title, sizeof(title), "engine-view  %.2f ms  p99 %.2f ms (1 s)  tick %llu%s  %s",
                static_cast<f64>(pacing.last_ms()), static_cast<f64>(p99),
                static_cast<unsigned long long>(session.tick().value),
                options.record_input.empty() ? "" : "  recording",
                pointer_captured ? "Esc: give the pointer back" : "Esc: take the pointer to look");
          }
          window.set_title(title);
        }
      }
      ++submissions;
      ++rendered;

      const bool last = (options.frames != 0 && rendered >= options.frames) || session_done;
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
    if (interactive.on && !options.benchmark.empty() && view_renderer.valid() && exit_code == 0) {
      // The last frames' GPU numbers fold in when their slots come around again: bring those
      // around with nothing drawn and nothing presented, and the session's last frames have
      // records too.
      for (u32 d = 0; d < k_frames_in_flight; ++d) {
        view_renderer.begin_frame();
        take_folded(view_renderer, folded, pending, records);
        view_renderer.abort_frame();
      }
    }
    if (view_renderer.valid()) view_renderer.wait_idle();
    view_renderer.sample_gpu_memory();
    if (grab_pointer && pointer_grabbed) (void)window.set_relative_mouse(false);
    if (interactive.on && exit_code == 0) {
      interactive_text = write_json(interactive_summary(interactive, session, options),
                                    JsonWriteOptions{.pretty = false});
      if (!options.record_input.empty() &&
          !save_recording(options, interactive, session, recording, error)) {
        exit_code = fail("record-input", error);
      }
    }
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

    // ---- an interactive session's --benchmark: the flythrough's JSONL, frame by frame ---------
    // One record per frame the window presented (the GPU's passes, the CPU's milliseconds, the
    // frame time, the ticks it consumed, visible pairs, streaming), then the same summary line a
    // flythrough ends with, so tools/flythrough.ps1's readers read a session without a new case.
    if (interactive.on && !options.benchmark.empty() && exit_code == 0) {
      scene::FlythroughSummary summary;
      summary.format = "engine.flythrough.v1";
      describe_run(scene_data, options,
                   interactive.replay ? options.replay_input : std::string("interactive"),
                   interactive.replay ? interactive.log_hash : 0, resolved, view_renderer, summary);
      const u32 frames = static_cast<u32>(rendered);
      renderer::summarize_frames(
          std::span<const scene::FrameRecord>(records.data(), records.size()), frames, 1,
          renderer::CameraPath{}, summary);
      summary.frames = frames;
      summary.repeats = 1;
      summary.seconds = static_cast<f64>(finished_ns - started_ns) / 1.0e9;
      summary.wall_ms_per_frame = frames > 0 ? summary.seconds * 1000.0 / frames : 0.0;
      summary.gpu_memory_used_mib = view_renderer.stats().gpu_memory.used_mib;
      summary.gpu_memory_budget_mib = view_renderer.stats().gpu_memory.budget_mib;
      JsonValue machine = JsonValue::object();
      machine.set("start", bench::machine_state_json(machine_start));
      machine.set("end", bench::machine_state_json(machine_end));
      summary.machine_state = std::move(machine);
      summary.quiet =
          bench::is_quiet(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{});
      summary.interactive = interactive_summary(interactive, session, options);
      const io::Status status = write_benchmark(
          options.benchmark, std::span<const scene::FrameRecord>(records.data(), records.size()),
          write_json(schema::to_json(summary), JsonWriteOptions{.pretty = false}));
      if (status != io::Status::Ok) {
        exit_code = fail("benchmark", std::string("cannot write ") + options.benchmark + ": " +
                                          io::status_name(status));
      }
    }
  } while (false);

  const renderer::Stats stats = view_renderer.stats();
  // The chain's bytes move with its capacity, so the figure is the run's last, not the load's.
  if (stats.rt.bytes > 0) rt_bytes = stats.rt.bytes;
  const std::string rt_text = write_json(rt_summary(stats.rt), JsonWriteOptions{.pretty = false});
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
    // stdout is the summary; the caveat goes beside it on stderr, the same line and the same
    // thresholds the bench harness prints — **before** the summary, which is the last thing out
    // and goes out whole. Scripts and the end-to-end tests read the last line of the two streams
    // merged, and stdout into a pipe is fully buffered: printed first and flushed at exit, as it
    // used to be, a summary longer than the buffer (an interactive one is) had the caveat spliced
    // into its middle.
    (void)bench::warn_if_busy(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{},
                              stderr);
    std::printf(
        "{\"frames\":%llu,\"seconds\":%.3f,\"avg_ms\":%.3f,\"width\":%u,\"height\":%u,"
        "\"clusters\":%u,\"leaf_clusters\":%u,\"triangles\":%u,\"lod_levels\":%u,\"build_ms\":%.1f,"
        "\"mesh_primitives\":%u,\"mesh_cache\":\"%s\",\"meshes\":%u,\"instances\":%u,\"pairs\":%u,"
        "\"cull\":%s,\"occlusion\":%s,\"cone\":%s,\"lod_px\":%.2f,\"raster\":\"%s\","
        "\"shadows\":\"%s\",\"shadow_casters\":%s,\"shadow_cascades\":%u,\"shadow_map\":%u,"
        "\"sw_px\":%.1f,"
        "\"visible_hw_last\":%u,\"visible_pass2_last\":%u,\"visible_sw_last\":%u,"
        "\"visible_pairs_last\":%u,\"shadow_casters_last\":%u,\"shadow_pairs_last\":%u,"
        "\"shadow_fallback_last\":%u,\"visible_min\":%u,"
        "\"visible_max\":%u,\"triangles_hw_last\":%u,\"vertex_fallback_last\":%u,"
        "\"deform\":\"%s\",\"deform_pool_bytes\":%llu,"
        "\"deform_whole_mesh_bytes\":%llu,\"deform_pool_used_bytes\":%llu,"
        "\"deform_pool_peak_bytes\":%llu,\"deform_entries\":%u,"
        "\"deform_overflow_entries\":%u,\"deform_overflow_bytes\":%llu,"
        "\"morph_channels\":%u,\"morph_static_cache_bytes\":%llu,"
        "\"morph_cached_instances\":%u,\"morph_clip\":\"%s\",\"rt_templates\":%s,"
        "\"skinned_instances\":%u,\"joints\":%u,\"clip\":\"%s\",\"anim\":%s,"
        "\"template_bytes\":%llu,\"rt_bytes\":%llu,\"geometry_bytes\":%llu,\"stream_bytes\":%llu,"
        "\"textures\":{\"built\":%u,\"decoded\":%u,\"bytes\":%llu},"
        "\"rt\":%s,\"views\":%s,\"streaming\":%s,"
        "\"host_memory\":{\"bytes\":%llu,\"peak_bytes\":%llu},"
        "\"gpu_memory\":{\"budget_mib\":%llu,\"used_mib\":%llu,"
        "\"device_local_total_mib\":%llu},\"machine_state\":%s,"
        "\"gpu_ms\":{\"cull\":%.4f,\"hw\":%.4f,\"sw\":%.4f,\"hiz\":%.4f,\"resolve\":%.4f,"
        "\"rt\":%.4f,\"clas\":%.4f,\"deform\":%.4f,\"deform_alloc\":%.4f,"
        "\"trace\":%.4f,\"shadow\":%.4f,\"shadow_cull\":%.4f,\"total\":%.4f,"
        "\"frames\":%llu},\"captured\":%s,\"interactive\":%s}\n",
        static_cast<unsigned long long>(rendered), seconds, avg_ms, extent_width, extent_height,
        scene_data.cluster_count(), scene_data.leaf_count(), scene_data.lod.leaf_triangle_count,
        scene_data.lod.level_cluster_counts.size(), static_cast<f64>(scene_data.build_ns) / 1.0e6,
        scene_data.mesh_primitives, scene_data.mesh_cache, scene_data.parts.size(),
        scene_data.instances.size(), scene_data.pair_count,
        resolved.settings.cull ? "true" : "false", resolved.occlusion ? "true" : "false",
        resolved.settings.cone ? "true" : "false", static_cast<f64>(resolved.settings.lod_px),
        renderer::raster_name(resolved.settings.raster), renderer::resolved_shadow_name(resolved),
        resolved.casters ? "true" : "false", resolved.shadow_cascades,
        resolved.csm ? resolved.settings.shadow_map : 0u, static_cast<f64>(resolved.settings.sw_px),
        stats.visible_hw, stats.visible_pass2, stats.visible_sw, stats.visible_pairs(),
        stats.shadow_casters, stats.shadow_pairs, stats.shadow_fallback, visible_min,
        stats.visible_max, stats.triangles_hw, stats.vertex_fallback,
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
        static_cast<unsigned long long>(stream_bytes), textures_built, textures_decoded,
        static_cast<unsigned long long>(texture_bytes), rt_text.c_str(), views_text.c_str(),
        streaming_text.c_str(), static_cast<unsigned long long>(host_memory),
        static_cast<unsigned long long>(host_memory_peak),
        static_cast<unsigned long long>(stats.gpu_memory.budget_mib),
        static_cast<unsigned long long>(stats.gpu_memory.used_mib),
        static_cast<unsigned long long>(stats.gpu_memory.device_local_total_mib),
        machine_text.c_str(), stats.cull_ms(), stats.hw_ms(), stats.sw_ms(), stats.hiz_ms(),
        stats.resolve_ms(), stats.rt_ms(), stats.clas_ms(), stats.deform_ms(),
        stats.deform_alloc_ms(), stats.trace_ms(), stats.shadow_ms(), stats.shadow_cull_ms(),
        stats.total_ms(), static_cast<unsigned long long>(stats.timed_frames),
        captured ? "true" : "false", interactive_text.c_str());
    std::fflush(stdout);
  }
  log::remove_sink(&stderr_sink);
  return exit_code;
}
