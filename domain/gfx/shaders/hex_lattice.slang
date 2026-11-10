// Stochastic (hex-tiled) texturing's lattice and blend, shared by the terrain's triplanar layers
// (terrain_material.slang, R96) and the mesh materials' UV plane (material.slang, R131). No
// bindings and no entry points: it computes where three tiles read and how they are weighed, and
// each caller reads its own maps there (docs/subsystems/renderer.md, "Stochastic
// sampling"; docs/subsystems/renderer.md, "Stochastic mesh materials").
//
// The lattice is Heitz and Neyret's (2018) triangle grid in Deliot and Heitz's (2019) skew,
// s = [[2√3, -2], [0, 4]] uv, with 2√3 replaced by 97/28 (0.005% long, so the triangles stay
// equilateral to that) so the skew is rational: the terrain takes a lattice point's integer part
// from the frame's integer cell in 64-bit integers exactly (ADR-0053); a mesh takes it from its own
// UVs, which are the mesh's and never a world position. Either way the caller hands over the skewed
// point's integer part (its low 32 bits: the hash reads no more) and its fraction.
//
// Moved here from terrain_material.slang without changing an operation, so the terrain's pictures
// are the ones they were (the terrain tests pin them).
#ifndef ENGINE_HEX_LATTICE_SLANG
#define ENGINE_HEX_LATTICE_SLANG

// Wellons' lowbias32: integer only, so the CPU reference reproduces it bit for bit.
uint hex_hash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// Three tiles of the lattice at a point of a texture plane.
struct HexTiles {
  float3 weight;  // Burley's exponentiated weights (power 7), summing to one
  float gain;     // 1 / sqrt(sum of squared weights): the variance-preserving operator's scale
  float2 shift[3];  // added to the plane's coordinate: the tile's offset, and its turn about its centre
  float2 turn[3];   // the turn's cosine and sine
};

// The tiles at a skewed point whose integer part is (bx, by) — the low 32 bits of it, wrapping as
// a 64-bit sum's low bits do — and whose fraction is `f`. `seed` separates two lattices that share
// a plane; `rotation` is the largest turn in radians (0: offsets only, so a directed texture keeps
// its direction).
HexTiles hex_tiles(uint bx, uint by, float2 f, uint seed, float rotation) {
  int2 corner[3];
  float3 w;
  const float z = 1.0 - f.x - f.y;
  if (z > 0.0) {
    w = float3(z, f.y, f.x);
    corner[0] = int2(0, 0);
    corner[1] = int2(0, 1);
    corner[2] = int2(1, 0);
  } else {
    w = float3(-z, 1.0 - f.y, 1.0 - f.x);
    corner[0] = int2(1, 1);
    corner[1] = int2(1, 0);
    corner[2] = int2(0, 1);
  }
  // Burley's exponentiated weights (2019, Eq. 5) at Mikkelsen's exponent, 7: below Burley's tile
  // structure (8 and up), and the closest of the blends measured to the image's histogram.
  const float3 w2 = w * w;
  w = w2 * w2 * w2 * w;
  w /= w.x + w.y + w.z;
  HexTiles hex;
  hex.weight = w;
  hex.gain = 1.0 / sqrt(dot(w, w));
  [unroll] for (uint k = 0; k < 3; ++k) {
    const uint h0 = hex_hash((bx + uint(corner[k].x)) ^ hex_hash((by + uint(corner[k].y)) ^ seed));
    const uint h1 = hex_hash(h0);
    const float2 offset = float2(float(h0 >> 8), float(h1 >> 8)) * (1.0 / 16777216.0);
    hex.shift[k] = offset;
    hex.turn[k] = float2(1.0, 0.0);
    if (rotation > 0.0) {
      const float angle = (float(hex_hash(h1) >> 8) * (2.0 / 16777216.0) - 1.0) * rotation;
      const float c = cos(angle);
      const float s = sin(angle);
      // The point from its tile's centre, in the plane's coordinate: s - corner through the skew's
      // inverse.
      const float2 rel = f - float2(corner[k]);
      const float2 r = float2(rel.x * (28.0 / 97.0) + rel.y * (14.0 / 97.0), rel.y * 0.25);
      hex.shift[k] += float2(c * r.x - s * r.y, s * r.x + c * r.y) - r;
      hex.turn[k] = float2(c, s);
    }
  }
  return hex;
}

// The operator itself (Heitz and Neyret 2018, Eq. 9, on the texels rather than a Gaussianized
// copy): mean + gain * sum w (x - mean). For three independent tiles it keeps the texture's mean
// and variance wherever the weights fall.
float3 hex_blend(HexTiles hex, float3 mean, float3 a, float3 b, float3 c) {
  return mean + hex.gain * (hex.weight.x * (a - mean) + hex.weight.y * (b - mean) +
                            hex.weight.z * (c - mean));
}

// A derivative (or any vector of the plane) turned with tile k, as its content is read.
float2 hex_turn(HexTiles hex, uint k, float2 v) {
  const float2 t = hex.turn[k];
  return float2(t.x * v.x - t.y * v.y, t.y * v.x + t.x * v.y);
}

#endif  // ENGINE_HEX_LATTICE_SLANG
