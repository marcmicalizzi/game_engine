// What G2 reads beyond the metallic-roughness core (docs/subsystems/assets.md, "Tangents, a second
// UV set and vertex colours", "Material extensions", "Lights and cameras", "The influences"):
// small glTF files written here, as data URIs, so every number the assertions name is in this file.
#include <core/log/log.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::assets;

namespace {

std::string base64(const std::vector<u8>& bytes) {
  static const char k_alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (usize i = 0; i < bytes.size(); i += 3) {
    const u32 b0 = bytes[i];
    const u32 b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0u;
    const u32 b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0u;
    const u32 triple = (b0 << 16) | (b1 << 8) | b2;
    out += k_alphabet[(triple >> 18) & 63];
    out += k_alphabet[(triple >> 12) & 63];
    out += i + 1 < bytes.size() ? k_alphabet[(triple >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? k_alphabet[triple & 63] : '=';
  }
  return out;
}

template <class T>
void append(std::vector<u8>& out, const T* values, usize count) {
  const usize at = out.size();
  out.resize(at + sizeof(T) * count);
  std::memcpy(out.data() + at, values, sizeof(T) * count);
}

std::string buffer_json(const std::vector<u8>& buffer) {
  return "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + base64(buffer) +
         "\",\"byteLength\":" + std::to_string(buffer.size()) + "}]";
}

bool load(const std::string& json, MeshData& mesh, std::string& error) {
  return load_gltf_memory(
      std::span<const u8>(reinterpret_cast<const u8*>(json.data()), json.size()), "", mesh, &error);
}

bool near(f32 a, f32 b, f32 eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// A quad drawn twice: primitive 0 with NORMAL, TEXCOORD_0, TANGENT, TEXCOORD_1 and COLOR_0, under a
// node that mirrors x (scale -2) and moves it; primitive 1 the same quad with none of the three.
// `extras` false writes the same file without the three attributes on primitive 0.
std::string quad_with_extras(bool extras) {
  const f32 positions[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  const f32 normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 uvs[8] = {0, 1, 1, 1, 1, 0, 0, 0};
  // v3's tangent has no length: it imports as "none".
  const f32 tangents[16] = {1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, -1, 0, 0, 0, 1};
  const f32 uvs1[8] = {0.25f, 0.5f, 0.75f, 0.5f, 0.75f, 0.0f, 0.25f, 0.0f};
  const f32 colors[16] = {1, 0, 0, 1, 0, 1, 0, 0.5f, 0, 0, 1, 1, 1, 1, 1, 1};
  const u16 indices[6] = {0, 1, 2, 0, 2, 3};
  std::vector<u8> buffer;
  append(buffer, positions, 12);  // 0
  append(buffer, normals, 12);    // 48
  append(buffer, uvs, 8);         // 96
  append(buffer, tangents, 16);   // 128
  append(buffer, uvs1, 8);        // 192
  append(buffer, colors, 16);     // 224
  append(buffer, indices, 6);     // 288
  buffer.resize(304, 0);
  std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],";
  json += "\"nodes\":[{\"mesh\":0,\"scale\":[-2,1,1],\"translation\":[5,0,0]}],";
  json +=
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2";
  if (extras) json += ",\"TANGENT\":3,\"TEXCOORD_1\":4,\"COLOR_0\":5";
  json += "},\"indices\":6},";
  json += "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":6}]}],";
  json +=
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[0,0,0],"
      "\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":5,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},"
      "{\"bufferView\":6,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],";
  json +=
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":64},"
      "{\"buffer\":0,\"byteOffset\":192,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":224,\"byteLength\":64},"
      "{\"buffer\":0,\"byteOffset\":288,\"byteLength\":12}],";
  json += buffer_json(buffer) + "}";
  return json;
}

geometry::ClusterFileData container_of(const MeshData& mesh) {
  geometry::ClusterFileData data;
  std::string error;
  const bool built =
      geometry::build_cluster_lod(mesh.positions, mesh.indices, geometry::ClusterLodOptions{},
                                  data.mesh, &error, attribute_source(mesh));
  REQUIRE_MESSAGE(built, error);
  data.materials.push_back(geometry::ClusterFileMaterial{});
  data.cluster_material.assign(data.mesh.mesh.clusters.size(), 0u);
  write_import_sections(mesh, data);
  return data;
}

}  // namespace

TEST_CASE("gltf G2: tangents, a second UV set and colours, through a mirroring node") {
  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load(quad_with_extras(true), mesh, error), error);
  REQUIRE(mesh.positions.size() == 8);
  REQUIRE(mesh.tangents.size() == 8);
  REQUIRE(mesh.uvs1.size() == 8);
  REQUIRE(mesh.colors.size() == 8);
  // A tangent goes through the linear part (x scaled by -2, then normalized), and the mirror turns
  // the handedness over: (1,0,0,+1) is (-1,0,0,-1), (0,1,0,-1) is (0,1,0,+1).
  CHECK(near(mesh.tangents[0].x, -1.0f));
  CHECK(near(mesh.tangents[0].y, 0.0f));
  CHECK(mesh.tangents[0].w == -1.0f);
  CHECK(near(mesh.tangents[2].y, 1.0f));
  CHECK(mesh.tangents[2].w == 1.0f);
  // No length: none, which is w = 0.
  CHECK(mesh.tangents[3].w == 0.0f);
  CHECK(mesh.uvs1[1].x == 0.75f);
  CHECK(mesh.uvs1[1].y == 0.5f);
  CHECK(mesh.colors[1].y == 1.0f);
  CHECK(mesh.colors[1].w == 0.5f);
  // The second primitive carries none of them: the "none" values, so the streams stay parallel.
  for (u32 v = 4; v < 8; ++v) {
    CHECK(mesh.tangents[v].w == 0.0f);
    CHECK(mesh.uvs1[v].x == 0.0f);
    CHECK(mesh.colors[v].x == 1.0f);
    CHECK(mesh.colors[v].w == 1.0f);
  }

  // Into a container: one record per cluster vertex, each its source vertex's, and back unchanged.
  geometry::ClusterFileData data = container_of(mesh);
  const geometry::ClusterMesh& built = data.mesh.mesh;
  REQUIRE(built.extra.size() == built.vertices.size());
  for (u32 v = 0; v < built.vertices.size(); ++v) {
    const u32 source = built.vertex_source[v];
    CHECK(built.extra[v].tangent == geometry::encode_tangent(mesh.tangents[source]));
    CHECK(built.extra[v].uv1 == geometry::encode_half2(mesh.uvs1[source]));
    CHECK(built.extra[v].color == geometry::encode_color_rgba8(mesh.colors[source]));
  }
  const test::TempDir tmp("assets_g2_extras");
  const std::string path = tmp.file("quad.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, data, &error), error);
  geometry::ClusterFileData read;
  REQUIRE_MESSAGE(geometry::read_cluster_file(path, read, &error), error);
  REQUIRE(read.mesh.mesh.extra.size() == built.extra.size());
  for (u32 v = 0; v < built.extra.size(); ++v) {
    CHECK(read.mesh.mesh.extra[v].tangent == built.extra[v].tangent);
    CHECK(read.mesh.mesh.extra[v].uv1 == built.extra[v].uv1);
    CHECK(read.mesh.mesh.extra[v].color == built.extra[v].color);
  }
  const Vec4 back = geometry::decode_tangent(read.mesh.mesh.extra[0].tangent);
  CHECK(back.w != 0.0f);
}

TEST_CASE("gltf G2: a file without the new attributes builds the container it built before") {
  MeshData with;
  MeshData without;
  std::string error;
  REQUIRE_MESSAGE(load(quad_with_extras(true), with, error), error);
  REQUIRE_MESSAGE(load(quad_with_extras(false), without, error), error);
  CHECK(without.tangents.empty());
  CHECK(without.uvs1.empty());
  CHECK(without.colors.empty());
  CHECK(without.lights.empty());
  CHECK(without.cameras.empty());
  const geometry::ClusterFileData plain = container_of(without);
  CHECK(plain.mesh.mesh.extra.empty());
  CHECK(plain.material_uv1.empty());
  // The same mesh with the three streams taken away builds exactly the plain one's bytes: the
  // streams add a section and change nothing else.
  with.tangents.clear();
  with.uvs1.clear();
  with.colors.clear();
  CHECK(geometry::cluster_file_hash(container_of(with)) == geometry::cluster_file_hash(plain));
}

TEST_CASE("gltf G2: a mirrored seam's two sides stay two vertices in the weld") {
  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load(quad_with_extras(true), mesh, error), error);
  // Make primitive 1 a copy of primitive 0 in everything but v0's handedness: the weld merges the
  // three identical vertices and keeps v0 and its mirrored twin apart.
  for (u32 v = 0; v < 4; ++v) {
    mesh.tangents[4 + v] = mesh.tangents[v];
    mesh.uvs1[4 + v] = mesh.uvs1[v];
    mesh.colors[4 + v] = mesh.colors[v];
  }
  mesh.tangents[4].w = -mesh.tangents[4].w;
  CHECK(weld_vertices(mesh) == 5);
  CHECK(mesh.tangents.size() == 5);
  CHECK(mesh.uvs1.size() == 5);
  CHECK(mesh.colors.size() == 5);
}

TEST_CASE("gltf G2: tangent and colour packing") {
  // +z right-handed is the one unit tangent whose octahedral word is zero, "none": it is written
  // two steps off zero and still decodes to +z.
  const u32 z = geometry::encode_tangent(Vec4{0.0f, 0.0f, 1.0f, 1.0f});
  CHECK(z != 0u);
  const Vec4 back = geometry::decode_tangent(z);
  CHECK(near(back.z, 1.0f, 1e-4f));
  CHECK(back.w == 1.0f);
  CHECK(geometry::encode_tangent(Vec4{0.0f, 0.0f, 0.0f, 1.0f}) == 0u);
  CHECK(geometry::decode_tangent(0u).w == 0.0f);
  const Vec4 left =
      geometry::decode_tangent(geometry::encode_tangent(Vec4{0.6f, 0.0f, 0.8f, -1.0f}));
  CHECK(near(left.x, 0.6f, 1e-4f));
  CHECK(near(left.z, 0.8f, 1e-4f));
  CHECK(left.w == -1.0f);
  CHECK(geometry::encode_color_rgba8(Vec4{1.0f, 1.0f, 1.0f, 1.0f}) ==
        geometry::k_vertex_color_white);
  CHECK(geometry::encode_color_rgba8(Vec4{1.0f, 0.0f, 0.5f, 2.0f}) == 0xff80'00ffu);
}

// One triangle, a spot light on a node turned a quarter about x, a camera on a scaled node, and a
// material stating every extension G2 reads.
std::string lit_triangle() {
  const f32 positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::vector<u8> buffer;
  append(buffer, positions, 9);
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_lights_punctual\","
      "\"KHR_materials_emissive_strength\",\"KHR_materials_ior\",\"KHR_materials_clearcoat\","
      "\"KHR_materials_transmission\",\"KHR_materials_volume\",\"KHR_materials_specular\","
      "\"KHR_materials_sheen\"],";
  json +=
      "\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[{\"type\":\"spot\",\"name\":\"lamp\","
      "\"color\":[1,0.5,0.25],\"intensity\":40,\"range\":10,\"spot\":{\"innerConeAngle\":0.2,"
      "\"outerConeAngle\":0.6}}]}},";
  json += "\"scene\":0,\"scenes\":[{\"nodes\":[0,1,2]}],";
  json += "\"nodes\":[{\"mesh\":0},";
  json +=
      "{\"translation\":[1,2,3],\"rotation\":[0.70710678,0,0,0.70710678],"
      "\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}}},";
  json += "{\"camera\":0,\"translation\":[0,1,5],\"scale\":[2,2,2]}],";
  json +=
      "\"cameras\":[{\"type\":\"perspective\",\"name\":\"view\",\"perspective\":{\"yfov\":0.8,"
      "\"znear\":0.1}}],";
  json += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],";
  json +=
      "\"materials\":[{\"emissiveFactor\":[1,0.5,0],\"extensions\":{"
      "\"KHR_materials_emissive_strength\":{\"emissiveStrength\":4},"
      "\"KHR_materials_ior\":{\"ior\":1.4},"
      "\"KHR_materials_clearcoat\":{\"clearcoatFactor\":0.5,\"clearcoatRoughnessFactor\":0.1},"
      "\"KHR_materials_transmission\":{\"transmissionFactor\":0.9},"
      "\"KHR_materials_volume\":{\"thicknessFactor\":0.02},"
      "\"KHR_materials_specular\":{\"specularFactor\":0.7},"
      "\"KHR_materials_sheen\":{\"sheenColorFactor\":[1,1,1]}}}],";
  json +=
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
      "\"min\":[0,0,0],\"max\":[1,1,0]}],";
  json += "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],";
  json += buffer_json(buffer) + "}";
  return json;
}

TEST_CASE("gltf G2: lights and cameras at their nodes, and the material extensions") {
  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load(lit_triangle(), mesh, error), error);
  REQUIRE(mesh.lights.size() == 1);
  const geometry::ClusterFileLight& light = mesh.lights[0];
  CHECK(light.type == geometry::k_light_spot);
  CHECK(light.position == Vec3(1.0f, 2.0f, 3.0f));
  // -z turned a quarter about x points up.
  CHECK(near(light.direction.x, 0.0f));
  CHECK(near(light.direction.y, 1.0f));
  CHECK(near(light.direction.z, 0.0f));
  CHECK(light.intensity == 40.0f);
  CHECK(light.range == 10.0f);
  CHECK(light.inner_cone == 0.2f);
  CHECK(light.outer_cone == 0.6f);
  CHECK(light.color == Vec3(1.0f, 0.5f, 0.25f));
  CHECK(std::string(light.name) == "lamp");
  REQUIRE(mesh.cameras.size() == 1);
  const geometry::ClusterFileCamera& camera = mesh.cameras[0];
  CHECK(camera.type == geometry::k_camera_perspective);
  CHECK(camera.position == Vec3(0.0f, 1.0f, 5.0f));
  // The node's scale is not the camera's rotation.
  CHECK(near(camera.rotation.w, 1.0f));
  CHECK(camera.yfov_or_xmag == 0.8f);
  CHECK(camera.aspect_or_ymag == 0.0f);  // left to the viewport
  CHECK(camera.znear == 0.1f);
  CHECK(camera.zfar == 0.0f);  // infinite
  CHECK(std::string(camera.name) == "view");

  REQUIRE(mesh.materials.size() == 1);
  const Material& m = mesh.materials[0];
  // Emissive strength is drawn: folded into the factor.
  CHECK(m.emissive == Vec3(4.0f, 2.0f, 0.0f));
  CHECK(m.emissive_strength == 4.0f);
  // The rest are read, recorded and not drawn.
  CHECK((m.extensions & k_ext_emissive_strength) != 0);
  CHECK((m.extensions & k_ext_not_drawn) == k_ext_not_drawn);
  CHECK(m.ior == 1.4f);
  CHECK(m.clearcoat == 0.5f);
  CHECK(m.clearcoat_roughness == 0.1f);
  CHECK(m.transmission_factor == 0.9f);
  CHECK(m.thickness == 0.02f);
  CHECK(m.specular == 0.7f);
  CHECK(cluster_file_material(m).emissive == Vec3(4.0f, 2.0f, 0.0f));

  // Sections 51 and 52 round-trip.
  geometry::ClusterFileData data = container_of(mesh);
  const test::TempDir tmp("assets_g2_lights");
  const std::string path = tmp.file("lit.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, data, &error), error);
  geometry::ClusterFileData read;
  REQUIRE_MESSAGE(geometry::read_cluster_file(path, read, &error), error);
  REQUIRE(read.lights.size() == 1);
  REQUIRE(read.cameras.size() == 1);
  CHECK(std::memcmp(&read.lights[0], &light, sizeof(light)) == 0);
  CHECK(std::memcmp(&read.cameras[0], &camera, sizeof(camera)) == 0);
}

// A triangle skinned to eight joints: vertex 0 names all eight through JOINTS_0/1 and WEIGHTS_0/1,
// the other two four or fewer.
std::string eight_influences() {
  const f32 positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const u8 joints0[12] = {0, 1, 2, 3, 0, 0, 0, 0, 0, 1, 0, 0};
  const u8 joints1[12] = {4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0};
  const f32 weights0[12] = {0.30f, 0.20f, 0.10f, 0.05f, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0};
  const f32 weights1[12] = {0.15f, 0.10f, 0.05f, 0.05f, 0, 0, 0, 0, 0, 0, 0, 0};
  std::vector<u8> buffer;
  append(buffer, positions, 9);  // 0
  append(buffer, joints0, 12);   // 36
  append(buffer, joints1, 12);   // 48
  append(buffer, weights0, 12);  // 60
  append(buffer, weights1, 12);  // 108
  std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0";
  for (int j = 1; j <= 8; ++j)
    json += "," + std::to_string(j);
  json += "]}],\"nodes\":[{\"mesh\":0,\"skin\":0}";
  for (int j = 0; j < 8; ++j)
    json += ",{\"name\":\"j" + std::to_string(j) + "\"}";
  json += "],\"skins\":[{\"joints\":[1,2,3,4,5,6,7,8]}],";
  json +=
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,"
      "\"JOINTS_1\":2,\"WEIGHTS_0\":3,\"WEIGHTS_1\":4}}]}],";
  json +=
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],"
      "\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
      "{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}],";
  json +=
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":12},"
      "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":108,\"byteLength\":48}],";
  json += buffer_json(buffer) + "}";
  return json;
}

TEST_CASE(
    "gltf G2: eight influences keep the heaviest four, and the error against eight is bounded") {
  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load(eight_influences(), mesh, error), error);
  REQUIRE(mesh.skin_bindings.size() == 3);
  const geometry::SkinBinding& b = mesh.skin_bindings[0];
  // 0.30 (j0), 0.20 (j1), 0.15 (j4), then 0.10 twice (j2 and j5): the tie goes to the lower joint.
  CHECK(b.joints[0] == 0);
  CHECK(b.joints[1] == 1);
  CHECK(b.joints[2] == 4);
  CHECK(b.joints[3] == 2);
  CHECK(u32{b.weights[0]} + b.weights[1] + b.weights[2] + b.weights[3] == 255u);
  CHECK(mesh.influences_over_four == 1);
  CHECK(near(mesh.influence_dropped_max, 0.25f, 1e-5f));
  // The others had four or fewer: unchanged, the order of the file kept.
  CHECK(mesh.skin_bindings[1].weights[0] == 255);
  CHECK(mesh.skin_bindings[2].joints[1] == 1);

  // The error against skinning with all eight, on a pose that moves joint k by k along x: eight
  // give sum w_k k = 2.3, the four kept give (0.3*0 + 0.2*1 + 0.15*4 + 0.1*2) / 0.75 = 1.333. The
  // bound is the dropped share times the spread of the joints' displacements, 0.25 * 7.
  const f32 all =
      0.30f * 0 + 0.20f * 1 + 0.10f * 2 + 0.05f * 3 + 0.15f * 4 + 0.10f * 5 + 0.05f * 6 + 0.05f * 7;
  f32 kept = 0.0f;
  for (u32 k = 0; k < 4; ++k)
    kept += static_cast<f32>(b.weights[k]) / 255.0f * static_cast<f32>(b.joints[k]);
  const f32 moved = std::fabs(all - kept);
  MESSAGE("four-of-eight skinning moves vertex 0 by " << moved << " of a bound "
                                                      << mesh.influence_dropped_max * 7.0f);
  CHECK(moved <= mesh.influence_dropped_max * 7.0f);
}

// **Where an asset's own tangent frame and the one the resolve derives differ** (assets.md, "What
// G2 reads"), on the Khronos samples tools/fetch-samples.ps1 fetches, when the checkout has them
// (it says so and passes when it has not). The derived frame is `tangent_frame` in material.slang,
// per triangle from its positions and UVs, Gram-Schmidt against the vertex normal; the asset's is
// the vertex's own. A corner counts as differing when the two tangents are more than 10 degrees
// apart in the surface, as mirrored when the bitangents point opposite ways, and as unframed when
// the triangle's UVs span no area (the resolve leaves the normal map off there) but the asset has
// one.
TEST_CASE("gltf G2: the Khronos samples' tangents against the per-triangle frame") {
  const char* k_samples[] = {
      "Lantern/Lantern.glb", "BoomBox/BoomBox.glb",          "Corset/Corset.glb",
      "Avocado/Avocado.glb", "SciFiHelmet/SciFiHelmet.gltf", "FlightHelmet/FlightHelmet.gltf",
      "Suzanne/Suzanne.gltf"};
  for (const char* name : k_samples) {
    const std::string relative(name);
    const std::string path =
        test::data_path(std::string(ENGINE_SOURCE_DIR "/content/samples/") + relative,
                        std::string("content/samples/") + relative);
    if (!test::path_exists(path)) {
      MESSAGE("sample not fetched, skipped: " << relative);
      continue;
    }
    MeshData mesh;
    std::string error;
    REQUIRE_MESSAGE(load_gltf(path, mesh, &error), error);
    if (mesh.tangents.empty()) {
      MESSAGE(relative << ": no TANGENT; the resolve's frame is the only one");
      continue;
    }
    REQUIRE(mesh.tangents.size() == mesh.positions.size());
    REQUIRE(mesh.normals.size() == mesh.positions.size());
    u32 corners = 0, apart = 0, mirrored = 0, unframed = 0;
    // The corners split by the sign of their triangle's UV area: [the file's bitangent along +dP/dv,
    // against it][area > 0, area < 0] (R202; docs/experiments/tangent-space-convention.md, "The
    // author's frame against the derived one").
    u32 by_area[2][2] = {};
    for (u32 t = 0; t + 2 < mesh.indices.size(); t += 3) {
      const u32 c[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
      const Vec3 e1 = mesh.positions[c[1]] - mesh.positions[c[0]];
      const Vec3 e2 = mesh.positions[c[2]] - mesh.positions[c[0]];
      const Vec2 d1 = mesh.uvs[c[1]] - mesh.uvs[c[0]];
      const Vec2 d2 = mesh.uvs[c[2]] - mesh.uvs[c[0]];
      const f32 area = d1.x * d2.y - d2.x * d1.y;
      for (const u32 v : c) {
        const Vec4 asset = mesh.tangents[v];
        if (asset.w == 0.0f) continue;
        ++corners;
        if (std::fabs(area) < 1e-12f) {
          ++unframed;
          continue;
        }
        const Vec3 n = mesh.normals[v];
        Vec3 tangent = (e1 * d2.y - e2 * d1.y) * (1.0f / area);
        Vec3 bitangent = (e2 * d1.x - e1 * d2.x) * (1.0f / area);
        tangent = tangent - n * dot(n, tangent);
        bitangent = bitangent - n * dot(n, bitangent);
        if (!(length_squared(tangent) > 1e-20f)) {
          ++unframed;
          continue;
        }
        tangent = normalize(tangent);
        Vec3 own = Vec3{asset.x, asset.y, asset.z};
        own = own - n * dot(n, own);
        if (!(length_squared(own) > 1e-20f)) continue;
        own = normalize(own);
        if (dot(own, tangent) < std::cos(10.0f * 3.14159265f / 180.0f)) ++apart;
        const bool against = dot(cross(n, own) * asset.w, bitangent) < 0.0f;
        if (against) ++mirrored;
        ++by_area[against ? 1 : 0][area > 0.0f ? 0 : 1];
      }
    }
    MESSAGE(relative << ": the file's bitangent along +dP/dv on " << by_area[0][0]
                     << " corners of positive UV area and " << by_area[0][1]
                     << " of negative; against it on " << by_area[1][0] << " and " << by_area[1][1]);
    MESSAGE(relative << ": " << corners << " corners with a tangent; " << apart
                     << " more than 10 degrees from the per-triangle frame ("
                     << (corners ? 100.0 * apart / corners : 0.0) << " %), " << mirrored
                     << " with the bitangent the other way, " << unframed
                     << " on triangles with no UV area");
    CHECK(corners > 0);
  }
}

TEST_CASE("gltf G2: a Draco-compressed primitive is refused by name") {
  const f32 positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::vector<u8> buffer;
  append(buffer, positions, 9);
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_draco_mesh_compression\"],"
      "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"extensions\":{"
      "\"KHR_draco_mesh_compression\":{\"bufferView\":0,\"attributes\":{\"POSITION\":0}}}}]}],"
      "\"accessors\":[{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],"
      "\"max\":[1,1,0]}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],";
  json += buffer_json(buffer) + "}";
  MeshData mesh;
  std::string error;
  CHECK_FALSE(load(json, mesh, error));
  CHECK(error.find("Draco") != std::string::npos);
  CHECK(mesh.positions.empty());
}

namespace {

// The import's lines about an `extras.sampling` it ignored (T150), counted by message.
struct SamplingLines final : log::Sink {
  u32 unknown = 0;
  u32 foreign = 0;
  std::string last_value;
  std::string last_material;
  void write(const log::Record& record) override {
    const bool is_unknown =
        record.message.find("neither stochastic nor tiled") != std::string_view::npos;
    const bool is_foreign =
        record.message.find("extras.sampling is not a string") != std::string_view::npos;
    if (!is_unknown && !is_foreign) return;
    (is_unknown ? unknown : foreign) += 1;
    for (const log::Field& field : record.fields) {
      const std::string text =
          field.kind == log::Field::Kind::String ? std::string(field.string()) : std::string();
      if (field.key == "value") last_value = text;
      if (field.key == "material") last_material = text;
    }
  }
};

}  // namespace

// T150 (assets.md, "Stochastic sampling an imported material asks for"): a material's
// `extras.sampling` in a pack role's words. Absent and "tiled" read as tiled, "stochastic" as
// stochastic and into its container record's flags, in any case; any other value is ignored with a
// line and the file loads tiled. Extras of other keys leave the material as it was.
TEST_CASE("gltf T150: a material's extras.sampling asks for stochastic sampling") {
  const auto with_materials = [](const std::string& materials) {
    std::string json = quad_with_extras(false);
    const std::string at = "\"scene\":0,";
    json.insert(json.find(at) + at.size(), "\"materials\":[" + materials + "],");
    return json;
  };
  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(
      load(with_materials("{\"name\":\"plain\"},"
                          "{\"name\":\"tiled\",\"extras\":{\"sampling\":\"tiled\"}},"
                          "{\"name\":\"wall\",\"extras\":{\"sampling\":\"stochastic\"}},"
                          "{\"name\":\"other\",\"extras\":{\"note\":\"stochastic\"}}"),
           mesh, error),
      error);
  REQUIRE(mesh.materials.size() == 4);
  CHECK_FALSE(mesh.materials[0].stochastic);
  CHECK_FALSE(mesh.materials[1].stochastic);
  CHECK(mesh.materials[2].stochastic);
  CHECK_FALSE(mesh.materials[3].stochastic);
  CHECK(cluster_file_material_sampling(mesh.materials[0]).flags == 0u);
  CHECK(cluster_file_material_sampling(mesh.materials[1]).flags == 0u);
  CHECK(cluster_file_material_sampling(mesh.materials[2]).flags ==
        geometry::k_material_sampling_stochastic);
  // The rest of the record is the plain material's, byte for byte.
  geometry::ClusterFileMaterialSampling asked = cluster_file_material_sampling(mesh.materials[2]);
  asked.flags = 0;
  const geometry::ClusterFileMaterialSampling plain =
      cluster_file_material_sampling(mesh.materials[0]);
  CHECK(std::memcmp(&asked, &plain, sizeof(plain)) == 0);
  // The cache key folds a tag in for a file that asks (an entry built from its bytes before the
  // reading is rebuilt), and only then: the plain and the tiled file key as their bytes.
  const test::TempDir tmp("assets_t150_sampling");
  REQUIRE(tmp.ok());
  const auto key_is_bytes = [&](const char* stem, const std::string& materials) {
    const std::string path = tmp.file(std::string(stem) + ".gltf");
    const std::string json = with_materials(materials);
    {
      std::ofstream f(path, std::ios::binary);
      f.write(json.data(), static_cast<std::streamsize>(json.size()));
      REQUIRE(f.good());
    }
    u64 key = 0;
    u64 bytes = 0;
    std::string why;
    REQUIRE_MESSAGE(source_mesh_hash(path, key, bytes, &why), why);
    return key == bytes;
  };
  CHECK(key_is_bytes("plain", "{}"));
  CHECK(key_is_bytes("tiled", "{\"extras\":{\"sampling\":\"tiled\"}}"));
  CHECK_FALSE(key_is_bytes("asked", "{\"extras\":{\"sampling\":\"stochastic\"}}"));
  CHECK_FALSE(key_is_bytes("cased", "{\"extras\":{\"sampling\":\"Stochastic\"}}"));

  // **Nothing in `extras` refuses the file** (the coordinator's ruling on T150's review, finding
  // 2): `extras` is every tool's namespace. The scene enum's spelling `Stochastic`, in any case,
  // asks; a string the engine does not know draws tiled with one warning; a value that is not a
  // string (a Blender custom property of that name) is another tool's and draws tiled with one
  // line at info. Each line is counted here, by message, through a sink of the test's own.
  SamplingLines lines;
  log::add_sink(&lines);
  struct Case {
    const char* materials;
    bool stochastic;
    u32 unknown;
    u32 foreign;
    const char* value;
  };
  const Case cases[] = {
      {"{\"name\":\"w\",\"extras\":{\"sampling\":\"Stochastic\"}}", true, 0, 0, ""},
      {"{\"name\":\"w\",\"extras\":{\"sampling\":\"TILED\"}}", false, 0, 0, ""},
      {"{\"name\":\"w\",\"extras\":{\"sampling\":\"hex\"}}", false, 1, 0, "hex"},
      {"{\"name\":\"w\",\"extras\":{\"sampling\":1}}", false, 0, 1, "1"},
      {"{\"name\":\"w\",\"extras\":{\"sampling\":null}}", false, 0, 1, "null"},
      {"{\"name\":\"w\",\"extras\":{\"sampling\":{\"mode\":\"hex\"}}}", false, 0, 1, "mode"},
  };
  for (const Case& c : cases) {
    INFO(c.materials);
    lines.unknown = 0;
    lines.foreign = 0;
    lines.last_value.clear();
    lines.last_material.clear();
    MeshData loaded;
    std::string why;
    CHECK_MESSAGE(load(with_materials(c.materials), loaded, why), why);
    log::flush();
    REQUIRE(loaded.materials.size() == 1);
    CHECK(loaded.materials[0].stochastic == c.stochastic);
    CHECK(lines.unknown == c.unknown);
    CHECK(lines.foreign == c.foreign);
    if (c.unknown + c.foreign != 0) {
      CHECK_MESSAGE(lines.last_value.find(c.value) != std::string::npos, lines.last_value);
      CHECK_MESSAGE(lines.last_material.find("w") != std::string::npos, lines.last_material);
    }
  }
  log::remove_sink(&lines);
}
