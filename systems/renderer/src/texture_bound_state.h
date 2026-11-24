#pragma once

// The bound's state (R172 stage 1; ADR-0065; docs/subsystems/texture.md, "Levels resident by what
// the frame samples"): what `GpuScene` keeps of every built texture it holds by level, the meshes
// that sample each one, the material records that name each one, the images leaving, and the one
// thread that reads a promotion's levels off the frame. gpu_scene_texture_bound.cpp is its code.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/math/world.h>
#include <domain/gfx/image_chunks.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/texture/texture_file.h>
#include <systems/renderer/texture_bound.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace engine::renderer {

// Whether a load makes the state: `renderer.textures.bound`, or a residency ceiling
// (`renderer.textures.ceiling_mib`, R172 stage 2), read once per load.
bool texture_bound_enabled();

struct TextureBoundState {
  struct Texture {
    u64 key = 0;
    std::string path;  // the `.tex` a promotion reads its levels from
    u32 width = 0;     // the whole chain's level 0
    u32 height = 0;
    u32 levels = 0;
    gfx::Format format = gfx::Format::Undefined;
    bool variance = false;  // a normal map with its variance chain at the pair's second slot
    u32 floor_top = 0;      // the levels from here down are always resident (the floor)
    u32 resident_top = 0;   // the chain's level the resident image's level 0 is
    u32 asked_top = ~0u;    // a read in flight brings this top (~0u: none)
    u32 wanted_top = 0;     // the last evaluation's, at the lead
    u32 now_top = 0;        // the last evaluation's, at the observer's own place
    u32 pair = ~0u;         // its pair in the run
    u32 record = ~0u;       // its entry of `GpuScene::load_texture_records_`
    gfx::ImageResource image;
    gfx::ImageViewHandle view;
    gfx::ImageResource variance_image;
    gfx::ImageViewHandle variance_view;
    Vector<u32> materials;  // indices into `materials` below: the records that name it
    bool placed = false;    // the load uploaded it (a key no mesh's built set reached is not)
    bool late = false;      // counted late at the current `now_top`
    // Stage 2, the residency (R172; texture.md, "Stage 2: the trim under a ceiling"). On the
    // residency's clock (`now`): the last time level `l` or a finer one was asked by the feedback
    // or made resident, so the levels' times never fall towards the coarse end, and a level is
    // as young as the latest of the two; the load counts as making every level resident at 0.
    f64 asked_at[16] = {};
    u32 asked_level = ~0u;  // the finest level the feedback's folds asked since the last frame
    u32 last_asked = ~0u;   // the frame before's, for the trend
    u32 demote_top = ~0u;   // a demotion's read in flight brings this top (~0u: none)
    // W167: a move's read in flight (its image re-placed at the same top, out of a draining chunk).
    bool moving = false;
  };
  // A mesh's managed textures: which, and log2 of its least density times the image's shorter
  // side (texels a metre at scale 1 along the least stretched direction of any triangle).
  struct User {
    u32 texture = 0;
    f64 log2_texels_per_m = 0.0;
  };
  struct MaterialCopy {
    u32 index = 0;  // in the scene's material table
    gfx::ResolveMaterial record;
  };
  struct Leaving {
    gfx::ImageResource image;
    gfx::ImageViewHandle view;
    gfx::ImageResource variance_image;
    gfx::ImageViewHandle variance_view;
    u32 pair = ~0u;
    u32 frames = 0;
  };
  struct Read {
    u32 texture = 0;
    u32 first = 0;
    // The texture's own path (`Texture::path`, fixed after the plan): a pointer, so that asking a
    // read allocates nothing on the frame's thread (W167).
    const std::string* path = nullptr;
    texture::TextureData data;
    bool ok = false;
    bool demote = false;  // a trim's: the levels from a coarser top
    u64 frees = 0;        // the device bytes a demotion was planned to release
    bool move = false;    // W167: the same top again, to empty a draining chunk
  };
  // A level the trim may release: its texture, the level, and when it was last asked.
  struct Candidate {
    f64 asked_at = 0.0;
    u32 texture = 0;
    u32 level = 0;
  };

  Vector<Texture> textures;
  HashMap<u64, u32> by_key;         // a key the bound holds -> its texture
  Vector<Vector<User>> mesh_users;  // per mesh of the scene
  Vector<Vec4> mesh_sphere;         // per mesh: mesh-space centre, radius
  Vector<MaterialCopy> materials;
  // W167: the copies of records an admitted tile mesh wrote (under a ceiling alone), by the
  // material index, and the copies its retirement freed for the next.
  HashMap<u32, u32> copy_of_material;
  Vector<u32> free_materials;
  Vector<Leaving> leaving;
  Vector<u32> free_pairs;
  u32 first_slot = 0;
  u32 pairs = 0;
  u32 frames_leaving = 3;  // k_joint_slots + 1
  gfx::ImageResource placeholder;
  gfx::ImageViewHandle placeholder_view;

  // The observer, between evaluations.
  bool settled = false;  // a first frame has brought its levels in
  bool whole = false;    // `texture_bound_whole`: every texture to level 0
  bool have_eye = false;
  WorldPos last_eye{};  // the last frame's
  WorldPos eval_eye{};  // where the last evaluation stood
  f64 eval_f = 0.0;     // its pixels a radian, and the corner's cos^2
  f64 eval_c2 = 0.0;
  f64 eval_lead = 0.0;
  u64 frames = 0;
  u64 eval_frame = 0;
  Vector<f64> mesh_q;      // per mesh: log2 of the least footprint over scale, at the lead
  Vector<f64> mesh_q_now;  // the same at the observer's own place
  Vector<u32> order;       // the textures wanting a promotion, this evaluation

  // Stage 2, the residency: the bound evaluated (`renderer.textures.bound`), the ceiling in bytes
  // (`renderer.textures.ceiling_mib`, 0: none), the hold, the clock, and per pair the texture it
  // holds, the top it holds it from and the recorded frame it was given, which the feedback's fold
  // reads (a pair keeps its texture while it leaves, so a fold of a frame that sampled it still
  // finds whose it was; one given after the folded frame is not read).
  bool bound = false;
  u64 ceiling = 0;
  f64 hold_s = 8.0;
  f64 now = 0.0;
  Vector<u32> pair_texture;
  Vector<u32> pair_top;
  Vector<u64> pair_since;
  Vector<Candidate> candidates;  // the trim's, reserved at the plan
  Vector<u32> plan_top;          // per texture: the top the trim's plan takes it to
  u64 releasing = 0;             // the bytes demotions in flight were planned to release

  // W167 (W47's stage 1): the held textures' images in chunks of their own, so that a trimmed
  // level's bytes go back to the device; draining the chunk being emptied by moves (~0u: none);
  // the ceiling, when a device budget named it, worked out again at a cut.
  gfx::ImageChunks chunks;
  u32 draining = ~0u;
  u64 budget = 0;           // `renderer.textures.device_budget_mib` in bytes, 0: none named
  f64 budget_at = -1.0e30;  // the clock when the ceiling was last worked out from it

  // The reader: one thread, a queue in and a list out.
  std::thread worker;
  std::mutex mutex;
  std::condition_variable wake;
  Vector<Read> queue;
  Vector<Read> done;
  Vector<Read> taken;  // the frame's, swapped with `done`
  bool stop = false;
  // W167: the bytes of reads done and not yet admitted or dropped, under `mutex`. The reader takes
  // no new read while they pass `in_hand_bound` (one always goes when nothing is in hand): on
  // the forest staging an unbounded reader read 3,820 demotions ahead of a 4 MiB-a-frame
  // admission and held 30 GB of commit for them (texture.md, "Stage 3").
  u64 in_hand = 0;
  u64 in_hand_bound = 0;

  TextureBoundUse use;
};

}  // namespace engine::renderer
