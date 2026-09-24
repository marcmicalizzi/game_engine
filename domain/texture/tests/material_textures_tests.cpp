// The per-slot defaults, the options word, the format rules and the cache key: the pieces three
// callers (engine-content, engine-view's own cache writer, the renderer) have to agree on byte for
// byte.
#include <core/hash/hash.h>
#include <domain/texture/material_textures.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::texture;

TEST_CASE("material textures: every slot gets its format, and a shared image the first role's") {
  geometry::ClusterFileMaterial a;
  a.base_color_image = 0;
  a.normal_image = 1;
  a.metallic_roughness_image = geometry::encode_optional_image(2);
  a.occlusion_image = geometry::encode_optional_image(2);  // an ORM image, named twice
  geometry::ClusterFileMaterial b;
  b.base_color_image = -1;
  b.normal_image = -1;
  b.occlusion_image = geometry::encode_optional_image(3);
  b.emissive_image = geometry::encode_optional_image(4);
  b.metallic_roughness_image = geometry::encode_optional_image(9);  // past the end: ignored
  const geometry::ClusterFileMaterial materials[2] = {a, b};
  Vector<u32> roles;
  image_roles(materials, 6, roles);
  REQUIRE(roles.size() == 6);
  CHECK(roles[0] == k_role_base_color);
  CHECK(roles[1] == k_role_normal);
  CHECK(roles[2] == (k_role_metallic_roughness | k_role_occlusion));
  CHECK(roles[3] == k_role_occlusion);
  CHECK(roles[4] == k_role_emissive);
  CHECK(roles[5] == 0);

  const TextureBuildOptions base = options_for_roles(roles[0]);
  CHECK(base.format == FormatChoice::bc7);
  CHECK(base.color_space == ColorSpace::srgb);
  CHECK(base.mips);
  const TextureBuildOptions normal = options_for_roles(roles[1]);
  CHECK(normal.format == FormatChoice::bc5);
  CHECK(normal.normal_map);
  CHECK(normal.color_space == ColorSpace::linear);
  const TextureBuildOptions orm = options_for_roles(roles[2]);
  CHECK(orm.format == FormatChoice::bc7);
  CHECK(orm.color_space == ColorSpace::linear);
  const TextureBuildOptions occlusion = options_for_roles(roles[3]);
  CHECK(occlusion.format == FormatChoice::bc4);
  const TextureBuildOptions emissive = options_for_roles(roles[4]);
  CHECK(emissive.format == FormatChoice::bc7);
  CHECK(emissive.color_space == ColorSpace::srgb);
  // Colour wins over data when an asset gives one image both kinds of role.
  CHECK(options_for_roles(k_role_base_color | k_role_normal).color_space == ColorSpace::srgb);
}

TEST_CASE("material textures: records key an embedded image by its bytes and leave a file's open") {
  geometry::ClusterFileData data;
  geometry::ClusterFileMaterial m;
  m.base_color_image = 0;
  m.normal_image = 1;
  data.materials.push_back(m);
  data.image_paths.push_back("");            // embedded
  data.image_paths.push_back("normal.png");  // a file beside the source
  data.image_paths.push_back("");            // named by nothing
  data.images.resize(3);
  const char png[] = "not really a png, but bytes all the same";
  data.images[0].bytes.append(
      std::span<const u8>(reinterpret_cast<const u8*>(png), sizeof(png) - 1));
  data.images[2].bytes.append(
      std::span<const u8>(reinterpret_cast<const u8*>(png), sizeof(png) - 1));
  fill_cluster_texture_records(data);
  REQUIRE(data.textures.size() == 3);

  const u64 hash = hash_bytes(png, sizeof(png) - 1);
  const TextureBuildOptions base = options_for_roles(k_role_base_color);
  CHECK(data.textures[0].roles == k_role_base_color);
  CHECK(data.textures[0].options == pack_texture_options(base));
  CHECK(data.textures[0].source_hash == hash);
  CHECK(data.textures[0].key == texture_cache_key(hash, base));

  CHECK(data.textures[1].roles == k_role_normal);
  CHECK(data.textures[1].options == pack_texture_options(options_for_roles(k_role_normal)));
  CHECK(data.textures[1].source_hash == 0);  // the file's bytes are not the container's
  CHECK(data.textures[1].key == 0);

  CHECK(data.textures[2].roles == 0);  // bytes, but no slot samples them: nothing is built
  CHECK(data.textures[2].options == 0);
  CHECK(data.textures[2].key == 0);
}

TEST_CASE("material textures: the options word round-trips and refuses what it does not know") {
  for (u32 f = 0; f <= static_cast<u32>(FormatChoice::bc7); ++f) {
    for (u32 bits = 0; bits < 8; ++bits) {
      TextureBuildOptions options;
      options.format = static_cast<FormatChoice>(f);
      options.color_space = (bits & 1) != 0 ? ColorSpace::srgb : ColorSpace::linear;
      options.normal_map = (bits & 2) != 0;
      options.mips = (bits & 4) != 0;
      const u32 packed = pack_texture_options(options);
      CHECK(packed != 0);
      TextureBuildOptions back;
      REQUIRE(unpack_texture_options(packed, back));
      CHECK(back.format == options.format);
      CHECK(back.color_space == options.color_space);
      CHECK(back.normal_map == options.normal_map);
      CHECK(back.mips == options.mips);
    }
  }
  TextureBuildOptions out;
  CHECK_FALSE(unpack_texture_options(0, out));               // "no texture wanted"
  CHECK_FALSE(unpack_texture_options(0x1u, out));            // no validity bit
  CHECK_FALSE(unpack_texture_options(0x80u | 0xfu, out));    // a format past the last
  CHECK_FALSE(unpack_texture_options(0x80u | 0x100u, out));  // a bit a newer build set
}

TEST_CASE("material textures: auto picks the format, and sRGB is refused where it cannot be") {
  TextureFormat format = TextureFormat::rgba8;
  TextureBuildOptions options;
  REQUIRE(resolve_texture_format(options, 4, format));
  CHECK(format == TextureFormat::bc7);
  options.normal_map = true;
  REQUIRE(resolve_texture_format(options, 3, format));
  CHECK(format == TextureFormat::bc5);
  options.normal_map = false;
  options.color_space = ColorSpace::linear;
  REQUIRE(resolve_texture_format(options, 1, format));
  CHECK(format == TextureFormat::bc4);
  options.color_space = ColorSpace::srgb;
  REQUIRE(resolve_texture_format(options, 1, format));
  CHECK(format == TextureFormat::bc7);  // grey colour: BC4 has no sRGB form
  options.format = FormatChoice::bc4;
  std::string error;
  CHECK_FALSE(resolve_texture_format(options, 1, format, &error));
  CHECK_MESSAGE(error.find("no sRGB form") != std::string::npos, error);
  options.format = FormatChoice::bc5;
  options.normal_map = true;  // a normal map is data, whatever the colour space says
  REQUIRE(resolve_texture_format(options, 3, format));
  CHECK(format == TextureFormat::bc5);
  FormatChoice choice = FormatChoice::automatic;
  CHECK(parse_format_choice("bc3", choice));
  CHECK(choice == FormatChoice::bc3);
  CHECK(parse_format_choice("auto", choice));
  CHECK(choice == FormatChoice::automatic);
  CHECK_FALSE(parse_format_choice("dxt5", choice));
}

TEST_CASE("material textures: the cache key answers to the bytes, the options and the version") {
  TextureBuildOptions options;
  const u64 key = texture_cache_key(1234, options);
  CHECK(key != texture_cache_key(1235, options));
  TextureBuildOptions linear = options;
  linear.color_space = ColorSpace::linear;
  CHECK(key != texture_cache_key(1234, linear));
  TextureBuildOptions no_mips = options;
  no_mips.mips = false;
  CHECK(key != texture_cache_key(1234, no_mips));
  TextureBuildOptions bc1 = options;
  bc1.format = FormatChoice::bc1;
  CHECK(key != texture_cache_key(1234, bc1));
  CHECK(key == texture_cache_key(1234, options));
  const std::string path = texture_cache_path("root", 0x0123456789abcdefull);
  CHECK(path.find("textures") != std::string::npos);
  CHECK(path.find("0123456789abcdef.tex") != std::string::npos);
}
