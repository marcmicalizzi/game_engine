#include <systems/renderer/pair_shift.h>

#include <algorithm>

namespace engine::renderer {

void push_shift(Vector<PairShift>& runs, const PairShift& run) {
  if (run.begin >= run.end) return;
  if (!runs.empty()) {
    PairShift& last = runs.back();
    if (last.end == run.begin && last.set == run.set && last.value == run.value) {
      last.end = run.end;
      return;
    }
  }
  runs.push_back(run);
}

void compose_shifts(Vector<PairShift>& runs, std::span<const PairShift> later,
                    Vector<PairShift>& scratch) {
  if (later.empty()) return;
  scratch.clear();
  u32 a = 0;
  u32 b = 0;
  // The sweep's position: everything before it has been emitted.
  u32 at = 0;
  const u32 na = runs.size();
  const u32 nb = static_cast<u32>(later.size());
  while (a < na || b < nb) {
    const PairShift* ra = a < na ? &runs[a] : nullptr;
    const PairShift* rb = b < nb ? &later[b] : nullptr;
    // The next interval starts at the first slot either list covers at or after `at`.
    u32 begin = ~u32{0};
    if (ra != nullptr) begin = std::min(begin, std::max(ra->begin, at));
    if (rb != nullptr) begin = std::min(begin, std::max(rb->begin, at));
    const bool in_a = ra != nullptr && ra->begin <= begin && begin < ra->end;
    const bool in_b = rb != nullptr && rb->begin <= begin && begin < rb->end;
    // It ends where either run ends or the other one starts.
    u32 end = ~u32{0};
    if (ra != nullptr) end = std::min(end, in_a ? ra->end : ra->begin);
    if (rb != nullptr) end = std::min(end, in_b ? rb->end : rb->begin);
    PairShift out{begin, end, 0, 0};
    if (in_a && in_b) {
      out.set = (rb->set | ra->set) != 0 ? 1u : 0u;
      out.value = rb->set != 0 ? rb->value : ra->value + rb->value;
    } else if (in_a) {
      out.set = ra->set;
      out.value = ra->value;
    } else {
      out.set = rb->set;
      out.value = rb->value;
    }
    push_shift(scratch, out);
    at = end;
    if (ra != nullptr && ra->end <= at) ++a;
    if (rb != nullptr && rb->end <= at) ++b;
  }
  // Copied back, not swapped: a swap would trade capacities between every list and the one scratch,
  // and a list handed the smaller one would allocate again in a steady frame (engine_view's frame
  // loop caught one, frame 68 of the streamed trees' second flight).
  runs.assign(scratch.begin(), scratch.end());
}

void remove_shift_range(Vector<PairShift>& runs, u32 begin, u32 end, Vector<PairShift>& scratch) {
  if (begin >= end || runs.empty()) return;
  // Nothing to do unless a run overlaps the range.
  const PairShift* first = find_shift(std::span<const PairShift>(runs.data(), runs.size()), begin);
  if (first == nullptr) {
    // The first run at or after `begin`.
    u32 lo = 0;
    u32 hi = runs.size();
    while (lo < hi) {
      const u32 mid = (lo + hi) / 2;
      if (runs[mid].begin < begin) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo >= runs.size() || runs[lo].begin >= end) return;
  }
  scratch.clear();
  for (const PairShift& run : runs) {
    if (run.end <= begin || run.begin >= end) {
      scratch.push_back(run);
      continue;
    }
    if (run.begin < begin) scratch.push_back(PairShift{run.begin, begin, run.value, run.set});
    if (run.end > end) scratch.push_back(PairShift{end, run.end, run.value, run.set});
  }
  runs.assign(scratch.begin(), scratch.end());  // copied back, as above
}

const PairShift* find_shift(std::span<const PairShift> runs, u32 slot) noexcept {
  u32 lo = 0;
  u32 hi = static_cast<u32>(runs.size());
  while (lo < hi) {
    const u32 mid = (lo + hi) / 2;
    if (runs[mid].end <= slot) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < runs.size() && runs[lo].begin <= slot && slot < runs[lo].end) return &runs[lo];
  return nullptr;
}

u64 shift_slots(std::span<const PairShift> runs) noexcept {
  u64 slots = 0;
  for (const PairShift& run : runs)
    slots += run.end - run.begin;
  return slots;
}

u32 shift_run_bound(u32 slots) noexcept { return slots; }

void reserve_shift_runs(Vector<PairShift>& runs, u32 slots) {
  const u32 bound = shift_run_bound(slots);
  if (runs.capacity() < bound) runs.reserve(bound);
}

}  // namespace engine::renderer
