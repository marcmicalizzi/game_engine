// **Indirect light: the world-space cache** (docs/subsystems/renderer.md, "Indirect light: the
// world-space cache"; roadmap R69 stage 1). Mirrors domain/gfx/gi_cache.h, which has the layout
// and the rules; this file is the key, the lookup, the insert and the resolve's read. The passes
// that fill it are gi_update.slang's.
//
// No bindings and no entry points: material.slang includes it for the resolve's ambient
// (`scene_ambient`), and gi_update.slang reaches it through the resolve's own source.

#ifndef ENGINE_GI_CACHE_SLANG
#define ENGINE_GI_CACHE_SLANG

#include "scene.slang"

static const uint k_gi_cell_words = 8u;
static const uint k_gi_bucket = 8u;
static const uint k_gi_levels = 9u;
static const int64_t k_gi_cell_min_mm = 25;
static const float k_gi_cell_min_m = 0.025;
static const float k_gi_fixed = 1024.0;  // 2^k_gi_fraction_bits
static const float k_gi_ratio_max = 15.99;
static const uint k_gi_max_samples = 256u;
static const uint k_gi_max_age = 240u;
static const uint k_gi_no_slot = 0xffffffffu;
// gfx::k_gi_dispatch_pitch: the groups a row of a pass's two-dimensional dispatch.
static const uint k_gi_dispatch_pitch = 32768u;

static const uint k_gi_read = 1u;
static const uint k_gi_bounce = 2u;
static const uint k_gi_reset = 4u;

float gi_luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// gfx::gi_level: the smallest level whose cell is at least `distance * scale` wide.
uint gi_level(float distance, float scale) {
  const float size = max(distance * scale, k_gi_cell_min_m);
  uint level = 0u;
  while (level + 1u < k_gi_levels && size > k_gi_cell_min_m * float(1u << level)) ++level;
  return level;
}

int64_t gi_cell_mm(uint level) { return k_gi_cell_min_mm << level; }

// gfx::gi_face: the normal's dominant axis and its sign.
uint gi_face(float3 n) {
  const float3 a = abs(n);
  if (a.x >= a.y && a.x >= a.z) return n.x >= 0.0 ? 0u : 1u;
  if (a.y >= a.z) return n.y >= 0.0 ? 2u : 3u;
  return n.z >= 0.0 ? 4u : 5u;
}

// gfx::gi_axis_mm: one axis of a point `from_eye` metres from the frame's eye, in whole millimetres
// of the world. The eye's offset in its cell plus the point's offset from the eye is a float the
// size of the distance from the eye, never of the world; the cell is added in 64-bit integers.
int64_t gi_axis_mm(int eye_cell, float eye_local, float from_eye) {
  precise const float local = eye_local + from_eye;
  precise const float mm = floor(local * 1000.0 + 0.5);
  return int64_t(eye_cell) * 64000 + int64_t(mm);
}

int64_t3 gi_point_mm(FrameEye eye, float3 from_eye) {
  return int64_t3(gi_axis_mm(eye.cell.x, eye.local.x, from_eye.x),
                  gi_axis_mm(eye.cell.y, eye.local.y, from_eye.y),
                  gi_axis_mm(eye.cell.z, eye.local.z, from_eye.z));
}

uint64_t gi_mix(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

// gfx::gi_hash.
uint64_t gi_hash(int64_t3 c, uint level, uint face) {
  uint64_t h = gi_mix(uint64_t(c.x) ^ (uint64_t(level * 6u + face) << 58));
  h = gi_mix(h ^ uint64_t(c.y));
  return gi_mix(h ^ uint64_t(c.z));
}

uint gi_key_of(uint64_t h) { return uint(h >> 32) | 1u; }
uint gi_bucket_of(uint64_t h, uint count) {
  return uint(h & 0xffffffffULL) & (count - 1u) & ~(k_gi_bucket - 1u);
}

// The slot holding `h`'s key, or k_gi_no_slot. The whole bucket is scanned, so a slot the fold
// freed in the middle of it breaks nothing.
uint gi_find(uint64_t cells, uint count, uint64_t h) {
  const uint bucket = gi_bucket_of(h, count);
  const uint key = gi_key_of(h);
  uint* words = (uint*)cells;
  for (uint i = 0u; i < k_gi_bucket; ++i) {
    if (words[(bucket + i) * k_gi_cell_words] == key) return bucket + i;
  }
  return k_gi_no_slot;
}

// The slot holding `h`'s key, claiming the bucket's first empty one where none does; k_gi_no_slot
// when the bucket is full of other keys (the sample is dropped, and the read falls back). Two
// threads inserting one key claim the same slot: each tries the empty slots in the same order, and
// the loser's compare-exchange returns the winner's key.
uint gi_insert(uint64_t cells, uint count, uint64_t h) {
  const uint found = gi_find(cells, count, h);
  if (found != k_gi_no_slot) return found;
  const uint bucket = gi_bucket_of(h, count);
  const uint key = gi_key_of(h);
  Atomic<uint>* words = (Atomic<uint>*)cells;
  for (uint i = 0u; i < k_gi_bucket; ++i) {
    const uint previous = words[(bucket + i) * k_gi_cell_words].compareExchange(0u, key);
    if (previous == 0u || previous == key) return bucket + i;
  }
  return k_gi_no_slot;
}

// A cell's mean ratio, and how many samples it averages (0: nothing yet).
float3 gi_mean(uint64_t cells, uint slot, out uint samples) {
  uint* words = (uint*)cells + slot * k_gi_cell_words;
  const uint rg = words[6];
  const uint b = words[7];
  samples = b >> 16;
  return float3(f16tof32(rg & 0xffffu), f16tof32(rg >> 16), f16tof32(b & 0xffffu));
}

// The cache's ratio at a point: trilinear over the eight cells of `level` round it whose face is
// the normal's, each weighted by its share of the point and only where it has samples; at the next
// coarser level where none of the eight has. False where neither level has any: the caller keeps
// the harmonics.
bool gi_read(uint64_t cells, uint count, FrameEye eye, float3 from_eye, uint level, uint face,
             out float3 ratio) {
  ratio = float3(0.0);
  const int64_t3 mm = gi_point_mm(eye, from_eye);
  for (uint l = level; l < min(level + 2u, k_gi_levels); ++l) {
    const int64_t size = gi_cell_mm(l);
    const int64_t3 shifted = mm - int64_t3(size / 2);
    const int64_t3 base = int64_t3(terrain_floor_div(shifted.x, size),
                                   terrain_floor_div(shifted.y, size),
                                   terrain_floor_div(shifted.z, size));
    const float3 f = float3(shifted - base * size) / float(size);
    float3 sum = float3(0.0);
    float weight = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner) {
      const int64_t3 o = int64_t3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
      const float3 w3 = lerp(1.0 - f, f, float3(o));
      const float w = w3.x * w3.y * w3.z;
      if (w <= 0.0) continue;
      const uint slot = gi_find(cells, count, gi_hash(base + o, l, face));
      if (slot == k_gi_no_slot) continue;
      uint samples;
      const float3 mean = gi_mean(cells, slot, samples);
      if (samples == 0u) continue;
      sum += mean * w;
      weight += w;
    }
    if (weight > 1e-4) {
      ratio = sum / weight;
      return true;
    }
  }
  return false;
}

#endif  // ENGINE_GI_CACHE_SLANG
