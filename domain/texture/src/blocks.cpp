// Block compression, the one translation unit that includes the encoders (cmake/
// EngineTexture.cmake): BC7 through bc7enc, BC1/BC3/BC4/BC5 through rgbcx, and the decoders the
// tool and the tests measure with. The settings are fixed here and nowhere else, because they are
// part of what a cache entry means — changing one is a bump of `k_texture_cache_version`.
#include <core/jobs/job_system.h>
#include <domain/texture/texture_build.h>

#include <cstring>

// bc7enc.h has no include guard and declares `color_rgba` at global scope; this is the only file
// that includes it.
#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

namespace engine::texture {

namespace {

// rgbcx's quality level for BC1 and BC3 colour: 18 levels, 0 to 4 competing with stb_dxt and the
// rest with squish and NVTT; 10 is the library's own "if in doubt" (rgbcx.h).
constexpr u32 k_rgbcx_level = 10;
// BC7's uber level, 0 to 4: how many endpoint refinements each candidate mode gets. 1 is the
// measured knee on the samples (docs/subsystems/texture.md, "What the settings cost").
constexpr u32 k_bc7_uber_level = 1;

// Both encoders build lookup tables into globals on first use and say so: not thread safe. Once
// per process, before any block, whatever thread asks first.
// A function-local static is initialized exactly once, and every other caller waits for it.
void init_encoders() {
  static const bool initialized = [] {
    bc7enc_compress_block_init();
    rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
    return true;
  }();
  (void)initialized;
}

// The 4x4 texels of block (bx, by), edge texels repeated where the level ends inside the block —
// what every encoder expects of a partial block, and what a GPU never samples anyway.
void gather_block(const u8* rgba, u32 width, u32 height, u32 bx, u32 by, u8 out[64]) noexcept {
  for (u32 y = 0; y < 4; ++y) {
    u32 sy = by * 4 + y;
    if (sy >= height) sy = height - 1;
    for (u32 x = 0; x < 4; ++x) {
      u32 sx = bx * 4 + x;
      if (sx >= width) sx = width - 1;
      std::memcpy(out + (y * 4 + x) * 4, rgba + (static_cast<usize>(sy) * width + sx) * 4, 4);
    }
  }
}

void encode_block(TextureFormat format, const u8 texels[64],
                  const bc7enc_compress_block_params& bc7, u8* out) {
  switch (format) {
    case TextureFormat::bc1:
      // Three-colour blocks allowed (better for gradients through black), transparent texels for
      // black not: the format is sampled as RGB, and a texel the encoder made transparent would be
      // black there — correct — but the promise that alpha is always one is simpler to keep.
      rgbcx::encode_bc1(k_rgbcx_level, out, texels, true, false);
      break;
    case TextureFormat::bc3: rgbcx::encode_bc3_hq(k_rgbcx_level, out, texels); break;
    case TextureFormat::bc4: rgbcx::encode_bc4_hq(out, texels, 4); break;
    case TextureFormat::bc5: rgbcx::encode_bc5_hq(out, texels, 0, 1, 4); break;
    case TextureFormat::bc7: bc7enc_compress_block(out, texels, &bc7); break;
    case TextureFormat::rgba8: std::memcpy(out, texels, 4); break;
  }
}

}  // namespace

void encode_level(TextureFormat format, std::span<const u8> rgba, u32 width, u32 height,
                  bool perceptual, std::span<u8> out, jobs::JobSystem* pool) {
  if (width == 0 || height == 0) return;
  if (format == TextureFormat::rgba8) {
    const usize bytes = static_cast<usize>(width) * height * 4;
    if (out.size() >= bytes && rgba.size() >= bytes) std::memcpy(out.data(), rgba.data(), bytes);
    return;
  }
  init_encoders();
  bc7enc_compress_block_params bc7;
  bc7enc_compress_block_params_init(&bc7);
  if (!perceptual) bc7enc_compress_block_params_init_linear_weights(&bc7);
  bc7.m_uber_level = k_bc7_uber_level;

  const u32 blocks_x = (width + 3) / 4;
  const u32 blocks_y = (height + 3) / 4;
  const u32 block_bytes = texture_block_bytes(format);
  const u8* texels = rgba.data();
  u8* dst = out.data();
  auto rows = [&](u32 begin, u32 end) {
    u8 block[64];
    for (u32 by = begin; by < end; ++by) {
      for (u32 bx = 0; bx < blocks_x; ++bx) {
        gather_block(texels, width, height, bx, by, block);
        encode_block(format, block, bc7,
                     dst + (static_cast<usize>(by) * blocks_x + bx) * block_bytes);
      }
    }
  };
  // One block row a job at most, so a 2,048-texel level is 512 jobs — enough to spread over any
  // machine's pool — and every block is written by exactly one of them.
  if (pool == nullptr || blocks_y < 2) {
    rows(0u, blocks_y);
  } else {
    pool->parallel_for(jobs::Pool::Performance, blocks_y, 1,
                       [&rows](u32 begin, u32 end) { rows(begin, end); });
  }
}

bool decode_level(TextureFormat format, std::span<const u8> blocks, u32 width, u32 height,
                  Vector<u8>& rgba, std::string* error) {
  rgba.clear();
  if (blocks.size() < texture_level_bytes(format, width, height)) {
    if (error != nullptr) *error = "decode_level: the level's blocks are truncated";
    return false;
  }
  rgba.resize(width * height * 4);
  if (format == TextureFormat::rgba8) {
    std::memcpy(rgba.data(), blocks.data(), static_cast<usize>(width) * height * 4);
    return true;
  }
  init_encoders();
  const u32 blocks_x = (width + 3) / 4;
  const u32 blocks_y = (height + 3) / 4;
  const u32 block_bytes = texture_block_bytes(format);
  u8 texels[64];
  for (u32 by = 0; by < blocks_y; ++by) {
    for (u32 bx = 0; bx < blocks_x; ++bx) {
      const u8* block = blocks.data() + (static_cast<usize>(by) * blocks_x + bx) * block_bytes;
      for (u32 i = 0; i < 16; ++i) {
        texels[i * 4 + 0] = 0;
        texels[i * 4 + 1] = 0;
        texels[i * 4 + 2] = 0;
        texels[i * 4 + 3] = 255;
      }
      switch (format) {
        case TextureFormat::bc1: rgbcx::unpack_bc1(block, texels, true); break;
        case TextureFormat::bc3: rgbcx::unpack_bc3(block, texels); break;
        case TextureFormat::bc4: rgbcx::unpack_bc4(block, texels, 4); break;
        case TextureFormat::bc5: rgbcx::unpack_bc5(block, texels, 0, 1, 4); break;
        case TextureFormat::bc7:
          bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(texels));
          break;
        case TextureFormat::rgba8: break;
      }
      for (u32 y = 0; y < 4; ++y) {
        const u32 ty = by * 4 + y;
        if (ty >= height) break;
        for (u32 x = 0; x < 4; ++x) {
          const u32 tx = bx * 4 + x;
          if (tx >= width) break;
          std::memcpy(rgba.data() + (static_cast<usize>(ty) * width + tx) * 4,
                      texels + (y * 4 + x) * 4, 4);
        }
      }
    }
  }
  return true;
}

}  // namespace engine::texture
