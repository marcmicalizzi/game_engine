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
// gfx::k_gi_crown_history_frames, gfx::k_gi_stamp_crown (stage 8): a foliage cell remembers this
// many frames, and its stamp's top bit says it is one (the fold has a cell's words and not its
// key's tag).
static const uint k_gi_crown_history_frames = 64u;
static const uint k_gi_stamp_crown = 0x80000000u;
static const uint k_gi_no_slot = 0xffffffffu;
// gfx::k_gi_dispatch_pitch: the groups a row of a pass's two-dimensional dispatch.
static const uint k_gi_dispatch_pitch = 32768u;

static const uint k_gi_read = 1u;
static const uint k_gi_bounce = 2u;
// gfx::k_gi_show: the resolve's read takes one level whole, the own plus two bits at
// `k_gi_show_shift` (stage 6's diagnostic, `renderer.gi.show_level`).
static const uint k_gi_show = 4u;
static const uint k_gi_show_shift = 6u;
static const uint k_gi_faces = 8u;
static const uint k_gi_young_rays = 16u;
static const uint k_gi_hit_fallback = 32u;
// gfx::k_gi_reveal: a level is taken by what its difference from the next one up shows (stage 6).
static const uint k_gi_reveal = 1u << 23;
// gfx::gi_flag_numbers: the samples a cell is read whole at, and the evict age.
uint gi_confident(uint flags) { return max((flags >> 8) & 0xffu, 1u); }
uint gi_evict_age(uint flags) { return (flags >> 16) & 0x7fu; }
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

// gfx::k_gi_face_crown: the face of a foliage sample's cells (stage 7): none of the six. A crown's
// leaves face every way at every depth, so a face in their key splits their samples six ways and
// describes nothing; what a leaf's own facing does to its light is in its prior.
static const uint k_gi_face_crown = 6u;

// gfx::gi_hash. A crown's cells take the tags past the six faces' (9 levels of 6 faces end at 53).
uint64_t gi_hash(int64_t3 c, uint level, uint face) {
  const uint tag = face == k_gi_face_crown ? 54u + level : level * 6u + face;
  uint64_t h = gi_mix(uint64_t(c.x) ^ (uint64_t(tag) << 58));
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
// **Stage 6: what a difference between two levels shows** (gfx::gi_reveal, the same arithmetic;
// renderer.md, "Indirect light", "Stage 6"): the share a level of mean `m` over `n` samples takes
// of a read whose next level up has mean `m_up` over `n_up`, by their difference alone. A sample's
// variance is taken as `k_gi_sample_cv2` times the square of the larger mean; the difference's
// square less what that noise explains of it is the detail between the two levels, and the share
// is that detail over itself plus the finer mean's own noise — the shrinkage weight of a noisy
// mean towards a prior of known spread, the spread estimated by its positive part. Nothing where
// the levels agree within their noise; and where it is something, the noise the level adds to the
// read is under the difference it shows.
static const float k_gi_sample_cv2 = 2.0;
float gi_reveal(float m, float n, float m_up, float n_up) {
  const float top = max(m, m_up);
  const float s2 = k_gi_sample_cv2 * top * top;
  const float d = m - m_up;
  const float d2 = max(d * d - s2 * (1.0 / n + 1.0 / n_up), 0.0);
  const float den = d2 + s2 / n;
  return den > 0.0 ? d2 / den : 0.0;
}
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
  float weight;  // the trilinear weights of the cells that hold a sample (stage 6's cover)
};

GiLevelRead gi_read_level(uint64_t cache, uint count, int64_t3 mm, uint l, uint face,
                          uint confident) {
  GiLevelRead out;
  out.sum = float3(0.0);
  out.confidence = 0.0;
  out.square = 0.0;
  out.spread = 0.0;
  out.weight = 0.0;
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
    out.weight += w;
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
//
// **Stage 6** (`reveal`, `k_gi_reveal`): a level its count does not read whole waits for the next
// one up, and takes what their difference shows (`gi_reveal`) where that is more than its count
// gives it — by the weight of the cells that are there where they hold under an eighth of it, so a
// far corner's mean is not shown as the point's. No level is read that the count's walk did not
// read: a level that is not whole always sent the walk on to the next.
float gi_read_face(uint64_t cache, uint count, int64_t3 mm, uint level, uint levels, uint face,
                   uint confident, bool reveal, out float3 acc) {
  acc = float3(0.0);
  float remaining = 1.0;
  const uint last = min(level + levels, k_gi_levels);
  GiLevelRead held;
  held.sum = float3(0.0);
  held.confidence = 0.0;
  held.square = 0.0;
  held.spread = 0.0;
  held.weight = 0.0;
  bool have = false;
  for (uint l = level; l < last; ++l) {
    const GiLevelRead r = gi_read_level(cache, count, mm, l, face, confident);
    if (!(r.confidence > 1e-6)) continue;
    if (have) {
      // The level below, now that the one above it is known: its share by its count, or — where
      // the two differ by more than their samples' noise explains — by what the difference shows.
      const float shown = gi_reveal(gi_luminance(held.sum) / held.confidence,
                                    held.square / held.spread,
                                    gi_luminance(r.sum) / r.confidence, r.square / r.spread) *
                          min(k_gi_read_cover * held.weight, 1.0);
      const float take = max(min(held.square / (held.spread * float(confident)), 1.0) *
                                 min(k_gi_read_cover * held.confidence, 1.0),
                             shown);
      acc += (remaining * take / held.confidence) * held.sum;
      remaining *= 1.0 - take;
      have = false;
      if (remaining <= k_gi_read_stop) break;
    }
    // The samples a cell of the level's mean holds over `confident`, and — where the cells that are there
    // hold under an eighth of the weight between them, which a point's own cell alone holds once
    // it is read whole — that share of it: a mean extrapolated from a far corner is not taken
    // whole, so the point's own cell arriving beside it is no step.
    const float take = min(r.square / (r.spread * float(confident)), 1.0) *
                       min(k_gi_read_cover * r.confidence, 1.0);
    // A level that takes all that is left by its own count needs no look at the one above it.
    // Nor does one after which the count's walk would stop: the reveal reads no level the count's
    // walk does not.
    if (!reveal || remaining * (1.0 - take) <= k_gi_read_stop) {
      acc += (remaining * take / r.confidence) * r.sum;
      remaining *= 1.0 - take;
      if (remaining <= k_gi_read_stop) break;
      continue;
    }
    held = r;
    have = true;
  }
  if (have) {
    const float take = min(held.square / (held.spread * float(confident)), 1.0) *
                       min(k_gi_read_cover * held.confidence, 1.0);
    acc += (remaining * take / held.confidence) * held.sum;
    remaining *= 1.0 - take;
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
//
// `crown` (stage 7): the point is foliage with a prior, and `level` and `levels` are its walk on
// the crown's own cells (`k_gi_face_crown`) — one face, so eight lookups a level, and no blend.
float gi_read(uint64_t cache, uint count, uint flags, FrameEye eye, float3 from_eye, uint level,
              uint levels, float3 normal, out float3 ratio, bool crown = false) {
  ratio = float3(0.0);
  const int64_t3 mm = gi_point_mm(eye, from_eye);
  float3 first;
  const uint confident = gi_confident(flags);
  const bool reveal = (flags & k_gi_reveal) != 0u;
  if (crown) {
    // The counts are in sixteenths of a sample (`k_gi_crown_weight`), and there is no reveal: the
    // detail between a crown's levels is its prior's, not its cells'.
    const float taken_crown =
        gi_read_face(cache, count, mm, level, levels, k_gi_face_crown,
                     min(confident * k_gi_crown_weight, 0xffffu), false, first);
    if (!(taken_crown > 0.0)) return 0.0;
    ratio = first / taken_crown;
    return taken_crown;
  }
  const float taken_first =
      gi_read_face(cache, count, mm, level, levels, gi_face(normal), confident, reveal, first);
  float second_size;
  float largest;
  const uint second_face = gi_second_face(normal, second_size, largest);
  const float w = (flags & k_gi_faces) != 0u ? gi_face_blend(largest, second_size) : 0.0;
  float3 second = float3(0.0);
  float taken_second = 0.0;
  if (w > 0.0) {
    taken_second =
        gi_read_face(cache, count, mm, level, levels, second_face, confident, reveal, second);
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

// **Stage 7: a crown's shade as a prior** (gfx::CrownPriorDesc, crown_prior.h; renderer.md,
// "Indirect light", "Stage 7"). A mesh with a crown has a small grid over its own bounds: the mean
// fraction of diffuse light its own leaves let through from the upper hemisphere and from the
// lower one, at each cell's centre (Beer-Lambert in the leaf area the mesh's leaves were gathered
// into). A foliage sample divides what its ray brought back by its own prior before the cells take
// it, and a foliage pixel multiplies its own back: what the cells hold for foliage is the light
// outside the crown's own shade, which is smooth, so a coarse cell holds it well, fills in a frame
// and is read alone.
struct CrownPrior {
  float3 origin;    // mesh space: the centre of the grid's first cell
  uint offset;      // bytes from the table's start to the grid; k_no_crown: none
  float3 inv_cell;  // cells a metre
  uint dims;        // x | y << 8 | z << 16
};
static const uint k_no_crown = 0xffffffffu;
// gfx::k_crown_floor: the prior is never read below this (a sample divides by it).
static const float k_gi_crown_floor = 1.0 / 16.0;
// The levels above a foliage surface's own that its samples are added to (the own level's
// leaf-sized cells never fill, and nothing reads them), and the one its read starts at: the field
// is smooth, so the read takes the level two up — sixteen times the own cell's samples, and every
// facing's together — and the one above only while that is young.
static const uint k_gi_crown_first = 1u;
static const uint k_gi_crown_read = 2u;
static const uint k_gi_crown_read_levels = 2u;
// **A crown's cell holds a ratio of sums** (gfx::k_gi_crown_weight). What a cell must hold for
// foliage is the light over the prior; the mean of each sample's own quotient would do it, and
// multiplies a deep sample's noise by up to sixteen (the first build: the still picture's change
// between frames half as large again) and the cell's total light by whatever the samples' light
// that does not follow the prior adds. So a foliage sample adds its light **undivided** and its
// prior to the cell's count, in sixteenths: the cell's mean is then the sum of its samples'
// light over the sum of their priors — the ratio estimator, whose noise is the samples' own and
// which gives back, over the pixels that read it with their own priors, exactly the light the
// samples brought. A crown's cell therefore counts sixteen for a sample in the open; its reads'
// counts (the samples a level is read whole at, the young test) are in the same sixteenths.
static const uint k_gi_crown_weight = 16u;
uint gi_crown_weight(float crown) {
  return uint(clamp(crown * float(k_gi_crown_weight) + 0.5, 1.0, float(k_gi_crown_weight)));
}

// One cell's two means (gfx::crown_prior_sample reads the same bytes): the upper hemisphere's in
// the low byte of a pair, the lower one's in the high.
float2 gi_crown_cell(uint64_t grid, uint3 dims, uint3 c) {
  const uint i = (c.z * dims.y + c.y) * dims.x + c.x;
  const uint word = ((uint*)grid)[i >> 1];
  const uint pair = (word >> ((i & 1u) * 16u)) & 0xffffu;
  return float2(float(pair & 0xffu), float(pair >> 8)) * (1.0 / 255.0);
}

// The prior of an alpha-tested surface of `instance` at `position` (from the frame's eye) with
// normal `normal`: the grid's two means at the point in mesh space, trilinear between the cells'
// centres and held at the grid's edge, weighted by the share of the surface's hemisphere that is
// above the horizon (gfx::crown_prior), never under the floor. A negative number where the frame
// has no table (`renderer.gi.crown_prior` off) or the mesh no record (one that arrived after the
// load): the surface keeps its faces.
//
// **A mesh of low leaves reads one cell of no shade** (stage 8; the renderer's
// `build_crown_priors`, renderer.md "Stage 8"): a sward, a shrub and a sapling are foliage too —
// their cards face every way at every depth, so a face in their cells' key splits their samples
// as it split a crown's, their own level's cells never fill, and their walk read two to four
// levels on two faces for good. Their record is a grid of one cell that lets everything
// through, so this function returns one for them with no rule of its own, and they read and
// fill the crown's cells with no shade but what their own solvers already paint. (Pictures of
// leaves — an impostor, a form's merged cards — have no record: this lookup on every one of
// their pixels cost more than their one-level walk.)
float gi_crown_prior(uint64_t table, uint count, InstanceDesc instance, FrameEye eye,
                     float3 position, float3 normal) {
  if (table == 0 || instance.mesh >= count) return -1.0;
  const CrownPrior crown = ((CrownPrior*)table)[instance.mesh];
  if (crown.offset == k_no_crown) return -1.0;
  // Into mesh space: the inverse of the instance's linear part, by its rows' cross products.
  const float3 d = position - instance_from_eye(instance, eye);
  const float3 r0 = instance.rows[0].xyz;
  const float3 r1 = instance.rows[1].xyz;
  const float3 r2 = instance.rows[2].xyz;
  const float3 c0 = cross(r1, r2);
  const float det = dot(r0, c0);
  if (!(abs(det) > 1e-30)) return -1.0;
  const float3 p = (c0 * d.x + cross(r2, r0) * d.y + cross(r0, r1) * d.z) / det;
  const uint3 dims = uint3(crown.dims & 0xffu, (crown.dims >> 8) & 0xffu, (crown.dims >> 16) & 0xffu);
  const float3 at = clamp((p - crown.origin) * crown.inv_cell, float3(0.0), float3(dims - 1u));
  const uint3 base = uint3(at);
  const uint3 next = min(base + 1u, dims - 1u);
  const float3 f = at - float3(base);
  const uint64_t grid = table + uint64_t(crown.offset);
  const float2 x00 = lerp(gi_crown_cell(grid, dims, uint3(base.x, base.y, base.z)),
                          gi_crown_cell(grid, dims, uint3(next.x, base.y, base.z)), f.x);
  const float2 x10 = lerp(gi_crown_cell(grid, dims, uint3(base.x, next.y, base.z)),
                          gi_crown_cell(grid, dims, uint3(next.x, next.y, base.z)), f.x);
  const float2 x01 = lerp(gi_crown_cell(grid, dims, uint3(base.x, base.y, next.z)),
                          gi_crown_cell(grid, dims, uint3(next.x, base.y, next.z)), f.x);
  const float2 x11 = lerp(gi_crown_cell(grid, dims, uint3(base.x, next.y, next.z)),
                          gi_crown_cell(grid, dims, uint3(next.x, next.y, next.z)), f.x);
  const float2 means = lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
  return max(lerp(means.y, means.x, 0.5 + 0.5 * normal.y), k_gi_crown_floor);
}

// **Stage 4: the shell's directions** (gfx::gi_shell_base, gfx::gi_shell_columns; renderer.md,
// "Indirect light", "Stage 4"). Direction `index` of `count` is a cell of an equal-area grid on
// the sphere, `columns` of longitude by enough rows of the height to hold `count`, jittered inside
// its cell by a hash of the cache's frame count and the index — never of a position, so a scene
// moved by whole cells traces the same directions.
static const uint k_gi_stat_shell = 9u;
static const uint k_gi_stat_shell_hits = 10u;
static const uint k_gi_stat_shell_near = 11u;
// gfx::k_gi_stat_shell_kept (stage 8): the directions that met a surface whose cells are whole,
// and stopped at the eye's ray.
static const uint k_gi_stat_shell_kept = 12u;
// gfx::k_gi_shell_first (`ResolveParams::gi_shell_word`): the directions are the sample pass's
// first threads.
static const uint k_gi_shell_first = 1u << 24;
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
