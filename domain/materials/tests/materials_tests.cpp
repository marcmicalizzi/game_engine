// Material sets and packs (docs/subsystems/materials.md): a set written into a container's
// materials, the override order, and a pack's manifest — read, refused, found once per process.
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;

namespace {

materials::MaterialSet bark_set() {
  materials::MaterialSet set;
  set.base_color = "bark/base_color.png";
  set.normal = "bark/normal.png";
  set.roughness = "bark/roughness.png";
  set.metallic = "bark/metallic.png";
  set.height = "bark/height.png";
  set.tile_m = 0.5f;
  set.normal_scale = 0.4f;
  return set;
}

// A container with `count` flat materials, as a solver leaves it before a set is applied.
geometry::ClusterFileData flat(u32 count) {
  geometry::ClusterFileData data;
  for (u32 i = 0; i < count; ++i) {
    geometry::ClusterFileMaterial m;
    m.base_color = Vec4{0.3f, 0.2f, 0.1f, 1.0f};
    m.metallic = 0.0f;
    m.roughness = 0.9f;
    data.materials.push_back(m);
    data.material_sampling.push_back(geometry::ClusterFileMaterialSampling{});
  }
  return data;
}

std::string pack_text(std::string_view role, std::string_view extra = "") {
  std::string text =
      R"({"format":"engine.materials.pack/1","name":"test","version":"1","roles":{")";
  text += role;
  text += R"(":{"tile_m":0.35,"normal_scale":0.4,"normal_convention":"opengl",)";
  text += extra;
  text += R"("maps":{"base_color":{"file":"r/b.png"},"normal":{"file":"r/n.png"},)"
          R"("roughness":{"file":"r/r.png"},"metallic":{"file":"r/m.png"},)"
          R"("height":{"file":"r/h.png"}}}}})";
  return text;
}

}  // namespace

TEST_CASE("materials: a set's images go into the slots, roughness alone and metallic zero") {
  geometry::ClusterFileData data = flat(2);
  materials::apply_set(bark_set(), materials::UvLayout::metres, data, 0);
  REQUIRE(data.image_paths.size() == 3);  // base colour, roughness, normal; metallic and height
                                          // are carried and read by nothing yet
  CHECK(data.images.size() == data.image_paths.size());
  const geometry::ClusterFileMaterial& m = data.materials[0];
  CHECK(data.image_paths[static_cast<u32>(m.base_color_image)] == "bark/base_color.png");
  CHECK(data.image_paths[static_cast<u32>(m.normal_image)] == "bark/normal.png");
  CHECK(data.image_paths[static_cast<u32>(
            geometry::decode_optional_image(m.metallic_roughness_image))] == "bark/roughness.png");
  CHECK(m.base_color.x == 1.0f);
  CHECK(m.roughness == 1.0f);
  CHECK(m.metallic == 0.0f);
  CHECK(m.normal_scale == 0.4f);
  // Metres of surface: the set's repeat is a transform of 1 / tile_m on every slot it samples.
  geometry::TextureSlotSampling slots[geometry::k_material_slots];
  f32 occlusion = 0.0f;
  geometry::decode_material_sampling(data.material_sampling[0], slots, occlusion);
  CHECK(slots[geometry::k_slot_base_color].transform.scale.x == doctest::Approx(2.0f));
  CHECK(slots[geometry::k_slot_normal].transform.scale.y == doctest::Approx(2.0f));
  CHECK(slots[geometry::k_slot_metallic_roughness].transform.scale.x == doctest::Approx(2.0f));
  // The untouched material is untouched.
  CHECK(data.materials[1].base_color_image == -1);
  CHECK(data.materials[1].base_color.x == 0.3f);

  // A second part sampling the same images shares them; a card or a part in repeats has no
  // transform.
  materials::apply_set(bark_set(), materials::UvLayout::card, data, 1);
  CHECK(data.image_paths.size() == 3);
  geometry::decode_material_sampling(data.material_sampling[1], slots, occlusion);
  CHECK(slots[geometry::k_slot_base_color].transform.identity());

  // A set that names nothing changes nothing.
  geometry::ClusterFileData untouched = flat(1);
  materials::apply_set(materials::MaterialSet{}, materials::UvLayout::metres, untouched, 0);
  CHECK(untouched.image_paths.empty());
  CHECK(untouched.materials[0].base_color.x == 0.3f);
}

TEST_CASE("materials: the recipe's set, else the pack's role, else none") {
  materials::Pack pack;
  std::string error;
  REQUIRE_MESSAGE(materials::parse_pack(pack_text("palm/bark"), "C:/packs/p", pack, error), error);
  const materials::MaterialSet recipe = bark_set();
  bool from_pack = true;
  CHECK(materials::resolve_set(&recipe, &pack, "palm/bark", &from_pack) == &recipe);
  CHECK_FALSE(from_pack);
  const materials::MaterialSet empty;  // a block with a tile_m and no images is no set
  const materials::MaterialSet* found =
      materials::resolve_set(&empty, &pack, "palm/bark", &from_pack);
  REQUIRE(found != nullptr);
  CHECK(from_pack);
  CHECK(found->base_color == "C:/packs/p/r/b.png");
  CHECK(found->tile_m == doctest::Approx(0.35f));
  CHECK(materials::resolve_set(nullptr, &pack, "palm/skirt", &from_pack) == nullptr);
  CHECK(materials::resolve_set(nullptr, nullptr, "palm/skirt", &from_pack) == nullptr);
  CHECK(materials::pack_key(&pack) != 0);
  CHECK(materials::pack_key(nullptr) == 0);
}

TEST_CASE("materials: a pack's manifest is read whole or refused with the role named") {
  std::string error;
  materials::Pack pack;
  REQUIRE_MESSAGE(materials::parse_pack(pack_text("rock/granite"), "D:/p", pack, error), error);
  CHECK(pack.name == "test");
  REQUIRE(pack.roles.size() == 1);
  CHECK(pack.roles[0].set.normal_scale == doctest::Approx(0.4f));
  CHECK(pack.roles[0].set.height == "D:/p/r/h.png");

  // The key moves with the text and with where the pack is.
  materials::Pack moved;
  REQUIRE(materials::parse_pack(pack_text("rock/granite"), "D:/q", moved, error));
  CHECK(moved.hash != pack.hash);

  CHECK_FALSE(materials::parse_pack(pack_text("granite"), "D:/p", pack, error));
  CHECK(error.find("granite") != std::string::npos);
  CHECK_FALSE(materials::parse_pack(R"({"format":"other","roles":{}})", "D:/p", pack, error));
  std::string directx = pack_text("rock/granite");
  directx.replace(directx.find("\"opengl\""), 8, "\"directx\"");
  CHECK_FALSE(materials::parse_pack(directx, "D:/p", pack, error));
  CHECK(error.find("normal_convention") != std::string::npos);
  std::string missing = pack_text("rock/granite");
  missing.replace(missing.find(R"("height":{"file":"r/h.png"})"), 27, R"("height":{})");
  CHECK_FALSE(materials::parse_pack(missing, "D:/p", pack, error));
  CHECK(error.find("height") != std::string::npos);
  std::string tile = pack_text("rock/granite");
  tile.replace(tile.find("0.35"), 4, "40.0");
  CHECK_FALSE(materials::parse_pack(tile, "D:/p", pack, error));
  CHECK(error.find("tile_m") != std::string::npos);
}

TEST_CASE("materials: pack_at reads a directory's pack once and none where there is none") {
  engine::test::TempDir tmp("materials");
  const std::string dir = tmp.file("pack");
  REQUIRE(io::make_directories(dir) == io::Status::Ok);
  CHECK(materials::pack_at(dir) == nullptr);  // no pack.json: none, and remembered
  const std::string other = tmp.file("other");
  REQUIRE(io::make_directories(other) == io::Status::Ok);
  REQUIRE(io::write_file(io::join_path(other, "pack.json"), pack_text("palm/frond")) ==
          io::Status::Ok);
  const materials::Pack* pack = materials::pack_at(other);
  REQUIRE(pack != nullptr);
  CHECK(materials::pack_at(other) == pack);
  REQUIRE(materials::find_role(pack, "palm/frond") != nullptr);
  CHECK(materials::find_role(pack, "palm/frond")->base_color.find("/other/r/b.png") !=
        std::string::npos);
  CHECK(materials::pack_at("") == nullptr);
}
