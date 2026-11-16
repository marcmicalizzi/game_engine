// The shader side of a folded dispatch (domain/gfx/dispatch.h, `gfx::fold_dispatch`; gfx.md, "A
// dispatch that grows with the scene"): rows of `k_dispatch_pitch` workgroups on x, as many rows
// as it takes on y, so no dimension passes Vulkan's guaranteed 65,535. A reader rebuilds its index
// here and rejects the last row's overhang with the bound it already checks. A dispatch of one row
// is a plain one-dimensional dispatch, which these read the same way (y is zero).

// Mirrors gfx::k_dispatch_pitch.
static const uint k_dispatch_pitch = 32768u;

// The workgroup's index in the folded dispatch.
uint folded_group(uint3 group) {
  return group.y * k_dispatch_pitch + group.x;
}

// The thread's index in the folded dispatch, for workgroups of `width` threads on x.
uint folded_thread(uint3 thread_id, uint width) {
  return thread_id.y * (k_dispatch_pitch * width) + thread_id.x;
}
