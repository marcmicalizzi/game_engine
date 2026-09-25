// How a material's slots read their images: the glTF sampler of the texture each slot names and
// the reference's KHR_texture_transform (docs/subsystems/assets.md, "Samplers and texture
// transforms"). A two-triangle quad whose three materials use a repeating, a mirrored and a
// clamped sampler, nearest and linear filters, a transform with every field set, the occlusion
// strength, and a transform that moves its texture to TEXCOORD_1 — which the importer refuses as
// it refuses a reference's own texCoord of 1. Then the same materials through the `.clusters`
// container and back, and through a container written before the section existed, which reads
// every slot at the glTF defaults.
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::assets;
using geometry::k_slot_base_color;
using geometry::k_slot_emissive;
using geometry::k_slot_metallic_roughness;
using geometry::k_slot_normal;
using geometry::k_slot_occlusion;
using geometry::TextureFilter;
using geometry::TextureSampler;
using geometry::TextureWrap;

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

// A unit quad (four positions, four UVs, six u16 indices) as a data-URI buffer, one primitive per
// material so every material is referenced by the mesh.
std::string quad_gltf() {
  const f32 positions[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  const f32 uvs[8] = {0, 1, 1, 1, 1, 0, 0, 0};
  const u16 indices[6] = {0, 1, 2, 0, 2, 3};
  std::vector<u8> buffer;
  append(buffer, positions, 12);
  append(buffer, uvs, 8);
  append(buffer, indices, 6);
  buffer.resize(buffer.size() + 4, 0);  // 80 + 12 = 92, padded to 96
  const std::string uri = "data:application/octet-stream;base64," + base64(buffer);
  std::string json;
  json += "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_texture_transform\"],";
  json += "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],";
  json += "\"meshes\":[{\"primitives\":[";
  for (int m = 0; m < 3; ++m) {
    if (m > 0) json += ",";
    json += "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":2,\"material\":" +
            std::to_string(m) + "}";
  }
  json += "]}],";
  json += "\"materials\":[";
  // "tiled": a nearest-magnified base colour on a repeating sampler with every transform field
  // set; a mirrored-and-clamped normal map; an occlusion map with no sampler of its own (the
  // defaults) at strength 0.4 and a scale-only transform; and an emissive map whose transform
  // moves it to TEXCOORD_1.
  json +=
      "{\"name\":\"tiled\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
      "\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.5,0.25],\"rotation\":1.5,"
      "\"scale\":[3,2]}}}},"
      "\"normalTexture\":{\"index\":1},"
      "\"occlusionTexture\":{\"index\":2,\"strength\":0.4,"
      "\"extensions\":{\"KHR_texture_transform\":{\"scale\":[8,8]}}},"
      "\"emissiveTexture\":{\"index\":1,\"extensions\":{\"KHR_texture_transform\":"
      "{\"texCoord\":1}}},\"emissiveFactor\":[1,1,1]},";
  // "clamped": a clamped, nearest base colour that picks one level; its metallic-roughness map
  // is on the mirrored sampler.
  json +=
      "{\"name\":\"clamped\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":3},"
      "\"metallicRoughnessTexture\":{\"index\":1}}},";
  // "plain": no textures at all.
  json += "{\"name\":\"plain\"}],";
  json +=
      "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":1,\"sampler\":1},{\"source\":2},"
      "{\"source\":0,\"sampler\":2}],";
  json +=
      "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9985,\"wrapS\":10497,\"wrapT\":10497},"
      "{\"wrapS\":33648,\"wrapT\":33071},"
      "{\"magFilter\":9728,\"minFilter\":9986,\"wrapS\":33071,\"wrapT\":33071}],";
  json +=
      "\"images\":[{\"uri\":\"bricks.png\"},{\"uri\":\"bricks_normal.png\"},"
      "{\"uri\":\"bricks_ao.png\"}],";
  json +=
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[0,0,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],";
  json +=
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":12}],";
  json += "\"buffers\":[{\"uri\":\"" + uri + "\",\"byteLength\":" + std::to_string(buffer.size()) +
          "}]}";
  return json;
}

void check_sampling(const Material* materials) {
  const Material& tiled = materials[0];
  CHECK(tiled.base_color_image == 0);
  const geometry::TextureSlotSampling& color = tiled.sampling[k_slot_base_color];
  CHECK(color.sampler.wrap_s == TextureWrap::repeat);
  CHECK(color.sampler.wrap_t == TextureWrap::repeat);
  CHECK(color.sampler.mag == TextureFilter::nearest);
  CHECK(color.sampler.min == TextureFilter::linear);   // LINEAR_MIPMAP_NEAREST
  CHECK(color.sampler.mip == TextureFilter::nearest);  // one level, not a blend of two
  CHECK(color.transform.offset == Vec2(0.5f, 0.25f));
  CHECK(color.transform.rotation == 1.5f);
  CHECK(color.transform.scale == Vec2(3.0f, 2.0f));
  const geometry::TextureSlotSampling& normal = tiled.sampling[k_slot_normal];
  CHECK(tiled.normal_image == 1);
  CHECK(normal.sampler.wrap_s == TextureWrap::mirrored_repeat);
  CHECK(normal.sampler.wrap_t == TextureWrap::clamp_to_edge);
  CHECK(normal.sampler.mag == TextureFilter::linear);  // undefined filters read linear
  CHECK(normal.sampler.min == TextureFilter::linear);
  CHECK(normal.sampler.mip == TextureFilter::linear);
  CHECK(normal.transform.identity());
  // No sampler on the texture: the glTF defaults. Strength and transform come through.
  CHECK(tiled.occlusion_image == 2);
  CHECK(tiled.occlusion_strength == 0.4f);
  CHECK(tiled.sampling[k_slot_occlusion].sampler == TextureSampler{});
  CHECK(tiled.sampling[k_slot_occlusion].transform.scale == Vec2(8.0f, 8.0f));
  CHECK(tiled.sampling[k_slot_occlusion].transform.offset == Vec2(0.0f, 0.0f));
  // The transform named TEXCOORD_1: refused like a reference's own texCoord of 1, and its slot
  // left at the defaults.
  CHECK(tiled.emissive_image == -1);
  CHECK(tiled.sampling[k_slot_emissive].sampler == TextureSampler{});
  CHECK(tiled.sampling[k_slot_emissive].transform.identity());

  const Material& clamped = materials[1];
  CHECK(clamped.base_color_image == 0);
  const TextureSampler& c = clamped.sampling[k_slot_base_color].sampler;
  CHECK(c.wrap_s == TextureWrap::clamp_to_edge);
  CHECK(c.wrap_t == TextureWrap::clamp_to_edge);
  CHECK(c.mag == TextureFilter::nearest);
  CHECK(c.min == TextureFilter::nearest);  // NEAREST_MIPMAP_LINEAR
  CHECK(c.mip == TextureFilter::linear);
  CHECK(clamped.sampling[k_slot_base_color].transform.identity());
  CHECK(clamped.metallic_roughness_image == 1);
  CHECK(clamped.sampling[k_slot_metallic_roughness].sampler.wrap_s == TextureWrap::mirrored_repeat);
  CHECK(clamped.occlusion_strength == 1.0f);  // no occlusion map: the neutral strength

  const Material& plain = materials[2];
  for (u32 s = 0; s < geometry::k_material_slots; ++s) {
    CHECK(plain.sampling[s].sampler == TextureSampler{});
    CHECK(plain.sampling[s].transform.identity());
  }
}

}  // namespace

TEST_CASE("gltf samplers: wraps, filters and texture transforms reach the material's slots") {
  MeshData mesh;
  std::string error;
  const std::string json = quad_gltf();
  REQUIRE_MESSAGE(
      load_gltf_memory(std::span<const u8>(reinterpret_cast<const u8*>(json.data()), json.size()),
                       "", mesh, &error),
      error);
  REQUIRE(mesh.materials.size() == 3);
  check_sampling(mesh.materials.data());
}

TEST_CASE("gltf samplers: the container carries them, and an older container reads the defaults") {
  MeshData mesh;
  std::string error;
  const std::string json = quad_gltf();
  REQUIRE_MESSAGE(
      load_gltf_memory(std::span<const u8>(reinterpret_cast<const u8*>(json.data()), json.size()),
                       "", mesh, &error),
      error);
  REQUIRE(mesh.materials.size() == 3);

  // The container as the content build writes it: a DAG, the materials, one sampling record per
  // material, and the image paths.
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(geometry::build_cluster_lod(mesh.positions, mesh.indices,
                                              geometry::ClusterLodOptions{}, data.mesh, &error),
                  error);
  for (const Material& m : mesh.materials) {
    geometry::ClusterFileMaterial record;
    record.base_color_image = m.base_color_image;
    record.normal_image = m.normal_image;
    record.metallic_roughness_image = geometry::encode_optional_image(m.metallic_roughness_image);
    record.occlusion_image = geometry::encode_optional_image(m.occlusion_image);
    record.emissive_image = geometry::encode_optional_image(m.emissive_image);
    data.materials.push_back(record);
    data.material_sampling.push_back(
        geometry::encode_material_sampling(m.sampling, m.occlusion_strength));
  }
  data.cluster_material.assign(data.mesh.mesh.clusters.size(), 0u);
  for (const ImageRef& image : mesh.images)
    data.image_paths.push_back(image.uri);

  const test::TempDir tmp("assets_sampler_container");
  const std::string path = tmp.file("quad.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, data, &error), error);
  geometry::ClusterFileData read;
  REQUIRE_MESSAGE(geometry::read_cluster_file(path, read, &error), error);
  REQUIRE(read.material_sampling.size() == 3);
  Material back[3];
  for (u32 i = 0; i < 3; ++i) {
    back[i].base_color_image = read.materials[i].base_color_image;
    back[i].normal_image = read.materials[i].normal_image;
    back[i].metallic_roughness_image =
        geometry::decode_optional_image(read.materials[i].metallic_roughness_image);
    back[i].occlusion_image = geometry::decode_optional_image(read.materials[i].occlusion_image);
    back[i].emissive_image = geometry::decode_optional_image(read.materials[i].emissive_image);
    geometry::decode_material_sampling(geometry::cluster_material_sampling(read, i),
                                       back[i].sampling, back[i].occlusion_strength);
  }
  check_sampling(back);

  // A container written before section 33 has none of it, and every slot of every material reads
  // what glTF says a texture with no sampler and no transform reads as.
  data.material_sampling.clear();
  const std::string older = tmp.file("older.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(older, data, &error), error);
  geometry::ClusterFileData old_read;
  REQUIRE_MESSAGE(geometry::read_cluster_file(older, old_read, &error), error);
  CHECK(old_read.material_sampling.empty());
  for (u32 i = 0; i < 3; ++i) {
    geometry::TextureSlotSampling slots[geometry::k_material_slots];
    f32 strength = 0.0f;
    geometry::decode_material_sampling(geometry::cluster_material_sampling(old_read, i), slots,
                                       strength);
    CHECK(strength == 1.0f);
    for (const geometry::TextureSlotSampling& slot : slots) {
      CHECK(slot.sampler.wrap_s == TextureWrap::repeat);
      CHECK(slot.sampler.wrap_t == TextureWrap::repeat);
      CHECK(slot.sampler.mag == TextureFilter::linear);
      CHECK(slot.sampler.min == TextureFilter::linear);
      CHECK(slot.sampler.mip == TextureFilter::linear);
      CHECK(slot.transform.identity());
    }
  }
}
