#pragma once

// The scene generator "cliffs" (docs/subsystems/rocks.md, "The cliffs generator"; roadmap G71): a
// placement generator through the scene-generator registry, as the scatter is, that finds a
// terrain's steep ground from the heights its ground answers, traces the foot of each steep band
// into a line, and stands a cliff face (`engine.rocks.Cliff`) along it — so a mountain scene gets
// its rock faces without an author drawing them. A face's identity is a hash of its line in whole
// millimetres, and a line is traced inside one block of a world-anchored grid from that block's
// heights alone, so a streamed world and a whole read make the same faces whichever tile asks.
//
// What it writes: each face's recipe, a `.rock.json`, under the host's derived-data root (a name
// that is its canonical parameters' hash, so a file already there is the right one), which the
// scene reader builds through the mesh-generator registry like any recipe.
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/rocks/rocks.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/rocks.h>
#include <string>

namespace engine::rocks {

// The registry's name: a scene's `{"generator": "cliffs", "params": {...}}`.
inline constexpr const char* k_cliffs_generator = "cliffs";

// One face the generator traced: its origin — the first vertex of its line, in whole millimetres of
// the world, and the ground there in metres — its identity, its anchor, the block it was traced
// in, and its recipe, whose line is metres from the origin.
struct CliffFace {
  i64 x_mm = 0;
  i64 z_mm = 0;
  f64 y = 0.0;
  u64 identity = 0;
  // Its anchor: the middle vertex of its line, whole millimetres. A face belongs to the region and
  // the tile its anchor is in — not its origin, which a line run on past its block's edge moves
  // out of the block.
  i64 anchor_x_mm = 0;
  i64 anchor_z_mm = 0;
  i32 block_x = 0;
  i32 block_z = 0;
  f64 length = 0.0;       // metres along its line
  f64 mean_height = 0.0;  // metres
  // Its tier (rocks.md, "A face in tiers"): the band of the layer's tier breaks it stands in, and
  // whether it is the lowest tier at its line (the one with the talus and the foot boulders).
  i32 tier = 0;
  bool lowest = true;
  Cliff cliff;
};

// How far behind its mean plane the solver stands a face's back: the recipe's `depth`, else the
// carving's deepest reach on a face `height_m` tall plus two metres, never under 4 m.
f64 cliff_back_m(const Cliff& cliff, f64 height_m);

// The ranges `engine.rocks.CliffLayer` documents (its `face` but for the line, which the ground
// gives): false, with a sentence naming the first field out of range, otherwise.
bool cliff_layer_in_range(const CliffLayer& layer, std::string& error);

// Bake the supporting terrain into a standalone recipe. One preliminary solve measures its
// footprint; the grid starts at spacing_m (0.25..32 m), coarsening only to meet CliffGround's
// explicit sample/dimension limits. Origin is the recipe's world placement. No terrain edits.
// Failure leaves the recipe unchanged. The generator invokes this for each traced face.
bool fit_cliff_ground(Cliff& cliff, const scene_gen::Ground& ground, WorldPos origin, f32 spacing_m,
                      std::string* error = nullptr);

// Traces block (bx, bz) of the layer's tracing grid — `block_m` square, world-anchored, block (0,
// 0) at the origin — over `ground`: the feet of its steep ground as faces, in the order they were
// found (by the cell their line starts in, rows of x in order of z). A function of the layer, the
// block and the ground's heights over the block and a margin round it, and nothing else.
// A face whose smoothed line folds is merged to the recipe validator's vertex gap; one that
// collapses under it is left out and counted in `faces_skipped`.
bool trace_cliff_block(const CliffLayer& layer, const scene_gen::Ground& ground, i32 bx, i32 bz,
                       Vector<CliffFace>& out, std::string* error = nullptr,
                       u32* faces_skipped = nullptr);

}  // namespace engine::rocks
