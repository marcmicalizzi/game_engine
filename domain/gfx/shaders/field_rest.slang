// The frame's field block and the far field at rest (docs/subsystems/renderer.md, "The field
// kind" and "The far field at rest"; gfx/flora_motion.h; R95, R152). No bindings and no entry
// points: the three passes that read the block include this — the pool pass (deform.slang), which
// poses a blade from it, and the pool's allocator (deform_alloc.slang) and the ray tracing chain's
// records (clas_records.slang), which ask it whether an instance stands at rest.
//
// **Past the rest distance a field does not sway, and the frame builds nothing for it.** A blade's
// share of the wind falls to zero over a band that ends `k_field_rest_margin_m` short of the
// distance (`field_fade`), so the last posed frame of a blade is its rest pose, to the bit: the
// pool pass returns the grid's position for a blade with no angle. An instance whose every root is
// at least the distance from the eye (`field_rests`) then takes no pool block, so every position
// reader draws it from the grid (`k_no_pool_slot`), and the chain's records treat it as the rigid
// instance it is this frame: its clusters' structures are the resident cache's, built once and
// referenced. The margin is why the allocator and the records may disagree about one instance by a
// float's rounding and the picture not know: a blade posed inside it is where the grid has it.

#ifndef ENGINE_FIELD_REST_SLANG
#define ENGINE_FIELD_REST_SLANG

#include "scene.slang"

// Mirrors gfx::FloraFieldFrame (240 bytes).
struct FloraFieldFrame {
  FrameEye eye;
  float4 wind;     // to.x, to.z, strength, the flutter's phase
  float4 waves[2]; // the four waves' (x, z) vectors, rad/m
  float4 phases;   // their phases at the eye
  float4 weights;  // their shares of the gust
  float4 walker;   // the walker's feet from the eye, radius (<= 0: none)
  float4 push;     // height, the push's most radians, margin; w the hand-off's clock's bits (G136)
  // The far field at rest: the distance (0: no rule), one over the fade band's width, the largest
  // |root| of any field mesh (mesh space), the walker's reach for an instance's origin (0: none).
  float4 rest;
  uint field_first;  // the scene's field records: deform entries [first, first + count)
  uint field_count;
  uint reserved[14];
};
static const uint k_field_frame_bytes = 240;      // gfx::k_flora_field_frame_bytes
static const float k_field_rest_margin_m = 0.25;  // gfx::k_field_rest_margin_m

// gfx::field_fade: the share of the wind a blade whose root is `d` metres from the eye takes.
float field_fade(FloraFieldFrame field, float d) {
  if (!(field.rest.x > 0.0)) return 1.0;
  return saturate((field.rest.x - k_field_rest_margin_m - d) * field.rest.y);
}

// gfx::field_rests, operation for operation: a field instance every one of whose roots is at least
// the rest distance from the eye. `precise`, as `relative_axis` is: the allocator and the records'
// passes each compile this, and must answer alike for the words they share.
bool field_rests(InstanceDesc instance, float3 from_eye, FloraFieldFrame field) {
  if (!(field.rest.x > 0.0)) return false;
  if (instance.deform - field.field_first >= field.field_count) return false;
  const uint role = instance.flags & k_instance_handoff_mask;
  if (role == k_instance_handoff_near) return false;
  float bound = field.rest.x;
  if (role == k_instance_handoff_far) bound = max(bound, handoff_to(instance));
  precise const float reach = field.rest.z * instance.scale_max;
  precise const float least = bound + reach;
  precise const float far2 = least * least;
  precise const float d2 =
      from_eye.x * from_eye.x + from_eye.y * from_eye.y + from_eye.z * from_eye.z;
  if (!(d2 >= far2)) return false;
  if (field.rest.w > 0.0) {
    precise const float dx = from_eye.x - field.walker.x;
    precise const float dz = from_eye.z - field.walker.z;
    precise const float near = field.rest.w + reach;
    precise const float w2 = dx * dx + dz * dz;
    precise const float near2 = near * near;
    if (w2 < near2) return false;
  }
  return true;
}

// The block a pass was handed (`field`: a FloraFieldFrame* or 0), asked about one instance.
// Two words of the block decide for every instance that is not a field's, before the eye is read.
bool field_rests_at(uint64_t field, InstanceDesc instance) {
  if (field == 0) return false;
  FloraFieldFrame* block = (FloraFieldFrame*)field;
  if (instance.deform - block->field_first >= block->field_count) return false;
  const FloraFieldFrame frame = block[0];
  return field_rests(instance, instance_from_eye(instance, frame.eye), frame);
}

#endif  // ENGINE_FIELD_REST_SLANG
