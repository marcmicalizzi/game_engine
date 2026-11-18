// The keep rule of a frame past its ray tracing chain's capacity (rt_keep.h; R174;
// docs/subsystems/renderer.md, "Past the capacity, by size on screen"): no device. The shader runs
// the same rule (clas_records.slang), and `clas records: past the capacity the cut keeps what
// rt_keep keeps` holds the two together on the GPU.

#include <domain/gfx/rt_keep.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

using namespace engine;
using namespace engine::gfx;

namespace {

struct Kept {
  std::vector<u32> drawn;
  std::vector<u32> casters;
  RtKeepResult result;
};

Kept keep(const std::vector<RtKeepItem>& items, u32 capacity, u32 reference_capacity,
          bool drawn_first) {
  Kept k;
  k.drawn.assign(items.size(), 0xdeadu);
  k.casters.assign(items.size(), 0xdeadu);
  k.result = rt_keep(items, capacity, reference_capacity, drawn_first, k.drawn, k.casters);
  return k;
}

u64 wanted_of(const std::vector<RtKeepItem>& items, bool cached) {
  u64 w = 0;
  for (const RtKeepItem& item : items)
    w += item.cached == cached ? u64{item.drawn} + item.casters : 0u;
  return w;
}

// Items with many equal keys (q and h from small ranges), counts with zeros, about a third cached.
std::vector<RtKeepItem> random_items(u32 n, u32 seed) {
  std::mt19937 rng(seed);
  std::vector<RtKeepItem> items(n);
  for (RtKeepItem& item : items) {
    item.drawn = rng() % 4 == 0 ? 0u : 1u + rng() % 300;
    item.casters = rng() % 3 == 0 ? 0u : rng() % 60;
    item.cached = rng() % 3 == 0;
    item.key.q = rng() % 64;
    item.key.h = rng() % 8;
  }
  return items;
}

}  // namespace

TEST_CASE(
    "rt keep: two orders of the same placements keep the same set at a capacity that forces a "
    "drop") {
  const std::vector<RtKeepItem> items = random_items(2000, 174u);
  const u64 built = wanted_of(items, false);
  const u64 cached = wanted_of(items, true);
  u32 dropping = 0;
  for (const bool drawn_first : {false, true}) {
    for (u32 step = 0; step <= 40; ++step) {
      const u32 capacity = static_cast<u32>(built * step / 40);
      const u32 reference_capacity = static_cast<u32>(cached * ((step * 7) % 41) / 40);
      const Kept base = keep(items, capacity, reference_capacity, drawn_first);
      if (base.result.words.dropped_drawn + base.result.words.dropped_casters != 0) ++dropping;
      for (u32 order = 0; order < 8; ++order) {
        std::vector<u32> perm(items.size());
        std::iota(perm.begin(), perm.end(), 0u);
        std::mt19937 rng(1000u + order);
        std::shuffle(perm.begin(), perm.end(), rng);
        std::vector<RtKeepItem> shuffled(items.size());
        for (usize i = 0; i < perm.size(); ++i)
          shuffled[i] = items[perm[i]];
        const Kept other = keep(shuffled, capacity, reference_capacity, drawn_first);
        CHECK(other.result.words == base.result.words);
        CHECK(other.result.cuts[0] == base.result.cuts[0]);
        CHECK(other.result.cuts[1] == base.result.cuts[1]);
        u32 differ = 0;
        for (usize i = 0; i < perm.size(); ++i) {
          differ += other.drawn[i] != base.drawn[perm[i]] ? 1u : 0u;
          differ += other.casters[i] != base.casters[perm[i]] ? 1u : 0u;
        }
        CHECK(differ == 0);
      }
    }
  }
  // The capacities above force drops on most steps, or the case proves nothing.
  CHECK(dropping > 40);
}

TEST_CASE("rt keep: the kept set is a prefix of the keys, ties kept or dropped whole") {
  SUBCASE("random: within a kind every kept item is keyed below every dropped one") {
    const std::vector<RtKeepItem> items = random_items(3000, 7u);
    const u64 built = wanted_of(items, false);
    const u64 cached = wanted_of(items, true);
    for (const bool drawn_first : {false, true}) {
      for (u32 step = 0; step <= 20; ++step) {
        const u32 capacity = static_cast<u32>(built * step / 20);
        const u32 reference_capacity = static_cast<u32>(cached * (20 - step) / 20);
        const Kept k = keep(items, capacity, reference_capacity, drawn_first);
        u32 most_kept[2] = {0, 0};
        u32 least_dropped[2] = {0xffffffffu, 0xffffffffu};
        u64 kept[2] = {0, 0};
        for (usize i = 0; i < items.size(); ++i) {
          const RtKeepItem& item = items[i];
          const u32 kind = item.cached ? 1u : 0u;
          // All of each kind or none, and a caster never without its drawn structures.
          CHECK((k.drawn[i] == 0 || k.drawn[i] == item.drawn));
          CHECK((k.casters[i] == 0 || k.casters[i] == item.casters));
          if (k.casters[i] != 0) CHECK(k.drawn[i] == item.drawn);
          kept[kind] += u64{k.drawn[i]} + k.casters[i];
          const u32 dk = rt_keep_item_key(item.key, false, drawn_first);
          const u32 ck = rt_keep_item_key(item.key, true, drawn_first);
          if (item.drawn != 0) {
            if (k.drawn[i] != 0)
              most_kept[kind] = std::max(most_kept[kind], dk);
            else
              least_dropped[kind] = std::min(least_dropped[kind], dk);
          }
          if (item.casters != 0) {
            if (k.casters[i] != 0)
              most_kept[kind] = std::max(most_kept[kind], ck);
            else
              least_dropped[kind] = std::min(least_dropped[kind], ck);
          }
        }
        for (u32 kind = 0; kind < 2; ++kind) {
          if (least_dropped[kind] != 0xffffffffu && kept[kind] != 0)
            CHECK(most_kept[kind] < least_dropped[kind]);
          // The cut is the least dropped key, and nothing past the capacity is kept.
          const RtKeepCut& cut = k.result.cuts[kind];
          CHECK(cut.crosses == (least_dropped[kind] != 0xffffffffu));
          if (cut.crosses) CHECK(cut.key == least_dropped[kind]);
          CHECK(kept[kind] + cut.room == (kind == 0 ? capacity : reference_capacity));
        }
        CHECK(k.result.words.built == kept[0]);
        CHECK(k.result.words.references_kept == kept[1]);
        CHECK(k.result.words.wanted == built);
        CHECK(k.result.words.references_wanted == cached);
      }
    }
  }

  SUBCASE("a tie straddling the capacity is dropped whole") {
    // Three items of one key whose sum crosses: all three go, the one below stays.
    std::vector<RtKeepItem> items(4);
    for (u32 i = 0; i < 3; ++i)
      items[i] = {10, 0, false, {5, 9}};
    items[3] = {10, 0, false, {4, 9}};
    const Kept k = keep(items, 25, 0, false);
    CHECK(k.drawn == std::vector<u32>{0, 0, 0, 10});
    CHECK(k.result.words.dropped_drawn == 3);
    CHECK(k.result.words.built == 10);
    CHECK(k.result.cuts[0].crosses);
    CHECK(k.result.cuts[0].room == 15);
    // At a capacity that holds the tie it is kept whole.
    const Kept all = keep(items, 40, 0, false);
    CHECK(all.drawn == std::vector<u32>{10, 10, 10, 10});
    CHECK_FALSE(all.result.cuts[0].crosses);
  }

  SUBCASE("several hashes in one size step straddling the capacity part it by the hash") {
    std::vector<RtKeepItem> items;
    for (u32 h = 0; h < 6; ++h)
      items.push_back({10, 0, false, {3, 5 - h}});
    const Kept k = keep(items, 35, 0, false);
    // h 0, 1, 2 kept (30), h 3 crosses, so it and everything after it go.
    CHECK(k.drawn == std::vector<u32>{0, 0, 0, 10, 10, 10});
    CHECK(k.result.cuts[0].key == rt_keep_item_key({3, 3}, false, false));
  }

  SUBCASE("drawn first keeps every drawn structure before any caster") {
    // R143's property: with the capacity exactly the drawn total, every drawn structure is kept,
    // the far instance's included, and no caster is.
    std::vector<RtKeepItem> items = {
        {100, 50, false, {0, 0}}, {20, 5, false, {9000, 1}}, {7, 0, false, {30000, 2}}};
    const Kept k = keep(items, 127, 0, true);
    CHECK(k.drawn == std::vector<u32>{100, 20, 7});
    CHECK(k.casters == std::vector<u32>{0, 0, 0});
    CHECK(k.result.words.dropped_drawn == 0);
    CHECK(k.result.words.dropped_casters == 2);
    // Without drawn first the near instance's casters come before the far one's drawn clusters.
    const Kept by_size = keep(items, 127, 0, false);
    CHECK(by_size.drawn == std::vector<u32>{100, 0, 0});
    CHECK(by_size.casters == std::vector<u32>{0, 0, 0});
    CHECK(by_size.result.words.dropped_drawn == 2);
  }

  SUBCASE("the two capacities are cut on their own, and both can cross in one frame") {
    std::vector<RtKeepItem> items = {{10, 0, false, {1, 0}},
                                     {10, 0, false, {2, 0}},
                                     {10, 0, true, {1, 0}},
                                     {10, 0, true, {2, 0}}};
    const Kept refs_only = keep(items, 100, 15, false);
    CHECK(refs_only.drawn == std::vector<u32>{10, 10, 10, 0});
    CHECK_FALSE(refs_only.result.cuts[0].crosses);
    CHECK(refs_only.result.cuts[1].crosses);
    const Kept both = keep(items, 15, 15, false);
    CHECK(both.drawn == std::vector<u32>{10, 0, 10, 0});
    CHECK(both.result.cuts[0].crosses);
    CHECK(both.result.cuts[1].crosses);
    CHECK(both.result.words.dropped_drawn == 2);
  }

  SUBCASE("a capacity of nothing keeps nothing, one past the demand keeps everything") {
    const std::vector<RtKeepItem> items = random_items(500, 3u);
    const Kept none = keep(items, 0, 0, false);
    CHECK(none.result.words.built == 0);
    CHECK(none.result.words.references_kept == 0);
    const u32 built = static_cast<u32>(wanted_of(items, false));
    const u32 cached = static_cast<u32>(wanted_of(items, true));
    const Kept all = keep(items, built, cached, false);
    CHECK(all.result.words.built == built);
    CHECK(all.result.words.references_kept == cached);
    CHECK(all.result.words.dropped_drawn + all.result.words.dropped_casters == 0);
  }

  SUBCASE("one large instance first in key order and past the capacity alone keeps nothing") {
    // The prefix's price at a small named budget (renderer.md, "A cut can keep nothing at a small
    // named budget"): first-fit would have skipped it and kept the small ones.
    std::vector<RtKeepItem> items = {
        {200, 0, false, {0, 0}}, {5, 0, false, {100, 0}}, {5, 0, false, {200, 0}}};
    const Kept k = keep(items, 150, 0, true);
    CHECK(k.result.words.built == 0);
    CHECK(k.result.cuts[0].room == 150);
  }
}

TEST_CASE(
    "rt keep: the size key is monotone in distance and radius, and a scene moved by whole "
    "cells keys alike") {
  SUBCASE("q's ends and its steps") {
    // d/r = 1/16 is the bottom, 4,096 the top, a zero radius and a NaN distance the top too.
    CHECK(rt_keep_q(1.0f, 256.0f) == 0);
    CHECK(rt_keep_q(1.0f, 1.0e6f) == 0);
    CHECK(rt_keep_q(16777216.0f, 1.0f) == k_rt_keep_q_max);
    CHECK(rt_keep_q(1.0e30f, 1.0e-30f) == k_rt_keep_q_max);
    CHECK(rt_keep_q(4.0f, 0.0f) == k_rt_keep_q_max);
    // At the eye too: a frame with no spheres keys every instance at the top, whatever its place,
    // so the hash alone orders them (the records case "a set sized under the frame" on a device
    // found d = r = 0 keyed at 8,192 when this was a difference of bits).
    CHECK(rt_keep_q(0.0f, 0.0f) == k_rt_keep_q_max);
    CHECK(rt_keep_q(std::nanf(""), 1.0f) == k_rt_keep_q_max);
    // d = r is 8 octaves of (d/r)² above the bottom: 8,192.
    CHECK(rt_keep_q(9.0f, 9.0f) == 8 * 1024);
    // An octave of (d/r)² is 1,024 steps.
    CHECK(rt_keep_q(2.0f, 1.0f) - rt_keep_q(1.0f, 1.0f) == 1024);
  }

  SUBCASE("monotone in distance and radius") {
    InstanceDesc instance;
    const FrameEye eye{};
    u32 last = 0;
    for (u32 k = 0; k < 4000; ++k) {
      instance.rows[0].w = 0.25f + 0.37f * static_cast<f32>(k);
      const u32 q = rt_keep_key(instance, eye, Vec4(0.0f, 0.0f, 0.0f, 1.0f)).q;
      CHECK(q >= last);
      last = q;
    }
    CHECK(last > 0);
    instance.rows[0].w = 50.0f;
    last = k_rt_keep_q_max;
    for (u32 k = 1; k < 2000; ++k) {
      const f32 radius = 0.01f * static_cast<f32>(k);
      const u32 q = rt_keep_key(instance, eye, Vec4(0.0f, 0.0f, 0.0f, radius)).q;
      CHECK(q <= last);
      last = q;
    }
    // The largest axis scale multiplies the radius: twice the scale, a larger instance.
    instance.scale_max = 2.0f;
    CHECK(rt_keep_key(instance, eye, Vec4(0.0f, 0.0f, 0.0f, 1.0f)).q <
          rt_keep_key(InstanceDesc{}, eye, Vec4(50.0f, 0.0f, 0.0f, 1.0f)).q);
  }

  SUBCASE("a scene and its eye moved by whole cells key alike, and the hash is the placement's") {
    std::mt19937 rng(2026u);
    std::uniform_real_distribution<f32> local(0.0f, 64.0f);
    std::uniform_real_distribution<f32> unit(-1.0f, 1.0f);
    const i32 moves[] = {1, -1, 17, -6548, 156250};
    for (u32 trial = 0; trial < 500; ++trial) {
      InstanceDesc instance;
      for (Vec4& row : instance.rows)
        row = Vec4(unit(rng), unit(rng), unit(rng), local(rng));
      instance.cell = {static_cast<i32>(rng() % 9) - 4, static_cast<i32>(rng() % 3) - 1,
                       static_cast<i32>(rng() % 9) - 4};
      instance.scale_max = 1.0f + 0.5f * (unit(rng) + 1.0f);
      FrameEye eye{};
      eye.cell = {static_cast<i32>(rng() % 5) - 2, 0, static_cast<i32>(rng() % 5) - 2};
      eye.local = Vec3(local(rng), local(rng), local(rng));
      eye.residual = Vec3(unit(rng) * 1.0e-6f, 0.0f, unit(rng) * 1.0e-6f);
      const Vec4 sphere(unit(rng), unit(rng), unit(rng), 0.1f + local(rng) / 8.0f);
      const RtKeepKey key = rt_keep_key(instance, eye, sphere);
      for (const i32 move : moves) {
        InstanceDesc moved = instance;
        FrameEye moved_eye = eye;
        moved.cell.x += move;
        moved.cell.z -= move;
        moved_eye.cell.x += move;
        moved_eye.cell.z -= move;
        CHECK(rt_keep_key(moved, moved_eye, sphere) == key);
      }
      // The record's other words are not the placement: they do not reach the hash.
      InstanceDesc renamed = instance;
      renamed.mesh = 7;
      renamed.first_pair = 123456;
      renamed.material_base = 99;
      CHECK(rt_keep_key(renamed, eye, sphere) == key);
      CHECK(key.h <= 0xffffu);
      CHECK(key.q <= k_rt_keep_q_max);
    }
  }

  SUBCASE("a caster's key is above its drawn key, and drawn first puts every drawn below") {
    const RtKeepKey small{k_rt_keep_q_max, 0xffffu};
    const RtKeepKey large{0, 0};
    for (const bool drawn_first : {false, true}) {
      CHECK(rt_keep_item_key(large, true, drawn_first) >
            rt_keep_item_key(large, false, drawn_first));
      CHECK(rt_keep_item_key(small, true, drawn_first) >
            rt_keep_item_key(small, false, drawn_first));
    }
    CHECK(rt_keep_item_key(small, false, true) < rt_keep_item_key(large, true, true));
    CHECK(rt_keep_item_key(small, false, false) > rt_keep_item_key(large, true, false));
  }
}
