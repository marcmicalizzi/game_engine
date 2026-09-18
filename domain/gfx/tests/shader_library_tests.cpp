// SPIR-V reflection against the shaders this module ships, the library's embedded and file
// paths (through the build's manifest), and hot reload through slangc with a temporary shader.
// Everything but module creation runs without a GPU, so CI covers the compiler round trip.
#include <domain/gfx/device.h>
#include <domain/gfx/shader_library.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <shaders/bindless_copy.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/fill.spv.h>
#include <shaders/scale.spv.h>
#include <shaders/triangle.spv.h>
#include <string>

using namespace engine;

namespace {

std::span<const u32> words_of(const unsigned char* bytes, usize size) {
  return std::span<const u32>(reinterpret_cast<const u32*>(bytes), size / 4);
}

std::string forward_slashes(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

}  // namespace

TEST_CASE("shader reflection: entry points, workgroup sizes, bindings, push constants") {
  gfx::ShaderReflection r;
  std::string error;

  REQUIRE_MESSAGE(
      gfx::reflect_spirv(words_of(shaders::k_fill_spirv, shaders::k_fill_spirv_size), r, &error),
      error);
  CHECK(r.spirv_version >= 0x00010600u);
  REQUIRE(r.entry_points.size() == 1);
  CHECK(r.entry_points[0].name == "fill");
  CHECK(r.entry_points[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);
  CHECK(r.entry_points[0].local_size[0] == 64);
  CHECK(r.entry_points[0].local_size[1] == 1);
  CHECK(r.push_constant_bytes == 12);
  REQUIRE(r.bindings.size() == 1);
  CHECK(r.bindings[0].set == 0);
  CHECK(r.bindings[0].binding == 0);
  CHECK(r.bindings[0].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  CHECK(r.bindings[0].count == 1);
  CHECK(r.bindings[0].name == "g_data");

  REQUIRE(
      gfx::reflect_spirv(words_of(shaders::k_scale_spirv, shaders::k_scale_spirv_size), r, &error));
  REQUIRE(r.bindings.size() == 2);
  CHECK(r.binding_at(0, 0) != nullptr);
  CHECK(r.binding_at(0, 1) != nullptr);
  CHECK(r.binding_at(0, 1)->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  CHECK(r.binding_at(1, 0) == nullptr);

  REQUIRE(gfx::reflect_spirv(
      words_of(shaders::k_bindless_copy_spirv, shaders::k_bindless_copy_spirv_size), r, &error));
  REQUIRE(r.bindings.size() == 3);
  CHECK(r.bindings[0].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  CHECK(r.bindings[0].count == 0);  // unbounded
  CHECK(r.bindings[1].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
  CHECK(r.bindings[1].count == 0);
  CHECK(r.bindings[2].type == VK_DESCRIPTOR_TYPE_SAMPLER);
  CHECK(r.bindings[2].binding == 2);
  REQUIRE(r.entry_points.size() == 1);
  CHECK(r.entry_points[0].local_size[0] == 8);
  CHECK(r.entry_points[0].local_size[1] == 8);

  REQUIRE(gfx::reflect_spirv(
      words_of(shaders::k_cluster_mesh_spirv, shaders::k_cluster_mesh_spirv_size), r, &error));
  REQUIRE(r.entry_points.size() == 4);
  const gfx::ShaderEntryPoint* mesh = r.entry("mesh_main");
  REQUIRE(mesh != nullptr);
  CHECK(mesh->stage == VK_SHADER_STAGE_MESH_BIT_EXT);
  CHECK(mesh->local_size[0] == 128);
  REQUIRE(r.entry("fs_main") != nullptr);
  CHECK(r.entry("fs_main")->stage == VK_SHADER_STAGE_FRAGMENT_BIT);
  CHECK(r.entry("fs_color") != nullptr);
  CHECK(r.entry("fs_visibility") != nullptr);
  CHECK(r.entry("missing") == nullptr);
  // float4x4, three addresses (clusters, the MeshDesc array, triangles), the visible offset and
  // the triangle capacity, the visible list, the visibility buffer, width, height, the instances.
  CHECK(r.push_constant_bytes == 128);
  CHECK(r.bindings.empty());

  REQUIRE(gfx::reflect_spirv(words_of(shaders::k_triangle_spirv, shaders::k_triangle_spirv_size), r,
                             &error));
  REQUIRE(r.entry("vs_main") != nullptr);
  CHECK(r.entry("vs_main")->stage == VK_SHADER_STAGE_VERTEX_BIT);
  CHECK(r.entry("vs_main")->local_size[0] == 0);
  CHECK(r.push_constant_bytes == 16);

  const u32 garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK_FALSE(gfx::reflect_spirv(garbage, r, &error));
  CHECK(error.find("SPIR-V") != std::string::npos);
}

TEST_CASE("shader library: embedded shaders, the build manifest, and hot reload through slangc") {
  gfx::Device device;
  std::string error;
  const bool have_device = device.create(gfx::DeviceOptions{}, &error);
  if (!have_device) MESSAGE("no device, reflection only: " << error);

  gfx::ShaderLibrary library;
  REQUIRE(library.create(have_device ? &device : nullptr, &error));
  library.add_embedded("cluster_mesh", shaders::k_cluster_mesh_spirv,
                       shaders::k_cluster_mesh_spirv_size);
  library.add_embedded("fill", shaders::k_fill_spirv, shaders::k_fill_spirv_size);
  CHECK(library.count() == 2);

  const gfx::Shader* fill = library.get("fill", &error);
  REQUIRE_MESSAGE(fill != nullptr, error);
  CHECK(fill->hash != 0);
  CHECK_FALSE(fill->from_file);
  CHECK(fill->reflection.push_constant_bytes == 12);
  CHECK((fill->module != VK_NULL_HANDLE) == have_device);
  CHECK(library.get("nope", &error) == nullptr);
  CHECK(error.find("unknown shader") != std::string::npos);

  // The manifest the build wrote: the same shader from its .spv file hashes identically.
  REQUIRE_MESSAGE(library.load_manifest(ENGINE_SHADER_MANIFEST, &error), error);
  CHECK(library.count() >= 6);
  CHECK_FALSE(library.compiler().empty());
  const gfx::Shader* embedded_mesh = nullptr;
  {
    gfx::ShaderLibrary embedded_only;
    REQUIRE(embedded_only.create(nullptr));
    embedded_only.add_embedded("cluster_mesh", shaders::k_cluster_mesh_spirv,
                               shaders::k_cluster_mesh_spirv_size);
    embedded_mesh = embedded_only.get("cluster_mesh");
    REQUIRE(embedded_mesh != nullptr);
    const gfx::Shader* file_mesh = library.get("cluster_mesh", &error);
    REQUIRE_MESSAGE(file_mesh != nullptr, error);
    CHECK(file_mesh->from_file);
    CHECK(file_mesh->hash == embedded_mesh->hash);
    CHECK(file_mesh->reflection.entry_points.size() == 4);
  }
  std::string description;
  library.describe(description);
  CHECK(description.find("cluster_mesh (file) entries: mesh_main[128x1x1]") != std::string::npos);
  CHECK(description.find("push:128") != std::string::npos);

  // Hot reload: a temporary shader compiled on first use, recompiled when its source changes,
  // and kept at the last good version when a save does not compile.
  const auto dir = std::filesystem::temp_directory_path() / "engine_shader_library_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string source = forward_slashes((dir / "hot.slang").string());
  const std::string spirv = forward_slashes((dir / "hot.spv").string());
  const std::string manifest = forward_slashes((dir / "manifest.json").string());
  auto write_source = [&](const char* push_type) {
    const std::string text = std::string("struct Params { ") + push_type +
                             " value; };\n[[vk::push_constant]] ConstantBuffer<Params> g_params;\n"
                             "[[vk::binding(0, 0)]] RWStructuredBuffer<uint> g_out;\n"
                             "[shader(\"compute\")]\n[numthreads(32, 1, 1)]\n"
                             "void hot_main(uint3 id : SV_DispatchThreadID) { g_out[id.x] = id.x + "
                             "uint(g_params.value.x); }\n";
    REQUIRE(io::write_file(source, text) == io::Status::Ok);
  };
  write_source("uint2");
  REQUIRE(io::write_file(manifest,
                         std::string("{\"slangc\": \"") + library.compiler() +
                             "\", \"shaders\": [{\"name\": \"hot\", \"source\": \"" + source +
                             "\", \"spirv\": \"" + spirv +
                             "\", \"args\": [\"-target\", \"spirv\", \"-profile\", \"spirv_1_6\", "
                             "\"-emit-spirv-directly\", \"-fvk-use-entrypoint-name\", \"-O2\", "
                             "\"-warnings-disable\", \"41012\"]}]}\n") == io::Status::Ok);

  gfx::ShaderLibrary hot;
  REQUIRE(hot.create(have_device ? &device : nullptr));
  REQUIRE_MESSAGE(hot.load_manifest(manifest, &error), error);
  const gfx::Shader* shader = hot.get("hot", &error);
  REQUIRE_MESSAGE(shader != nullptr, error);  // compiled because hot.spv did not exist
  CHECK(std::filesystem::exists(spirv));
  CHECK(shader->from_file);
  CHECK(shader->generation == 0);
  CHECK(shader->reflection.push_constant_bytes == 8);
  REQUIRE(shader->reflection.entry("hot_main") != nullptr);
  CHECK(shader->reflection.entry("hot_main")->local_size[0] == 32);
  const u64 first_hash = shader->hash;

  Vector<std::string> changed;
  CHECK(hot.poll_changes(changed, &error) == 0);  // nothing changed yet
  CHECK(error.empty());

  // A newer source: push the file time two seconds ahead so the change is unambiguous.
  write_source("float4");
  std::filesystem::last_write_time(
      source, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2));
  REQUIRE(hot.poll_changes(changed, &error) == 1);
  CHECK(error.empty());
  REQUIRE(changed.size() == 1);
  CHECK(changed[0] == "hot");
  shader = hot.get("hot");
  REQUIRE(shader != nullptr);
  CHECK(shader->generation == 1);
  CHECK(shader->reflection.push_constant_bytes == 16);
  CHECK(shader->hash != first_hash);

  // A broken save: reported once, previous version kept.
  REQUIRE(io::write_file(source, "this is not slang\n") == io::Status::Ok);
  std::filesystem::last_write_time(
      source, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(4));
  CHECK(hot.poll_changes(changed, &error) == 0);
  CHECK(error.find("hot: slangc exited with") != std::string::npos);
  CHECK(changed.empty());
  shader = hot.get("hot");
  REQUIRE(shader != nullptr);
  CHECK(shader->generation == 1);
  CHECK(shader->reflection.push_constant_bytes == 16);
  CHECK(hot.poll_changes(changed, &error) == 0);  // the same broken save is not reported again
  CHECK(error.empty());
  CHECK_FALSE(hot.recompile("hot", &error));
  CHECK_FALSE(hot.recompile("nope", &error));

  hot.destroy();
  library.destroy();
  std::filesystem::remove_all(dir);
  if (have_device) device.destroy();
}

TEST_CASE("shader library: hot reload follows #include, transitively") {
  // No device: this is about file timestamps and the compiler, both of which work on a machine
  // with no GPU, which is where CI runs it.
  gfx::ShaderLibrary library;
  std::string error;
  REQUIRE(library.create(nullptr, &error));
  REQUIRE_MESSAGE(library.load_manifest(ENGINE_SHADER_MANIFEST, &error), error);

  // The shipped shaders include shaders/scene.slang, and the resolve includes brdf.slang as
  // well; whatever the exact set is on the day, the library has to have found it.
  REQUIRE(library.get("cluster_mesh", &error) != nullptr);
  CHECK(library.include_count("cluster_mesh") >= 1);
  CHECK(library.include_count("no_such_shader") == 0);
  std::string description;
  library.describe(description);
  CHECK(description.find("includes:") != std::string::npos);
  const std::string slangc = library.compiler();
  REQUIRE_FALSE(slangc.empty());
  library.destroy();

  const auto dir = std::filesystem::temp_directory_path() / "engine_shader_include_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string deeper = forward_slashes((dir / "deeper.slang").string());
  const std::string common = forward_slashes((dir / "common.slang").string());
  const std::string leaf = forward_slashes((dir / "leaf.slang").string());
  const std::string spirv = forward_slashes((dir / "leaf.spv").string());
  const std::string manifest = forward_slashes((dir / "manifest.json").string());

  auto write_deeper = [&](const char* factor) {
    REQUIRE(io::write_file(deeper, std::string("static const uint k_factor = ") + factor + ";\n") ==
            io::Status::Ok);
  };
  auto write_common = [&](const char* bias) {
    // Includes a file of its own, so the shader reaches deeper.slang only through this one.
    REQUIRE(io::write_file(common, std::string("#include \"deeper.slang\"\n"
                                               "// #include \"deeper.slang\"\n"
                                               "uint transform(uint v) { return v * k_factor + ") +
                                       bias + "; }\n") == io::Status::Ok);
  };
  write_deeper("2");
  write_common("1");
  REQUIRE(io::write_file(leaf,
                         "#include \"common.slang\"\n"
                         "[[vk::binding(0, 0)]] RWStructuredBuffer<uint> g_out;\n"
                         "[shader(\"compute\")]\n[numthreads(32, 1, 1)]\n"
                         "void leaf_main(uint3 id : SV_DispatchThreadID) "
                         "{ g_out[id.x] = transform(id.x); }\n") == io::Status::Ok);
  REQUIRE(io::write_file(manifest,
                         std::string("{\"slangc\": \"") + slangc +
                             "\", \"shaders\": [{\"name\": \"leaf\", \"source\": \"" + leaf +
                             "\", \"spirv\": \"" + spirv +
                             "\", \"args\": [\"-target\", \"spirv\", \"-profile\", \"spirv_1_6\", "
                             "\"-emit-spirv-directly\", \"-fvk-use-entrypoint-name\", \"-O2\", "
                             "\"-warnings-disable\", \"41012\"]}]}\n") == io::Status::Ok);

  gfx::ShaderLibrary included;
  REQUIRE(included.create(nullptr, &error));
  REQUIRE_MESSAGE(included.load_manifest(manifest, &error), error);
  const gfx::Shader* shader = included.get("leaf", &error);
  REQUIRE_MESSAGE(shader != nullptr, error);
  // common.slang and, through it, deeper.slang -- each once, although common.slang names
  // deeper.slang twice and the second one is commented out.
  CHECK(included.include_count("leaf") == 2);
  const u64 first_hash = shader->hash;

  Vector<std::string> changed;
  CHECK(included.poll_changes(changed, &error) == 0);
  CHECK(error.empty());

  // The included file changes and the shader that includes it reloads, although its own source
  // was not touched. This is the case poll_changes used to miss.
  write_common("7");
  std::filesystem::last_write_time(
      common, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2));
  REQUIRE_MESSAGE(included.poll_changes(changed, &error) == 1, error);
  REQUIRE(changed.size() == 1);
  CHECK(changed[0] == "leaf");
  shader = included.get("leaf");
  REQUIRE(shader != nullptr);
  CHECK(shader->generation == 1);
  CHECK(shader->hash != first_hash);
  const u64 second_hash = shader->hash;
  CHECK(included.poll_changes(changed, &error) == 0);  // reported once

  // And a file the shader reaches only through that include.
  write_deeper("5");
  std::filesystem::last_write_time(
      deeper, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(4));
  REQUIRE_MESSAGE(included.poll_changes(changed, &error) == 1, error);
  CHECK(changed[0] == "leaf");
  shader = included.get("leaf");
  REQUIRE(shader != nullptr);
  CHECK(shader->generation == 2);
  CHECK(shader->hash != second_hash);

  // An include that stops being included stops being watched.
  REQUIRE(io::write_file(common, "uint transform(uint v) { return v + 3; }\n") == io::Status::Ok);
  std::filesystem::last_write_time(
      common, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(6));
  REQUIRE_MESSAGE(included.poll_changes(changed, &error) == 1, error);
  CHECK(included.include_count("leaf") == 1);
  write_deeper("11");
  std::filesystem::last_write_time(
      deeper, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(8));
  CHECK(included.poll_changes(changed, &error) == 0);
  CHECK(error.empty());

  // A broken include keeps the last good shader and reports slangc's message once.
  REQUIRE(io::write_file(common, "this is not slang\n") == io::Status::Ok);
  std::filesystem::last_write_time(
      common, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(10));
  const u32 generation = included.get("leaf")->generation;
  CHECK(included.poll_changes(changed, &error) == 0);
  CHECK(error.find("leaf: slangc exited with") != std::string::npos);
  CHECK(included.get("leaf")->generation == generation);
  CHECK(included.poll_changes(changed, &error) == 0);
  CHECK(error.empty());

  included.destroy();
  std::filesystem::remove_all(dir);
}
