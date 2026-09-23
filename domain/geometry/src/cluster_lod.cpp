#include <domain/geometry/cluster_lod.h>

#include <meshoptimizer.h>

// clusterlod.h is meshoptimizer's reference cluster LOD builder, header-only with the
// implementation compiled into this one translation unit.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>

namespace engine::geometry {

namespace {

struct BuildContext {
  std::span<const Vec3> positions;
  ClusterLodMesh* out;
  bool normal_cones = true;
  Vector<clodBounds> group_bounds;  // simplified bounds per group, in output order
  Vector<unsigned int> local_vertices;
  Vector<unsigned char> local_triangles;
  bool failed = false;
};

// The attribute record the simplifier is given per source vertex: the normal, then the UV.
// Five floats and a matching stride, so `attribute_protect_mask` bit k names slot k — the same
// layout meshoptimizer's own cluster-LOD sample uses, with the UVs where it puts them.
constexpr u32 k_lod_attribute_count = 5;

u8 seam_bit(SeamRule rule) noexcept {
  switch (rule) {
    case SeamRule::protect: return static_cast<u8>(meshopt_SimplifyVertex_Protect);
    case SeamRule::lock: return static_cast<u8>(meshopt_SimplifyVertex_Lock);
    case SeamRule::none: break;
  }
  return 0;
}

// Appends `part`'s vertex ids to `to`, or `k_no_vertex_id` for each of its vertices when it carries
// none, so the merged stream stays parallel to the merged vertices. `to.vertices` must already hold
// the part's vertices. Nothing is appended when no part of the merge has ids (`wanted` false),
// which is what keeps a merge of id-less meshes exactly what it was.
void append_vertex_ids(ClusterMesh& to, const ClusterMesh& part, bool wanted) {
  if (!wanted) return;
  if (!part.vertex_ids.empty() && part.vertex_ids.size() == part.vertices.size() &&
      part.vertex_id_source != VertexIdSource::none) {
    to.vertex_ids.append(std::span<const u32>(part.vertex_ids.data(), part.vertex_ids.size()));
  } else {
    to.vertex_ids.resize(to.vertices.size(), k_no_vertex_id);
  }
}

int emit_group(void* context, clodGroup group, const clodCluster* clusters, size_t cluster_count) {
  auto* ctx = static_cast<BuildContext*>(context);
  ClusterLodMesh& out = *ctx->out;
  const int group_index = static_cast<int>(ctx->group_bounds.size());
  ctx->group_bounds.push_back(group.simplified);
  const u32 depth = group.depth >= 0 ? static_cast<u32>(group.depth) : 0;
  while (out.level_cluster_counts.size() <= depth)
    out.level_cluster_counts.push_back(0);

  for (size_t i = 0; i < cluster_count; ++i) {
    const clodCluster& cluster = clusters[i];
    if (cluster.vertex_count > 255 || cluster.index_count / 3 > 512) {
      ctx->failed = true;
      return group_index;
    }
    ctx->local_vertices.resize(static_cast<u32>(cluster.vertex_count));
    ctx->local_triangles.resize(static_cast<u32>(cluster.index_count));
    clodLocalIndices(ctx->local_vertices.data(), ctx->local_triangles.data(), cluster.indices,
                     cluster.index_count);

    ClusterDesc desc;
    desc.vertex_offset = out.mesh.vertices.size();
    desc.triangle_offset = out.mesh.triangles.size();
    desc.vertex_count = static_cast<u32>(cluster.vertex_count);
    desc.triangle_count = static_cast<u32>(cluster.index_count / 3);
    desc.center =
        Vec3{cluster.bounds.center[0], cluster.bounds.center[1], cluster.bounds.center[2]};
    desc.radius = cluster.bounds.radius;
    // clusterlod bounds carry no cone; meshoptimizer's meshlet bounds do, on the local indices.
    const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
        ctx->local_vertices.data(), ctx->local_triangles.data(), cluster.index_count / 3,
        &ctx->positions[0].x, ctx->positions.size(), sizeof(Vec3));
    desc.cone_apex = Vec3{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]};
    if (ctx->normal_cones) {
      auto byte = [](signed char v) { return u32{static_cast<u8>(v)}; };
      desc.cone = byte(bounds.cone_axis_s8[0]) | (byte(bounds.cone_axis_s8[1]) << 8) |
                  (byte(bounds.cone_axis_s8[2]) << 16) | (byte(bounds.cone_cutoff_s8) << 24);
    }
    for (u32 v = 0; v < desc.vertex_count; ++v) {
      const u32 source = ctx->local_vertices[v];
      out.mesh.vertices.push_back(ctx->positions[source]);
      out.mesh.vertex_source.push_back(source);
    }
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const unsigned char* tri = &ctx->local_triangles[t * 3];
      out.mesh.triangles.push_back(ClusterMesh::pack(tri[0], tri[1], tri[2]));
    }
    out.mesh.clusters.push_back(desc);

    ClusterLodDesc lod;
    if (cluster.refined >= 0) {
      const clodBounds& own = ctx->group_bounds[static_cast<u32>(cluster.refined)];
      lod.own = Vec4{own.center[0], own.center[1], own.center[2], own.radius};
      lod.own_error = own.error;
    } else {
      lod.own = Vec4{desc.center.x, desc.center.y, desc.center.z, desc.radius};
      lod.own_error = 0.0f;
    }
    lod.parent = Vec4{group.simplified.center[0], group.simplified.center[1],
                      group.simplified.center[2], group.simplified.radius};
    lod.parent_error = group.simplified.error;
    lod.level = depth;
    lod.group = static_cast<u32>(group_index);
    out.lod.push_back(lod);
    ++out.level_cluster_counts[depth];
    if (depth == 0) out.leaf_triangle_count += desc.triangle_count;
  }
  return group_index;
}

}  // namespace

bool build_cluster_lod(std::span<const Vec3> positions, std::span<const u32> indices,
                       const ClusterLodOptions& options, ClusterLodMesh& out, std::string* error,
                       const AttributeSource& attributes) {
  out = ClusterLodMesh{};
  if (positions.empty() || indices.empty() || indices.size() % 3 != 0) {
    if (error != nullptr)
      *error = "build_cluster_lod: need vertices and a multiple of three indices";
    return false;
  }
  if (options.max_triangles < 4 || options.max_triangles > 256 || options.max_vertices == 0 ||
      options.max_vertices > 255) {
    if (error != nullptr)
      *error = "build_cluster_lod: max_triangles must be 4..256 and max_vertices 1..255";
    return false;
  }
  for (const u32 index : indices) {
    if (index >= positions.size()) {
      if (error != nullptr) *error = "build_cluster_lod: index out of range";
      return false;
    }
  }

  clodConfig config = options.ray_tracing ? clodDefaultConfigRT(options.max_triangles)
                                          : clodDefaultConfig(options.max_triangles);
  config.max_vertices = options.max_vertices;
  config.optimize_bounds = true;
  clodMesh mesh{};
  mesh.indices = indices.data();
  mesh.index_count = indices.size();
  mesh.vertex_count = positions.size();
  mesh.vertex_positions = &positions[0].x;
  mesh.vertex_positions_stride = sizeof(Vec3);

  // What the simplifier is given beyond the positions, and why it has to be given it.
  //
  // `clodDefaultConfig` runs meshoptimizer's **permissive** simplification, whose whole meaning
  // is "a collapse may cross an attribute discontinuity unless the vertex is tagged". Handed
  // positions alone — no attributes, no weights, no tags, which is what this builder did until
  // 2026-09-19 — it has neither a reason to avoid such a collapse (there is no attribute term in
  // its error metric at all) nor a rule forbidding one. On an atlas of large clean islands that
  // is invisible. On one made of thousands of small islands, which is what an AI generator and a
  // photogrammetry scan produce, a single collapse welds two unrelated islands together and every
  // triangle spanning the join interpolates its UVs across the whole atlas.
  //
  // So: the normal and the UV as weighted attributes, and a tag on every vertex that sits on a
  // discontinuity. Both arrays are indexed by *source* vertex, which is the space clusterlod's
  // indices live in.
  Vector<f32> attribute_values;
  Vector<u8> vertex_lock;
  f32 attribute_weights[k_lod_attribute_count] = {};
  Vector<Vec3> computed_normals;
  const u8 uv_bit = seam_bit(options.uv_seams);
  const u8 normal_bit = seam_bit(options.normal_seams);
  const u8 skin_bit = seam_bit(options.skin_seams);
  const u8 morph_bit = attributes.morph.empty() ? u8{0} : seam_bit(options.morph_seams);
  const bool have_ids = attributes.vertex_ids.size() == positions.size() &&
                        attributes.vertex_id_source != VertexIdSource::none;
  const u8 id_bit = have_ids ? seam_bit(options.id_seams) : u8{0};
  const bool weighted = options.normal_weight > 0.0f || options.uv_weight > 0.0f;
  const bool tagged =
      uv_bit != 0 || normal_bit != 0 || skin_bit != 0 || morph_bit != 0 || id_bit != 0;
  const u32 vertex_count = static_cast<u32>(positions.size());
  std::span<const Vec3> normals = attributes.normals;
  // Only when something will read them: with `normal_weight` at its default of zero and no
  // normal-seam rule, computing them would be a pass over the mesh nobody looks at.
  if ((options.normal_weight > 0.0f || normal_bit != 0) && normals.size() != positions.size()) {
    compute_vertex_normals(positions, indices, computed_normals);
    normals = std::span<const Vec3>(computed_normals.data(), computed_normals.size());
  }
  const bool have_uvs = attributes.uvs.size() == positions.size();
  const bool have_skin = attributes.skin.size() == positions.size();
  if (weighted) {
    attribute_values.resize(vertex_count * k_lod_attribute_count, 0.0f);
    for (u32 v = 0; v < vertex_count; ++v) {
      f32* record = &attribute_values[v * k_lod_attribute_count];
      const Vec3 n = v < normals.size() ? normals[v] : Vec3{0.0f, 1.0f, 0.0f};
      record[0] = n.x;
      record[1] = n.y;
      record[2] = n.z;
      if (have_uvs) {
        record[3] = attributes.uvs[v].x;
        record[4] = attributes.uvs[v].y;
      }
    }
    attribute_weights[0] = options.normal_weight;
    attribute_weights[1] = options.normal_weight;
    attribute_weights[2] = options.normal_weight;
    attribute_weights[3] = have_uvs ? options.uv_weight : 0.0f;
    attribute_weights[4] = have_uvs ? options.uv_weight : 0.0f;
    mesh.vertex_attributes = attribute_values.data();
    mesh.vertex_attributes_stride = sizeof(f32) * k_lod_attribute_count;
    mesh.attribute_weights = attribute_weights;
    mesh.attribute_count = k_lod_attribute_count;
  }
  if (tagged) {
    // A discontinuity is a vertex that shares a *position* with another vertex but disagrees with
    // it about an attribute — precisely the duplicates `weld_vertices` refuses to merge, since it
    // keys on position, normal, UV and binding together. meshoptimizer's own position remap is
    // what clusterlod uses for connectivity, so asking it the same question asks it in the same
    // terms. The tag goes on the duplicate rather than on the canonical vertex, which is enough:
    // the simplifier walks a vertex's whole wedge ring and protects the ring if any wedge is
    // tagged.
    Vector<unsigned int> position_remap(vertex_count);
    meshopt_generatePositionRemap(position_remap.data(), &positions[0].x, positions.size(),
                                  sizeof(Vec3));
    // The morph comparison uses the same interned id the weld keyed on, so "these two vertices
    // carry different deltas" means exactly the same thing in both places, and the walk is over
    // the sparse data rather than over channels x vertices.
    Vector<u32> morph_keys;
    if (morph_bit != 0) morph_vertex_keys(attributes.morph, vertex_count, morph_keys);
    vertex_lock.resize(vertex_count, 0u);
    for (u32 v = 0; v < vertex_count; ++v) {
      const u32 canonical = position_remap[v];
      if (canonical == v) continue;
      if (uv_bit != 0 && have_uvs &&
          (attributes.uvs[v].x != attributes.uvs[canonical].x ||
           attributes.uvs[v].y != attributes.uvs[canonical].y)) {
        vertex_lock[v] |= uv_bit;
      }
      if (normal_bit != 0 && v < normals.size() && canonical < normals.size() &&
          (normals[v].x != normals[canonical].x || normals[v].y != normals[canonical].y ||
           normals[v].z != normals[canonical].z)) {
        vertex_lock[v] |= normal_bit;
      }
      if (skin_bit != 0 && have_skin) {
        const SkinBinding& a = attributes.skin[v];
        const SkinBinding& b = attributes.skin[canonical];
        bool same = true;
        for (u32 k = 0; k < 4; ++k)
          same = same && a.joints[k] == b.joints[k] && a.weights[k] == b.weights[k];
        if (!same) vertex_lock[v] |= skin_bit;
      }
      if (morph_bit != 0 && v < morph_keys.size() && canonical < morph_keys.size() &&
          morph_keys[v] != morph_keys[canonical]) {
        vertex_lock[v] |= morph_bit;
      }
      // Two points of the base the author named apart, at one position: the weld kept them apart,
      // and so does the simplifier.
      if (id_bit != 0 && attributes.vertex_ids[v] != attributes.vertex_ids[canonical])
        vertex_lock[v] |= id_bit;
    }
    mesh.vertex_lock = vertex_lock.data();
  }

  BuildContext ctx;
  ctx.positions = positions;
  ctx.out = &out;
  ctx.normal_cones = options.normal_cones;
  out.mesh.source_vertex_count = static_cast<u32>(positions.size());
  out.mesh.source_triangle_count = static_cast<u32>(indices.size() / 3);
  const size_t produced = clodBuild(config, mesh, &ctx, &emit_group);
  if (ctx.failed) {
    if (error != nullptr) *error = "build_cluster_lod: a cluster exceeded the format limits";
    out = ClusterLodMesh{};
    return false;
  }
  if (produced != out.mesh.clusters.size() || out.mesh.clusters.empty()) {
    if (error != nullptr) *error = "build_cluster_lod: the builder produced no clusters";
    out = ClusterLodMesh{};
    return false;
  }
  out.group_count = ctx.group_bounds.size();
  fill_cluster_attributes(out.mesh, positions, indices, attributes);
  quantize_positions(out.mesh);  // one grid over every level
  return true;
}

f32 projected_error(Vec4 sphere, f32 error, const LodView& view) noexcept {
  if (error >= k_lod_terminal_error) return k_lod_terminal_error;
  const Vec3 center{sphere.x, sphere.y, sphere.z};
  const f32 distance = length(center - view.camera) - sphere.w;
  const f32 clamped = distance > view.znear ? distance : view.znear;
  return error / clamped * view.proj_scale;
}

bool lod_selects(const ClusterLodDesc& lod, const LodView& view) noexcept {
  const f32 own = projected_error(lod.own, lod.own_error, view);
  const f32 parent = projected_error(lod.parent, lod.parent_error, view);
  return own <= view.threshold_px && parent > view.threshold_px;
}

u32 select_lod(const ClusterLodMesh& mesh, const LodView& view, Vector<u32>& out) {
  u32 count = 0;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    if (lod_selects(mesh.lod[i], view)) {
      out.push_back(i);
      ++count;
    }
  }
  return count;
}

u32 select_lod_raw(const ClusterLodMesh& mesh, f32 threshold, Vector<u32>& out) {
  u32 count = 0;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    const ClusterLodDesc& lod = mesh.lod[i];
    if (lod.own_error <= threshold && lod.parent_error > threshold) {
      out.push_back(i);
      ++count;
    }
  }
  return count;
}

bool merge_cluster_lod(std::span<const ClusterLodMesh> parts, ClusterLodMesh& out,
                       Vector<u32>* part_of_cluster, std::string* error) {
  out = ClusterLodMesh{};
  if (part_of_cluster != nullptr) part_of_cluster->clear();
  if (parts.empty()) {
    if (error != nullptr) *error = "merge_cluster_lod: no parts";
    return false;
  }
  u32 levels = 0;
  for (const ClusterLodMesh& part : parts) {
    if (part.mesh.clusters.empty() || part.lod.size() != part.mesh.clusters.size() ||
        part.mesh.vertex_source.size() != part.mesh.vertices.size()) {
      if (error != nullptr) *error = "merge_cluster_lod: a part is empty or inconsistent";
      return false;
    }
    levels = part.level_cluster_counts.size() > levels ? part.level_cluster_counts.size() : levels;
  }
  out.level_cluster_counts.resize(levels, 0u);

  // Streams first, remembering where each part landed.
  const u32 part_count = static_cast<u32>(parts.size());
  Vector<u32> vertex_base(part_count);
  Vector<u32> triangle_base(part_count);
  Vector<u32> group_base(part_count);
  const bool attributes = parts[0].mesh.attributes.size() == parts[0].mesh.vertices.size();
  // The parts of one mesh share one skin, so the merged palette is the widest of them; a part
  // with no bindings contributes default ones rather than shortening the stream, exactly as it
  // does for attributes.
  const bool skinned = parts[0].mesh.skin.size() == parts[0].mesh.vertices.size();
  // **The parts of one mesh share one set of morph channels**, by index: glTF requires every
  // primitive of a mesh to have the same number of targets, and the mesh's weights array drives
  // all of them, so channel k means the same expression in every part. The scales are computed
  // from the *whole* source channel (`fill_cluster_morph`), so every part quantized channel k on
  // the same grid and the merge is a concatenation with no requantization — but only if the parts
  // really do agree, so that is checked rather than assumed.
  u32 morph_channels = 0;
  const ClusterMesh* morph_template = nullptr;
  for (const ClusterLodMesh& whole : parts) {
    if (whole.mesh.morph_channels.empty()) continue;
    if (morph_template == nullptr) {
      morph_template = &whole.mesh;
      morph_channels = whole.mesh.morph_channels.size();
      continue;
    }
    if (whole.mesh.morph_channels.size() != morph_channels) {
      if (error != nullptr)
        *error = "merge_cluster_lod: the parts disagree about how many morph channels they have";
      return false;
    }
    for (u32 c = 0; c < morph_channels; ++c) {
      const MorphChannel& a = morph_template->morph_channels[c];
      const MorphChannel& b = whole.mesh.morph_channels[c];
      if (a.position_scale != b.position_scale || a.normal_scale != b.normal_scale) {
        if (error != nullptr)
          *error = "merge_cluster_lod: two parts quantized one morph channel differently";
        return false;
      }
    }
  }
  // **The parts of one mesh share one id space** — they were built over the same welded source — so
  // the ids concatenate unchanged, and the parts must agree about where they came from. A part
  // built without ids contributes `k_no_vertex_id` rather than shortening the stream, as a part
  // with no bindings contributes default ones.
  VertexIdSource id_source = VertexIdSource::none;
  for (const ClusterLodMesh& whole : parts) {
    const VertexIdSource source = whole.mesh.vertex_id_source;
    if (source == VertexIdSource::none) continue;
    if (id_source != VertexIdSource::none && source != id_source) {
      if (error != nullptr)
        *error = "merge_cluster_lod: the parts disagree about where their vertex ids came from";
      return false;
    }
    id_source = source;
  }
  out.mesh.vertex_id_source = id_source;
  bool morph_normals = false;
  if (morph_template != nullptr) {
    out.mesh.morph_channels.append(std::span<const MorphChannel>(
        morph_template->morph_channels.data(), morph_template->morph_channels.size()));
    out.mesh.morph_names = morph_template->morph_names;
    for (const ClusterLodMesh& whole : parts) {
      morph_normals = morph_normals || !whole.mesh.morph_normal_deltas.empty();
      // The largest displacement of a channel is the largest over every part that carries it.
      for (u32 c = 0; c < whole.mesh.morph_channels.size() && c < morph_channels; ++c) {
        MorphChannel& into = out.mesh.morph_channels[c];
        const MorphChannel& from = whole.mesh.morph_channels[c];
        if (from.max_displacement > into.max_displacement)
          into.max_displacement = from.max_displacement;
      }
    }
  }
  for (u32 p = 0; p < part_count; ++p) {
    const ClusterMesh& part = parts[p].mesh;
    if (skinned) {
      out.mesh.skin_joint_count = part.skin_joint_count > out.mesh.skin_joint_count
                                      ? part.skin_joint_count
                                      : out.mesh.skin_joint_count;
    }
    vertex_base[p] = out.mesh.vertices.size();
    triangle_base[p] = out.mesh.triangles.size();
    group_base[p] = out.group_count;
    const u32 source_base = out.mesh.source_vertex_count;
    out.mesh.vertices.append(std::span<const Vec3>(part.vertices.data(), part.vertices.size()));
    for (const u32 source : part.vertex_source)
      out.mesh.vertex_source.push_back(source_base + source);
    if (attributes && part.attributes.size() == part.vertices.size()) {
      out.mesh.attributes.append(
          std::span<const VertexAttributes>(part.attributes.data(), part.attributes.size()));
    } else {
      out.mesh.attributes.resize(out.mesh.vertices.size(), VertexAttributes{});
    }
    if (skinned && part.skin.size() == part.vertices.size()) {
      out.mesh.skin.append(std::span<const SkinBinding>(part.skin.data(), part.skin.size()));
    } else if (skinned) {
      out.mesh.skin.resize(out.mesh.vertices.size(), SkinBinding{});
    }
    append_vertex_ids(out.mesh, part, id_source != VertexIdSource::none);
    out.mesh.triangles.append(std::span<const u32>(part.triangles.data(), part.triangles.size()));
    out.mesh.source_vertex_count += part.source_vertex_count;
    out.mesh.source_triangle_count += part.source_triangle_count;
    out.group_count += parts[p].group_count;
    out.leaf_triangle_count += parts[p].leaf_triangle_count;
    for (u32 level = 0; level < parts[p].level_cluster_counts.size(); ++level)
      out.level_cluster_counts[level] += parts[p].level_cluster_counts[level];
  }

  // Clusters: every part's leaves, then everything else, in part order. The morph directory is
  // filled in that same order, which is what keeps the delta runs in slice order.
  if (morph_template != nullptr) out.mesh.morph_cluster_slices.push_back(0);
  for (u32 pass = 0; pass < 2; ++pass) {
    for (u32 p = 0; p < part_count; ++p) {
      const ClusterLodMesh& part = parts[p];
      for (u32 i = 0; i < part.mesh.clusters.size(); ++i) {
        const bool leaf = part.lod[i].level == 0;
        if (leaf != (pass == 0)) continue;
        ClusterDesc desc = part.mesh.clusters[i];
        desc.vertex_offset += vertex_base[p];
        desc.triangle_offset += triangle_base[p];
        ClusterLodDesc lod = part.lod[i];
        lod.group += group_base[p];
        out.mesh.clusters.push_back(desc);
        out.lod.push_back(lod);
        if (morph_template != nullptr) {
          append_cluster_morph(out.mesh, part.mesh, i, 0, morph_normals);
          out.mesh.morph_cluster_slices.push_back(out.mesh.morph_slices.size());
        }
        if (part_of_cluster != nullptr) part_of_cluster->push_back(p);
      }
    }
  }
  pad_morph_streams(out.mesh);
  quantize_positions(out.mesh);  // one grid over all the parts, not one per part
  return true;
}

bool merge_cluster_meshes(std::span<const ClusterLodMesh> parts, ClusterLodMesh& out,
                          Vector<ClusterMeshPart>& parts_out, std::string* error,
                          ClusterOrder order) {
  out = ClusterLodMesh{};
  parts_out.clear();
  if (parts.empty()) {
    if (error != nullptr) *error = "merge_cluster_meshes: no meshes";
    return false;
  }
  u32 levels = 0;
  for (const ClusterLodMesh& part : parts) {
    if (part.mesh.clusters.empty() || part.lod.size() != part.mesh.clusters.size() ||
        part.mesh.vertex_source.size() != part.mesh.vertices.size() ||
        part.mesh.quantized.size() < part.mesh.vertices.size() * 3) {
      if (error != nullptr) *error = "merge_cluster_meshes: a mesh is empty or inconsistent";
      return false;
    }
    levels = part.level_cluster_counts.size() > levels ? part.level_cluster_counts.size() : levels;
  }
  out.level_cluster_counts.resize(levels, 0u);
  parts_out.resize(static_cast<u32>(parts.size()));

  const bool attributes = parts[0].mesh.attributes.size() == parts[0].mesh.vertices.size();
  // Separate meshes have separate skeletons, so the merged palette width is only the bound the
  // validator needs: a mesh's own bindings index its own skin, and the renderer reaches the right
  // bone matrices through the *instance's* deform record, never through the merged mesh.
  const bool skinned = parts[0].mesh.skin.size() == parts[0].mesh.vertices.size();
  // Separate meshes have separate channel sets, so the merged array is their concatenation and a
  // mesh's run is recorded on its `ClusterMeshPart` — the same split the skin palette gets, and
  // for the same reason: an instance reaches its own weights through its deform record, never
  // through the merged mesh.
  bool morph_normals = false;
  bool any_morph = false;
  for (const ClusterLodMesh& whole : parts) {
    any_morph = any_morph || !whole.mesh.morph_channels.empty();
    morph_normals = morph_normals || !whole.mesh.morph_normal_deltas.empty();
  }
  // Separate meshes have separate id spaces, so the merged stream is their concatenation and each
  // mesh says on its own `ClusterMeshPart` where its ids came from. The merged mesh's own source is
  // the first mesh's that has one — what a one-mesh merge needs, and all a whole-scene validator
  // can ask of it; a caller that wants to know what an id means asks the part.
  bool any_ids = false;
  for (const ClusterLodMesh& whole : parts) {
    if (whole.mesh.vertex_id_source == VertexIdSource::none || whole.mesh.vertex_ids.empty())
      continue;
    if (!any_ids) out.mesh.vertex_id_source = whole.mesh.vertex_id_source;
    any_ids = true;
  }
  if (any_morph) out.mesh.morph_cluster_slices.push_back(0);
  for (u32 p = 0; p < parts.size(); ++p) {
    const ClusterLodMesh& whole = parts[p];
    const ClusterMesh& part = whole.mesh;
    if (skinned) {
      out.mesh.skin_joint_count = part.skin_joint_count > out.mesh.skin_joint_count
                                      ? part.skin_joint_count
                                      : out.mesh.skin_joint_count;
    }
    ClusterMeshPart& info = parts_out[p];
    info.first_vertex = out.mesh.vertices.size();
    info.first_cluster = out.mesh.clusters.size();
    info.cluster_count = part.clusters.size();
    info.leaf_cluster_count =
        whole.level_cluster_counts.empty() ? 0 : whole.level_cluster_counts[0];
    info.quant_origin = part.quant_origin;
    info.quant_scale = part.quant_scale;
    info.first_morph_channel = out.mesh.morph_channels.size();
    info.morph_channel_count = part.morph_channels.size();
    const u32 morph_base = info.first_morph_channel;
    out.mesh.morph_channels.append(
        std::span<const MorphChannel>(part.morph_channels.data(), part.morph_channels.size()));
    for (const std::string& name : part.morph_names)
      out.mesh.morph_names.push_back(name);
    const u32 vertex_base = info.first_vertex;
    const u32 triangle_base = out.mesh.triangles.size();
    const u32 group_base = out.group_count;
    const u32 source_base = out.mesh.source_vertex_count;
    out.mesh.vertices.append(std::span<const Vec3>(part.vertices.data(), part.vertices.size()));
    for (const u32 source : part.vertex_source)
      out.mesh.vertex_source.push_back(source_base + source);
    if (attributes && part.attributes.size() == part.vertices.size()) {
      out.mesh.attributes.append(
          std::span<const VertexAttributes>(part.attributes.data(), part.attributes.size()));
    } else {
      out.mesh.attributes.resize(out.mesh.vertices.size(), VertexAttributes{});
    }
    if (skinned && part.skin.size() == part.vertices.size()) {
      out.mesh.skin.append(std::span<const SkinBinding>(part.skin.data(), part.skin.size()));
    } else if (skinned) {
      out.mesh.skin.resize(out.mesh.vertices.size(), SkinBinding{});
    }
    info.vertex_id_source =
        part.vertex_ids.size() == part.vertices.size() && !part.vertex_ids.empty()
            ? part.vertex_id_source
            : VertexIdSource::none;
    append_vertex_ids(out.mesh, part, any_ids);
    out.mesh.triangles.append(std::span<const u32>(part.triangles.data(), part.triangles.size()));
    // Three u16 a vertex, the mesh's own padding dropped so the merged index is 3 * vertex.
    out.mesh.quantized.append(
        std::span<const u16>(part.quantized.data(), part.vertices.size() * 3));
    out.mesh.source_vertex_count += part.source_vertex_count;
    out.mesh.source_triangle_count += part.source_triangle_count;
    out.group_count += whole.group_count;
    out.leaf_triangle_count += whole.leaf_triangle_count;
    for (u32 level = 0; level < whole.level_cluster_counts.size(); ++level)
      out.level_cluster_counts[level] += whole.level_cluster_counts[level];
    // This mesh's clusters, contiguously.
    auto emit = [&](u32 i) {
      ClusterDesc desc = part.clusters[i];
      desc.vertex_offset += vertex_base;
      desc.triangle_offset += triangle_base;
      ClusterLodDesc lod = whole.lod[i];
      lod.group += group_base;
      out.mesh.clusters.push_back(desc);
      out.lod.push_back(lod);
      if (any_morph) {
        append_cluster_morph(out.mesh, part, i, morph_base, morph_normals);
        out.mesh.morph_cluster_slices.push_back(out.mesh.morph_slices.size());
      }
    };
    if (order == ClusterOrder::keep) {
      // A paged mesh is already in the one order its page table describes, so the merge only
      // shifts the offsets. Its leaves are wherever the layout put them, which is at the end.
      u32 first_leaf = part.clusters.size();
      for (u32 i = 0; i < part.clusters.size(); ++i) {
        if (whole.lod[i].level == 0 && i < first_leaf) first_leaf = i;
        emit(i);
      }
      info.first_leaf_cluster = first_leaf < part.clusters.size() ? first_leaf : 0;
    } else {
      // The scene convention: this mesh's own leaves first, so a direct draw of them is one range.
      for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < part.clusters.size(); ++i) {
          if ((whole.lod[i].level == 0) == (pass == 0)) emit(i);
        }
      }
    }
  }
  // The shaders read the last triple as two whole 32-bit words.
  if (out.mesh.quantized.size() % 2 != 0) out.mesh.quantized.push_back(0);
  pad_morph_streams(out.mesh);
  out.mesh.quant_origin = parts[0].mesh.quant_origin;
  out.mesh.quant_scale = parts[0].mesh.quant_scale;
  return true;
}

namespace {

// The closest point of triangle (a, b, c) to p, with its barycentric coordinates. Ericson,
// "Real-Time Collision Detection", the region test written out rather than solved, because the
// degenerate cases (a sliver, a point) have to answer *something* and this form does.
Vec3 closest_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c, f32& wa, f32& wb, f32& wc) noexcept {
  const Vec3 ab = b - a;
  const Vec3 ac = c - a;
  const Vec3 ap = p - a;
  const f32 d1 = dot(ab, ap);
  const f32 d2 = dot(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f) {
    wa = 1.0f;
    wb = 0.0f;
    wc = 0.0f;
    return a;
  }
  const Vec3 bp = p - b;
  const f32 d3 = dot(ab, bp);
  const f32 d4 = dot(ac, bp);
  if (d3 >= 0.0f && d4 <= d3) {
    wa = 0.0f;
    wb = 1.0f;
    wc = 0.0f;
    return b;
  }
  const f32 vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
    const f32 denominator = d1 - d3;
    const f32 v = denominator != 0.0f ? d1 / denominator : 0.0f;
    wa = 1.0f - v;
    wb = v;
    wc = 0.0f;
    return a + ab * v;
  }
  const Vec3 cp = p - c;
  const f32 d5 = dot(ab, cp);
  const f32 d6 = dot(ac, cp);
  if (d6 >= 0.0f && d5 <= d6) {
    wa = 0.0f;
    wb = 0.0f;
    wc = 1.0f;
    return c;
  }
  const f32 vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
    const f32 denominator = d2 - d6;
    const f32 w = denominator != 0.0f ? d2 / denominator : 0.0f;
    wa = 1.0f - w;
    wb = 0.0f;
    wc = w;
    return a + ac * w;
  }
  const f32 va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
    const f32 denominator = (d4 - d3) + (d5 - d6);
    const f32 w = denominator != 0.0f ? (d4 - d3) / denominator : 0.0f;
    wa = 0.0f;
    wb = 1.0f - w;
    wc = w;
    return b + (c - b) * w;
  }
  const f32 total = va + vb + vc;
  if (!(total > 0.0f)) {
    wa = 1.0f;
    wb = 0.0f;
    wc = 0.0f;
    return a;
  }
  const f32 inverse = 1.0f / total;
  wa = va * inverse;
  wb = vb * inverse;
  wc = vc * inverse;
  return a + ab * wb + ac * wc;
}

// A triangle of the cut, with the attributes the GPU would interpolate over it.
struct SampledTriangle {
  Vec3 p[3];
  Vec2 uv[3];
  Vec3 normal[3];
};

f32 percentile(Vector<f32>& values, f32 fraction) {
  if (values.empty()) return 0.0f;
  std::sort(values.begin(), values.end());
  const f32 position = fraction * static_cast<f32>(values.size() - 1);
  u32 index = static_cast<u32>(position);
  if (index >= values.size()) index = values.size() - 1;
  return values[index];
}

}  // namespace

bool measure_lod_attribute_error(const ClusterLodMesh& mesh, std::span<const u32> cut,
                                 const AttributeErrorOptions& options, AttributeError& out,
                                 std::string* error) {
  out = AttributeError{};
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (mesh.mesh.clusters.empty()) return fail("measure_lod_attribute_error: the mesh is empty");
  if (mesh.mesh.attributes.size() != mesh.mesh.vertices.size())
    return fail("measure_lod_attribute_error: the mesh carries no attributes");
  if (cut.empty()) return fail("measure_lod_attribute_error: the cut is empty");
  if (!(options.uv_texels > 0.0f))
    return fail("measure_lod_attribute_error: the atlas size must be positive");
  for (const u32 c : cut) {
    if (c >= mesh.mesh.clusters.size())
      return fail("measure_lod_attribute_error: a cut entry is out of range");
  }

  auto gather = [&mesh](const ClusterDesc& desc, u32 triangle, SampledTriangle& tri) {
    const u32 packed = mesh.mesh.triangles[desc.triangle_offset + triangle];
    for (u32 k = 0; k < 3; ++k) {
      const u32 at = desc.vertex_offset + ClusterMesh::unpack(packed, k);
      tri.p[k] = mesh.mesh.vertices[at];
      tri.uv[k] = decode_half2(mesh.mesh.attributes[at].uv_half2);
      tri.normal[k] = decode_normal_oct(mesh.mesh.attributes[at].normal_oct);
    }
  };

  // The coarse surface, flattened once.
  Vector<SampledTriangle> coarse;
  u32 coarse_count = 0;
  for (const u32 c : cut)
    coarse_count += mesh.mesh.clusters[c].triangle_count;
  if (coarse_count == 0) return fail("measure_lod_attribute_error: the cut has no triangles");
  coarse.reserve(coarse_count);
  for (const u32 c : cut) {
    const ClusterDesc& desc = mesh.mesh.clusters[c];
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      SampledTriangle tri;
      gather(desc, t, tri);
      coarse.push_back(tri);
    }
  }

  // A uniform grid over the coarse triangles, in CSR form so the whole thing is three arrays and
  // no allocation per cell. About one triangle a cell, capped so a large mesh does not turn the
  // index into the cost.
  Vec3 lo = coarse[0].p[0];
  Vec3 hi = lo;
  for (const SampledTriangle& tri : coarse) {
    for (u32 k = 0; k < 3; ++k) {
      lo = Vec3{std::min(lo.x, tri.p[k].x), std::min(lo.y, tri.p[k].y), std::min(lo.z, tri.p[k].z)};
      hi = Vec3{std::max(hi.x, tri.p[k].x), std::max(hi.y, tri.p[k].y), std::max(hi.z, tri.p[k].z)};
    }
  }
  const f32 extent = std::max(std::max(hi.x - lo.x, hi.y - lo.y), std::max(hi.z - lo.z, 1e-6f));
  u32 axis_cells = static_cast<u32>(std::cbrt(static_cast<f64>(coarse.size())) + 1.0);
  if (axis_cells < 1) axis_cells = 1;
  if (axis_cells > 64) axis_cells = 64;
  const f32 cell = extent / static_cast<f32>(axis_cells);
  const f32 inverse_cell = 1.0f / cell;
  const u32 cell_count = axis_cells * axis_cells * axis_cells;
  auto cell_of = [&](f32 v, f32 origin) {
    const f32 raw = (v - origin) * inverse_cell;
    const i32 index = static_cast<i32>(std::floor(raw));
    return static_cast<u32>(
        index < 0
            ? 0
            : (index >= static_cast<i32>(axis_cells) ? static_cast<i32>(axis_cells) - 1 : index));
  };
  Vector<u32> cell_start(cell_count + 1, 0u);
  auto for_each_cell = [&](const SampledTriangle& tri, auto&& visit) {
    const f32 tlo[3] = {std::min(std::min(tri.p[0].x, tri.p[1].x), tri.p[2].x),
                        std::min(std::min(tri.p[0].y, tri.p[1].y), tri.p[2].y),
                        std::min(std::min(tri.p[0].z, tri.p[1].z), tri.p[2].z)};
    const f32 thi[3] = {std::max(std::max(tri.p[0].x, tri.p[1].x), tri.p[2].x),
                        std::max(std::max(tri.p[0].y, tri.p[1].y), tri.p[2].y),
                        std::max(std::max(tri.p[0].z, tri.p[1].z), tri.p[2].z)};
    const f32 origin[3] = {lo.x, lo.y, lo.z};
    const u32 first[3] = {cell_of(tlo[0], origin[0]), cell_of(tlo[1], origin[1]),
                          cell_of(tlo[2], origin[2])};
    const u32 last[3] = {cell_of(thi[0], origin[0]), cell_of(thi[1], origin[1]),
                         cell_of(thi[2], origin[2])};
    for (u32 z = first[2]; z <= last[2]; ++z) {
      for (u32 y = first[1]; y <= last[1]; ++y) {
        for (u32 x = first[0]; x <= last[0]; ++x)
          visit((z * axis_cells + y) * axis_cells + x);
      }
    }
  };
  for (const SampledTriangle& tri : coarse)
    for_each_cell(tri, [&](u32 index) { ++cell_start[index + 1]; });
  for (u32 i = 0; i < cell_count; ++i)
    cell_start[i + 1] += cell_start[i];
  Vector<u32> cell_items(cell_start[cell_count], 0u);
  Vector<u32> fill(cell_count, 0u);
  for (u32 t = 0; t < coarse.size(); ++t) {
    for_each_cell(coarse[t], [&](u32 index) {
      cell_items[cell_start[index] + fill[index]] = t;
      ++fill[index];
    });
  }

  // The fine surface: every level-0 triangle, or a fixed stride through them.
  Vector<u32> leaves;
  for (u32 c = 0; c < mesh.mesh.clusters.size(); ++c) {
    if (mesh.lod[c].level == 0) leaves.push_back(c);
  }
  if (leaves.empty()) return fail("measure_lod_attribute_error: the mesh has no level-0 clusters");
  u32 leaf_triangles = 0;
  for (const u32 c : leaves)
    leaf_triangles += mesh.mesh.clusters[c].triangle_count;
  u32 stride = 1;
  if (options.max_samples > 0 && leaf_triangles > options.max_samples)
    stride = (leaf_triangles + options.max_samples - 1) / options.max_samples;

  Vector<f32> uv_errors;
  Vector<f32> normal_errors;
  f64 uv_total = 0.0;
  f64 normal_total = 0.0;
  u32 walked = 0;
  for (const u32 c : leaves) {
    const ClusterDesc& desc = mesh.mesh.clusters[c];
    for (u32 t = 0; t < desc.triangle_count; ++t, ++walked) {
      if (walked % stride != 0) continue;
      SampledTriangle fine;
      gather(desc, t, fine);
      const Vec3 point = (fine.p[0] + fine.p[1] + fine.p[2]) * (1.0f / 3.0f);
      const Vec2 want_uv = (fine.uv[0] + fine.uv[1] + fine.uv[2]) * (1.0f / 3.0f);
      Vec3 want_normal = fine.normal[0] + fine.normal[1] + fine.normal[2];
      want_normal = length_squared(want_normal) > 1e-20f ? normalize(want_normal) : fine.normal[0];

      // Expanding shells of cells: stop as soon as the best distance is inside the shell already
      // searched, which is what makes this exact rather than approximate.
      const u32 base[3] = {cell_of(point.x, lo.x), cell_of(point.y, lo.y), cell_of(point.z, lo.z)};
      f32 best = 3.402823466e+38f;
      u32 best_triangle = ~u32{0};
      f32 best_w[3] = {0.0f, 0.0f, 0.0f};
      for (u32 radius = 0; radius <= axis_cells; ++radius) {
        const i32 r = static_cast<i32>(radius);
        for (i32 dz = -r; dz <= r; ++dz) {
          for (i32 dy = -r; dy <= r; ++dy) {
            for (i32 dx = -r; dx <= r; ++dx) {
              // Only the shell, not the ball: the interior was searched last round.
              if (radius > 0 && std::abs(dx) != r && std::abs(dy) != r && std::abs(dz) != r)
                continue;
              const i32 x = static_cast<i32>(base[0]) + dx;
              const i32 y = static_cast<i32>(base[1]) + dy;
              const i32 z = static_cast<i32>(base[2]) + dz;
              if (x < 0 || y < 0 || z < 0 || x >= static_cast<i32>(axis_cells) ||
                  y >= static_cast<i32>(axis_cells) || z >= static_cast<i32>(axis_cells)) {
                continue;
              }
              const u32 index =
                  (static_cast<u32>(z) * axis_cells + static_cast<u32>(y)) * axis_cells +
                  static_cast<u32>(x);
              for (u32 i = cell_start[index]; i < cell_start[index + 1]; ++i) {
                const u32 candidate = cell_items[i];
                const SampledTriangle& tri = coarse[candidate];
                f32 w[3];
                const Vec3 on =
                    closest_on_triangle(point, tri.p[0], tri.p[1], tri.p[2], w[0], w[1], w[2]);
                const f32 distance = length_squared(on - point);
                if (distance < best) {
                  best = distance;
                  best_triangle = candidate;
                  best_w[0] = w[0];
                  best_w[1] = w[1];
                  best_w[2] = w[2];
                }
              }
            }
          }
        }
        // Everything within `radius` cells of the sample's own cell has been searched, so nothing
        // outside can be nearer than `radius * cell` — which is what makes the shell walk exact
        // rather than a heuristic.
        const f32 safe = static_cast<f32>(radius) * cell;
        if (best_triangle != ~u32{0} && best <= safe * safe) break;
      }
      ++out.samples;
      if (best_triangle == ~u32{0}) {
        ++out.unmatched;
        continue;
      }
      const SampledTriangle& tri = coarse[best_triangle];
      const Vec2 got_uv = tri.uv[0] * best_w[0] + tri.uv[1] * best_w[1] + tri.uv[2] * best_w[2];
      Vec3 got_normal =
          tri.normal[0] * best_w[0] + tri.normal[1] * best_w[1] + tri.normal[2] * best_w[2];
      got_normal = length_squared(got_normal) > 1e-20f ? normalize(got_normal) : tri.normal[0];
      const Vec2 delta = got_uv - want_uv;
      const f32 uv_texels = std::sqrt(delta.x * delta.x + delta.y * delta.y) * options.uv_texels;
      const f32 cosine = std::max(-1.0f, std::min(1.0f, dot(got_normal, want_normal)));
      const f32 degrees = std::acos(cosine) * 180.0f / 3.14159265358979323846f;
      uv_errors.push_back(uv_texels);
      normal_errors.push_back(degrees);
      uv_total += static_cast<f64>(uv_texels);
      normal_total += static_cast<f64>(degrees);
      if (uv_texels > options.uv_tolerance_texels) ++out.uv_outliers;
      if (degrees > options.normal_tolerance_deg) ++out.normal_outliers;
      out.uv_max_texels = std::max(out.uv_max_texels, uv_texels);
      out.normal_max_deg = std::max(out.normal_max_deg, degrees);
    }
  }
  const u32 measured = uv_errors.size();
  if (measured > 0) {
    out.uv_mean_texels = static_cast<f32>(uv_total / static_cast<f64>(measured));
    out.normal_mean_deg = static_cast<f32>(normal_total / static_cast<f64>(measured));
    out.uv_p99_texels = percentile(uv_errors, 0.99f);
    out.normal_p99_deg = percentile(normal_errors, 0.99f);
  }
  return true;
}

bool validate_cluster_lod(const ClusterLodMesh& mesh, std::span<const u32> source_indices,
                          std::string* error) {
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (mesh.mesh.clusters.size() != mesh.lod.size())
    return fail("lod table does not match clusters");
  if (mesh.level_cluster_counts.empty()) return fail("no levels");
  // The morph directory is over *every* cluster of the DAG, so it is checked here rather than on
  // the level-0 view below, whose clusters are a prefix of them.
  if (!validate_morph_stream(mesh.mesh, error)) return false;

  // Level 0 covers the source exactly once: reuse the plain validator on a level-0 view.
  ClusterMesh leaves;
  leaves.source_vertex_count = mesh.mesh.source_vertex_count;
  leaves.source_triangle_count = mesh.mesh.source_triangle_count;
  leaves.vertices = mesh.mesh.vertices;
  leaves.vertex_source = mesh.mesh.vertex_source;
  leaves.triangles = mesh.mesh.triangles;
  leaves.quantized = mesh.mesh.quantized;  // the whole mesh's grid, shared by every level
  leaves.quant_origin = mesh.mesh.quant_origin;
  leaves.quant_scale = mesh.mesh.quant_scale;
  leaves.skin = mesh.mesh.skin;  // every level's bindings, so validate_clusters checks them
  leaves.skin_joint_count = mesh.mesh.skin_joint_count;
  // Every level's ids, for the same reason: one id per source vertex is a property of the whole
  // DAG, and a coarse cluster's copy of a vertex is where a broken merge or page layout would show.
  leaves.vertex_ids = mesh.mesh.vertex_ids;
  leaves.vertex_id_source = mesh.mesh.vertex_id_source;
  for (u32 i = 0; i < mesh.mesh.clusters.size(); ++i) {
    if (mesh.lod[i].level == 0) leaves.clusters.push_back(mesh.mesh.clusters[i]);
  }
  ClusterBuildOptions limits;
  limits.max_vertices = 255;
  limits.max_triangles = 512;
  std::string leaf_error;
  if (!validate_clusters(leaves, source_indices, limits, &leaf_error)) {
    if (error != nullptr) *error = "level 0: " + leaf_error;
    return false;
  }
  if (mesh.leaf_triangle_count != mesh.mesh.source_triangle_count)
    return fail("leaf triangle count mismatch");

  f32 max_finite_error = 0.0f;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    const ClusterLodDesc& lod = mesh.lod[i];
    if (!(lod.own_error <= lod.parent_error)) return fail("own error exceeds parent error");
    if (lod.level == 0 && lod.own_error != 0.0f)
      return fail("level 0 cluster with nonzero own error");
    if (lod.group >= mesh.group_count) return fail("group index out of range");
    if (lod.level >= mesh.level_cluster_counts.size()) return fail("level out of range");
    if (lod.parent_error < k_lod_terminal_error && lod.parent_error > max_finite_error) {
      max_finite_error = lod.parent_error;
    }
  }

  // The raw cut is never empty and never grows with the threshold.
  Vector<u32> cut;
  u32 previous_triangles = ~u32{0};
  for (u32 step = 0; step <= 8; ++step) {
    cut.clear();
    const f32 threshold = max_finite_error * static_cast<f32>(step) / 8.0f * 1.001f;
    if (select_lod_raw(mesh, threshold, cut) == 0) return fail("empty cut");
    u32 triangles = 0;
    for (const u32 c : cut)
      triangles += mesh.mesh.clusters[c].triangle_count;
    if (triangles > previous_triangles) return fail("cut triangle count grew with the threshold");
    previous_triangles = triangles;
  }
  return true;
}

}  // namespace engine::geometry
