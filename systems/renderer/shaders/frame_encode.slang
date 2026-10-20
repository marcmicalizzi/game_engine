// The output encode after the frame passes (systems/renderer/frame_pass.h; docs/subsystems/
// renderer.md, "Frame passes"). A frame with a pass registered is resolved into a linear image —
// the exposed radiance, `gfx::DisplayEncoding::Linear` — the passes composite onto it, and this
// pass draws it into the frame's target through `display_output` (display.slang), the one encode
// the resolve itself would have called: the display's curve from the sky's shoulder, the transfer
// and the dither at the pixel's place in its view. So the picture is still quantized once
// (ADR-0052), and a pass that changes nothing draws what the resolve would have, to the precision
// of the half the linear image holds.
//
// A pixel whose alpha is 0 is the stand-in sky's clear, which the resolve discarded and no pass
// touched: it is discarded here too, so the target keeps the clear this pass began with, byte for
// byte, as the resolve keeps it.
//
// **The UI's seam** (ADR-0054 decision 3, resolved 2026-10-19; renderer.md, "The overlay"). An HDR
// frame with an overlay comes here with the overlay drawn into a UI layer of its own — premultiplied
// linear colour, 1.0 the UI's white, and coverage — and this pass composites it **between the tone
// curve and the transfer**: the scene toned to the display in units of paper white, under the layer,
// then the one transfer. So UI white is paper white exactly, the UI is never toned or clipped,
// transparency blends in linear light, and nothing is encoded twice. A pixel the layer does not touch
// is encoded exactly as it was without one.

#include "../../../domain/gfx/shaders/display.slang"

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];

// Mirrors scene_renderer.cpp's FrameEncodePush (36 bytes).
struct FrameEncodePush {
  uint view_x;  // the view's rectangle's corner in the target: the dither is the view's own
  uint view_y;
  uint texture;   // bindless sampled slot of the linear image
  uint encoding;  // gfx::DisplayEncoding
  uint steps;     // the target's code steps the dither is one of; 0 for none
  float knee;     // the display curve's knee: the sky's shoulder, 1 with no sky
  float paper_white_nits;
  float peak_nits;
  uint ui_texture;  // bindless sampled slot of the overlay's UI layer; k_no_ui for none
};

static const uint k_no_ui = 0xFFFFFFFFu;

[[vk::push_constant]] ConstantBuffer<FrameEncodePush> g_push;

struct EncodeOut {
  float4 position : SV_Position;
};

[shader("vertex")]
EncodeOut vs_encode(uint vertex_id : SV_VertexID) {
  // One triangle covering the viewport, as the resolve's.
  EncodeOut out;
  const float x = vertex_id == 1 ? 3.0 : -1.0;
  const float y = vertex_id == 2 ? 3.0 : -1.0;
  out.position = float4(x, y, 0.0, 1.0);
  return out;
}

// `display_output` with the UI layer `ui` (premultiplied linear colour, coverage in alpha) composited
// between its curve and its transfer: the same tone, in units of the display's white, the same
// transfer and dither after. In Rec. 709 linear light, which is the composite in BT.2020's too, since
// one is a linear map of the other.
float3 encode_under_ui(float3 exposed, float4 ui, float knee, uint2 pixel, DisplayTarget target) {
  const float3 e = max(exposed, 0.0);
  if (target.encoding == k_display_linear) {
    return min(max(e * (1.0 - ui.a) + ui.rgb, 0.0), 65504.0);
  }
  if (target.encoding == k_display_sdr) {
    const float3 toned = float3(display_tone1(e.x, knee, 1.0), display_tone1(e.y, knee, 1.0),
                                display_tone1(e.z, knee, 1.0));
    return display_output(toned * (1.0 - ui.a) + ui.rgb, pixel, target.steps);
  }
  const float ceiling = max(target.peak_nits / max(target.paper_white_nits, 1e-6), 1.0);
  const float3 toned = float3(display_tone1(e.x, knee, ceiling), display_tone1(e.y, knee, ceiling),
                              display_tone1(e.z, knee, ceiling));
  const float3 nits = (toned * (1.0 - ui.a) + ui.rgb) * target.paper_white_nits;
  if (target.encoding == k_display_scrgb) return nits / 80.0;
  return display_dithered(pq_encode(bt709_to_bt2020(nits)), pixel, target.steps);
}

[shader("fragment")]
float4 fs_encode(EncodeOut in) : SV_Target {
  const uint2 pixel = uint2(in.position.xy);
  const float4 value = g_textures[g_push.texture].Load(int3(int2(pixel), 0));
  DisplayTarget target;
  target.encoding = g_push.encoding;
  target.steps = g_push.steps;
  target.paper_white_nits = g_push.paper_white_nits;
  target.peak_nits = g_push.peak_nits;
  const uint2 noise_pixel = pixel - uint2(g_push.view_x, g_push.view_y);
  if (g_push.ui_texture != k_no_ui) {
    // The layer's pixel, if the UI touched it: composited over the scene, or over the stand-in
    // sky's light where no surface was drawn (an alpha-0 pixel's colour is that light, the clear).
    const float4 ui = g_textures[g_push.ui_texture].Load(int3(int2(pixel), 0));
    if (ui.a > 0.0 || any(ui.rgb > 0.0)) {
      return float4(encode_under_ui(value.rgb, ui, g_push.knee, noise_pixel, target), 1.0);
    }
  }
  if (value.a == 0.0) discard;
  return float4(display_output(value.rgb, g_push.knee, noise_pixel, target), 1.0);
}
