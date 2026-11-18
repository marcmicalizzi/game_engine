#include <core/containers/vector.h>
#include <core/math/world.h>
#include <domain/gfx/rt_keep.h>

#include <algorithm>
#include <bit>

namespace engine::gfx {
namespace {

// lowbias32, as `fade_mix` in shaders/scene.slang.
u32 mix(u32 h) noexcept {
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

// One row of the 3x4 applied to the sphere's centre, then the instance's translation from the eye:
// the shader's expansion, one rounding a line. Contraction is off for every target (ADR-0035), so
// none of these is fused.
f32 centre_axis(const Vec4& row, const Vec4& c, f32 from_eye) noexcept {
  const f32 a = row.x * c.x;
  const f32 b = row.y * c.y;
  const f32 ab = a + b;
  const f32 e = row.z * c.z;
  const f32 abe = ab + e;
  return abe + from_eye;
}

struct Entry {
  u32 key = 0;
  u32 structures = 0;
};

// The cut over one capacity's entries, sorted by key: the first key whose group passes it.
RtKeepCut cut_of(const Vector<Entry>& sorted, u32 capacity) {
  RtKeepCut cut;
  u64 before = 0;
  u32 i = 0;
  while (i < sorted.size()) {
    const u32 key = sorted[i].key;
    u64 group = 0;
    while (i < sorted.size() && sorted[i].key == key) {
      group += sorted[i].structures;
      ++i;
    }
    if (before + group > capacity) {
      cut.crosses = true;
      cut.key = key;
      cut.room = static_cast<u32>(capacity - before);
      return cut;
    }
    before += group;
  }
  cut.room = static_cast<u32>(capacity - before);
  return cut;
}

bool kept_at(const RtKeepCut& cut, u32 key) noexcept { return !cut.crosses || key < cut.key; }

}  // namespace

u32 rt_keep_q(f32 d2, f32 r2) noexcept {
  // Both are non-negative (or a NaN with its sign clear), so their bits are under 2^31 and the
  // difference fits. Clamped before the offset, so nothing overflows (the shader's int would wrap).
  const i32 u = static_cast<i32>(std::bit_cast<u32>(d2)) - static_cast<i32>(std::bit_cast<u32>(r2));
  constexpr i32 k_top =
      static_cast<i32>((k_rt_keep_q_max + 1) << k_rt_keep_q_shift) - k_rt_keep_q_offset;
  if (u < -k_rt_keep_q_offset) return 0;
  if (u >= k_top) return k_rt_keep_q_max;
  return static_cast<u32>(u + k_rt_keep_q_offset) >> k_rt_keep_q_shift;
}

u32 rt_keep_hash(const InstanceDesc& instance, const FrameEye& eye) noexcept {
  u32 h = mix((static_cast<u32>(instance.cell.x) - static_cast<u32>(eye.cell.x)) ^ k_rt_keep_seed);
  h = mix(h ^ (static_cast<u32>(instance.cell.y) - static_cast<u32>(eye.cell.y)));
  h = mix(h ^ (static_cast<u32>(instance.cell.z) - static_cast<u32>(eye.cell.z)));
  for (const Vec4& row : instance.rows) {
    h = mix(h ^ std::bit_cast<u32>(row.x));
    h = mix(h ^ std::bit_cast<u32>(row.y));
    h = mix(h ^ std::bit_cast<u32>(row.z));
    h = mix(h ^ std::bit_cast<u32>(row.w));
  }
  return h >> 16;
}

RtKeepKey rt_keep_key(const InstanceDesc& instance, const FrameEye& eye,
                      const Vec4& sphere) noexcept {
  const f32 from_x =
      relative_axis(instance.cell.x, instance.rows[0].w, eye.cell.x, eye.local.x, eye.residual.x);
  const f32 from_y =
      relative_axis(instance.cell.y, instance.rows[1].w, eye.cell.y, eye.local.y, eye.residual.y);
  const f32 from_z =
      relative_axis(instance.cell.z, instance.rows[2].w, eye.cell.z, eye.local.z, eye.residual.z);
  const f32 x = centre_axis(instance.rows[0], sphere, from_x);
  const f32 y = centre_axis(instance.rows[1], sphere, from_y);
  const f32 z = centre_axis(instance.rows[2], sphere, from_z);
  const f32 xx = x * x;
  const f32 yy = y * y;
  const f32 zz = z * z;
  const f32 xy = xx + yy;
  const f32 d2 = xy + zz;
  const f32 r = sphere.w * instance.scale_max;
  const f32 r2 = r * r;
  return {rt_keep_q(d2, r2), rt_keep_hash(instance, eye)};
}

RtKeepResult rt_keep(std::span<const RtKeepItem> items, u32 capacity, u32 reference_capacity,
                     bool drawn_first, std::span<u32> kept_drawn, std::span<u32> kept_casters) {
  RtKeepResult result;
  Vector<Entry> entries[2];
  u64 wanted[2] = {0, 0};
  for (const RtKeepItem& item : items) {
    const u32 kind = item.cached ? 1u : 0u;
    wanted[kind] += u64{item.drawn} + item.casters;
    if (item.drawn != 0)
      entries[kind].push_back({rt_keep_item_key(item.key, false, drawn_first), item.drawn});
    if (item.casters != 0)
      entries[kind].push_back({rt_keep_item_key(item.key, true, drawn_first), item.casters});
  }
  const u32 capacities[2] = {capacity, reference_capacity};
  for (u32 kind = 0; kind < 2; ++kind) {
    // The order of equal keys does not matter: a group is summed whole.
    std::sort(entries[kind].begin(), entries[kind].end(),
              [](const Entry& a, const Entry& b) { return a.key < b.key; });
    result.cuts[kind] = cut_of(entries[kind], capacities[kind]);
  }
  u64 kept[2] = {0, 0};
  for (usize i = 0; i < items.size(); ++i) {  // spans index by usize
    const RtKeepItem& item = items[i];
    const u32 kind = item.cached ? 1u : 0u;
    const RtKeepCut& cut = result.cuts[kind];
    const bool drawn =
        item.drawn == 0 || kept_at(cut, rt_keep_item_key(item.key, false, drawn_first));
    const bool casters =
        item.casters == 0 || kept_at(cut, rt_keep_item_key(item.key, true, drawn_first));
    kept_drawn[i] = drawn ? item.drawn : 0u;
    kept_casters[i] = drawn && casters ? item.casters : 0u;
    if (!drawn) {
      ++result.words.dropped_drawn;  // its casters with it, counted once
    } else if (!casters) {
      ++result.words.dropped_casters;
    }
    kept[kind] += u64{kept_drawn[i]} + kept_casters[i];
  }
  result.words.built = static_cast<u32>(kept[0]);
  result.words.wanted = static_cast<u32>(wanted[0]);
  result.words.references_kept = static_cast<u32>(kept[1]);
  result.words.references_wanted = static_cast<u32>(wanted[1]);
  return result;
}

}  // namespace engine::gfx
