#pragma once

// The content build's atlas step (docs/subsystems/atlas.md): **re-chart an imported mesh, pack the
// charts into one atlas per material, and rebake every texture the material samples into it**, so
// that a generator's UV layout stops deciding how far the LOD can collapse.
//
// Why it exists. The cluster LOD builder may slide a collapse along a UV island edge but never
// across one (`geometry::ClusterLodOptions::uv_seams`, docs/subsystems/geometry.md "What the
// simplifier is given, and why"), so an atlas cut into thousands of small islands leaves nothing to
// collapse: E10 (docs/experiments/e10-generated-props.md) measured Meshy's triangle remesh and
// TRELLIS.2 putting 34-98% of vertices on a seam, and their 1-pixel cuts drawing 70-100% of the
// finest cut. The atlas is the generator's, and a repair that keeps it keeps the problem. This
// step replaces it: the surface is cut into charts of its own — by xatlas on a simplified proxy of
// the mesh, carried onto the full mesh and parameterized there by least-squares conformal maps
// (proxy_charts.cpp), or by xatlas directly on a small mesh — with the old UVs and the normals
// ignored, the charts are packed into a square atlas as large as the material's largest texture,
// and every texture is resampled from the old UV space into the new one. A material whose new
// charts would fold more than a few percent of it is declined and keeps its own atlas.
//
// What it does to the mesh. Only **textured** materials are touched, one atlas each: a primitive
// whose material names no image keeps its UVs, and so does one whose images cannot all be read (a
// texture the rebake could not carry across would be sampled through the wrong UVs). For the rest,
// every triangle keeps its place in `indices` and its corners' positions, normals, skin bindings,
// morph deltas and canonical vertex ids; each corner gets a vertex whose UV is the new one, and a
// source vertex that a new chart boundary runs through becomes one vertex per chart, every copy
// with the source vertex's id (it is the same point of the base). The new vertices are **appended**
// and the old ones a repacked triangle no longer references are left for the weld that follows to
// drop, which is also what merges two old vertices the generator split for its own seam once they
// land in one chart with one UV. The new UVs are rounded to half floats before anything is baked
// against them, because the container stores them that way (`geometry::VertexAttributes`) and a
// texture baked for UVs the renderer does not interpolate is off by up to two texels of 4096.
//
// What it does to the textures. Every image a repacked material samples becomes a new embedded
// PNG, square, at the source image's own largest side: base colour and emissive are resampled in
// linear light (they are sRGB), metallic-roughness and occlusion as stored, and a tangent-space
// normal map is **converted**, not resampled: the tangent frame follows the UV parameterization, so
// a texel is decoded through the old triangle's frame into object space and re-encoded in the new
// triangle's frame (`NormalMapMode::resample` skips that, and exists to measure what it costs).
// The images no material references afterwards are dropped from `MeshData::images`, and the
// material slots are renumbered, so a container built from the result carries the rebaked images
// and nothing of the source's layout.
//
// Determinism. The output is a function of the mesh, the images and the options: xatlas is built
// single-threaded (cmake/EngineAtlas.cmake), the materials' charting and packing run as jobs whose
// results are merged in material order, and the rebake's rows are independent pure functions of
// the source image and the triangles. A `pool` of any size, or none, gives the same bytes.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/uv_repair.h>
#include <foundation/image/decode.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::jobs {
class JobSystem;
}

namespace engine::atlas {

// How a tangent-space normal map crosses from one UV parameterization to another.
enum class NormalMapMode : u8 {
  // Decode through the old triangle's tangent frame into object space and re-encode in the new
  // triangle's frame — the frame `material.slang`'s `tangent_frame` builds from the triangle's
  // positions and UVs, Gram-Schmidt against the interpolated normal. The default.
  convert = 0,
  // Resample the stored vectors as if they were colours. Wrong wherever the new chart is rotated
  // or mirrored against the old one; kept so the error it makes can be measured.
  resample = 1,
};

// What a texel of a rebaked image holds.
enum class TextureSpace : u8 {
  srgb = 0,            // base colour, emissive: filtered in linear light
  linear = 1,          // metallic-roughness, occlusion: filtered as stored
  tangent_normal = 2,  // a tangent-space normal map: see NormalMapMode
};

struct RepackOptions {
  // Gutter between charts, in texels of the atlas, on top of the ring bilinear filtering samples
  // (xatlas `PackOptions::padding`, with `bilinear` on). The rebake dilates into it.
  u32 padding = 4;
  // The atlas is square at the largest side of the material's images times `supersample`,
  // clamped to this range, and each rebaked image is its source's side times the same.
  u32 min_resolution = 256;
  u32 max_resolution = 8192;
  // 1, 2 or 4. A rebake resamples every texel once more than the source was, which softens a
  // texture the renderer samples without mips (atlas.md, "What it costs the picture"); baking at
  // twice the side halves the width of that extra filter, at four times the texels.
  u32 supersample = 1;
  // The charts are cut on a copy of the material's surface simplified to about this many
  // triangles, and carried onto the full mesh by projection (proxy_charts.h, atlas.md "Why a
  // proxy"); 0 lets xatlas chart the full mesh itself, which on a generated mesh leaves about as
  // many seams as the atlas being replaced. A mesh under twice this is charted directly either way.
  u32 proxy_triangles = 2000;
  // xatlas `ChartOptions::maxCost` in thousandths, for a mesh charted directly: how much normal
  // deviation, roundness and straightness a chart may accumulate before it stops growing. 2000 is
  // xatlas's own default. (The proxy is charted with xatlas's defaults.)
  u32 max_chart_cost_milli = 2000;
  NormalMapMode normal_maps = NormalMapMode::convert;
  // Dilation passes into the gutters after the rebake; what is still empty after them (the unused
  // part of the atlas) takes the mean of the covered texels.
  u32 dilation = 16;
  // How many surface samples the rebake error is measured over, per image (0 skips it).
  u32 error_samples = 65536;
};

// A stable 64-bit identity of the options and this module's own version, for the content build's
// cache key: two containers repacked with different options are two different meshes.
u64 repack_options_key(const RepackOptions& options) noexcept;

// How far a rebaked image is from its source, sampled on the surface: at points spread over the
// triangles, the source bilinearly at the old UV against the rebake bilinearly at the new one.
// Colour images in 8-bit units of the stored value (the largest channel difference); a converted
// normal map in degrees between the two object-space normals the renderer would shade with.
struct RebakeError {
  u32 samples = 0;
  f64 mean = 0.0;
  f64 p99 = 0.0;
  f64 max = 0.0;
};

struct RepackedMaterial {
  i32 material = -1;
  u32 triangles = 0;
  u32 charts = 0;
  u32 resolution = 0;           // the square atlas the charts were packed into
  f32 utilization = 0.0f;       // xatlas: covered texels over the atlas
  f32 texels_per_unit = 0.0f;   // in the normalized frame the charts were cut in (see repack.cpp)
  u32 pack_attempts = 0;        // packings, the estimate included, until the charts fit one atlas
  u32 unatlased_triangles = 0;  // faces xatlas refused, or split between two charts: not baked
  u32 crumb_charts = 0;         // chart pieces under one texel of a 4096 atlas as stored
  u32 crumb_triangles = 0;      // theirs: not baked, UVs from a chart beside them if one is
  u32 source_vertices = 0;      // distinct vertices the material's triangles used before
  u32 vertices = 0;             // vertices it has after, one per (vertex, chart)
  u32 proxy_triangles = 0;      // what the proxy simplified to; 0 when charted directly
  u32 proxy_charts = 0;         // the proxy's charts a triangle could land on
  u32 folded_triangles = 0;     // triangles whose new UVs wind against their chart's
  f64 chart_ms = 0.0;           // charting and parameterization (the proxy's included)
  f64 pack_ms = 0.0;            // every packing, together
};

struct RebakedImage {
  i32 material = -1;
  u32 source_image = 0;  // index into the MeshData::images the step was given
  u32 image = 0;         // index into the MeshData::images it left
  TextureSpace space = TextureSpace::srgb;
  u32 width = 0;
  u32 height = 0;
  u32 source_width = 0;
  u32 source_height = 0;
  u64 png_bytes = 0;
  RebakeError error;
};

struct RepackReport {
  u32 materials_repacked = 0;
  u32 materials_untextured = 0;  // named no image: left as they were
  u32 materials_unreadable = 0;  // named an image that could not be read or decoded: left
  u32 materials_declined = 0;    // new charts would fold too much of it (repack.cpp): left
  u32 triangles = 0;             // in repacked materials
  u32 charts = 0;
  u32 vertices_before = 0;  // the whole mesh, as given
  u32 vertices_after = 0;   // the whole mesh, before the weld drops what nothing references
  u32 images_before = 0;
  u32 images_after = 0;
  u64 image_bytes = 0;  // PNG bytes of the rebaked images
  f64 decode_ms = 0.0;
  f64 chart_ms = 0.0;  // the materials' xatlas runs, summed (they run concurrently)
  f64 rebake_ms = 0.0;
  f64 encode_ms = 0.0;
  f64 total_ms = 0.0;
  Vector<RepackedMaterial> materials;
  Vector<RebakedImage> images;
  Vector<std::string> notes;  // why a material was left as it was, one sentence each
};

// Repacks every textured material of `mesh` in place (the header comment above has the rules).
// `source_dir` is what an image's `uri` is relative to — the glTF file's directory, as
// `load_gltf` resolved it. `pool` runs the materials and the rebake's rows; null runs everything
// on the calling thread, with the same result. Returns false with a sentence and the mesh
// unchanged when the mesh is malformed or xatlas refuses it; a material it cannot repack is not a
// failure, it is left as it was and named in `report.notes`.
bool repack_atlas(assets::MeshData& mesh, std::string_view source_dir, const RepackOptions& options,
                  jobs::JobSystem* pool, RepackReport& report, std::string* error = nullptr);

// ---- the rebake, on its own ------------------------------------------------------------------
//
// Public so that it can be tested against a known answer without a mesh or xatlas in the way.
// Triangles are given corner by corner, three entries per triangle in every span: UVs in image
// space (0..1 across the image, v down, as glTF has them), positions and normals only for a normal
// map in `convert` mode.
struct RebakeTriangles {
  std::span<const Vec2> old_uv;
  std::span<const Vec2> new_uv;
  std::span<const Vec3> positions;
  std::span<const Vec3> normals;
};

// Which triangle owns each texel of a `size` x `size` image laid out by `new_uv`: the first
// triangle whose interior holds the texel's centre, else the nearest triangle within 1.5 texels
// (the ring bilinear filtering reaches from inside a chart, which the packer left room for), else
// none. Ties go to the lower triangle index, so the map is a function of the input alone.
struct TexelCoverage {
  u32 size = 0;
  Vector<u32> owner;  // per texel: triangle index, | k_ring when from the ring; k_uncovered if none
  static constexpr u32 k_ring = 0x80000000u;
  static constexpr u32 k_uncovered = 0xffffffffu;
};

bool build_coverage(std::span<const Vec2> new_uv, u32 size, TexelCoverage& out,
                    std::string* error = nullptr);

// Rebakes one image: every covered texel samples `source` bilinearly (repeat wrap) at the old UV
// of its point on the owning triangle (the closest point, for a ring texel), in the space given;
// then `dilation` passes spread the covered texels into the empty ones and the rest take the
// covered mean. `out` is RGBA8, `coverage.size` square. `normal_scale` is the material's
// normalTexture.scale, which the renderer applies before the frame and the conversion undoes after.
bool rebake_image(const image::Image& source, TextureSpace space, NormalMapMode mode,
                  f32 normal_scale, const RebakeTriangles& triangles, const TexelCoverage& coverage,
                  u32 dilation, jobs::JobSystem* pool, image::Image& out,
                  std::string* error = nullptr);

// The error `RebakeError` describes, over at most `max_samples` points at a fixed stride.
RebakeError measure_rebake(const image::Image& source, const image::Image& rebaked,
                           TextureSpace space, f32 normal_scale, const RebakeTriangles& triangles,
                           u32 max_samples);

}  // namespace engine::atlas
