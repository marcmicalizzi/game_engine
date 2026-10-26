#pragma once

// **A game's props** (docs/subsystems/apps.md, "The game contract"; roadmap P43): the objects a
// game module puts into the world — instances of meshes the scene file declared as `props`
// (`engine.scene.Prop`, renderer::SceneProp), which the load brought in and placed nowhere. The
// module places, moves and removes them through `HostApi` (`prop_place`, `prop_move`,
// `prop_remove`; module_abi.h, ABI 3), asks for the nearest one (`nearest_prop`), and the host owns
// the table: f64 positions, a yaw and a uniform scale per prop.
//
// **How a prop reaches the picture.** The live props are one run of the renderer's tail of
// instances — one `renderer::DynamicBlock` keyed `k_game_block_key` after the streamed world's tile
// blocks, or the whole tail when the scene streams nothing — handed over through
// `SceneRenderer::set_dynamic_instances` on the frames a prop changed and on no other
// (docs/subsystems/renderer.md, "Instances that come and go"): the renderer compares a block that
// comes back under its key with what it holds and writes nothing for one that did not change, so a
// frame with no change costs the world's own hand-over and nothing for the props. The scene is made
// dynamic by `--game` **only when it lists props** (main.cpp): a scene with none stays exactly the
// scene it was, with the renderer's no-allocation frame loop and the frame budgets untouched.
//
// **Why a fixed cap** (`k_max_props`, 1,024; a constant, not a tunable). The table is sized once so
// a tick allocates nothing — the module ABI's rule for both sides of the boundary — and a prop id
// packs its slot in the low bits, so the cap is also the id's shape: a tunable could not change it
// at run time without breaking every id a module's state already holds. A thousand placed objects
// is far past what a module places by hand; a game that scatters more has a scatter layer for it.
//
// **Ids.** A prop id is `slot | generation << k_prop_slot_bits`, the slot's generation counted from
// 1 at every placement, so an id is never zero, a removed prop's id is `NotFound` for the rest of
// the run rather than another prop that took its slot, and the ids a module keeps in its state
// survive
// `--game-state`: the file holds each prop's id and the restore puts it back in the same slot at
// the same generation.
//
// **A prop is visual.** The walker walks through it; the ray tracing chain and the shadows see it
// as the tail's instances are seen. Collision with props is a row of its own (roadmap P44).

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/world.h>
#include <foundation/hotload/module_abi.h>
#include <systems/renderer/scene.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::view {

inline constexpr u32 k_prop_slot_bits = 10;
inline constexpr u32 k_max_props = 1u << k_prop_slot_bits;
// `nearest_prop`'s "any mesh".
inline constexpr u32 k_any_prop = 0xFFFFFFFFu;
// The tail block's key. A tile's key is its coordinates (`world::tile_key`: x and z as u32s, the
// sign bit flipped, x high), so this is tile (INT32_MAX, INT32_MAX): 2^31 tiles out along both
// axes, past the farthest position a `WorldPos` may hold (`k_world_extent_m`), so no tile the ring
// can hold shares it.
inline constexpr u64 k_game_block_key = ~u64{0};

struct GamePropsStats {
  u64 placed = 0;
  u64 moved = 0;
  u64 removed = 0;
  u64 rewrites = 0;  // hand-overs of the block: the frames a prop changed on
  u64 queries = 0;   // nearest_prop calls
  u64 found = 0;
};

class GameProps {
 public:
  GameProps();

  // The scene's props by name, before the module loads (`renderer::read_scene_props`): what
  // `mesh_of` answers in the module's `on_load`. Then `attach` with the loaded scene, whose props
  // must be the same names in the same order; it fills in each prop's scene mesh. False with
  // `error` when they are not.
  void declare(const Vector<renderer::SceneProp>& props);
  bool attach(const renderer::SceneData& scene, std::string* error);
  bool declared() const noexcept { return !props_.empty(); }
  bool attached() const noexcept { return attached_; }
  u32 declared_count() const noexcept { return props_.size(); }
  const std::string& name_of(u32 prop_mesh) const noexcept;

  // ---- the services (module_abi.h, ABI 3) ----
  // The index of a declared prop by name: `Unsupported` when the scene declares none at all (a
  // session with no props, like `nearest_placement`'s with no world — not a load refusal),
  // `NotFound` when it declares some and not this one (remembered: the load is refused naming it).
  hotload::Result mesh_of(std::string_view name, u32& out) const noexcept;
  // A new prop of declared mesh `mesh` at `position` (world metres, three f64), turned `yaw`
  // radians about +y, at uniform `scale`: `NotFound` for a mesh index the scene has not,
  // `InvalidArgument` for a non-finite number, a scale at or below zero or a position outside the
  // world, `OutOfRange` when the table is full, `Unsupported` before the scene is attached.
  hotload::Result place(u32 mesh, const f64* position, f32 yaw, f32 scale, u32& out_prop) noexcept;
  hotload::Result move(u32 prop, const f64* position, f32 yaw) noexcept;
  hotload::Result remove(u32 prop) noexcept;
  // The nearest placed prop of `mesh` (or of any, `k_any_prop`) within `radius` metres of `from`
  // over the ground (x and z): its id, its position and its distance. Ties go to the lower slot.
  hotload::Result nearest(u32 mesh, const WorldPos& from, f64 radius, u32& out_prop, WorldPos& out,
                          f64& distance) noexcept;

  // ---- the tail ----
  // The live props as instances, in slot order, for the renderer's block; kept, and rebuilt only
  // after a change. `changed()` says a prop changed since the last `handed()`, which whoever hands
  // the block over calls once it did.
  const Vector<renderer::SceneInstance>& tail();
  bool changed() const noexcept { return dirty_; }
  void handed() noexcept;
  u32 live() const noexcept { return live_; }
  const GamePropsStats& stats() const noexcept { return stats_; }

  // ---- the state file's `props` ----
  // `[{"id","prop","position":[x,y,z],"yaw","scale"}]`: every live prop by its declared name, the
  // position as JSON doubles, the angle and the scale as the f64 of their f32, so the text reads
  // back to the same bits. `from_json` needs the props declared and takes the ids as written.
  JsonValue to_json() const;
  bool from_json(const JsonValue& value, std::string* error);
  // The summary's `props` block: `declared`, `placed`, `moved`, `removed`, `live`, `rewrites`,
  // `queries`, `found`.
  JsonValue summary_json() const;

 private:
  struct Slot {
    bool live = false;
    u32 prop = 0;  // index into `props_`
    u32 generation = 0;
    WorldPos position{};
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
  };
  static u32 id_of(u32 slot, u32 generation) noexcept {
    return slot | (generation << k_prop_slot_bits);
  }
  Slot* find(u32 id) noexcept;
  bool valid_position(const f64* position) const noexcept;

  Vector<renderer::SceneProp> props_;
  Vector<Slot> slots_;  // k_max_props, sized once by `declare`
  Vector<renderer::SceneInstance> tail_;
  bool attached_ = false;
  bool dirty_ = false;
  bool tail_stale_ = false;
  u32 live_ = 0;
  GamePropsStats stats_;
};

// ---- actors (module_abi.h, ABI 5; apps.md, "Actors: the characters a game moves")
// ----------------
//
// The props' neighbour: a prop is a mesh the module places from nothing, an **actor** is a skinned
// instance the scene file already holds and names, which the module finds, moves and plays clips
// on. The two halves live apart because they need different builds: the names are read here, with
// the scene file, before the module loads (its `on_load` may look one up, as it looks up a prop),
// in every build that runs a game; the table that moves and poses them is the animated world's
// (animation.h `GameActors`), in a build with animation, reached through `ActorHost`.

// One actor the scene file declares: an instance with a `name` that animates — an `animation`
// block, or any instance under `--animate` — by its index in the file's instances, which is its
// index in the scene's **whole read** (the file's instances come first, before scatters and
// generators). A streamed world's load keeps only some of them and the loaded index can differ;
// the animated world maps the one to the other (`loaded_actor`, below).
struct DeclaredActor {
  std::string name;
  u32 instance = 0;
};

// The scene file's actors, in instance order; a name given twice is the first instance's (the
// second is said once on stderr). False with `error` for a file that does not read.
bool read_scene_actors(const std::string& path, bool animate, Vector<DeclaredActor>& out,
                       std::string& error);

// **An actor is never streamed** (renderer.md, "An actor is never streamed"): every declared
// actor's instance into `desc.authored.keep`, before `renderer::load_scene`, so a streamed world
// keeps it in the loaded prefix — its entity, joints and instance row there on every frame — and
// never hands it to the ring that would stream it out from under the game.
void keep_actors(const Vector<DeclaredActor>& declared, renderer::SceneDesc& desc);

// **A declared actor's loaded instance** (renderer.md, "The whole read's index"): its handle is
// the whole read's index, and `loaded_of_read` (`SceneData::loaded_of_read`; empty for a scene
// read whole, `read_count` instances of which `loaded_count` are loaded) takes it to the animated
// world's and the renderer's. `loaded` is `renderer::k_not_loaded` for an instance past the scene.
// False, with a sentence naming the actor and its instance, for one a streamed world handed to its
// tail — where nothing animates or moves it, so the game would hear `NotFound` for an actor its
// scene names; never a silent miss.
bool loaded_actor(const DeclaredActor& actor, std::span<const u32> loaded_of_read, u32 read_count,
                  u32 loaded_count, u32& loaded, std::string& error);

// What the runner forwards an actor's commands to, by scene instance (`DeclaredActor::instance`;
// the runner has already checked it is one). Implemented by the animated world (animation.h).
class ActorHost {
 public:
  virtual ~ActorHost() = default;
  virtual hotload::Result place(u32 instance, const f64* position, f32 yaw) = 0;
  virtual hotload::Result play(u32 instance, std::string_view clip, f32 fade_s, bool looping) = 0;
  virtual hotload::Result stop(u32 instance, f32 fade_s) = 0;
  virtual hotload::Result speed(u32 instance, f32 speed) = 0;
  virtual hotload::Result state(u32 instance, hotload::ActorState& out) = 0;
  // The state file's `actors` (every bound actor's placement and player), and the summary's.
  virtual JsonValue to_json() const = 0;
  virtual JsonValue summary_json() const = 0;
  virtual JsonValue calls_json() const = 0;
};

}  // namespace engine::view
