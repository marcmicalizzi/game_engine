#pragma once

// world_sound capability (ADR-0027; ADR-0079, proposed; docs/subsystems/world_sound.md, and the
// design in docs/subsystems/audio.md, "Sound for the world: the design (S110)").
//
// **The sound stage a host creates for a game** (roadmap S111, the design's stage 1): one object
// that owns the clip store, the mixer, the output (a device or the null backend), the listener, the
// clips a scene's `sound` block declares and the voices a game module starts. A host has one object
// to create, hand a listener and tick; sound code lives here, not in an app, as rendering code
// lives in `systems/renderer`. Later stages add the world's synthesised sources to the same stage:
// S112 the wind in the trees and at the ears, S114 their path, S113 running water and the surf.
//
// **What a module sees does not depend on whether anything is heard** (ADR-0079 decision 5):
// `play` answers with a handle the stage counts itself, never the mixer's slot, whether or not a
// voice sounded; `stop` answers for any handle issued; there is no question a caller can ask about
// a voice, a level or a device. A stage opened `Off` keeps the declared names and the count and
// makes no mixer, no thread and no allocation after `open`, and answers exactly the same.
//
//   [x] schema types      `engine.scene.Sound` in schemas/scene.schema: the block is the scene's,
//                         so a build without this capability reads the same scene
//   [ ] scheduler entry   none: the host ticks the stage at its session's tick (engine-view)
//   [ ] render passes     none: sound draws nothing
//   [ ] derived data      none here: a clip's `.clip` is domain/audio's content-build step
//   [ ] protocol methods  none yet
//   [x] tunables          none of its own at S111; the mixer's `audio.*` size it
//   [x] LOD policy        the pool is the budget (audio.md "The voice policy"); S112's beds next
//   [x] determinism       k_determinism below
//   [x] zero cost unused  `Off`: no store, no mixer, no thread (a world on builds its mixer)
//   [x] docs, tests, size table
//   [x] bench             bench/world_sound_bench.cpp
//
// The thread model is the mixer's: every call here is the controlling thread's (the host's tick);
// a device's thread pulls the mixer on its own.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/wind.h>

#include <memory>
#include <string>
#include <string_view>

namespace engine::scene {
struct Sound;
struct SoundRecipe;
}  // namespace engine::scene

namespace engine::world_sound {

/// Determinism stance (ADR-0010): **derived**. Sound is presentation: nothing it computes is read
/// back into the simulation, hashed, saved or replayed; the null render of a scripted session is
/// still the same bytes on every run and toolchain (audio.md "Determinism").
inline constexpr const char* k_determinism = "derived";

/// How the stage is heard (engine-view `--audio`).
enum class AudioMode : u8 {
  Off = 0,     // mute: names and handles only, no mixer
  Null = 1,    // the null backend, pulled by the stage a tick at a time (tests, agents)
  Device = 2,  // a playback device (named, or the default); falls back to the null backend
};
const char* audio_mode_name(AudioMode mode) noexcept;

/// What a call answers. The host maps it onto its own interface's results (the hot-load ABI's).
enum class Answer : u8 { Ok = 0, NotFound, Unsupported, InvalidArgument };

struct StageConfig {
  AudioMode mode = AudioMode::Off;
  std::string device;    // Device: a device by name as the platform spells it; empty the default
  std::string wav_out;   // Null: the render kept as a 32-bit float WAV; empty keeps nothing
  std::string base_dir;  // where a clip's `path` is relative to: the scene file's directory
  // The host's ticks a second: a Null stage renders 48,000 / tick_hz frames a tick, the remainder
  // carried exactly so a second of ticks is a second of frames.
  u32 tick_hz = 240;
  // The decoded clips' budget (audio.md "Clips"). A game's default is 64 MB: launch memory is a
  // budget (S110's table).
  u64 clip_budget_bytes = 64ull << 20;
  // The mixer's pool; 0 reads `audio.voices`.
  u32 voices = 0;
  // **The fault seam** (tests): the output refused at `open`, or its write failing at this tick
  // (0: never), so the two failures a device has are proven on the null backend. Either leaves
  // the stage silent with the reason logged once and in the summary, and every call answered as
  // before; a device that is lost and cannot be opened again does the same.
  bool fault_refuse_open = false;
  u64 fault_fail_write_at_tick = 0;
  // **The world's sound** (S112): the wind in the crowns and at the ears, from the canopy table and
  // the frame's wind the host hands the stage. Unless `Off`, a stage with the world on builds its
  // mixer whether or not the scene has a `sound` block.
  bool world = false;
  // The wind's turbulence over the frame's mean at each source's place (`gfx::wind_gust`). Off
  // only in the cases that hold a law to a fixed wind: the gust's sines would move the level.
  bool turbulence = true;
};

/// What a crown is, for its sound (audio.md, "The shaped-noise voice"; world-sound.md): leaves
/// struck together (`Blade`, a broadleaf's rustle, and what a mesh that says nothing is), needles
/// shedding vortices (`Needle`: a band of Aeolian tones at 0.2 U / d over a rustle), or twigs alone
/// (`Bare`, low, under 1 kHz).
enum class CanopyClass : u8 { Blade = 0, Needle = 1, Bare = 2 };

/// One crown, or a far tile's crowns merged into one at their centroid: where its foliage stands
/// (the foot of the tree), how high its foliage's middle is above it, its one-sided leaf area and
/// its class. A host measures the area from the mesh's foliage parts and declares the class.
/// **A crown has a size** (S112-2): `extent_m`, the horizontal distance from `at` to its farthest
/// leaf, so a listener inside a stand (a whole wood flattened into one glTF mesh, or a far group
/// merged) hears it as all round, at its level at its edge, and not as a point source at its middle
/// (`k_crown_min_m` is the least). `merged`: a group's crowns summed, sent to the beds only, never
/// chosen as a voice of its own.
struct Crown {
  WorldPos at{};
  f32 height_m = 0.0f;  // the foliage's middle above `at`
  f32 leaf_area_m2 = 0.0f;
  CanopyClass kind = CanopyClass::Blade;
  f32 needle_mm = 1.2f;  // Needle: the needles' width
  f32 extent_m = 0.0f;
  bool merged = false;
};

/// Where a canopy table's visit sends each crown.
struct CrownSink {
  void* user = nullptr;
  void (*emit)(void* user, const Crown& crown) noexcept = nullptr;
};

/// **The host's canopy table** (audio.md, "The capability"): every crown within `reach_m` of
/// `centre`, from the scene's instances and the active placement tiles, sent to `sink`. A function
/// pointer and a context, as scene generators are tables. Called at `audio.world.refresh_hz`.
struct CanopySource {
  void* user = nullptr;
  void (*visit)(void* user, const WorldPos& centre, f64 reach_m, const CrownSink& sink) = nullptr;
};

/// **The frame's wind as every reader has it** (S112): the scene's (or none), W151's live state
/// where a session set one (the mean, the direction and the forced gust), and the sky's clock the
/// turbulence is read at. The stage takes the arguments the trees' sway takes
/// (`renderer::scene_flora_wind`) and asks the gust at each source's own place.
/// **What shelters the listener's ears** (S114; world_sound.md, "Distance"): a host's table of what
/// stands round the listener, a function pointer and a context as the canopy is. `height_at` is
/// the height above the listener's ground of whatever stands across the wind at (x, z) — a wall's
/// top, a roof, a rock — and 0 where the air is open; `across_x`, `across_z` is the unit direction
/// across the wind there, so a host can tell a wall, which stands along it, from a trunk, which
/// does not. The stage asks it at a few places upwind of the listener at each refresh and lowers
/// the wind at the ears as a windbreak lowers the wind in its lee.
struct ShelterSource {
  void* user = nullptr;
  f32 (*height_at)(void* user, f64 x, f64 z, f32 across_x, f32 across_z) noexcept = nullptr;
};

struct WorldWind {
  const gfx::WindDesc* scene = nullptr;
  gfx::WindLive live{};
  f64 sky_time_s = 0.0;
  // **The water's clock** (S113): the world clock (`FrameDesc::world_time_s`), which the waves the
  // picture draws move on, so a wave heard breaking is the wave seen breaking.
  f64 world_time_s = 0.0;
};

/// **Running water** (S113; world_sound.md, "Running water and the surf"): one stretch of a
/// solved channel between two neighbouring stations, as a host's flow table sends it — the two
/// stations at the water's surface (y the solved level), what the stretch carries, the level it
/// gives up, how wide its water is and its regime, and for a fall (a lip and its foot) whether its
/// foot lands in water (a pool, a body) or on the bed. The sound is the power the water gives up,
/// rho g Q h, a share of it heard in the air.
struct Stretch {
  WorldPos a{};
  WorldPos b{};
  f32 discharge_m3s = 0.0f;
  f32 drop_m = 0.0f;  // the level given up from `a` to `b`, metres (a fall's height)
  f32 wet_width_m = 0.0f;
  f32 froude = 0.0f;
  bool fall = false;
  bool lands_on_water = false;
};
struct StretchSink {
  void* user = nullptr;
  void (*emit)(void* user, const Stretch& stretch) noexcept = nullptr;
  // A stream (not a fall) whose middle is farther than this, less its half length, is not taken
  // (`audio.world.water_reach_m`), so a host need not visit past it; 0: `reach_m`.
  f64 stream_reach_m = 0.0;
};
/// **The host's flow table**: every fall within `reach_m` of `centre` and every stream within the
/// sink's `stream_reach_m`, from the solved channel description. Called at
/// `audio.world.refresh_hz`, as the canopy table is; a host visits only what is in reach (S113).
struct FlowSource {
  void* user = nullptr;
  void (*visit)(void* user, const WorldPos& centre, f64 reach_m, const StretchSink& sink) = nullptr;
};

/// **The surf** (S113): one stretch of a body's shore where its train breaks, as a host's surf
/// table sends it — the break's place at the water's level and the shore's direction there, the
/// length of shore it stands for, the wave power breaking gives up a metre of shore, the height
/// the wave breaks at and the surf similarity there, and the train's period and arrival at the
/// break, so its crest stands there at the water's clock `arrival_s + n period_s`.
struct Surf {
  WorldPos at{};
  Vec2 along{1.0f, 0.0f};  // unit, along the shore
  f32 length_m = 0.0f;
  f32 power_w_m = 0.0f;
  f32 height_m = 0.0f;
  f32 surf_similarity = 0.0f;
  f32 period_s = 0.0f;
  f32 arrival_s = 0.0f;
};
/// The wave power a stretch of shore takes in, watts a metre: a sea's deep-water energy flux,
/// rho g^2 H^2 T / (64 pi) for significant height H and peak period T, which shoaling keeps to the
/// break and breaking gives up (S113). A host's surf table scales it by the refraction's share.
f64 surf_power_w_m(f64 height_m, f64 period_s) noexcept;
/// **The airborne efficiency** (S113; docs/experiments/world-sound.md, "Read again by S113"): the
/// share of the power running water and breaking waves give up that is heard in the air. No
/// source read gives it; it is **a stated choice**, anchored on Bolin and Abom's 60 dB over surf
/// of 0.4 m (a Baltic sea of 3 s, read 20 m from the break line: the distance is a choice).
inline constexpr f64 k_water_airborne_efficiency = 1.75e-7;
struct SurfSink {
  void* user = nullptr;
  void (*emit)(void* user, const Surf& surf) noexcept = nullptr;
};
struct SurfSource {
  void* user = nullptr;
  void (*visit)(void* user, const WorldPos& centre, f64 reach_m, const SurfSink& sink) = nullptr;
};

/// `audio.world.near_m` (96): a canopy table walks its crowns one by one within this of the
/// listener and sends farther ones merged, one crown a class a group (the capability's LOD).
f64 world_near_m() noexcept;
/// **The canopy table's allowance a refresh** (S112-2; world_sound.md, "What a refresh costs"):
/// `audio.world.near_crowns` (1,024), the crowns a table sends one by one, nearest group first,
/// before it sends the rest of its near groups merged; and `audio.world.walk_instances` (4,096),
/// the placements it walks to make a changed tile's crowns again, the rest carried to the next.
u32 world_near_crowns() noexcept;
u32 world_walk_instances() noexcept;

/// What one world voice was last told: its kind, its level at the listener in dB SPL over its
/// bands, each band's level, the movable band's centre (0: none) and the wind it was made from.
enum class WorldVoiceKind : u8 { None = 0, Crown, Bed, EarLeft, EarRight, Stream, Fall, Surf };
struct WorldVoiceInfo {
  WorldVoiceKind kind = WorldVoiceKind::None;
  bool sounding = false;
  f32 level_db = -1000.0f;
  f32 band_db[9] = {};
  f32 tone_hz = 0.0f;
  f32 wind_m_s = 0.0f;
};

/// What the world's sound did, counted.
struct WorldStats {
  u64 refreshes = 0;       // the canopy table walked
  u64 crowns_seen = 0;     // crowns the last refresh was sent
  u64 crowns_dropped = 0;  // crowns past `audio.world.max_crowns` in the last refresh
  u64 commands = 0;        // world voices' plays, sets and stops sent
  u64 plays = 0;
  u64 stops = 0;
  u32 crown_voices = 0;  // sounding now
  u32 bed_voices = 0;
  u32 ear_voices = 0;
  // **The world's ceiling** (S112-2; world_sound.md, "The world's hold"): the dB the world's
  // voices were held under their laws at the last tick (0: not acting), and the ticks it acted on.
  f32 held_db = 0.0f;
  u64 held_ticks = 0;
  u64 place_retries = 0;  // a voice placed again after the ring refused its move
  // **Shelter** (S114): the share of the open wind the ears' wind had at the last refresh, 1 in the
  // open (or with no shelter table), from what stands upwind (`ShelterSource`).
  f32 shelter = 1.0f;
  // **Running water and the surf** (S113): what the last refresh was sent and kept, the voices
  // sounding now, and the bubbles struck.
  u64 stretches_seen = 0;
  u64 surf_seen = 0;
  u64 water_dropped = 0;  // past `audio.world.max_water` in the last refresh
  u32 stream_voices = 0;
  u32 fall_voices = 0;
  u32 surf_voices = 0;
  u64 bubbles = 0;
};

/// What the stage did, counted where it did it.
struct StageStats {
  u64 ticks = 0;
  u64 plays = 0;           // `play` answered Ok: handles issued
  u64 stops = 0;           // `stop` answered Ok
  u64 refused = 0;         // `play`/`stop` answered InvalidArgument
  u64 voices_started = 0;  // plays the mixer took (0 with the stage Off)
  u64 voices_refused = 0;  // plays the mixer refused (a full pool of louder voices)
  u64 listener_moves = 0;  // listener commands sent
  u64 frame_moves = 0;     // the mixer's frame moved to another 64 m cell
  u64 source_resends = 0;  // voices sent again in a moved frame
  u64 frames = 0;          // frames the stage pulled from the null backend
  u64 wav_frames = 0;      // of them, written to `wav_out`
};

/// A recipe's samples, mono at the mix rate: integer phase, `audio::sin_quarter`, an integer noise
/// generator and a gain from dB by a fixed series, so the same recipe is the same bytes everywhere.
/// False with `error` for a value out of its range (scene.schema's `SoundRecipe`).
bool render_recipe(const scene::SoundRecipe& recipe, Vector<f32>& out, std::string* error);

/// 10^(db / 20) without the C library: an exact power of two times a fixed series (|error| under
/// 1e-15 relative), so a clip's gain is the same float on every toolchain.
f64 gain_from_db(f64 db) noexcept;

/// A scene file's `sound` block, read straight from its text (as `renderer::read_scene_props`).
/// `present` false and true returned for a scene with none. False with `error` for a file that
/// does not read or a block that is not valid (an empty or repeated name, neither or both of
/// `path` and `recipe`, a value out of range).
bool read_scene_sound(const std::string& scene_path, scene::Sound& out, bool& present,
                      std::string* error);

class SoundStage {
 public:
  SoundStage();
  ~SoundStage();
  SoundStage(const SoundStage&) = delete;
  SoundStage& operator=(const SoundStage&) = delete;

  /// Takes the block's clips by name and, unless `Off`, builds the store, decodes or generates
  /// every clip, builds the mixer and opens the output. `block` null: a scene with no `sound`
  /// block, and every call answers `Unsupported`. False with `error` for a clip that does not
  /// decode, a WAV that cannot be started, or a call after the first. An output that will not open
  /// is not a failure: the stage is silent and answers as before (`stopped` in the summary).
  bool open(const StageConfig& config, const scene::Sound* block, std::string* error);
  bool is_open() const noexcept;
  bool has_block() const noexcept;
  AudioMode mode() const noexcept;

  /// A declared clip's index by name: `NotFound` for a name the block does not declare,
  /// `Unsupported` with no block.
  Answer clip(std::string_view name, u32& out) const noexcept;
  /// A one-shot of clip `clip` at `at` (world metres), or on the listener (2D) with `at` null, at
  /// linear `gain` (in [0, 16]) under the clip's own. `Ok` with a handle the stage counts (1, 2,
  /// ...), whether or not a voice sounded; `InvalidArgument` for an unknown clip, a gain out of
  /// range or a position that is not finite; `Unsupported` with no block.
  Answer play(u32 clip, const WorldPos* at, f32 gain, u32& out_handle) noexcept;
  /// Fades a handle's voice out over the mixer's ramp. `Ok` for any handle issued, sounding,
  /// ended or never heard; `InvalidArgument` for 0 or one never issued.
  Answer stop(u32 handle) noexcept;

  /// The listener: where the ears are and which way they face (unit `forward` and `up`; -z ahead,
  /// +y up as core/math). The mixer is handed positions in a frame at the corner of the listener's
  /// 64 m cell (audio_system.md "The mixer's frame"), and a move inside the cell is one command.
  void set_listener(const WorldPos& at, const Vec3& forward, const Vec3& up) noexcept;

  /// The world's sound (S112): the host's canopy table, kept until replaced, and the frame's wind,
  /// handed before every `tick`. Neither does anything with the world off.
  void set_canopy(const CanopySource& canopy) noexcept;
  void set_shelter(const ShelterSource& shelter) noexcept;
  /// Running water and the surf (S113): the host's flow and surf tables, kept until replaced.
  void set_flow(const FlowSource& flow) noexcept;
  void set_surf(const SurfSource& surf) noexcept;
  void set_wind(const WorldWind& wind) noexcept;
  const WorldStats& world_stats() const noexcept;
  /// The world's voices as last described (tests: the levels the laws are held to). `count` is
  /// the crowns' (`audio.world.crowns`), then the beds' (`audio.world.beds`), then the two ears,
  /// then (S113) the streams' (`audio.world.streams`), the falls' and the surf's.
  u32 world_voice_count() const noexcept;
  WorldVoiceInfo world_voice(u32 index) const noexcept;

  /// Once a host tick, after the tick's plays: the world's voices described (when it is on), the
  /// mixer's and the output's updates, and under the null backend the tick's frames pulled and,
  /// with `wav_out`, kept. Allocates nothing.
  void tick() noexcept;

  /// Finishes the WAV (its sizes written) and closes the output. A WAV that could not be written
  /// stops the audio (said in the summary) and is not a failure of the session. Safe to call twice.
  bool close(std::string* error);

  const StageStats& stats() const noexcept;
  /// The null backend's last tick of frames, interleaved, and its channel count (tests).
  const f32* last_block() const noexcept;
  u32 last_block_frames() const noexcept;
  u32 channels() const noexcept;
  /// `hash_mix` of every frame pulled so far (0 with nothing pulled): the stage's null render.
  u64 render_hash() const noexcept;
  /// The summary's `sound` block: the mode, the backend and device, the clips, the counts.
  JsonValue summary_json() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace engine::world_sound
