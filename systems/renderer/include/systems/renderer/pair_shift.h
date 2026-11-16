#pragma once

// **The pairs numbered again, as runs of slots** (T154; docs/subsystems/renderer.md, "The pairs are
// numbered again, and no slot moves"). When a streamed world's layout numbers its pairs again where
// the slots stand, every slot past the first that moved takes a new `first_pair`, and until T154
// each table set copied every such record out of the CPU's table again — the forest's 300,000
// slots, 29 MB a set, 16 to 24 ms of the frame's thread — while the CPU's own pass over them cost
// 3.7 to 5.8 ms more (a 96-byte record a slot, the table larger than the cache). But a slot's new
// number is a function of the run it is in: a block's slots all move by the block's distance, and
// a free run's null slots all take the run's one number. So a change says what moved as a short
// list of runs — a few per block, not a record per slot — which each table set applies on the GPU
// (`pair_expand.slang`, `pair_shift_main`), and the CPU's own table catches up a bounded number of
// slots a frame (`GpuScene`'s settle).
//
// A run either **adds** `value` to every slot's first pair in it (modulo 2^32, so a run that moves
// pairs down adds a wrapped negative) or **sets** every one to `value`. Runs are kept sorted by
// slot and disjoint; a slot no run covers keeps its number.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <span>

namespace engine::renderer {

struct PairShift {
  u32 begin = 0;  // the first slot
  u32 end = 0;    // one past the last
  u32 value = 0;  // added (mod 2^32), or set
  u32 set = 0;    // 0: add `value`; 1: set to `value`
};
static_assert(sizeof(PairShift) == 16);  // mirrored by `pair_expand.slang`'s `uint4`

// A first pair after the run.
inline u32 shifted(const PairShift& run, u32 first_pair) noexcept {
  return run.set != 0 ? run.value : first_pair + run.value;
}

// Appends `run` behind the last one (sorted, disjoint), merged into it when they touch and do the
// same thing. An empty run is dropped.
void push_shift(Vector<PairShift>& runs, const PairShift& run);

// `runs` then `later`, as one list of runs: what a slot's number becomes when `runs` is applied and
// then `later`. Both sorted and disjoint; so is the result, with adjacent runs of the same effect
// merged. `scratch` is reused, so a list that does not outgrow the last allocates nothing.
void compose_shifts(Vector<PairShift>& runs, std::span<const PairShift> later,
                    Vector<PairShift>& scratch);

// Slots `[begin, end)` taken out of `runs`: a change wrote them, with their numbers, so nothing
// pending applies to them any more.
void remove_shift_range(Vector<PairShift>& runs, u32 begin, u32 end, Vector<PairShift>& scratch);

// The run covering `slot`, or null: a binary search.
const PairShift* find_shift(std::span<const PairShift> runs, u32 slot) noexcept;

// The slots `runs` cover.
u64 shift_slots(std::span<const PairShift> runs) noexcept;

}  // namespace engine::renderer
