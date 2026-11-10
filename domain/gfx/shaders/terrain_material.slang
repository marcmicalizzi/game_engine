// Optional native surface evaluator. Included only in MATERIAL_TERRAIN shader modules; ordinary
// scenes retain their existing shaders. Layout: gfx::k_material_terrain_layers (112-byte entries).
#ifndef ENGINE_TERRAIN_MATERIAL_SLANG
#define ENGINE_TERRAIN_MATERIAL_SLANG

#include "hex_lattice.slang"  // the lattice and the blend, shared with stochastic mesh materials
#include "formation.slang"    // W85: the world formation's bands (gfx::FormationParams)
#include "wet_ground.slang"   // S99: wet ground as a material (the swash, the channels' banks)

static const uint k_material_terrain_layers = 512u;
static const uint k_material_terrain_map = 1024u;
static const uint k_material_terrain_height = 2048u;
static const uint k_material_terrain_stochastic = 4096u;
// A layer's response (R99; gfx::k_material_terrain_snow, _sand, _beach; renderer.md, "Sand and
// snow"). Neither bit: the layer as W68 and W72 draw it.
static const uint k_material_terrain_snow = 16384u;
static const uint k_material_terrain_sand = 32768u;
static const uint k_material_terrain_beach = 65536u;
static const uint k_material_terrain_formation = 262144u;  // W85 (gfx::k_material_terrain_formation)
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
  uint2 formation;  // W85: the FormationParams block's device address, low word first; 0 none
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
// blends them by the variance-preserving operator (renderer.md, "Stochastic
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

// The hash, the tiles and the blend are hex_lattice.slang's (R131 moved them there, unchanged, to
// share them with the mesh materials); these names are the terrain's, kept for its callers and its
// probe.
uint terrain_hash(uint x) { return hex_hash(x); }
typealias TerrainHex = HexTiles;

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
  const uint seed = terrain_hash(uint(pitch) ^ 0x9e3779b9u);
  return hex_tiles(uint(bx), uint(by), f, seed, rotation);
}

float3 terrain_hex_blend(TerrainHex hex, float3 mean, float3 a, float3 b, float3 c) {
  return hex_blend(hex, mean, a, b, c);
}

// ---- What the pixel can see: fewer reads where it cannot tell (R136) ---------------------------
// Five rules cut a stochastic layer's reads (renderer.md, "What a far pixel reads"). Four go by the
// pixel's footprint and are exactly off for a footprint under one lattice cell, so with them alone
// the near ground draws what it did; each is a band, never a step, and each needs filtered, mipped
// textures (the path tracer and a decoded scene read level 0, where nothing below is true). The
// fifth, tiles by weight, acts everywhere and moves a pixel by under a code step where it does.
//
// TERRAIN_FOOTPRINT_LEVERS selects them at compile time (bit 1 the tiles by footprint, 2 the
// projections, 4 the maps, 8 the layers, 16 the tiles by weight). **The engine ships with 16
// alone**: on the v31 fell the four footprint rules act on half a percent of the frame at 1920 x
// 1080 and cost the rest of it 0.4 ms of registers and branches, where tiles by weight returns a
// fifth of the resolve (docs/experiments/terrain-resolve-2026-11-07.md). The four stay compiled
// out, measured and tested (terrain_material_probe.slang and the renderer's footprint test build
// them in), for a view whose far ground is most of the frame to turn on.
#ifndef TERRAIN_FOOTPRINT_LEVERS
#define TERRAIN_FOOTPRINT_LEVERS 16
#endif
static const uint k_terrain_lever_tiles = 1u;
static const uint k_terrain_lever_projections = 2u;
static const uint k_terrain_lever_maps = 4u;
static const uint k_terrain_lever_layers = 8u;
static const uint k_terrain_levers = TERRAIN_FOOTPRINT_LEVERS;

// The lattice's cell in the plane's UV (one repeat is 1): neighbouring lattice points are
// |s^-1 (1, 0)| = 28/97 = 0.2887 apart (1 / (2 sqrt 3) with the skew's rational 97/28), so a tile
// dominates a hexagon about that wide.
static const float k_terrain_hex_cell_uv = 28.0 / 97.0;
// **Tiles by footprint.** Where the pixel's footprint (the side of its area in the plane,
// sqrt |du x dv|) is one cell, three tiles start blending to the nearest one; at two cells — the
// cell half the pixel's side, a quarter of its area — one tile is read with the whole weight. Both
// reads land on the texture filtered at the footprint's mip: the variance-preserving blend of three
// independent tiles keeps that mip's mean and variance (Heitz and Neyret 2018, Eq. 9) and one tile
// is that mip, so past a cell a pixel the two are the same distribution and only the arrangement of
// sub-pixel cells, which no pixel resolves, differs.
static const float k_terrain_one_tile_from = 1.0;  // cells per pixel side: the band starts
static const float k_terrain_one_tile_at = 2.0;    // and one tile reads alone
// **Maps by footprint.** The texture filter's width is at least max(minor axis, major axis / 16)
// of the footprint (the sampler's anisotropy is 16 at most, `GpuScene`'s material samplers), so
// where that is a whole repeat the hardware reads every map's last mip, its one texel: the layer's
// mean colour, mean roughness and mean height, which the layer record already carries, and a flat
// normal (the mean slope of a tileable height field is zero). From half a repeat the reads blend to
// the means; from a whole one the colour, metallic-roughness and normal maps are not read at all.
// The normal-variance chain (R97) is still read there, once: its last mip is the whole map's
// variation, which the footprint's roughness has to carry.
static const float k_terrain_flat_from = 0.5;  // filter width, repeats: the band starts
static const float k_terrain_flat_at = 1.0;    // and the maps are their means
// **Projections by what they contribute.** The 1/128 floor (`k_terrain_hex_axis_floor`) holds while
// the pixel resolves cells. As the footprint grows past a cell, the floor rises to 1/32 with a band
// to 1/16 above it: a share s of a projection that is dropped and renormalized moves the colour by
// at most s times the gap between two projections' reads, which past a cell are reads of the same
// texture at a mip whose variation has mostly gone (the measurement bounds the result: FLIP under
// 0.005 over the fell's frame, docs/experiments/terrain-resolve-2026-11-07.md).
static const float k_terrain_far_axis_floor = 1.0 / 32.0;
static const float k_terrain_far_axis_full = 1.0 / 16.0;
// **Layers by weight.** A mask layer's weight under 1/128 is skipped and one under 1/64 ramps to
// it, past a cell a pixel of that layer, its share given to the others as the height blend's
// normalization does; the mask's coverage is the original weights', so the root never shows.
static const float k_terrain_far_layer_floor = 1.0 / 128.0;
static const float k_terrain_far_layer_full = 1.0 / 64.0;

// The footprint's side in the plane's UV over the cell: sqrt |du x dv| / cell.
float terrain_footprint_cells(float2 du, float2 dv) {
  return sqrt(abs(du.x * dv.y - du.y * dv.x)) / k_terrain_hex_cell_uv;
}
// The texture filter's width in repeats, a lower bound on what the sampler takes: the larger of
// the footprint's minor axis and its major axis over the anisotropy's 16.
float terrain_filter_width(float2 du, float2 dv) {
  const float major = max(length(du), length(dv));
  const float minor = abs(du.x * dv.y - du.y * dv.x) / max(major, 1e-30);
  return max(minor, major * (1.0 / 16.0));
}
// 0 where three tiles read, 1 where one does; exactly 0 under a cell.
float terrain_one_tile(float cells) {
  if ((k_terrain_levers & k_terrain_lever_tiles) == 0u) return 0.0;
  return smoothstep(k_terrain_one_tile_from, k_terrain_one_tile_at, cells);
}
// 0 where the maps are read, 1 where they are their means; exactly 0 under half a repeat.
float terrain_flat_maps(float width) {
  if ((k_terrain_levers & k_terrain_lever_maps) == 0u) return 0.0;
  return smoothstep(k_terrain_flat_from, k_terrain_flat_at, width);
}
// The projections kept and their renormalized shares: the old step at 1/128 under a cell (`far`
// 0), a band from `lo` to `hi` rising with `far` to 1/32 and 1/16 past two cells.
float3 terrain_kept_projections(float3 axes, float far) {
  float3 kept;
  if (far <= 0.0 || (k_terrain_levers & k_terrain_lever_projections) == 0u) {
    kept = axes * step(k_terrain_hex_axis_floor, axes);
  } else {
    const float lo = lerp(k_terrain_hex_axis_floor, k_terrain_far_axis_floor, far);
    const float hi = lerp(k_terrain_hex_axis_floor, k_terrain_far_axis_full, far);
    kept = axes * saturate((axes - lo) / (hi - lo));
  }
  return kept / (kept.x + kept.y + kept.z);
}
// A mask layer's weight as read past a cell (`far`): under the floor nothing, a band to the full
// weight. Exactly the weight at `far` 0.
float terrain_far_layer_weight(float weight, float far) {
  if (far <= 0.0 || (k_terrain_levers & k_terrain_lever_layers) == 0u) return weight;
  const float lo = far * k_terrain_far_layer_floor;
  const float hi = far * k_terrain_far_layer_full;
  return weight * saturate((weight - lo) / (hi - lo));
}
// **Tiles by weight** (lever 16). Burley's power-7 weights leave a tile under 1/128 of the blend
// over about half of every cell (its barycentric under half the largest's: 0.5^7 = 1/128), where
// it moves the pixel by under 1/128 of its departure from the mean — under a code step. Such a tile
// is not read: a weight under 1/256 is dropped, one under 1/128 ramps to its own, and the kept
// weights are renormalized and the operator's gain taken from them, so the blend stays a
// variance-preserving one of the tiles it reads. Where every weight is 1/128 or more the hex is
// returned untouched, bit for bit; this is the one rule that reads fewer tiles on the near ground.
static const float k_terrain_tile_floor = 1.0 / 256.0;
static const float k_terrain_tile_full = 1.0 / 128.0;
static const uint k_terrain_lever_tile_weight = 16u;
TerrainHex terrain_hex_kept(TerrainHex hex) {
  if ((k_terrain_levers & k_terrain_lever_tile_weight) == 0u) return hex;
  if (min(hex.weight.x, min(hex.weight.y, hex.weight.z)) >= k_terrain_tile_full) return hex;
  float3 w = hex.weight * saturate((hex.weight - k_terrain_tile_floor) /
                                   (k_terrain_tile_full - k_terrain_tile_floor));
  w /= w.x + w.y + w.z;  // the largest weight is at least 1/3 and always kept
  hex.weight = w;
  hex.gain = 1.0 / sqrt(dot(w, w));
  return hex;
}

// The nearest lattice point of the three: the tile with the largest weight.
uint terrain_hex_nearest(TerrainHex hex) {
  if (hex.weight.x >= hex.weight.y && hex.weight.x >= hex.weight.z) return 0u;
  return hex.weight.y >= hex.weight.z ? 1u : 2u;
}

// One projection of a stochastic layer: every map read at the hex's three places with the same
// weights (the maps stay coordinated), each place's UV derivatives turned with its tile so the
// mip and the anisotropy are the footprint's (Deliot and Heitz 2019: SampleGrad, never the
// derivative of the jittered UV). The colour, roughness and height blend about the layer's own
// means; the slopes about zero, the mean slope of any tileable height field.
void terrain_hex_sample(ResolveMaterial layer, TerrainHex lattice, float2 uv, float2 du,
                        float2 dv, bool filtered, out float3 colour, out float roughness,
                        out float height, out float2 slope, out float loss) {
  const TerrainHex hex = terrain_hex_kept(lattice);
  const bool normals = layer.normal_texture != k_no_texture && layer.normal_scale != 0.0;
  const bool grads = filtered && (layer.flags & k_material_mipped) != 0;
  // R136: past a cell a pixel one tile (`alone`), past a repeat's filter width the maps' means
  // (`maps` false); both only with mipped, filtered reads, and both exactly off on the near ground.
  // One loop for every case, the skipped reads predicated off, so the kernel keeps one copy of the
  // reads and the near ground's registers.
  const float one = grads ? terrain_one_tile(terrain_footprint_cells(du, dv)) : 0.0;
  const float flat = grads ? terrain_flat_maps(terrain_filter_width(du, dv)) : 0.0;
  const bool alone = one >= 1.0;
  const bool maps = flat < 1.0;
  const uint nearest = terrain_hex_nearest(hex);
  const uint normal_slot = slot_sampler(layer, layer.samplers_mr_normal, 16);
  // R97: the tiles' roughness is read unwidened and the widening applied once to the blend. Read
  // through `sample_material_grad` each tile would arrive widened, and the variance-preserving
  // operator scales a tile's departure from the layer's (unwidened) mean by up to √3 — the widening
  // itself would then vary with the lattice. The tiles' shortening is a plain weighted mean: a
  // mixture's unresolved variance, not a texel to keep the contrast of.
  const bool variance = normals && grads && (layer.flags & k_material_normal_variance) != 0;
  ResolveMaterial plain = layer;
  plain.flags &= ~k_material_normal_variance;
  loss = 0.0;
  float3 c[3];
  float r[3];
  float h[3];
  float2 d[3];
  // The nearest tile's reads, kept by constant index as the unrolled loop passes it.
  float3 c_near = terrain_layer_mean_colour(layer);
  float r_near = terrain_layer_mean_roughness(layer);
  float h_near = terrain_layer_mean_height(layer);
  float2 d_near = float2(0.0);
  float l_near = 0.0;
  [unroll] for (uint k = 0; k < 3; ++k) {
    // A tile not read (R136) stands at the layer's means and a flat slope, which the operator
    // leaves out; the results below are then the nearest tile's own.
    c[k] = terrain_layer_mean_colour(layer);
    r[k] = terrain_layer_mean_roughness(layer);
    h[k] = terrain_layer_mean_height(layer);
    d[k] = float2(0.0);
    if ((alone && k != nearest) || hex.weight[k] <= 0.0) continue;
    const float2 turn = hex.turn[k];
    const float2 p = uv + hex.shift[k];
    const float2 du_k = float2(turn.x * du.x - turn.y * du.y, turn.y * du.x + turn.x * du.y);
    const float2 dv_k = float2(turn.x * dv.x - turn.y * dv.y, turn.y * dv.x + turn.x * dv.y);
    float tile_loss = 0.0;
    if (variance) {
      tile_loss =
          g_textures[NonUniformResourceIndex(layer.normal_texture + 1u)]
              .SampleGrad(g_samplers[NonUniformResourceIndex(normal_slot)], p, du_k, dv_k)
              .r;
      loss += hex.weight[k] * tile_loss;
    }
    if (k == nearest) l_near = tile_loss;
    if (!maps) continue;
    const MaterialSample s =
        filtered ? sample_material_grad(plain, p, du_k, dv_k) : sample_material(plain, p);
    c[k] = s.albedo;
    r[k] = s.roughness;
    h[k] = s.metallic;
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
    if (k == nearest) {
      c_near = c[k];
      r_near = r[k];
      h_near = h[k];
      d_near = d[k];
    }
  }
  colour = saturate(terrain_hex_blend(hex, terrain_layer_mean_colour(layer), c[0], c[1], c[2]));
  roughness = saturate(terrain_hex_blend(hex, float3(terrain_layer_mean_roughness(layer)),
                                         float3(r[0]), float3(r[1]), float3(r[2])).x);
  height = saturate(terrain_hex_blend(hex, float3(terrain_layer_mean_height(layer)), float3(h[0]),
                                      float3(h[1]), float3(h[2])).x);
  slope = hex.gain * (hex.weight.x * d[0] + hex.weight.y * d[1] + hex.weight.z * d[2]);
  // R136's bands: toward the nearest tile alone, then toward the maps' means. Both are reads this
  // loop already made, so a band costs no read; under a cell neither runs.
  if (one > 0.0) {
    colour = lerp(colour, saturate(c_near), one);
    roughness = lerp(roughness, saturate(r_near), one);
    height = lerp(height, saturate(h_near), one);
    slope = lerp(slope, d_near, one);
    loss = lerp(loss, l_near, one);
  }
  if (flat > 0.0) {
    colour = lerp(colour, terrain_layer_mean_colour(layer), flat);
    roughness = lerp(roughness, terrain_layer_mean_roughness(layer), flat);
    height = lerp(height, terrain_layer_mean_height(layer), flat);
    slope *= 1.0 - flat;
  }
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
    // R136: past a cell a pixel (the surface's own footprint, so every projection agrees) the
    // floor rises in a band; under a cell it is the 1/128 step it was.
    const bool grads = filtered && (layer.flags & k_material_mipped) != 0;
    const float far = grads ? smoothstep(k_terrain_one_tile_from, k_terrain_one_tile_at,
                                         sqrt(length(cross(dx, dy))) / k_terrain_hex_cell_uv)
                            : 0.0;
    const float3 kept = terrain_kept_projections(axes, far);
    [unroll] for (uint axis = 0; axis < 3; ++axis) {
      if (kept[axis] <= 0.0) continue;
      const float2 uv = axis == 0 ? phase.zy : (axis == 1 ? phase.xz : phase.xy);
      const float2 du = axis == 0 ? dx.zy : (axis == 1 ? dx.xz : dx.xy);
      const float2 dv = axis == 0 ? dy.zy : (axis == 1 ? dy.xz : dy.xy);
      const float3 t = axis == 0 ? float3(0, 0, 1) : float3(1, 0, 0);
      const float3 b = axis == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
      const TerrainHex hex = terrain_hex(geo.eye, point, axis, pitch, terrain_layer_rotation(layer));
      float3 colour;
      float roughness, height, loss;
      float2 slope;
      terrain_hex_sample(layer, hex, uv, du, dv, filtered, colour, roughness, height, slope, loss);
      if (loss > 0.0)
        roughness = brdf_filter_roughness(roughness, brdf_normal_variance(loss, layer.normal_scale),
                                          0.0);
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

// Post-fetch policy only (R97). A tiled layer with a variance chain follows the texture footprint
// for its normal and roughness: it no longer flattens its normal at detail_far_m, because the
// chain moves what the footprint stops resolving into the roughness instead. The caller fetches it
// with fade=1 and the colour and height keep the tiled fade here. A painted (`Map`) layer never
// faded and a stochastic one does not (R96), so neither is touched.
TerrainLayerSample terrain_filter_sample(TerrainLayerSample sample, ResolveMaterial layer,
                                         float fade) {
  if ((layer.flags & k_material_normal_variance) != 0 &&
      (layer.flags & (k_material_terrain_map | k_material_terrain_stochastic)) == 0) {
    sample.colour = lerp(terrain_layer_mean_colour(layer), sample.colour, fade);
    sample.height = lerp(0.5, sample.height, fade);
  }
  return sample;
}

// W85: a layer banded by the scene's formation (renderer.md, "Formation bands"), after the detail
// fade and the variance policy so the band survives where the layer's texture has faded to its
// mean: its colour times the bed's and the macro bed's tint, its roughness plus their offsets,
// box-filtered over the pixel's footprint across the beds. A layer without the bit, or a terrain
// whose record names no formation, is returned as it came.
TerrainLayerSample terrain_formation_band(TerrainRecord rec, ResolveMaterial layer,
                                          TerrainLayerSample sample, FrameEye eye, float3 point,
                                          float3 dp_dx, float3 dp_dy) {
  if ((layer.flags & k_material_terrain_formation) == 0u) return sample;
  const uint64_t address = uint64_t(rec.formation.x) | (uint64_t(rec.formation.y) << 32);
  if (address == 0u) return sample;
  const FormationPoint p =
      formation_coordinate((FormationParams*)address, eye.cell, eye.local + eye.residual + point);
  const float4 band = formation_band(address, p, dp_dx, dp_dy);
  sample.colour *= band.rgb;
  sample.roughness = saturate(sample.roughness + band.w);
  return sample;
}

// ---- Sand and snow: a response per layer (R99) -------------------------------------------------
// The grounding, every number's source and which are design choices:
// docs/experiments/sand-and-snow-substrates-2026-11-02.md. The CPU mirror is
// domain/gfx/tests/terrain_material_tests.cpp, over terrain_material_probe.slang's response_main.
// The numbers ride in words a layer never reads otherwise (gfx::set_terrain_layer).
float terrain_layer_glints(ResolveMaterial layer) { return layer.emissive.x; }
float terrain_snow_grain_mm(ResolveMaterial layer) { return layer.emissive.y; }
float terrain_snow_lobe(ResolveMaterial layer) { return layer.emissive.z; }
float terrain_snow_thickness(ResolveMaterial layer) { return layer.occlusion_strength; }
float terrain_beach_wet(ResolveMaterial layer) { return layer.emissive.y; }

// Ice's absorption coefficient 4 pi chi / lambda, per metre, at the channels' wavelengths 610, 550
// and 465 nm (Warren and Brandt 2008's chi: 6.890e-9, 2.289e-9, and 1.640e-10 between its 460 and
// 470 nm rows), and Kokhanovsky's shape factor: the absorption length is 16 grain diameters.
static const float3 k_terrain_ice_absorption = float3(0.141938186, 0.0522989497, 0.00443336150);
static const float k_terrain_snow_shape = 16.0;

// Snow's spherical albedo per channel from its optical grain diameter (Kokhanovsky and Zege 2004):
// exp(-sqrt(alpha l)). 0.977, 0.986, 0.996 at 0.25 mm; bluer as the grain coarsens.
float3 terrain_snow_albedo(float grain_mm) {
  return exp(-sqrt(k_terrain_ice_absorption * (k_terrain_snow_shape * 0.001 * grain_mm)));
}

// The specular lobe's weight w (in multiples of the dielectric F0 = 0.04) as the BSDF's metallic
// word m, with the diffuse kept: 0.04 (1 - m) + a' m = 0.04 w and a' (1 - m) = a give
// k m^2 + b m - c = 0, taken in its cancellation-free form. w = 1 is m = 0 exactly.
float terrain_lobe_metallic(float a, float w) {
  const float k = 0.04;
  const float c = k * (w - 1.0);
  if (c <= 0.0) return 0.0;
  const float b = a + k * (w - 2.0);
  return 2.0 * c / (b + sqrt(b * b + 4.0 * k * c));
}

// Lekner and Dorf (1988) over Angstrom (1925): a water film over a dry albedo a returns
// (1 - r_e)(1 - r_i) a / (1 - r_i a) of the light, with water's diffuse external reflectance
// r_e = 0.066 and internal r_i = 1 - (1 - r_e) / n^2 = 0.4744 (n = 1.333). This is the factor.
static const float k_terrain_wet_through = 0.490946;
static const float k_terrain_wet_internal = 0.474362;
float3 terrain_wet_darkening(float3 a) {
  return k_terrain_wet_through / (1.0 - k_terrain_wet_internal * a);
}

// Glints (Jakob et al. 2014; Zirr and Kaplanyan 2016; Deliot and Belcour 2023, simplified): the
// ground in world-anchored cells of power-of-two millimetres, the pixel holding a glinting facet
// with chance p = `density * min(1, 4 mm / footprint)`, none past a 256 mm footprint; a facet's
// normal is drawn from GGX of alpha 0.15 about the macro normal (Walter et al. 2007), turned at most
// 25 degrees, its own lobe 0.1 rough. Two levels about the footprint draw (R125): the finer cell
// l0 = floor(log2 footprint) and the coarser l0 + 1, each with its own world-anchored hash seeded by
// its level, the coarser's chance growing and the finer's fading by the fractional log t, scaled so
// the chance either draws is p exactly; where both draw the finer's facet wins. At t = 1 the
// coarser level holds all of p and is the next octave's finer one, the same cells and the same
// hash, so no curve of equal footprint re-draws the set (one level, as before R125, doubled its
// cell and re-drew its facets along every such curve, which on flat ground is a row of the screen).
// Design choices, all of them (the page says so): a facet replaces the pixel's one normal, so its
// diffuse turns with it, and a facet turned far enough to glint with the sun behind the eye would
// speckle the snow with dark pixels (the first captures did); the near-normal facets glint towards
// the sun, where snow sparkles most.
static const float k_terrain_glint_roughness = 0.1;
static const float k_terrain_glint_tilt = 0.15;
static const float k_terrain_glint_max_tan = 0.466307658;  // tan 25 degrees
static const float k_terrain_glint_full_mm = 4.0;
static const float k_terrain_glint_far_mm = 256.0;

struct TerrainGlint {
  float3 normal;  // the facet's, or the macro normal without one
  bool lit;
  float chance;   // the footprint's chance p of a facet in the pixel
  float blend;    // the fractional log t: the coarser level's share
  float chance0;  // the finer level's chance, x (1 - t)
  float chance1;  // the coarser level's chance, x t
  float draw0;    // the finer cell's hash in [0, 1): a facet when below its chance
  float draw1;    // the coarser cell's
  uint size_mm;   // the finer cell's side; the coarser's is twice it
  uint level_lit; // 0 the finer level's facet, 1 the coarser's, 2 none
};

// An orthonormal basis about n (Duff et al. 2017, "Building an orthonormal basis, revisited").
void terrain_basis(float3 n, out float3 t, out float3 b) {
  const float s = n.z >= 0.0 ? 1.0 : -1.0;
  const float a = -1.0 / (s + n.z);
  const float c = n.x * n.y * a;
  t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
  b = float3(c, s + n.y * n.y * a, -n.y);
}

int64_t terrain_glint_axis(int cell, float local_mm, int64_t size) {
  const float whole = floor(local_mm);
  return terrain_magnitude_floor_div(int64_t(cell) * int64_t(64000) + int64_t(whole), size);
}

// The hash of the cell of side 2^level millimetres holding the point: the frame's integer cell and
// its local millimetres, never a world float, so a cell draws the same on every frame and eye.
uint terrain_glint_cell(FrameEye eye, float3 point, uint level, uint seed) {
  const int64_t size = int64_t(1u << level);
  const int64_t ix = terrain_glint_axis(eye.cell.x, (eye.local.x + eye.residual.x + point.x) * 1000.0,
                                        size);
  const int64_t iz = terrain_glint_axis(eye.cell.z, (eye.local.z + eye.residual.z + point.z) * 1000.0,
                                        size);
  return terrain_hash(uint(ix) ^ terrain_hash(uint(iz) ^ terrain_hash(level ^ seed)));
}

TerrainGlint terrain_glint(FrameEye eye, float3 point, float3 n, float footprint_mm, float density,
                           float alpha, uint seed) {
  TerrainGlint g;
  g.normal = n;
  g.lit = false;
  g.chance = 0.0;
  g.blend = 0.0;
  g.chance0 = 0.0;
  g.chance1 = 0.0;
  g.draw0 = 1.0;
  g.draw1 = 1.0;
  g.size_mm = 0u;
  g.level_lit = 2u;
  if (density <= 0.0 || footprint_mm >= k_terrain_glint_far_mm) return g;
  const float lf = log2(max(footprint_mm, 1.0));
  const float fl = floor(lf);
  const uint level = uint(max(fl, 0.0));
  const float t = saturate(lf - fl);
  g.size_mm = 1u << level;
  g.blend = t;
  g.chance = density * min(1.0, k_terrain_glint_full_mm / max(footprint_mm, 1.0)) *
             (1.0 - smoothstep(0.5 * k_terrain_glint_far_mm, k_terrain_glint_far_mm, footprint_mm));
  // x (1 - t) and x t drawn independently light a pixel with chance x - x^2 t (1 - t); that is p
  // at x = 2p / (1 + sqrt(1 - 4 t (1 - t) p)), the root without cancellation (x = p at t = 0 or 1).
  const float x = 2.0 * g.chance / (1.0 + sqrt(max(1.0 - 4.0 * t * (1.0 - t) * g.chance, 0.0)));
  g.chance0 = x * (1.0 - t);
  g.chance1 = x * t;
  const uint h0 = terrain_glint_cell(eye, point, level, seed);
  const uint hc = terrain_glint_cell(eye, point, level + 1u, seed);
  g.draw0 = float(h0 >> 8) * (1.0 / 16777216.0);
  g.draw1 = float(hc >> 8) * (1.0 / 16777216.0);
  uint h;
  if (g.draw0 < g.chance0) {
    h = h0;
    g.level_lit = 0u;
  } else if (g.draw1 < g.chance1) {
    h = hc;
    g.level_lit = 1u;
  } else {
    return g;
  }
  const uint h1 = terrain_hash(h ^ 0x68e31da4u);
  const uint h2 = terrain_hash(h1);
  const float u1 = float(h1 >> 8) * (1.0 / 16777216.0);
  const float u2 = float(h2 >> 8) * (1.0 / 16777216.0);
  const float tan_t = min(alpha * sqrt(u1 / (1.0 - u1)), k_terrain_glint_max_tan);
  const float cos_t = 1.0 / sqrt(1.0 + tan_t * tan_t);
  const float sin_t = tan_t * cos_t;
  const float phi = (2.0 * u2 - 1.0) * 3.14159265358979;
  float3 tx;
  float3 bx;
  terrain_basis(n, tx, bx);
  g.normal = normalize(tx * (sin_t * cos(phi)) + bx * (sin_t * sin(phi)) + n * cos_t);
  g.lit = true;
  return g;
}
// The layer's response, once per layer on its record word (never per texel): snow's albedo by its
// grain (times its tint, and its map over the map's mean) and its thickness as its height; the
// beach's wet darkening, per channel, at the place's wetness; and both responses' glints at the
// footprint. A layer with neither bit returns its sample untouched.
TerrainLayerSample terrain_respond(FrameEye eye, ResolveMaterial layer, TerrainLayerSample s,
                                   float3 point, float3 n, float footprint_mm, bool filtered,
                                   float wet) {
  if ((layer.flags & (k_material_terrain_snow | k_material_terrain_sand)) == 0u) return s;
  uint seed;
  if ((layer.flags & k_material_terrain_snow) != 0u) {
    const float3 mean = max(terrain_layer_mean_colour(layer), float3(1e-4));
    s.colour = terrain_snow_albedo(terrain_snow_grain_mm(layer)) * layer.albedo.rgb *
               saturate(s.colour / mean);
    s.height = terrain_snow_thickness(layer);
    seed = 0x534E4F57u;  // "SNOW"
  } else {
    if ((layer.flags & k_material_terrain_beach) != 0u && wet > 0.0) {
      const float3 dark = terrain_wet_darkening(s.colour);
      const float amount = wet * terrain_beach_wet(layer);
      s.colour *= lerp(float3(1.0), dark, amount);
      s.roughness *= lerp(1.0, (dark.x + dark.y + dark.z) * (1.0 / 3.0), 0.5 * amount);
    }
    seed = 0x53414E44u;  // "SAND"
  }
  if (filtered && footprint_mm > 0.0) {
    const TerrainGlint g = terrain_glint(eye, point, n, footprint_mm, terrain_layer_glints(layer),
                                         k_terrain_glint_tilt, seed);
    if (g.lit) {
      s.bump = g.normal / max(dot(g.normal, n), 0.05) - n;
      s.roughness = k_terrain_glint_roughness;
    }
  }
  return s;
}

// Combine already-fetched distributions: the second moment includes separation of their means.
// Trace of slope covariance widens alpha^2; equal slopes add nothing. This isotropic approximation
// is deliberately shared with the double reference, not claimed to preserve anisotropy (R97).
float terrain_filter_combination(float alpha2, float slope_second, float3 mean_slope) {
  const float unresolved = max(slope_second - dot(mean_slope, mean_slope), 0.0);
  return sqrt(sqrt(saturate(alpha2 + unresolved)));
}

// **The wet response** (S99; wet_ground.slang; terrain.md, "Wet ground"): the sampled ground at
// saturation `s` under a film of weight `film`, over a substrate of `porosity` — darker by Lekner
// and Dorf scaled by the porosity, its roughness lowered towards the film's — before any light
// sees it, so the sun's lobe, the sky's ambient, the shadows and the reflection all read the wet
// surface. s = 0 is the sample untouched, to the bit.
MaterialSample terrain_wet(MaterialSample sampled, float s, float film, float porosity) {
  if (!(s > 0.0)) return sampled;
  sampled.albedo = wet_ground_albedo(sampled.albedo, s, porosity);
  sampled.roughness = wet_ground_roughness(sampled.roughness, s, film);
  return sampled;
}

// `shore_s` and `shore_film` (S99): the saturation the water stage found at the point (0 and 0, the
// default, everywhere else and for every other caller), applied by `terrain_wet` over the blend's
// porosity last.
MaterialSample sample_terrain_material(SceneGeometry geo, ResolveMaterial root, float2 map_uv,
                                      float2 map_dx, float2 map_dy, float3 point, float3 n,
                                      float3 dp_dx, float3 dp_dy, float distance_m, bool filtered,
                                      out float3 shading_normal, bool variance_enabled = true,
                                      float shore_s = 0.0, float shore_film = 0.0) {
  // The substrate's porosity under the pixel, by the layers' blend (0.5, the schema's default,
  // where no layer says).
  float shore_porosity = 0.5;
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
  // The pixel's footprint on the ground, millimetres (zero for the path tracer, whose rays carry
  // none): the mask's mip and R99's glint cells follow it.
  const float footprint_mm = 1000.0 * max(length(dp_dx), length(dp_dy));
  // R99's two blend-level outputs: the specular lobe's metallic word (snow) and the wind sand's
  // share, the detail share the resolve weights W42's ripples by. Zero unless a layer responds.
  float metallic = 0.0;
  float detail = 0.0;
  const uint k_responses =
      k_material_terrain_snow | k_material_terrain_sand | k_material_terrain_beach;
  if ((rec.flags & k_terrain_masked) != 0) {
    // **Ground materials by place**: the tile's table names up to four palette layers and the mask
    // weighs them; the mip follows the pixel's footprint on the ground (none for the path tracer,
    // whose rays carry no footprint, as its texture reads take level 0).
    const float level = filtered && footprint_mm > 0.0
                            ? log2(max(footprint_mm / float(rec.mask_spacing_mm), 1.0))
                            : 0.0;
    const TerrainMaskSample mask = terrain_mask_sample(rec, geo.eye, point, level);
    if (mask.valid) {
      const float wet = max(rec.wetness, mask.wetness);
      TerrainLayerSample layers[4];
      float weight[4];
      uint response[4];
      float total = 0.0;
      bool any_height = false;
      bool any_variance = false;
      bool any_snow = false;
      bool any_wind = false;
      [unroll] for (uint c = 0; c < 4; ++c) {
        const uint index = (mask.table >> (8 * c)) & 0xFFu;
        weight[c] = index == 0xFFu || index >= rec.layer_count ? 0.0 : mask.weights[c];
        layers[c].colour = float3(0.0);
        layers[c].roughness = 0.0;
        layers[c].bump = float3(0.0);
        layers[c].height = 0.5;
        response[c] = 0u;
        if (weight[c] <= 0.0) continue;
        ResolveMaterial layer = table[rec.first_layer + index];
        // R136, layers by weight: a stochastic layer under 1/128 of the pixel is not read once
        // the pixel is past a cell of it; its coverage still counts, so the mask covers what it
        // did and the others take its share through the blend's normalization.
        const uint far_reads = k_material_terrain_stochastic | k_material_mipped;
        if (filtered && (layer.flags & far_reads) == far_reads) {
          const float cells = sqrt(length(cross(dp_dx, dp_dy))) * 1000.0 /
                              (terrain_layer_pitch_mm(layer) * k_terrain_hex_cell_uv);
          const float coverage = weight[c];
          weight[c] = terrain_far_layer_weight(
              weight[c], smoothstep(k_terrain_one_tile_from, k_terrain_one_tile_at, cells));
          if (weight[c] <= 0.0) {
            total += coverage;
            continue;
          }
          total += coverage - weight[c];
        }
        if (!variance_enabled) layer.flags &= ~k_material_normal_variance;
        const bool variance = (layer.flags & k_material_normal_variance) != 0;
        layers[c] = terrain_layer_sample(geo, layer, map_uv, map_dx, map_dy, point, n, axes, dp_dx,
                                         dp_dy, variance ? 1.0 : fade, filtered);
        layers[c] = terrain_filter_sample(layers[c], layer, fade);
        layers[c] = terrain_formation_band(rec, layer, layers[c], geo.eye, point, dp_dx, dp_dy);
        // R99: the layer's response, on its record word, once per layer.
        response[c] = layer.flags & k_responses;
        if (response[c] != 0u) {
          layers[c] = terrain_respond(geo.eye, layer, layers[c], point, n, footprint_mm, filtered,
                                      wet);
          any_snow = any_snow || (response[c] & k_material_terrain_snow) != 0u;
          any_wind = any_wind || response[c] == k_material_terrain_sand;
        }
        any_variance = any_variance || variance;
        result.normal_filter = result.normal_filter || variance;
        // Snow lies on what is under it by its thickness, through the height blend (R99).
        any_height = any_height || (layer.flags & k_material_terrain_height) != 0 ||
                     (response[c] & k_material_terrain_snow) != 0u;
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
        float pores = 0.0;
        float alpha2 = 0.0;
        float slope_second = 0.0;
        float lobe = 0.0;
        float wind = 0.0;
        [unroll] for (uint c = 0; c < 4; ++c) {
          if (blended[c] <= 0.0) continue;
          const float w = blended[c] / sum;
          const uint index = (mask.table >> (8 * c)) & 0xFFu;
          colour += w * layers[c].colour;
          roughness += w * layers[c].roughness;
          bump += w * layers[c].bump;
          const float a = brdf_alpha(layers[c].roughness);
          alpha2 += w * a * a;
          slope_second += w * dot(layers[c].bump, layers[c].bump);
          // A beach's wetness is its own (Lekner and Dorf, applied to its colour above), so its
          // share takes none of the porosity rule's.
          const ResolveMaterial entry = table[rec.first_layer + index];
          porosity += w * ((response[c] & k_material_terrain_beach) != 0u
                               ? 0.0
                               : terrain_layer_porosity(entry));
          pores += w * terrain_layer_porosity(entry);
          if (any_snow)
            lobe += w * ((response[c] & k_material_terrain_snow) != 0u ? terrain_snow_lobe(entry)
                                                                         : 1.0);
          if (response[c] == k_material_terrain_sand) wind += w;
        }
        result.albedo = lerp(result.albedo, colour, cover);
        const float root_alpha = brdf_alpha(result.roughness);
        result.roughness = lerp(result.roughness, roughness, cover);
        slope = bump * cover;
        if (any_variance) {
          result.roughness = terrain_filter_combination(
              lerp(root_alpha * root_alpha, alpha2, cover), slope_second * cover, slope);
        }
        porosity = lerp(0.5, porosity, cover);
        shore_porosity = lerp(0.5, pores, cover);
        // Wetness (Lagarde 2013): porous ground darkens by up to 0.8 and loses up to 0.4 of its
        // roughness; rock barely changes. A separate quantity, so no substrate turns glossy dry.
        const float darkening = lerp(1.0, 0.2, porosity);
        result.albedo *= lerp(1.0, darkening, wet);
        result.roughness *= lerp(1.0, darkening, 0.5 * wet);
        // R99: the snow's lobe weight, through the metallic word with the diffuse kept; the wind
        // sand's share, for the resolve's W42 detail.
        if (any_snow) {
          metallic = terrain_lobe_metallic(dot(result.albedo, float3(1.0 / 3.0)),
                                           lerp(1.0, lobe, cover));
          result.albedo /= 1.0 - metallic;
        }
        if (any_wind) detail = wind * cover;
      } else {
        result.albedo *= lerp(1.0, 0.6, wet);
        result.roughness *= lerp(1.0, 0.6, 0.5 * wet);
      }
    }
  } else {
    // R99 in W68's layers: each responds as a palette layer does (no wetness here), and the lobe
    // and the wind share follow the layers' coverage as everything else does. Both stay exactly
    // 1 and 0 through layers without a response.
    bool any_snow = false;
    float lobe = 1.0;
    float wind = 0.0;
    for (uint i = 0; i < rec.layer_count; ++i) {
      ResolveMaterial layer = table[rec.first_layer + i];
      if (!variance_enabled) layer.flags &= ~k_material_normal_variance;
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
      const bool variance = (layer.flags & k_material_normal_variance) != 0;
      result.normal_filter = result.normal_filter || variance;
      TerrainLayerSample s = terrain_layer_sample(geo, layer, map_uv, map_dx, map_dy, point,
                                                  n, axes, dp_dx, dp_dy,
                                                  variance ? 1.0 : fade, filtered);
      s = terrain_filter_sample(s, layer, fade);
      s = terrain_formation_band(rec, layer, s, geo.eye, point, dp_dx, dp_dy);
      const uint response = layer.flags & k_responses;
      if (response != 0u) {
        s = terrain_respond(geo.eye, layer, s, point, n, footprint_mm, filtered, 0.0);
        any_snow = any_snow || (response & k_material_terrain_snow) != 0u;
        lobe = lerp(lobe, (response & k_material_terrain_snow) != 0u ? terrain_snow_lobe(layer) : 1.0,
                    coverage);
        wind = lerp(wind, response == k_material_terrain_sand ? 1.0 : 0.0, coverage);
      } else if (any_snow || wind > 0.0) {
        lobe = lerp(lobe, 1.0, coverage);
        wind = lerp(wind, 0.0, coverage);
      }
      const float a = brdf_alpha(result.roughness);
      const float b = brdf_alpha(s.roughness);
      const float second = lerp(dot(slope, slope), dot(s.bump, s.bump), coverage);
      result.albedo = lerp(result.albedo, s.colour, coverage);
      result.roughness = lerp(result.roughness, s.roughness, coverage);
      shore_porosity = lerp(shore_porosity, terrain_layer_porosity(layer), coverage);
      slope = lerp(slope, s.bump, coverage);
      if (result.normal_filter)
        result.roughness = terrain_filter_combination(lerp(a * a, b * b, coverage), second, slope);
    }
    if (any_snow) {
      metallic = terrain_lobe_metallic(dot(result.albedo, float3(1.0 / 3.0)), lobe);
      result.albedo /= 1.0 - metallic;
    }
    detail = wind;
  }
  result.metallic = metallic;
  result.emissive = float3(0.0);
  result.detail_mask = detail;
  shading_normal = normalize(n + slope);
  return terrain_wet(result, shore_s, shore_film, shore_porosity);
}
#endif
