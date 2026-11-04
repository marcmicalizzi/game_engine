// engine-content morphs end to end (docs/subsystems/assets.md, "Morph channels from MakeHuman
// targets"): a synthetic MakeHuman base, synthetic targets, a mapping and a two-control catalogue,
// and a canonical-topology glTF written here — nothing binary in the tree, and nothing of the
// owner's bodies or of MakeHuman's files. The deltas are held to a double-precision reference
// computed in this file, the source's buffer to being the output's prefix, the weights track to
// its padding, and the output to a round trip through load_gltf.
//
// The base puts ten helper vertices *before* the body's, so canonical id k is MakeHuman vertex
// k + 10: a tool that read a canonical id as a MakeHuman index (the roadmap row's first premise)
// would read the wrong vertices and fail every delta check.

#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/assets/gltf.h>
#include <domain/assets/gltf_document.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Output {
  i32 code = -1;
  std::string text;
};

Output run_content(const std::vector<std::string>& args) {
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv{exe};
  for (const auto& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Output result;
  if (!p.spawn(argv, &error, /*merge_stderr=*/false)) {
    FAIL(error);
    return result;
  }
  p.close_stdin();
  p.read_all(result.text);
  result.code = p.wait();
  return result;
}

u64 uint_of(const JsonValue& object, std::string_view key) {
  u64 v = ~0ull;
  const JsonValue* member = object.find(key);
  if (member == nullptr || !member->get_u64(v)) return ~0ull;
  return v;
}

std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

void write_all(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

constexpr u32 k_helpers = 10;  // MakeHuman vertices before the body's, in no body face
constexpr u32 k_grid = 16;     // the body is a 16 x 16 grid of vertices
constexpr u32 k_body = k_grid * k_grid;
constexpr u32 k_mh = k_helpers + k_body;
constexpr u32 k_seam = 8;  // the grid column split by a UV seam in the export

// A target in MakeHuman's index space, written as text and kept as the doubles the text says.
struct Target {
  std::string name;
  std::vector<double> d = std::vector<double>(k_mh * 3, 0.0);
  std::string text = "# synthetic target\n";
  void move(u32 vertex, double dx, double dy, double dz) {
    d[vertex * 3] = dx;
    d[vertex * 3 + 1] = dy;
    d[vertex * 3 + 2] = dz;
    text += std::to_string(vertex) + " " + num(dx) + " " + num(dy) + " " + num(dz) + "\n";
  }
};

u32 mh_of(u32 i, u32 j) { return k_helpers + j * k_grid + i; }

struct Fixture {
  std::filesystem::path root;
  std::vector<double> mh_raw = std::vector<double>(k_mh * 3, 0.0);
  std::vector<u32> export_ids;    // canonical id per export vertex
  std::vector<float> export_pos;  // metres, as stored
  std::vector<u32> tri;
  std::vector<u8> bin;
  std::string json;
  std::vector<Target> baseline, shapes;
  double scale = 0.0;
};

void put_f32(std::vector<u8>& bin, float v) {
  u8 b[4];
  std::memcpy(b, &v, 4);
  bin.insert(bin.end(), b, b + 4);
}

void pad4(std::vector<u8>& bin) {
  while (bin.size() % 4 != 0)
    bin.push_back(0);
}

// Writes the whole fixture under `root` and computes the scale the way the tool must.
Fixture make_fixture(const std::filesystem::path& root) {
  Fixture f;
  f.root = root;
  // The MakeHuman base: helpers first (their own group), then the body grid.
  std::string obj = "# synthetic base\n";
  for (u32 h = 0; h < k_helpers; ++h) {
    f.mh_raw[h * 3] = 50.0 + h;
    f.mh_raw[h * 3 + 1] = -40.0;  // far below the body: a helper must not set the height
  }
  for (u32 j = 0; j < k_grid; ++j)
    for (u32 i = 0; i < k_grid; ++i) {
      const u32 v = mh_of(i, j);
      f.mh_raw[v * 3] = 0.25 * i - 2.0;
      f.mh_raw[v * 3 + 1] = 1.0 * j;
      f.mh_raw[v * 3 + 2] = 0.125 * std::sin(0.5 * i) + 0.0625 * j;
    }
  for (u32 v = 0; v < k_mh; ++v)
    obj += "v " + num(f.mh_raw[v * 3]) + " " + num(f.mh_raw[v * 3 + 1]) + " " +
           num(f.mh_raw[v * 3 + 2]) + "\n";
  obj += "vt 0 0\ng helper-tongue\nf 1/1 2/1 3/1\nf 4/1 5/1 6/1 7/1\ng body\n";
  for (u32 j = 0; j + 1 < k_grid; ++j)
    for (u32 i = 0; i + 1 < k_grid; ++i)
      obj += "f " + std::to_string(mh_of(i, j) + 1) + "/1 " + std::to_string(mh_of(i + 1, j) + 1) +
             "/1 " + std::to_string(mh_of(i + 1, j + 1) + 1) + "/1 " +
             std::to_string(mh_of(i, j + 1) + 1) + "/1\n";
  write_all(root / "refs" / "base.obj", obj);

  // The baseline: the three young-male ancestries, each lifting the top row a little differently.
  const char* ancestries[3] = {"african", "asian", "caucasian"};
  for (u32 a = 0; a < 3; ++a) {
    Target t;
    t.name = std::string(ancestries[a]) + "-male-young.target";
    for (u32 i = 0; i < k_grid; ++i)
      t.move(mh_of(i, k_grid - 1), 0.0, 0.3 + 0.1 * a + 0.01 * i, 0.0);
    t.move(mh_of(3, 0), 0.0, -0.2 * (a + 1), 0.0);
    t.move(2, 0.0, -100.0, 0.0);  // a helper: outside the body, so outside the height
    write_all(root / "refs" / "adult-body-baseline-targets" / t.name, t.text);
    f.baseline.push_back(t);
  }
  std::vector<double> base = f.mh_raw;
  for (const Target& t : f.baseline)
    for (u32 k = 0; k < base.size(); ++k)
      base[k] += t.d[k] / 3.0;
  double lo = 1e300, hi = -1e300;
  for (u32 v = k_helpers; v < k_mh; ++v) {
    lo = std::min(lo, base[v * 3 + 1]);
    hi = std::max(hi, base[v * 3 + 1]);
  }
  f.scale = 1.75 / (hi - lo);

  // The mapping: canonical id k is MakeHuman vertex k + 10.
  std::string mapping = "{\"revision\": \"synthetic\", \"source_template_vertex_ids\": [";
  for (u32 k = 0; k < k_body; ++k)
    mapping += (k ? ", " : "") + std::to_string(k + k_helpers);
  write_all(root / "release" / "exports" / "control-data.json", mapping + "]}");

  // The shape targets and the catalogue: "width" for both sexes, "chest" for the female only.
  const auto shape = [&](const char* name) -> Target& {
    f.shapes.push_back(Target{});
    f.shapes.back().name = name;
    return f.shapes.back();
  };
  {
    Target& t = shape("torso/width-decr.target");
    for (u32 j = 0; j < k_grid; ++j)
      for (u32 i = 0; i < 4; ++i)
        t.move(mh_of(i, j), 0.05 + 0.001 * j, 0.0, -0.01 * i);
    t.move(3, 90.0, 90.0, 90.0);  // a helper vertex the export never sees
  }
  {
    Target& t = shape("torso/width-incr-l.target");
    for (u32 j = 4; j < 12; ++j)
      for (u32 i = 12; i < k_grid; ++i)
        t.move(mh_of(i, j), 0.04, 0.0, 0.0125 * j);
  }
  {
    Target& t = shape("torso/width-incr-r.target");
    for (u32 i = 0; i < k_grid; ++i)
      t.move(mh_of(i, 12), 0.0, 0.03, 0.0);
    t.move(mh_of(5, 12), 0.0, 0.07, 0.0);  // a later line for the same vertex wins
  }
  {
    Target& t = shape("chest-down.target");
    for (u32 i = 4; i < 12; ++i)
      t.move(mh_of(i, 9), 0.0, -0.02, 0.05);
  }
  {
    Target& t = shape("chest-up.target");
    for (u32 i = 4; i < 12; ++i)
      t.move(mh_of(i, 10), 0.0, 0.02, 0.06);
  }
  for (const Target& t : f.shapes)
    write_all(root / "targets" / t.name, t.text);
  write_all(root / "targets" / "body-controls.json",
            "[\n  {\"key\": \"width\", \"label\": \"Width\", \"negative\": "
            "[\"torso/width-decr.target\"], \"positive\": [\"torso/width-incr-l.target\", "
            "\"torso/width-incr-r.target\"], \"female\": false},\n  {\"key\": \"chest\", "
            "\"label\": \"Chest\", \"negative\": [\"chest-down.target\"], \"positive\": "
            "[\"chest-up.target\"], \"female\": true}\n]\n");

  // The export: one vertex a canonical id, then the seam column again, so 16 ids have two
  // vertices and the triangles right of the seam use the copies.
  for (u32 k = 0; k < k_body; ++k)
    f.export_ids.push_back(k);
  for (u32 j = 0; j < k_grid; ++j)
    f.export_ids.push_back(j * k_grid + k_seam);
  const u32 verts = static_cast<u32>(f.export_ids.size());
  for (u32 v = 0; v < verts; ++v) {
    const u32 mh = f.export_ids[v] + k_helpers;
    for (u32 c = 0; c < 3; ++c)
      f.export_pos.push_back(static_cast<float>(f.mh_raw[mh * 3 + c] * 0.1));
  }
  const auto export_of = [&](u32 i, u32 j, bool right) {
    return right && i == k_seam ? k_body + j : j * k_grid + i;
  };
  for (u32 j = 0; j + 1 < k_grid; ++j)
    for (u32 i = 0; i + 1 < k_grid; ++i) {
      const bool right = i >= k_seam;
      const u32 a = export_of(i, j, right), b = export_of(i + 1, j, right);
      const u32 c = export_of(i + 1, j + 1, right), d = export_of(i, j + 1, right);
      f.tri.insert(f.tri.end(), {a, b, c, a, c, d});
    }

  // The buffer: views 0..10.
  std::vector<std::pair<u32, u32>> views;
  const auto view = [&](auto&& fill) {
    pad4(f.bin);
    const u32 start = static_cast<u32>(f.bin.size());
    fill();
    views.push_back({start, static_cast<u32>(f.bin.size()) - start});
  };
  view([&] {
    for (u32 id : f.export_ids)
      put_f32(f.bin, static_cast<float>(id));
  });
  view([&] {
    for (float p : f.export_pos)
      put_f32(f.bin, p);
  });
  view([&] {
    for (u32 v = 0; v < verts; ++v) {
      put_f32(f.bin, 0);
      put_f32(f.bin, 0);
      put_f32(f.bin, 1);
    }
  });
  view([&] {
    for (u32 v = 0; v < verts * 4; ++v)
      f.bin.push_back(0);
  });
  view([&] {
    for (u32 v = 0; v < verts; ++v) {
      put_f32(f.bin, 1);
      put_f32(f.bin, 0);
      put_f32(f.bin, 0);
      put_f32(f.bin, 0);
    }
  });
  view([&] {
    for (u32 i : f.tri) {
      f.bin.push_back(static_cast<u8>(i & 0xFF));
      f.bin.push_back(static_cast<u8>(i >> 8));
    }
  });
  view([&] {
    for (u32 v = 0; v < verts; ++v) {
      put_f32(f.bin, 0);
      put_f32(f.bin, v % 7 == 0 ? 0.01f : 0.0f);
      put_f32(f.bin, 0);
    }
  });
  view([&] {
    for (u32 k = 0; k < 16; ++k)
      put_f32(f.bin, k % 5 == 0 ? 1.0f : 0.0f);
  });
  view([&] {
    put_f32(f.bin, 0.0f);
    put_f32(f.bin, 1.0f);
  });
  view([&] {
    put_f32(f.bin, 0.25f);
    put_f32(f.bin, 0.75f);
  });
  view([&] {
    for (float p : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f})
      put_f32(f.bin, p);
  });
  view([&] {
    for (u32 b : {0u, 0u, 1u, 0u, 2u, 0u})
      f.bin.push_back(static_cast<u8>(b));
  });
  pad4(f.bin);

  std::string j =
      "{\"asset\": {\"generator\": \"morphs_tests\", \"version\": \"2.0\"}, \"scene\": 0, "
      "\"scenes\": [{\"name\": \"Scene\", \"nodes\": [0, 1, 2]}], \"nodes\": ["
      "{\"name\": \"root\", \"translation\": [0.0, 1.0, 0.0]}, "
      "{\"name\": \"body\", \"mesh\": 0, \"skin\": 0}, "
      "{\"name\": \"prop\", \"mesh\": 1, \"translation\": [2.5, 0, 0]}], "
      "\"animations\": [{\"name\": \"breathe\", \"channels\": [{\"sampler\": 0, "
      "\"target\": {\"node\": 1, \"path\": \"weights\"}}], \"samplers\": [{\"input\": 8, "
      "\"interpolation\": \"LINEAR\", \"output\": 9}]}], "
      "\"meshes\": [{\"name\": \"body\", \"primitives\": [{\"attributes\": "
      "{\"_CANONICAL_ID\": 0, \"POSITION\": 1, \"NORMAL\": 2, \"JOINTS_0\": 3, "
      "\"WEIGHTS_0\": 4}, \"indices\": 5, \"targets\": [{\"POSITION\": 6}]}], "
      "\"weights\": [0.0], \"extras\": {\"targetNames\": [\"corrective.knee\"]}}, "
      "{\"name\": \"prop\", \"extras\": {\"note\": \"caf\\u00e9\"}, \"primitives\": "
      "[{\"attributes\": {\"POSITION\": 10}, \"indices\": 11}]}], "
      "\"skins\": [{\"joints\": [0], \"inverseBindMatrices\": 7}], ";
  const std::string n = std::to_string(verts);
  j += "\"accessors\": ["
       "{\"bufferView\": 0, \"componentType\": 5126, \"count\": " +
       n +
       ", \"type\": \"SCALAR\"}, "
       "{\"bufferView\": 1, \"componentType\": 5126, \"count\": " +
       n +
       ", \"type\": \"VEC3\", "
       "\"min\": [-0.2, 0, 0], \"max\": [0.2, 1.5, 0.1]}, "
       "{\"bufferView\": 2, \"componentType\": 5126, \"count\": " +
       n +
       ", \"type\": \"VEC3\"}, "
       "{\"bufferView\": 3, \"componentType\": 5121, \"count\": " +
       n +
       ", \"type\": \"VEC4\"}, "
       "{\"bufferView\": 4, \"componentType\": 5126, \"count\": " +
       n +
       ", \"type\": \"VEC4\"}, "
       "{\"bufferView\": 5, \"componentType\": 5123, \"count\": " +
       std::to_string(f.tri.size()) +
       ", \"type\": \"SCALAR\"}, "
       "{\"bufferView\": 6, \"componentType\": 5126, \"count\": " +
       n +
       ", \"type\": \"VEC3\", "
       "\"min\": [0, 0, 0], \"max\": [0, 0.01, 0]}, "
       "{\"bufferView\": 7, \"componentType\": 5126, \"count\": 1, \"type\": \"MAT4\"}, "
       "{\"bufferView\": 8, \"componentType\": 5126, \"count\": 2, \"type\": \"SCALAR\", "
       "\"min\": [0], \"max\": [1]}, "
       "{\"bufferView\": 9, \"componentType\": 5126, \"count\": 2, \"type\": \"SCALAR\"}, "
       "{\"bufferView\": 10, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\", "
       "\"min\": [0, 0, 0], \"max\": [1, 1, 0]}, "
       "{\"bufferView\": 11, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\"}], ";
  j += "\"bufferViews\": [";
  for (u32 v = 0; v < views.size(); ++v)
    j += (v ? ", " : "") + std::string("{\"buffer\": 0, \"byteOffset\": ") +
         std::to_string(views[v].first) + ", \"byteLength\": " + std::to_string(views[v].second) +
         "}";
  j += "], \"buffers\": [{\"byteLength\": " + std::to_string(f.bin.size()) +
       ", \"uri\": \"canonical-body.bin\"}]}";
  f.json = j;
  write_all(root / "body" / "canonical-body.gltf", j);
  write_all(root / "body" / "canonical-body.bin",
            std::string_view(reinterpret_cast<const char*>(f.bin.data()), f.bin.size()));
  std::string identity =
      "{\"schema\": \"canonical.export.identity/1\", \"revision\": \"syn-r001\", "
      "\"gltf_sha256\": \"0\", \"mappings\": [{\"export_vertex_to_canonical_id\": [";
  for (u32 v = 0; v < verts; ++v)
    identity += (v ? ", " : "") + std::to_string(f.export_ids[v]);
  write_all(root / "body" / "canonical-body.identity.json",
            identity + "]}], \"attribute\": \"_CANONICAL_ID\"}");
  return f;
}

std::vector<std::string> args_for(const Fixture& f, const std::filesystem::path& source,
                                  const char* sex, const std::filesystem::path& out) {
  return {"morphs",       source.string(),
          "--mapping",    (f.root / "release" / "exports" / "control-data.json").string(),
          "--controls",   (f.root / "targets" / "body-controls.json").string(),
          "--targets",    (f.root / "targets").string(),
          "--references", (f.root / "refs").string(),
          "--sex",        sex,
          "--out",        out.string(),
          "--id",         std::string(sex) + "-synthetic-v1"};
}

// The reference delta of one side, in double, per export vertex.
std::vector<double> reference_delta(const Fixture& f, std::initializer_list<u32> shapes) {
  std::vector<double> sum(k_mh * 3, 0.0);
  for (u32 s : shapes)
    for (u32 k = 0; k < sum.size(); ++k)
      sum[k] = sum[k] + f.shapes[s].d[k];
  std::vector<double> out;
  for (u32 id : f.export_ids)
    for (u32 c = 0; c < 3; ++c)
      out.push_back(sum[(id + k_helpers) * 3 + c] * f.scale);
  return out;
}

// The reference normals of a body, all in double.
std::vector<double> reference_normals(const Fixture& f, const std::vector<double>& q) {
  std::vector<double> sum(k_body * 3, 0.0);
  for (u32 corner = 0; corner < 3; ++corner)
    for (u32 t = 0; t * 3 < f.tri.size(); ++t) {
      const double* p0 = &q[f.tri[t * 3] * 3];
      const double* p1 = &q[f.tri[t * 3 + 1] * 3];
      const double* p2 = &q[f.tri[t * 3 + 2] * 3];
      const double a[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
      const double b[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
      const double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                           a[0] * b[1] - a[1] * b[0]};
      const u32 id = f.export_ids[f.tri[t * 3 + corner]];
      for (u32 c = 0; c < 3; ++c)
        sum[id * 3 + c] += n[c];
    }
  std::vector<double> out;
  for (u32 id : f.export_ids) {
    const double* n = &sum[id * 3];
    const double l = std::max(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1e-20);
    for (u32 c = 0; c < 3; ++c)
      out.push_back(n[c] / l);
  }
  return out;
}

}  // namespace

TEST_CASE("engine-content morphs appends the controls' targets by canonical id") {
  test::TempDir tmp("morphs_e2e");
  REQUIRE(tmp.ok());
  const std::filesystem::path root = tmp.path();
  const Fixture f = make_fixture(root);
  const std::filesystem::path source = root / "body" / "canonical-body.gltf";
  const std::filesystem::path out = root / "out-male";
  const Output run = run_content(args_for(f, source, "male", out));
  REQUIRE_MESSAGE(run.code == 0, run.text);

  // The summary line.
  JsonValue summary;
  REQUIRE(parse_json(run.text, summary).ok);
  CHECK(summary.find("command")->as_string() == "morphs");
  CHECK(summary.find("input")->as_string() == "gltf");
  CHECK(uint_of(summary, "channels_added") == 2);
  CHECK(uint_of(summary, "tracks_padded") == 1);
  REQUIRE(summary.find("meshes_left_unchanged")->size() == 1);
  CHECK(summary.find("meshes_left_unchanged")->as_array()[0].as_string() == "prop");
  double scale = 0.0;
  REQUIRE(summary.find("scale_raw_to_m")->get_f64(scale));
  CHECK(scale == f.scale);

  // The source's buffer is the output's prefix, byte for byte.
  const std::string bin = read_all(out / "body.bin");
  REQUIRE(bin.size() > f.bin.size());
  CHECK(std::memcmp(bin.data(), f.bin.data(), f.bin.size()) == 0);

  assets::GltfDocument doc;
  std::string error;
  REQUIRE_MESSAGE(assets::read_gltf_document((out / "body.gltf").string(), doc, &error), error);
  const assets::DocValue& mesh = doc.json.find("meshes")->items[0];
  const assets::DocValue& names = *mesh.find("extras")->find("targetNames");
  REQUIRE(names.items.size() == 3);
  CHECK(names.items[0].text == "corrective.knee");
  CHECK(names.items[1].text == "shape.width.less");
  CHECK(names.items[2].text == "shape.width.more");
  CHECK(mesh.find("weights")->items.size() == 3);
  const assets::DocValue& targets = *mesh.find("primitives")->items[0].find("targets");
  REQUIRE(targets.items.size() == 3);

  // The deltas against the double-precision reference, cast where the tool casts.
  const std::vector<double> less = reference_delta(f, {0});
  const std::vector<double> more = reference_delta(f, {1, 2});
  std::vector<double> pos;
  for (float p : f.export_pos)
    pos.push_back(p);
  const std::vector<double> n0 = reference_normals(f, pos);
  for (u32 side = 0; side < 2; ++side) {
    const std::vector<double>& ref = side == 0 ? less : more;
    i64 pa = -1, na = -1;
    REQUIRE(targets.items[1 + side].find("POSITION")->get_i64(pa));
    REQUIRE(targets.items[1 + side].find("NORMAL")->get_i64(na));
    Vector<f64> got;
    u32 comps = 0;
    REQUIRE(assets::read_accessor(doc, static_cast<u32>(pa), got, comps, &error));
    REQUIRE(got.size() == ref.size());
    u32 moved = 0;
    for (u32 k = 0; k < ref.size(); ++k) {
      CHECK(got[k] == static_cast<f64>(static_cast<f32>(ref[k])));
      if (ref[k] != 0.0) ++moved;
    }
    CHECK(moved > 0);
    std::vector<double> q = pos;
    for (u32 k = 0; k < q.size(); ++k)
      q[k] += ref[k];
    const std::vector<double> n1 = reference_normals(f, q);
    REQUIRE(assets::read_accessor(doc, static_cast<u32>(na), got, comps, &error));
    double worst = 0.0;
    for (u32 k = 0; k < n1.size(); ++k)
      worst = std::max(worst, std::fabs(got[k] - (n1[k] - n0[k])));
    // The tool computes the unmorphed body's face normals in float, as numpy does on float32
    // positions; this reference is double throughout.
    CHECK(worst < 1e-5);
  }
  // The helper vertex the "less" target moves by 9 never reaches the export.
  for (double d : less)
    CHECK(std::fabs(d) < 0.5);

  // The weights track: one key per frame, the old value then zeros.
  const assets::DocValue& sampler =
      doc.json.find("animations")->items[0].find("samplers")->items[0];
  i64 output = -1;
  REQUIRE(sampler.find("output")->get_i64(output));
  CHECK(output != 9);
  Vector<f64> track;
  u32 comps = 0;
  REQUIRE(assets::read_accessor(doc, static_cast<u32>(output), track, comps, &error));
  REQUIRE(track.size() == 6);
  CHECK(track[0] == 0.25);
  CHECK(track[1] == 0.0);
  CHECK(track[2] == 0.0);
  CHECK(track[3] == 0.75);

  // The mesh without canonical ids is exactly as it was.
  assets::DocValue input;
  REQUIRE(assets::parse_document_json(f.json, input));
  std::string before, after;
  REQUIRE(assets::write_document_json(input.find("meshes")->items[1], before));
  REQUIRE(assets::write_document_json(doc.json.find("meshes")->items[1], after));
  CHECK(before == after);

  // The identity and provenance records.
  assets::DocValue identity;
  REQUIRE(assets::parse_document_json(read_all(out / "body.identity.json"), identity));
  CHECK(identity.find("revision")->text == "male-synthetic-v1");
  CHECK(identity.find("gltf_sha256")->text == summary.find("body_gltf_sha256")->as_string());
  CHECK(identity.keys.back() == "source_identity");
  assets::DocValue provenance;
  REQUIRE(assets::parse_document_json(read_all(out / "provenance.json"), provenance));
  CHECK(provenance.find("total_channels")->text == "3");
  CHECK(provenance.find("added_channels")->items.size() == 2);

  // The output is the writer's own form: CRLF line breaks, and a parse and a write give it back.
  const std::string text = read_all(out / "body.gltf");
  CHECK(text.substr(0, 3) == "{\r\n");
  assets::DocValue reparsed;
  REQUIRE(assets::parse_document_json(text, reparsed));
  std::string rewritten;
  REQUIRE(assets::write_document_json(reparsed, rewritten, "\r\n"));
  CHECK(rewritten + "\r\n" == text);

  // load_gltf reads the channels back by name.
  assets::MeshData data;
  REQUIRE_MESSAGE(assets::load_gltf((out / "body.gltf").string(), data, &error), error);
  std::vector<std::string> loaded;
  for (const auto& m : data.morph)
    loaded.push_back(std::string(m.name));
  CHECK(std::find(loaded.begin(), loaded.end(), "shape.width.less") != loaded.end());
  CHECK(std::find(loaded.begin(), loaded.end(), "shape.width.more") != loaded.end());
  CHECK(std::find(loaded.begin(), loaded.end(), "corrective.knee") != loaded.end());

  // The same inputs give the same bytes.
  const Output again = run_content(args_for(f, source, "male", root / "out-male-2"));
  REQUIRE_MESSAGE(again.code == 0, again.text);
  CHECK(read_all(root / "out-male-2" / "body.gltf") == text);
  CHECK(read_all(root / "out-male-2" / "body.bin") == bin);
}

TEST_CASE("engine-content morphs adds the female controls and reads a GLB the same") {
  test::TempDir tmp("morphs_glb");
  REQUIRE(tmp.ok());
  const std::filesystem::path root = tmp.path();
  const Fixture f = make_fixture(root);
  const Output gltf =
      run_content(args_for(f, root / "body" / "canonical-body.gltf", "female", root / "out-gltf"));
  REQUIRE_MESSAGE(gltf.code == 0, gltf.text);
  JsonValue summary;
  REQUIRE(parse_json(gltf.text, summary).ok);
  CHECK(uint_of(summary, "channels_added") == 4);

  // The same document as a GLB: JSON chunk (no buffer URI) and BIN chunk.
  assets::DocValue json;
  REQUIRE(assets::parse_document_json(f.json, json));
  json.find("buffers")->items[0] = assets::DocValue::make_object();
  json.find("buffers")->items[0].set("byteLength", assets::DocValue::make_int(f.bin.size()));
  std::string chunk;
  REQUIRE(assets::write_document_json(json, chunk));
  while (chunk.size() % 4 != 0)
    chunk.push_back(' ');
  std::string glb = "glTF";
  const auto u32le = [&](u32 v) {
    for (u32 k = 0; k < 4; ++k)
      glb.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));
  };
  u32le(2);
  u32le(static_cast<u32>(12 + 8 + chunk.size() + 8 + f.bin.size()));
  u32le(static_cast<u32>(chunk.size()));
  u32le(0x4E4F534Au);
  glb += chunk;
  u32le(static_cast<u32>(f.bin.size()));
  u32le(0x004E4942u);
  glb.append(reinterpret_cast<const char*>(f.bin.data()), f.bin.size());
  write_all(root / "glb" / "complete.glb", glb);

  const Output from_glb =
      run_content(args_for(f, root / "glb" / "complete.glb", "female", root / "out-glb"));
  REQUIRE_MESSAGE(from_glb.code == 0, from_glb.text);
  JsonValue glb_summary;
  REQUIRE(parse_json(from_glb.text, glb_summary).ok);
  CHECK(glb_summary.find("input")->as_string() == "glb");
  CHECK(glb_summary.find("output")->as_string() == "gltf+bin");
  CHECK(glb_summary.find("identity_source")->is_null());
  // A GLB of the same document and buffer writes the same body.
  CHECK(read_all(root / "out-glb" / "body.gltf") == read_all(root / "out-gltf" / "body.gltf"));
  CHECK(read_all(root / "out-glb" / "body.bin") == read_all(root / "out-gltf" / "body.bin"));
}

TEST_CASE("engine-content morphs refuses a mapping that is not the base's body group") {
  test::TempDir tmp("morphs_refuse");
  REQUIRE(tmp.ok());
  const std::filesystem::path root = tmp.path();
  const Fixture f = make_fixture(root);
  // Canonical id as MakeHuman index: the premise the roadmap row first had.
  std::string mapping = "{\"source_template_vertex_ids\": [";
  for (u32 k = 0; k < k_body; ++k)
    mapping += (k ? ", " : "") + std::to_string(k);
  write_all(root / "release" / "exports" / "control-data.json", mapping + "]}");
  const Output run =
      run_content(args_for(f, root / "body" / "canonical-body.gltf", "male", root / "out"));
  CHECK(run.code == 1);
  CHECK_FALSE(std::filesystem::exists(root / "out" / "body.gltf"));

  const Output bad = run_content({"morphs", "x.gltf", "--sex", "other"});
  CHECK(bad.code == 2);
}
