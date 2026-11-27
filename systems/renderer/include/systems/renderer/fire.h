#pragma once

// **Fires** (docs/subsystems/renderer.md, "Fires"; docs/subsystems/volumetrics.md, "Fire"; roadmap
// R85): a scene's `fires` and a prop's `fire` (`engine.scene.Fire`), as the renderer keeps them and
// as a frame burns them. A fire is two things, drawn by two owners:
//
// - **a light**, which is the renderer's: a point light at the flame's centre in the frame's light
//   array (`add_fire_lights`), its intensity and colour flickering by a function of the world's one
//   clock and the fire's seed — no history, so the same clock is the same light — shadowed as the
//   array's point lights are (a ray under `--shadows rt`, nothing under the cascaded maps, which
//   shadow only the key light). It is in the frame whether or not the scene has a medium.
// - **a flame and its smoke**, which are the medium's (systems/volumetrics): the frame's fires are
//   handed to the frame passes (`FramePassContext::fires`) as `FrameFire`s, and nothing here draws
//   them. A scene with fires and no `medium` is lit by them and shows no flame.
//
// A placed prop's fire comes and goes with the prop: the renderer finds the instances of a mesh
// whose prop declares a fire in the scene's dynamic tail each time the tail changes
// (`FireSet::place`), so a game lights a fire by placing the prop and needs no call of its own.
//
// **A lamp** (`engine.scene.Lamp`, roadmap R86; renderer.md, "Lamps") is a fire's light and nothing
// else: a point light of a colour, an intensity and a reach, with no flame, no smoke and no
// flicker, read into a `FireDesc` whose `lamp` is set, kept and placed by the same `FireSet` (a
// scene's `lamps`, a prop's `lamp`), burnt by `burn_lamp` at its own steady light into a list of
// its own that the medium is never handed, and lit by `add_lamp_lights`: every lamp of the frame,
// up to `k_max_lamps`, behind the fires. Its globe is the mesh's emissive material, not the
// renderer's.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <systems/renderer/tile_layout.h>

#include <span>
#include <string>

namespace engine::scene {
struct Fire;
struct Lamp;
}  // namespace engine::scene

namespace engine::renderer {

struct FrameLighting;
struct SceneData;
struct SceneInstance;

// One fire as the scene file gave it (`engine.scene.Fire`, read by `read_fire`).
struct FireDesc {
  std::string name;
  // The flame's base, at its centre: a world position on the scene's `fires`, an offset from the
  // prop's origin (its x, y, z in metres) on a prop's `fire`.
  WorldPos position{};
  f32 radius_m = 0.35f;
  f32 height_m = 0.9f;
  f32 intensity = 250.0f;  // candela with a sky; the stand-in's units without one
  f32 color_k = 1800.0f;
  f32 smoke_density = 0.5f;
  f32 smoke_rise_m_s = 1.2f;
  f32 smoke_spread = 0.25f;
  f32 flicker = 0.5f;
  u32 seed = 1;
  // **A lamp** (`read_lamp`): no flame, no smoke, no flicker; its light is `color` (linear Rec. 709
  // of luminance 1) at `intensity`, ending at `reach_m` (0: `fire_light_reach` of the intensity),
  // and `radius_m` is its globe's, where a traced shadow ray stops short of the light.
  bool lamp = false;
  Vec3 color{1.0f, 1.0f, 1.0f};
  f32 reach_m = 0.0f;
  // **A spot lamp** (R239): `direction` the cone's axis at unit length — in the world on a scene's
  // lamp, in the prop's frame on a prop's — or zero for a point lamp; `cos_inner` and `cos_outer`
  // the cosines of the cone's half-angles, as `gfx::ResolveLight::spot.x` and `.y` carry them.
  Vec3 direction{};
  f32 cos_inner = 1.0f;
  f32 cos_outer = 0.70710678f;
};

// The block's numbers checked: false with a sentence naming the field for a size, an intensity, a
// temperature or a share out of range, or a position no cell can hold.
bool read_fire(const scene::Fire& in, FireDesc& out, std::string* error);
// A lamp's block checked the same way (a colour channel outside [0, 1000], an intensity outside
// [0, 1e7], a reach outside [0, 400], a direction that is not three finite numbers or is too short
// to have one, a cone's outer half-angle outside (0, 180) or an inner one outside [0, outer]) into
// a `FireDesc` with `lamp` set: its colour taken to a luminance of 1 (all zero: white), its
// direction to unit length (all zero: a point), its flicker and smoke 0.
bool read_lamp(const scene::Lamp& in, FireDesc& out, std::string* error);

// **One fire of one frame**: where it stands in the world and how its light burns at the frame's
// clock. The medium reads it (frame_pass.h); the light array is filled from it.
struct FrameFire {
  WorldPos base{};  // the flame's base, at its centre
  f32 radius_m = 0.35f;
  f32 height_m = 0.9f;
  // The light at this clock: its intensity (the block's times the flicker), its colour (linear
  // Rec. 709 of luminance 1: the blackbody of the flickered temperature), and its reach — the
  // radius the windowed inverse square falls to nothing at.
  f32 intensity = 0.0f;
  Vec3 color{1.0f, 1.0f, 1.0f};
  f32 reach_m = 0.0f;
  // The flame's emission colour (the block's temperature, unflickered: the light flickers, the
  // flame's own noise moves) and the block's numbers the medium shapes the flame and smoke from.
  Vec3 flame_color{1.0f, 1.0f, 1.0f};
  f32 base_intensity = 0.0f;
  f32 smoke_density = 0.0f;
  f32 smoke_rise_m_s = 1.2f;
  f32 smoke_spread = 0.25f;
  f32 flicker = 0.0f;
  u32 seed = 1;
  // A lamp's cone (R239; `FireDesc::direction`): its axis in the world at unit length, turned with
  // a placed prop, or zero for a point; the half-angles' cosines. A fire's is always zero.
  Vec3 direction{};
  f32 cos_inner = 1.0f;
  f32 cos_outer = 0.70710678f;
};

// A frame's fires, the scene's and its placed props' together, and the light array's room for
// them: **every one is lit** (`add_fire_lights`, R87), in their order. Until R87 the array took the
// four nearest the eye, which put a fire's light out and lit another as the eye walked past them.
inline constexpr u32 k_max_fires = 64;
// **A frame's lamps**, the scene's and its placed props' together, and the light array's room for
// them: every one is lit (`add_lamp_lights`). A scene file naming more is refused with the count;
// a tail placing more keeps the first `k_max_lamps` in its order and counts the rest
// (`FireSet::lamps_refused`, `Stats::lamps_refused`), never dropping one unsaid. A pixel walks only
// the lights whose reach covers it (gfx/light_grid.h, R87), so the room is memory — 64 bytes a
// light in each frame slot's array, and a word a cell for each 32 lights in its grid — and not a
// cost at every pixel; it was 64 before R87 and is 256 since.
inline constexpr u32 k_max_lamps = 256;
// Where the light stands: this share of the flame's height above its base.
inline constexpr f32 k_fire_light_height = 0.45f;
// The light's reach: where its inverse square has fallen to this illuminance (lux with a sky), so a
// 250 cd campfire reaches 71 m — every pixel past it would be lit by under a twentieth of a lux.
inline constexpr f32 k_fire_reach_illuminance = 0.05f;
inline constexpr f32 k_fire_max_reach_m = 400.0f;
// The flicker: a sum of four sines at these frequencies (Hz) and weights, the phases the seed's;
// at `flicker` 1 the intensity swings by `k_fire_flicker_intensity` either side and the
// temperature by `k_fire_flicker_kelvin` of itself.
inline constexpr f32 k_fire_flicker_hz[4] = {0.71f, 1.93f, 4.27f, 9.61f};
inline constexpr f32 k_fire_flicker_weight[4] = {0.45f, 0.30f, 0.17f, 0.08f};
inline constexpr f32 k_fire_flicker_intensity = 0.35f;
inline constexpr f32 k_fire_flicker_kelvin = 0.04f;

// A blackbody's colour at `kelvin` (1,000 to 25,000; clamped), linear Rec. 709 of luminance 1: the
// Planckian locus's chromaticity (Kim et al.'s cubic fit) at Y = 1, negative channels clipped and
// the luminance put back.
Vec3 blackbody_rgb(f32 kelvin) noexcept;
// The flicker's signal at a time, in [-1, 1]: four sines whose phases are the seed's, each reduced
// to its cycle in f64 before the sine, so a clock years in is as smooth as one at zero.
f32 fire_flicker_signal(u32 seed, f64 time_s) noexcept;
// The light's reach for an intensity: sqrt(I / k_fire_reach_illuminance), at most
// `k_fire_max_reach_m`.
f32 fire_light_reach(f32 intensity) noexcept;
// One fire at a clock: standing at `base`, its sizes and intensity multiplied by `scale` (a prop's;
// intensity by its square, since a fire twice the size has four times the flame).
// `units` is the frame's light per candela: 1 / `gfx::k_sky_sun_lux` with a sky (whose light is in
// sun units, 128,000 lux each), 1 for the stand-in; the reach is the block's, worked out in
// candela.
FrameFire burn_fire(const FireDesc& desc, const WorldPos& base, f32 scale, f64 time_s,
                    f32 units = 1.0f) noexcept;
// One lamp (`FireDesc::lamp`): steady at any clock, its colour and intensity the block's (the
// intensity by the square of `scale`), its reach its own or `fire_light_reach`, its `radius_m` the
// globe's; no flame and no smoke. Its own function, so a fire's light is computed by the code it
// was.
FrameFire burn_lamp(const FireDesc& desc, const WorldPos& base, f32 scale,
                    f32 units = 1.0f) noexcept;

// **The light array's fires**: every one of `fires` that burns (at most `k_max_fires`), in their
// order — never by distance from the eye, so neither the set nor its order changes as the eye
// moves — as point lights appended to `out` after the lights it has, in the frame's space
// (measured from `eye` in f64), while the array has room. Returns how many it added.
u32 add_fire_lights(std::span<const FrameFire> fires, const WorldPos& eye,
                    FrameLighting& out) noexcept;
// **The light array's lamps**: every one of `lamps` (burnt by `burn_lamp`, at most `k_max_lamps`)
// in their order, as lights at their centres appended after the lights `out` has, while the array
// has room: a point (`gfx::k_light_point`), or with a `direction` a spot (`gfx::k_light_spot`, the
// axis in `direction_type.xyz`, the cone's cosines in `spot.x` and `.y`); either way the globe's
// radius in `spot.z`. Returns how many it added.
u32 add_lamp_lights(std::span<const FrameFire> lamps, const WorldPos& eye,
                    FrameLighting& out) noexcept;

// **The fires a scene's renderer keeps** (scene_fire.cpp): the scene's own, and those of the props
// placed in its dynamic tail, found again each time the tail changes. Reserved once, so a frame
// and a change allocate nothing.
class FireSet {
 public:
  // Takes the scene's `fires` and the props' fires by mesh.
  void reset(const SceneData& scene);
  // The tail's instances of meshes whose prop declares a fire, each at its placement: its origin
  // and position, the fire's offset turned by its rotation and scaled by its largest scale. At most
  // `k_max_fires` with the scene's, in the tail's order.
  void place(std::span<const SceneInstance> tail) noexcept;
  // The same over a tail handed over as blocks, each its own run or the tail's (W81).
  void place(std::span<const SceneInstance> tail, std::span<const DynamicBlock> blocks) noexcept;
  // This frame's fires at the world's clock, in the frame's light units (`burn_fire`), into `out`
  // (cleared first).
  void burn(f64 time_s, f32 units, Vector<FrameFire>& out) const;
  // This frame's lamps, the same way, into `out` (cleared first): the scene's, then the placed.
  void burn_lamps(f32 units, Vector<FrameFire>& out) const;
  u32 count() const noexcept { return static_cast<u32>(fixed_.size() + placed_.size()); }
  bool empty() const noexcept { return fixed_.empty() && placed_.empty(); }
  u32 lamp_count() const noexcept {
    return static_cast<u32>(fixed_lamps_.size() + placed_lamps_.size());
  }
  // The placed props' lamps the last `place` found no room for, past `k_max_lamps`.
  u32 lamps_refused() const noexcept { return lamps_refused_; }

 private:
  struct Placed {
    u32 desc = 0;  // into `prop_fires_`
    WorldPos base{};
    f32 scale = 1.0f;
    Vec3 direction{};  // a lamp's cone axis turned with the instance (R239); zero: a point
  };
  Vector<FireDesc> fixed_;       // the scene's own, at their world positions
  Vector<FireDesc> prop_fires_;  // one per prop that declares one
  Vector<u32> mesh_fire_;        // per scene mesh: its prop's fire + 1, or 0
  Vector<Placed> placed_;
  Vector<FireDesc> fixed_lamps_;  // the scene's own lamps
  Vector<FireDesc> prop_lamps_;   // one per prop that declares one
  Vector<u32> mesh_lamp_;         // per scene mesh: its prop's lamp + 1, or 0
  Vector<Placed> placed_lamps_;
  u32 lamps_refused_ = 0;
  void add(std::span<const SceneInstance> instances) noexcept;
  static void place_at(const SceneInstance& instance, const FireDesc& desc, u32 index,
                       Vector<Placed>& out) noexcept;
};

}  // namespace engine::renderer
