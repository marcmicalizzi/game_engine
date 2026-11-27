#include <domain/gfx/visibility_resolve.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/surface_reflect.h>

namespace engine::renderer {

namespace {

tunables::Int surfaces{
    "renderer.reflect.surfaces", 1, 0, 3,
    "A smooth surface (roughness under 0.2: glass, polished stone, metal) reflects the scene "
    "through one ray along its mirror direction where the resolve traces: 0 off (the ambient, as "
    "before), 1 on, 2 on in even frames and off in odd ones, 3 the diagnostic, each traced "
    "pixel red where its ray met nothing and green where it met a surface (renderer.md, "
    "Reflections on smooth surfaces; R71)"};
tunables::Float shell_m{
    "renderer.reflect.shell_m", 32.0, 0.0, 1024.0,
    "Metres round the eye whose geometry the frame's ray tracing structures hold as casters "
    "whatever the frustum, for the smooth surfaces' reflections, in a scene that has one (0: none, "
    "the frame's cut alone) (renderer.md, Reflections on smooth surfaces; R71)"};

}  // namespace

bool surface_reflect_tunable(u64 frame_index) noexcept {
  switch (surfaces.get()) {
    case 0: return false;
    case 2: return (frame_index & 1u) == 0u;
    default: return true;
  }
}

u32 surface_reflect_flags(u64 frame_index) noexcept {
  if (!surface_reflect_tunable(frame_index)) return 0u;
  return surfaces.get() == 3
             ? gfx::k_shadow_surface_reflect | gfx::k_shadow_surface_reflect_diagnostic
             : gfx::k_shadow_surface_reflect;
}

f32 surface_reflect_shell_m() noexcept { return static_cast<f32>(shell_m.get()); }

}  // namespace engine::renderer
