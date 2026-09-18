#include <domain/sim/timing_wheel.h>

#include <algorithm>
#include <bit>

namespace engine::sim {

namespace {

constexpr u8 k_flag_periodic = 1u;

// Level 1 spans about a minute, level 2 about an hour, level 3 about a day, level 4 about 2.8
// years. Level 0's slot count is derived from the fixed step so that it spans about a second.
constexpr u32 k_upper_slots[k_wheel_levels - 1] = {60u, 60u, 24u, 1024u};

constexpr u32 k_min_level0_slots = 4u;
constexpr u32 k_max_level0_slots = 4096u;

}  // namespace

TimingWheel::TimingWheel(const TimingWheelConfig& config) {
  i64 step_us = config.step.us;
  if (step_us <= 0) step_us = 16'667;
  resolution_[0] = step_us;

  i64 per_second = (1'000'000 + step_us / 2) / step_us;  // rounded, so 60 Hz gives 60 and not 61
  if (per_second < static_cast<i64>(k_min_level0_slots)) {
    per_second = static_cast<i64>(k_min_level0_slots);
  }
  if (per_second > static_cast<i64>(k_max_level0_slots)) {
    per_second = static_cast<i64>(k_max_level0_slots);
  }
  slot_count_[0] = static_cast<u32>(per_second);
  for (u32 level = 1; level < k_wheel_levels; ++level) {
    resolution_[level] = resolution_[level - 1] * static_cast<i64>(slot_count_[level - 1]);
    slot_count_[level] = k_upper_slots[level - 1];
  }

  u32 buckets = 0;
  u32 words = 0;
  for (u32 level = 0; level < k_wheel_levels; ++level) {
    bucket_base_[level] = buckets;
    word_base_[level] = words;
    buckets += slot_count_[level];
    words += (slot_count_[level] + 63u) / 64u;
  }
  far_bucket_ = buckets;

  heads_.assign(buckets + 1u, k_invalid_slot);
  occupancy_.assign(words, u64{0});

  now_ = config.epoch.us > 0 ? config.epoch : GameTime{0};
  refresh_now_indices();
  if (config.initial_capacity != 0) slab_.reserve(config.initial_capacity);
  batch_.reserve(64);
}

GameTime TimingWheel::horizon() const noexcept {
  const u32 top = k_wheel_levels - 1;
  const i64 end =
      static_cast<i64>((now_index_[k_index_count - 1] + 1u) * static_cast<u64>(slot_count_[top])) *
      resolution_[top];
  return GameTime{end - now_.us};
}

u64 TimingWheel::level_index(u32 level, i64 us) const noexcept {
  const i64 clamped = us > 0 ? us : 0;
  return static_cast<u64>(clamped / resolution_[level]);
}

void TimingWheel::indices_of(i64 us, u64* out) const noexcept {
  const i64 clamped = us > 0 ? us : 0;
  out[0] = static_cast<u64>(clamped / resolution_[0]);
  for (u32 level = 0; level < k_wheel_levels; ++level) {
    out[level + 1] = out[level] / static_cast<u64>(slot_count_[level]);
  }
}

void TimingWheel::refresh_now_indices() noexcept { indices_of(now_.us, now_index_); }

// The level a timer belongs at: the lowest one whose *block* — the span a level covers before
// its parent has to hand it more work — contains both the due time and now. Because level L+1's
// index is level L's divided by level L's slot count, "the same block at level L" is exactly
// "the same index at level L+1", so this is five comparisons and no arithmetic.
u32 TimingWheel::choose_level(const u64* index) const noexcept {
  for (u32 level = 0; level < k_wheel_levels; ++level) {
    if (index[level + 1] == now_index_[level + 1]) return level;
  }
  return k_wheel_levels;
}

u32 TimingWheel::bucket_from(u32 level, const u64* index) const noexcept {
  const u64 within = index[level] - index[level + 1] * static_cast<u64>(slot_count_[level]);
  return bucket_base_[level] + static_cast<u32>(within);
}

u32 TimingWheel::bucket_of(u32 index) const noexcept {
  const Slot& slot = slab_[index];
  if (static_cast<u32>(slot.level) >= k_wheel_levels) return far_bucket_;
  const u32 level = slot.level;
  const u64 absolute = level_index(level, slot.due.us);
  return bucket_base_[level] + static_cast<u32>(absolute % static_cast<u64>(slot_count_[level]));
}

void TimingWheel::link(u32 slot_index, u32 level, const u64* index) noexcept {
  Slot& slot = slab_[slot_index];
  slot.level = static_cast<u8>(level);
  const bool is_far = level >= k_wheel_levels;
  const u32 bucket = is_far ? far_bucket_ : bucket_from(level, index);
  slot.prev = k_invalid_slot;
  slot.next = heads_[bucket];
  if (slot.next != k_invalid_slot) slab_[slot.next].prev = slot_index;
  heads_[bucket] = slot_index;
  if (is_far) {
    ++far_count_;
    return;
  }
  const u32 within = bucket - bucket_base_[level];
  occupancy_[word_base_[level] + within / 64u] |= (u64{1} << (within % 64u));
}

// Places a detached slot wherever its due time says it belongs now. Used by the cascade, by the
// far list, and by a periodic re-arming: the three places a timer moves without the caller
// already knowing where to.
void TimingWheel::relink(u32 slot_index) noexcept {
  if (slab_[slot_index].due < now_) slab_[slot_index].due = now_;
  u64 index[k_index_count];
  indices_of(slab_[slot_index].due.us, index);
  link(slot_index, choose_level(index), index);
}

void TimingWheel::unlink(u32 slot_index) noexcept {
  Slot& slot = slab_[slot_index];
  const u32 bucket = bucket_of(slot_index);
  if (slot.prev != k_invalid_slot) {
    slab_[slot.prev].next = slot.next;
  } else {
    heads_[bucket] = slot.next;
  }
  if (slot.next != k_invalid_slot) slab_[slot.next].prev = slot.prev;
  slot.next = k_invalid_slot;
  slot.prev = k_invalid_slot;
  if (static_cast<u32>(slot.level) >= k_wheel_levels) {
    --far_count_;
    return;
  }
  if (heads_[bucket] == k_invalid_slot) {
    const u32 level = slot.level;
    const u32 within = bucket - bucket_base_[level];
    occupancy_[word_base_[level] + within / 64u] &= ~(u64{1} << (within % 64u));
  }
}

u32 TimingWheel::alloc_slot() {
  if (free_head_ != k_invalid_slot) {
    const u32 index = free_head_;
    free_head_ = slab_[index].next;
    Slot& slot = slab_[index];
    slot.next = k_invalid_slot;
    slot.prev = k_invalid_slot;
    ++slot.generation;  // even (free) becomes odd (live)
    return index;
  }
  Slot fresh{};
  fresh.next = k_invalid_slot;
  fresh.prev = k_invalid_slot;
  fresh.generation = 1u;
  fresh.system = k_no_system;
  slab_.push_back(fresh);
  return slab_.size() - 1u;
}

void TimingWheel::free_slot(u32 index) noexcept {
  Slot& slot = slab_[index];
  ++slot.generation;  // odd (live) becomes even (free): every handle to it is now stale
  slot.next = free_head_;
  slot.prev = k_invalid_slot;
  slot.flags = 0;
  free_head_ = index;
}

bool TimingWheel::is_live(TimerHandle handle) const noexcept {
  return handle.valid() && handle.index < slab_.size() &&
         slab_[handle.index].generation == handle.generation;
}

bool TimingWheel::due_time(TimerHandle handle, GameTime& out) const noexcept {
  if (!is_live(handle)) return false;
  out = slab_[handle.index].due;
  return true;
}

TimerHandle TimingWheel::schedule(GameTime at, const TimerPayload& payload) {
  if (at < now_) at = now_;
  const u32 index = alloc_slot();
  {
    Slot& slot = slab_[index];
    slot.due = at;
    slot.interval = GameTime{0};
    slot.sequence = next_sequence_++;
    slot.payload = payload;
    slot.system = k_no_system;
    slot.flags = 0;
  }
  u64 position[k_index_count];
  indices_of(at.us, position);
  link(index, choose_level(position), position);
  ++live_;
  return TimerHandle{index, slab_[index].generation};
}

TimerHandle TimingWheel::schedule_periodic(u16 system, GameTime interval, GameTime phase) {
  return schedule_periodic(system, interval, phase, TimerPayload{});
}

TimerHandle TimingWheel::schedule_periodic(u16 system, GameTime interval, GameTime phase,
                                           const TimerPayload& payload) {
  i64 period = interval.us;
  if (period < resolution_[0]) period = resolution_[0];  // a zero period would never advance

  i64 offset = phase.us % period;
  if (offset < 0) offset += period;
  i64 first = offset;
  if (first <= now_.us) first += ((now_.us - first) / period + 1) * period;

  const u32 index = alloc_slot();
  {
    Slot& slot = slab_[index];
    slot.due = GameTime{first};
    slot.interval = GameTime{period};
    slot.sequence = next_sequence_++;
    slot.payload = payload;
    slot.system = system;
    slot.flags = k_flag_periodic;
  }
  u64 position[k_index_count];
  indices_of(first, position);
  link(index, choose_level(position), position);
  periodics_.push_back(index);
  ++live_;
  return TimerHandle{index, slab_[index].generation};
}

bool TimingWheel::cancel(TimerHandle handle) noexcept {
  if (!is_live(handle)) return false;
  const u32 index = handle.index;
  unlink(index);
  if ((slab_[index].flags & k_flag_periodic) != 0) {
    for (u32 i = 0; i < periodics_.size(); ++i) {
      if (periodics_[i] != index) continue;
      periodics_[i] = periodics_[periodics_.size() - 1u];
      periodics_.pop_back();
      break;
    }
  }
  free_slot(index);
  --live_;
  return true;
}

void TimingWheel::cascade(u32 level, u32 slot) {
  const u32 bucket = bucket_base_[level] + slot;
  u32 index = heads_[bucket];
  if (index == k_invalid_slot) return;
  // Detach the whole list before re-inserting any of it: link() writes into the same heads_
  // entry, and walking a list that is being rebuilt under you is how wheels lose timers.
  heads_[bucket] = k_invalid_slot;
  occupancy_[word_base_[level] + slot / 64u] &= ~(u64{1} << (slot % 64u));
  while (index != k_invalid_slot) {
    const u32 next = slab_[index].next;
    slab_[index].next = k_invalid_slot;
    slab_[index].prev = k_invalid_slot;
    relink(index);
    index = next;
  }
}

void TimingWheel::rehome_far() {
  u32 index = heads_[far_bucket_];
  if (index == k_invalid_slot) return;
  heads_[far_bucket_] = k_invalid_slot;
  far_count_ = 0;
  while (index != k_invalid_slot) {
    const u32 next = slab_[index].next;
    slab_[index].next = k_invalid_slot;
    slab_[index].prev = k_invalid_slot;
    relink(index);
    index = next;
  }
}

void TimingWheel::set_now(GameTime to) {
  if (to.us <= now_.us) {
    now_ = to;
    refresh_now_indices();
    return;
  }
  u64 previous[k_index_count];
  for (u32 i = 0; i < k_index_count; ++i)
    previous[i] = now_index_[i];
  now_ = to;
  refresh_now_indices();

  // Highest level first, so a timer that falls from level 4 to level 3 is still seen by level
  // 3's own cascade in the same move. A jump longer than a level's whole revolution cascades
  // every bucket of it once, which is correct because re-linking a bucket that is still in the
  // future puts its timers straight back where they were.
  for (u32 level = k_wheel_levels; level-- > 1;) {
    const u64 before = previous[level];
    const u64 after = now_index_[level];
    if (after == before) continue;
    u64 steps = after - before;
    if (steps > static_cast<u64>(slot_count_[level])) steps = static_cast<u64>(slot_count_[level]);
    for (u64 step_index = 1; step_index <= steps; ++step_index) {
      cascade(level,
              static_cast<u32>((before + step_index) % static_cast<u64>(slot_count_[level])));
    }
  }
  if (far_count_ != 0 && previous[k_index_count - 1] != now_index_[k_index_count - 1]) {
    rehome_far();
  }
}

bool TimingWheel::next_occupied(u32 level, u32 first, u32 last, u32& out_slot) const noexcept {
  if (first > last) return false;
  const u32 base = word_base_[level];
  u32 word = first / 64u;
  const u32 last_word = last / 64u;
  u64 bits = occupancy_[base + word] & (~u64{0} << (first % 64u));
  while (true) {
    if (word == last_word) bits &= (~u64{0} >> (63u - (last % 64u)));
    if (bits != 0) {
      out_slot = word * 64u + static_cast<u32>(std::countr_zero(bits));
      return true;
    }
    if (word == last_word) return false;
    ++word;
    bits = occupancy_[base + word];
  }
}

bool TimingWheel::seek(GameTime to, u64& out_index) {
  const u32 top = k_wheel_levels - 1;
  u64 limit[k_index_count];
  indices_of(to.us, limit);

  while (true) {
    const u64 slots0 = static_cast<u64>(slot_count_[0]);
    const u64 now0 = now_index_[0];
    const u64 block0 = now_index_[1] * slots0;
    const u64 block0_end = block0 + slots0;
    u64 last0 = block0_end - 1u;
    if (limit[0] < last0) last0 = limit[0];

    u32 found = 0;
    if (now0 <= last0 && next_occupied(0, static_cast<u32>(now0 - block0),
                                       static_cast<u32>(last0 - block0), found)) {
      const u64 absolute = block0 + found;
      // The last bucket of the window is the only partial one: it may hold entries that are not
      // due yet, and returning it with nothing due would spin.
      if (absolute == limit[0]) {
        const u32 bucket = bucket_base_[0] + found;
        bool due = false;
        for (u32 i = heads_[bucket]; i != k_invalid_slot; i = slab_[i].next) {
          if (slab_[i].due <= to) {
            due = true;
            break;
          }
        }
        if (!due) return false;
      }
      const i64 start = static_cast<i64>(absolute) * resolution_[0];
      if (start > now_.us) set_now(GameTime{start});
      out_index = absolute;
      return true;
    }

    if (block0_end > limit[0]) return false;  // the rest of the window is in this block, and empty

    bool jumped = false;
    for (u32 level = 1; level <= top; ++level) {
      const u64 slots = static_cast<u64>(slot_count_[level]);
      const u64 now_position = now_index_[level];
      const u64 block = now_index_[level + 1] * slots;
      const u64 block_end = block + slots;
      u64 last = block_end - 1u;
      if (limit[level] < last) last = limit[level];
      const u64 first = now_position + 1u;
      u32 slot = 0;
      if (first <= last && next_occupied(level, static_cast<u32>(first - block),
                                         static_cast<u32>(last - block), slot)) {
        set_now(GameTime{static_cast<i64>(block + slot) * resolution_[level]});
        jumped = true;
        break;
      }
      // `to` lands inside this block and the block is empty from here on, so no higher level can
      // hold anything due before `to` either.
      if (block_end > limit[level]) return false;
    }
    if (jumped) continue;

    // Everything in the wheel proper is past `to`; only the far list can still hold something.
    // It is scanned linearly because it is meant to be nearly empty: a timer more than the top
    // level's span away is a rarity, not a population.
    if (far_count_ == 0) return false;
    GameTime nearest{0};
    bool any = false;
    for (u32 i = heads_[far_bucket_]; i != k_invalid_slot; i = slab_[i].next) {
      if (!any || slab_[i].due < nearest) {
        nearest = slab_[i].due;
        any = true;
      }
    }
    if (!any || nearest > to) return false;
    set_now(nearest);
    rehome_far();
  }
}

void TimingWheel::deliver_bucket(u64 index0, GameTime to, const EventSink& sink, u64& delivered) {
  const u32 bucket = bucket_base_[0] + static_cast<u32>(index0 % static_cast<u64>(slot_count_[0]));
  batch_.clear();
  for (u32 i = heads_[bucket]; i != k_invalid_slot; i = slab_[i].next) {
    if (slab_[i].due <= to) batch_.push_back(TimerHandle{i, slab_[i].generation});
  }
  if (batch_.empty()) return;

  // The one place delivery order is decided: due time first, then insertion sequence. Both are
  // integers assigned by this class, so the order is the same on every machine and does not
  // depend on how the bucket's list happens to be threaded.
  std::sort(batch_.begin(), batch_.end(), [this](TimerHandle a, TimerHandle b) {
    const Slot& first = slab_[a.index];
    const Slot& second = slab_[b.index];
    if (first.due.us != second.due.us) return first.due.us < second.due.us;
    return first.sequence < second.sequence;
  });

  for (u32 n = 0; n < batch_.size(); ++n) {
    const TimerHandle handle = batch_[n];
    if (!is_live(handle)) continue;  // an earlier sink call cancelled it
    const u32 index = handle.index;
    TimerEvent event;
    {
      const Slot& slot = slab_[index];
      event.at = slot.due;
      event.interval = slot.interval;
      event.payload = slot.payload;
      event.sequence = slot.sequence;
      event.handle = handle;
      event.system = slot.system;
      event.periodic = (slot.flags & k_flag_periodic) != 0;
    }
    unlink(index);
    // A periodic re-arms *before* the sink runs, so a sink that cancels its own timer wins and a
    // sink that does not gets a wheel already holding the next firing.
    if (event.periodic) {
      slab_[index].due = GameTime{event.at.us + slab_[index].interval.us};
      relink(index);
    } else {
      free_slot(index);
      --live_;
    }
    ++delivered;
    sink(event);
  }
}

void TimingWheel::advance(GameTime to, const EventSink& sink) {
  if (to <= now_) return;
  u64 delivered = 0;
  u64 index0 = 0;
  while (seek(to, index0))
    deliver_bucket(index0, to, sink, delivered);
  set_now(to);
}

u64 TimingWheel::count_due(GameTime to, u64 limit) const {
  if (to <= now_) return 0;
  u64 bound[k_index_count];
  indices_of(to.us, bound);
  u64 count = 0;
  for (u32 level = 0; level < k_wheel_levels; ++level) {
    const u64 slots = static_cast<u64>(slot_count_[level]);
    const u64 now_position = now_index_[level];
    const u64 block = now_index_[level + 1] * slots;
    const u64 block_end = block + slots;
    u64 last = block_end - 1u;
    if (bound[level] < last) last = bound[level];
    for (u64 index = now_position; index <= last; ++index) {
      const u32 bucket = bucket_base_[level] + static_cast<u32>(index - block);
      for (u32 i = heads_[bucket]; i != k_invalid_slot; i = slab_[i].next) {
        const Slot& slot = slab_[i];
        if (slot.due > to) continue;
        // A periodic's firings are counted arithmetically: a daily periodic over a game year is
        // one division, not 365 visits.
        if ((slot.flags & k_flag_periodic) != 0 && slot.interval.us > 0) {
          count += static_cast<u64>((to.us - slot.due.us) / slot.interval.us) + 1u;
        } else {
          ++count;
        }
        if (count > limit) return count;
      }
    }
    if (block_end > bound[level]) return count;  // `to` is inside this block: higher levels later
  }
  for (u32 i = heads_[far_bucket_]; i != k_invalid_slot; i = slab_[i].next) {
    if (slab_[i].due > to) continue;
    ++count;
    if (count > limit) return count;
  }
  return count;
}

bool TimingWheel::summarizes(u16 system) const noexcept {
  if (system == k_no_system) return false;
  for (u32 i = 0; i < summarizers_.size(); ++i) {
    if (summarizers_[i].system == system) return true;
  }
  return false;
}

void TimingWheel::coarsen(GameTime to, FastForwardResult& result) {
  for (u32 n = 0; n < periodics_.size(); ++n) {
    const u32 index = periodics_[n];
    if (slab_[index].interval.us <= 0) continue;
    if (!summarizes(slab_[index].system)) continue;
    if (slab_[index].due > to) continue;
    const i64 period = slab_[index].interval.us;
    const i64 skipped = (to.us - slab_[index].due.us) / period + 1;
    unlink(index);
    slab_[index].due = GameTime{slab_[index].due.us + skipped * period};
    relink(index);
    ++result.coarsened;
  }
}

FastForwardResult TimingWheel::advance(GameTime from, GameTime to, u64 budget,
                                       const EventSink& sink) {
  FastForwardResult result;
  result.from = now_;
  result.to = to;
  ENGINE_ASSERT(from == now_, "TimingWheel::advance: `from` must be the wheel's current time");
  (void)from;
  if (to <= now_) return result;

  result.due_estimate = count_due(to, budget);
  if (result.due_estimate > budget) {
    result.over_budget = true;
    SummarizeInterval interval;
    interval.from = now_;
    interval.to = to;
    for (u32 i = 0; i < summarizers_.size(); ++i) {
      const Summarizer& summarizer = summarizers_[i];
      interval.system = summarizer.system;
      interval.tier = 3;
      if (summarizer.fn != nullptr) summarizer.fn(summarizer.context, interval);
      ++result.summarized;
    }
    coarsen(to, result);
  }

  u64 delivered = 0;
  u64 index0 = 0;
  while (seek(to, index0))
    deliver_bucket(index0, to, sink, delivered);
  set_now(to);
  result.delivered = delivered;
  return result;
}

u32 TimingWheel::add_summarizer(const Summarizer& summarizer) {
  summarizers_.push_back(summarizer);
  return summarizers_.size() - 1u;
}

void TimingWheel::reset(GameTime at) {
  slab_.clear();
  periodics_.clear();
  batch_.clear();
  for (u32 i = 0; i < heads_.size(); ++i)
    heads_[i] = k_invalid_slot;
  for (u32 i = 0; i < occupancy_.size(); ++i)
    occupancy_[i] = 0;
  free_head_ = k_invalid_slot;
  live_ = 0;
  far_count_ = 0;
  next_sequence_ = 0;
  now_ = at.us > 0 ? at : GameTime{0};
  refresh_now_indices();
}

bool TimingWheel::validate() const {
  u32 counted = 0;
  for (u32 bucket = 0; bucket < heads_.size(); ++bucket) {
    const bool is_far = bucket == far_bucket_;
    u32 level = k_wheel_levels;
    u32 within = 0;
    if (!is_far) {
      for (u32 candidate = 0; candidate < k_wheel_levels; ++candidate) {
        if (bucket >= bucket_base_[candidate] &&
            bucket < bucket_base_[candidate] + slot_count_[candidate]) {
          level = candidate;
          within = bucket - bucket_base_[candidate];
          break;
        }
      }
      if (level == k_wheel_levels) return false;
      const bool bit =
          (occupancy_[word_base_[level] + within / 64u] & (u64{1} << (within % 64u))) != 0;
      if (bit != (heads_[bucket] != k_invalid_slot)) return false;
    }
    u32 previous = k_invalid_slot;
    for (u32 i = heads_[bucket]; i != k_invalid_slot; i = slab_[i].next) {
      if (slab_[i].prev != previous) return false;
      if ((slab_[i].generation & 1u) == 0u) return false;
      if (static_cast<u32>(slab_[i].level) != level) return false;
      if (!is_far) {
        if (bucket_of(i) != bucket) return false;
        u64 position[k_index_count];
        indices_of(slab_[i].due.us, position);
        if (choose_level(position) != level) return false;
      }
      previous = i;
      ++counted;
    }
  }
  return counted == live_;
}

}  // namespace engine::sim
