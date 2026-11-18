// **Indirect light: the guide** (docs/subsystems/renderer.md, "Indirect light", "R171: where a dark
// room's light comes from"; roadmap R171, fifth leg). Mirrors the guide's half of
// domain/gfx/gi_cache.h, which has the layout and the rules: what the samples of a coarse cell have
// learned about where their light comes from, as the share of it through each of `k_gi_guide_bins`
// equal solid angles of the sphere, from which the sample pass draws half its directions.
//
// No bindings and no entry points; only gi_update.slang includes it, so the resolve's modules are
// the bytes they were.

#ifndef ENGINE_GI_GUIDE_SLANG
#define ENGINE_GI_GUIDE_SLANG

#include "gi_cache.slang"

// gfx::k_gi_guide: bit 25 of `ResolveParams::gi_shell_word`.
static const uint k_gi_guide = 1u << 25;
static const uint k_gi_guide_rows = 8u;
static const uint k_gi_guide_cols = 16u;
static const uint k_gi_guide_bins = 128u;
static const uint k_gi_guide_header_words = 8u;
static const uint k_gi_guide_bytes = 800u;
static const uint k_gi_guide_up = 3u;
static const uint k_gi_guide_probe = 4u;
static const uint k_gi_guide_history_frames = 16u;
static const uint k_gi_guide_min_history = 1024u;
static const uint k_gi_guide_trained = 256u;
static const float k_gi_guide_alpha = 0.5;
static const uint k_gi_guide_max_age = 64u;
static const uint k_gi_guide_full_frames = 8u;

// gfx::gi_guide_alpha (R171's sixth leg): a guide's share of the mixture by the samples and the
// folds behind it. None for its first frame, whole at `k_gi_guide_full_frames`.
float gi_guide_alpha(uint trained, uint frames) {
  if (trained < k_gi_guide_trained || frames < 2u) return 0.0;
  const uint f = min(frames, k_gi_guide_full_frames);
  return k_gi_guide_alpha * float(f - 1u) / float(k_gi_guide_full_frames - 1u);
}
static const uint k_gi_no_guide = 0xffffffffu;

// gfx::gi_guide_count, and where the table starts: behind the cells.
uint gi_guide_count(uint count) { return max(count / 256u, 64u); }
uint64_t gi_guides(uint64_t cache, uint count) {
  return gi_cell_words(cache, count) + uint64_t(count) * 32u;
}
uint64_t gi_guide_at(uint64_t cache, uint count, uint guide) {
  return gi_guides(cache, count) + uint64_t(guide) * k_gi_guide_bytes;
}

// gfx::gi_guide_hash.
uint64_t gi_guide_hash(uint64_t cell_hash) { return gi_mix(cell_hash ^ 0x6775696465ULL); }

// The guide holding `h`'s key among the `k_gi_guide_probe` slots from its first, claiming the first
// empty one where none does and `insert` is set; k_gi_no_guide otherwise. As `gi_insert`: every
// thread tries the same slots in the same order, and the loser of a claim reads the winner's key.
uint gi_guide_slot(uint64_t cache, uint count, uint64_t h, bool insert) {
  const uint guides = gi_guide_count(count);
  const uint key = gi_key_of(h);
  const uint first = uint(h & 0xffffffffULL) & (guides - 1u);
  for (uint i = 0u; i < k_gi_guide_probe; ++i) {
    const uint g = (first + i) & (guides - 1u);
    if (*(uint*)gi_guide_at(cache, count, g) == key) return g;
  }
  if (!insert) return k_gi_no_guide;
  for (uint i = 0u; i < k_gi_guide_probe; ++i) {
    const uint g = (first + i) & (guides - 1u);
    Atomic<uint>* word = (Atomic<uint>*)gi_guide_at(cache, count, g);
    const uint previous = word[0].compareExchange(0u, key);
    if (previous == 0u || previous == key) return g;
  }
  return k_gi_no_guide;
}

// gfx::gi_guide_bin.
uint gi_guide_bin(float3 d) {
  const float u = (1.0 - d.y) * 0.5 * float(k_gi_guide_rows);
  float v = atan2(d.z, d.x) * (1.0 / 6.28318530718);
  v = (v - floor(v)) * float(k_gi_guide_cols);
  const uint row = u <= 0.0 ? 0u : (u >= float(k_gi_guide_rows) ? k_gi_guide_rows - 1u : uint(u));
  const uint col = v >= float(k_gi_guide_cols) ? k_gi_guide_cols - 1u : uint(v);
  return row * k_gi_guide_cols + col;
}

// gfx::gi_guide_direction.
float3 gi_guide_direction(uint bin, float2 uv) {
  const float row = float(bin / k_gi_guide_cols);
  const float col = float(bin % k_gi_guide_cols);
  const float y = 1.0 - 2.0 * (row + uv.x) / float(k_gi_guide_rows);
  const float s = sqrt(max(0.0, 1.0 - y * y));
  const float phi = 6.28318530718 * (col + uv.y) / float(k_gi_guide_cols);
  return float3(s * cos(phi), y, s * sin(phi));
}

// A guide's share of bin `bin` (65,535 the largest) and the sum of its shares.
uint gi_guide_share(uint64_t guide, uint bin) {
  uint* words = (uint*)(guide + k_gi_guide_header_words * 4u);
  return (words[bin >> 1] >> ((bin & 1u) * 16u)) & 0xffffu;
}

// The bin a guide draws at `u` of 0..1: the first whose running share passes `u` of the sum. The
// shares are read two to a word.
uint gi_guide_draw(uint64_t guide, uint sum, float u) {
  uint* words = (uint*)(guide + k_gi_guide_header_words * 4u);
  const uint target = min(uint(u * float(sum)), sum - 1u);
  uint running = 0u;
  for (uint w = 0u; w < k_gi_guide_bins / 2u; ++w) {
    const uint pair = words[w];
    const uint low = pair & 0xffffu;
    if (target < running + low) return w * 2u;
    running += low;
    const uint high = pair >> 16;
    if (target < running + high) return w * 2u + 1u;
    running += high;
  }
  return k_gi_guide_bins - 1u;
}

// gfx::gi_guide_pdf and gfx::gi_guide_weight.
float gi_guide_pdf(uint share, uint sum) {
  return sum == 0u ? 0.0 : float(share) / float(sum) * (float(k_gi_guide_bins) / 12.5663706144);
}
float gi_guide_weight(float lobe, float guide, float alpha) {
  const float mixture = (1.0 - alpha) * lobe + alpha * guide;
  return mixture > 0.0 ? lobe / mixture : 0.0;
}

// A sample's contribution into its guide's frame sums: the luminance of the weighted ratio in
// fixed point to the bin of its direction, and the sample counted whether it brought light or not.
void gi_guide_add(uint64_t guide, uint bin, float light, uint stamp) {
  Atomic<uint>* words = (Atomic<uint>*)guide;
  words[1].store(stamp);
  words[2].add(1u);
  const uint fixed = uint(light * k_gi_fixed + 0.5);
  if (fixed != 0u) words[k_gi_guide_header_words + k_gi_guide_bins / 2u + bin].add(fixed);
}

// gfx::gi_guide_fold, a thread a guide; one nothing sampled for `k_gi_guide_max_age` frames is
// freed. `now` is the cache's frame as the stamps count it.
void gi_guide_fold(uint64_t guide, uint now) {
  uint* words = (uint*)guide;
  if (words[0] == 0u) return;
  const uint n = words[2];
  if (n == 0u) {
    if (((now - words[1]) & k_gi_stamp_frames) > k_gi_guide_max_age) {
      for (uint i = 0u; i < k_gi_guide_bytes / 4u; ++i) words[i] = 0u;
    }
    return;
  }
  const uint cap = max(k_gi_guide_min_history, min(n, 0x7ffffffu) * k_gi_guide_history_frames);
  const uint total = min(words[3] + n, cap);
  const float rate = float(n) / float(total);
  const float per_sample = 1.0 / (k_gi_fixed * float(n));
  const float scale = asfloat(words[5]);
  uint* shares = words + k_gi_guide_header_words;
  uint* sums = shares + k_gi_guide_bins / 2u;
  float largest = 0.0;
  for (uint b = 0u; b < k_gi_guide_bins; ++b) {
    const float before =
        float((shares[b >> 1] >> ((b & 1u) * 16u)) & 0xffffu) * (1.0 / 65535.0) * scale;
    largest = max(largest, before + (float(sums[b]) * per_sample - before) * rate);
  }
  uint sum = 0u;
  for (uint w = 0u; w < k_gi_guide_bins / 2u; ++w) {
    uint pair = 0u;
    for (uint k = 0u; k < 2u; ++k) {
      const uint b = w * 2u + k;
      const float before = float((shares[w] >> (k * 16u)) & 0xffffu) * (1.0 / 65535.0) * scale;
      const float estimate = before + (float(sums[b]) * per_sample - before) * rate;
      const uint q = largest > 0.0 ? min(uint(estimate / largest * 65535.0 + 0.5), 65535u) : 0u;
      pair |= q << (k * 16u);
      sum += q;
      sums[b] = 0u;
    }
    shares[w] = pair;
  }
  words[2] = 0u;
  words[3] = total;
  words[4] = sum;
  words[5] = asuint(largest);
  words[6] = min(words[6] + 1u, k_gi_guide_full_frames);
}

#endif
