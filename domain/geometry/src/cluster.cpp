#include <core/containers/hash_set.h>
#include <domain/geometry/cluster.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <meshoptimizer.h>

namespace engine::geometry {

namespace {

u32 pack_cone_s8(const meshopt_Bounds& bounds) noexcept {
  auto byte = [](signed char v) { return u32{static_cast<u8>(v)}; };
  return byte(bounds.cone_axis_s8[0]) | (byte(bounds.cone_axis_s8[1]) << 8) |
         (byte(bounds.cone_axis_s8[2]) << 16) | (byte(bounds.cone_cutoff_s8) << 24);
}

}  // namespace

u32 encode_cone(Vec3 axis, f32 cutoff) noexcept {
  auto snorm8 = [](f32 v, bool round_up) {
    const f32 c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    const f32 scaled = c * 127.0f;
    i32 i = round_up ? static_cast<i32>(std::ceil(scaled)) : static_cast<i32>(std::lround(scaled));
    i = i < -127 ? -127 : (i > 127 ? 127 : i);
    return u32{static_cast<u8>(static_cast<i8>(i))};
  };
  return snorm8(axis.x, false) | (snorm8(axis.y, false) << 8) | (snorm8(axis.z, false) << 16) |
         (snorm8(cutoff, true) << 24);
}

NormalCone decode_cone(u32 packed) noexcept {
  auto component = [packed](u32 byte) {
    return static_cast<f32>(static_cast<i8>((packed >> (8 * byte)) & 0xffu)) / 127.0f;
  };
  NormalCone cone;
  cone.axis = Vec3{component(0), component(1), component(2)};
  cone.cutoff = component(3);
  return cone;
}

SkinBinding make_skin_binding(const u32 joints[4], const f32 weights[4]) noexcept {
  SkinBinding out;
  f32 total = 0.0f;
  for (u32 i = 0; i < 4; ++i)
    total += weights[i] > 0.0f ? weights[i] : 0.0f;
  if (!(total > 0.0f)) {
    // No influence at all: bind rigidly to the first joint rather than leaving the vertex at
    // the origin of the palette, which is what a missing WEIGHTS_0 accessor means in practice.
    out.joints[0] = static_cast<u8>(joints[0] < k_max_skin_joints ? joints[0] : 0);
    return out;
  }
  const f32 inverse_total = 1.0f / total;
  u32 sum = 0;
  u32 largest = 0;
  f32 largest_weight = -1.0f;
  for (u32 i = 0; i < 4; ++i) {
    const f32 w = weights[i] > 0.0f ? weights[i] * inverse_total : 0.0f;
    const u32 q = static_cast<u32>(w * 255.0f + 0.5f);
    out.joints[i] = static_cast<u8>(joints[i] < k_max_skin_joints ? joints[i] : 0);
    out.weights[i] = static_cast<u8>(q > 255 ? 255 : q);
    sum += out.weights[i];
    if (w > largest_weight) {
      largest_weight = w;
      largest = i;
    }
  }
  // The rounding error lands on the biggest influence, which is where it is least visible and
  // where there is always room for it: its quantized weight is at least 64 whenever four
  // influences share the vertex, so ±3 never underflows or saturates.
  const i32 correction = 255 - static_cast<i32>(sum);
  const i32 corrected = static_cast<i32>(out.weights[largest]) + correction;
  out.weights[largest] = static_cast<u8>(corrected < 0 ? 0 : (corrected > 255 ? 255 : corrected));
  return out;
}

bool cluster_backfacing(const ClusterDesc& cluster, Vec3 camera) noexcept {
  const NormalCone cone = decode_cone(cluster.cone);
  if (cone.cutoff >= 1.0f) return false;
  const Vec3 to_apex = cluster.cone_apex - camera;
  const f32 distance = length(to_apex);
  if (distance <= 1e-12f) return false;
  return dot(to_apex * (1.0f / distance), cone.axis) >= cone.cutoff;
}

bool build_clusters(std::span<const Vec3> positions, std::span<const u32> indices,
                    const ClusterBuildOptions& options, ClusterMesh& out, std::string* error,
                    const AttributeSource& attributes) {
  out = ClusterMesh{};
  if (positions.empty() || indices.empty() || indices.size() % 3 != 0) {
    if (error != nullptr) *error = "build_clusters: need vertices and a multiple of three indices";
    return false;
  }
  if (options.max_vertices == 0 || options.max_vertices > 255 || options.max_triangles == 0 ||
      options.max_triangles > 512 || options.max_triangles % 4 != 0) {
    if (error != nullptr) {
      *error =
          "build_clusters: max_vertices must be 1..255 and max_triangles a multiple of 4 up to 512";
    }
    return false;
  }
  for (const u32 index : indices) {
    if (index >= positions.size()) {
      if (error != nullptr) *error = "build_clusters: index out of range";
      return false;
    }
  }

  const usize bound =
      meshopt_buildMeshletsBound(indices.size(), options.max_vertices, options.max_triangles);
  Vector<meshopt_Meshlet> meshlets(static_cast<u32>(bound));
  Vector<unsigned int> meshlet_vertices(static_cast<u32>(bound * options.max_vertices));
  Vector<unsigned char> meshlet_triangles(static_cast<u32>(bound * options.max_triangles * 3));
  const usize count = meshopt_buildMeshlets(
      meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(), indices.data(),
      indices.size(), &positions[0].x, positions.size(), sizeof(Vec3), options.max_vertices,
      options.max_triangles, options.cone_weight);

  out.source_vertex_count = static_cast<u32>(positions.size());
  out.source_triangle_count = static_cast<u32>(indices.size() / 3);
  out.clusters.reserve(static_cast<u32>(count));
  for (usize m = 0; m < count; ++m) {
    meshopt_Meshlet& meshlet = meshlets[static_cast<u32>(m)];
    meshopt_optimizeMeshlet(&meshlet_vertices[meshlet.vertex_offset],
                            &meshlet_triangles[meshlet.triangle_offset], meshlet.triangle_count,
                            meshlet.vertex_count);
    const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
        &meshlet_vertices[meshlet.vertex_offset], &meshlet_triangles[meshlet.triangle_offset],
        meshlet.triangle_count, &positions[0].x, positions.size(), sizeof(Vec3));

    ClusterDesc desc;
    desc.vertex_offset = out.vertices.size();
    desc.triangle_offset = out.triangles.size();
    desc.vertex_count = meshlet.vertex_count;
    desc.triangle_count = meshlet.triangle_count;
    desc.center = Vec3{bounds.center[0], bounds.center[1], bounds.center[2]};
    desc.radius = bounds.radius;
    desc.cone_apex = Vec3{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]};
    desc.cone = options.normal_cones ? pack_cone_s8(bounds) : k_cone_none;
    for (u32 v = 0; v < meshlet.vertex_count; ++v) {
      const u32 source = meshlet_vertices[meshlet.vertex_offset + v];
      out.vertices.push_back(positions[source]);
      out.vertex_source.push_back(source);
    }
    for (u32 t = 0; t < meshlet.triangle_count; ++t) {
      const unsigned char* tri = &meshlet_triangles[meshlet.triangle_offset + t * 3];
      out.triangles.push_back(ClusterMesh::pack(tri[0], tri[1], tri[2]));
    }
    out.clusters.push_back(desc);
  }
  fill_cluster_attributes(out, positions, indices, attributes);
  quantize_positions(out);
  return true;
}

void quantize_positions(ClusterMesh& mesh) {
  mesh.quantized.clear();
  mesh.quant_origin = Vec3{};
  mesh.quant_scale = 1.0f;
  const u32 count = mesh.vertices.size();
  if (count == 0) return;
  Vec3 lo = mesh.vertices[0];
  Vec3 hi = lo;
  for (u32 i = 1; i < count; ++i) {
    const Vec3 p = mesh.vertices[i];
    lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const f32 extent = std::max(std::max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
  mesh.quant_origin = lo;
  mesh.quant_scale = extent > 0.0f ? extent / 65535.0f : 1.0f;
  const f32 inverse_scale = 1.0f / mesh.quant_scale;
  mesh.quantized.reserve(count * 3 + 1);
  for (u32 i = 0; i < count; ++i) {
    const Vec3 p = mesh.vertices[i];
    const f32 axis[3] = {p.x - lo.x, p.y - lo.y, p.z - lo.z};
    for (u32 k = 0; k < 3; ++k) {
      const f32 grid = std::round(axis[k] * inverse_scale);
      const f32 clamped = grid < 0.0f ? 0.0f : (grid > 65535.0f ? 65535.0f : grid);
      mesh.quantized.push_back(static_cast<u16>(static_cast<u32>(clamped)));
    }
  }
  // One pad entry so a shader may read every triple as two whole 32-bit words.
  if ((mesh.quantized.size() & 1u) != 0) mesh.quantized.push_back(0);
}

Vec3 dequantize_position(const ClusterMesh& mesh, u32 vertex) noexcept {
  const u64 first = u64{vertex} * 3;
  if (first + 3 > mesh.quantized.size()) return mesh.quant_origin;
  const u32 i = static_cast<u32>(first);
  return Vec3{mesh.quant_origin.x + static_cast<f32>(mesh.quantized[i]) * mesh.quant_scale,
              mesh.quant_origin.y + static_cast<f32>(mesh.quantized[i + 1]) * mesh.quant_scale,
              mesh.quant_origin.z + static_cast<f32>(mesh.quantized[i + 2]) * mesh.quant_scale};
}

bool validate_clusters(const ClusterMesh& mesh, std::span<const u32> source_indices,
                       const ClusterBuildOptions& options, std::string* error) {
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (source_indices.size() % 3 != 0) return fail("source index count is not a multiple of three");
  if (mesh.vertices.size() != mesh.vertex_source.size())
    return fail("vertex_source does not match vertices");

  // The 16-bit grid reproduces every float position within half a step.
  if (u64{mesh.quantized.size()} < u64{mesh.vertices.size()} * 3)
    return fail("quantized positions are missing or short");
  if ((mesh.quantized.size() & 1u) != 0)
    return fail("quantized positions are not padded to an even count");
  if (!(mesh.quant_scale > 0.0f)) return fail("quantization scale is not positive");
  const f32 quant_tolerance = mesh.quant_scale * 0.5f + 1e-6f;
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    const Vec3 p = mesh.vertices[v];
    const Vec3 q = dequantize_position(mesh, v);
    if (std::fabs(q.x - p.x) > quant_tolerance || std::fabs(q.y - p.y) > quant_tolerance ||
        std::fabs(q.z - p.z) > quant_tolerance)
      return fail("a dequantized position is more than half a grid step from the original");
  }

  // The skin binding stream: present or absent as a whole, weights that sum to exactly 255, and
  // joints inside the palette the mesh says it binds to. The fixed sum is what lets the shader
  // skip a divide, so a stream that does not hold it is a bug in whoever built it, not something
  // the GPU is asked to repair.
  if (!mesh.skin.empty()) {
    if (mesh.skin.size() != mesh.vertices.size())
      return fail("the skin binding stream is not parallel to the vertices");
    if (mesh.skin_joint_count == 0) return fail("a skinned mesh binds to no joints");
    if (mesh.skin_joint_count > k_max_skin_joints)
      return fail("a skin binds to more joints than a u8 index holds");
    for (const SkinBinding& binding : mesh.skin) {
      u32 total = 0;
      for (u32 k = 0; k < 4; ++k) {
        total += binding.weights[k];
        if (binding.weights[k] != 0 && binding.joints[k] >= mesh.skin_joint_count)
          return fail("a skin binding names a joint outside the mesh's palette");
      }
      if (total != 255) return fail("skin binding weights do not sum to 255");
    }
  } else if (mesh.skin_joint_count != 0) {
    return fail("the mesh names a joint palette but carries no skin bindings");
  }

  // Every source triangle exactly once: compare sorted canonical corner triples.
  Vector<u64> expected;
  Vector<u64> found;
  auto key_of = [](u32 a, u32 b, u32 c) {
    u32 v[3] = {a, b, c};
    std::sort(v, v + 3);
    return (u64{v[0]} << 42) | (u64{v[1]} << 21) | u64{v[2]};
  };
  for (usize i = 0; i + 2 < source_indices.size(); i += 3) {
    expected.push_back(key_of(source_indices[i], source_indices[i + 1], source_indices[i + 2]));
  }
  u32 total_triangles = 0;
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    const ClusterDesc& d = mesh.clusters[c];
    if (d.vertex_count == 0 || d.vertex_count > options.max_vertices)
      return fail("cluster vertex count out of limits");
    if (d.triangle_count == 0 || d.triangle_count > options.max_triangles)
      return fail("cluster triangle count out of limits");
    if (u64{d.vertex_offset} + d.vertex_count > mesh.vertices.size())
      return fail("cluster vertex range out of bounds");
    if (u64{d.triangle_offset} + d.triangle_count > mesh.triangles.size())
      return fail("cluster triangle range out of bounds");
    for (u32 v = 0; v < d.vertex_count; ++v) {
      const Vec3 p = mesh.vertices[d.vertex_offset + v];
      if (length(p - d.center) > d.radius * 1.001f + 1e-5f)
        return fail("vertex outside its cluster sphere");
    }
    const NormalCone cone = decode_cone(d.cone);
    if (cone.cutoff < -1.0f || cone.cutoff > 1.0f) return fail("cone cutoff out of range");
    const bool has_cone = cone.cutoff < 1.0f;
    if (has_cone && std::fabs(length(cone.axis) - 1.0f) > 0.02f)
      return fail("cone axis is not unit length");
    // Every face normal lies within the cone's half-angle: cos(half-angle) = sqrt(1 - cutoff^2),
    // with slack for the snorm8 axis and the rounded-up cutoff.
    const f32 min_cos =
        has_cone ? std::sqrt(std::max(0.0f, 1.0f - cone.cutoff * cone.cutoff)) : 0.0f;
    for (u32 t = 0; t < d.triangle_count; ++t) {
      const u32 packed = mesh.triangles[d.triangle_offset + t];
      u32 corners[3];
      for (u32 k = 0; k < 3; ++k) {
        corners[k] = ClusterMesh::unpack(packed, k);
        if (corners[k] >= d.vertex_count) return fail("local index out of range");
      }
      found.push_back(key_of(mesh.vertex_source[d.vertex_offset + corners[0]],
                             mesh.vertex_source[d.vertex_offset + corners[1]],
                             mesh.vertex_source[d.vertex_offset + corners[2]]));
      if (has_cone) {
        const Vec3 p0 = mesh.vertices[d.vertex_offset + corners[0]];
        const Vec3 p1 = mesh.vertices[d.vertex_offset + corners[1]];
        const Vec3 p2 = mesh.vertices[d.vertex_offset + corners[2]];
        const Vec3 face = cross(p1 - p0, p2 - p0);
        if (length_squared(face) <= 1e-24f) continue;  // degenerate faces have no normal
        if (dot(normalize(face), normalize(cone.axis)) < min_cos - 0.03f)
          return fail("triangle normal outside its cluster's cone");
      }
    }
    total_triangles += d.triangle_count;
  }
  if (total_triangles != mesh.source_triangle_count)
    return fail("triangle total does not match the source");
  std::sort(expected.begin(), expected.end());
  std::sort(found.begin(), found.end());
  if (expected.size() != found.size()) return fail("triangle count mismatch");
  for (u32 i = 0; i < expected.size(); ++i) {
    if (expected[i] != found[i]) return fail("a source triangle is missing or duplicated");
  }
  return true;
}

}  // namespace engine::geometry

// ---- attributes --------------------------------------------------------------------------------

namespace engine::geometry {

namespace {

u16 f32_to_f16(f32 value) noexcept {
  u32 bits = 0;
  std::memcpy(&bits, &value, 4);
  const u32 sign = (bits >> 16) & 0x8000u;
  const u32 exponent = (bits >> 23) & 0xffu;
  u32 mantissa = bits & 0x7fffffu;
  if (exponent == 0xff) return static_cast<u16>(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
  const i32 e = static_cast<i32>(exponent) - 127 + 15;
  if (e >= 31) return static_cast<u16>(sign | 0x7c00u);
  if (e <= 0) {
    if (e < -10) return static_cast<u16>(sign);
    mantissa |= 0x800000u;
    const u32 shift = static_cast<u32>(14 - e);
    u32 half = mantissa >> shift;
    const u32 remainder = mantissa & ((1u << shift) - 1);
    const u32 halfway = 1u << (shift - 1);
    if (remainder > halfway || (remainder == halfway && (half & 1u) != 0)) ++half;
    return static_cast<u16>(sign | half);
  }
  u32 half = sign | (static_cast<u32>(e) << 10) | (mantissa >> 13);
  const u32 remainder = mantissa & 0x1fffu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0)) ++half;
  return static_cast<u16>(half);
}

f32 f16_to_f32(u16 half) noexcept {
  const u32 sign = (u32{half} & 0x8000u) << 16;
  const u32 exponent = (half >> 10) & 0x1fu;
  const u32 mantissa = half & 0x3ffu;
  u32 bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // Denormal: renormalize.
      u32 m = mantissa;
      i32 e = -1;
      do {
        m <<= 1;
        ++e;
      } while ((m & 0x400u) == 0);
      bits = sign | (static_cast<u32>(113 - e) << 23) | ((m & 0x3ffu) << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  f32 value = 0.0f;
  std::memcpy(&value, &bits, 4);
  return value;
}

f32 sign_or_one(f32 v) noexcept { return v < 0.0f ? -1.0f : 1.0f; }

}  // namespace

u32 encode_normal_oct(Vec3 normal) noexcept {
  const f32 l1 = std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z);
  Vec2 p = l1 > 0.0f ? Vec2{normal.x / l1, normal.y / l1} : Vec2{0.0f, 0.0f};
  if (normal.z < 0.0f) {
    p = Vec2{(1.0f - std::fabs(p.y)) * sign_or_one(p.x),
             (1.0f - std::fabs(p.x)) * sign_or_one(p.y)};
  }
  auto snorm = [](f32 v) {
    const f32 c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    const i32 i = static_cast<i32>(std::lround(c * 32767.0f));
    return static_cast<u32>(static_cast<u16>(static_cast<i16>(i)));
  };
  return snorm(p.x) | (snorm(p.y) << 16);
}

Vec3 decode_normal_oct(u32 packed) noexcept {
  const f32 x = static_cast<f32>(static_cast<i16>(packed & 0xffffu)) / 32767.0f;
  const f32 y = static_cast<f32>(static_cast<i16>(packed >> 16)) / 32767.0f;
  Vec3 n{x, y, 1.0f - std::fabs(x) - std::fabs(y)};
  if (n.z < 0.0f) {
    n = Vec3{(1.0f - std::fabs(y)) * sign_or_one(x), (1.0f - std::fabs(x)) * sign_or_one(y), n.z};
  }
  return normalize(n);
}

u32 encode_half2(Vec2 v) noexcept { return u32{f32_to_f16(v.x)} | (u32{f32_to_f16(v.y)} << 16); }

Vec2 decode_half2(u32 packed) noexcept {
  return Vec2{f16_to_f32(static_cast<u16>(packed & 0xffffu)),
              f16_to_f32(static_cast<u16>(packed >> 16))};
}

void compute_vertex_normals(std::span<const Vec3> positions, std::span<const u32> indices,
                            Vector<Vec3>& out) {
  out.clear();
  out.resize(static_cast<u32>(positions.size()));
  for (Vec3& n : out)
    n = Vec3{};
  for (usize i = 0; i + 2 < indices.size(); i += 3) {
    const u32 a = indices[i];
    const u32 b = indices[i + 1];
    const u32 c = indices[i + 2];
    if (a >= positions.size() || b >= positions.size() || c >= positions.size()) continue;
    const Vec3 face =
        cross(positions[b] - positions[a], positions[c] - positions[a]);  // area-weighted
    out[a] = out[a] + face;
    out[b] = out[b] + face;
    out[c] = out[c] + face;
  }
  for (Vec3& n : out)
    n = length_squared(n) > 1e-20f ? normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
}

u32 weld_vertices(Vector<Vec3>& positions, Vector<Vec3>& normals, Vector<Vec2>& uvs,
                  std::span<u32> indices, Vector<SkinBinding>* skin) {
  const u32 vertex_count = positions.size();
  if (vertex_count == 0) return 0;
  const bool have_normals = normals.size() == vertex_count;
  const bool have_uvs = uvs.size() == vertex_count;
  const bool have_skin = skin != nullptr && skin->size() == vertex_count;
  meshopt_Stream streams[4];
  usize stream_count = 0;
  streams[stream_count++] = meshopt_Stream{positions.data(), sizeof(Vec3), sizeof(Vec3)};
  if (have_normals)
    streams[stream_count++] = meshopt_Stream{normals.data(), sizeof(Vec3), sizeof(Vec3)};
  if (have_uvs) streams[stream_count++] = meshopt_Stream{uvs.data(), sizeof(Vec2), sizeof(Vec2)};
  // The binding is part of the key: two duplicates at one position with different weights are
  // different vertices, because a weld that merged them would give one of the two surfaces the
  // other's deformation.
  if (have_skin)
    streams[stream_count++] =
        meshopt_Stream{skin->data(), sizeof(SkinBinding), sizeof(SkinBinding)};

  Vector<unsigned int> remap(vertex_count);
  const u32 unique = static_cast<u32>(meshopt_generateVertexRemapMulti(
      remap.data(), indices.empty() ? nullptr : indices.data(),
      indices.empty() ? vertex_count : indices.size(), vertex_count, streams, stream_count));
  Vector<Vec3> welded_positions(unique);
  meshopt_remapVertexBuffer(welded_positions.data(), positions.data(), vertex_count, sizeof(Vec3),
                            remap.data());
  positions = std::move(welded_positions);
  if (have_normals) {
    Vector<Vec3> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), normals.data(), vertex_count, sizeof(Vec3),
                              remap.data());
    normals = std::move(welded);
  }
  if (have_uvs) {
    Vector<Vec2> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), uvs.data(), vertex_count, sizeof(Vec2), remap.data());
    uvs = std::move(welded);
  }
  if (have_skin) {
    Vector<SkinBinding> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), skin->data(), vertex_count, sizeof(SkinBinding),
                              remap.data());
    *skin = std::move(welded);
  }
  if (!indices.empty()) {
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
  }
  return unique;
}

void fill_cluster_attributes(ClusterMesh& mesh, std::span<const Vec3> positions,
                             std::span<const u32> indices, const AttributeSource& attributes) {
  Vector<Vec3> computed;
  std::span<const Vec3> normals = attributes.normals;
  if (normals.size() != positions.size()) {
    compute_vertex_normals(positions, indices, computed);
    normals = std::span<const Vec3>(computed.data(), computed.size());
  }
  const bool have_uvs = attributes.uvs.size() == positions.size();
  mesh.attributes.clear();
  mesh.attributes.reserve(mesh.vertex_source.size());
  for (const u32 source : mesh.vertex_source) {
    VertexAttributes a;
    a.normal_oct =
        encode_normal_oct(source < normals.size() ? normals[source] : Vec3{0.0f, 1.0f, 0.0f});
    a.uv_half2 = encode_half2(have_uvs ? attributes.uvs[source] : Vec2{0.0f, 0.0f});
    mesh.attributes.push_back(a);
  }

  // The skin binding travels the same road as the normals and the UVs: through `vertex_source`,
  // which is what gives every LOD level its bindings, since clusterlod keeps original vertices.
  // That is also why skinning is crack-free — every copy of a source vertex, in every cluster
  // and on every level, gets bitwise the same four joints and four weights.
  mesh.skin.clear();
  mesh.skin_joint_count = 0;
  if (attributes.skin.size() != positions.size()) return;
  mesh.skin_joint_count = attributes.joint_count;
  mesh.skin.reserve(mesh.vertex_source.size());
  for (const u32 source : mesh.vertex_source)
    mesh.skin.push_back(attributes.skin[source]);
}

}  // namespace engine::geometry
