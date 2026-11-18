#pragma once

// **What a frame past its ray tracing chain's capacity keeps** (docs/subsystems/renderer.md, "Past
// the capacity, by size on screen"; ADR-0074; roadmap R174): the CPU mirror of the keep rule the
// records chain runs (`clas_records.slang`), as `geometry::lod_selects` mirrors the cull pass's
// LOD test. Every instance that wants a structure has a **keep key** — its size on screen, `q`,
// then a hash of its placement, `h` — and each of the chain's two capacities (the frame's own
// structures, the cache's references) is spent as a **prefix** of its items' keys: the least key
// at which the structures keyed at or below it pass the capacity is the cut, everything keyed
// below it is kept, and equal keys are kept or dropped together. Nothing reads a place in a list,
// so two orders of the same placements keep the same placements at one capacity.
//
// `rt_keep_key` is the shader's key operation for operation: the sphere's centre through the
// instance's 3x4 by an explicit fixed-order expansion (each product and sum rounded on its own;
// the shader's are `precise`, and the tree compiles with contraction off), measured from the eye
// by `engine::relative_axis`. **The sphere is the GPU's** (`CullParams::mesh_bounds`, written by
// `mesh_bounds_main` through `length()`, which the GPU does not round correctly): pass the bits the
// device wrote, never a sphere computed here.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/cluster_cull.h>

#include <span>

namespace engine::gfx {

inline constexpr u32 k_rt_keep_q_max = 32767;  // q is 15 bits
// q's quantization of u = asuint(d²) − asuint(r²): (u + 8·2²³) >> 13, clamped; 1,024 steps an
// octave of (d/r)², d/r from 1/16 to 4,096.
inline constexpr i32 k_rt_keep_q_offset = 8 << 23;
inline constexpr u32 k_rt_keep_q_shift = 13;
inline constexpr u32 k_rt_keep_seed = 0x9e3779b9u;  // the hash's seed, as `fade_seed`'s
// A histogram level: one word a bin, for each capacity; level 1 over a key's top 16 bits, level 2
// over the crossing bin's `h`. Four of them (two levels, two capacities): 1 MiB.
inline constexpr u32 k_rt_keep_bins = 65536;
inline constexpr u32 k_rt_keep_histograms = 4;
inline constexpr u64 k_rt_keep_histogram_bytes = u64{k_rt_keep_bins} * k_rt_keep_histograms * 4;
inline constexpr u32 k_rt_keep_key_bytes = 8;  // a slot's two words: q, then h and the kept bits
// The cut's words (each capacity's crossing, the room before it, its key and the room left; the
// new passes' dispatches), laid out with the passes in clas_records.slang.
inline constexpr u32 k_rt_keep_cut_words = 16;

// **The keep block** (`ClusterRecordParams::keep`; renderer.md, "Past the capacity, by size on
// screen"): what the cut's passes read and write. One set is shared by the frames in flight; the
// block itself (the eye, written into it every frame), the cut words and the histograms are
// written at the start of every frame's chain by a pass the render graph orders (renderer.md, "The
// cut's tables are cleared every frame"), and the keys are written whole by every frame that cuts
// before anything reads them.
struct ClusterKeepTables {
  FrameEye eye;         // view 0's, the eye a set's views share
  u64 mesh_bounds = 0;  // float4 per mesh, mesh space: CullParams::mesh_bounds, the GPU's sphere; 0
  u64 keys = 0;         // u32[2] per instance slot: q, h (in `instance_counts`)
  // u32[k_rt_keep_bins * k_rt_keep_histograms]: level 1 built, level 1 cached, level 2 built,
  // level 2 cached
  u64 histograms = 0;
  u64 cut = 0;         // u32[k_rt_keep_cut_words]: five words a capacity
  u32 slot_count = 0;  // the keys' length, in instance slots
  u32 pad = 0;
};
static_assert(sizeof(ClusterKeepTables) == 88);

struct RtKeepKey {
  u32 q = 0;  // 15 bits: its size on screen; smaller is larger and kept first
  u32 h = 0;  // 16 bits: its placement's hash, which parts the instances one q holds
  constexpr bool operator==(const RtKeepKey&) const = default;
};

// q from the squared distance and the squared radius: monotone in both, no division, nothing
// transcendental. A NaN distance and a zero radius fall at the top.
u32 rt_keep_q(f32 d2, f32 r2) noexcept;

// h: the high half of lowbias32 over the instance's cell relative to the eye's (x, y, z, each as a
// wrapping 32-bit difference) and the twelve words of its 3x4, row by row, x to w — so a scene
// moved by whole cells with its eye hashes alike, and nothing of the record's place does.
u32 rt_keep_hash(const InstanceDesc& instance, const FrameEye& eye) noexcept;

// The key of an instance of a mesh whose sphere (mesh space; xyz centre, w radius) is `sphere`,
// from the frame's eye.
RtKeepKey rt_keep_key(const InstanceDesc& instance, const FrameEye& eye,
                      const Vec4& sphere) noexcept;

// An item's key. With drawn first (the ray path) every drawn structure goes before any caster;
// otherwise at one size the drawn go before the casters. A caster's key is always above its own
// drawn key, so casters never stand without their drawn structures.
constexpr u32 rt_keep_item_key(RtKeepKey key, bool caster, bool drawn_first) noexcept {
  const u32 kind = caster ? 1u : 0u;
  return drawn_first ? (kind << 31) | (key.q << 16) | key.h : (key.q << 17) | (kind << 16) | key.h;
}

// One instance as the cut sees it: what it wants built of each kind, whether it is cached (its
// structures are references, which spend the references' capacity), and its key.
struct RtKeepItem {
  u32 drawn = 0;
  u32 casters = 0;
  bool cached = false;
  RtKeepKey key;
};

// One capacity's cut: whether anything passes it, the key that does (`K`: every item keyed below
// it is kept), and the room it leaves (the capacity less what is kept).
struct RtKeepCut {
  bool crosses = false;
  u32 key = 0;
  u32 room = 0;
  constexpr bool operator==(const RtKeepCut&) const = default;
};

// The count words the shader writes (`ClusterRecordParams::record_count` 0 to 3, and the cache's
// 4 and 5): what was built and wanted, the instances that lost their drawn structures (and with
// them everything), those that kept them and lost their casters, the references kept and wanted.
struct RtKeepWords {
  u32 built = 0;
  u32 wanted = 0;
  u32 dropped_drawn = 0;
  u32 dropped_casters = 0;
  u32 references_kept = 0;
  u32 references_wanted = 0;
  constexpr bool operator==(const RtKeepWords&) const = default;
};

struct RtKeepResult {
  RtKeepWords words;
  RtKeepCut cuts[2];  // the frame's own structures, the cache's references
};

// The rule: what each instance keeps of its drawn and caster structures (all or none of each;
// `kept_drawn` and `kept_casters` as long as `items`), the count words and the two cuts. A frame
// within both capacities keeps everything. The result is a function of the multiset of items and
// the two capacities: the order of `items` changes only the order of the outputs.
RtKeepResult rt_keep(std::span<const RtKeepItem> items, u32 capacity, u32 reference_capacity,
                     bool drawn_first, std::span<u32> kept_drawn, std::span<u32> kept_casters);

}  // namespace engine::gfx
