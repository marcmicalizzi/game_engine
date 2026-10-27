// Optional native surface evaluator. Included only in MATERIAL_TERRAIN shader modules; ordinary
// scenes retain their existing shaders. Layout: gfx::k_material_terrain_layers (112-byte entries).
#ifndef ENGINE_TERRAIN_MATERIAL_SLANG
#define ENGINE_TERRAIN_MATERIAL_SLANG

static const uint k_material_terrain_layers = 512u;
static const uint k_material_terrain_map = 1024u;
static const uint k_material_terrain_height = 2048u;
static const uint k_material_terrain_stochastic = 4096u;
static const uint k_terrain_masked = 1u;
static const uint k_terrain_no_mask_tile = 0xFFFFFFFFu;

// gfx::TerrainRecord (visibility_resolve.h), one 112-byte table entry the root names by
// `uv_offset.z`. Scalars only, so its layout is the C++ one word for word.
struct TerrainRecord {
  uint first_layer;
  uint layer_count;
  float detail_near_m;
  float detail_far_m;
  int map_x0_mm;
  int map_z0_mm;
  float map_per_mm;
  uint flags;
  uint2 mask;  // device address, low word first
  int mask_tile_x0;
  int mask_tile_z0;
  uint mask_tiles_x;
  uint mask_tiles_z;
  uint mask_spacing_mm;
  uint mask_cells;
  uint mask_words;
  uint mask_mips;
  float wetness;
  float height_blend;
  uint pad0;
  uint pad1;
  uint pad2;
  uint pad3;
  uint pad4;
  uint pad5;
  uint pad6;
  uint pad7;
};

// The root's one terrain word (gfx::terrain_record_index).
uint terrain_record_index(ResolveMaterial root) { return asuint(root.uv_offset.z); }
// A layer entry's terrain fields (gfx::set_terrain_layer).
float3 terrain_layer_mean_colour(ResolveMaterial layer) { return layer.uv_transform.xyz; }
float terrain_layer_mean_roughness(ResolveMaterial layer) { return layer.uv_transform.w; }
float terrain_layer_weight(ResolveMaterial layer) { return layer.uv_offset.x; }
float terrain_layer_porosity(ResolveMaterial layer) { return layer.uv_offset.y; }
float terrain_layer_pitch_mm(ResolveMaterial layer) { return layer.uv_scale; }

// floor(a / b) for b > 0 on magnitudes: the negative-i64 `%` probe (native-terrain-materials.md)
// is why no signed division or remainder of a negative number appears here.
int64_t terrain_magnitude_floor_div(int64_t a, int64_t b) {
  if (a >= 0) return int64_t(uint64_t(a) / uint64_t(b));
  return -int64_t((uint64_t(-a) + uint64_t(b) - 1u) / uint64_t(b));
}

// One axis of a point in the mask level: its tile, its mip-0 point in the tile, and the fraction
// (scene_gen::mask_tile_of), from the frame's integer cell and its local millimetres.
void terrain_mask_axis(int cell, float local_mm, int64_t spacing, int64_t cells, out int64_t tile,
                       out uint point, out float frac) {
  const float whole_local = floor(local_mm);
  const int64_t mm = int64_t(cell) * int64_t(64000) + int64_t(whole_local);
  int64_t q = terrain_magnitude_floor_div(mm, spacing);
  float t = (float(mm - q * spacing) + (local_mm - whole_local)) / float(spacing);
  if (t >= 1.0) {
    q += 1;
    t -= 1.0;
  }
  tile = terrain_magnitude_floor_div(q, cells);
  point = uint(q - tile * cells);
  frac = t;
}

struct TerrainMaskSample {
  uint table;      // four palette indices, 0xFF for none
  float4 weights;  // in the table's order
  float wetness;
  bool valid;
};

float4 terrain_unpack4(uint w) {
  return float4(float(w & 0xFFu), float((w >> 8) & 0xFFu), float((w >> 16) & 0xFFu),
                float(w >> 24)) * (1.0 / 255.0);
}

// The mask level at a frame-local point, at mip `level` (its fraction blends two mips): the one
// tile whose cell holds the point (scene_gen::mask_sample, the CPU's formula), so every geometry
// level, tile and cut that draws the point reads the same weights.
TerrainMaskSample terrain_mask_sample(TerrainRecord rec, FrameEye eye, float3 point, float level) {
  TerrainMaskSample s;
  s.table = 0xFFFFFFFFu;
  s.weights = float4(0.0);
  s.wetness = 0.0;
  s.valid = false;
  const int64_t spacing = int64_t(rec.mask_spacing_mm);
  const int64_t cells = int64_t(rec.mask_cells);
  int64_t tx, tz;
  uint px, pz;
  float fx, fz;
  terrain_mask_axis(eye.cell.x, (eye.local.x + eye.residual.x + point.x) * 1000.0, spacing, cells,
                    tx, px, fx);
  terrain_mask_axis(eye.cell.z, (eye.local.z + eye.residual.z + point.z) * 1000.0, spacing, cells,
                    tz, pz, fz);
  const int64_t rx = tx - int64_t(rec.mask_tile_x0);
  const int64_t rz = tz - int64_t(rec.mask_tile_z0);
  if (rx < 0 || rz < 0 || rx >= int64_t(rec.mask_tiles_x) || rz >= int64_t(rec.mask_tiles_z))
    return s;
  const uint64_t base = uint64_t(rec.mask.x) | (uint64_t(rec.mask.y) << 32);
  uint* pages = (uint*)base;
  const uint tiles = rec.mask_tiles_x * rec.mask_tiles_z;
  const uint slot = pages[uint(rz) * rec.mask_tiles_x + uint(rx)];
  if (slot == k_terrain_no_mask_tile) return s;
  uint* words = pages + tiles + slot * rec.mask_words;
  s.table = words[0];
  s.valid = true;
  uint points = 0;
  for (uint m = 0; m < rec.mask_mips; ++m) points += ((rec.mask_cells >> m) + 1) * ((rec.mask_cells >> m) + 1);
  const float lv = max(level, 0.0);
  const uint m0 = min(uint(lv), rec.mask_mips - 1);
  const uint m1 = min(m0 + 1, rec.mask_mips - 1);
  const float blend = m0 == rec.mask_mips - 1 ? 0.0 : lv - float(m0);
  uint offset = 0;
  for (uint m = 0; m < m1 + 1; ++m) {
    const uint side = (rec.mask_cells >> m) + 1;
    if (m == m0 || m == m1) {
      const float share = (m == m0 ? 1.0 - blend : 0.0) + (m == m1 && m1 != m0 ? blend : 0.0);
      if (share > 0.0) {
        const float scale = 1.0 / float(1u << m);
        const float ux = (float(px) + fx) * scale;
        const float uz = (float(pz) + fz) * scale;
        const uint cx = min(uint(ux), side - 2);
        const uint cz = min(uint(uz), side - 2);
        const float ax = ux - float(cx);
        const float az = uz - float(cz);
        const uint i00 = offset + cz * side + cx;
        const uint i10 = i00 + 1;
        const uint i01 = i00 + side;
        const uint i11 = i01 + 1;
        const float q00 = (1 - ax) * (1 - az), q10 = ax * (1 - az), q01 = (1 - ax) * az, q11 = ax * az;
        s.weights += share * (q00 * terrain_unpack4(words[1 + i00]) + q10 * terrain_unpack4(words[1 + i10]) +
                              q01 * terrain_unpack4(words[1 + i01]) + q11 * terrain_unpack4(words[1 + i11]));
        const uint wet00 = (words[1 + points + i00 / 4] >> (8 * (i00 % 4))) & 0xFFu;
        const uint wet10 = (words[1 + points + i10 / 4] >> (8 * (i10 % 4))) & 0xFFu;
        const uint wet01 = (words[1 + points + i01 / 4] >> (8 * (i01 % 4))) & 0xFFu;
        const uint wet11 = (words[1 + points + i11 / 4] >> (8 * (i11 % 4))) & 0xFFu;
        s.wetness += share * (q00 * float(wet00) + q10 * float(wet10) + q01 * float(wet01) +
                              q11 * float(wet11)) * (1.0 / 255.0);
      }
    }
    offset += side * side;
  }
  return s;
}

// A repeat phase from the frame's integer cell, never an absolute floating world position.
// The millimetre pitch is shared by every tile and mip. Take the unsigned magnitude's remainder
// and apply the sign explicitly: the negative-i64 probe exposed unsigned residue from `%` on the
// development compiler/device combination (native-terrain-materials.md). Local derivatives stay
// unwrapped, so crossing a repeat never selects a coarse mip.
float terrain_phase(int cell, float local, float from_eye, float residual, int64_t pitch) {
  const int64_t mm = int64_t(cell) * int64_t(64000);
  const uint64_t magnitude = uint64_t(mm < 0 ? -mm : mm);
  const uint64_t remainder = magnitude % uint64_t(pitch);
  const float signed_phase = (mm < 0 ? -float(remainder) : float(remainder)) / float(pitch);
  return frac(signed_phase + (local + residual + from_eye) * (1000.0 / float(pitch)));
}

float3 terrain_phase3(FrameEye eye, float3 point, float pitch_mm) {
  const int64_t pitch = int64_t(pitch_mm);
  return float3(terrain_phase(eye.cell.x, eye.local.x, point.x, eye.residual.x, pitch),
                terrain_phase(eye.cell.y, eye.local.y, point.y, eye.residual.y, pitch),
                terrain_phase(eye.cell.z, eye.local.z, point.z, eye.residual.z, pitch));
}

// Project the map's normal slopes onto the surface tangent plane. Flat maps contribute zero,
// including on oblique and negative-facing surfaces; each projection supplies its own axes.
float3 terrain_project_slope(float3 v, float scale, float3 n, float3 t, float3 b) {
  const float3 slope = (t * v.x + b * v.y) * (scale / max(v.z, 0.01));
  return slope - n * dot(n, slope);
}

float3 terrain_slope(ResolveMaterial layer, float2 uv, float2 dx, float2 dy, float3 n,
                     float3 t, float3 b, bool filtered) {
  if (layer.normal_texture == k_no_texture || layer.normal_scale == 0.0) return float3(0.0);
  const uint slot = slot_sampler(layer, layer.samplers_mr_normal, 16);
  float4 texel;
  if (filtered && (layer.flags & k_material_mipped) != 0)
    texel = g_textures[NonUniformResourceIndex(layer.normal_texture)].SampleGrad(
        g_samplers[NonUniformResourceIndex(slot)], uv, dx, dy);
  else
    texel = g_textures[NonUniformResourceIndex(layer.normal_texture)].SampleLevel(
        g_samplers[NonUniformResourceIndex(slot)], uv, 0);
  const float3 v = normal_map_vector(layer, texel);
  return terrain_project_slope(v, layer.normal_scale, n, t, b);
}


// ---- Stochastic (hex-tiled) sampling of a triplanar layer (R96) ---------------------------------
// A layer naming `"sampling": "stochastic"` reads each projection at three tiles of a triangle
// lattice, each tile its own random offset (and, when the layer allows it, a bounded turn), and
// blends them by the variance-preserving operator (native-terrain-materials.md, "Stochastic
// sampling"). Every map of the layer — colour, roughness, height, normal — reads the same three
// places with the same weights, so the maps stay coordinated.
//
// The lattice is Heitz and Neyret's (2018) triangle grid in Deliot and Heitz's (2019) skew,
// s = [[2√3, -2], [0, 4]] uv, with 2√3 replaced by 97/28 (0.005% long, so the triangles stay
// equilateral to that) so the skew is rational: a lattice point's integer part then comes from
// the frame's integer cell in 64-bit integers exactly, and only the frame-local rest is a float
// (ADR-0053), as in `terrain_phase`. No absolute world position is formed.
static const float k_terrain_hex_axis_floor = 1.0 / 128.0;  // a projection below this is skipped

float terrain_layer_rotation(ResolveMaterial layer) { return layer.uv_offset.z; }
float terrain_layer_mean_height(ResolveMaterial layer) { return layer.uv_offset.w; }

// Wellons' lowbias32: integer only, so the CPU reference reproduces it bit for bit.
uint terrain_hash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// Three tiles of the lattice at a point of one projection's plane.
struct TerrainHex {
  float3 weight;  // Burley's exponentiated weights (power 7), summing to one
  float gain;     // 1 / sqrt(sum of squared weights): the variance-preserving operator's scale
  float2 shift[3];  // added to the layer's phase: the tile's offset, and its turn about its centre
  float2 turn[3];   // the turn's cosine and sine
};

// `axis` is the projection (0 reads (z, y), 1 (x, z), 2 (x, y), the phases' own pairs);
// `rotation` the layer's largest turn in radians (0: offsets only, so a directed texture keeps its
// direction).
TerrainHex terrain_hex(FrameEye eye, float3 point, uint axis, float pitch_mm, float rotation) {
  const float3 rest = (eye.local + eye.residual + point) * 1000.0;
  const int cu = axis == 0 ? eye.cell.z : eye.cell.x;
  const int cv = axis == 1 ? eye.cell.z : eye.cell.y;
  const float fu = axis == 0 ? rest.z : rest.x;
  const float fv = axis == 1 ? rest.z : rest.y;
  const int64_t pitch = int64_t(pitch_mm);
  const int64_t den = 28 * pitch;
  const int64_t iu = int64_t(cu) * int64_t(64000);
  const int64_t iv = int64_t(cv) * int64_t(64000);
  const int64_t nx = 97 * iu - 56 * iv;
  const int64_t ny = 112 * iv;
  const int64_t qx = terrain_magnitude_floor_div(nx, den);
  const int64_t qy = terrain_magnitude_floor_div(ny, den);
  const float fden = float(den);
  const float2 t = float2(float(nx - qx * den) + (97.0 * fu - 56.0 * fv),
                          float(ny - qy * den) + 112.0 * fv) / fden;
  const float2 whole = floor(t);
  const float2 f = t - whole;
  const int64_t bx = qx + int64_t(whole.x);
  const int64_t by = qy + int64_t(whole.y);
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
  TerrainHex hex;
  hex.weight = w;
  hex.gain = 1.0 / sqrt(dot(w, w));
  const uint seed = terrain_hash(uint(pitch) ^ 0x9e3779b9u);
  [unroll] for (uint k = 0; k < 3; ++k) {
    const uint h0 = terrain_hash(uint(bx + int64_t(corner[k].x)) ^
                                 terrain_hash(uint(by + int64_t(corner[k].y)) ^ seed));
    const uint h1 = terrain_hash(h0);
    const float2 offset = float2(float(h0 >> 8), float(h1 >> 8)) * (1.0 / 16777216.0);
    hex.shift[k] = offset;
    hex.turn[k] = float2(1.0, 0.0);
    if (rotation > 0.0) {
      const float angle =
          (float(terrain_hash(h1) >> 8) * (2.0 / 16777216.0) - 1.0) * rotation;
      const float c = cos(angle);
      const float s = sin(angle);
      // The point from its tile's centre, in the layer's uv: s - corner through the skew's inverse.
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
float3 terrain_hex_blend(TerrainHex hex, float3 mean, float3 a, float3 b, float3 c) {
  return mean + hex.gain * (hex.weight.x * (a - mean) + hex.weight.y * (b - mean) +
                            hex.weight.z * (c - mean));
}

// One projection of a stochastic layer: every map read at the hex's three places with the same
// weights (the maps stay coordinated), each place's UV derivatives turned with its tile so the
// mip and the anisotropy are the footprint's (Deliot and Heitz 2019: SampleGrad, never the
// derivative of the jittered UV). The colour, roughness and height blend about the layer's own
// means; the slopes about zero, the mean slope of any tileable height field.
void terrain_hex_sample(ResolveMaterial layer, TerrainHex hex, float2 uv, float2 du, float2 dv,
                        bool filtered, out float3 colour, out float roughness, out float height,
                        out float2 slope) {
  const bool normals = layer.normal_texture != k_no_texture && layer.normal_scale != 0.0;
  const bool grads = filtered && (layer.flags & k_material_mipped) != 0;
  const uint normal_slot = slot_sampler(layer, layer.samplers_mr_normal, 16);
  float3 c[3];
  float r[3];
  float h[3];
  float2 d[3];
  [unroll] for (uint k = 0; k < 3; ++k) {
    const float2 turn = hex.turn[k];
    const float2 p = uv + hex.shift[k];
    const float2 du_k = float2(turn.x * du.x - turn.y * du.y, turn.y * du.x + turn.x * du.y);
    const float2 dv_k = float2(turn.x * dv.x - turn.y * dv.y, turn.y * dv.x + turn.x * dv.y);
    const MaterialSample s =
        filtered ? sample_material_grad(layer, p, du_k, dv_k) : sample_material(layer, p);
    c[k] = s.albedo;
    r[k] = s.roughness;
    h[k] = s.metallic;
    d[k] = float2(0.0);
    if (normals) {
      float4 texel;
      if (grads)
        texel = g_textures[NonUniformResourceIndex(layer.normal_texture)].SampleGrad(
            g_samplers[NonUniformResourceIndex(normal_slot)], p, du_k, dv_k);
      else
        texel = g_textures[NonUniformResourceIndex(layer.normal_texture)].SampleLevel(
            g_samplers[NonUniformResourceIndex(normal_slot)], p, 0);
      const float3 v = normal_map_vector(layer, texel);
      const float2 q = v.xy / max(v.z, 0.01);
      // The tile reads its content turned by R; the map's y is the image's up, opposite the UV's v,
      // so the stored slope pair turns by R too (Mikkelsen 2022, section 3, in this map's frame).
      d[k] = float2(turn.x * q.x - turn.y * q.y, turn.y * q.x + turn.x * q.y);
    }
  }
  colour = saturate(terrain_hex_blend(hex, terrain_layer_mean_colour(layer), c[0], c[1], c[2]));
  roughness = saturate(terrain_hex_blend(hex, float3(terrain_layer_mean_roughness(layer)),
                                         float3(r[0]), float3(r[1]), float3(r[2])).x);
  height = saturate(terrain_hex_blend(hex, float3(terrain_layer_mean_height(layer)), float3(h[0]),
                                      float3(h[1]), float3(h[2])).x);
  slope = hex.gain * (hex.weight.x * d[0] + hex.weight.y * d[1] + hex.weight.z * d[2]);
}
// One layer at a point: its colour, roughness, normal slope and height (0.5 without a height map),
// projected as it asks, faded to its means past the detail distances.
struct TerrainLayerSample {
  float3 colour;
  float roughness;
  float3 bump;
  float height;
};

TerrainLayerSample terrain_layer_sample(SceneGeometry geo, ResolveMaterial layer, float2 map_uv,
                                        float2 map_dx, float2 map_dy, float3 point, float3 n,
                                        float3 axes, float3 dp_dx, float3 dp_dy, float fade,
                                        bool filtered) {
  TerrainLayerSample r;
  r.colour = float3(0.0);
  r.roughness = 0.0;
  r.bump = float3(0.0);
  r.height = 0.0;
  const bool heights = (layer.flags & k_material_terrain_height) != 0;
  const bool mapped = (layer.flags & k_material_terrain_map) != 0;
  if (mapped) {
    const MaterialSample sample = filtered ? sample_material_grad(layer, map_uv, map_dx, map_dy)
                                           : sample_material(layer, map_uv);
    r.colour = sample.albedo;
    r.roughness = sample.roughness;
    r.height = sample.metallic;
    r.bump = terrain_slope(layer, map_uv, map_dx, map_dy, n, float3(1, 0, 0), float3(0, 0, 1),
                           filtered);
  } else if ((layer.flags & k_material_terrain_stochastic) != 0) {
    // R96: no fade. The lattice leaves no repeat for distance to reveal, so the far ground keeps
    // the texture's own variation at the mip its footprint selects instead of the layer's mean.
    // A projection whose blend share is under 1/128 is skipped and the rest renormalized: a flat
    // floor reads one projection's three tiles, the nine reads a tiled layer always makes.
    const float pitch = terrain_layer_pitch_mm(layer);
    const float3 phase = terrain_phase3(geo.eye, point, pitch);
    const float scale = 1000.0 / pitch;
    const float3 dx = dp_dx * scale;
    const float3 dy = dp_dy * scale;
    float3 kept = axes * step(k_terrain_hex_axis_floor, axes);
    kept /= kept.x + kept.y + kept.z;
    [unroll] for (uint axis = 0; axis < 3; ++axis) {
      if (kept[axis] <= 0.0) continue;
      const float2 uv = axis == 0 ? phase.zy : (axis == 1 ? phase.xz : phase.xy);
      const float2 du = axis == 0 ? dx.zy : (axis == 1 ? dx.xz : dx.xy);
      const float2 dv = axis == 0 ? dy.zy : (axis == 1 ? dy.xz : dy.xy);
      const float3 t = axis == 0 ? float3(0, 0, 1) : float3(1, 0, 0);
      const float3 b = axis == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
      const TerrainHex hex = terrain_hex(geo.eye, point, axis, pitch, terrain_layer_rotation(layer));
      float3 colour;
      float roughness, height;
      float2 slope;
      terrain_hex_sample(layer, hex, uv, du, dv, filtered, colour, roughness, height, slope);
      r.colour += kept[axis] * colour;
      r.roughness += kept[axis] * roughness;
      r.height += kept[axis] * height;
      if (layer.normal_texture != k_no_texture && layer.normal_scale != 0.0)
        r.bump += kept[axis] * terrain_project_slope(float3(slope, 1.0), layer.normal_scale, n, t, b);
    }
  } else if (fade <= 0.0) {
    r.colour = terrain_layer_mean_colour(layer);
    r.roughness = terrain_layer_mean_roughness(layer);
    r.height = 0.5;
  } else {
    const float pitch = terrain_layer_pitch_mm(layer);
    const float3 phase = terrain_phase3(geo.eye, point, pitch);
    const float scale = 1000.0 / pitch;
    const float3 dx = dp_dx * scale;
    const float3 dy = dp_dy * scale;
    [unroll] for (uint axis = 0; axis < 3; ++axis) {
      const float2 uv = axis == 0 ? phase.zy : (axis == 1 ? phase.xz : phase.xy);
      const float2 du = axis == 0 ? dx.zy : (axis == 1 ? dx.xz : dx.xy);
      const float2 dv = axis == 0 ? dy.zy : (axis == 1 ? dy.xz : dy.xy);
      const float3 t = axis == 0 ? float3(0, 0, 1) : float3(1, 0, 0);
      const float3 b = axis == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
      const MaterialSample sample = filtered ? sample_material_grad(layer, uv, du, dv)
                                             : sample_material(layer, uv);
      r.colour += axes[axis] * sample.albedo;
      r.roughness += axes[axis] * sample.roughness;
      r.height += axes[axis] * sample.metallic;
      r.bump += axes[axis] * terrain_slope(layer, uv, du, dv, n, t, b, filtered);
    }
    // The means are computed from linear decoded texels at scene preparation, so this policy
    // also works in a decoded/no-cache scene, whose images carry only one mip level.
    r.colour = lerp(terrain_layer_mean_colour(layer), r.colour, fade);
    r.roughness = lerp(terrain_layer_mean_roughness(layer), r.roughness, fade);
    r.height = lerp(0.5, r.height, fade);
    r.bump *= fade;
  }
  if (!heights) r.height = 0.5;
  return r;
}

MaterialSample sample_terrain_material(SceneGeometry geo, ResolveMaterial root, float2 map_uv,
                                      float2 map_dx, float2 map_dy, float3 point, float3 n,
                                      float3 dp_dx, float3 dp_dy, float distance_m, bool filtered,
                                      out float3 shading_normal) {
  ResolveMaterial* table = (ResolveMaterial*)geo.materials;
  const TerrainRecord rec = ((TerrainRecord*)geo.materials)[terrain_record_index(root)];
  // All layouts use the same map frame. Reconstruct it from the position rather than half UVs,
  // whose quantization changes with a chunk's extent and would move a painted boundary at LOD.
  // Past 2^24 mm (16.7 km) from the map's corner a float no longer holds the cell's millimetre,
  // so there the offset is split into whole 1024 mm, exact to 17,000 km, and the rest; within it
  // the arithmetic is W68's, operation for operation, so a scene drawn before draws the same bytes.
  const int64_t x_mm = int64_t(geo.eye.cell.x) * int64_t(64000) - int64_t(rec.map_x0_mm);
  const int64_t z_mm = int64_t(geo.eye.cell.z) * int64_t(64000) - int64_t(rec.map_z0_mm);
  const float per_mm = rec.map_per_mm;
  const float2 local_mm = (geo.eye.local.xz + geo.eye.residual.xz + point.xz) * 1000.0;
  const int64_t k_exact = 16777216;
  if (x_mm > -k_exact && x_mm < k_exact && z_mm > -k_exact && z_mm < k_exact) {
    map_uv = (float2(float(x_mm), float(z_mm)) + local_mm) * per_mm;
  } else {
    const int64_t x_hi = terrain_magnitude_floor_div(x_mm, 1024) * 1024;
    const int64_t z_hi = terrain_magnitude_floor_div(z_mm, 1024) * 1024;
    map_uv = float2(float(x_hi), float(z_hi)) * per_mm +
             (float2(float(x_mm - x_hi), float(z_mm - z_hi)) + local_mm) * per_mm;
  }
  map_dx = dp_dx.xz * (1000.0 * per_mm);
  map_dy = dp_dy.xz * (1000.0 * per_mm);
  MaterialSample result = filtered ? sample_material_grad(root, map_uv, map_dx, map_dy)
                                   : sample_material(root, map_uv);
  float3 slope = float3(0.0);
  float3 axes = n * n;
  axes *= axes;
  axes /= max(axes.x + axes.y + axes.z, 1e-12);
  const float fade = 1.0 - smoothstep(rec.detail_near_m, rec.detail_far_m, distance_m);
  if ((rec.flags & k_terrain_masked) != 0) {
    // **Ground materials by place**: the tile's table names up to four palette layers and the mask
    // weighs them; the mip follows the pixel's footprint on the ground (none for the path tracer,
    // whose rays carry no footprint, as its texture reads take level 0).
    const float footprint_mm = 1000.0 * max(length(dp_dx), length(dp_dy));
    const float level = filtered && footprint_mm > 0.0
                            ? log2(max(footprint_mm / float(rec.mask_spacing_mm), 1.0))
                            : 0.0;
    const TerrainMaskSample mask = terrain_mask_sample(rec, geo.eye, point, level);
    if (mask.valid) {
      TerrainLayerSample layers[4];
      float weight[4];
      float total = 0.0;
      bool any_height = false;
      [unroll] for (uint c = 0; c < 4; ++c) {
        const uint index = (mask.table >> (8 * c)) & 0xFFu;
        weight[c] = index == 0xFFu || index >= rec.layer_count ? 0.0 : mask.weights[c];
        layers[c].colour = float3(0.0);
        layers[c].roughness = 0.0;
        layers[c].bump = float3(0.0);
        layers[c].height = 0.5;
        if (weight[c] <= 0.0) continue;
        const ResolveMaterial layer = table[rec.first_layer + index];
        layers[c] = terrain_layer_sample(geo, layer, map_uv, map_dx, map_dy, point, n, axes, dp_dx,
                                         dp_dy, fade, filtered);
        any_height = any_height || (layer.flags & k_material_terrain_height) != 0;
        total += weight[c];
      }
      if (total > 0.0) {
        const float cover = min(total, 1.0);
        // Height blend (Mishkinis 2013): where weights overlap, the layer standing higher shows.
        float blended[4];
        float sum = 0.0;
        if (any_height && rec.height_blend > 0.0) {
          float top = -1e9;
          [unroll] for (uint c = 0; c < 4; ++c)
            if (weight[c] > 0.0) top = max(top, layers[c].height + weight[c]);
          [unroll] for (uint c = 0; c < 4; ++c) {
            blended[c] = weight[c] > 0.0
                             ? max(layers[c].height + weight[c] - (top - rec.height_blend), 0.0)
                             : 0.0;
            sum += blended[c];
          }
        } else {
          [unroll] for (uint c = 0; c < 4; ++c) {
            blended[c] = weight[c];
            sum += blended[c];
          }
        }
        float3 colour = float3(0.0);
        float roughness = 0.0;
        float3 bump = float3(0.0);
        float porosity = 0.0;
        [unroll] for (uint c = 0; c < 4; ++c) {
          if (blended[c] <= 0.0) continue;
          const float w = blended[c] / sum;
          const uint index = (mask.table >> (8 * c)) & 0xFFu;
          colour += w * layers[c].colour;
          roughness += w * layers[c].roughness;
          bump += w * layers[c].bump;
          porosity += w * terrain_layer_porosity(table[rec.first_layer + index]);
        }
        result.albedo = lerp(result.albedo, colour, cover);
        result.roughness = lerp(result.roughness, roughness, cover);
        slope = bump * cover;
        porosity = lerp(0.5, porosity, cover);
        // Wetness (Lagarde 2013): porous ground darkens by up to 0.8 and loses up to 0.4 of its
        // roughness; rock barely changes. A separate quantity, so no substrate turns glossy dry.
        const float wet = max(rec.wetness, mask.wetness);
        const float darkening = lerp(1.0, 0.2, porosity);
        result.albedo *= lerp(1.0, darkening, wet);
        result.roughness *= lerp(1.0, darkening, 0.5 * wet);
      } else {
        const float wet = max(rec.wetness, mask.wetness);
        result.albedo *= lerp(1.0, 0.6, wet);
        result.roughness *= lerp(1.0, 0.6, 0.5 * wet);
      }
    }
  } else {
    for (uint i = 0; i < rec.layer_count; ++i) {
      const ResolveMaterial layer = table[rec.first_layer + i];
      float coverage = terrain_layer_weight(layer);
      if (layer.occlusion_texture != k_no_texture) {
        const uint slot = slot_sampler(layer, layer.samplers_occlusion_emissive, 0);
        if (filtered && (layer.flags & k_material_mipped) != 0)
          coverage *= g_textures[NonUniformResourceIndex(layer.occlusion_texture)].SampleGrad(
              g_samplers[NonUniformResourceIndex(slot)], map_uv, map_dx, map_dy).r;
        else
          coverage *= g_textures[NonUniformResourceIndex(layer.occlusion_texture)].SampleLevel(
              g_samplers[NonUniformResourceIndex(slot)], map_uv, 0).r;
      }
      if (coverage <= 0.0) continue;
      const TerrainLayerSample s = terrain_layer_sample(geo, layer, map_uv, map_dx, map_dy, point,
                                                        n, axes, dp_dx, dp_dy, fade, filtered);
      result.albedo = lerp(result.albedo, s.colour, coverage);
      result.roughness = lerp(result.roughness, s.roughness, coverage);
      slope = lerp(slope, s.bump, coverage);
    }
  }
  result.metallic = 0.0;
  result.emissive = float3(0.0);
  result.detail_mask = 0.0;
  shading_normal = normalize(n + slope);
  return result;
}
#endif
