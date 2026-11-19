// What a scene file's text costs to read before anything of it is expanded, opened or loaded
// (roadmap D40): `read_scene_text`, the first stage of `read_scene_file`, with no device and no
// generator. The forest staging's file is 172,866,850 bytes, of which 101 MB are its 470,641
// instances; the synthetic text here is that many instances of the staging's own shape, written
// as the engine writes a scene. `ENGINE_BENCH_SCENE` names a real file to read instead.
//
// Run alone (`--filter=renderer.scene.read_text`) for its peaks to be its own (bench_peak.h).

#include <core/base/types.h>
#include <foundation/bench/bench.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene_text.h>

#include <bench_peak.h>
#include <test_paths.h>

#include <charconv>
#include <schemas/scene.h>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_staging_instances = 470'641;
constexpr u32 k_staging_meshes = 3'849;

void append_number(std::string& out, f64 v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
  const std::string_view text(buf, static_cast<usize>(r.ptr - buf));
  if (text.find_first_of(".eE") == std::string_view::npos) out.append(".0");
}

// `instances` instances shaped like the staging's — a ground flag, a mesh, a name, a uniform
// scale, a translation of three doubles and a yaw — after `meshes` meshes, in the engine's
// canonical pretty form (keys in order, two-space indentation).
std::string synthetic_scene(u32 instances, u32 meshes) {
  std::string out;
  out.reserve(static_cast<usize>(instances) * 230u + static_cast<usize>(meshes) * 80u + 256u);
  out.append("{\n  \"format\": \"engine.scene.v1\",\n  \"instances\": [");
  u64 state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (u32 i = 0; i < instances; ++i) {
    if (i > 0) out.push_back(',');
    const f64 s = 0.5 + static_cast<f64>(next() % 1000) / 1000.0;
    out.append("\n    {\n      \"ground\": ");
    out.append((next() & 1) != 0 ? "true" : "false");
    out.append(",\n      \"mesh\": ");
    out.append(std::to_string(next() % meshes));
    out.append(",\n      \"name\": \"tree-");
    out.append(std::to_string(i));
    out.append("\",\n      \"scale\": [");
    for (u32 c = 0; c < 3; ++c) {
      out.append(c == 0 ? "\n        " : ",\n        ");
      append_number(out, static_cast<f64>(static_cast<f32>(s)));
    }
    out.append("\n      ],\n      \"translation\": [");
    for (u32 c = 0; c < 3; ++c) {
      out.append(c == 0 ? "\n        " : ",\n        ");
      append_number(out, (static_cast<f64>(next() % 4000000) - 2000000.0) / 1000.0 +
                             (c == 1 ? static_cast<f64>(next() % 1000) * 1.3e-5 : 0.0));
    }
    out.append("\n      ],\n      \"yaw_deg\": ");
    append_number(out, static_cast<f64>(static_cast<f32>(static_cast<f64>(next() % 36000) / 100.0)));
    out.append("\n    }");
  }
  out.append("\n  ],\n  \"meshes\": [");
  for (u32 m = 0; m < meshes; ++m) {
    if (m > 0) out.push_back(',');
    out.append("\n    {\n      \"name\": \"mesh-");
    out.append(std::to_string(m));
    out.append("\",\n      \"path\": \"meshes/mesh-");
    out.append(std::to_string(m));
    out.append(".clusters\"\n    }");
  }
  out.append("\n  ],\n  \"name\": \"synthetic staging\"\n}");
  return out;
}

std::string scene_text() {
  if (const std::string path = engine::test::detail::environment("ENGINE_BENCH_SCENE"); !path.empty()) {
    std::string text;
    ENGINE_VERIFY(io::read_file(path, text) == io::Status::Ok, "scene bench: cannot read the file");
    return text;
  }
  const u32 instances = bench::smoke_mode() ? k_staging_instances / 100 : k_staging_instances;
  return synthetic_scene(instances, k_staging_meshes);
}

}  // namespace

ENGINE_BENCH(scene_read_text, "renderer.scene.read_text") {
  const std::string text = scene_text();
  while (state.keep_running()) {
    scene::Scene file;
    std::string error;
    ENGINE_VERIFY(renderer::read_scene_text(text, "bench", false, file, error),
                  "scene bench: the text did not read");
    bench::keep(file.instances.size());
  }
  state.set_items(1);
  state.set_bytes(text.size());
  test::report_peak("renderer.scene.read_text", text.size());
}
