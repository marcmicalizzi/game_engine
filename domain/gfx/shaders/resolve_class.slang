// **The resolve's pixel classes** (docs/subsystems/renderer.md, "The resolve by pixel class";
// roadmap R151; docs/experiments/resolve-by-class-2026-11-12.md): what a pixel is, as far as the
// material resolve's cost goes, found from its visibility word by the resolve's own decode and
// nothing past it. No includes and no entry points: a file that includes this one has included
// visibility_resolve.slang first (`ResolveParams`, the Panini map, and with RESOLVE_WATER the shore
// records). The classify pass (resolve_classify.slang) writes the class as depth.

#ifndef RESOLVE_CLASS_SLANG
#define RESOLVE_CLASS_SLANG 1

// gfx::ResolveClass (domain/gfx/resolve_classes.h), value for value.
static const uint k_resolve_class_sky = 0u;       // no surface: the sky, or the open sea's plane
static const uint k_resolve_class_rigid = 1u;     // a rigid, opaque mesh
static const uint k_resolve_class_ground = 2u;    // a layered ground (k_material_terrain_layers)
static const uint k_resolve_class_masked = 3u;    // an alpha-tested material: the leaves
static const uint k_resolve_class_deformed = 4u;  // a deformed instance's opaque parts: trunks, blades
static const uint k_resolve_class_water = 5u;     // a water material
static const uint k_resolve_class_wet = 6u;       // ground within a shore's or a bank's reach
// gfx::k_resolve_class_depth_step and resolve_class_depth: class c is at (c + 1) sixteenths, exact
// in a float and in the viewport's transform. Depth 0 is no class's — it is what a fullscreen
// triangle has under the default depth range, the one draw's — and neither is the cleared 1.0.
static const float k_resolve_class_depth_step = 0.0625;
float resolve_class_depth(uint resolve_class) {
  return float(resolve_class + 1u) * k_resolve_class_depth_step;
}

#ifdef RESOLVE_WATER
// **Whether a pixel that is not water may still need the water module**: the wet ground (S99;
// `water_ground_wetness` in the resolve) darkens ground near water, on pixels that are not water,
// from the scene's shore records. This is that function's *bounds*, grown: the same ground test,
// the same walk over the bodies' records and the banks' tree, answering "inside a record's square
// **and within the heights the record can wet**" where the resolve goes on to the saturation. The
// heights are the resolve's own, record by record: a still body wets from its level up to its
// fringe (`wet_m`), a body with a swash or a tide the range its front and its tide can reach, a
// node of the banks' tree its level range, a stretch from two metres under its surface to its
// fringe above it, within its half width and the fringe's run. So it claims every pixel the
// resolve could wet and a margin round them, never fewer. (Until the row's second pass it asked
// the squares alone, and 75 to 98 % of the class was ground above a pond's fringe.)
//
// The pixel's point is taken from its **depth word**, not from its triangle: the visibility word
// carries the depth the rasterizer (or the ray) wrote, and the point where that depth lies on the
// pixel's ray is three planes' intersection, with no fetch. It is not the resolve's point to the
// bit (that one is where the ray meets the triangle, off the 16-bit grid), but both lie on the
// pixel's one ray, so they differ along it, by what moves the surface sideways (a step of the
// receiver's grid: a traced primary's depth is on the float triangle, the resolve's point on the
// grid's; a 256th of a pixel: a rasterizer snaps its corners) over the tangent of the angle the
// ray meets the surface at. Every square is grown by `margin`, half a metre and two thousandths
// of the point's distance, hundreds of times that at any angle a surface shows more than a sliver
// at. Every range of heights is grown by `rise`: a slack along the ray of a hundred steps of the
// receiver's grid (a centimetre at least) and two thousandths of the distance, which holds down
// to an angle of half a degree, times the ray's own slope of height; and beside it four steps of
// the grid, a two-thousandth of the distance and two centimetres for the point itself. A square's
// slack is not used for the heights: at half a metre along a ray that looks down from a few
// metres up it claimed most of a gentle bank (a pond's fringe is 0.3 m), which is what the class
// was made to stop doing. What is left unproven is a surface seen within half a degree of
// edge-on down a steep ray, which is less than a pixel wide.
//
// `k_shadow_wet_class_off` (gfx::k_shadow_wet_class_off, `renderer.resolve.class_wet_plain`, a
// measurement switch): no pixel is wet-class, so the wet reach's ground falls to its plain classes
// and, under `modules`, to the module without water; the pixels that then differ from the picture
// are the ones the water module wets, which is how the class's over-claim is counted.
static const uint k_shadow_wet_class_off = 1u << 20;

// A stretch's part of `classify_wet`: `water_channel_saturation`'s reach, grown.
bool classify_wet_stretch(WaterShore s, float3 local, float margin, float rise) {
  const float2 a = s.stretch_a.xy;
  const float2 d = s.stretch_b.xy - a;
  const float len2 = dot(d, d);
  const float u = len2 > 0.0 ? saturate(dot(local.xz - a, d) / len2) : 0.0;
  const float across = length(local.xz - (a + d * u));
  // The distance to the stretch's line is within `margin` of the resolve's, and its half width
  // there no more than the wider end's.
  const float reach = max(s.stretch_a.w, s.stretch_b.w) + k_bank_run * s.wet_m;
  if (!(across < reach + margin)) return false;
  const float lo = min(s.stretch_a.z, s.stretch_b.z) - 2.0;
  const float hi = max(s.stretch_a.z, s.stretch_b.z) + s.wet_m;
  return local.y > lo - rise && local.y < hi + rise;
}

// Whether a pixel could be wet-class at all: the resolve's own ground test, and that there is a
// record to ask. Integers and nothing else, so two shaders that ask it of one pixel agree.
bool classify_wettable(ResolveParams* params, uint material_flags, InstanceDesc instance) {
  if (params->water_shore_count == 0) return false;
  if ((params->shadow_flags & k_shadow_wet_class_off) != 0) return false;
  WaterShore* shores = (WaterShore*)params->water_shores;
  return (material_flags & k_water_wet_materials) != 0 ||
         (instance.flags & k_instance_uv_from_corner) != 0 || instance.terrain != 0 ||
         instance.mesh == shores[0].terrain_mesh;
}

bool classify_wet(ResolveParams* params, uint material_flags, InstanceDesc instance, float2 source,
                  float depth) {
  if (!classify_wettable(params, material_flags, instance)) return false;
  WaterShore* shores = (WaterShore*)params->water_shores;
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  // clip = view_proj * (q, 1) with q the point less the view's eye; (ndc.x, ndc.y, depth) are
  // clip.xyz over clip.w, so (row_i.xyz - c_i row_w.xyz) . q = c_i row_w.w - row_i.w for the three.
  const float3 c = float3(source.x / float(params->width) * 2.0 - 1.0,
                          1.0 - source.y / float(params->height) * 2.0, depth);
  const float4 rw = params->view_proj[3];
  float3 a[3];
  float3 b;
  for (uint i = 0; i < 3; ++i) {
    const float4 r = params->view_proj[i];
    a[i] = r.xyz - c[i] * rw.xyz;
    b[i] = c[i] * rw.w - r.w;
  }
  const float3 c12 = cross(a[1], a[2]);
  const float det = dot(a[0], c12);
  if (!(abs(det) > 0.0)) return true;  // no point to test: claimed
  const float3 q = (b.x * c12 + b.y * cross(a[2], a[0]) + b.z * cross(a[0], a[1])) / det;
  const float3 position = q + params->camera.xyz;
  const float distance = length(q);
  const float margin = 0.5 + 0.002 * distance;
  MeshDesc* meshes = (MeshDesc*)params->mesh;
  const float grid = meshes[instance.mesh].quant.w * instance.scale_max;
  const float along = max(100.0 * grid, 0.01) + 0.002 * distance;
  const float rise = along * abs(q.y) / max(distance, 1e-3) + 4.0 * grid + 0.0005 * distance + 0.02;
  uint s = 0;
  for (; s < params->water_shore_count; ++s) {
    const WaterShore shore = shores[s];
    if (water_shore_channel(shore)) break;  // the bodies' records end where the banks' tree starts
    const float3 local = position - instance_from_eye(instances[shore.instance], params->eye);
    if (any(local.xz < shore.lo - margin) || any(local.xz > shore.hi + margin)) continue;
    // The heights the resolve asks of this record (`water_ground_wetness`): the fringe over a
    // still level, or where the front and the tide can reach and the fringe above.
    float lo = 0.0;
    float hi = shore.wet_m;
    if (water_swash_on(shore) || shore.tide_m > 0.0) {
      lo = -shore.tide_m - 1.5 * shore.run_up_m;
      hi = max(shore.wet_m, 1.5 * shore.run_up_m) + shore.tide_m;
    }
    if (local.y >= lo - rise && local.y <= hi + rise) return true;
  }
  if (s < params->water_shore_count && (params->shadow_flags & k_shadow_wet_banks_off) == 0) {
    const float3 local = position - instance_from_eye(instances[shores[s].instance], params->eye);
    while (s < params->water_shore_count) {
      const bool inside = all(local.xz >= shores[s].lo - margin) &&
                          all(local.xz <= shores[s].hi + margin);
      const float4 head = shores[s].stretch_a;
      if (head.w < 0.0) {
        // A node: its subtree, or past it by its escape index (its square, and its level range).
        const bool level = local.y >= head.x - rise && local.y <= head.y + rise;
        s = inside && level ? s + 1 : max(shores[s].material, s + 1);
        continue;
      }
      if (inside && classify_wet_stretch(shores[s], local, margin, rise)) return true;
      ++s;
    }
  }
  return false;
}
#endif

// What a pixel's visibility word and its material's flags say of it, with no float in any
// decision but the Panini map's: the class it has if it is not wet (`base`), and whether it could
// be wet-class (`wettable`), with what `classify_wet` then asks.
struct PixelFacts {
  uint base;
  bool wettable;
  uint flags;
  InstanceDesc instance;
  float2 source;
  float depth;
};

// `fs_resolve`'s own way to the pixel's visibility word and through it to the material's flags,
// and nothing past them.
PixelFacts classify_pixel_facts(ResolveParams* params, uint2 out_pixel) {
  PixelFacts facts = {};
  facts.base = k_resolve_class_sky;
  const bool resample = params->panini_x > 0.0;
  const uint out_width = resample ? params->out_width : params->width;
  const uint out_height = resample ? params->out_height : params->height;
  // The ramp is every pixel's, and a pixel outside the picture is the sky's.
  if (params->mode == 10) return facts;
  if (out_pixel.x >= out_width || out_pixel.y >= out_height) return facts;
  float2 source = float2(out_pixel) + 0.5;
  if (resample) {
    const float2 ndc = float2((source.x / float(out_width)) * 2.0 - 1.0,
                              1.0 - (source.y / float(out_height)) * 2.0);
    const float2 src_ndc = panini_source_ndc(params, ndc);
    source = float2((src_ndc.x * 0.5 + 0.5) * float(params->width),
                    (0.5 - src_ndc.y * 0.5) * float(params->height));
    if (source.x < 0.0 || source.y < 0.0) return facts;
  }
  const uint2 pixel = uint2(source);
  if (pixel.x >= params->width || pixel.y >= params->height) return facts;
  if (params->coverage != 0) {
    uint* tiles = (uint*)params->coverage;
    if (tiles[(pixel.y >> 5) * params->coverage_pitch + (pixel.x >> 5)] == 0) return facts;
  }
  uint64_t* visibility = (uint64_t*)params->visibility;
  const uint64_t value = visibility[pixel.y * params->width + pixel.x];
  if (value == 0) return facts;
  const uint pair = uint(value & 0xffffffffu) >> 8;
  uint2 entry;
  if (params->pairs != 0) {
    entry = ((uint2*)params->pairs)[pair];
  } else {
    entry = visible_entry(params->visible, entry_of_pair(params->pair_entries, pair));
  }
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  facts.instance = instances[entry.x];
  ResolveMaterial* materials = (ResolveMaterial*)params->materials;
  uint* cluster_materials = (uint*)params->cluster_materials;
  facts.flags = materials[cluster_materials[entry.y] + facts.instance.material_base].flags;
  facts.source = source;
  facts.depth = asfloat(uint(value >> 32));
#ifdef RESOLVE_WATER
  if ((facts.flags & k_material_water) != 0) {
    facts.base = k_resolve_class_water;
    return facts;
  }
  facts.wettable = classify_wettable(params, facts.flags, facts.instance);
#endif
  if ((facts.flags & 512u) != 0) {  // k_material_terrain_layers
    facts.base = k_resolve_class_ground;
  } else if ((facts.flags & k_material_masked) != 0) {
    facts.base = k_resolve_class_masked;
  } else if (facts.instance.deform != k_invalid_deform) {
    facts.base = k_resolve_class_deformed;
  } else {
    facts.base = k_resolve_class_rigid;
  }
  return facts;
}

// The class of the view's output pixel `out_pixel`: what the classify pass writes.
uint classify_pixel(ResolveParams* params, uint2 out_pixel) {
  const PixelFacts facts = classify_pixel_facts(params, out_pixel);
#ifdef RESOLVE_WATER
  if (facts.wettable &&
      classify_wet(params, facts.flags, facts.instance, facts.source, facts.depth)) {
    return k_resolve_class_wet;
  }
#endif
  return facts.base;
}

// **Whether a fragment of a class's draw may be of that class** (the class exit at the top of
// `fs_resolve`): the draw's class is the fragment's own depth, and the pixel's is asked of its
// facts alone — its class by flags, or the wet class where it could be wet — never of
// `classify_wet`'s floats. So the answer is the same in every shader that asks it of one pixel:
// the classify pass's module decides which pixels are wet, once, and the depth test holds every
// draw to it. A second module that asked the floats again and rounded one otherwise would discard
// the pixel in the one draw whose depth test it passes, and nothing would draw it. What this
// leaves running is a lane of plain ground in the wet class's draw and the reverse, where the two
// meet inside a quad: lines, not areas.
bool resolve_class_may_draw(ResolveParams* params, uint2 out_pixel, float class_depth) {
  const PixelFacts facts = classify_pixel_facts(params, out_pixel);
  if (resolve_class_depth(facts.base) == class_depth) return true;
  return facts.wettable && class_depth == resolve_class_depth(k_resolve_class_wet);
}

#endif  // RESOLVE_CLASS_SLANG
