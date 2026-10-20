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
  Cliff cliff;
};

// The ranges `engine.rocks.CliffLayer` documents (its `face` but for the line, which the ground
// gives): false, with a sentence naming the first field out of range, otherwise.
bool cliff_layer_in_range(const CliffLayer& layer, std::string& error);

// Traces block (bx, bz) of the layer's tracing grid — `block_m` square, world-anchored, block (0,
// 0) at the origin — over `ground`: the feet of its steep ground as faces, in the order they were
// found (by the cell their line starts in, rows of x in order of z). A function of the layer, the
// block and the ground's heights over the block and a margin round it, and nothing else.
bool trace_cliff_block(const CliffLayer& layer, const scene_gen::Ground& ground, i32 bx, i32 bz,
                       Vector<CliffFace>& out, std::string* error = nullptr);

}  // namespace engine::rocks
