#pragma once

// A compute dispatch whose workgroup count grows with the scene, folded into rows so that no
// dimension asks for more groups than the device allows (docs/subsystems/gfx.md, "A dispatch that
// grows with the scene", T160).
//
// Vulkan guarantees only 65,535 workgroups in each dimension of a dispatch
// (`maxComputeWorkGroupCount`). NVIDIA reports 2^31 - 1 on x, so a one-dimensional dispatch of a
// forest's 262,144 cull groups runs there and is undefined behaviour everywhere the limit is the
// guaranteed one (a conforming device may report exactly 65,535). So a dispatch over pairs,
// entries, slots or instances is `fold_dispatch(groups, caps)`: rows of `k_dispatch_pitch` groups
// on x and as many rows as it takes on y, which reaches 2^31 groups within the guaranteed limits.
// The shader rebuilds its index with `folded_group` or `folded_thread` (`shaders/dispatch.slang`)
// and rejects the overhang of the last row, which it already did for the overhang of the last
// workgroup. A dispatch under one row is the same one-dimensional dispatch it was, so a shader that
// folds still reads a plain `dispatch(n, 1, 1)` rightly.
//
// The pitch is fixed rather than the device's own limit on x so that every device runs the same
// dispatch and a count past 65,535 groups exercises the rows on the GPU the tests run on.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/requirements.h>

namespace engine::gfx {

// The groups in a row of a folded dispatch. Mirrored in `shaders/dispatch.slang`. Half of the
// guaranteed 65,535 and a power of two, so the shader's multiply is a shift.
inline constexpr u32 k_dispatch_pitch = 32768;
static_assert(k_dispatch_pitch <= k_workgroup_count);

struct DispatchGroups {
  u32 x = 0;
  u32 y = 0;
};

// `groups` workgroups as rows of `k_dispatch_pitch`: {groups, 1} under one row, {pitch, rows} past
// it. Asserts that the device takes both dimensions; past 2^31 groups the caller has a scene no
// pair index can name anyway.
inline DispatchGroups fold_dispatch(u64 groups, const DeviceCaps& caps) noexcept {
  DispatchGroups out;
  out.x = static_cast<u32>(groups < k_dispatch_pitch ? groups : k_dispatch_pitch);
  const u64 rows = (groups + k_dispatch_pitch - 1) / k_dispatch_pitch;
  out.y = static_cast<u32>(rows);
  ENGINE_ASSERT(out.x <= caps.max_compute_workgroup_count_x &&
                    rows <= u64{caps.max_compute_workgroup_count_y},
                "a folded dispatch asks for more workgroups than the device allows");
  (void)caps;
  return out;
}

// Records the folded dispatch of `groups` workgroups: what every call site does with the size.
inline void dispatch_folded(CommandList cb, u64 groups, const DeviceCaps& caps) noexcept {
  const DispatchGroups g = fold_dispatch(groups, caps);
  cb.dispatch(g.x, g.y, 1);
}

// ---- indirect dispatches (T165)
// ------------------------------------------------------------------
//
// A dispatch whose count the GPU writes is folded the same way, on the GPU: its arguments are
// `{min(groups, pitch), rows, 1}`, written by the pass that knows the count. Three forms:
// a pass that grows the count one workgroup at a time grows the two words (`folded_append` in
// dispatch.slang: the vertex path's expansion, the CLAS cache's passes); one that stores a count it
// knows whole stores them (`folded_args`: the cache's slot scan and eviction); and a count kept as
// a survivor count `{n, 1, 1}` that other passes read as a count is folded into arguments of its
// own by `fold_args.slang` (the deformed-vertex pool's runs, the software rasterizer). The reader
// rebuilds its index with `folded_group`/`folded_thread` and rejects the last row's overhang
// against the count. The arguments match `fold_dispatch(groups)` exactly, so under one row every
// dispatch is the one it was.

// Mirrors FoldParams in fold_args.slang: thread i folds the u32 count at `counts + i *
// count_stride` into the DispatchIndirectArgs at `args + i * args_stride`.
struct FoldArgsParams {
  u64 counts = 0;
  u64 args = 0;
  u32 count_stride = 0;
  u32 args_stride = 0;
  u32 n = 0;
  u32 pad = 0;
};
static_assert(sizeof(FoldArgsParams) == 32);
inline constexpr u32 k_fold_args_workgroup = 32;  // numthreads of fold_args_main

}  // namespace engine::gfx
