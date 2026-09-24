// The atlas step (repack.h, docs/subsystems/atlas.md): which materials, xatlas per material, the
// rebake per image, and the surgery that puts the result back into the mesh. The rebake itself is
// in rebake.cpp.

#include "proxy_charts.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/time/time.h>
#include <domain/atlas/repack.h>
#include <domain/geometry/cluster.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <xatlas.h>

namespace engine::atlas {

namespace {

// Bumped whenever this module would repack the same mesh with the same options into different
// bytes — a change to the charting or packing options it gives xatlas, to the rebake's sampling,
// or to the xatlas pin — so the content build's cache key moves with it (repack_options_key).
// 1: the first version (2026-09-23).
// 2: the LSCM solve sums a matrix entry's duplicates in insertion order (a stable sort), where it
//    summed them in whatever order std::sort left equal keys — MSVC's library's on Windows,
//    libstdc++'s on Linux — and the tree stopped contracting floating-point arithmetic
//    (ADR-0035). The same mesh can chart to different UVs than at 1.
constexpr u64 k_repack_version = 2;

// xatlas cuts charts with absolute epsilons (a face under FLT_EPSILON of area is refused, two
// positions within 1e-4 are one in places), which makes its behaviour a function of the units a
// mesh was authored in: a 5 cm prop's triangles are thousands of times smaller than a 5 m one's.
// Every material's positions are therefore moved into a box whose largest side is this many
// units before xatlas sees them, which makes the charts scale-free — the same reasoning as the
// LOD builder's normalized attribute weights. The UVs xatlas returns are unaffected: the packer
// scales charts by texels per unit of *this* frame, and the atlas is normalized to 0..1 anyway.
constexpr f32 k_chart_frame = 256.0f;

// The packer is asked for one square atlas: its own estimate of texels per unit first, scaled to
// fill the atlas, then this much smaller per attempt until every chart fits one atlas.
constexpr u32 k_max_pack_attempts = 24;
constexpr f32 k_pack_shrink = 0.96f;

// The share of a material's triangles that may come out folded against their chart, or with no
// UV area, before the material is left with its own atlas. Measured on E10's sixty props: the 49
// it repacks leave at most 2.7% so; the eleven it declines 5.4% (a thin-walled pot) to 50% (a
// rope's strands), and their rebaked textures would be wrong wherever they fold (atlas.md, "The
// gate").
constexpr f64 k_max_fold_share = 0.05;

f64 ms_since(i64 start_ns) { return static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6; }

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// The image slots of a material, with the space each is sampled in.
struct SlotRef {
  i32* image = nullptr;
  TextureSpace space = TextureSpace::srgb;
  f32 normal_scale = 1.0f;
};

u32 material_slots(assets::Material& m, SlotRef out[5]) {
  out[0] = SlotRef{&m.base_color_image, TextureSpace::srgb, 1.0f};
  out[1] = SlotRef{&m.metallic_roughness_image, TextureSpace::linear, 1.0f};
  out[2] = SlotRef{&m.normal_image, TextureSpace::tangent_normal, m.normal_scale};
  out[3] = SlotRef{&m.occlusion_image, TextureSpace::linear, 1.0f};
  out[4] = SlotRef{&m.emissive_image, TextureSpace::srgb, 1.0f};
  return 5;
}

const char* space_name(TextureSpace space) {
  switch (space) {
    case TextureSpace::srgb: return "srgb";
    case TextureSpace::linear: return "linear";
    case TextureSpace::tangent_normal: return "normal";
  }
  return "?";
}

// One source image, decoded once for every material that samples it.
struct SourceImage {
  const assets::ImageRef* ref = nullptr;
  std::string path;  // the file, for an image the source named rather than embedded
  bool needed = false;
  bool ok = false;
  image::Image pixels;
  std::string error;
};

void decode_source(void* data) {
  SourceImage& s = *static_cast<SourceImage*>(data);
  if (!s.ref->bytes.empty()) {
    s.ok = image::decode_image(std::span<const u8>(s.ref->bytes.data(), s.ref->bytes.size()),
                               s.pixels, 4, &s.error);
  } else if (!s.path.empty()) {
    s.ok = image::read_image(s.path, s.pixels, 4, &s.error) == io::Status::Ok;
    if (!s.ok && s.error.empty()) s.error = "cannot read '" + s.path + "'";
  } else {
    s.error = "the image has neither bytes nor a path";
  }
}

// One distinct (source image, space) a material samples, and what becomes of it.
struct Bake {
  u32 source = 0;
  TextureSpace space = TextureSpace::srgb;
  f32 normal_scale = 1.0f;
  u32 size = 0;
  image::Image out;
  Vector<u8> png;
  RebakeError error;
  u32 final_index = 0;
};

// Everything one material's repack produces. A job writes only its own group.
struct Group {
  i32 material = -1;
  Vector<u32> first_index;  // the index offset of each of its triangles, in primitive order
  u32 resolution = 0;
  const RepackOptions* options = nullptr;
  const assets::MeshData* mesh = nullptr;
  // xatlas's answer
  Vector<u32> vertex_source;  // per new vertex: the mesh vertex it copies
  Vector<Vec2> uv;            // per new vertex: its UV in 0..1, rounded to half floats
  Vector<u32> corners;        // per triangle corner: the new vertex, local to this group
  Vector<u8> unatlased;       // per triangle: xatlas refused the face
  RepackedMaterial report;
  f64 chart_ms = 0.0;
  bool ok = false;
  std::string error;
  Vector<Bake> bakes;
};

Vec2 half_rounded(f32 u, f32 v) noexcept {
  return Vec2{geometry::f16_to_f32(geometry::f32_to_f16(u)),
              geometry::f16_to_f32(geometry::f32_to_f16(v))};
}

// The packer, given an atlas whose charts are computed: one square atlas of `resolution`, found
// by scaling xatlas's own estimate of texels per unit to fill the square and shrinking it until
// every chart fits (k_max_pack_attempts, k_pack_shrink). Returns the side the UVs are to be
// divided by — the resolution, or when nothing fitted the grown atlas's larger side.
f32 pack_square(xatlas::Atlas* atlas, u32 resolution, u32 padding, RepackedMaterial& report) {
  xatlas::PackOptions pack;
  pack.padding = padding;
  pack.bilinear = true;
  pack.blockAlign = false;
  pack.bruteForce = false;
  pack.rotateCharts = true;
  pack.rotateChartsToAxis = true;
  pack.resolution = resolution;
  pack.texelsPerUnit = 0.0f;  // estimated: one atlas that grows to fit
  xatlas::PackCharts(atlas, pack);
  u32 packings = 1;
  const u32 grown = std::max(atlas->width, atlas->height);
  const f32 estimated = atlas->texelsPerUnit;
  bool fixed = false;
  if (grown > 0 && estimated > 0.0f) {
    f32 tpu = estimated * static_cast<f32>(resolution) / static_cast<f32>(grown);
    for (u32 attempt = 0; attempt < k_max_pack_attempts; ++attempt) {
      pack.texelsPerUnit = tpu;
      xatlas::PackCharts(atlas, pack);
      ++packings;
      if (atlas->atlasCount <= 1) {
        fixed = true;
        break;
      }
      tpu *= k_pack_shrink;
    }
  }
  f32 normalize = static_cast<f32>(resolution);
  if (!fixed) {
    // It never fitted: back to the grown atlas, squeezed into the square. Charts keep their
    // shapes and their gutters shrink by the same factor; the report says how many packings.
    pack.texelsPerUnit = 0.0f;
    xatlas::PackCharts(atlas, pack);
    ++packings;
    normalize = static_cast<f32>(std::max({atlas->width, atlas->height, resolution}));
  }
  report.pack_attempts = packings;
  report.charts = atlas->chartCount;
  report.utilization = atlas->utilization != nullptr ? atlas->utilization[0] : 0.0f;
  report.texels_per_unit = atlas->texelsPerUnit;
  report.resolution = resolution;
  return normalize;
}

void chart_group(void* data) {
  Group& g = *static_cast<Group*>(data);
  const i64 start = time::monotonic_ns();
  const assets::MeshData& mesh = *g.mesh;
  const u32 triangle_count = g.first_index.size();

  // Every corner's own vertex, in primitive order.
  const u32 corner_count = triangle_count * 3u;
  Vector<u32> corner_vertex;
  corner_vertex.reserve(corner_count);
  for (const u32 first : g.first_index) {
    for (u32 k = 0; k < 3; ++k)
      corner_vertex.push_back(mesh.indices[first + k]);
  }

  // What the charts are cut on: **one vertex per position**, not per source vertex. A generator
  // splits its vertices along its own atlas's borders, and xatlas reads an edge whose two sides
  // are different vertices at one position as a seam: its normal-seam term charges a chart for
  // growing across one (and without normals every such edge counts, since two faces are never
  // exactly coplanar), so given the split vertices it redraws the old atlas. Welded by position,
  // the old borders are gone and the charts are cut by the shape alone. The corners are mapped
  // back to their own vertices afterwards, so the normals, skin and morph deltas a source vertex
  // carries are untouched. Positions are equal when their bits are, which is the weld's rule.
  struct PositionKey {
    u32 bits[3] = {};
    u32 vertex = 0;
  };
  Vector<PositionKey> keys;
  {
    Vector<u8> seen(mesh.positions.size(), u8{0});
    for (const u32 v : corner_vertex) {
      if (seen[v] != 0) continue;
      seen[v] = 1u;
      PositionKey key;
      std::memcpy(key.bits, &mesh.positions[v], sizeof(key.bits));
      key.vertex = v;
      keys.push_back(key);
    }
  }
  g.report.source_vertices = keys.size();
  std::sort(keys.begin(), keys.end(), [](const PositionKey& a, const PositionKey& b) {
    for (u32 i = 0; i < 3; ++i) {
      if (a.bits[i] != b.bits[i]) return a.bits[i] < b.bits[i];
    }
    return a.vertex < b.vertex;
  });
  // A run of equal positions is one point; points are numbered by first use along the corners,
  // so the numbering does not depend on the sort's order of positions.
  Vector<u32> run_of(mesh.positions.size(), ~0u);
  u32 runs = 0;
  for (u32 i = 0; i < keys.size(); ++i) {
    if (i != 0 && std::memcmp(keys[i].bits, keys[i - 1].bits, sizeof(keys[i].bits)) != 0) ++runs;
    run_of[keys[i].vertex] = runs;
  }
  ++runs;
  Vector<u32> point_of_run(runs, ~0u);
  Vector<u32> point_vertex;  // per point: a source vertex at it
  PointMesh points;
  points.indices.reserve(corner_count);
  for (const u32 v : corner_vertex) {
    u32& point = point_of_run[run_of[v]];
    if (point == ~0u) {
      point = point_vertex.size();
      point_vertex.push_back(v);
    }
    points.indices.push_back(point);
  }
  Vec3 lo = mesh.positions[point_vertex[0]];
  Vec3 hi = lo;
  for (const u32 v : point_vertex) {
    const Vec3 p = mesh.positions[v];
    lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const f32 extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
  const f32 scale = extent > 0.0f ? k_chart_frame / extent : 1.0f;
  points.positions.reserve(point_vertex.size() * 3);
  for (const u32 v : point_vertex) {
    const Vec3 p = mesh.positions[v];
    points.positions.push_back((p.x - lo.x) * scale);
    points.positions.push_back((p.y - lo.y) * scale);
    points.positions.push_back((p.z - lo.z) * scale);
  }

  // The charts: on a proxy when the mesh is large enough for one to differ from it, directly
  // otherwise. Either way xatlas ends up holding one mesh whose charts are computed, and an output
  // vertex names a point through `point_of_xref`.
  const i64 chart_start = time::monotonic_ns();
  xatlas::Atlas* atlas = xatlas::Create();
  ProxyUvMesh proxy;
  const bool use_proxy =
      g.options->proxy_triangles != 0 && triangle_count > 2u * g.options->proxy_triangles;
  if (use_proxy) {
    std::string message;
    const f32 chart_cost = static_cast<f32>(g.options->max_chart_cost_milli) / 1000.0f;
    if (!proxy_uv_mesh(points, g.options->proxy_triangles, chart_cost, proxy, message)) {
      g.error = "material " + std::to_string(g.material) + ": " + message;
      xatlas::Destroy(atlas);
      return;
    }
    xatlas::UvMeshDecl decl;
    decl.vertexUvData = proxy.uv.data();
    decl.vertexStride = sizeof(Vec2);
    decl.vertexCount = proxy.uv.size();
    decl.indexData = proxy.corners.data();
    decl.indexCount = proxy.corners.size();
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    // A chart per material, so two charts whose parameter spaces overlap are never merged.
    decl.faceMaterialData = proxy.face_chart.data();
    if (xatlas::AddUvMesh(atlas, decl) != xatlas::AddMeshError::Success) {
      g.error = "xatlas refused the charts of material " + std::to_string(g.material);
      xatlas::Destroy(atlas);
      return;
    }
    xatlas::ComputeCharts(atlas, xatlas::ChartOptions{});
    g.report.proxy_triangles = proxy.proxy_triangles;
    g.report.proxy_charts = proxy.target_charts;
    g.report.folded_triangles = proxy.folded;
  } else {
    xatlas::MeshDecl decl;
    decl.vertexPositionData = points.positions.data();
    decl.vertexPositionStride = sizeof(f32) * 3;
    decl.vertexCount = points.point_count();
    decl.indexData = points.indices.data();
    decl.indexCount = points.indices.size();
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    // Deliberately neither the normals nor the old UVs: a generator splits its normals along its
    // own chart borders (E10's second pass), and xatlas weighs normal and texture seams as places
    // to cut — handing it either would redraw the fragmented atlas this step exists to replace.
    const xatlas::AddMeshError added = xatlas::AddMesh(atlas, decl, 1);
    if (added != xatlas::AddMeshError::Success) {
      g.error = std::string("xatlas refused material ") + std::to_string(g.material) + ": " +
                xatlas::StringForEnum(added);
      xatlas::Destroy(atlas);
      return;
    }
    xatlas::ChartOptions chart_options;
    chart_options.maxCost = static_cast<f32>(g.options->max_chart_cost_milli) / 1000.0f;
    xatlas::ComputeCharts(atlas, chart_options);
  }
  g.report.chart_ms = ms_since(chart_start);
  const i64 pack_start = time::monotonic_ns();
  const f32 normalize = pack_square(atlas, g.resolution, g.options->padding, g.report);
  g.report.pack_ms = ms_since(pack_start);

  if (atlas->meshCount != 1 || atlas->meshes[0].indexCount != corner_count) {
    g.error = "xatlas returned a mesh of another shape for material " + std::to_string(g.material);
    xatlas::Destroy(atlas);
    return;
  }
  const xatlas::Mesh& out = atlas->meshes[0];
  auto point_of_xref = [&](u32 xref) { return use_proxy ? proxy.uv_point[xref] : xref; };
  // A face is **refused** when xatlas left a corner of it out of every chart (a zero-length edge,
  // no area on the surface or in UV) or when its corners landed in two charts. The second happens
  // on a proxy's charts: xatlas builds a UV mesh's charts by flood fill over equal UV values, a
  // sliver of no UV area is left out of the fill, and where a sliver is what joins two parts of a
  // piece, its corners are packed wherever those parts went. A refused face is not baked, and all
  // its corners take UVs in **one** chart — its first charted corner's: a copy of the same point in
  // that chart when there is one, that corner's own UV otherwise — so it is at most a sliver inside
  // one chart. Left alone, such faces were stretched across the atlas between two charts and drew
  // a smear of whatever lay between: on one E10 prop, 31 faces covering 3.5 times the atlas's area.
  //
  // A **crumb** is a piece of a chart — faces joined through the UV vertices they share — whose
  // triangles cover less than one texel of a 4,096 atlas as stored: a triangle or two the transfer
  // left on their own across a non-manifold edge, a sliver. Pieces are sized by surface area, so
  // nothing stops one coming out that small, and an island under a texel is what the source's UV
  // repair exists to fold away (geometry.md, "UV-degenerate triangles: the repair"; the E10
  // harness's island check). A crumb's faces are refused too, and anchored in a piece that is not
  // one; a crumb with nothing else beside it (a floating speck) keeps its own UVs.
  const f32 inv = 1.0f / normalize;
  auto packed_uv = [&](const xatlas::Vertex& v) { return Vec2{v.uv[0] * inv, v.uv[1] * inv}; };
  Vector<u8> refused(triangle_count, u8{0});
  Vector<u32> piece(out.vertexCount);
  for (u32 k = 0; k < piece.size(); ++k)
    piece[k] = k;
  auto piece_of = [&](u32 k) {
    while (piece[k] != k) {
      piece[k] = piece[piece[k]];
      k = piece[k];
    }
    return k;
  };
  for (u32 t = 0; t < triangle_count; ++t) {
    const xatlas::Vertex& a = out.vertexArray[out.indexArray[t * 3]];
    const xatlas::Vertex& b = out.vertexArray[out.indexArray[t * 3 + 1]];
    const xatlas::Vertex& c = out.vertexArray[out.indexArray[t * 3 + 2]];
    const bool charted = a.atlasIndex >= 0 && b.atlasIndex >= 0 && c.atlasIndex >= 0;
    if (!charted || a.chartIndex != b.chartIndex || a.chartIndex != c.chartIndex) {
      refused[t] = 1u;
      continue;
    }
    const u32 r = piece_of(out.indexArray[t * 3]);
    for (u32 j = 1; j < 3; ++j) {
      const u32 s = piece_of(out.indexArray[t * 3 + j]);
      if (r != s) piece[std::max(r, s)] = std::min(r, s);
    }
  }
  Vector<f64> piece_texels(out.vertexCount, 0.0);
  for (u32 t = 0; t < triangle_count; ++t) {
    if (refused[t] != 0) continue;
    piece_texels[piece_of(out.indexArray[t * 3])] +=
        geometry::stored_uv_area_texels(packed_uv(out.vertexArray[out.indexArray[t * 3]]),
                                        packed_uv(out.vertexArray[out.indexArray[t * 3 + 1]]),
                                        packed_uv(out.vertexArray[out.indexArray[t * 3 + 2]]));
  }
  // A vertex no unrefused face uses has a piece of no area, so it is never an anchor either.
  auto in_crumb = [&](u32 k) {
    return piece_texels[piece_of(k)] < geometry::k_uv_degenerate_texels;
  };
  Vector<u8> crumb_face(triangle_count, u8{0});
  for (u32 t = 0; t < triangle_count; ++t) {
    if (refused[t] != 0 || !in_crumb(out.indexArray[t * 3])) continue;
    refused[t] = 1u;
    crumb_face[t] = 1u;
  }
  // The charted xatlas vertices by (point, chart), crumbs left out, so a refused corner can find a
  // copy of its point in the chart its face goes to. (Two pieces of one chart never share a point:
  // a UV vertex is one point in one chart, so they would be one piece.)
  struct Charted {
    u32 point = 0;
    i32 chart = 0;
    u32 vertex = 0;
  };
  Vector<Charted> charted;
  for (u32 k = 0; k < out.vertexCount; ++k) {
    const xatlas::Vertex& v = out.vertexArray[k];
    if (v.atlasIndex >= 0 && !in_crumb(k))
      charted.push_back(Charted{point_of_xref(v.xref), v.chartIndex, k});
  }
  auto before = [](const Charted& x, const Charted& y) {
    if (x.point != y.point) return x.point < y.point;
    if (x.chart != y.chart) return x.chart < y.chart;
    return x.vertex < y.vertex;
  };
  std::sort(charted.begin(), charted.end(), before);
  // The first charted copy of `point` in `chart`, or with `chart` < 0 in any chart; ~0u if none.
  auto copy_of = [&](u32 point, i32 chart) -> u32 {
    const Charted key{point, chart < 0 ? std::numeric_limits<i32>::min() : chart, 0u};
    const auto it = std::lower_bound(charted.begin(), charted.end(), key, before);
    if (it == charted.end() || it->point != point || (chart >= 0 && it->chart != chart)) return ~0u;
    return it->vertex;
  };
  // Where a refused corner's UV comes from. Each point of a refused face gets a **source**: a
  // charted copy of itself when a chart holds it, and otherwise the source of the nearest such
  // point through the refused faces, breadth first in face order. A refused face goes to the chart
  // of its first corner's source; each corner takes the copy of its own point in that chart where
  // there is one, else its point's source if that is in the same chart, else the first corner's.
  // So a refused face is at most a sliver inside one chart, beside the charted points it is joined
  // to, and a run of slivers stays joined to the chart through the points they share instead of
  // becoming an island of no area. Refused faces that touch no chart at all have no source and
  // keep what xatlas gave them.
  auto point_of_corner = [&](u32 c) {
    return point_of_xref(out.vertexArray[out.indexArray[c]].xref);
  };
  const u32 point_count = points.point_count();
  Vector<u32> first(point_count + 1u, 0u);  // point -> the refused faces at it
  for (u32 t = 0; t < triangle_count; ++t) {
    if (refused[t] == 0) continue;
    for (u32 j = 0; j < 3; ++j)
      ++first[point_of_corner(t * 3 + j) + 1u];
  }
  for (u32 p = 0; p < point_count; ++p)
    first[p + 1u] += first[p];
  Vector<u32> faces_at(first[point_count]);
  {
    Vector<u32> cursor(point_count, 0u);
    for (u32 p = 0; p < point_count; ++p)
      cursor[p] = first[p];
    for (u32 t = 0; t < triangle_count; ++t) {
      if (refused[t] == 0) continue;
      for (u32 j = 0; j < 3; ++j)
        faces_at[cursor[point_of_corner(t * 3 + j)]++] = t;
    }
  }
  Vector<u32> point_source(point_count, ~0u);
  Vector<u32> queue;
  for (u32 t = 0; t < triangle_count; ++t) {
    if (refused[t] == 0) continue;
    for (u32 j = 0; j < 3; ++j) {
      const u32 p = point_of_corner(t * 3 + j);
      if (point_source[p] != ~0u) continue;
      const u32 copy = copy_of(p, -1);
      if (copy == ~0u) continue;
      point_source[p] = copy;
      queue.push_back(p);
    }
  }
  for (u32 q = 0; q < queue.size(); ++q) {
    const u32 p = queue[q];
    for (u32 i = first[p]; i < first[p + 1u]; ++i) {
      const u32 t = faces_at[i];
      for (u32 j = 0; j < 3; ++j) {
        const u32 other = point_of_corner(t * 3 + j);
        if (point_source[other] != ~0u) continue;
        point_source[other] = point_source[p];
        queue.push_back(other);
      }
    }
  }
  // The xatlas vertex each corner takes its UV from: its own, or for a refused face the above.
  Vector<u32> uv_vertex(corner_count);
  for (u32 t = 0; t < triangle_count; ++t) {
    if (refused[t] == 0) {
      for (u32 j = 0; j < 3; ++j)
        uv_vertex[t * 3 + j] = out.indexArray[t * 3 + j];
      continue;
    }
    u32 anchor = ~0u;
    for (u32 j = 0; j < 3 && anchor == ~0u; ++j)
      anchor = point_source[point_of_corner(t * 3 + j)];
    for (u32 j = 0; j < 3; ++j) {
      const u32 k = out.indexArray[t * 3 + j];
      if (anchor == ~0u) {
        uv_vertex[t * 3 + j] = k;  // no chart to go to: as xatlas left it
        continue;
      }
      const i32 chart = out.vertexArray[anchor].chartIndex;
      const xatlas::Vertex& v = out.vertexArray[k];
      if (v.atlasIndex >= 0 && v.chartIndex == chart && !in_crumb(k)) {
        uv_vertex[t * 3 + j] = k;
        continue;
      }
      const u32 p = point_of_corner(t * 3 + j);
      const u32 copy = copy_of(p, chart);
      if (copy != ~0u) {
        uv_vertex[t * 3 + j] = copy;
      } else if (point_source[p] != ~0u && out.vertexArray[point_source[p]].chartIndex == chart) {
        uv_vertex[t * 3 + j] = point_source[p];
      } else {
        uv_vertex[t * 3 + j] = anchor;
      }
    }
  }
  // One new vertex per (source vertex, xatlas vertex its UV comes from), numbered by first use:
  // the UV is the chart's, everything else the source vertex's. Two source vertices at one point
  // in one chart get the same UV, which is what lets the weld merge them when nothing else differs.
  Vector<u32> head(out.vertexCount, ~0u);
  Vector<u32> next;
  g.corners.resize(corner_count);
  for (u32 c = 0; c < corner_count; ++c) {
    const u32 k = uv_vertex[c];
    const u32 source = corner_vertex[c];
    u32 n = head[k];
    while (n != ~0u && g.vertex_source[n] != source)
      n = next[n];
    if (n == ~0u) {
      n = g.vertex_source.size();
      const xatlas::Vertex& v = out.vertexArray[k];
      Vec2 uv{0.0f, 0.0f};
      if (v.atlasIndex >= 0) uv = half_rounded(v.uv[0] * inv, v.uv[1] * inv);
      g.vertex_source.push_back(source);
      g.uv.push_back(uv);
      next.push_back(head[k]);
      head[k] = n;
    }
    g.corners[c] = n;
  }
  g.unatlased = std::move(refused);
  for (u32 t = 0; t < triangle_count; ++t) {
    if (crumb_face[t] != 0)
      ++g.report.crumb_triangles;
    else if (g.unatlased[t] != 0)
      ++g.report.unatlased_triangles;
  }
  {
    Vector<u8> counted(out.vertexCount, u8{0});
    for (u32 t = 0; t < triangle_count; ++t) {
      if (crumb_face[t] == 0) continue;
      const u32 r = piece_of(out.indexArray[t * 3]);
      if (counted[r] == 0) ++g.report.crumb_charts;
      counted[r] = 1u;
    }
  }
  g.report.vertices = g.vertex_source.size();
  g.report.triangles = triangle_count;
  xatlas::Destroy(atlas);
  g.chart_ms = ms_since(start);
  g.ok = true;
}

struct EncodeJob {
  Bake* bake = nullptr;
  bool ok = false;
};

void encode_bake(void* data) {
  EncodeJob& job = *static_cast<EncodeJob*>(data);
  const image::Image& im = job.bake->out;
  job.ok =
      image::encode_png(im.width, im.height, 4,
                        std::span<const u8>(im.pixels.data(), im.pixels.size()), job.bake->png);
  job.bake->out = image::Image{};  // the PNG is what is kept; the pixels are 4 bytes a texel
}

template <class T>
void run_jobs(jobs::JobSystem* pool, Vector<T>& items, void (*fn)(void*)) {
  if (pool != nullptr && items.size() > 1) {
    Vector<jobs::Job> list;
    list.reserve(items.size());
    jobs::Counter counter;
    for (T& item : items)
      list.push_back(jobs::Job{fn, &item, &counter});
    pool->schedule(jobs::Pool::Performance, std::span<const jobs::Job>(list.data(), list.size()),
                   counter);
    pool->wait(counter);
  } else {
    for (T& item : items)
      fn(&item);
  }
}

// Structural checks on the mesh the importer handed over, so nothing below indexes out of range.
bool check_mesh(const assets::MeshData& mesh, std::string* error) {
  const usize v = mesh.positions.size();
  if (!mesh.normals.empty() && mesh.normals.size() != v)
    return fail(error, "the mesh's normals are not one per position");
  if (!mesh.uvs.empty() && mesh.uvs.size() != v)
    return fail(error, "the mesh's UVs are not one per position");
  if (!mesh.skin_bindings.empty() && mesh.skin_bindings.size() != v)
    return fail(error, "the mesh's skin bindings are not one per position");
  if (!mesh.vertex_ids.empty() && mesh.vertex_ids.size() != v)
    return fail(error, "the mesh's canonical vertex ids are not one per position");
  for (const u32 i : mesh.indices) {
    if (i >= v) return fail(error, "an index is outside the mesh's vertices");
  }
  for (const assets::Primitive& p : mesh.primitives) {
    if (p.index_count % 3 != 0 ||
        static_cast<u64>(p.first_index) + p.index_count > mesh.indices.size())
      return fail(error, "a primitive's index range is not whole triangles inside the indices");
  }
  for (const geometry::MorphChannelSource& c : mesh.morph) {
    if (c.position_deltas.size() != c.vertices.size() ||
        (!c.normal_deltas.empty() && c.normal_deltas.size() != c.vertices.size()))
      return fail(error, "a morph channel's deltas are not one per vertex it names");
    for (const u32 i : c.vertices) {
      if (i >= v) return fail(error, "a morph channel names a vertex outside the mesh");
    }
  }
  return true;
}

}  // namespace

u64 repack_options_key(const RepackOptions& options) noexcept {
  const u64 words[10] = {k_repack_version,
                         options.padding,
                         options.min_resolution,
                         options.max_resolution,
                         options.supersample,
                         options.proxy_triangles,
                         options.max_chart_cost_milli,
                         static_cast<u64>(options.normal_maps),
                         options.dilation,
                         0x7861746c61737631ull};  // "xatlasv1": the pin, cmake/EngineAtlas.cmake
  return hash_bytes(words, sizeof(words));
}

bool repack_atlas(assets::MeshData& mesh, std::string_view source_dir, const RepackOptions& options,
                  jobs::JobSystem* pool, RepackReport& report, std::string* error) {
  const i64 start = time::monotonic_ns();
  report = RepackReport{};
  report.vertices_before = mesh.positions.size();
  report.vertices_after = mesh.positions.size();
  report.images_before = mesh.images.size();
  report.images_after = mesh.images.size();
  if (options.min_resolution == 0 || options.min_resolution > options.max_resolution ||
      options.max_resolution > 16384) {
    return fail(error, "the atlas resolution range is not within 1..16384");
  }
  if (options.supersample != 1 && options.supersample != 2 && options.supersample != 4)
    return fail(error, "the atlas supersampling is 1, 2 or 4");
  if (!check_mesh(mesh, error)) return false;

  // ---- which materials ------------------------------------------------------------------------
  Vector<u8> used(mesh.materials.size(), u8{0});
  for (const assets::Primitive& p : mesh.primitives) {
    if (p.material >= 0 && static_cast<u32>(p.material) < mesh.materials.size() &&
        p.index_count != 0)
      used[static_cast<u32>(p.material)] = 1u;
  }
  Vector<SourceImage> sources(mesh.images.size());
  for (u32 i = 0; i < mesh.images.size(); ++i) {
    sources[i].ref = &mesh.images[i];
    if (mesh.images[i].bytes.empty() && !mesh.images[i].uri.empty())
      sources[i].path = io::join_path(source_dir, mesh.images[i].uri);
  }
  Vector<u8> textured(mesh.materials.size(), u8{0});
  for (u32 m = 0; m < mesh.materials.size(); ++m) {
    if (used[m] == 0) continue;
    SlotRef slots[5];
    const u32 n = material_slots(mesh.materials[m], slots);
    for (u32 s = 0; s < n; ++s) {
      const i32 image = *slots[s].image;
      if (image < 0) continue;
      textured[m] = 1u;
      if (static_cast<u32>(image) < sources.size()) sources[static_cast<u32>(image)].needed = true;
    }
    if (textured[m] == 0) ++report.materials_untextured;
  }

  // ---- decode what is needed --------------------------------------------------------------------
  {
    const i64 t = time::monotonic_ns();
    Vector<SourceImage*> needed;
    for (SourceImage& s : sources) {
      if (s.needed) needed.push_back(&s);
    }
    if (pool != nullptr && needed.size() > 1) {
      Vector<jobs::Job> list;
      jobs::Counter counter;
      for (SourceImage* s : needed)
        list.push_back(jobs::Job{decode_source, s, &counter});
      pool->schedule(jobs::Pool::Performance, std::span<const jobs::Job>(list.data(), list.size()),
                     counter);
      pool->wait(counter);
    } else {
      for (SourceImage* s : needed)
        decode_source(s);
    }
    report.decode_ms = ms_since(t);
  }

  // ---- one group per repackable material, in material order ------------------------------------
  Vector<Group> groups;
  for (u32 m = 0; m < mesh.materials.size(); ++m) {
    if (used[m] == 0 || textured[m] == 0) continue;
    SlotRef slots[5];
    const u32 n = material_slots(mesh.materials[m], slots);
    std::string unreadable;
    u32 resolution = 0;
    Vector<Bake> bakes;
    for (u32 s = 0; s < n; ++s) {
      const i32 image = *slots[s].image;
      if (image < 0) continue;
      if (static_cast<u32>(image) >= sources.size()) {
        unreadable = "names image " + std::to_string(image) + ", which the file does not have";
        break;
      }
      const SourceImage& src = sources[static_cast<u32>(image)];
      if (!src.ok) {
        unreadable = "image " + std::to_string(image) + " could not be read: " + src.error;
        break;
      }
      const u32 side = std::max(src.pixels.width, src.pixels.height);
      resolution = std::max(resolution, side);
      bool seen = false;
      for (const Bake& b : bakes)
        seen = seen || (b.source == static_cast<u32>(image) && b.space == slots[s].space);
      if (!seen) {
        Bake b;
        b.source = static_cast<u32>(image);
        b.space = slots[s].space;
        b.normal_scale = slots[s].normal_scale;
        b.size =
            std::clamp(side * options.supersample, options.min_resolution, options.max_resolution);
        bakes.push_back(std::move(b));
      }
    }
    if (!unreadable.empty()) {
      ++report.materials_unreadable;
      report.notes.push_back("material " + std::to_string(m) + " '" + mesh.materials[m].name +
                             "' " + unreadable + "; it keeps its own UVs and images");
      continue;
    }
    Group g;
    g.material = static_cast<i32>(m);
    g.resolution = std::clamp(resolution * options.supersample, options.min_resolution,
                              options.max_resolution);
    g.options = &options;
    g.mesh = &mesh;
    g.report.material = static_cast<i32>(m);
    g.bakes = std::move(bakes);
    for (const assets::Primitive& p : mesh.primitives) {
      if (p.material != static_cast<i32>(m)) continue;
      for (u32 i = 0; i < p.index_count; i += 3)
        g.first_index.push_back(p.first_index + i);
    }
    if (g.first_index.empty()) continue;
    groups.push_back(std::move(g));
  }
  if (groups.empty()) {
    report.total_ms = ms_since(start);
    return true;  // nothing textured: the mesh is exactly as it was
  }

  // ---- chart and pack, one job per material
  // ------------------------------------------------------- xatlas prints warnings to stdout by
  // default, which is where engine-content's JSON goes.
  static const bool k_quiet = [] {
    xatlas::SetPrint(nullptr, false);
    return true;
  }();
  (void)k_quiet;
  run_jobs(pool, groups, chart_group);
  for (const Group& g : groups) {
    if (!g.ok) return fail(error, g.error);
    report.chart_ms += g.chart_ms;
  }
  // The gate: a material whose new charts fold too much of it — the proxy could not follow the
  // surface there (spines, teeth, a thin part wound round itself) — keeps its own atlas, because
  // a folded triangle shares its texels with another and the rebake cannot give both their own.
  for (u32 i = 0; i < groups.size();) {
    const Group& g = groups[i];
    const u32 bad = g.report.folded_triangles + g.report.unatlased_triangles;
    if (static_cast<f64>(bad) <= k_max_fold_share * static_cast<f64>(g.report.triangles)) {
      ++i;
      continue;
    }
    ++report.materials_declined;
    report.notes.push_back("material " + std::to_string(g.material) + " '" +
                           mesh.materials[static_cast<u32>(g.material)].name +
                           "': its new charts would fold " +
                           std::to_string(g.report.folded_triangles) + " and flatten " +
                           std::to_string(g.report.unatlased_triangles) + " of its " +
                           std::to_string(g.report.triangles) + " triangles, over the " +
                           std::to_string(static_cast<u32>(k_max_fold_share * 100.0)) +
                           "% the step accepts; it keeps its own UVs and images");
    groups.erase(groups.begin() + i);
  }
  if (groups.empty()) {
    report.total_ms = ms_since(start);
    return true;
  }

  // ---- rebake, image by image, rows in parallel
  // --------------------------------------------------
  {
    const i64 t = time::monotonic_ns();
    const bool has_uvs = !mesh.uvs.empty();
    for (Group& g : groups) {
      const u32 corners = g.first_index.size() * 3u;
      Vector<Vec2> old_uv(corners);
      Vector<Vec2> new_uv(corners);
      Vector<Vec3> positions(corners);
      Vector<Vec3> normals(mesh.normals.empty() ? 0 : corners);
      for (u32 t_index = 0; t_index < g.first_index.size(); ++t_index) {
        for (u32 k = 0; k < 3; ++k) {
          const u32 c = t_index * 3 + k;
          const u32 v = mesh.indices[g.first_index[t_index] + k];
          // Both UV sets as the container stores them, so the bake is for the UVs the renderer
          // interpolates on either side.
          old_uv[c] = has_uvs ? half_rounded(mesh.uvs[v].x, mesh.uvs[v].y) : Vec2{0.0f, 0.0f};
          // A refused face is not baked: it has no area in UV (or none on the surface) to show a
          // texture on, and its corners' UVs are borrowed (chart_group), from another chart at
          // worst.
          new_uv[c] = g.unatlased[t_index] != 0 ? Vec2{0.0f, 0.0f} : g.uv[g.corners[c]];
          positions[c] = mesh.positions[v];
          if (!normals.empty()) normals[c] = mesh.normals[v];
        }
      }
      RebakeTriangles tris;
      tris.old_uv = std::span<const Vec2>(old_uv.data(), old_uv.size());
      tris.new_uv = std::span<const Vec2>(new_uv.data(), new_uv.size());
      tris.positions = std::span<const Vec3>(positions.data(), positions.size());
      tris.normals = std::span<const Vec3>(normals.data(), normals.size());
      TexelCoverage coverage;
      for (Bake& b : g.bakes) {
        if (coverage.size != b.size) {
          std::string message;
          if (!build_coverage(tris.new_uv, b.size, coverage, &message)) return fail(error, message);
        }
        const SourceImage& src = sources[b.source];
        std::string message;
        if (!rebake_image(src.pixels, b.space, options.normal_maps, b.normal_scale, tris, coverage,
                          options.dilation, pool, b.out, &message)) {
          return fail(error, "rebaking image " + std::to_string(b.source) + ": " + message);
        }
        // Measured the way the renderer reads each side — a normal map through each side's own
        // frame whether it was converted or resampled — so `resample` shows what it costs.
        b.error =
            measure_rebake(src.pixels, b.out, b.space, b.normal_scale, tris, options.error_samples);
      }
    }
    report.rebake_ms = ms_since(t);
  }

  // ---- encode, one job per image
  // ------------------------------------------------------------------
  {
    const i64 t = time::monotonic_ns();
    Vector<EncodeJob> encodes;
    for (Group& g : groups) {
      for (Bake& b : g.bakes)
        encodes.push_back(EncodeJob{&b, false});
    }
    run_jobs(pool, encodes, encode_bake);
    for (const EncodeJob& e : encodes) {
      if (!e.ok) return fail(error, "a rebaked image could not be encoded as PNG");
    }
    report.encode_ms = ms_since(t);
  }

  // ---- put it back: vertices, indices, morph deltas, images, material slots
  // ------------------------
  const u32 old_vertex_count = mesh.positions.size();
  if (mesh.uvs.empty()) mesh.uvs.assign(old_vertex_count, Vec2{0.0f, 0.0f});
  Vector<u32> appended_source;  // per appended vertex, the vertex it copies
  for (Group& g : groups) {
    const u32 base = mesh.positions.size();
    for (u32 k = 0; k < g.vertex_source.size(); ++k) {
      const u32 v = g.vertex_source[k];
      // Copied through a value: push_back may reallocate the vector it reads from.
      const Vec3 p = mesh.positions[v];
      mesh.positions.push_back(p);
      if (!mesh.normals.empty()) {
        const Vec3 n = mesh.normals[v];
        mesh.normals.push_back(n);
      }
      mesh.uvs.push_back(g.uv[k]);
      if (!mesh.skin_bindings.empty()) {
        const geometry::SkinBinding s = mesh.skin_bindings[v];
        mesh.skin_bindings.push_back(s);
      }
      // A copy of a vertex is the same point of the base: it keeps the vertex's canonical id, so
      // a file written beside the mesh still names it after the repack (geometry.md, "Canonical
      // vertex identity"), and the weld, which keys on the id, merges copies exactly as before.
      if (!mesh.vertex_ids.empty()) {
        const u32 id = mesh.vertex_ids[v];
        mesh.vertex_ids.push_back(id);
      }
      appended_source.push_back(v);
    }
    for (u32 t = 0; t < g.first_index.size(); ++t) {
      for (u32 k = 0; k < 3; ++k)
        mesh.indices[g.first_index[t] + k] = base + g.corners[t * 3 + k];
    }
    report.materials.push_back(g.report);
    ++report.materials_repacked;
    report.triangles += g.report.triangles;
    report.charts += g.report.charts;
  }
  // A morph channel's deltas follow a vertex into every copy of it, in the order the copies were
  // appended, so a sorted channel stays sorted.
  if (!mesh.morph.empty()) {
    Vector<u32> entry_of(old_vertex_count, ~0u);
    for (geometry::MorphChannelSource& channel : mesh.morph) {
      for (u32 i = 0; i < channel.vertices.size(); ++i)
        entry_of[channel.vertices[i]] = i;
      const u32 count = channel.vertices.size();
      for (u32 k = 0; k < appended_source.size(); ++k) {
        const u32 i = entry_of[appended_source[k]];
        if (i == ~0u) continue;
        channel.vertices.push_back(old_vertex_count + k);
        const Vec3 d = channel.position_deltas[i];
        channel.position_deltas.push_back(d);
        if (!channel.normal_deltas.empty()) {
          const Vec3 nd = channel.normal_deltas[i];
          channel.normal_deltas.push_back(nd);
        }
      }
      for (u32 i = 0; i < count; ++i)
        entry_of[channel.vertices[i]] = ~0u;
    }
  }
  report.vertices_after = mesh.positions.size();

  // Images: the source images some material still names (a material left as it was), in their
  // own order, then the rebaked ones in material order; the slots renumbered to match.
  for (Group& g : groups) {
    SlotRef slots[5];
    const u32 n = material_slots(mesh.materials[static_cast<u32>(g.material)], slots);
    for (u32 s = 0; s < n; ++s) {
      i32& image = *slots[s].image;
      if (image < 0) continue;
      for (u32 b = 0; b < g.bakes.size(); ++b) {
        if (g.bakes[b].source == static_cast<u32>(image) && g.bakes[b].space == slots[s].space) {
          image = -2 - static_cast<i32>(b);  // rebaked image b of this group, until renumbered
          break;
        }
      }
    }
  }
  Vector<u32> kept_index(mesh.images.size(), ~0u);
  Vector<assets::ImageRef> images;
  for (u32 m = 0; m < mesh.materials.size(); ++m) {
    SlotRef slots[5];
    const u32 n = material_slots(mesh.materials[m], slots);
    for (u32 s = 0; s < n; ++s) {
      const i32 image = *slots[s].image;
      if (image >= 0 && static_cast<u32>(image) < kept_index.size())
        kept_index[static_cast<u32>(image)] = 0;
    }
  }
  for (u32 i = 0; i < mesh.images.size(); ++i) {
    if (kept_index[i] == ~0u) continue;
    kept_index[i] = images.size();
    images.push_back(std::move(mesh.images[i]));
  }
  for (Group& g : groups) {
    for (u32 b = 0; b < g.bakes.size(); ++b) {
      Bake& bake = g.bakes[b];
      bake.final_index = images.size();
      assets::ImageRef ref;
      ref.name = "atlas.m" + std::to_string(g.material) + "." + space_name(bake.space);
      ref.mime_type = "image/png";
      ref.bytes = std::move(bake.png);
      report.image_bytes += ref.bytes.size();
      RebakedImage row;
      row.material = g.material;
      row.source_image = bake.source;
      row.image = bake.final_index;
      row.space = bake.space;
      row.width = bake.size;
      row.height = bake.size;
      row.source_width = sources[bake.source].pixels.width;
      row.source_height = sources[bake.source].pixels.height;
      row.png_bytes = ref.bytes.size();
      row.error = bake.error;
      report.images.push_back(row);
      images.push_back(std::move(ref));
    }
  }
  for (u32 m = 0; m < mesh.materials.size(); ++m) {
    SlotRef slots[5];
    const u32 n = material_slots(mesh.materials[m], slots);
    for (u32 s = 0; s < n; ++s) {
      i32& image = *slots[s].image;
      if (image >= 0) {
        image = static_cast<u32>(image) < kept_index.size()
                    ? static_cast<i32>(kept_index[static_cast<u32>(image)])
                    : image;
      } else if (image <= -2) {
        // Only the group of this material wrote negative slots below -1.
        for (const Group& g : groups) {
          if (g.material != static_cast<i32>(m)) continue;
          image = static_cast<i32>(g.bakes[static_cast<u32>(-2 - image)].final_index);
          break;
        }
      }
    }
  }
  mesh.images = std::move(images);
  report.images_after = mesh.images.size();
  report.total_ms = ms_since(start);
  return true;
}

}  // namespace engine::atlas
