#pragma once

// The boundary between the engine and a hot-reloadable game module (docs/subsystems/hotload.md).
//
// A game module is a shared library built by `engine_game_module()` (cmake/EngineGameModule.cmake)
// that links **no engine library**. This header is everything the two sides share, and it is
// written so that a module can include it with nothing but `core/base/types.h` beside it:
//
//   - the module exports one C function, `engine_hotload_describe`, which fills a `ModuleDesc`:
//     the ABI it was built against, its exported functions (name, signature, pointer) and the
//     schema type of its state;
//   - every call the module makes into the engine goes through the `HostApi` table of function
//     pointers it is handed — allocation, logging, the host's clock, and read-only views of schema
//     objects by descriptor — so the module never holds a second copy of the engine's log
//     categories, tunables or allocator registries;
//   - nothing crosses the boundary but C types, the host's own plain structs by value, and the
//     module's `schema::TypeInfo` by pointer (constant data plus the module's own construct and
//     destroy functions), which the host reads only while the module is loaded.
//
// Exceptions do not cross it either: the module is compiled like the engine (no exceptions on
// GCC and Clang, none used on MSVC), and every failure is a status code plus a line the module
// hands to `HostApi::fail`.

#include <core/base/types.h>

#include <cstddef>  // offsetof, for ENGINE_HOTLOAD_HAS

#if defined(_WIN32)
#define ENGINE_HOTLOAD_EXPORT __declspec(dllexport)
#else
#define ENGINE_HOTLOAD_EXPORT __attribute__((visibility("default")))
#endif

namespace engine::schema {
struct TypeInfo;
}

namespace engine::hotload {

// Bumped whenever anything in this header changes shape or meaning. A module built against
// another number is refused before any of its code but `describe` runs.
//
// **2** (2026-10-16): `HostApi` gained the game runner's five services at its end (`game_action`,
// `hud_text`, `hud_meter`, `end_run`, `nearest_placement`; docs/subsystems/apps.md, "The game
// contract"). Nothing before them moved, but a module built against 1 would hand the host a
// `ModuleDesc` it reads the same and call a table it believes is shorter, so it is refused all the
// same: one number, one layout.
//
// **3** (2026-10-17): `HostApi` gained, at its end, the services through which a game acts on the
// world (roadmap P43; apps.md, "The game contract"): props (`prop_mesh`, `prop_place`, `prop_move`,
// `prop_remove`, `nearest_prop`), the walker (`walker_set`, `walker_teleport`) and sound
// (`sound_clip`, `sound_play`, `sound_stop`). Additive again — nothing before them moved — and
// refused again for a module built against 2, for the same reason.
//
// **4** (2026-10-18): `HostApi` gained, at its end, the HUD's structured elements (`hud_element`,
// `hud_remove`) and the graphics state a game may read and ask to change (`graphics_state`,
// `request_graphics`; roadmap P47). **From 4 on, the table is additive for the module as well as
// the host: a module built against 3 loads on a host of 4 and runs unchanged** (`k_abi_oldest`),
// because nothing before the new entries moved and a module built against 3 calls only what 3 had.
// A module learns what the host it runs on has from `HostApi::size` (`has_entry` below; the
// helpers answer `Unsupported` for an entry the table does not reach). A module built against a
// newer ABI than the host's is still refused: it would call past the end of the host's table.
//
// **5** (2026-10-19): `HostApi` gained, at its end, the **actors** — the skinned instances of the
// scene a game moves and plays clips on (`actor_find`, `actor_spawn`, `actor_place`, `actor_play`,
// `actor_stop`, `actor_speed`, `actor_state`; roadmap S88; apps.md, "Actors: the characters a game
// moves") — and the scene's ground (`ground_height`). Additive, as 4 was: a module built against 3
// or 4 loads and runs unchanged, and `ActorState` leads with its own `size` like ABI 4's structs.
//
// **6** (2026-10-19): `HostApi` gained, at its end, the HUD's **images** and the **game's fonts**
// (`hud_image`, `hud_font`; roadmap P49; apps.md, "The HUD, provisional"), and `HudElement` grew at
// its end for the first time — `font`, `letter_spacing_px` and a drop shadow. The host reads an
// element only as far as the module's `size` says and leaves the rest at the defaults, so a module
// built against 3, 4 or 5 loads and its elements draw exactly as they drew.
//
// **7** (2026-10-25): two **optional exports** a host calls when a module has them, of two new
// signatures (roadmap W79; apps.md, "The loading presentation"): `game_loading` (`LoadingFn`),
// handed a `GameLoadProgress` every loading frame — the host's own loading screen steps aside
// and the module draws its own through the HUD it has — and `game_ready_milestone`
// (`MilestoneFn`), the `GameMilestone` the module wants reached before its `game_tick` starts and
// the player has control. `HostApi` did not change: a module built against 3 to 6 exports neither
// and loads and runs exactly as it did.
inline constexpr u32 k_abi_version = 7;
// The oldest module ABI a host of this header loads (4, above, says why 3 and not 4).
inline constexpr u32 k_abi_oldest = 3;

// The one symbol the host looks up.
inline constexpr const char* k_entry_point = "engine_hotload_describe";

// ---- views: what a module reads, and how --------------------------------------------------------

// A host object the module may read: a slot and the version it had when the host bound it. A view
// whose object was unbound or rebound reads as an error, never as memory (the same rule as the
// Luau binding's views, docs/subsystems/scripting.md "Lifetime").
struct View {
  u32 slot = 0;
  u32 version = 0;
};

// A field path resolved once against a host type ("health", "attributes.strength"), by index into
// the host's table. Resolve in `on_load`, read by handle on every call.
struct FieldRef {
  u32 index = 0xFFFFFFFFu;
};

enum class ValueKind : u32 {
  Nil = 0,  // an empty optional
  Bool,
  Int,     // every signed integer, widened to i64
  UInt,    // every unsigned integer, widened to u64
  Float,   // f32 and f64, widened to f64
  String,  // borrowed: valid until the call that read it returns
  Enum,    // `i` is the value, `str` the enumerator's name (empty when it has none)
  Vector,  // vec2/vec3/vec4/quat in `v` (f32), worldpos/dvec3 in `d` (f64); `count` components
  Id128,   // `id`
  Array,   // an array or fixed array: `count` elements, read with HostApi::read_element
};

struct Value {
  ValueKind kind = ValueKind::Nil;
  u32 count = 0;
  union {
    bool b;
    i64 i;
    u64 u;
    f64 f;
    f32 v[4];
    f64 d[3];
    u64 id[2];
  };
  struct {
    const char* data;
    u64 length;
  } str = {nullptr, 0};

  Value() noexcept : d{0.0, 0.0, 0.0} {}
};

// What a HostApi function returns. Zero is success.
enum class Result : i32 {
  Ok = 0,
  NotFound = 1,     // a type, a field path or an enumerator name that does not exist
  StaleView = 2,    // the view's object was unbound or rebound
  WrongType = 3,    // a view of one type read with a field resolved against another
  Unsupported = 4,  // a field kind views do not read (json, bytes, maps)
  OutOfRange = 5,   // an element index past the end
  InvalidArgument = 6,
};

enum class LogLevel : u32 { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

// ---- the HUD's structured elements (ABI 4; apps.md, "The HUD, provisional") --------------------

// Where an element stands: one of nine points of the picture's safe region (5 % in from each edge):
// its corners, the centres of its edges, and its centre.
enum class HudAnchor : u32 {
  TopLeft = 0,
  Top = 1,
  TopRight = 2,
  Left = 3,
  Centre = 4,
  Right = 5,
  BottomLeft = 6,
  Bottom = 7,
  BottomRight = 8,
};
// How the text's box sits on the anchor across: its left edge, its centre or its right edge there.
// Down, the anchor's row decides: a top anchor hangs the box from the point, a middle one centres
// it, a bottom one stands it on the point.
enum class HudAlign : u32 { Left = 0, Centre = 1, Right = 2 };
enum class HudPanel : u32 { None = 0, Solid = 1 };

// One element: a line of text the module places and the host keeps — **retained**, unlike
// `hud_text`'s lines: it stands from the tick that said it until the module says it again under
// the same id (which replaces it), hides it (`visible` 0), removes it (`hud_remove`), or a new run
// starts. **Every length is in pixels of a 1080-pixel-high picture at 100 %**: the host multiplies
// `size_px`, `offset_px`, `outline_px` and `panel_padding_px` by the HUD's scale (the picture's
// height over 1080, times the display's scale) and holds the font to a minimum, so a module never
// sees the window's size and an element is legible at any. Colours are RGBA, 8 bits a channel.
// `size` is the struct's size as the module built it, so the struct can grow the way `HostApi`
// does. The text is UTF-8, copied by the host (up to 127 bytes); nothing is kept past the call.
struct HudElement {
  u32 size = sizeof(HudElement);
  u32 id = 0;  // the module's own; ids from `k_hud_reserved` up are the host's
  HudAnchor anchor = HudAnchor::Centre;
  HudAlign align = HudAlign::Centre;
  f32 offset_px[2] = {0.0f, 0.0f};  // from the anchor: x right, y down
  f32 size_px = 18.0f;              // the font's size
  f32 outline_px = 0.0f;            // 0: no outline
  u8 color[4] = {255, 255, 255, 255};
  u8 outline_color[4] = {0, 0, 0, 255};
  HudPanel panel = HudPanel::None;
  u8 panel_color[4] = {0, 0, 0, 115};  // the legacy panel's 0.45 alpha
  f32 panel_padding_px = 6.0f;
  u32 visible = 1;
  const char* text = nullptr;
  u64 text_length = 0;
  // ---- ABI 6 (2026-10-19): read by the host only when `size` reaches them ----
  u32 font = 0;                            // 0: the HUD's own; else a handle `hud_font` returned
  f32 letter_spacing_px = 0.0f;            // added after each character; may be negative
  f32 shadow_offset_px[2] = {0.0f, 0.0f};  // a drop shadow, x right, y down; 0, 0: none
  u8 shadow_color[4] = {0, 0, 0, 160};
  u32 reserved = 0;  // zero: the next field's place
};
// ABI 4's (and 5's) element: everything before `font`. The host takes any element from this size
// up, copies it into its own up to the module's `size` and leaves the rest at the defaults.
inline constexpr u32 k_hud_element_v4_size = 72;

// **An image on the HUD** (ABI 6): an element of the same id space as `HudElement` (an id names one
// or the other), on the same nine anchors with the same alignment and 1080p offsets, drawing a PNG
// or JPEG from the **game's content directory**: `path` is `/`-separated and relative, looked up
// beside the scene file and then beside the module (an absolute path or a `..` is
// `InvalidArgument`). It is drawn `size_px` big — both 0: the image's own pixels as 1080p pixels;
// one 0: that side from the other by the image's aspect — times `tint`, and with **nine-slice
// borders** `slice_px` (left, top, right, bottom, in the image's pixels, drawn at the HUD's scale)
// the corners keep their size and the edges and the middle stretch, so one small PNG is a panel of
// any size. All zero: the whole image stretched. Retained like an element; `hud_remove` removes it.
// Led by its `size`.
struct HudImage {
  u32 size = sizeof(HudImage);
  u32 id = 0;  // the module's own, below `k_hud_reserved`, shared with the text elements
  HudAnchor anchor = HudAnchor::Centre;
  HudAlign align = HudAlign::Centre;
  f32 offset_px[2] = {0.0f, 0.0f};             // from the anchor: x right, y down
  f32 size_px[2] = {0.0f, 0.0f};               // width, height
  f32 slice_px[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // left, top, right, bottom
  u8 tint[4] = {255, 255, 255, 255};
  u32 visible = 1;
  const char* path = nullptr;  // UTF-8, copied by the host (up to 255 bytes)
  u64 path_length = 0;
};

// Element ids from here up are the host's: `hud_text`'s line n is `k_hud_line_id + n` and
// `hud_meter`'s slot m is `k_hud_meter_id + m`, the top-left stack, so a report lists every element
// one way. `hud_element` and `hud_remove` refuse them (`InvalidArgument`).
inline constexpr u32 k_hud_reserved = 0xFFFF0000u;
inline constexpr u32 k_hud_line_id = 0xFFFF0000u;
inline constexpr u32 k_hud_meter_id = 0xFFFF0100u;

// ---- graphics, as the host resolved them (ABI 4) -----------------------------------------------

// What the session draws with — not what was asked for: the host's resolved settings, by name, and
// the reasons a setting is not what the command line or a request asked (`auto` became `csm`
// because the device has no ray queries, ...). Names, not numbers, so the engine can add a raster
// path without changing the ABI. Filled into the caller's struct up to its `size`.
struct GraphicsState {
  u32 size = sizeof(GraphicsState);
  u32 width = 0;  // the picture, pixels
  u32 height = 0;
  u32 ray_tracing =
      0;        // 1: the frame builds acceleration structures (traced shadows, the ray path)
  u32 hdr = 0;  // 1: the display is an HDR encoding
  u32 reason_count = 0;
  char raster[24] = {};   // "hw", "vertex", "rt", "sw", "direct"
  char shadows[24] = {};  // "rt", "csm", "off"
  char present[24] = {};  // "fifo", "mailbox", "fifo-latest-ready", "offscreen", ...
  char display[24] = {};  // "sdr", "hdr10", "scrgb", "linear"
  char tier[24] = {};     // the device's verdict: "raster", "rt", "rt-cluster"
  char reasons[4][96] = {};
};

// A change a module asks for. The host **records** it and applies it at its next safe point — a
// restart of the session, today; nothing is rebuilt under a running frame — and answers
// `Unsupported` for what it cannot change at all. An empty name, or a zero size, leaves that
// setting as it is.
struct GraphicsRequest {
  u32 size = sizeof(GraphicsRequest);
  u32 width = 0;
  u32 height = 0;
  u32 reserved = 0;
  char raster[24] = {};
  char shadows[24] = {};
  char present[24] = {};
  char display[24] = {};
};

// ---- actors, as the host keeps them (ABI 5; apps.md, "Actors: the characters a game moves") ----

// One actor as it stands after the last tick: where, which way, which clip and where in it, and
// the hash of the pose the last animation step left — the summary's `pose_hash` as a number, so a
// module and a test that reads the report compare the same thing. Filled into the caller's struct
// up to its `size`, as `GraphicsState` is.
struct ActorState {
  u32 size = sizeof(ActorState);
  u32 instance = 0;                   // the scene instance the actor is
  f64 position[3] = {0.0, 0.0, 0.0};  // world metres: where the instance's origin stands
  f32 yaw = 0.0f;                     // radians about +y, as `prop_place` turns a prop
  f32 playhead_s = 0.0f;              // seconds into `clip`
  f32 speed = 1.0f;                   // the playback rate (`actor_speed`)
  f32 weight = 1.0f;                  // 1 the clip, toward 0 a fade to the rest pose
  u32 looping = 0;
  u32 tier = 0;        // the animation LOD tier, 0 the finest
  u32 playing = 0;     // 0: stopped, the rest pose
  u32 fading = 0;      // a cross-fade, a fade in or a fade to rest in flight
  u64 pose_hash = 0;   // over the joint matrices of the last step
  char clip[64] = {};  // the library's name ("bar/bend"); empty when stopped
};

// **The startup's milestones** (ABI 7; apps.md, "--startup-trace"), in the order a load reaches
// them: what `game_ready_milestone` names and `GameLoadProgress::milestones` has a bit for.
enum class GameMilestone : u32 {
  Main = 0,
  Device = 1,
  SceneLoaded = 2,
  GpuScene = 3,
  FirstFrame = 4,
  PlayerControllable = 5,  // the default: control as soon as the walker has taken a tick
  GroundCollisionReady = 6,
  NearbyStable = 7,
};
inline constexpr u32 k_game_milestones = 8;

// **Where the load is** (ABI 7), handed to `game_loading` every loading frame: the host's own
// `LoadProgress` (core/profiling/startup_trace.h) in the ABI's terms. The strings are borrowed,
// NUL-terminated, and valid for the call. Led by its `size`, as the host built it; a module reads
// no field past it.
struct GameLoadProgress {
  u32 size = sizeof(GameLoadProgress);
  u32 stages_done = 0;
  u32 stages_expected = 0;
  u32 milestones = 0;    // bit i: `GameMilestone` i has been reached
  f32 fraction = 0.0f;   // [0, 1], never falls during one load
  u32 ready = 0;         // 1 once the milestone the module asked for is reached (the last call)
  u64 bytes_read = 0;    // the process's reads since the load began
  u64 items = 0;         // what the load's stages counted: meshes, tiles, instances stood
  u64 elapsed_ns = 0;    // since the load began
  const char* stage = "";   // the load's stage now ("load scene")
  const char* detail = "";  // what it is doing inside ("cluster read: tree.clusters"), or ""
};

// The engine as a module sees it. Every function takes the table's `host` first. The table and
// everything it points at outlive every call into the module; the module may keep the pointer.
struct HostApi {
  u32 abi_version;
  u32 size;  // sizeof(HostApi) as the host built it
  void* host;

  // The engine heap, under the hotload tag. Never malloc in a module and free in the host, or the
  // reverse: on Windows a DLL may carry its own C runtime and its own heap.
  void* (*allocate)(void* host, u64 bytes, u64 align);
  void (*deallocate)(void* host, void* p, u64 bytes, u64 align);
  // A record in the `hotload` category with the module's name as a field. No fatal level: a module
  // does not get to stop the engine.
  void (*log)(void* host, LogLevel level, const char* message, u64 length);
  // The host's fixed-step clock, never the wall clock (what `engine.tick()` is to a Luau script).
  u64 (*tick)(void* host);
  f64 (*fixed_step)(void* host);

  // Resolves a dotted path through nested structs of a registered schema type. A failure is
  // remembered: when `on_load` returns, every path that did not resolve is listed in the
  // refusal, so a module with three misspelt names hears about all three at once.
  Result (*resolve)(void* host, const char* type, u64 type_length, const char* path,
                    u64 path_length, FieldRef* out);
  // An enumerator's value by name, for an enum-typed field; remembered like `resolve`.
  Result (*enum_value)(void* host, FieldRef field, const char* name, u64 length, i64* out);
  // Reads one field of a viewed object. On any failure `out` is Nil.
  Result (*read)(void* host, View view, FieldRef field, Value* out);
  // Element `index` of an array field (scalar elements only).
  Result (*read_element)(void* host, View view, FieldRef field, u64 index, Value* out);

  // The line a failing `describe` or `on_load` leaves for the host to report. Called from any
  // other export, it is that export's failure: a game runner ends its session with the line
  // (docs/subsystems/apps.md, "The game contract"), and a host that runs no game ignores it.
  void (*fail)(void* host, const char* message, u64 length);

  // ---- a game runner's services (ABI 2; docs/subsystems/apps.md, "The game contract") ----
  // engine-view's `--game` answers them; a host that runs no game answers `Unsupported` and
  // ignores the rest. Everything is by value or into the caller's memory: nothing the host
  // returns needs freeing, and nothing a module passes is kept past the call.

  // Declares a game action the module reads (`interact`, `use`, `game_1`..`game_4`): `out_bit` is
  // its bit in `GameTick`'s `actions_pressed`, `actions_held` and `actions_released`. In `on_load`
  // only; an unknown name is remembered like a field path that does not resolve, so the load is
  // refused naming it.
  Result (*game_action)(void* host, const char* name, u64 length, u32* out_bit);
  // This tick's HUD: text line `line` (0 at the top), and a horizontal meter `value` of `max`
  // under a name. The host clears both at the start of every tick, so a module says each tick
  // what the HUD shows; the last tick before a frame is what the frame draws.
  void (*hud_text)(void* host, u32 line, const char* text, u64 length);
  void (*hud_meter)(void* host, const char* name, u64 length, f64 value, f64 max);
  // Ends the run after this tick's frame, with `json` (an object, as text) as the run's result in
  // the summary and the report. Text that is not a JSON object ends the run with an error.
  void (*end_run)(void* host, const char* json, u64 length);
  // The nearest placement of a kind (a scatter's or the ruins', by name) within `radius` metres
  // of the walker's feet: its position (world, three f64) and its distance. `NotFound` when the
  // session streams no world, or nothing of that kind is that near.
  Result (*nearest_placement)(void* host, const char* kind, u64 length, f64 radius,
                              f64* out_position, f64* out_distance);

  // ---- a game acts on the world (ABI 3; apps.md, "The game contract"; roadmap P43) ----
  // The same style: `host` first, by value or into the caller's memory, nothing kept past the call.
  // A host that runs no game answers `Unsupported` to every one.

  // **Props**: instances of the meshes the scene file declared as `props` (`engine.scene.Prop`),
  // loaded with the scene and placed by nothing but the module. `prop_mesh`, in `on_load` only, is
  // the index of a declared prop by name: `NotFound` is remembered like a field path that does not
  // resolve, so the load is refused naming it; `Unsupported` — the session's scene declares no
  // props at all, as `nearest_placement` has no world — is not, so a module runs on any scene and
  // places nothing there. `prop_place` makes a new prop of that mesh at `position` (world metres,
  // three f64), turned `yaw` radians about +y, at uniform `scale`, and hands back its id:
  // `OutOfRange` when the table is full (1,024 props; `view::k_max_props`), `InvalidArgument` for a
  // non-finite number, a scale at or below zero or a position outside the world. `prop_move` and
  // `prop_remove` take the id, and `NotFound` is an id that is not placed (a removed prop's id
  // stays dead). `nearest_prop` is the nearest placed prop of a mesh (or of any, `k_any_prop`)
  // within `radius` metres of the walker's feet over the ground, like `nearest_placement`: its id,
  // its position and its distance. A prop is visual — the walker walks through it — and reaches the
  // picture on the frame after the tick that changed it.
  Result (*prop_mesh)(void* host, const char* name, u64 length, u32* out_mesh);
  Result (*prop_place)(void* host, u32 mesh, const f64* position, f32 yaw, f32 scale,
                       u32* out_prop);
  Result (*prop_move)(void* host, u32 prop, const f64* position, f32 yaw);
  Result (*prop_remove)(void* host, u32 prop);
  Result (*nearest_prop)(void* host, u32 mesh, f64 radius, u32* out_prop, f64* out_position,
                         f64* out_distance);
  // **The walker.** `walker_set` scales its walking and sprinting speeds — each clamped to [0, 4],
  // 1 the recording's own `WalkParams` to the bit — from the next tick on; `InvalidArgument` for a
  // non-finite number. `walker_teleport` puts the walker's feet at `position` on this tick,
  // standing on the ground under it, the camera with it at `heading` and `pitch` (radians, as
  // `GameTick` gives them; the pitch held to the session's limit); flying, the camera itself goes
  // there. `InvalidArgument` for a non-finite number or a position outside the world. Both are
  // module calls at a tick and so in the recording as what they are: a recorded `--game` session
  // replays to the same trajectory **with the same module**, and is refused without it. A module
  // reads the effect through `feet` and `moved_m` next tick; `GameTick` gains nothing.
  Result (*walker_set)(void* host, f32 speed_scale, f32 sprint_scale);
  Result (*walker_teleport)(void* host, const f64* position, f32 heading, f32 pitch);
  // **Sound**: clips the scene file declared as `sounds` (`engine.scene.Sound`), decoded with the
  // scene. `sound_clip`, in `on_load` only, is a declared clip's index by name, remembered and
  // refused like `prop_mesh` (`Unsupported`, not remembered, on a scene with none or in a build
  // without audio). `sound_play` plays it at `position` (world, three f64), or on the listener with
  // `k_sound_2d`, at `gain`, and hands back a voice; `sound_stop` stops one. The commands are
  // output: nothing flows back into the module, so a replay is the same replay with or without a
  // device.
  // **Answered `Unsupported` by every host until the audio half of P43 lands**; the entries are
  // here so that landing it does not change the ABI again.
  Result (*sound_clip)(void* host, const char* name, u64 length, u32* out_clip);
  Result (*sound_play)(void* host, u32 clip, const f64* position, f32 gain, u32* out_voice);
  Result (*sound_stop)(void* host, u32 voice);

  // ---- ABI 4: the HUD's structured elements and the graphics state (roadmap P47) ----
  // A module built against 3 never reaches these; one built against 4 checks `size` (`has_entry`)
  // before it calls them on a host it does not know, which the helpers below do for it.

  // **A HUD element** (`HudElement` above), placed or replaced by its id, retained until removed.
  // `InvalidArgument` for a reserved id, an anchor, alignment or panel outside its enum, a
  // non-finite length, a size at or below zero, a negative outline or padding, a NUL inside the
  // text's length, or a struct smaller than ABI 4's; `OutOfRange` when the host's table is full
  // (64 elements). `hud_remove`: `NotFound` for an id that is not placed.
  Result (*hud_element)(void* host, const HudElement* element);
  Result (*hud_remove)(void* host, u32 id);
  // **The graphics the session resolved** (`GraphicsState`), read-only, into the caller's struct;
  // and a request the host records for its next safe point (`GraphicsRequest`).
  // `request_graphics` answers `Ok` for a request recorded, `Unsupported` for one asking what the
  // host cannot change (a raster path the device lacks, HDR on an SDR display, a name it does not
  // know), and `InvalidArgument` for a malformed struct.
  Result (*graphics_state)(void* host, GraphicsState* out);
  Result (*request_graphics)(void* host, const GraphicsRequest* request);

  // ---- ABI 5: actors and the ground (roadmap S88; apps.md, "Actors: the characters a game
  // moves") ----
  // An **actor** is a skinned instance the scene file names (`"name"` on an instance that animates:
  // an `animation` block, or any under `--animate`). Its handle is its scene instance's index —
  // stable for the scene file, and what a module keeps in its state. A command is applied **at the
  // tick the module made it**, before that tick's animation step, so a recorded session replays to
  // the same poses with the same module; the picture takes a move on the next frame, as it does a
  // prop's. A module built against 3 or 4 never reaches these, and the helpers below answer
  // `Unsupported` on a host whose table ends before them.

  // The actor named `name`, in `on_load` or at any tick. `NotFound` — the scene has actors and not
  // this one — is remembered in `on_load` like a field path that does not resolve, so the load is
  // refused naming it; `Unsupported` — the session has no actors at all (the scene names none, or
  // the build has no animation) — is a plain answer, so one module runs on any scene.
  Result (*actor_find)(void* host, const char* name, u64 length, u32* out_actor);
  // **A new actor at run time**: a skinned instance of a scene mesh made by the module. Answered
  // `Unsupported` by every host until the renderer draws a deformed instance in its dynamic tail
  // (roadmap R84's remainder); the entry is here so that landing it does not change the ABI again.
  Result (*actor_spawn)(void* host, const char* mesh, u64 length, const f64* position, f32 yaw,
                        u32* out_actor);
  // Stands the actor's origin at `position` (world metres, three f64) turned `yaw` radians about
  // +y, level — the scene's pitch and roll are dropped, its scale kept — at this tick. Every actor
  // moved in a tick reaches the renderer in one write before the next frame. `NotFound` for a
  // handle that is not an actor, `InvalidArgument` for a non-finite number or a position outside
  // the world, `Unsupported` before the scene is loaded (`on_load`) or in a session whose renderer
  // cannot move a loaded instance (a scene that also declares props, or streams a world: the
  // renderer's tail owns those instances' table, apps.md says what it needs).
  Result (*actor_place)(void* host, u32 actor, const f64* position, f32 yaw);
  // Plays the actor's clip `clip` (its library name "bar/bend", the name without the file's prefix
  // "bend", or its index among the skeleton's clips as digits), cross-fading over `fade_s` seconds
  // from whatever plays (0 a cut; from the rest pose a fade in), looping or not. `NotFound` for an
  // actor or a clip its skeleton has not, `InvalidArgument` for a negative or non-finite fade.
  Result (*actor_play)(void* host, u32 actor, const char* clip, u64 length, f32 fade_s,
                       u32 looping);
  // Fades the actor to its rest pose over `fade_s` seconds (0 at once) and stops its clip.
  Result (*actor_stop)(void* host, u32 actor, f32 fade_s);
  // The playback rate: 1 the clip's own, 0 holds the pose, negative plays backwards.
  Result (*actor_speed)(void* host, u32 actor, f32 speed);
  // The actor as the last tick left it (`ActorState`), into the caller's struct up to its size.
  // `InvalidArgument` for a struct smaller than ABI 5's.
  Result (*actor_state)(void* host, u32 actor, ActorState* out);
  // **The ground's height at (x, z)**, world metres: the scene's ground provider at the nearest
  // millimetre, the function the walker stands on — deterministic, a function of the scene and the
  // ground's time. `Unsupported` for a scene with no ground, `InvalidArgument` for a non-finite
  // coordinate.
  Result (*ground_height)(void* host, f64 x, f64 z, f64* out_height);

  // ---- ABI 6: the HUD's images and the game's fonts (roadmap P49; apps.md, "The HUD,
  // provisional") ----
  // **An image** (`HudImage` above), placed or replaced by its id like `hud_element`. The host
  // decodes a path once, the first time it is named (so a bad file is answered here), and uploads
  // it when a frame first draws it. `InvalidArgument` for what `hud_element` refuses, a size or a
  // border that is negative or not finite, an empty, absolute or `..` path, a NUL inside its
  // length, a file `foundation/image` cannot decode, or a struct smaller than ABI 6's; `NotFound`
  // for a path in neither the scene's directory nor the module's; `OutOfRange` past the element
  // table (64) or sixteen paths.
  Result (*hud_image)(void* host, const HudImage* image);
  // **A font the game ships**: a TrueType or OpenType file by path (looked up as an image's is),
  // added to the HUD's atlas and baked at each element's size on the host's scale ladder; `size_px`
  // (1080p pixels, in (0, 128]) is the size it is baked at first. Answers a handle for
  // `HudElement::font`; the same path twice is the same handle. `NotFound` as for an image,
  // `InvalidArgument` for a file that is not a font or a bad size or path, `OutOfRange` past eight.
  Result (*hud_font)(void* host, const char* path, u64 length, f32 size_px, u32* out_font);
};

// Whether the host's table reaches the entry at `offset` (`offsetof(HostApi, hud_element)`): a
// module built against a newer header asks before it calls an entry an older host may not have.
// `ENGINE_HOTLOAD_HAS(api, hud_element)` spells it.
inline bool has_entry(const HostApi* api, u64 offset) noexcept {
  return api != nullptr && api->size >= offset + sizeof(void (*)());
}
#define ENGINE_HOTLOAD_HAS(api, entry) \
  ::engine::hotload::has_entry((api), offsetof(::engine::hotload::HostApi, entry))

// `nearest_prop`'s "a prop of any mesh", and `sound_play`'s "on the listener".
inline constexpr u32 k_any_prop = 0xFFFFFFFFu;
inline constexpr const f64* k_sound_2d = nullptr;

// What every exported function receives first. Owned by the host, at the same address for the
// module's whole life across reloads; `state` points at the module's state object (null for a
// module without one) and changes when a reload migrates it.
struct ModuleContext {
  const HostApi* api;
  void* state;
};

// ---- exported functions: a closed set of signatures ---------------------------------------------

using VoidFn = void (*)(ModuleContext*);
using NumberFn = f64 (*)(ModuleContext*, f64);
using ViewNumberFn = f64 (*)(ModuleContext*, View);
using TickFn = void (*)(ModuleContext*, View self, f32 dt);
using ConditionFn = bool (*)(ModuleContext*, View a, View b);
// ABI 7's optional exports: `game_loading` and `game_ready_milestone` (a `GameMilestone`).
using LoadingFn = void (*)(ModuleContext*, const GameLoadProgress* progress);
using MilestoneFn = u32 (*)(ModuleContext*);

// A signature's number. A function type that is not in the list does not compile into an export
// table, and the host refuses to hand out a function under a signature it was not exported with,
// so a module and a host that disagree about a call are stopped before the call.
template <class F>
inline constexpr u32 k_signature = 0;
template <>
inline constexpr u32 k_signature<VoidFn> = 1;
template <>
inline constexpr u32 k_signature<NumberFn> = 2;
template <>
inline constexpr u32 k_signature<ViewNumberFn> = 3;
template <>
inline constexpr u32 k_signature<TickFn> = 4;
template <>
inline constexpr u32 k_signature<ConditionFn> = 5;
template <>
inline constexpr u32 k_signature<LoadingFn> = 6;
template <>
inline constexpr u32 k_signature<MilestoneFn> = 7;
// The largest signature number this header knows (an export naming a larger one is refused).
inline constexpr u32 k_signature_max = 7;

struct Export {
  const char* name;
  u32 signature;
  void (*function)();
};

template <class F>
Export export_function(const char* name, F function) noexcept {
  static_assert(k_signature<F> != 0,
                "engine hotload: this function's type is not one of the export signatures in "
                "module_abi.h (VoidFn, NumberFn, ViewNumberFn, TickFn, ConditionFn, LoadingFn, "
                "MilestoneFn)");
  return Export{name, k_signature<F>, reinterpret_cast<void (*)()>(function)};
}

// ---- the description --------------------------------------------------------------------------

struct ModuleDesc {
  u32 abi_version;     // k_abi_version as the module saw it
  u32 desc_size;       // sizeof(ModuleDesc) as the module saw it
  u32 type_info_size;  // sizeof(schema::TypeInfo) the state type was built against (0: no state)
  u32 export_count;
  const char* name;
  const schema::TypeInfo* state_type;  // null: the module keeps no state
  const Export* exports;
  // Called on the new build before it replaces the old one, with the new state already
  // migrated: resolve field paths here. Non-zero, or any path that did not resolve, refuses
  // the load and keeps the old module running.
  i32 (*on_load)(ModuleContext*);
  // Called on the old build once its replacement has loaded, before its state is destroyed.
  void (*on_unload)(ModuleContext*);
};

using DescribeFn = i32 (*)(const HostApi* api, ModuleDesc* out);

// ---- module-side conveniences (header-only, so a module needs nothing else) ---------------------

inline void fail(const ModuleContext* context, const char* message) noexcept {
  u64 n = 0;
  while (message[n] != '\0')
    ++n;
  context->api->fail(context->api->host, message, n);
}

inline void fail(const HostApi* api, const char* message) noexcept {
  u64 n = 0;
  while (message[n] != '\0')
    ++n;
  api->fail(api->host, message, n);
}

inline Result resolve(const ModuleContext* context, const char* type, const char* path,
                      FieldRef& out) noexcept {
  u64 t = 0;
  while (type[t] != '\0')
    ++t;
  u64 p = 0;
  while (path[p] != '\0')
    ++p;
  return context->api->resolve(context->api->host, type, t, path, p, &out);
}

inline Result enum_value(const ModuleContext* context, FieldRef field, const char* name,
                         i64& out) noexcept {
  u64 n = 0;
  while (name[n] != '\0')
    ++n;
  return context->api->enum_value(context->api->host, field, name, n, &out);
}

inline Result read(const ModuleContext* context, View view, FieldRef field, Value& out) noexcept {
  return context->api->read(context->api->host, view, field, &out);
}

inline Result read_element(const ModuleContext* context, View view, FieldRef field, u64 index,
                           Value& out) noexcept {
  return context->api->read_element(context->api->host, view, field, index, &out);
}

inline void log(const ModuleContext* context, LogLevel level, const char* message) noexcept {
  u64 n = 0;
  while (message[n] != '\0')
    ++n;
  context->api->log(context->api->host, level, message, n);
}

// ---- the game runner's services (ABI 2) ----

inline u64 text_length(const char* text) noexcept {
  u64 n = 0;
  while (text[n] != '\0')
    ++n;
  return n;
}

inline Result game_action(const ModuleContext* context, const char* name, u32& bit) noexcept {
  return context->api->game_action(context->api->host, name, text_length(name), &bit);
}

// Whether action `bit` (from `game_action`) is set in one of `GameTick`'s three masks.
inline bool action_set(const Value& mask, u32 bit) noexcept {
  return bit < 64 && ((mask.u >> bit) & 1u) != 0;
}

inline void hud_text(const ModuleContext* context, u32 line, const char* text) noexcept {
  context->api->hud_text(context->api->host, line, text, text_length(text));
}

inline void hud_meter(const ModuleContext* context, const char* name, f64 value, f64 max) noexcept {
  context->api->hud_meter(context->api->host, name, text_length(name), value, max);
}

inline void end_run(const ModuleContext* context, const char* json) noexcept {
  context->api->end_run(context->api->host, json, text_length(json));
}

inline Result nearest_placement(const ModuleContext* context, const char* kind, f64 radius,
                                f64 (&position)[3], f64& distance) noexcept {
  return context->api->nearest_placement(context->api->host, kind, text_length(kind), radius,
                                         position, &distance);
}

// ---- a game acts on the world (ABI 3) ----

inline Result prop_mesh(const ModuleContext* context, const char* name, u32& mesh) noexcept {
  return context->api->prop_mesh(context->api->host, name, text_length(name), &mesh);
}

inline Result prop_place(const ModuleContext* context, u32 mesh, const f64 (&position)[3], f32 yaw,
                         f32 scale, u32& prop) noexcept {
  return context->api->prop_place(context->api->host, mesh, position, yaw, scale, &prop);
}

inline Result prop_move(const ModuleContext* context, u32 prop, const f64 (&position)[3],
                        f32 yaw) noexcept {
  return context->api->prop_move(context->api->host, prop, position, yaw);
}

inline Result prop_remove(const ModuleContext* context, u32 prop) noexcept {
  return context->api->prop_remove(context->api->host, prop);
}

inline Result nearest_prop(const ModuleContext* context, u32 mesh, f64 radius, u32& prop,
                           f64 (&position)[3], f64& distance) noexcept {
  return context->api->nearest_prop(context->api->host, mesh, radius, &prop, position, &distance);
}

inline Result walker_set(const ModuleContext* context, f32 speed_scale, f32 sprint_scale) noexcept {
  return context->api->walker_set(context->api->host, speed_scale, sprint_scale);
}

inline Result walker_teleport(const ModuleContext* context, const f64 (&position)[3], f32 heading,
                              f32 pitch) noexcept {
  return context->api->walker_teleport(context->api->host, position, heading, pitch);
}

inline Result sound_clip(const ModuleContext* context, const char* name, u32& clip) noexcept {
  return context->api->sound_clip(context->api->host, name, text_length(name), &clip);
}

// `position` null (`k_sound_2d`) plays on the listener.
inline Result sound_play(const ModuleContext* context, u32 clip, const f64* position, f32 gain,
                         u32& voice) noexcept {
  return context->api->sound_play(context->api->host, clip, position, gain, &voice);
}

inline Result sound_stop(const ModuleContext* context, u32 voice) noexcept {
  return context->api->sound_stop(context->api->host, voice);
}

// ---- the HUD's elements and the graphics state (ABI 4) ----
// Each answers `Unsupported` on a host whose table ends before the entry.

// `element.text` from a C string when `text_length` is left at zero.
inline Result hud_element(const ModuleContext* context, const HudElement& element) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, hud_element)) return Result::Unsupported;
  HudElement e = element;
  if (e.text != nullptr && e.text_length == 0) e.text_length = text_length(e.text);
  return context->api->hud_element(context->api->host, &e);
}

inline Result hud_remove(const ModuleContext* context, u32 id) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, hud_remove)) return Result::Unsupported;
  return context->api->hud_remove(context->api->host, id);
}

inline Result graphics_state(const ModuleContext* context, GraphicsState& out) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, graphics_state)) return Result::Unsupported;
  return context->api->graphics_state(context->api->host, &out);
}

inline Result request_graphics(const ModuleContext* context,
                               const GraphicsRequest& request) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, request_graphics)) return Result::Unsupported;
  return context->api->request_graphics(context->api->host, &request);
}

// ---- actors and the ground (ABI 5) ----
// Each answers `Unsupported` on a host whose table ends before the entry.

inline Result actor_find(const ModuleContext* context, const char* name, u32& actor) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, actor_find)) return Result::Unsupported;
  return context->api->actor_find(context->api->host, name, text_length(name), &actor);
}

inline Result actor_spawn(const ModuleContext* context, const char* mesh, const f64 (&position)[3],
                          f32 yaw, u32& actor) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, actor_spawn)) return Result::Unsupported;
  return context->api->actor_spawn(context->api->host, mesh, text_length(mesh), position, yaw,
                                   &actor);
}

inline Result actor_place(const ModuleContext* context, u32 actor, const f64 (&position)[3],
                          f32 yaw) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, actor_place)) return Result::Unsupported;
  return context->api->actor_place(context->api->host, actor, position, yaw);
}

inline Result actor_play(const ModuleContext* context, u32 actor, const char* clip, f32 fade_s,
                         bool looping) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, actor_play)) return Result::Unsupported;
  return context->api->actor_play(context->api->host, actor, clip, text_length(clip), fade_s,
                                  looping ? 1u : 0u);
}

inline Result actor_stop(const ModuleContext* context, u32 actor, f32 fade_s) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, actor_stop)) return Result::Unsupported;
  return context->api->actor_stop(context->api->host, actor, fade_s);
}

inline Result actor_speed(const ModuleContext* context, u32 actor, f32 speed) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, actor_speed)) return Result::Unsupported;
  return context->api->actor_speed(context->api->host, actor, speed);
}

inline Result actor_state(const ModuleContext* context, u32 actor, ActorState& out) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, actor_state)) return Result::Unsupported;
  return context->api->actor_state(context->api->host, actor, &out);
}

inline Result ground_height(const ModuleContext* context, f64 x, f64 z, f64& height) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, ground_height)) return Result::Unsupported;
  return context->api->ground_height(context->api->host, x, z, &height);
}

// ---- the HUD's images and fonts (ABI 6) ----
// Each answers `Unsupported` on a host whose table ends before the entry.

// `image.path` from a C string when `path_length` is left at zero.
inline Result hud_image(const ModuleContext* context, const HudImage& image) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, hud_image)) return Result::Unsupported;
  HudImage i = image;
  if (i.path != nullptr && i.path_length == 0) i.path_length = text_length(i.path);
  return context->api->hud_image(context->api->host, &i);
}

inline Result hud_font(const ModuleContext* context, const char* path, f32 size_px,
                       u32& font) noexcept {
  if (!ENGINE_HOTLOAD_HAS(context->api, hud_font)) return Result::Unsupported;
  return context->api->hud_font(context->api->host, path, text_length(path), size_px, &font);
}

template <class State>
State& state(ModuleContext* context) noexcept {
  return *static_cast<State*>(context->state);
}

}  // namespace engine::hotload

// Opens the module's entry point:
//
//   ENGINE_HOTLOAD_DESCRIBE(api, out) {
//     out->name = "sandbox_npc";
//     ...
//     return 0;
//   }
#define ENGINE_HOTLOAD_DESCRIBE(api, out)                                 \
  extern "C" ENGINE_HOTLOAD_EXPORT ::engine::i32 engine_hotload_describe( \
      const ::engine::hotload::HostApi* api, ::engine::hotload::ModuleDesc* out)
