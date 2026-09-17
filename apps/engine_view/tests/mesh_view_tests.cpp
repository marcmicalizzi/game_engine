// End to end: engine-view renders a glTF file, the container the derived-data cache made of it
// on the run before, and a container engine-content wrote. The fixture is a unit cube written
// as a GLB at test time (two primitives with two materials, one of them textured with a 4x4
// checker PNG in the BIN chunk), so nothing binary lives in the tree, and every run gets its
// own `--ddc` directory so the cache in the repository is neither read nor written here.
// Without a display or device the app exits 3 and the test records the skip.
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

Run run_app(const std::string& app, const std::vector<std::string>& args) {
  std::vector<std::string_view> argv;
  argv.push_back(app);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
               /*merge_stderr=*/true)) {
    FAIL("cannot spawn " << app << ": " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  return run;
}

Run view(std::vector<std::string> args) { return run_app(ENGINE_APP_PATH, args); }

// The JSON summary is the last line of a run; these read it.
struct Summary {
  JsonValue value;
  u64 number(const char* key) const {
    u64 out = 0;
    const JsonValue* found = value.find(key);
    return found != nullptr && found->get_u64(out) ? out : ~u64{0};
  }
  std::string text(const char* key) const {
    const JsonValue* found = value.find(key);
    return found != nullptr ? std::string(found->as_string()) : std::string();
  }
};

bool parse_summary(const Run& run, Summary& out) {
  if (run.output.size() < 2) return false;
  const usize line_start = run.output.find_last_of('\n', run.output.size() - 2);
  const std::string last = run.output.substr(line_start == std::string::npos ? 0 : line_start + 1);
  return parse_json(last, out.value).ok;
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

// engine-content is built into the same directory as engine-view, and writes the container the
// viewer reads; the test drives the real pair rather than reimplementing either.
std::string content_app() {
  const std::filesystem::path app(ENGINE_APP_PATH);
  return slashes(app.parent_path() / ("engine-content" + app.extension().string()));
}

}  // namespace

TEST_CASE("engine-view: renders a glTF mesh with one cluster DAG per material") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_view_mesh_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string ddc = slashes(dir / "ddc");
  const std::string capture = slashes(dir / "mesh.png");
  REQUIRE(write_cube_glb(mesh));
  // The arguments every run below shares, so that the pictures and the cuts are comparable.
  const std::vector<std::string> common = {"--width",  "320",   "--height",   "240",
                                           "--frames", "6",     "--no-vsync", "--orbit",
                                           "20",       "--ddc", ddc};
  auto with = [&](std::vector<std::string> extra) {
    std::vector<std::string> args = common;
    args.insert(args.end(), extra.begin(), extra.end());
    return args;
  };

  const Run run = view(with({"--mesh", mesh, "--capture", capture}));
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    std::filesystem::remove_all(dir);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  Summary summary;
  REQUIRE_MESSAGE(parse_summary(run, summary), run.output);
  auto number = [&](const char* key) { return summary.number(key); };
  CHECK(number("mesh_primitives") == 2);
  CHECK(number("triangles") == 12);
  CHECK(number("leaf_clusters") == 2);   // six triangles per material fit one cluster each
  CHECK(number("visible_hw_last") > 0);  // the cube drew
  CHECK(number("visible_hw_last") <= 2);
  CHECK(summary.text("mesh_cache") == "miss");  // the cache directory was empty
  CHECK(std::filesystem::exists(capture));
  CHECK(std::filesystem::file_size(capture) > 1000);

  // The build went into the derived-data cache, so the same command again reads the container
  // instead of importing and clustering the glTF, and draws exactly what the first run drew.
  const Run hit = view(with({"--mesh", mesh}));
  REQUIRE_MESSAGE(hit.exit_code == 0, hit.output);
  Summary hit_summary;
  REQUIRE_MESSAGE(parse_summary(hit, hit_summary), hit.output);
  CHECK(hit_summary.text("mesh_cache") == "hit");
  CHECK(hit_summary.number("clusters") == number("clusters"));
  CHECK(hit_summary.number("leaf_clusters") == number("leaf_clusters"));
  CHECK(hit_summary.number("triangles") == number("triangles"));
  CHECK(hit_summary.number("visible_hw_last") == number("visible_hw_last"));
  // A container does not record how many primitives were merged into it.
  CHECK(hit_summary.number("mesh_primitives") == 0);

  // --no-cache neither reads the entry that is now there nor writes one.
  const Run uncached = view(with({"--no-cache", "--mesh", mesh}));
  REQUIRE_MESSAGE(uncached.exit_code == 0, uncached.output);
  Summary uncached_summary;
  REQUIRE_MESSAGE(parse_summary(uncached, uncached_summary), uncached.output);
  CHECK(uncached_summary.text("mesh_cache") == "none");
  CHECK(uncached_summary.number("mesh_primitives") == 2);
  CHECK(uncached_summary.number("visible_hw_last") == number("visible_hw_last"));

  // The same mesh through engine-content: `build` writes a container beside the fixture and
  // `--mesh` loads it, with the same cut as the glTF the container was built from.
  const std::string container = slashes(dir / "cube.clusters");
  const Run built = run_app(content_app(), {"build", mesh, container});
  REQUIRE_MESSAGE(built.exit_code == 0, built.output);
  REQUIRE(std::filesystem::exists(container));
  const Run from_file = view(with({"--mesh", container}));
  REQUIRE_MESSAGE(from_file.exit_code == 0, from_file.output);
  Summary file_summary;
  REQUIRE_MESSAGE(parse_summary(from_file, file_summary), from_file.output);
  CHECK(file_summary.text("mesh_cache") == "file");
  CHECK(file_summary.number("clusters") == number("clusters"));
  CHECK(file_summary.number("triangles") == number("triangles"));
  CHECK(file_summary.number("visible_hw_last") == number("visible_hw_last"));

  // The same mesh through ray queries against cluster acceleration structures built every frame
  // from the cull output; exit 3 where the GPU has no cluster acceleration structures.
  const Run rt = view(with({"--mesh", mesh, "--raster", "rt"}));
  if (rt.exit_code == 3) {
    MESSAGE("--raster rt unavailable here: " << rt.output);
  } else {
    REQUIRE_MESSAGE(rt.exit_code == 0, rt.output);
    const usize rt_start = rt.output.find_last_of('\n', rt.output.size() - 2);
    const std::string rt_last = rt.output.substr(rt_start == std::string::npos ? 0 : rt_start + 1);
    CHECK(rt_last.find("\"raster\":\"rt\"") != std::string::npos);
    JsonValue rt_summary;
    REQUIRE_MESSAGE(parse_json(rt_last, rt_summary).ok, rt_last);
    u64 visible = 0;
    const JsonValue* rt_visible = rt_summary.find("visible_hw_last");
    REQUIRE(rt_visible != nullptr);
    CHECK(rt_visible->get_u64(visible));
    CHECK(visible > 0);  // the cube's clusters went through the cull, the builds, and the trace
  }

  // A file that does not exist fails cleanly (exit 1) once the window and device are up, as
  // does a container that is not one.
  const Run missing = view({"--frames", "1", "--ddc", ddc, "--mesh", slashes(dir / "missing.glb")});
  CHECK(missing.exit_code == 1);
  CHECK(missing.output.find("mesh") != std::string::npos);
  const std::string not_a_container = slashes(dir / "broken.clusters");
  {
    std::ofstream(not_a_container, std::ios::binary) << "not a cluster file at all";
  }
  const Run broken = view({"--frames", "1", "--ddc", ddc, "--mesh", not_a_container});
  CHECK(broken.exit_code == 1);
  // The fixture and the capture stay next to the temp directory for a look after the run.
  const auto temp = std::filesystem::temp_directory_path();
  std::filesystem::copy_file(dir / "cube.glb", temp / "engine_view_mesh_cube.glb",
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(dir / "mesh.png", temp / "engine_view_mesh_last.png",
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::remove_all(dir);
}
