// Distance forms (geometry.md, "Distance forms"): what a mesh can draw from a camera at least a
// distance away, as a mesh of its own — its clusters the source's bit for bit, and from that far
// the same selection as the source for every view.

#include <domain/geometry/cluster_lod.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

void append_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  const u32 base = positions.size();
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / static_cast<f32>(n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / static_cast<f32>(n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = base + z * n + x;
      for (const u32 i : {a, a + n, a + 1, a + 1, a + n, a + n + 1})
        indices.push_back(i);
    }
  }
}

// A heightfield with two authored levels above its decimated chain (authored_levels_tests.cpp's),
// and a coverage word per cluster a tenth of each error, so the foliage threshold is exercised.
struct Fixture {
  ClusterLodMesh lod;
  Vector<u32> coverage;
};
Fixture fixture() {
  Fixture f;
  Vector<Vec3> positions;
  Vector<u32> fine;
  append_terrain(65, 10.0f, positions, fine);
  Vector<u32> coarse;
  append_terrain(9, 10.0f, positions, coarse);
  Vector<u32> quad;
  append_terrain(2, 10.0f, positions, quad);
  const ClusterLodLevel levels[] = {{coarse, 0.5f}, {quad, 2.0f}};
  std::string error;
  REQUIRE_MESSAGE(
      build_cluster_lod_levels(positions, fine, levels, ClusterLodOptions{}, f.lod, &error), error);
  for (const ClusterLodDesc& d : f.lod.lod) {
    const f32 parent =
        d.parent_error < k_lod_terminal_error ? d.parent_error * 0.1f : k_lod_terminal_error;
    f.coverage.push_back(pack_lod_coverage(d.own_error * 0.1f, parent));
  }
  return f;
}

LodView view_1080p() {
  LodView view;
  view.znear = 0.1f;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;
  view.threshold_px = 1.0f;
  view.foliage_px = 5.0f;
  return view;
}

LodFormCut cut_at(const LodView& view, f32 distance, f32 fade) {
  LodFormCut cut;
  cut.distance = distance;
  cut.znear = view.znear;
  cut.proj_scale = view.proj_scale;
  cut.threshold_px = view.threshold_px;
  cut.foliage_px = view.foliage_px;
  cut.fade = fade;
  return cut;
}

bool same_bytes(const void* a, const void* b, usize n) { return std::memcmp(a, b, n) == 0; }

// Every kept cluster is the source's: the same descriptor but for where its runs start, the same
// vertices, attributes, grid positions and triangles, the same LOD description but for a frontier
// cluster's own error and level, and the same coverage but for a frontier's own half.
void check_bit_for_bit(const Fixture& f, const ClusterLodMesh& form, const Vector<u32>& coverage,
                       const LodFormMap& map) {
  REQUIRE(form.mesh.clusters.size() == map.clusters.size());
  CHECK(form.mesh.quant_origin.x == f.lod.mesh.quant_origin.x);
  CHECK(form.mesh.quant_scale == f.lod.mesh.quant_scale);
  u32 frontier = 0;
  for (u32 i = 0; i < map.clusters.size(); ++i) {
    const u32 c = map.clusters[i];
    ClusterDesc a = f.lod.mesh.clusters[c];
    ClusterDesc b = form.mesh.clusters[i];
    const u32 va = a.vertex_offset, vb = b.vertex_offset;
    const u32 ta = a.triangle_offset, tb = b.triangle_offset;
    a.vertex_offset = b.vertex_offset = 0;
    a.triangle_offset = b.triangle_offset = 0;
    CHECK(same_bytes(&a, &b, sizeof(a)));
    CHECK(same_bytes(&f.lod.mesh.vertices[va], &form.mesh.vertices[vb], a.vertex_count * 12u));
    CHECK(same_bytes(&f.lod.mesh.attributes[va], &form.mesh.attributes[vb],
                     a.vertex_count * sizeof(VertexAttributes)));
    CHECK(same_bytes(&f.lod.mesh.quantized[3 * va], &form.mesh.quantized[3 * vb],
                     a.vertex_count * 6u));
    CHECK(same_bytes(&f.lod.mesh.triangles[ta], &form.mesh.triangles[tb], a.triangle_count * 4u));
    ClusterLodDesc la = f.lod.lod[c];
    const ClusterLodDesc lb = form.lod[i];
    u32 ca = f.coverage[c];
    if (lb.own_error == 0.0f && la.own_error != 0.0f) {
      ++frontier;
      la.own_error = 0.0f;
      la.level = 0;
      ca &= 0xFFFF0000u;
    } else if (lb.level == 0 && la.level != 0) {
      ++frontier;
      la.level = 0;
      ca &= 0xFFFF0000u;
    } else if (la.level == 0) {
      ++frontier;
      ca &= 0xFFFF0000u;
    }
    CHECK(same_bytes(&la, &lb, sizeof(la)));
    CHECK(ca == coverage[i]);
  }
  CHECK(frontier == map.frontier);
}

// The clusters `view` selects in `mesh`, as source indices through `back` (empty: the identity).
Vector<u32> selected(const ClusterLodMesh& mesh, const Vector<u32>& coverage, const LodView& view,
                     const Vector<u32>& back) {
  Vector<u32> out;
  select_lod(mesh, view, std::span<const u32>(coverage.data(), coverage.size()), out);
  if (!back.empty()) {
    for (u32& c : out)
      c = back[c];
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST_CASE("distance forms: a cut at zero is the mesh") {
  const Fixture f = fixture();
  ClusterLodMesh form;
  Vector<u32> coverage;
  LodFormMap map;
  std::string error;
  REQUIRE_MESSAGE(extract_lod_form(f.lod, f.coverage, cut_at(view_1080p(), 0.0f, 0.1f), form,
                                   coverage, map, &error),
                  error);
  CHECK(form.mesh.clusters.size() == f.lod.mesh.clusters.size());
  CHECK(form.mesh.vertices.size() == f.lod.mesh.vertices.size());
  CHECK(form.mesh.quantized == f.lod.mesh.quantized);
  CHECK(form.mesh.triangles == f.lod.mesh.triangles);
  CHECK(map.frontier == f.lod.level_cluster_counts[0]);
  check_bit_for_bit(f, form, coverage, map);
}

TEST_CASE("distance forms: from the cut's distance the form selects what the mesh does") {
  const Fixture f = fixture();
  const LodView base = view_1080p();
  for (const f32 fade : {0.0f, 0.1f}) {
    for (const f32 distance : {20.0f, 60.0f, 250.0f, 2000.0f}) {
      CAPTURE(fade);
      CAPTURE(distance);
      ClusterLodMesh form;
      Vector<u32> coverage;
      LodFormMap map;
      std::string error;
      REQUIRE_MESSAGE(extract_lod_form(f.lod, f.coverage, cut_at(base, distance, fade), form,
                                       coverage, map, &error),
                      error);
      CHECK(form.mesh.clusters.size() < f.lod.mesh.clusters.size());
      CHECK(map.frontier > 0);
      check_bit_for_bit(f, form, coverage, map);
      // Cameras round the origin at the cut's distance and beyond, high and low, at the cut's
      // projection and a coarser one: the same clusters, and the same fade splits.
      u32 views = 0;
      for (u32 k = 0; k < 24; ++k) {
        const f32 angle = 0.2618f * static_cast<f32>(k);
        for (const f32 scale : {1.0f, 1.3f, 3.0f}) {
          for (const f32 height : {0.0f, 0.4f}) {
            LodView view = base;
            view.fade = fade;
            const f32 d = distance * scale;
            view.camera = normalize(Vec3{std::cos(angle) * (1.0f - height), height,
                                         std::sin(angle) * (1.0f - height)}) *
                          d;
            const Vector<u32> whole = selected(f.lod, f.coverage, view, {});
            const Vector<u32> far = selected(form, coverage, view, map.clusters);
            CHECK(whole == far);
            for (u32 i = 0; i < form.lod.size(); ++i) {
              const u32 c = map.clusters[i];
              const f32 fc = lod_coverage_parent(f.coverage[c]);
              CHECK(lod_fade_split(form.lod[i].parent, form.lod[i].parent_error,
                                   lod_coverage_parent(coverage[i]), view) ==
                    lod_fade_split(f.lod.lod[c].parent, f.lod.lod[c].parent_error, fc, view));
            }
            view.proj_scale *= 0.5f;  // a smaller window: still the same
            CHECK(selected(f.lod, f.coverage, view, {}) ==
                  selected(form, coverage, view, map.clusters));
            ++views;
          }
        }
      }
      CHECK(views == 144);
      // Nearer than the cut the form still draws a cut of its own, never nothing.
      LodView near = base;
      near.camera = Vec3{0.0f, 2.0f, 0.0f};
      Vector<u32> some;
      CHECK(select_lod(form, near, std::span<const u32>(coverage.data(), coverage.size()), some) >
            0);
    }
  }
}

TEST_CASE("distance forms: far enough, only the root level is left") {
  const Fixture f = fixture();
  ClusterLodMesh form;
  Vector<u32> coverage;
  LodFormMap map;
  std::string error;
  REQUIRE_MESSAGE(extract_lod_form(f.lod, f.coverage, cut_at(view_1080p(), 1.0e6f, 0.1f), form,
                                   coverage, map, &error),
                  error);
  for (const ClusterLodDesc& d : form.lod)
    CHECK(d.parent_error == k_lod_terminal_error);
  CHECK(form.mesh.clusters.size() == map.frontier);
  CHECK(form.leaf_triangle_count == 2);  // the authored quad
}

TEST_CASE("distance forms: what has no form is refused with a sentence") {
  Fixture f = fixture();
  ClusterLodMesh form;
  Vector<u32> coverage;
  LodFormMap map;
  std::string error;
  const Vector<u32> short_coverage(3, 0u);
  CHECK_FALSE(extract_lod_form(f.lod, short_coverage, cut_at(view_1080p(), 10.0f, 0.1f), form,
                               coverage, map, &error));
  CHECK(error.find("coverage") != std::string::npos);
  f.lod.mesh.skin.resize(f.lod.mesh.vertices.size());
  CHECK_FALSE(extract_lod_form(f.lod, f.coverage, cut_at(view_1080p(), 10.0f, 0.1f), form, coverage,
                               map, &error));
  CHECK(error.find("skinned") != std::string::npos);
}
