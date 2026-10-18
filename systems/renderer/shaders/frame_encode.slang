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

#include "../../../domain/gfx/shaders/display.slang"

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];

// Mirrors scene_renderer.cpp's FrameEncodePush (32 bytes).
struct FrameEncodePush {
  uint view_x;  // the view's rectangle's corner in the target: the dither is the view's own
  uint view_y;
  uint texture;   // bindless sampled slot of the linear image
  uint encoding;  // gfx::DisplayEncoding
  uint steps;     // the target's code steps the dither is one of; 0 for none
  float knee;     // the display curve's knee: the sky's shoulder, 1 with no sky
  float paper_white_nits;
  float peak_nits;
};

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

[shader("fragment")]
float4 fs_encode(EncodeOut in) : SV_Target {
  const uint2 pixel = uint2(in.position.xy);
  const float4 value = g_textures[g_push.texture].Load(int3(int2(pixel), 0));
  if (value.a == 0.0) discard;
  DisplayTarget target;
  target.encoding = g_push.encoding;
  target.steps = g_push.steps;
  target.paper_white_nits = g_push.paper_white_nits;
  target.peak_nits = g_push.peak_nits;
  const uint2 noise_pixel = pixel - uint2(g_push.view_x, g_push.view_y);
  return float4(display_output(value.rgb, g_push.knee, noise_pixel, target), 1.0);
}
