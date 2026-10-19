// The motion contract's container records (motion.h; docs/subsystems/flora.md, "Motion
// attributes").
#include <domain/flora/motion.h>

namespace engine::flora {

u16 encode_bend_weight(f32 weight) noexcept {
  const f32 w = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
  return static_cast<u16>(w * 65535.0f + 0.5f);
}

f32 decode_bend_weight(u16 weight) noexcept { return static_cast<f32>(weight) / 65535.0f; }

bool motion_records(const Motion& motion, const geometry::ClusterLodMesh& mesh,
                    Vector<geometry::ClusterFileMotionVertex>& out) {
  out.clear();
  if (motion.parts.size() > 0x10000u) return false;
  const Vector<u32>& source = mesh.mesh.vertex_source;
  const u32 n = motion.vertex_part.size();
  if (n == 0 || motion.vertex_weight.size() != n) return false;
  out.reserve(source.size());
  for (const u32 s : source) {
    // Every primitive's DAG is built over the solver's whole vertex array, and the merge shifts a
    // part's source indices by the vertex counts of the parts before it, so the solver's vertex is
    // the source index modulo that count (geometry::merge_cluster_lod).
    const u32 v = s % n;
    geometry::ClusterFileMotionVertex record;
    record.part = static_cast<u16>(motion.vertex_part[v]);
    record.weight = encode_bend_weight(motion.vertex_weight[v]);
    out.push_back(record);
  }
  return true;
}

}  // namespace engine::flora
