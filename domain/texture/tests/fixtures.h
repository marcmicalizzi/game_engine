#pragma once

// Test images made with integer arithmetic only, so that they are the same bytes on every compiler
// and C library — which is what lets the determinism test pin hashes of what is built from them.

#include <core/base/types.h>
#include <core/containers/vector.h>

namespace engine::texture::test_fixtures {

// A colour image with something of everything an encoder meets: a smooth two-axis gradient over
// most of it, hard-edged tiles, a band of pseudo-random noise (a xorshift, not the C library's
// rand), and an alpha channel that is opaque except in one corner when `alpha` is set.
inline void colour_image(u32 width, u32 height, bool alpha, Vector<u8>& out) {
  out.resize(width * height * 4);
  u32 state = 0x9e3779b9u;
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      u8* t = &out[(y * width + x) * 4];
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      const u32 gx = width > 1 ? x * 255 / (width - 1) : 0;
      const u32 gy = height > 1 ? y * 255 / (height - 1) : 0;
      if (y < height / 4) {
        t[0] = static_cast<u8>(state & 0xff);  // noise band
        t[1] = static_cast<u8>((state >> 8) & 0xff);
        t[2] = static_cast<u8>((state >> 16) & 0xff);
      } else if (x < width / 4) {
        const bool tile = ((x / 5) + (y / 7)) % 2 == 0;  // hard edges
        t[0] = tile ? 230 : 20;
        t[1] = tile ? 40 : 200;
        t[2] = tile ? 90 : 160;
      } else {
        t[0] = static_cast<u8>(gx);
        t[1] = static_cast<u8>(gy);
        t[2] = static_cast<u8>((gx + gy) / 2);
      }
      t[3] = alpha && x < width / 3 && y > height / 2 ? static_cast<u8>((x * 37 + y * 11) & 0xff)
                                                      : 255;
    }
  }
}

// A tangent-space normal map of integer bumps: a grid of domes whose slopes are exact ratios,
// encoded the usual way (x, y, z from -1..1 to 0..255), with z left as the file would have it —
// short of unit here and there, which is what the builder has to fix.
inline void normal_image(u32 width, u32 height, Vector<u8>& out) {
  out.resize(width * height * 4);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      u8* t = &out[(y * width + x) * 4];
      const i32 cx = static_cast<i32>(x % 16) - 8;
      const i32 cy = static_cast<i32>(y % 16) - 8;
      t[0] = static_cast<u8>(128 + cx * 9);
      t[1] = static_cast<u8>(128 + cy * 9);
      t[2] = static_cast<u8>(255 - (cx * cx + cy * cy));
      t[3] = 255;
    }
  }
}

// One channel of data in R (the others copies of it): a ramp with a few steps in it.
inline void gray_image(u32 width, u32 height, Vector<u8>& out) {
  out.resize(width * height * 4);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      u8* t = &out[(y * width + x) * 4];
      const u32 v = ((x + y) * 255 / (width + height)) ^ ((y / 8) % 2 == 0 ? 0u : 0x20u);
      t[0] = t[1] = t[2] = static_cast<u8>(v & 0xff);
      t[3] = 255;
    }
  }
}

}  // namespace engine::texture::test_fixtures
