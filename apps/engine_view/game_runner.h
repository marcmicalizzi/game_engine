#pragma once

// **The game runner** behind `engine-view --game <module>` (docs/subsystems/apps.md, "--game:
// running a game module"; roadmap P41): a hot-reloadable game module (foundation/hotload, built by
// `engine_game_module()`) run from the walking session, ticked on every tick after the walker and
// before the frame, and allowed to say a few things back — HUD lines and meters, log lines, "the
// run is over", and "where is the nearest ruin".
//
// **Why the contract is the hotload ABI's and nothing new.** A game lives in another repository
// (ADR-0047) and is rebuilt while the session runs; anything it shares with the engine has to
// survive the engine changing under it and a module being swapped under the engine. The hotload
// ABI already solves both: a module reads host data through views resolved by name (a field the
// engine renames is a refusal naming the path, not a wrong read), keeps its state in one schema
// struct the host migrates across a rebuild, and calls the engine only through `HostApi`. So a
// tick is a `GameTick` (`game_tick.schema`) bound as a view and handed to the module's
// `game_tick` export (`TickFn`), and the services are five `HostApi` functions (ABI 2). Nothing
// gives a module the document, the renderer or a file: what it may change is its own state and
// what the HUD shows.
//
// **Determinism.** Everything in a `GameTick` is a function of the session's ticks: the camera and
// the walker are the session's, the actions are the tick's input state, and the sky's clock is
// advanced a tick at a time here (by the time-lapse keys at the ticks they were pressed on, as the
// window's own clock is per frame) rather than read from a frame. So a recorded `--game` session
// replays to the same module state, offscreen or in the window. `nearest_placement` answers from
// the placements the scene generates by position and seed, never from what has streamed in.
//
// **Where the state lives, and what a tick allocates.** The module's state is one object the host
// allocated when it loaded the module (`ModuleContext::state`), migrated on a reload; the tick
// view, the HUD's lines and meters and the action table are fixed arrays in this object. A tick
// writes the view, calls the module, and copies HUD text into those arrays: nothing is allocated
// on a tick, by the host or (the module ABI's rule) by the module.

#include "fly_camera.h"
#include "game_props.h"

#include <core/base/types.h>
#include <core/json/json_value.h>
#include <core/math/world.h>
#include <foundation/hotload/module_abi.h>
#include <foundation/input/input.h>

#include <memory>
#include <string>
#include <string_view>

namespace engine::view {

// The game actions a module may declare (map revision 5, fly_camera.h): the only names
// `HostApi::game_action` accepts.
inline constexpr const char* k_game_actions[] = {"interact", "use",    "game_1",
                                                 "game_2",   "game_3", "game_4"};
inline constexpr u32 k_game_action_count = 6;

// The HUD a module asked for: up to `k_lines` text lines and `k_meters` meters said on its last
// tick (`hud_text`, `hud_meter`: the top-left stack, said again every tick), and up to
// `k_elements` **structured elements** (`hud_element`, ABI 4: anchored, styled, retained until
// removed). Each is a fixed buffer, so collecting it allocates nothing. The lines and meters are
// elements too, of the host's reserved ids (`hotload::k_hud_line_id + n`, `k_hud_meter_id + m`),
// and are laid out as the one panel they always were, so a module that calls only them draws the
// picture it drew (apps.md, "The HUD, provisional").
struct GameHud {
  static constexpr u32 k_lines = 16;
  static constexpr u32 k_line_chars = 96;
  static constexpr u32 k_meters = 8;
  static constexpr u32 k_name_chars = 32;
  static constexpr u32 k_elements = 64;
  static constexpr u32 k_element_chars = 128;
  struct Line {
    bool set = false;
    char text[k_line_chars] = {};
  };
  struct Meter {
    char name[k_name_chars] = {};
    f64 value = 0.0;
    f64 max = 0.0;
  };
  // A `hotload::HudElement` as the host keeps it: the text copied, the rest by value.
  struct Element {
    u32 id = 0;
    hotload::HudAnchor anchor = hotload::HudAnchor::Centre;
    hotload::HudAlign align = hotload::HudAlign::Centre;
    f32 offset_px[2] = {0.0f, 0.0f};
    f32 size_px = 18.0f;
    f32 outline_px = 0.0f;
    u8 color[4] = {255, 255, 255, 255};
    u8 outline_color[4] = {0, 0, 0, 255};
    hotload::HudPanel panel = hotload::HudPanel::None;
    u8 panel_color[4] = {0, 0, 0, 115};
    f32 panel_padding_px = 6.0f;
    bool visible = true;
    u64 tick = 0;  // the tick that last said it
    char text[k_element_chars] = {};
  };
  Line lines[k_lines];
  Meter meters[k_meters];
  u32 meter_count = 0;
  Element elements[k_elements];  // in the order they were first placed, which is drawing order
  u32 element_count = 0;
  u64 tick = 0;  // the tick that said the lines and meters

  // The lines and meters, at the start of every tick; the elements stay.
  void clear() noexcept;
  // Everything, the elements too (a new run).
  void clear_all() noexcept;
  // Whether there is nothing to draw: no line, no meter and no visible element.
  bool empty() const noexcept;
  // Whether the top-left stack — the lines and meters — is empty.
  bool stack_empty() const noexcept;
  // Places or replaces an element by its id (the runner has judged it), or removes one.
  hotload::Result set_element(const hotload::HudElement& element, u64 tick) noexcept;
  hotload::Result remove_element(u32 id) noexcept;
  const Element* find_element(u32 id) const noexcept;
  JsonValue to_json() const;
};

// The element's anchor and alignment by name, as the report writes them ("top_left", "centre").
const char* hud_anchor_name(hotload::HudAnchor anchor) noexcept;
const char* hud_align_name(hotload::HudAlign align) noexcept;

// One tick, as the runner reads it from the session (or a test makes it).
struct GameTickInput {
  u64 tick = 0;
  WorldPos eye{};
  WorldPos feet{};
  f32 heading = 0.0f;
  f32 pitch = 0.0f;
  bool walking = false;
  bool on_ground = false;
  bool sprinting = false;
  f64 moved_m = 0.0;
  const input::InputState* input = nullptr;  // null: no action is pressed
  // Sun-rate key presses on this tick (fly_camera.h `ViewControl`): the game's clock steps the
  // rate as the window's does.
  u32 sun_slower = 0;
  u32 sun_faster = 0;
};

// Where the nearest placement of a kind is: answered by whoever has the streamed world
// (world_view.h); false when there is none within `radius` of `from`, or no world.
using PlacementQuery = bool (*)(void* user, std::string_view kind, const WorldPos& from, f64 radius,
                                WorldPos& out, f64& distance);
// The query's cost so far, for the summary: tiles generated from the scene for queries, and their
// wall time (world_view.h `query_stats`).
using PlacementStats = void (*)(void* user, u64& generated_tiles, f64& generate_ms);
// **The cap on a query's radius** (apps.md "The game contract"; roadmap P44): a query past it is
// `InvalidArgument` — a `Result` a module already handles — because the answer is a function of the
// scene (world.md "The consumers"): every tile the search reaches that the cache does not hold is
// generated, which at this cap over 32 m tiles is up to 33² tiles cold when nothing of the kind
// is near. The fixture looks 400 m for its HUD line.
inline constexpr f64 k_max_placement_query_m = 512.0;

class GameRunner {
 public:
  GameRunner();
  ~GameRunner();
  GameRunner(const GameRunner&) = delete;
  GameRunner& operator=(const GameRunner&) = delete;

  struct Config {
    std::string module;      // the library the host loads and watches
    std::string state_file;  // --game-state: restored at start when present, saved at finish
    // Where the per-load copies go; empty: beside the module ("<module>.hotload").
    std::string shadow_directory;
  };
  // Loads the module through `hotload::ModuleHost` (refusals in the host's own words), resolves
  // its `game_tick`, and restores the state file when there is one. `map` is the session's, which
  // the declared actions are looked up in; an action the map lacks (a log recorded before revision
  // 5) is accepted and never pressed. False with `error`.
  bool start(const Config& config, const input::ActionMap& map, u32 tick_hz, std::string* error);
  bool started() const noexcept;
  // The sky's clock at the session's start: `FrameDesc::sun_time_s`'s offset, the day's rate in
  // game seconds a second, and the ground's surface time the sky stands on.
  void set_clock(f64 sun_offset_s, f64 sun_rate, f64 ground_time_s) noexcept;
  // **The clock as the frame draws it** (renderer.md "One clock"; roadmap P44): a `--game`
  // session's frames take `FrameDesc::sun_time_s` from here — the offset plus the day's game
  // seconds so far, stepped a tick at a time by the ticks that ran, at the rate the keys had set by
  // each — in the window, in an offscreen replay and in a `--capture` alike, so a replay of a
  // recording in which
  // `]` was pressed draws the sky where the live session drew it. The clock runs on after
  // `end_run`: the result panel's sky keeps turning while the module is no longer ticked.
  f64 sun_time_s() const noexcept;
  f64 day_s() const noexcept;         // the day's game seconds so far
  f64 sun_rate() const noexcept;      // the rate now
  u32 rate_changes() const noexcept;  // ladder steps taken
  void set_placements(PlacementQuery query, void* user, PlacementStats stats = nullptr) noexcept;
  // **The props the module places** (game_props.h): declare the scene's before `start`, since the
  // module's `on_load` names them, and `attach` the loaded scene after it. The host hands the live
  // props to the renderer's tail (main.cpp) and this holds the table.
  GameProps& props() noexcept;
  const GameProps& props() const noexcept;
  // The loaded module's name and build (the host's generation), for a recording's header.
  const std::string& module_name() const noexcept;
  u32 build() const noexcept;
  // **The session the walker's services act on** (`walker_set`, `walker_teleport`;
  // fly_camera.h, "A game's requests at a tick"): set by whoever attaches the runner to a session.
  // Without one the two answer `Unsupported`.
  void set_session(FlySession* session) noexcept;

  // **The session's tick observer** (`FlySession::set_tick_observer`): one `tick` a session tick.
  static void observe(void* user, const FlySession& session, const input::InputState& input);
  void tick(const GameTickInput& in);

  // Between frames: stats the module's file and swaps a rebuild in with its state carried. 1 when
  // it did; a refused rebuild keeps the old build running and is said once on stderr and counted.
  u32 poll();

  // Whether the module ended the run (`end_run`) or failed (`fail` from an export, or a result
  // that is not a JSON object). A replay, an injected session and an offscreen run end after this
  // frame; a live window stays on the result panel (apps.md "--game", "Ending").
  bool ended() const noexcept;
  bool failed() const noexcept;
  const std::string& error() const noexcept;
  // `end_run`'s object (null until it is called), and the runs this session has started.
  const JsonValue& result() const noexcept;
  u32 runs() const noexcept;
  // **A new run** (the result panel's R; roadmap P44): after `end_run`, the module's state object
  // is put back at its descriptor's defaults through the host (`ModuleHost::reset_state`), the HUD
  // cleared, the run counted and the ended run's result kept in the summary's `results`. Nothing
  // else moves: the walker stands where it stands, the clock runs on, the module is not reloaded.
  // False when the run has not ended, or ended by a failure.
  bool new_run();
  const GameHud& hud() const noexcept;
  u64 ticks() const noexcept;
  // The module's state as its descriptor writes it.
  JsonValue state_json() const;

  // Saves the state file (when configured) and unloads the module. False with `error` when the
  // state could not be written.
  bool finish(std::string* error);
  // The summary's and the report's `game` block.
  JsonValue summary_json() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace engine::view
