#pragma once

// **The tile source** (docs/subsystems/scene_gen.md, "The tile source"; ADR-0048): the seam
// between the ground and whoever draws it or walks on it. It answers the heights of a window of the
// world's lattice at a spacing and a game time, and — for a ground that moves — how far its
// fastest feature travels between two times, which is what a time-lapse times its fields by. It
// says nothing about how the heights were made: no provider's name, parameters, seed or gather,
// and no file. So the renderer's tile levels and the collision's ground ask a tile source and
// cannot tell a procedural ground (`GroundProvider::tiles()`) from a tile set built ahead of time
// and read back from the derived-data cache (the renderer's tests build one).
//
// **The world's lattice.** A spacing is a whole number of millimetres and lattice point (i, j) is
// (i, j) times it from the world's origin (`ring_lattice`), so a tile's window is the same integers
// whoever asks and whichever level it is drawn at, and every coarser lattice is a subset of a finer
// one whose spacing divides it.

#include <core/base/types.h>

#include <span>

namespace engine::scene_gen {

struct TileSourceOps {
  // The heights at lattice points (i0 + i, j0 + j) of the world's lattice at `spacing_mm`, at game
  // time `time_s`: `nx * nz` metres, rows of x in order of z, blocks [block_begin, block_end) of
  // `window_blocks(nx, nz)` (a caller spreads the blocks over as many jobs as it likes and gets the
  // same bytes). A source that does not move answers every time with its one surface. False,
  // writing nothing, when the source has nothing there: a spacing a tile set was not built at, a
  // window outside it.
  bool (*heights)(const void* state, f64 time_s, i64 spacing_mm, i32 i0, i32 j0, u32 nx, u32 nz,
                  u32 block_begin, u32 block_end, std::span<f32> out) noexcept = nullptr;
  // How far its fastest feature travels between two game times, metres, along the way rather than
  // between the ends. Null: the heights do not depend on time.
  f64 (*travel_m)(const void* state, f64 from_s, f64 to_s) noexcept = nullptr;
};

// A tile source, not owned: the provider or the tile set behind it outlives every reader.
class TileSource {
 public:
  TileSource() noexcept = default;
  TileSource(const TileSourceOps* ops, const void* state) noexcept : ops_(ops), state_(state) {}

  bool valid() const noexcept { return ops_ != nullptr && ops_->heights != nullptr; }
  // Whether the heights depend on the time asked for.
  bool moves() const noexcept { return ops_ != nullptr && ops_->travel_m != nullptr; }
  bool heights(f64 time_s, i64 spacing_mm, i32 i0, i32 j0, u32 nx, u32 nz, u32 block_begin,
               u32 block_end, std::span<f32> out) const noexcept {
    return valid() &&
           ops_->heights(state_, time_s, spacing_mm, i0, j0, nx, nz, block_begin, block_end, out);
  }
  // Every block of the window at once.
  bool heights(f64 time_s, i64 spacing_mm, i32 i0, i32 j0, u32 nx, u32 nz,
               std::span<f32> out) const noexcept;
  f64 travel_m(f64 from_s, f64 to_s) const noexcept {
    return moves() ? ops_->travel_m(state_, from_s, to_s) : 0.0;
  }
  const TileSourceOps* ops() const noexcept { return ops_; }
  const void* state() const noexcept { return state_; }
  bool operator==(const TileSource& other) const noexcept = default;

 private:
  const TileSourceOps* ops_ = nullptr;
  const void* state_ = nullptr;
};

}  // namespace engine::scene_gen
