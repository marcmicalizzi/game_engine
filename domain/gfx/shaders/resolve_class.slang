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
// the same walk over the bodies' records and the banks' tree, answering "inside a record's square"
// where the resolve goes on to the record's level and its saturation. So it claims every pixel the
// resolve could wet and more (a pixel above a body's fringe, a bank's square past the stretch's
// own width), never fewer.
//
// The pixel's point is taken from its **depth word**, not from its triangle: the visibility word
// carries the depth the rasterizer (or the ray) wrote, and the point where that depth lies on the
// pixel's ray is three planes' intersection, with no fetch. It is not the resolve's point to the
// bit (that one is where the ray meets the triangle, off the 16-bit grid), so every square is
// grown by `margin`: half a metre and two thousandths of the point's distance, hundreds of times
// what the two points can differ by.
bool classify_wet(ResolveParams* params, uint material_flags, InstanceDesc instance, float2 source,
                  float depth) {
  if (params->water_shore_count == 0) return false;
  WaterShore* shores = (WaterShore*)params->water_shores;
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const bool ground = (material_flags & k_water_wet_materials) != 0 ||
                      (instance.flags & k_instance_uv_from_corner) != 0 || instance.terrain != 0 ||
                      instance.mesh == shores[0].terrain_mesh;
  if (!ground) return false;
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
  const float margin = 0.5 + 0.002 * length(q);
  uint s = 0;
  for (; s < params->water_shore_count; ++s) {
    const WaterShore shore = shores[s];
    if (water_shore_channel(shore)) break;  // the bodies' records end where the banks' tree starts
    const float3 local = position - instance_from_eye(instances[shore.instance], params->eye);
    if (all(local.xz >= shore.lo - margin) && all(local.xz <= shore.hi + margin)) return true;
  }
  if (s < params->water_shore_count && (params->shadow_flags & k_shadow_wet_banks_off) == 0) {
    const float3 local = position - instance_from_eye(instances[shores[s].instance], params->eye);
    while (s < params->water_shore_count) {
      const bool inside = all(local.xz >= shores[s].lo - margin) &&
                          all(local.xz <= shores[s].hi + margin);
      if (shores[s].stretch_a.w < 0.0) {
        // A node: its subtree, or past it by its escape index. Its level range is not asked.
        s = inside ? s + 1 : max(shores[s].material, s + 1);
        continue;
      }
      if (inside) return true;
      ++s;
    }
  }
  return false;
}
#endif

// The class of the view's output pixel `out_pixel`: `fs_resolve`'s own way to the pixel's
// visibility word and through it to the material's flags, and nothing past them.
uint classify_pixel(ResolveParams* params, uint2 out_pixel) {
  const bool resample = params->panini_x > 0.0;
  const uint out_width = resample ? params->out_width : params->width;
  const uint out_height = resample ? params->out_height : params->height;
  // The ramp is every pixel's, and a pixel outside the picture is the sky's.
  if (params->mode == 10) return k_resolve_class_sky;
  if (out_pixel.x >= out_width || out_pixel.y >= out_height) return k_resolve_class_sky;
  float2 source = float2(out_pixel) + 0.5;
  if (resample) {
    const float2 ndc = float2((source.x / float(out_width)) * 2.0 - 1.0,
                              1.0 - (source.y / float(out_height)) * 2.0);
    const float2 src_ndc = panini_source_ndc(params, ndc);
    source = float2((src_ndc.x * 0.5 + 0.5) * float(params->width),
                    (0.5 - src_ndc.y * 0.5) * float(params->height));
    if (source.x < 0.0 || source.y < 0.0) return k_resolve_class_sky;
  }
  const uint2 pixel = uint2(source);
  if (pixel.x >= params->width || pixel.y >= params->height) return k_resolve_class_sky;
  if (params->coverage != 0) {
    uint* tiles = (uint*)params->coverage;
    if (tiles[(pixel.y >> 5) * params->coverage_pitch + (pixel.x >> 5)] == 0) {
      return k_resolve_class_sky;
    }
  }
  uint64_t* visibility = (uint64_t*)params->visibility;
  const uint64_t value = visibility[pixel.y * params->width + pixel.x];
  if (value == 0) return k_resolve_class_sky;
  const uint pair = uint(value & 0xffffffffu) >> 8;
  uint2 entry;
  if (params->pairs != 0) {
    entry = ((uint2*)params->pairs)[pair];
  } else {
    entry = visible_entry(params->visible, entry_of_pair(params->pair_entries, pair));
  }
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const InstanceDesc instance = instances[entry.x];
  ResolveMaterial* materials = (ResolveMaterial*)params->materials;
  uint* cluster_materials = (uint*)params->cluster_materials;
  const uint flags = materials[cluster_materials[entry.y] + instance.material_base].flags;
#ifdef RESOLVE_WATER
  if ((flags & k_material_water) != 0) return k_resolve_class_water;
  if (classify_wet(params, flags, instance, source, asfloat(uint(value >> 32)))) {
    return k_resolve_class_wet;
  }
#endif
  if ((flags & 512u) != 0) return k_resolve_class_ground;  // k_material_terrain_layers
  if ((flags & k_material_masked) != 0) return k_resolve_class_masked;
  if (instance.deform != k_invalid_deform) return k_resolve_class_deformed;
  return k_resolve_class_rigid;
}

#endif  // RESOLVE_CLASS_SLANG
