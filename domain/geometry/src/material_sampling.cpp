// How a material's slots read their images (material_sampling.h).
#include <domain/geometry/material_sampling.h>

namespace engine::geometry {

namespace {

constexpr u32 k_nearest_mag = 1u << 4;
constexpr u32 k_nearest_min = 1u << 5;
constexpr u32 k_nearest_mip = 1u << 6;
constexpr u32 k_known_bits = 0x3fu | k_nearest_mag | k_nearest_min | k_nearest_mip;

bool wrap_of(u32 bits, TextureWrap& out) noexcept {
  switch (bits) {
    case 0: out = TextureWrap::repeat; return true;
    case 1: out = TextureWrap::mirrored_repeat; return true;
    case 2: out = TextureWrap::clamp_to_edge; return true;
    default: return false;
  }
}

}  // namespace

u32 pack_texture_sampler(const TextureSampler& sampler) noexcept {
  u32 word = static_cast<u32>(sampler.wrap_s) | (static_cast<u32>(sampler.wrap_t) << 2);
  if (sampler.mag == TextureFilter::nearest) word |= k_nearest_mag;
  if (sampler.min == TextureFilter::nearest) word |= k_nearest_min;
  if (sampler.mip == TextureFilter::nearest) word |= k_nearest_mip;
  return word;
}

bool unpack_texture_sampler(u32 word, TextureSampler& out) noexcept {
  out = TextureSampler{};
  if ((word & ~k_known_bits) != 0) return false;
  TextureSampler sampler;
  if (!wrap_of(word & 3u, sampler.wrap_s) || !wrap_of((word >> 2) & 3u, sampler.wrap_t))
    return false;
  sampler.mag = (word & k_nearest_mag) != 0 ? TextureFilter::nearest : TextureFilter::linear;
  sampler.min = (word & k_nearest_min) != 0 ? TextureFilter::nearest : TextureFilter::linear;
  sampler.mip = (word & k_nearest_mip) != 0 ? TextureFilter::nearest : TextureFilter::linear;
  out = sampler;
  return true;
}

const char* texture_wrap_name(TextureWrap wrap) noexcept {
  switch (wrap) {
    case TextureWrap::repeat: return "repeat";
    case TextureWrap::mirrored_repeat: return "mirrored_repeat";
    case TextureWrap::clamp_to_edge: return "clamp_to_edge";
  }
  return "?";
}

ClusterFileMaterialSampling encode_material_sampling(const TextureSlotSampling* slots,
                                                     f32 occlusion_strength) noexcept {
  ClusterFileMaterialSampling record;
  for (u32 s = 0; s < k_material_slots; ++s) {
    ClusterFileSlotSampling& out = record.slots[s];
    const TextureSlotSampling& in = slots[s];
    out.sampler = pack_texture_sampler(in.sampler);
    out.offset[0] = in.transform.offset.x;
    out.offset[1] = in.transform.offset.y;
    out.rotation = in.transform.rotation;
    out.scale[0] = in.transform.scale.x;
    out.scale[1] = in.transform.scale.y;
  }
  record.occlusion_strength = occlusion_strength;
  return record;
}

void decode_material_sampling(const ClusterFileMaterialSampling& record, TextureSlotSampling* slots,
                              f32& occlusion_strength) noexcept {
  for (u32 s = 0; s < k_material_slots; ++s) {
    const ClusterFileSlotSampling& in = record.slots[s];
    TextureSlotSampling& out = slots[s];
    if (!unpack_texture_sampler(in.sampler, out.sampler)) out.sampler = TextureSampler{};
    out.transform.offset = Vec2{in.offset[0], in.offset[1]};
    out.transform.rotation = in.rotation;
    out.transform.scale = Vec2{in.scale[0], in.scale[1]};
  }
  occlusion_strength = record.occlusion_strength;
}

}  // namespace engine::geometry
