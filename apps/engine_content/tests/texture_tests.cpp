// End to end: the texture node of the content build (docs/subsystems/texture.md). A glTF quad
// written at test time — three images in files of their own (base colour, normal map, and an
// occlusion-roughness-metal image both the metallic-roughness and the occlusion slot name), one
// embedded in the buffer (emissive), and one no slot names — builds into a scratch derived-data
// root: one `.tex` per sampled image in the format its slot asks for, the keys recorded in the
// container, `info` reading both, a second build finding every texture up to date by identity, a
// repainted image rebuilt and nothing else, the same bytes for one thread and for four, and
// `engine-content texture` refusing what it cannot build. Nothing binary lives in the tree.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/texture/texture_file.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
  std::vector<JsonValue> lines;  // stdout, one JSON object per line
};

Run content(std::vector<std::string> args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-content: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  usize at = 0;
  while (at < run.output.size()) {
    usize end = run.output.find('\n', at);
    if (end == std::string::npos) end = run.output.size();
    std::string_view line(run.output.data() + at, end - at);
    while (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    JsonValue value;
    if (!line.empty() && parse_json(line, value).ok && value.is_object())
      run.lines.push_back(std::move(value));
    at = end + 1;
  }
  return run;
}

u64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  u64 out = 0;
  return value != nullptr && value->get_u64(out) ? out : ~u64{0};
}

std::string text_of(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

std::string file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_file(const std::string& path, const void* data, usize size) {
  std::ofstream f(path, std::ios::binary);
  f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return f.good();
}

bool write_png(const std::string& path, u32 w, u32 h, const std::vector<u8>& rgba) {
  Vector<u8> png;
  if (!image::encode_png(w, h, 4, std::span<const u8>(rgba.data(), rgba.size()), png)) return false;
  return write_file(path, png.data(), png.size());
}

// A 64x48 image with something in every channel, `seed` making two of them different bytes.
std::vector<u8> pattern(u32 seed) {
  std::vector<u8> rgba(64 * 48 * 4);
  for (u32 y = 0; y < 48; ++y) {
    for (u32 x = 0; x < 64; ++x) {
      u8* t = &rgba[(y * 64 + x) * 4];
      t[0] = static_cast<u8>((x * 4 + seed * 37) & 0xff);
      t[1] = static_cast<u8>((y * 5 + seed * 11) & 0xff);
      t[2] = static_cast<u8>(((x / 8 + y / 8) % 2) * 200 + 20);
      t[3] = 255;
    }
  }
  return rgba;
}

// A tangent-space normal map of domes.
std::vector<u8> normal_map() {
  std::vector<u8> rgba(64 * 48 * 4);
  for (u32 y = 0; y < 48; ++y) {
    for (u32 x = 0; x < 64; ++x) {
      const i32 cx = static_cast<i32>(x % 16) - 8;
      const i32 cy = static_cast<i32>(y % 16) - 8;
      u8* t = &rgba[(y * 64 + x) * 4];
      t[0] = static_cast<u8>(128 + cx * 9);
      t[1] = static_cast<u8>(128 + cy * 9);
      t[2] = static_cast<u8>(255 - (cx * cx + cy * cy));
      t[3] = 255;
    }
  }
  return rgba;
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((bits >> (8 * i)) & 0xffu));
}

// The quad and its images, into `dir`. Returns false when a file could not be written.
bool write_fixture(const std::filesystem::path& dir) {
  auto at = [&](const char* name) { return (dir / name).string(); };
  if (!write_png(at("base.png"), 64, 48, pattern(1))) return false;
  if (!write_png(at("normal.png"), 64, 48, normal_map())) return false;
  if (!write_png(at("orm.png"), 64, 48, pattern(2))) return false;
  if (!write_png(at("unused.png"), 64, 48, pattern(3))) return false;
  Vector<u8> emissive;
  const std::vector<u8> emissive_pixels = pattern(4);
  if (!image::encode_png(64, 48, 4,
                         std::span<const u8>(emissive_pixels.data(), emissive_pixels.size()),
                         emissive)) {
    return false;
  }
  std::vector<u8> bin;
  const f32 positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const f32 normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 uvs[8] = {0, 1, 1, 1, 1, 0, 0, 0};
  for (const f32 v : positions)
    put_f32(bin, v);
  for (const f32 v : normals)
    put_f32(bin, v);
  for (const f32 v : uvs)
    put_f32(bin, v);
  const u16 indices[6] = {0, 1, 2, 0, 2, 3};
  for (const u16 i : indices) {
    bin.push_back(static_cast<u8>(i & 0xff));
    bin.push_back(static_cast<u8>(i >> 8));
  }
  const u32 png_at = static_cast<u32>(bin.size());  // 140, a multiple of four
  bin.insert(bin.end(), emissive.begin(), emissive.end());
  if (!write_file(at("quad.bin"), bin.data(), bin.size())) return false;
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
      "\"metallicRoughnessTexture\":{\"index\":2}},\"normalTexture\":{\"index\":1},"
      "\"occlusionTexture\":{\"index\":2},\"emissiveTexture\":{\"index\":3},"
      "\"emissiveFactor\":[1,1,1]}],"
      "\"textures\":[{\"source\":0},{\"source\":1},{\"source\":2},{\"source\":3}],"
      "\"images\":[{\"uri\":\"base.png\"},{\"uri\":\"normal.png\"},{\"uri\":\"orm.png\"},"
      "{\"bufferView\":4,\"mimeType\":\"image/png\"},{\"uri\":\"unused.png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":12},"
      "{\"buffer\":0,\"byteOffset\":" +
      std::to_string(png_at) + ",\"byteLength\":" + std::to_string(emissive.size()) +
      "}],"
      "\"buffers\":[{\"uri\":\"quad.bin\",\"byteLength\":" +
      std::to_string(bin.size()) + "}]}";
  return write_file(at("quad.gltf"), json.data(), json.size());
}

const JsonValue* textures_of(const JsonValue& line) { return line.find("textures"); }

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

}  // namespace

TEST_CASE("engine-content: a mesh's images build into the derived-data cache, keyed and reused") {
  const test::TempDir tmp("engine_content_textures");
  const std::filesystem::path dir = tmp.native();
  REQUIRE(write_fixture(dir));
  const std::string mesh = slashes(dir / "quad.gltf");
  const std::string ddc = slashes(dir / "ddc");

  // A named output with no cache root touches nothing but the output: no textures.
  const std::string alone = slashes(dir / "alone.clusters");
  const Run named = content({"build", mesh, alone});
  REQUIRE_MESSAGE(named.exit_code == 0, named.output);
  REQUIRE(named.lines.size() == 1);
  const JsonValue* skipped_step = textures_of(named.lines[0]);
  REQUIRE(skipped_step != nullptr);
  CHECK(skipped_step->find("enabled")->as_bool() == false);
  CHECK(number(*skipped_step, "built") == 0);
  CHECK_FALSE(std::filesystem::exists(dir / "ddc"));

  // Into the cache: four textures (base, normal, ORM once for two slots, the embedded emissive),
  // and not the image no slot names.
  const Run built = content({"build", mesh, "--cache", "--ddc", ddc, "--jobs", "1"});
  REQUIRE_MESSAGE(built.exit_code == 0, built.output);
  REQUIRE(built.lines.size() == 1);
  const JsonValue* step = textures_of(built.lines[0]);
  REQUIRE(step != nullptr);
  CHECK(step->find("enabled")->as_bool());
  CHECK(number(*step, "built") == 4);
  CHECK(number(*step, "failed") == 0);
  CHECK(number(*step, "missing") == 0);
  const JsonValue* detail = step->find("detail");
  REQUIRE(detail != nullptr);
  REQUIRE(detail->size() == 4);
  const char* formats[4] = {"bc7", "bc5", "bc7", "bc7"};
  const char* spaces[4] = {"srgb", "linear", "linear", "srgb"};
  std::vector<std::string> tex_paths;
  for (u32 i = 0; i < 4; ++i) {
    const JsonValue& row = (*detail)[i];
    CHECK(number(row, "image") == i);
    CHECK(text_of(row, "status") == "built");
    CHECK(text_of(row, "format") == formats[i]);
    CHECK(text_of(row, "color_space") == spaces[i]);
    CHECK(number(row, "levels") == 7);  // 64x48 down to 1x1
    CHECK(number(row, "bytes") > 0);
    tex_paths.push_back(text_of(row, "path"));
    CHECK(std::filesystem::exists(tex_paths.back()));
    CHECK(tex_paths.back().find("/textures/") != std::string::npos);
  }

  // The container records each key: `info` lists the four sampled images, with the embedded one's
  // key and no key for the files (their bytes are not the container's).
  const std::string container = text_of(built.lines[0], "path");
  const Run described = content({"info", container});
  REQUIRE_MESSAGE(described.exit_code == 0, described.output);
  REQUIRE(described.lines.size() == 1);
  const JsonValue* records = described.lines[0].find("textures");
  REQUIRE(records != nullptr);
  REQUIRE(records->size() == 4);
  CHECK(number((*records)[0], "key") == 0);
  CHECK(text_of((*records)[1], "format") == "bc5");
  CHECK(number((*records)[2], "roles") == (4u | 8u));  // metallic-roughness and occlusion
  CHECK(number((*records)[3], "key") == number((*detail)[3], "key"));

  // `info` on a texture is the texture's.
  const Run tex_info = content({"info", tex_paths[1]});
  REQUIRE_MESSAGE(tex_info.exit_code == 0, tex_info.output);
  REQUIRE(tex_info.lines.size() == 1);
  CHECK(text_of(tex_info.lines[0], "kind") == "texture");
  CHECK(text_of(tex_info.lines[0], "format") == "bc5");
  CHECK(tex_info.lines[0].find("normal_map")->as_bool());
  CHECK(number(tex_info.lines[0], "width") == 64);
  CHECK(number(tex_info.lines[0], "height") == 48);
  CHECK(number(tex_info.lines[0], "build_key") == number((*detail)[1], "key"));

  // Again: every texture is up to date by identity, and nothing is written.
  std::vector<std::string> before;
  for (const std::string& path : tex_paths)
    before.push_back(file_bytes(path));
  const auto stamp = std::filesystem::last_write_time(tex_paths[0]);
  const Run again = content({"build", mesh, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(again.exit_code == 0, again.output);
  const JsonValue* again_step = textures_of(again.lines[0]);
  CHECK(number(*again_step, "built") == 0);
  CHECK(number(*again_step, "skipped") == 4);
  CHECK(std::filesystem::last_write_time(tex_paths[0]) == stamp);

  // A manifest over the same mesh: the container is up to date and so are its textures.
  const std::string manifest = slashes(dir / "manifest.json");
  {
    std::ofstream out(manifest);
    out << "{\"meshes\":[{\"source\":\"quad.gltf\"}]}";
  }
  const Run all = content({"build-all", manifest, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(all.exit_code == 0, all.output);
  REQUIRE(all.lines.size() == 2);
  CHECK(text_of(all.lines[0], "status") == "skipped");
  CHECK(number(*textures_of(all.lines[0]), "skipped") == 4);
  CHECK(number(*textures_of(all.lines[1]), "built") == 0);

  // Repaint the base colour: its key moves and only it is built; the container is not rebuilt by
  // build-all, because its own key covers the glTF and its buffers, not the images beside it.
  REQUIRE(write_png((dir / "base.png").string(), 64, 48, pattern(9)));
  const Run repainted = content({"build-all", manifest, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(repainted.exit_code == 0, repainted.output);
  CHECK(text_of(repainted.lines[0], "status") == "skipped");
  CHECK(number(*textures_of(repainted.lines[0]), "built") == 1);
  CHECK(number(*textures_of(repainted.lines[0]), "skipped") == 3);

  // One thread and four build the same bytes: the textures and the container.
  const std::string ddc4 = slashes(dir / "ddc4");
  const Run four = content({"build", mesh, "--cache", "--ddc", ddc4, "--jobs", "4"});
  REQUIRE_MESSAGE(four.exit_code == 0, four.output);
  const JsonValue* four_detail = textures_of(four.lines[0])->find("detail");
  REQUIRE(four_detail != nullptr);
  REQUIRE(four_detail->size() == 4);
  for (u32 i = 1; i < 4; ++i) {  // image 0 was repainted since the first build
    CHECK(file_bytes(text_of((*four_detail)[i], "path")) == before[i]);
  }
  const std::string container4 = text_of(four.lines[0], "path");
  CHECK(file_bytes(container4) == file_bytes(container));
}

TEST_CASE("engine-content: texture builds one image and refuses what it cannot build") {
  const test::TempDir tmp("engine_content_texture");
  const std::filesystem::path dir = tmp.native();
  REQUIRE(write_png((dir / "n.png").string(), 64, 48, normal_map()));
  const std::string in = slashes(dir / "n.png");

  const Run normal = content({"texture", in, slashes(dir / "n.tex"), "--normal"});
  REQUIRE_MESSAGE(normal.exit_code == 0, normal.output);
  REQUIRE(normal.lines.size() == 1);
  CHECK(text_of(normal.lines[0], "format") == "bc5");
  CHECK(text_of(normal.lines[0], "color_space") == "linear");
  CHECK(number(normal.lines[0], "levels") == 7);
  texture::TextureData read;
  std::string error;
  REQUIRE_MESSAGE(texture::read_texture_file(slashes(dir / "n.tex"), read, &error), error);
  CHECK(read.format == texture::TextureFormat::bc5);
  CHECK((read.flags & texture::k_texture_normal_map) != 0);

  const Run flat = content(
      {"texture", in, slashes(dir / "f.tex"), "--no-mips", "--format", "rgba8", "--linear"});
  REQUIRE_MESSAGE(flat.exit_code == 0, flat.output);
  CHECK(number(flat.lines[0], "levels") == 1);
  CHECK(number(flat.lines[0], "block_bytes") == 64u * 48u * 4u);

  // Into a cache: the path is the key's, and the key is the one `build` would record.
  const Run cached = content({"texture", in, "--cache", "--ddc", slashes(dir / "ddc"), "--normal"});
  REQUIRE_MESSAGE(cached.exit_code == 0, cached.output);
  CHECK(text_of(cached.lines[0], "path").find("/textures/") != std::string::npos);
  CHECK(number(cached.lines[0], "build_key") == number(normal.lines[0], "build_key"));

  CHECK(content({"texture", in, slashes(dir / "x.tex"), "--format", "bc4", "--srgb"}).exit_code ==
        2);
  CHECK(content({"texture", in, slashes(dir / "x.tex"), "--normal", "--srgb"}).exit_code == 2);
  CHECK(content({"texture", in, slashes(dir / "x.tex"), "--format", "bc6h"}).exit_code == 2);
  CHECK(content({"texture", in}).exit_code == 2);
  CHECK(content({"texture", slashes(dir / "missing.png"), slashes(dir / "x.tex")}).exit_code == 1);
  CHECK_FALSE(std::filesystem::exists(dir / "x.tex"));
}
