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

// ---- indirect dispatches (T165) ----------------------------------------------------------------
//
// A dispatch whose count the GPU writes folds the same way: its `DispatchIndirectArgs` are
// {min(groups, pitch), rows, 1}, never {groups, 1, 1}. Under one row that is the dispatch it was.
// The reader rejects the last row's overhang against the count it already knows or is given.

// The arguments of `groups` workgroups, folded: what a pass that knows the whole count writes.
uint3 folded_args(uint groups) {
  const uint rows = (groups + k_dispatch_pitch - 1u) / k_dispatch_pitch;
  return uint3(min(groups, k_dispatch_pitch), max(rows, 1u), 1u);
}

// The arguments grown one workgroup at a time, by whoever appends workgroup `group` (the count
// before its own add): x counts the first row, y every row after it. `args` starts at {0, 1, 1}, so
// the words are `folded_args` of the count at every moment, and the cost is the one add x always
// took, which past the first row moves to y once a row.
void folded_append(Atomic<uint>* args, uint group) {
  if (group < k_dispatch_pitch) {
    args[0].add(1u);
  } else if (group % k_dispatch_pitch == 0u) {
    args[1].add(1u);
  }
}
