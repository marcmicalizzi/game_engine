#include <domain/geometry/zero_area_repair.h>

#include <utility>

namespace engine::geometry {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

bool same_point(Vec3 a, Vec3 b) noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }

}  // namespace

bool spans_zero_area(Vec3 a, Vec3 b, Vec3 c) noexcept {
  const Vec3 normal = cross(b - a, c - a);
  return normal.x == 0.0f && normal.y == 0.0f && normal.z == 0.0f;
}

bool repair_zero_area_triangles(std::span<const Vec3> positions, Vector<u32>& indices,
                                std::span<ZeroAreaRepairRange> ranges, ZeroAreaRepairReport& report,
                                std::string* error) {
  report = ZeroAreaRepairReport{};
  const u32 vertex_count = static_cast<u32>(positions.size());
  u32 previous_end = 0;
  for (u32 r = 0; r < ranges.size(); ++r) {
    const ZeroAreaRepairRange& range = ranges[r];
    const std::string which = "zero-area repair: run " + std::to_string(r);
    if (range.first % 3 != 0 || range.count % 3 != 0)
      return fail(error, which + " is not a whole number of triangles");
    if (u64{range.first} + range.count > indices.size())
      return fail(error, which + " runs past the " + std::to_string(indices.size()) + " indices");
    if (range.first < previous_end) return fail(error, which + " overlaps or precedes the last");
    previous_end = range.first + range.count;
    for (u32 i = range.first; i < range.first + range.count; ++i) {
      if (indices[i] >= vertex_count) {
        return fail(error, which + ": index " + std::to_string(indices[i]) + " is outside the " +
                               std::to_string(vertex_count) + " vertices");
      }
    }
  }

  // One pass over every run's triangles in index order, marking the ones to drop. A clean mesh
  // costs exactly this pass and nothing is copied.
  Vector<u8> drop(static_cast<u32>(indices.size() / 3), u8{0});
  for (const ZeroAreaRepairRange& range : ranges) {
    u32 droppable = 0;
    u32 last_droppable = 0;
    for (u32 i = range.first; i < range.first + range.count; i += 3) {
      const u32 a = indices[i];
      const u32 b = indices[i + 1];
      const u32 c = indices[i + 2];
      if (a == b || b == c || a == c) {
        ++report.repeated;
        continue;
      }
      const Vec3 pa = positions[a];
      const Vec3 pb = positions[b];
      const Vec3 pc = positions[c];
      if (!spans_zero_area(pa, pb, pc)) continue;
      ++report.triangles;
      if (same_point(pa, pb) || same_point(pb, pc) || same_point(pa, pc))
        ++report.coincident;
      else
        ++report.collinear;
      drop[i / 3] = 1;
      ++droppable;
      last_droppable = i / 3;
    }
    // A run is never emptied: when every triangle it has is one to drop, its last one stays.
    if (droppable != 0 && droppable * 3 == range.count) {
      drop[last_droppable] = 0;
      ++report.unrepaired;
      --droppable;
    }
    report.dropped += droppable;
  }
  if (report.dropped == 0) return true;

  // The dropped triangles leave the index buffer; runs keep their order and move down, and
  // indices outside every run are carried over as they were.
  Vector<u32> kept;
  kept.reserve(indices.size() - report.dropped * 3);
  u32 cursor = 0;
  for (ZeroAreaRepairRange& range : ranges) {
    for (; cursor < range.first; ++cursor)
      kept.push_back(indices[cursor]);
    const u32 first_kept = kept.size();
    for (u32 i = range.first; i < range.first + range.count; i += 3) {
      if (drop[i / 3] != 0) continue;
      kept.push_back(indices[i]);
      kept.push_back(indices[i + 1]);
      kept.push_back(indices[i + 2]);
    }
    cursor = range.first + range.count;
    range.first = first_kept;
    range.count = kept.size() - first_kept;
  }
  for (; cursor < indices.size(); ++cursor)
    kept.push_back(indices[cursor]);
  indices = std::move(kept);
  return true;
}

}  // namespace engine::geometry
