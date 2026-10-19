// The animated world (animation.h; docs/subsystems/animation.md, "The app-side glue";
// docs/subsystems/apps.md, "--animate" and "Animation in a session").
#include "animation.h"

#if ENGINE_VIEW_ANIMATION

#include "anim_lod.h"

#include <core/hash/hash.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/assets/gltf.h>
#include <domain/assets/mesh_generator.h>
#include <domain/ecs/world_commands.h>
#include <domain/gfx/cluster_cull.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/request.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine::view {

ENGINE_LOG_CATEGORY_DEFINE(log_view_anim, "view.anim");

namespace {

// The library's own name for a file's assets: its stem, never its path (systems/animation's
// `Library::load_gltf` says why), so a clip loaded here has the name and id `load_gltf` gives it.
std::string path_stem(std::string_view path) {
  usize begin = 0;
  for (usize i = 0; i < path.size(); ++i) {
    if (path[i] == '/' || path[i] == '\\') begin = i + 1;
  }
  usize end = path.size();
  for (usize i = end; i > begin; --i) {
    if (path[i - 1] == '.') {
      end = i - 1;
      break;
    }
  }
  return std::string(path.substr(begin, end - begin));
}

// The generator that made mesh `m`, or null for an authored file: the scene's water and terrain
// (`SceneMeshInfo::origin`), and a mesh generator's file (`.flora.json`, `.rock.json`; the
// registry's naming, domain/assets/mesh_generator.h). **A generated mesh is never a clip source**:
// none of them has a skin, and the water's pool is a `.glb` the clip loader used to open, find no
// skin in and fail the whole scene on.
const char* generated_by(const renderer::SceneDesc& desc, u32 m) {
  if (m < desc.mesh_info.size()) {
    const char* origin = desc.mesh_info[m].origin;
    if (std::strcmp(origin, "water") == 0 || std::strcmp(origin, "terrain") == 0) return origin;
  }
  if (!assets::mesh_generator_of(desc.meshes[m]).empty()) return "a mesh generator";
  return nullptr;
}

// What a mesh is to the clip loader.
enum class ClipSource : u8 {
  none,       // not asked about
  candidate,  // `--animate`, or a `--mesh` run: a skinned glTF animates, anything else is passed
              // over
  named,      // an instance's `animation` block plays on it: it must be a skinned glTF
};

// Loads the skins and clips of the meshes that are clip sources (animation.h, "Which meshes are
// clip sources"). A `.clusters` container carries the per-vertex binding stream but neither a
// skeleton nor a curve — those are in the source glTF — so a block that animates names the glTF,
// and the derived-data cache still serves its geometry.
bool load_clips(AnimatedScene& scene, const renderer::SceneDesc& desc,
                std::span<const ClipSource> role, std::span<const u32> named_by,
                std::string& error) {
  scene.mesh_skeleton.resize(desc.meshes.size(), animation::Library::k_not_found);
  for (u32 m = 0; m < desc.meshes.size(); ++m) {
    if (role[m] == ClipSource::none) continue;
    const std::string& path = desc.meshes[m];
    if (path.empty()) continue;  // the procedural heightfield
    const bool named = role[m] == ClipSource::named;
    const std::string who = named ? "instance " + std::to_string(named_by[m]) +
                                        " has an \"animation\" block, but its mesh " + path
                                  : std::string();
    if (const char* generator = generated_by(desc, m); generator != nullptr) {
      if (!named) continue;
      error = who + " is made by " + generator +
              " and has no skin to play a clip on; an animation block belongs on an instance of "
              "a skinned glTF";
      return false;
    }
    if (io::extension(path) == ".clusters") {
      if (!named) continue;
      error = who +
              " is a .clusters container, which carries the skin's bindings but not its skeleton "
              "or its clips; name the skinned glTF it was built from";
      return false;
    }
    assets::MeshData mesh;
    std::string why;
    if (!assets::load_gltf(path, mesh, &why)) {
      if (!named) {
        ENGINE_LOG_INFO(log_view_anim, "not a clip source", log::field("path", path),
                        log::field("reason", why));
        continue;
      }
      error = who + " cannot be read as glTF: " + why;
      return false;
    }
    if (mesh.skins.empty()) {
      if (!named) {
        ENGINE_LOG_INFO(log_view_anim, "rigid mesh, no clips", log::field("path", path));
        continue;
      }
      error = who +
              " has no skin to play a clip on; an animation block belongs on an instance "
              "of a skinned glTF";
      return false;
    }
    const u32 before = scene.library.skeleton_count();
    animation::LoadStats stats;
    if (!scene.library.add(mesh, path_stem(path), &stats, &error)) {
      error = path + ": " + error;
      return false;
    }
    if (scene.library.skeleton_count() > before) scene.mesh_skeleton[m] = before;
    ENGINE_LOG_INFO(log_view_anim, "clips loaded", log::field("path", path),
                    log::field("skeletons", stats.skeletons), log::field("clips", stats.clips),
                    log::field("skipped_channels", stats.skipped_channels));
  }
  return true;
}

u64 hash_f32(u64 seed, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return hash_combine(seed, bits);
}

}  // namespace

Id128 instance_id(u32 instance) { return Id128::from_seed(0x656e67'76696577ull, instance + 1); }

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

// `--animate` refuses a mesh with no skin — it has no skeleton to tick — so before this a
// morph-only file (every Khronos morph sample is one) played its default weights however
// `--morph-animate` was spelled. A file with neither a skin nor a weight curve has nothing to add
// and is passed over rather than refused: it is still drawn, it just animates nothing.
void load_morph_clips(animation::Library& library, const renderer::SceneDesc& desc) {
  for (u32 m = 0; m < desc.meshes.size(); ++m) {
    const std::string& path = desc.meshes[m];
    if (path.empty() || io::extension(path) == ".clusters" || generated_by(desc, m) != nullptr)
      continue;
    animation::LoadStats stats;
    std::string reason;
    if (!library.load_gltf(path, {}, &stats, &reason)) {
      ENGINE_LOG_INFO(log_view_anim, "no clips in this mesh", log::field("path", path),
                      log::field("reason", reason));
      continue;
    }
    ENGINE_LOG_INFO(log_view_anim, "morph clips loaded", log::field("path", path),
                    log::field("clips", stats.clips));
  }
}

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

// It is a function rather than a block in a frame loop's setup because the windowed path, the
// offscreen path and the reference path need exactly the same steps in exactly the same order, and
// two copies of "which instances are skinned" would be two answers the day one of them changed.
//
// **Every instance is read**, not the first: a scene whose first instance is a rigid palm and whose
// third is a character animates the character. Which meshes are opened as clip sources is
// `load_clips`' rule above; which instances play is this:
//   - an instance with its own `animation` block plays, and its mesh must be able to (refused with
//     a sentence naming the instance otherwise);
//   - `--animate` makes every instance whose mesh carries a skin play, the rest stay rigid;
//   - a `--mesh` run with no scene file has one instance, made here, or the grid's copies.
bool prepare_animation(std::unique_ptr<AnimatedScene>& out, renderer::SceneDesc& desc,
                       const AnimationOptions& options, std::string& error) {
  bool any_block = false;
  for (const renderer::SceneInstance& instance : desc.instances)
    any_block = any_block || instance.animation.play;
  if (!options.animate && !any_block) return true;

  out = std::make_unique<AnimatedScene>();
  Vector<ClipSource> role(desc.meshes.size(), ClipSource::none);
  Vector<u32> named_by(desc.meshes.size(), 0u);
  // `--animate` (and a `--mesh` run, which has only its one mesh) makes every mesh a candidate;
  // a block makes its own mesh a requirement, which outranks that.
  if (options.animate || desc.instances.empty()) {
    for (ClipSource& r : role)
      r = ClipSource::candidate;
  }
  for (u32 i = 0; i < desc.instances.size(); ++i) {
    const renderer::SceneInstance& instance = desc.instances[i];
    if (!instance.animation.play || instance.mesh >= role.size()) continue;
    if (role[instance.mesh] != ClipSource::named) named_by[instance.mesh] = i;
    role[instance.mesh] = ClipSource::named;
  }
  if (!load_clips(*out, desc, std::span<const ClipSource>(role.data(), role.size()),
                  std::span<const u32>(named_by.data(), named_by.size()), error)) {
    return false;
  }
  auto joints_of_mesh = [&](u32 mesh) {
    const u32 skeleton = mesh < out->mesh_skeleton.size() ? out->mesh_skeleton[mesh]
                                                          : animation::Library::k_not_found;
    return skeleton == animation::Library::k_not_found
               ? 0u
               : out->library.skeleton_data(skeleton).joint_count();
  };
  u32 skinned = 0;
  if (desc.instances.empty()) {
    // `--mesh --animate`: the grid is expanded by the loader and a single instance is not, so
    // the one case that has no `SceneInstance` yet gets one here.
    if (desc.grid_instances > 1) {
      desc.grid_joints = joints_of_mesh(0);
      skinned = desc.grid_joints > 0 ? desc.grid_instances * desc.grid_instances : 0u;
    } else {
      renderer::SceneInstance instance;
      instance.mesh = 0;
      instance.joints = joints_of_mesh(0);
      instance.animation.play = true;
      skinned = instance.joints > 0 ? 1u : 0u;
      desc.instances.push_back(instance);
    }
  } else {
    for (renderer::SceneInstance& instance : desc.instances) {
      const u32 joints = joints_of_mesh(instance.mesh);
      if (!instance.animation.play && options.animate && joints > 0) instance.animation.play = true;
      if (instance.animation.play) instance.joints = joints;
      skinned += instance.joints > 0 ? 1u : 0u;
    }
  }
  if (skinned == 0) {
    error = "no mesh of this scene has a skin; --animate needs a skinned glTF";
    return false;
  }
  return true;
}

// **What the camera says each character is worth, before the world is ticked.**
//
// Four calls: build the observer set from the views, ask the views which instances are on screen
// and how much that view is worth, run `sim::TierAssignment` over the rows, and hand the accepted
// changes to the capability. The tier code never sees an entity id and the capability never sees
// a camera; `row_instance` is where the two meet, exactly as `sim::apply_tier_changes`'
// `entities[change.index]` is.
//
// The changes go through `AnimationSystem::set_tier(Id128, u8)` rather than through the hooks
// table, and that is not a shortcut: `sim::MaterializationHooks` speaks `sim::EntityHandle`, only
// `domain/ecs` may make one, and `<flecs.h>` does not belong in `apps/` (AGENTS.md, ADR-0028 seam
// 5). Both paths go to one function — `apply_tier`, which acquires or releases the pose slot and
// advances the playhead across the frozen interval — so the demotion still releases the slot and
// the promotion still lands where ticking through the gap would have.
//
// It runs **before** each step, so the step that follows already has the new divisors, and on the
// frusta of the camera the clock stands at (`SceneRenderer::update_views`): a frame's on the frame
// clock, a tick's in a session, so the decision is a function of that clock and nothing else.
void update_animation_lod(AnimatedScene& scene, const renderer::ViewSet& views,
                          const renderer::Camera& camera) {
  if (!scene.lod || scene.row_instance.empty()) return;
  build_observers(views, camera, scene.observers);
  view_importance(views, camera.position,
                  std::span<const WorldPos>(scene.positions.data(), scene.positions.size()),
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
// reads no clock, so N steps are N steps whatever the machine was doing.
//
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
  ++scene.steps;
  for (u32 i = 0; i < scene.entities.size(); ++i) {
    renderer::InstanceJoints& run = scene.runs[i];
    run = renderer::InstanceJoints{};
    if (scene.entities[i].is_null()) continue;
    const u8 tier = i < scene.instance_tier.size() ? scene.instance_tier[i] : u8{0};
    if (!animation::lod_plan(tier).skin) continue;  // the rest pose, deliberately
    scene.system.joint_run(scene.entities[i], run.first, run.count);
  }
}

u32 advance_animation(AnimatedScene& scene, renderer::SceneRenderer& renderer,
                      const renderer::Camera& camera, const char* clock, u32 clock_hz,
                      u64 clock_ticks) {
  scene.clock = clock;
  scene.clock_hz = clock_hz;
  if (clock_ticks > scene.clock_ticks) scene.clock_ticks = clock_ticks;
  const u32 hz = clock_hz > 0 ? clock_hz : k_animation_hz;
  // Step k is due once the clock has reached `k * hz / 60`: the largest whole number of steps
  // the clock's time covers. Integer arithmetic, so the same tick count gives the same steps on
  // every machine.
  const u64 due = scene.clock_ticks * k_animation_hz / hz;
  u32 ran = 0;
  while (scene.steps < due) {
    const i64 lod_started = time::monotonic_ns();
    update_animation_lod(scene, renderer.update_views(camera), camera);
    const i64 ticked = time::monotonic_ns();
    step_animation(scene);
    scene.lod_ns += static_cast<f64>(ticked - lod_started);
    scene.tick_ns += static_cast<f64>(time::monotonic_ns() - ticked);
    ++ran;
  }
  return ran;
}

void AnimationTicks::attach(FlySession& session, AnimatedScene& animated,
                            renderer::SceneRenderer& view_renderer, u32 session_tick_hz) noexcept {
  scene = &animated;
  views_of = &view_renderer;
  tick_hz = session_tick_hz > 0 ? session_tick_hz : 1u;
  next = session.tick_observer();
  next_user = session.tick_observer_user();
  animated.clock = "ticks";
  animated.clock_hz = tick_hz;
  session.set_tick_observer(&AnimationTicks::observe, this);
}

// After the camera's integration and after the game's tick: `session.camera()` is this tick's
// camera, which is what the tiers are decided from — never a frame's interpolated one, which a
// window and an offscreen replay of the same ticks would not share.
void AnimationTicks::observe(void* user, const FlySession& session,
                             const input::InputState& input) {
  auto* self = static_cast<AnimationTicks*>(user);
  if (self->next != nullptr) self->next(self->next_user, session, input);
  const u64 tick = session.tick().value;
  if (self->before != nullptr) self->before(self->hooks_user, tick);
  (void)advance_animation(*self->scene, *self->views_of, session.camera(), "ticks", self->tick_hz,
                          tick);
  if (self->after != nullptr) self->after(self->hooks_user, tick);
}

void set_frame_joints(const AnimatedScene& scene, renderer::FrameDesc& frame) noexcept {
  frame.joints = scene.system.joint_matrices();
  frame.instance_joints = {scene.runs.data(), scene.runs.size()};
}

// Part two of the glue. It runs *after* the scene has loaded because the renderer's joint buffer
// is sized by the pose pool, and the pool only knows its size once the population is attached —
// which is also the honest place for `SceneData::max_joints` to be filled in.
bool attach_instances(AnimatedScene& scene, const renderer::SceneDesc& desc,
                      renderer::SceneData& data, const AnimationOptions& options,
                      std::string& error) {
  scene.lod = options.lod;
  scene.lod_scale = options.lod_scale;
  scene.system.install(scene.sim);
  const u32 count = data.instances.size();
  scene.entities.resize(count, Id128{});
  scene.runs.resize(count, renderer::InstanceJoints{});
  scene.instance_clip.resize(count, animation::Library::k_not_found);
  // The grid is expanded inside `load_scene`, so its instances have no `SceneInstance` of their
  // own to carry a block; they take the command line's clip and speed and a phase each. Every
  // other instance the scene has is `desc.instances[i]` at the same index; what follows them —
  // a game's props, appended at the load — is rigid.
  const bool from_grid = desc.instances.empty();

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
    const u32 mesh = data.instances[i].mesh;
    const u32 skeleton_index = mesh < scene.mesh_skeleton.size() ? scene.mesh_skeleton[mesh]
                                                                 : animation::Library::k_not_found;
    if (skeleton_index == animation::Library::k_not_found) continue;
    renderer::SceneAnimation request;
    if (from_grid || i >= desc.instances.size()) {
      request.clip = options.clip;
      request.speed = options.speed;
    } else {
      request = desc.instances[i].animation;
      if (request.clip.empty()) request.clip = options.clip;
      request.speed *= options.speed;
    }
    const u32 clip_index = find_clip(scene.library, skeleton_index, request.clip);
    if (clip_index == animation::Library::k_not_found) {
      error = request.clip.empty()
                  ? "instance " + std::to_string(i) + ": the skin has no animation to play"
                  : "instance " + std::to_string(i) + ": no clip named '" + request.clip + "'";
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
    scene.instance_clip[i] = clip_index;
    ++scene.instances;
    scene.joints = scene.joints > data.instance_joints[i] ? scene.joints : data.instance_joints[i];
    if (scene.clip_name.empty()) scene.clip_name = scene.library.clip(clip_index).name;

    // The cull pass tests this instance by its **rest-pose** spheres, and a skinned vertex is not
    // in them, so it carries a bound on how far the clip it plays can move one.
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
      ENGINE_LOG_INFO(log_view_anim, "skinned bounds padding",
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
    // The centre in the world, f64 (ADR-0053): the mesh's centre through the instance's 3x4 in the
    // instance's own frame — the eye at its cell and local, so the translation is zero and the
    // product is a float the size of the mesh — added to where the cell and local put the instance.
    const WorldCell placed = gfx::instance_cell(instance);
    const Mat4 own_frame =
        gfx::instance_matrix(instance, WorldEye{placed.cell, placed.local, Vec3{}});
    const Vec4 offset = own_frame * Vec4{mesh_center[instance.mesh], 1.0f};
    scene.row_instance.push_back(i);
    scene.positions.push_back(to_world(placed) + DVec3{offset.xyz()});
    scene.radii.push_back((mesh_radius[instance.mesh] + instance.bounds_padding) *
                          instance.scale_max);
  }
  scene.importance.resize(scene.row_instance.size(), 1.0f);
  // Every instance starts at tier 0, which is where `AnimationSystem::attach` put it, so the first
  // step's assignment is the only one that has a whole population to move and the rate limits in
  // `sim::TierParams` spread it over the steps after it.
  scene.tier_state.resize(scene.row_instance.size(), u8{0});
  scene.instance_tier.resize(count, u8{0});
  scene.histogram[0] = scene.row_instance.size();
  scene.tier_params = scaled_tier_params(animation::tier_params(), scene.lod_scale);
  // The pool hands out one slot per instance and never splits a run, so its capacity now is the
  // longest span a frame can hand over. The renderer clamps anything longer and says so once.
  data.max_joints = scene.system.poses().joint_capacity();
  // The instances now carry their padding, so the scene's bounding sphere — what the camera
  // frames, and what the light reach and the shadow bias scale with — has to be taken again.
  renderer::update_scene_bounds(data);
  return true;
}

// The histogram is the *population by tier* at the end, which is the number the LOD policy is
// judged by — `animation`'s bench says a 5/15/30/50 mix is 5.6× cheaper than everything at LOD0,
// so a run whose histogram is all in bucket 0 has a policy that is not working. `ms.tick` and
// `ms.lod` are CPU milliseconds a frame: the world's steps (the sampler, the skinning matrices,
// the per-instance `joint_run`) and the tier assignments that preceded them.
//
// **The hashes** are what a game's test pins: per animated instance, the clip its player plays
// (the library id), the playhead (the player's time, rate and fade, as bits), and the pose it
// draws (the bytes of the run of matrices the frame hands the renderer; an empty run — LOD2, LOD3
// — hashes as nothing, since the rest pose is what it draws). They are functions of the clock's
// ticks alone, so a window and an offscreen replay of one recording write the same ones.
scene::FlythroughAnimation animation_summary(const AnimatedScene& scene, u64 frames) {
  scene::FlythroughAnimation out;
  out.on = scene.lod;
  out.scale = static_cast<f64>(scene.lod_scale);
  out.instances = scene.instances;
  for (const u32 bucket : scene.histogram)
    out.tiers.push_back(bucket);
  out.promotions = scene.promotions;
  out.demotions = scene.demotions;
  out.deferred = scene.deferred;
  const f64 divisor = frames > 0 ? static_cast<f64>(frames) : 1.0;
  JsonValue ms = JsonValue::object();
  ms.set("tick", scene.tick_ns / 1.0e6 / divisor);
  ms.set("lod", scene.lod_ns / 1.0e6 / divisor);
  out.ms = std::move(ms);
  out.clock = scene.clock;
  out.clock_hz = scene.clock_hz;
  out.clock_ticks = scene.clock_ticks;
  out.steps = scene.steps;
  u64 all = k_hash_seed;
  const std::span<const anim::JointMatrix> matrices = scene.system.joint_matrices();
  for (u32 i = 0; i < scene.entities.size(); ++i) {
    if (scene.entities[i].is_null()) continue;
    scene::AnimatedInstance entry;
    entry.instance = i;
    entry.tier = i < scene.instance_tier.size() ? scene.instance_tier[i] : 0u;
    const u32 clip_index = scene.instance_clip[i];
    if (clip_index != animation::Library::k_not_found)
      entry.clip = scene.library.clip(clip_index).name;
    animation::AnimationPlayer player;
    u64 clip_hash = k_hash_seed;
    u64 playhead_hash = k_hash_seed;
    if (scene.system.player(scene.entities[i], player)) {
      clip_hash = hash_combine(hash_combine(clip_hash, player.clip.hi), player.clip.lo);
      clip_hash = hash_combine(hash_combine(clip_hash, player.fade_clip.hi), player.fade_clip.lo);
      playhead_hash = hash_f32(playhead_hash, player.time);
      playhead_hash = hash_f32(playhead_hash, player.speed);
      playhead_hash = hash_f32(playhead_hash, player.fade_time);
      playhead_hash = hash_f32(playhead_hash, player.fade_elapsed);
    }
    const renderer::InstanceJoints& run = scene.runs[i];
    const u64 pose_hash =
        run.count > 0 && run.first + run.count <= matrices.size()
            ? hash_bytes(matrices.data() + run.first, usize{run.count} * sizeof(anim::JointMatrix))
            : k_hash_seed;
    entry.clip_hash = renderer::hash_hex(clip_hash);
    entry.playhead_hash = renderer::hash_hex(playhead_hash);
    entry.pose_hash = renderer::hash_hex(pose_hash);
    all = hash_combine(hash_combine(hash_combine(hash_combine(all, i), clip_hash), playhead_hash),
                       pose_hash);
    out.animated.push_back(std::move(entry));
  }
  out.hash = renderer::hash_hex(all);
  return out;
}

JsonValue anim_summary(const AnimatedScene& scene, u64 frames) {
  return schema::to_json(animation_summary(scene, frames));
}

#if ENGINE_VIEW_GAME
// ---- the actors a game moves (animation.h `GameActors`; apps.md "Actors")
// ------------------------

namespace {

constexpr const char* k_player_type = "engine.animation.AnimationPlayer";
// A quarter-second a sample, 256 of them: a minute of path per actor, sized once at the bind so a
// tick allocates nothing; past it the samples stop and `path_dropped` counts what was not kept.
constexpr u32 k_path_samples = 256;

// The yaw a rotation turns +x to, about +y, as `quat_from_axis_angle(+y, yaw)` does.
f32 yaw_of(Quat q) {
  const Vec3 x = rotate(normalize(q), Vec3{1.0f, 0.0f, 0.0f});
  return std::atan2(-x.z, x.x);
}

JsonValue position_json(const WorldPos& p) {
  JsonValue out = JsonValue::array();
  for (const f64 v : {p.x, p.y, p.z})
    out.push_back(JsonValue(v));
  return out;
}

}  // namespace

struct GameActors::Impl {
  struct Sample {
    u64 tick = 0;
    WorldPos position{};
    f32 yaw = 0.0f;
    u32 clip = animation::Library::k_not_found;
    f32 playhead = 0.0f;
    u64 pose_hash = 0;
  };
  struct Actor {
    std::string name;
    u32 instance = 0;
    Id128 entity;
    u32 skeleton = animation::Library::k_not_found;
    u32 row = ~0u;  // the tier row, `AnimatedScene::row_instance`
    renderer::SceneInstance placement;
    WorldPos position{};
    f32 yaw = 0.0f;
    Vec3 centre{};  // the tier sphere's centre from the origin, before the rotation
    bool moved = false;
    // The weight's fade (a stop, or a play from the rest pose), a tick at a time.
    bool fading = false;
    bool stop_at_end = false;
    f32 fade_from = 1.0f;
    f32 fade_to = 1.0f;
    // In whole ticks, so where a fade ends is a count and not a sum of 1/240s.
    u64 fade_ticks = 1;
    u64 fade_elapsed = 0;
    Vector<Sample> path;
    u64 path_dropped = 0;
  };

  AnimatedScene* scene = nullptr;
  Vector<Actor> actors;
  u64 sample_every = 60;
  f64 dt = 1.0 / 240.0;
  Vector<u32> move_indices;
  Vector<renderer::SceneInstance> move_placements;
  u64 places = 0, plays = 0, stops = 0, speeds = 0, refused = 0, flushes = 0, moves = 0;

  Actor* find(u32 instance) noexcept {
    for (Actor& a : actors) {
      if (a.instance == instance) return &a;
    }
    return nullptr;
  }
  bool read(const Actor& a, animation::AnimationPlayer& out) const {
    return scene->system.player(a.entity, out);
  }
  // The player's own bytes back into the world, outside a tick (the module and the hooks run
  // between steps), applied at once so the next read sees it.
  void write(const Actor& a, const animation::AnimationPlayer& player) {
    ecs::WorldCommands commands(scene->sim.world());
    (void)commands.set_bytes(
        a.entity, k_player_type,
        std::span<const u8>(reinterpret_cast<const u8*>(&player), sizeof(player)));
    (void)commands.apply();
  }
  static void clear_clip(animation::AnimationPlayer& p) noexcept {
    p.clip = Id128{};
    p.fade_clip = Id128{};
    p.time = 0.0f;
    p.fade_time = 0.0f;
    p.fade_elapsed = 0.0f;
    p.fade_seconds = 0.0f;
    p.weight = 1.0f;
  }
  // The pose the last step left, as the summary hashes it (`animation_summary`).
  u64 pose_hash(const Actor& a) const {
    const std::span<const anim::JointMatrix> matrices = scene->system.joint_matrices();
    const renderer::InstanceJoints& run = scene->runs[a.instance];
    return run.count > 0 && run.first + run.count <= matrices.size()
               ? hash_bytes(matrices.data() + run.first,
                            usize{run.count} * sizeof(anim::JointMatrix))
               : k_hash_seed;
  }
  // The clip a player is heading for — the one fading in, else the one playing — and its time.
  u32 target_clip(const animation::AnimationPlayer& p, f32& time, bool& looping) const {
    const bool fading = !p.fade_clip.is_null();
    time = fading ? p.fade_time : p.time;
    looping = fading ? p.fade_looping : p.looping;
    const Id128& id = fading ? p.fade_clip : p.clip;
    return id.is_null() ? animation::Library::k_not_found : scene->library.clip_index(id);
  }
  void fill(const Actor& a, hotload::ActorState& out) const;
  void start_fade(Actor& a, f32 from, f32 to, f32 seconds, bool stop) noexcept {
    a.fading = true;
    a.stop_at_end = stop;
    a.fade_from = from;
    a.fade_to = to;
    const f64 ticks = std::round(static_cast<f64>(seconds) / dt);
    a.fade_ticks = ticks >= 1.0 ? static_cast<u64>(ticks) : 1u;
    a.fade_elapsed = 0;
  }

  // After the game's commands, before the step: every fade of the weight a tick on.
  void before(u64) {
    for (Actor& a : actors) {
      if (!a.fading) continue;
      animation::AnimationPlayer p;
      if (!read(a, p)) continue;
      ++a.fade_elapsed;
      const bool done = a.fade_elapsed >= a.fade_ticks;
      const f32 t =
          done
              ? 1.0f
              : static_cast<f32>(static_cast<f64>(a.fade_elapsed) / static_cast<f64>(a.fade_ticks));
      p.weight = a.fade_from + (a.fade_to - a.fade_from) * t;
      if (done) {
        a.fading = false;
        if (a.stop_at_end) clear_clip(p);
        a.stop_at_end = false;
      }
      write(a, p);
    }
  }
  // After the step: the path, a quarter-second at a time.
  void after(u64 tick) {
    if (sample_every == 0 || tick % sample_every != 0) return;
    for (Actor& a : actors) {
      if (a.path.size() >= k_path_samples) {
        ++a.path_dropped;
        continue;
      }
      Sample s;
      s.tick = tick;
      s.position = a.position;
      s.yaw = a.yaw;
      animation::AnimationPlayer p;
      bool looping = false;
      if (read(a, p)) s.clip = target_clip(p, s.playhead, looping);
      s.pose_hash = pose_hash(a);
      a.path.push_back(s);
    }
  }
};

GameActors::GameActors() : impl_(std::make_unique<Impl>()) {}
GameActors::~GameActors() = default;

u32 GameActors::count() const noexcept { return impl_->actors.size(); }

bool GameActors::bind(AnimatedScene& scene, const renderer::SceneDesc& desc,
                      const Vector<DeclaredActor>& declared, u32 tick_hz, std::string& error) {
  Impl& g = *impl_;
  g.scene = &scene;
  const u32 hz = tick_hz > 0 ? tick_hz : 240u;
  g.dt = 1.0 / static_cast<f64>(hz);
  g.sample_every = hz / 4 > 0 ? hz / 4 : 1u;
  g.actors.clear();
  for (const DeclaredActor& d : declared) {
    if (d.instance >= scene.entities.size() || d.instance >= desc.instances.size() ||
        scene.entities[d.instance].is_null()) {
      std::fprintf(stderr,
                   "engine-view: game: the scene names instance %u \"%s\", which does not animate "
                   "(its mesh has no skin); it is not an actor\n",
                   d.instance, d.name.c_str());
      continue;
    }
    Impl::Actor a;
    a.name = d.name;
    a.instance = d.instance;
    a.entity = scene.entities[d.instance];
    const renderer::SceneInstance& source = desc.instances[d.instance];
    a.skeleton = source.mesh < scene.mesh_skeleton.size() ? scene.mesh_skeleton[source.mesh]
                                                          : animation::Library::k_not_found;
    a.placement = source;
    a.position = source.origin + DVec3{source.transform.position};
    a.yaw = yaw_of(source.transform.rotation);
    for (u32 r = 0; r < scene.row_instance.size(); ++r) {
      if (scene.row_instance[r] == d.instance) a.row = r;
    }
    if (a.row != ~0u) {
      const DVec3 offset = scene.positions[a.row] - a.position;
      a.centre = rotate(
          inverse(normalize(source.transform.rotation)),
          Vec3{static_cast<f32>(offset.x), static_cast<f32>(offset.y), static_cast<f32>(offset.z)});
    }
    a.path.reserve(k_path_samples);
    g.actors.push_back(std::move(a));
  }
  g.move_indices.reserve(g.actors.size());
  g.move_placements.reserve(g.actors.size());
  (void)error;
  return true;
}

void GameActors::hook(AnimationTicks& ticks) noexcept {
  ticks.before = [](void* user, u64 tick) { static_cast<GameActors*>(user)->impl_->before(tick); };
  ticks.after = [](void* user, u64 tick) { static_cast<GameActors*>(user)->impl_->after(tick); };
  ticks.hooks_user = this;
}

hotload::Result GameActors::place(u32 instance, const f64* position, f32 yaw) {
  Impl& g = *impl_;
  Impl::Actor* a = g.find(instance);
  if (a == nullptr) return hotload::Result::NotFound;
  const WorldPos at{position[0], position[1], position[2]};
  if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || !std::isfinite(yaw) ||
      !world_cell_valid(at)) {
    ++g.refused;
    return hotload::Result::InvalidArgument;
  }
  // Every scene the renderer draws moves a loaded instance: a scene read whole at once, a dynamic
  // one (props, a world) through the next frame's table flip (renderer.md, "Instances that come
  // and go"; until 2026-10-19 a dynamic scene refused the move and this answered `Unsupported`).
  // Level: the scene's pitch and roll are dropped, its scale kept, so a character stands up.
  const Quat turn = quat_from_axis_angle(Vec3::unit_y(), yaw);
  a->position = at;
  a->yaw = yaw;
  a->placement.origin = at;
  a->placement.transform.position = Vec3{};
  a->placement.transform.rotation = turn;
  a->moved = true;
  if (a->row != ~0u) g.scene->positions[a->row] = at + DVec3{rotate(turn, a->centre)};
  ++g.places;
  return hotload::Result::Ok;
}

hotload::Result GameActors::play(u32 instance, std::string_view clip, f32 fade_s, bool looping) {
  Impl& g = *impl_;
  Impl::Actor* a = g.find(instance);
  if (a == nullptr) return hotload::Result::NotFound;
  if (!std::isfinite(fade_s) || fade_s < 0.0f) {
    ++g.refused;
    return hotload::Result::InvalidArgument;
  }
  const u32 index = find_clip(g.scene->library, a->skeleton, clip);
  if (clip.empty() || index == animation::Library::k_not_found) {
    ++g.refused;
    return hotload::Result::NotFound;
  }
  animation::AnimationPlayer before;
  if (!g.read(*a, before)) return hotload::Result::NotFound;
  const bool from_rest = before.clip.is_null() && before.fade_clip.is_null();
  if (!g.scene->system.play(a->entity, g.scene->library.clip(index).id, fade_s, looping)) {
    ++g.refused;
    return hotload::Result::NotFound;
  }
  animation::AnimationPlayer after;
  (void)g.read(*a, after);
  if (from_rest && fade_s > 0.0f) {
    // From the rest pose the capability cuts; the fade in is the weight's, from 0.
    after.weight = 0.0f;
    g.start_fade(*a, 0.0f, 1.0f, fade_s, false);
    g.write(*a, after);
  } else if (a->fading && a->stop_at_end) {
    // A stop in flight turns round: back up to the clip from where the weight stands.
    if (fade_s > 0.0f) {
      g.start_fade(*a, after.weight, 1.0f, fade_s, false);
    } else {
      a->fading = false;
      a->stop_at_end = false;
      after.weight = 1.0f;
      g.write(*a, after);
    }
  }
  ++g.plays;
  return hotload::Result::Ok;
}

hotload::Result GameActors::stop(u32 instance, f32 fade_s) {
  Impl& g = *impl_;
  Impl::Actor* a = g.find(instance);
  if (a == nullptr) return hotload::Result::NotFound;
  if (!std::isfinite(fade_s) || fade_s < 0.0f) {
    ++g.refused;
    return hotload::Result::InvalidArgument;
  }
  animation::AnimationPlayer p;
  if (!g.read(*a, p)) return hotload::Result::NotFound;
  ++g.stops;
  if (p.clip.is_null() && p.fade_clip.is_null()) {
    a->fading = false;  // already at rest
    a->stop_at_end = false;
    return hotload::Result::Ok;
  }
  if (fade_s <= 0.0f) {
    Impl::clear_clip(p);
    a->fading = false;
    a->stop_at_end = false;
    g.write(*a, p);
  } else {
    g.start_fade(*a, p.weight, 0.0f, fade_s, true);
  }
  return hotload::Result::Ok;
}

hotload::Result GameActors::speed(u32 instance, f32 rate) {
  Impl& g = *impl_;
  Impl::Actor* a = g.find(instance);
  if (a == nullptr) return hotload::Result::NotFound;
  if (!std::isfinite(rate)) {
    ++g.refused;
    return hotload::Result::InvalidArgument;
  }
  animation::AnimationPlayer p;
  if (!g.read(*a, p) || !g.scene->system.set_playhead(a->entity, p.time, rate)) {
    return hotload::Result::NotFound;
  }
  ++g.speeds;
  return hotload::Result::Ok;
}

void GameActors::Impl::fill(const Actor& a, hotload::ActorState& out) const {
  out.instance = a.instance;
  out.position[0] = a.position.x;
  out.position[1] = a.position.y;
  out.position[2] = a.position.z;
  out.yaw = a.yaw;
  animation::AnimationPlayer p;
  if (read(a, p)) {
    bool looping = false;
    const u32 clip = target_clip(p, out.playhead_s, looping);
    out.looping = looping ? 1u : 0u;
    out.speed = p.speed;
    out.weight = p.weight;
    out.playing = clip != animation::Library::k_not_found ? 1u : 0u;
    out.fading = !p.fade_clip.is_null() || a.fading ? 1u : 0u;
    out.clip[0] = '\0';
    if (clip != animation::Library::k_not_found) {
      const std::string& name = scene->library.clip(clip).name;
      const usize n = name.size() < sizeof(out.clip) - 1 ? name.size() : sizeof(out.clip) - 1;
      std::memcpy(out.clip, name.data(), n);
      out.clip[n] = '\0';
    }
  }
  out.tier = a.instance < scene->instance_tier.size() ? scene->instance_tier[a.instance] : 0u;
  out.pose_hash = pose_hash(a);
}

hotload::Result GameActors::state(u32 instance, hotload::ActorState& out) {
  Impl& g = *impl_;
  const Impl::Actor* a = g.find(instance);
  if (a == nullptr) return hotload::Result::NotFound;
  g.fill(*a, out);
  return hotload::Result::Ok;
}

// The state file's `actors`: `[{"name","instance","position":[x,y,z],"yaw","player":{…},
// "fade":{"from","to","ticks","elapsed_ticks","stop"}}]`, the player as its schema writes it, so a
// field the capability adds to it later reads back at its default.
JsonValue GameActors::to_json() const {
  const Impl& g = *impl_;
  JsonValue out = JsonValue::array();
  for (const Impl::Actor& a : g.actors) {
    JsonValue actor = JsonValue::object();
    actor.set("name", a.name);
    actor.set("instance", a.instance);
    actor.set("position", position_json(a.position));
    actor.set("yaw", static_cast<f64>(a.yaw));
    animation::AnimationPlayer p;
    if (g.read(a, p)) actor.set("player", schema::to_json(p));
    if (a.fading) {
      JsonValue fade = JsonValue::object();
      fade.set("from", static_cast<f64>(a.fade_from));
      fade.set("to", static_cast<f64>(a.fade_to));
      fade.set("ticks", a.fade_ticks);
      fade.set("elapsed_ticks", a.fade_elapsed);
      fade.set("stop", a.stop_at_end);
      actor.set("fade", std::move(fade));
    }
    out.push_back(std::move(actor));
  }
  return out;
}

bool GameActors::restore(const JsonValue& actors, std::string& error) {
  Impl& g = *impl_;
  if (!actors.is_array()) {
    error = "actors: not an array";
    return false;
  }
  for (u32 i = 0; i < actors.size(); ++i) {
    const JsonValue& entry = actors[i];
    const std::string where = "actors[" + std::to_string(i) + "]";
    const JsonValue* name = entry.find("name");
    if (name == nullptr || !name->is_string()) {
      error = where + " has no name";
      return false;
    }
    Impl::Actor* a = nullptr;
    for (Impl::Actor& candidate : g.actors) {
      if (candidate.name == name->as_string()) a = &candidate;
    }
    if (a == nullptr) {
      std::fprintf(stderr,
                   "engine-view: game: %s, \"%s\", is not an actor of this scene; skipped\n",
                   where.c_str(), std::string(name->as_string()).c_str());
      continue;
    }
    const JsonValue* position = entry.find("position");
    const JsonValue* yaw = entry.find("yaw");
    f64 p[3] = {0.0, 0.0, 0.0};
    f64 angle = 0.0;
    if (position == nullptr || !position->is_array() || position->size() != 3 || yaw == nullptr ||
        !yaw->get_f64(angle)) {
      error = where + ": no position and yaw";
      return false;
    }
    for (u32 k = 0; k < 3; ++k) {
      if (!(*position)[k].get_f64(p[k])) {
        error = where + ": a position that is not three numbers";
        return false;
      }
    }
    // Where it stood: through `place`, the same validation and the same tier row, but not counted
    // as a module's call.
    const u64 places = g.places;
    const hotload::Result placed = place(a->instance, p, static_cast<f32>(angle));
    g.places = places;
    if (placed == hotload::Result::InvalidArgument) {
      error = where + ": a position outside the world";
      return false;
    }
    if (const JsonValue* player = entry.find("player"); player != nullptr) {
      animation::AnimationPlayer restored;
      schema::ReadContext context;
      context.options.ignore_unknown_fields = true;
      if (!schema::from_json(restored, *player, context) || !context.ok()) {
        error = where + ": player: " +
                (context.diagnostics.empty() ? std::string("does not read")
                                             : context.diagnostics.front().path + ": " +
                                                   context.diagnostics.front().message);
        return false;
      }
      g.write(*a, restored);
    }
    a->fading = false;
    a->stop_at_end = false;
    if (const JsonValue* fade = entry.find("fade"); fade != nullptr && fade->is_object()) {
      f64 from = 1.0, to = 1.0;
      u64 ticks = 0, elapsed = 0;
      const JsonValue* stop = fade->find("stop");
      const JsonValue* ticks_json = fade->find("ticks");
      const JsonValue* elapsed_json = fade->find("elapsed_ticks");
      const auto number = [&](const char* key, f64& v) {
        const JsonValue* f = fade->find(key);
        return f != nullptr && f->get_f64(v) && std::isfinite(v);
      };
      if (!number("from", from) || !number("to", to) || ticks_json == nullptr ||
          !ticks_json->get_u64(ticks) || elapsed_json == nullptr ||
          !elapsed_json->get_u64(elapsed) || stop == nullptr || !stop->is_bool()) {
        error = where + ": fade: from, to, ticks, elapsed_ticks or stop is missing";
        return false;
      }
      g.start_fade(*a, static_cast<f32>(from), static_cast<f32>(to), 0.0f, stop->as_bool());
      a->fade_ticks = ticks > 0 ? ticks : 1u;
      a->fade_elapsed = elapsed;
    }
  }
  return true;
}

JsonValue GameActors::summary_json() const {
  const Impl& g = *impl_;
  const auto clip_name = [&](u32 clip) {
    return clip != animation::Library::k_not_found ? g.scene->library.clip(clip).name
                                                   : std::string();
  };
  JsonValue out = JsonValue::array();
  for (const Impl::Actor& a : g.actors) {
    hotload::ActorState s;
    g.fill(a, s);
    JsonValue actor = JsonValue::object();
    actor.set("name", a.name);
    actor.set("instance", a.instance);
    actor.set("position", position_json(a.position));
    actor.set("yaw", static_cast<f64>(a.yaw));
    actor.set("clip", std::string_view(s.clip));
    actor.set("playhead", static_cast<f64>(s.playhead_s));
    actor.set("speed", static_cast<f64>(s.speed));
    actor.set("weight", static_cast<f64>(s.weight));
    actor.set("looping", s.looping != 0);
    actor.set("playing", s.playing != 0);
    actor.set("fading", s.fading != 0);
    actor.set("tier", s.tier);
    actor.set("pose_hash", renderer::hash_hex(s.pose_hash));
    JsonValue path = JsonValue::array();
    for (const Impl::Sample& sample : a.path) {
      JsonValue p = JsonValue::object();
      p.set("tick", sample.tick);
      p.set("position", position_json(sample.position));
      p.set("yaw", static_cast<f64>(sample.yaw));
      p.set("clip", clip_name(sample.clip));
      p.set("playhead", static_cast<f64>(sample.playhead));
      p.set("pose_hash", renderer::hash_hex(sample.pose_hash));
      path.push_back(std::move(p));
    }
    actor.set("path", std::move(path));
    actor.set("path_dropped", a.path_dropped);
    out.push_back(std::move(actor));
  }
  return out;
}

JsonValue GameActors::calls_json() const {
  const Impl& g = *impl_;
  JsonValue out = JsonValue::object();
  out.set("bound", true);
  out.set("actors", g.actors.size());
  out.set("can_move", true);  // in every scene since 2026-10-19, dynamic ones included
  out.set("places", g.places);
  out.set("plays", g.plays);
  out.set("stops", g.stops);
  out.set("speeds", g.speeds);
  out.set("refused", g.refused);
  out.set("flushes", g.flushes);
  out.set("moves", g.moves);
  return out;
}

bool GameActors::flush(renderer::SceneRenderer& renderer, std::string* error) {
  Impl& g = *impl_;
  g.move_indices.clear();
  g.move_placements.clear();
  for (Impl::Actor& a : g.actors) {
    if (!a.moved) continue;
    a.moved = false;
    g.move_indices.push_back(a.instance);
    g.move_placements.push_back(a.placement);
  }
  if (g.move_indices.empty()) return true;
  ++g.flushes;
  g.moves += g.move_indices.size();
  return renderer.move_instances(
      std::span<const u32>(g.move_indices.data(), g.move_indices.size()),
      std::span<const renderer::SceneInstance>(g.move_placements.data(), g.move_placements.size()),
      error);
}
#endif  // ENGINE_VIEW_GAME

}  // namespace engine::view

#endif  // ENGINE_VIEW_ANIMATION
