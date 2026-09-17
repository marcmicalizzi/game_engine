// End to end: engine-view renders a glTF file. The fixture is a unit cube written as a GLB at
// test time (two primitives with two materials, one of them textured with a 4x4 checker PNG in
// the BIN chunk), so nothing binary lives in the tree. Without a display or device the app exits
// 3 and the test records the skip.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/platform/process.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run view(std::vector<std::string> args) {
  std::vector<std::string_view> argv;
  argv.push_back(ENGINE_APP_PATH);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
               /*merge_stderr=*/true)) {
    FAIL("cannot spawn engine-view: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  return run;
}

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_u16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v & 0xffu));
  out.push_back(static_cast<u8>(v >> 8));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

void pad4(std::vector<u8>& out, u8 fill) {
  while (out.size() % 4 != 0)
    out.push_back(fill);
}

std::string n(u32 v) { return std::to_string(v); }

// A unit cube as a GLB: 24 vertices with normals and UVs, two primitives of six triangles
// (a textured white material and a plain red one), and a 4x4 checker PNG in the BIN chunk.
bool write_cube_glb(const std::string& path) {
  const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                           Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                            Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<u8> bin;
  for (u32 f = 0; f < 6; ++f) {  // positions: corners wound counter-clockwise from outside
    const Vec3 nrm = normals[f];
    const Vec3 t = tangents[f];
    const Vec3 b = cross(nrm, t);
    const Vec3 corners[4] = {nrm * 0.5f - t * 0.5f - b * 0.5f, nrm * 0.5f + t * 0.5f - b * 0.5f,
                             nrm * 0.5f + t * 0.5f + b * 0.5f, nrm * 0.5f - t * 0.5f + b * 0.5f};
    for (const Vec3& c : corners) {
      put_f32(bin, c.x);
      put_f32(bin, c.y);
      put_f32(bin, c.z);
    }
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    for (u32 c = 0; c < 4; ++c) {
      put_f32(bin, normals[f].x);
      put_f32(bin, normals[f].y);
      put_f32(bin, normals[f].z);
    }
  }
  const u32 uv_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const f32 uvs[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (const f32 v : uvs)
      put_f32(bin, v);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const u16 base = static_cast<u16>(f * 4);
    const u16 tris[6] = {base, static_cast<u16>(base + 1), static_cast<u16>(base + 2),
                         base, static_cast<u16>(base + 2), static_cast<u16>(base + 3)};
    for (const u16 i : tris)
      put_u16(bin, i);
  }
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  pad4(bin, 0);
  const u32 png_offset = static_cast<u32>(bin.size());
  Vector<u8> pixels(4 * 4 * 4);
  for (u32 y = 0; y < 4; ++y) {
    for (u32 x = 0; x < 4; ++x) {
      const bool a = ((x + y) & 1) == 0;
      u8* p = &pixels[(y * 4 + x) * 4];
      p[0] = a ? 40 : 240;
      p[1] = a ? 90 : 220;
      p[2] = a ? 230 : 40;
      p[3] = 255;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(4, 4, 4, std::span<const u8>(pixels.data(), pixels.size()), png))
    return false;
  bin.insert(bin.end(), png.begin(), png.end());
  const u32 png_bytes = png.size();
  pad4(bin, 0);

  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":["
      "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,"
      "\"material\":0},"
      "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":4,"
      "\"material\":1}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],"
      "\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":0.8}},"
      "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.15,0.1,1],\"metallicFactor\":0,"
      "\"roughnessFactor\":0.5}}],"
      "\"textures\":[{\"source\":0}],"
      "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":18,\"type\":\"SCALAR\"},"
      "{\"bufferView\":3,\"byteOffset\":36,\"componentType\":5123,\"count\":18,"
      "\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "},{\"buffer\":0,\"byteOffset\":" + n(png_offset) + ",\"byteLength\":" + n(png_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + n(static_cast<u32>(bin.size())) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';

  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return f.good();
}

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

}  // namespace

TEST_CASE("engine-view: renders a glTF mesh with one cluster DAG per material") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_view_mesh_tests";
  std::filesystem::create_directories(dir);
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string capture = slashes(dir / "mesh.png");
  REQUIRE(write_cube_glb(mesh));
  const Run run = view({"--width", "320", "--height", "240", "--frames", "6", "--no-vsync",
                        "--orbit", "20", "--mesh", mesh, "--capture", capture});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    std::filesystem::remove_all(dir);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  const usize line_start = run.output.find_last_of('\n', run.output.size() - 2);
  const std::string last = run.output.substr(line_start == std::string::npos ? 0 : line_start + 1);
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(last, summary).ok, last);
  auto number = [&](const char* key) {
    u64 v = 0;
    const JsonValue* value = summary.find(key);
    return value != nullptr && value->get_u64(v) ? v : ~u64{0};
  };
  CHECK(number("mesh_primitives") == 2);
  CHECK(number("triangles") == 12);
  CHECK(number("leaf_clusters") == 2);   // six triangles per material fit one cluster each
  CHECK(number("visible_hw_last") > 0);  // the cube drew
  CHECK(number("visible_hw_last") <= 2);
  CHECK(std::filesystem::exists(capture));
  CHECK(std::filesystem::file_size(capture) > 1000);

  // A file that does not exist fails cleanly (exit 1) once the window and device are up.
  const Run missing = view({"--frames", "1", "--mesh", slashes(dir / "missing.glb")});
  CHECK(missing.exit_code == 1);
  CHECK(missing.output.find("mesh") != std::string::npos);
  // The fixture and the capture stay next to the temp directory for a look after the run.
  const auto temp = std::filesystem::temp_directory_path();
  std::filesystem::copy_file(dir / "cube.glb", temp / "engine_view_mesh_cube.glb",
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(dir / "mesh.png", temp / "engine_view_mesh_last.png",
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::remove_all(dir);
}
