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
// gfx::k_gi_history_frames: a cell's history is 256 samples or this many frames' samples, the
// longer.
static const uint k_gi_history_frames = 16u;
static const uint k_gi_no_slot = 0xffffffffu;
// gfx::k_gi_dispatch_pitch: the groups a row of a pass's two-dimensional dispatch.
static const uint k_gi_dispatch_pitch = 32768u;

static const uint k_gi_read = 1u;
static const uint k_gi_bounce = 2u;
static const uint k_gi_reset = 4u;
static const uint k_gi_faces = 8u;
static const uint k_gi_young_rays = 16u;
static const uint k_gi_hit_fallback = 32u;
// gfx::gi_flag_numbers: the samples a cell is read whole at, and the evict age.
uint gi_confident(uint flags) { return max((flags >> 8) & 0xffu, 1u); }
uint gi_evict_age(uint flags) { return (flags >> 16) & 0xffu; }
float gi_far_lift(uint flags) { return float(flags >> 24) / 255.0; }

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

// **The buffer** (gi_cache.h): the 64-byte header, the keys (one u32 a slot, so a bucket's eight
// are one 32-byte block), then the cells (eight words a slot).
static const uint64_t k_gi_header_bytes = 64;
static const uint k_gi_stat_rays = 0u;
static const uint k_gi_stat_samples = 1u;
static const uint k_gi_stat_young = 2u;
static const uint k_gi_stat_dropped = 3u;
static const uint k_gi_stat_cells = 4u;
static const uint k_gi_stat_young_cells = 5u;
static const uint k_gi_stat_evicted = 6u;
static const uint k_gi_stat_expired = 7u;
// The header's ninth word: the frame's young claims on the ray budget; a young candidate whose
// claim comes at or past the half of `ResolveParams::gi_rays` the trickle leaves takes no ray.
static const uint k_gi_ray_count = 8u;
static const uint k_gi_confident = 255u;
static const uint k_gi_young = 8u;
static const uint k_gi_evict_age = 16u;
static const float k_gi_face_blend = 0.6;
static const float k_gi_open_near_m = 1.0;
static const float k_gi_open_far_m = 10.0;

uint64_t gi_keys(uint64_t cache) { return cache + k_gi_header_bytes; }
uint64_t gi_cell_words(uint64_t cache, uint count) {
  return cache + k_gi_header_bytes + uint64_t(count) * 4u;
}

// The slot holding `h`'s key, or k_gi_no_slot: the bucket's eight keys in two 16-byte loads of
// one 32-byte block. The whole bucket is scanned, so a slot the fold freed in the middle of it
// breaks nothing.
uint gi_find(uint64_t cache, uint count, uint64_t h) {
  const uint bucket = gi_bucket_of(h, count);
  const uint key = gi_key_of(h);
  uint4* keys = (uint4*)(gi_keys(cache) + uint64_t(bucket) * 4u);
  const uint4 a = keys[0];
  const uint4 b = keys[1];
  if (a.x == key) return bucket;
  if (a.y == key) return bucket + 1u;
  if (a.z == key) return bucket + 2u;
  if (a.w == key) return bucket + 3u;
  if (b.x == key) return bucket + 4u;
  if (b.y == key) return bucket + 5u;
  if (b.z == key) return bucket + 6u;
  if (b.w == key) return bucket + 7u;
  return k_gi_no_slot;
}

// The slot holding `h`'s key, claiming the bucket's first empty one where none does; k_gi_no_slot
// when the bucket is full of other keys (the sample is dropped, the bucket marked as under
// pressure for the fold, and the read falls back). Two threads inserting one key claim the same
// slot: each tries the empty slots in the same order, and the loser's compare-exchange returns the
// winner's key.
uint gi_insert(uint64_t cache, uint count, uint frame, uint64_t h) {
  const uint found = gi_find(cache, count, h);
  if (found != k_gi_no_slot) return found;
  const uint bucket = gi_bucket_of(h, count);
  const uint key = gi_key_of(h);
  Atomic<uint>* keys = (Atomic<uint>*)gi_keys(cache);
  for (uint i = 0u; i < k_gi_bucket; ++i) {
    const uint previous = keys[bucket + i].compareExchange(0u, key);
    if (previous == 0u || previous == key) return bucket + i;
  }
  uint* words = (uint*)gi_cell_words(cache, count);
  words[bucket * k_gi_cell_words] = frame;
  Atomic<uint>* header = (Atomic<uint>*)cache;
  header[k_gi_stat_dropped].add(1u);
  return k_gi_no_slot;
}

// A cell's mean ratio, and how many samples it averages (0: nothing yet).
float3 gi_mean(uint64_t cache, uint count, uint slot, out uint samples) {
  uint2* words = (uint2*)(gi_cell_words(cache, count) + uint64_t(slot) * 32u + 24u);
  const uint2 m = words[0];
  samples = m.y >> 16;
  return float3(f16tof32(m.x & 0xffffu), f16tof32(m.x >> 16), f16tof32(m.y & 0xffffu));
}

// The samples a cell averages, 0 for no cell.
uint gi_samples(uint64_t cache, uint count, uint slot) {
  if (slot == k_gi_no_slot) return 0u;
  uint* words = (uint*)(gi_cell_words(cache, count) + uint64_t(slot) * 32u);
  return words[7] >> 16;
}

// **Stage 5: the ladder** (gfx::k_gi_ladder; renderer.md, "Indirect light", "Stage 5"). Every
// sample is added to its surface's own level and the three coarser, and every read walks the same
// four, finest first. A surface's cells are a quarter as many a level up, so a cell `l` levels up
// takes about 4^l times the samples of the own cell: the last of the four about 700 in the one
// frame a surface is first seen at 3840x2160, where its own cell takes 11. So there is a level
// whose mean is already quiet on the first frame, and the read shows that one and sharpens below
// it as the finer cells fill, in place of showing a fine cell's first samples.
static const uint k_gi_ladder = 4u;
// A walk stops once this share or less is left to the coarser levels (gfx::k_gi_read_stop): a
// converged neighbourhood reads one level's eight cells.
static const float k_gi_read_stop = 1.0 / 32.0;
// Below this much confidence in all the levels together the read leans on the caller's prior (the
// harmonics for a pixel, the bounded fallback for a bounce's hit), in proportion
// (gfx::k_gi_read_floor): a surface whose coarsest cell holds a handful of samples is not shown
// their mean whole.
static const float k_gi_read_floor = 1.0 / 16.0;
// The shares a level's cells must hold between them for its mean to be taken by its samples
// alone: an eighth, the least trilinear weight of the cell a point is in (gfx::k_gi_read_cover is
// its inverse).
static const float k_gi_read_cover = 8.0;

// One level's read round a point (gfx::GiLevelRead): over the eight cells round it on `face`, with
// each cell's share `s` its trilinear weight times its confidence `min(samples / confident, 1)`:
// the shares times the cells' means, the shares, the shares squared, and the shares squared over
// the cells' samples. The level's mean is `sum / confidence`, and `square / spread` is **the
// samples a cell of that mean holds** — the cells' counts averaged as the mean's variance weighs
// them (harmonically, by the shares squared): a cell's own count at its centre, and between eight
// cells of eleven samples, eleven. The level takes that count over `confident` of the read, at
// most all, so a level sharpens at one rate everywhere and its noise is never more than a mean of
// `confident` samples' (it is less wherever several cells share the point). A cell enters every
// sum continuously from its first sample, so nothing steps when a cell appears; a corner the
// surface never passes through (the layer above a floor) has no cell and counts for nothing, where
// counting it as unknown would send half of every floor's read to the coarser level for good.
struct GiLevelRead {
  float3 sum;
  float confidence;
  float square;
  float spread;
};

GiLevelRead gi_read_level(uint64_t cache, uint count, int64_t3 mm, uint l, uint face,
                          uint confident) {
  GiLevelRead out;
  out.sum = float3(0.0);
  out.confidence = 0.0;
  out.square = 0.0;
  out.spread = 0.0;
  const int64_t size = gi_cell_mm(l);
  const int64_t3 shifted = mm - int64_t3(size / 2);
  const int64_t3 base = int64_t3(terrain_floor_div(shifted.x, size),
                                 terrain_floor_div(shifted.y, size),
                                 terrain_floor_div(shifted.z, size));
  const float3 f = float3(shifted - base * size) / float(size);
  for (uint corner = 0u; corner < 8u; ++corner) {
    const int64_t3 o = int64_t3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
    const float3 w3 = lerp(1.0 - f, f, float3(o));
    const float w = w3.x * w3.y * w3.z;
    if (w <= 0.0) continue;
    const uint slot = gi_find(cache, count, gi_hash(base + o, l, face));
    if (slot == k_gi_no_slot) continue;
    uint samples;
    const float3 mean = gi_mean(cache, count, slot, samples);
    if (samples == 0u) continue;
    const float c = w * min(float(samples) / float(confident), 1.0);
    out.sum += mean * c;
    out.confidence += c;
    out.square += c * c;
    out.spread += c * c / float(samples);
  }
  return out;
}

// **The walk up the ladder** (gfx::gi_blend_levels, the same arithmetic): finest first, each level
// takes its share — the samples a cell of its mean holds over `confident`, at most all — of what the finer
// ones left and passes the rest up; the walk ends when `k_gi_read_stop` or less is left. So what a
// level adds to the read never carries more noise than a mean of `confident` samples would, however
// few its cells hold: with m samples a cell it takes m / confident of the read, and its noise is
// at most a sample's spread times sqrt(m) / confident. `acc` is what the levels took times their
// means and the return what they took in all, 0 to 1: the caller divides, and leans on its prior
// below `k_gi_read_floor`. Every term is continuous in every cell's sample count, so neither a
// cell's first samples nor a level's hand-over steps in time; in space each level is trilinear, so
// it does not step between cells either.
float gi_read_face(uint64_t cache, uint count, int64_t3 mm, uint level, uint levels, uint face,
                   uint confident, out float3 acc) {
  acc = float3(0.0);
  float remaining = 1.0;
  const uint last = min(level + levels, k_gi_levels);
  for (uint l = level; l < last; ++l) {
    const GiLevelRead r = gi_read_level(cache, count, mm, l, face, confident);
    if (!(r.confidence > 1e-6)) continue;
    // The samples a cell of the level's mean holds over `confident`, and — where the cells that are there
    // hold under an eighth of the weight between them, which a point's own cell alone holds once
    // it is read whole — that share of it: a mean extrapolated from a far corner is not taken
    // whole, so the point's own cell arriving beside it is no step.
    const float take = min(r.square / (r.spread * float(confident)), 1.0) *
                       min(k_gi_read_cover * r.confidence, 1.0);
    acc += (remaining * take / r.confidence) * r.sum;
    remaining *= 1.0 - take;
    if (remaining <= k_gi_read_stop) break;
  }
  return 1.0 - remaining;
}

// The face blend's weight for the second face (gfx::gi_face_blend).
float gi_face_blend(float largest, float second) {
  if (!(largest > 0.0)) return 0.0;
  const float t = saturate((second / largest - k_gi_face_blend) / (1.0 - k_gi_face_blend));
  return 0.5 * t * t * (3.0 - 2.0 * t);
}

// The second face of a normal: the axis of its second-largest component, with its sign, and that
// component's size.
uint gi_second_face(float3 n, out float second, out float largest) {
  const float3 a = abs(n);
  const uint first = gi_face(n) >> 1;
  uint axis;
  if (first == 0u) axis = a.y >= a.z ? 1u : 2u;
  else if (first == 1u) axis = a.x >= a.z ? 0u : 2u;
  else axis = a.x >= a.y ? 0u : 1u;
  largest = first == 0u ? a.x : (first == 1u ? a.y : a.z);
  second = axis == 0u ? a.x : (axis == 1u ? a.y : a.z);
  const float component = axis == 0u ? n.x : (axis == 1u ? n.y : n.z);
  return axis * 2u + (component >= 0.0 ? 0u : 1u);
}

// The cache's ratio at a point: the walk up `levels` levels from the point's own on the normal's
// face and — where the normal is within the blend of a face's edge (rounded rocks, trunks) — the
// next face's too, weighted by the normal's components, so the read does not step where the
// dominant axis changes. Returns the confidence the levels took in all (`gi_read_face`), 0 where no
// level of either face has a sample: the caller keeps its prior there, and leans on it below
// `k_gi_read_floor` (`gi_confidence_weight`).
float gi_read(uint64_t cache, uint count, uint flags, FrameEye eye, float3 from_eye, uint level,
              uint levels, float3 normal, out float3 ratio) {
  ratio = float3(0.0);
  const int64_t3 mm = gi_point_mm(eye, from_eye);
  float3 first;
  const uint confident = gi_confident(flags);
  const float taken_first =
      gi_read_face(cache, count, mm, level, levels, gi_face(normal), confident, first);
  float second_size;
  float largest;
  const uint second_face = gi_second_face(normal, second_size, largest);
  const float w = (flags & k_gi_faces) != 0u ? gi_face_blend(largest, second_size) : 0.0;
  float3 second = float3(0.0);
  float taken_second = 0.0;
  if (w > 0.0) {
    taken_second = gi_read_face(cache, count, mm, level, levels, second_face, confident, second);
  }
  // Each face's share by the blend's weight and by what its levels took, so a face with nothing
  // leaves the other whole and one that is young comes in as it fills.
  const float3 acc = lerp(first, second, w);
  const float taken = lerp(taken_first, taken_second, w);
  if (!(taken > 0.0)) return 0.0;
  ratio = acc / taken;
  return taken;
}

// How much of a read's value stands against the caller's prior (gfx::gi_confidence_weight): all of
// it from `k_gi_read_floor` of confidence up, in proportion below.
float gi_confidence_weight(float taken) { return saturate(taken / k_gi_read_floor); }

// The openness of a bounce's ray `t` metres long (gfx::gi_openness).
float gi_openness(float t) {
  const float x = saturate((t - k_gi_open_near_m) / (k_gi_open_far_m - k_gi_open_near_m));
  return x * x * (3.0 - 2.0 * x);
}

// **Stage 4: the shell's directions** (gfx::gi_shell_base, gfx::gi_shell_columns; renderer.md,
// "Indirect light", "Stage 4"). Direction `index` of `count` is a cell of an equal-area grid on
// the sphere, `columns` of longitude by enough rows of the height to hold `count`, jittered inside
// its cell by a hash of the cache's frame count and the index — never of a position, so a scene
// moved by whole cells traces the same directions.
static const uint k_gi_stat_shell = 9u;
static const uint k_gi_stat_shell_hits = 10u;
static const uint k_gi_stat_shell_near = 11u;
static const float k_gi_shell_near_m = 50.0;
uint gi_shell_hash(uint v) {
  v = v * 747796405u + 2891336453u;
  const uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
  return (w >> 22u) ^ w;
}
float3 gi_shell_direction(uint index, uint count, uint columns, uint frame) {
  const uint c = max(columns, 1u);
  const uint rows = (count + c - 1u) / c;
  const uint h = gi_shell_hash(index ^ gi_shell_hash(frame));
  const float ju = float(h & 0xffffu) / 65536.0;
  const float jv = float(h >> 16) / 65536.0;
  const float u = (float(index % c) + ju) / float(c);
  const float v = (float(index / c) + jv) / float(rows);
  const float y = 1.0 - 2.0 * v;
  const float r = sqrt(max(1.0 - y * y, 0.0));
  const float phi = 6.28318530718 * u;
  return float3(r * cos(phi), y, r * sin(phi));
}
#endif  // ENGINE_GI_CACHE_SLANG
