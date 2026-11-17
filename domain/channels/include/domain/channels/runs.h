#pragma once

// **How whole a stream is drawn** (roadmap W130; channels.md, "How whole a stream is drawn"): the
// count, over a solved description, of the separate runs of water a person walking a line sees,
// by the line's mean slope. The forest's session took it first with a script of its own (its
// owner's walk of 2026-11-15: over 30 %, 7.4 % of the length a 5 mm film and 6.0 pieces every
// 100 m); this is the engine's count, which `engine-content flow solve` prints for any scene.
//
// Along one block, the stretch between neighbouring stations i and i + 1 is one of:
//   * **a fall**: a lip and its foot (`k_channel_fall_lip`, `k_channel_fall_foot`). The sheet has
//     no surface there; the draw lays a nappe where the foot stands lower;
//   * **not drawn**: the draw's own rule (`scene_gen::channel_stretch_drawn`) leaves it out — both
//     stations over a bank, and the water a film under 2 cm or climbing;
//   * **drawn**: everything else.
// A **piece** is a maximal run of drawn stretches that are not falls: the sheet's surface in one
// piece (the forest's "runs of water"). A **run** joins pieces across a fall the draw lays a nappe
// on: the water a person sees as one. A **film** is a stretch whose two stations are both 5 mm deep
// or less (the cook's least written depth), drawn or not.

#include <core/base/types.h>
#include <core/json/json_value.h>

#include <schemas/scene.h>

namespace engine::channels {

// The slope classes, by a line's mean slope (its level's fall over its length): under 2 %, 2 to
// 5 %, 5 to 15 %, 15 to 30 %, over 30 % (the forest's own table's).
inline constexpr u32 k_run_classes = 5;
inline constexpr f64 k_run_class_slope[k_run_classes - 1] = {0.02, 0.05, 0.15, 0.30};
// A station this deep or less is a film (the profile's least written depth is 5 mm).
inline constexpr f64 k_run_film_depth_m = 0.0055;
// **The forest's own count** (its session's `streams47b.py`, read on 2026-11-16): a station 1.1 cm
// deep or less is "film", which takes in a fall's foot (Rand's sheet, under a centimetre at 20 l/s
// below a metre's drop) and the thin shooting water below it as well as the 5 mm film; each stretch
// is the kind of the station it leaves, and its pieces are the runs of one kind, film and water
// both. `thin_m` and `alternations` are that count, by horizontal length where theirs is by the
// points' 3D distance (7 % longer on a 37 % line).
inline constexpr f64 k_run_thin_depth_m = 0.011;
// **A climb** is a stretch that is not a fall whose written level rises along the flow by more
// than `k_run_climb_m`, and **a wall** one that rises by more than `k_run_wall_m`: over the
// lattice's quarter metre either, drawn, is a face of water standing in the air (W138; the
// forest's count of 2026-11-16, `17_walls.py`, whose thresholds these are: 42 drawn walls on 14
// lines of v47 at stage 1). They are counted in the description whether drawn or not (what the
// solve wrote), and `walls_drawn` is the forest's count (what the draw lays a sheet on). A jump's
// head to its foot climbs by the sequent depth, which is physical, and is not counted.
inline constexpr f64 k_run_climb_m = 0.05;
inline constexpr f64 k_run_wall_m = 0.5;

struct LineRuns {
  f64 length_m = 0;  // along the line, horizontal
  f64 fall_m = 0;    // the first station's level less the last's
  f64 slope() const noexcept { return length_m > 0 ? fall_m / length_m : 0.0; }
  f64 film_m = 0;        // stretches whose two stations are films
  f64 undrawn_m = 0;     // stretches the draw leaves out
  u32 pieces = 0;        // maximal runs of drawn surface, cut at every fall
  u32 runs = 0;          // pieces joined across every fall the draw lays a nappe on
  u32 falls = 0;         // lip-to-foot pairs
  u32 falls_drawn = 0;   // of them, those the draw lays a nappe on (the foot lower, drawn)
  u32 gaps = 0;          // maximal runs of stretches not drawn
  f64 thin_m = 0;        // stretches leaving a station `k_run_thin_depth_m` deep or less
  u32 alternations = 0;  // runs of one kind, thin or not (the forest's "pieces")
  u32 climbs = 0;        // stretches, not a fall or a jump, rising over `k_run_climb_m`
  u32 walls = 0;         // ... over `k_run_wall_m`
  u32 walls_drawn = 0;   // walls the draw lays a sheet on
  f64 climb_worst_m = 0;
};
LineRuns measure_runs(const scene::WaterChannel& block);

struct RunsBySlope {
  struct Class {
    u32 lines = 0;
    f64 length_m = 0, film_m = 0, undrawn_m = 0, thin_m = 0;
    u32 pieces = 0, runs = 0, falls = 0, falls_drawn = 0, gaps = 0, alternations = 0;
    u32 climbs = 0, walls = 0, walls_drawn = 0, wall_lines = 0;
    f64 climb_worst_m = 0;
  };
  Class classes[k_run_classes];
  void add(const LineRuns& line) noexcept;
};
u32 run_class(f64 slope) noexcept;
// One object a class: lines, km, film and undrawn shares of the length, pieces and runs every
// 100 m, falls (and those drawn), gaps, and climbs and walls (with the lines holding a wall and
// the highest climb).
JsonValue runs_json(const RunsBySlope& runs);

}  // namespace engine::channels
