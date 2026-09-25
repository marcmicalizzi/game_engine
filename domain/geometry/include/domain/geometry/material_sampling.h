#pragma once

// How a material's slots read their images (docs/subsystems/geometry.md, "How a material samples
// its images"): the glTF sampler of the texture each slot names — wrap in s and t, the
// magnification, minification and mip filters — and the slot's `KHR_texture_transform` (offset,
// rotation, scale). glTF puts the sampler on the *texture* and the transform on the *reference*
// to it, so both are per slot here, and a material that tiles its base colour three times across
// a wall while its normal map is clamped says exactly that.
//
// The types live in `geometry` rather than in `assets` because the `.clusters` container carries
// them (section kind 33, `material_sampling`, one `ClusterFileMaterialSampling` per material) and
// `assets` depends on `geometry`, not the other way round. The importer fills them, the content
// build and engine-view's cache writer write them, and the renderer turns them into bindless
// samplers and the material table's UV transform.
//
// **Zero is the glTF default**: a zero sampler word is repeat in both directions and linear
// filtering throughout, which is what a texture with no sampler gets and what a reader gives a
// container written before the section existed.

#include <core/base/types.h>
#include <core/math/math.h>

namespace engine::geometry {

// The material slots, in the order the per-slot arrays below index them.
inline constexpr u32 k_slot_base_color = 0;
inline constexpr u32 k_slot_metallic_roughness = 1;
inline constexpr u32 k_slot_normal = 2;
inline constexpr u32 k_slot_occlusion = 3;
inline constexpr u32 k_slot_emissive = 4;
inline constexpr u32 k_material_slots = 5;

// glTF `wrapS`/`wrapT`: 10497 REPEAT (the default), 33648 MIRRORED_REPEAT, 33071 CLAMP_TO_EDGE.
enum class TextureWrap : u8 { repeat = 0, mirrored_repeat = 1, clamp_to_edge = 2 };

// One filter decision. glTF's `minFilter` names two of them at once (the filter within a level
// and, for the `*_MIPMAP_*` values, the one between levels); see `TextureSampler::mip`.
enum class TextureFilter : u8 { linear = 0, nearest = 1 };

// A glTF sampler as the engine keeps it. `mip` is how two levels are blended. glTF's `NEAREST`
// and `LINEAR` minification filters ask for no mip chain at all, and the engine does not honour
// that: the content build always makes the whole chain and level 0 alone aliases, so such a
// sampler reads with a linear blend between levels, as an undefined one does.
struct TextureSampler {
  TextureWrap wrap_s = TextureWrap::repeat;
  TextureWrap wrap_t = TextureWrap::repeat;
  TextureFilter mag = TextureFilter::linear;
  TextureFilter min = TextureFilter::linear;
  TextureFilter mip = TextureFilter::linear;

  friend bool operator==(const TextureSampler& a, const TextureSampler& b) noexcept {
    return a.wrap_s == b.wrap_s && a.wrap_t == b.wrap_t && a.mag == b.mag && a.min == b.min &&
           a.mip == b.mip;
  }
  friend bool operator!=(const TextureSampler& a, const TextureSampler& b) noexcept {
    return !(a == b);
  }
};

// `KHR_texture_transform` on one texture reference: `uv' = T(offset) * R(rotation) * S(scale) *
// uv`, the extension's own order, with `rotation` in radians counter-clockwise in UV space (where v
// points down the image). The identity is offset 0, rotation 0, scale 1.
struct TextureTransform {
  Vec2 offset{0.0f, 0.0f};
  f32 rotation = 0.0f;
  Vec2 scale{1.0f, 1.0f};

  bool identity() const noexcept {
    return offset.x == 0.0f && offset.y == 0.0f && rotation == 0.0f && scale.x == 1.0f &&
           scale.y == 1.0f;
  }
  friend bool operator==(const TextureTransform& a, const TextureTransform& b) noexcept {
    return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.rotation == b.rotation &&
           a.scale.x == b.scale.x && a.scale.y == b.scale.y;
  }
  friend bool operator!=(const TextureTransform& a, const TextureTransform& b) noexcept {
    return !(a == b);
  }
};

// Everything about how one slot reads its image other than which image it is.
struct TextureSlotSampling {
  TextureSampler sampler;
  TextureTransform transform;
};

// The sampler as one word: bits 0..1 wrap s, 2..3 wrap t, bit 4 nearest magnification, bit 5
// nearest minification, bit 6 nearest between levels. Zero is the default sampler. `unpack`
// refuses a word with a bit or a wrap value this build does not know, and leaves `out` at the
// default then, which is what a reader falls back to.
u32 pack_texture_sampler(const TextureSampler& sampler) noexcept;
bool unpack_texture_sampler(u32 word, TextureSampler& out) noexcept;

const char* texture_wrap_name(TextureWrap wrap) noexcept;

// ---- the container's record (section kind 33) ---------------------------------------------------

// One slot, 24 bytes: the packed sampler word, then the transform's five floats.
struct ClusterFileSlotSampling {
  u32 sampler = 0;
  f32 offset[2] = {0.0f, 0.0f};
  f32 rotation = 0.0f;
  f32 scale[2] = {1.0f, 1.0f};
};

// One material, 128 bytes, parallel to the `Materials` section. `occlusion_strength` is glTF's
// `occlusionTexture.strength` — `occlusion = 1 + strength * (texel - 1)` — which the 64-byte
// material record had no word left for.
struct ClusterFileMaterialSampling {
  ClusterFileSlotSampling slots[k_material_slots];
  f32 occlusion_strength = 1.0f;
  u32 pad = 0;
};

static_assert(sizeof(ClusterFileSlotSampling) == 24, "a slot's sampling record is 24 bytes");
static_assert(sizeof(ClusterFileMaterialSampling) == 128,
              "a material's sampling record is 128 bytes on the wire");

// The in-memory form to the record and back. `decode` gives a slot whose sampler word it cannot
// read the default sampler (repeat, linear) rather than failing: a newer build's bits are its own
// business, and the default is what a file without the section reads as anyway.
ClusterFileMaterialSampling encode_material_sampling(const TextureSlotSampling* slots,
                                                     f32 occlusion_strength) noexcept;
void decode_material_sampling(const ClusterFileMaterialSampling& record, TextureSlotSampling* slots,
                              f32& occlusion_strength) noexcept;

}  // namespace engine::geometry
