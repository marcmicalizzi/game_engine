// **The resolve's pixel classes, written as depth** (docs/subsystems/renderer.md, "The resolve by
// pixel class"; roadmap R151; the grounding, docs/experiments/resolve-by-class-2026-11-12.md).
//
// The material resolve is one fullscreen draw of one fragment shader that holds every kind of
// pixel, and a fragment shader runs in groups: a group with a leaf pixel and a ground pixel in it
// runs both kinds' code for as long as the dearer takes. A visibility buffer can sort its pixels
// before it shades them. This pass does the sorting: one fullscreen triangle a view whose fragment
// stage reads the pixel's visibility word, decodes its pair to an instance and a cluster and the
// cluster to its material's flags — the tables the resolve itself decodes through — and writes the
// pixel's **class as depth**, `resolve_class_depth(class)`. The resolve is then drawn once
// per class with the fullscreen triangle *at that class's depth* (the viewport's depth range) and
// a depth-equal test with no depth write, so the fixed-function test rejects every other class's
// pixels before they are shaded (Karis 2021, the material depth pass).
//
// **Any partition is exact.** Which draw shades a pixel changes nothing the pixel computes, so a
// pixel this pass classes differently from what the resolve would find (it cannot: the decode is
// the resolve's own, line for line, through this file's include of it) would still be drawn the
// bytes it was. What must hold is that every pixel gets exactly one class and every class is
// drawn; the renderer's test holds both (resolve_class_tests.cpp).
//
// The one class that is *not* free to be wrong is `k_resolve_class_wet`, which only the water
// variant of this file writes (resolve_classify_water.slang): see `classify_wet` in
// resolve_class.slang, where the classes and `classify_pixel` are.
//
// It includes the resolve's file for `ResolveParams`, the push block and the Panini map, as the
// half pass does (resolve_half_sun.slang); Slang emits only what the entry points reach, so the
// module is a few kilobytes.

#define RESOLVE_HALF_PASS 1
#include "visibility_resolve.slang"
#include "resolve_class.slang"

[shader("vertex")]
FullscreenOut vs_classify(uint vertex_id : SV_VertexID) {
  // The resolve's own triangle (vs_fullscreen): (-1,-1), (3,-1), (-1,3).
  FullscreenOut out;
  const float x = vertex_id == 1 ? 3.0 : -1.0;
  const float y = vertex_id == 2 ? 3.0 : -1.0;
  out.position = float4(x, y, 0.0, 1.0);
  return out;
}

[shader("fragment")]
float fs_classify(FullscreenOut in) : SV_Depth {
  ResolveParams* params = (ResolveParams*)g_push.params;
  const uint2 out_pixel = uint2(in.position.xy) - uint2(params->view_x, params->view_y);
  return resolve_class_depth(classify_pixel(params, out_pixel));
}
