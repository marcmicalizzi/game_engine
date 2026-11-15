// The mesher's entry (docs/subsystems/structures.md, "The mesher"): a valid description, its
// walls solved once, then the shell and the interior into the surfaces' parts.

#include "emit.h"

#include <domain/structures/structures.h>

namespace engine::structures {

bool solve(const structure::Description& d, structure::MeshScope scope,
           MeshPart (&parts)[k_surfaces], Report& report, std::string& error) {
  for (MeshPart& part : parts)
    part = {};
  report = {};
  if (!validate(d, error)) return false;
  detail::Context ctx;
  ctx.d = &d;
  ctx.scope = scope;
  ctx.parts = parts;
  detail::solve_walls(d, ctx.walls);
  detail::mesh_shell(ctx);
  detail::mesh_interior(ctx);
  for (u32 s = 0; s < k_surfaces; ++s) {
    report.surface_triangles[s] = parts[s].indices.size() / 3;
    report.triangles += report.surface_triangles[s];
  }
  if (report.triangles == 0) {
    error = "the description has nothing in this scope to mesh";
    return false;
  }
  return true;
}

}  // namespace engine::structures
