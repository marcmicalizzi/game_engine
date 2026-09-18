#pragma once

// Every Recast and Detour header this module uses, in one place.
//
// **The file is not called recast.h**, and that is not a style choice. Recast's own headers are
// `<Recast.h>` and `<Detour*.h>` with no directory prefix, and MSVC matches include names
// case-insensitively: a `src/recast.h` on the include path answers `#include <Recast.h>` with
// itself, `#pragma once` makes the second inclusion empty, and every Recast type is suddenly
// undeclared with no hint of why. The same file compiles on Linux.
//
// Three reasons it is one place rather than an include list per file, the first two learned the
// hard way in domain/physics (see src/jolt.h there):
//
//  1. Recast's headers have no directory prefix — `<Recast.h>`, `<DetourNavMesh.h>` — so the
//     engine's clang-format configuration sorts them into its *first* include group, ahead of
//     `<core/...>`, because the third-party regex matches known prefixes and these look like
//     nothing in particular. Putting them behind one engine-shaped header means the rule is
//     stated once, in a file `tools/dev.ps1 format` has nothing to reorder, instead of in a
//     comment that the next format run quietly undoes.
//  2. Detour's types change width with the flags its library was compiled with (DT_POLYREF64
//     doubles dtPolyRef; DT_VIRTUAL_QUERYFILTER changes dtQueryFilter's layout). Exactly one set
//     of translation units may see them, and `engine_nav` is the only target that links Detour.
//
// The headers are consumed as SYSTEM includes (see domain/nav/CMakeLists.txt), so their warnings
// never reach the engine's -Werror.

#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>
#include <Recast.h>
#include <RecastAlloc.h>

namespace engine::nav::detail {

// Recast reports progress and timings through a virtual context. The engine has its own log and
// its own profiler (core/log, core/profiling), and a rebuild running on a worker must not write
// to either from inside the backend, so the context is silent: `doLog` and the timer hooks are
// left at their do-nothing defaults. Recast still needs an instance — every pipeline function
// takes one and dereferences it.
class SilentContext final : public rcContext {
 public:
  SilentContext() noexcept : rcContext(false) {}
};

}  // namespace engine::nav::detail
